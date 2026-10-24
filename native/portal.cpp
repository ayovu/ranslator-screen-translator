#include "portal.hpp"
#include "actions.hpp"
#ifdef Q_OS_LINUX
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QJsonArray>
#include <QSet>
#include <QTimer>
#include <QUuid>

static constexpr auto service = "org.freedesktop.portal.Desktop";
static constexpr auto desktop = "/org/freedesktop/portal/desktop";
QDBusArgument &operator<<(QDBusArgument &a, const PortalShortcut &s) {
    a.beginStructure();
    a << s.id << s.options;
    a.endStructure();
    return a;
}
const QDBusArgument &operator>>(const QDBusArgument &a, PortalShortcut &s) {
    a.beginStructure();
    a >> s.id >> s.options;
    a.endStructure();
    return a;
}
Portal::Portal(QObject *parent) : QObject(parent) {
    qDBusRegisterMetaType<PortalShortcut>();
    qDBusRegisterMetaType<QList<PortalShortcut>>();
    auto bus = QDBusConnection::sessionBus();
    bus.connect(service, QString(), "org.freedesktop.portal.Request", "Response", this,
                SLOT(response(uint, QVariantMap, QDBusMessage)));
    bus.connect(service, desktop, "org.freedesktop.portal.GlobalShortcuts", "Activated", this,
                SLOT(activated(QDBusObjectPath, QString, qulonglong, QVariantMap)));
}
static void closePortalObject(const QString &path, const QString &interface) {
    if (path.isEmpty())
        return;
    auto message = QDBusMessage::createMethodCall(service, path, interface, "Close");
    QDBusConnection::sessionBus().asyncCall(message);
}
Portal::~Portal() { cancelRequests(); }
void Portal::cancelInterface(const QString &interface) {
    const auto requests = pending;
    for (const auto &request : requests) {
        if (!interface.isEmpty() && request->interface != interface)
            continue;
        request->active = false;
        pending.remove(request->path);
        closePortalObject(request->path, "org.freedesktop.portal.Request");
    }
    if (pending.isEmpty())
        earlyResponses.clear();
}
void Portal::cancelRequests() {
    ++shortcutGeneration;
    cancelScreenshots();
    cancelInterface({});
    closePortalObject(session, "org.freedesktop.portal.Session");
    session.clear();
    trigger = {};
}
void Portal::cancelScreenshots() {
    ++screenshotGeneration;
    capturing = false;
    cancelInterface("org.freedesktop.portal.Screenshot");
}
void Portal::finish(const QSharedPointer<Request> &request, uint code, const QVariantMap &results) {
    if (!request->active)
        return;
    request->active = false;
    pending.remove(request->path);
    if (pending.isEmpty())
        earlyResponses.clear();
    auto reply = std::move(request->reply);
    reply(code, results);
}
void Portal::request(QString interface, QString method, QVariantList args, Reply reply,
                     int timeoutMs) {
    QString token = "r" + QUuid::createUuid().toString(QUuid::Id128);
    QString sender = QDBusConnection::sessionBus().baseService().mid(1).replace('.', '_');
    QString path = "/org/freedesktop/portal/desktop/request/" + sender + "/" + token;
    auto options = args.takeLast().toMap();
    options["handle_token"] = token;
    args << options;
    auto request = QSharedPointer<Request>::create(Request{path, interface, std::move(reply)});
    pending.insert(path, request);
    auto message = QDBusMessage::createMethodCall(service, desktop, interface, method);
    message.setArguments(args);
    auto *watcher =
        new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, request](auto *call) {
        QDBusPendingReply<QDBusObjectPath> result = *call;
        if (result.isError()) {
            finish(request, 2, {{"error", result.error().message()}});
        } else if (!request->active) {
            // Cancellation can precede creation of the server-side Request object.
            closePortalObject(result.value().path(), "org.freedesktop.portal.Request");
        } else if (result.value().path() != request->path) {
            // Older portal implementations need not use handle_token in the returned path.
            pending.remove(request->path);
            request->path = result.value().path();
            pending.insert(request->path, request);
            if (earlyResponses.contains(request->path)) {
                const auto response = earlyResponses.take(request->path);
                finish(request, response.first, response.second);
            }
        }
        call->deleteLater();
    });
    QTimer::singleShot(timeoutMs, this, [this, request] {
        if (!request->active)
            return;
        closePortalObject(request->path, "org.freedesktop.portal.Request");
        finish(request, 2, {{"error", "The desktop portal did not respond within two minutes."}});
    });
}
void Portal::setAutostart(bool enabled, std::function<void(bool, QString)> done, int timeoutMs) {
    request(
        "org.freedesktop.portal.Background", "RequestBackground",
        {QString(), QVariantMap{{"reason", "Keep Ranslator ready to translate selected text."},
                                {"autostart", enabled},
                                {"commandline", QStringList{"ranslator-launch", "--background"}},
                                {"dbus-activatable", false}}},
        [done](uint code, const QVariantMap &results) {
            if (code != 0) {
                const auto detail = results.value("error").toString();
                done(false,
                     code == 1
                         ? "Autostart permission was cancelled. Choose Start at login to retry."
                         : "The desktop could not change autostart. Check that a Background portal "
                           "is installed and enabled, then retry. " +
                               detail);
            } else if (!results.contains("autostart") ||
                       results.value("autostart").metaType().id() != QMetaType::Bool) {
                done(false, "The desktop did not confirm its autostart state. Update the "
                            "desktop portal and retry.");
            } else {
                done(results.value("autostart").toBool(), {});
            }
        },
        timeoutMs);
}
void Portal::response(uint code, const QVariantMap &results, const QDBusMessage &message) {
    if (pending.contains(message.path())) {
        finish(pending.value(message.path()), code, results);
    } else if (!pending.isEmpty() && earlyResponses.size() < 32) {
        // A legacy response can arrive before its method reply reveals the request path.
        earlyResponses.insert(message.path(), {code, results});
    }
}
void Portal::screenshot(std::function<void(QString, QString)> done, const QString &mode) {
    if (capturing) {
        done({}, "A screenshot request is already open.");
        return;
    }
    capturing = true;
    const auto generation = ++screenshotGeneration;
    auto capture = [this, generation, mode, done](uint version, uint targets) {
        if (generation != screenshotGeneration)
            return;
        QVariantMap options{{"interactive", mode == "picker" || mode == "region"},
                            {"modal", false}};
        // Version 3 can select screen vs area; no portal version identifies a specific monitor.
        const uint target = mode == "region" ? 4u : 1u;
        if (mode != "picker" && version >= 3 && (targets & target))
            options["target"] = target;
        request("org.freedesktop.portal.Screenshot", "Screenshot", {QString(), options},
                [this, generation, done](uint code, QVariantMap result) {
                    if (generation != screenshotGeneration)
                        return;
                    capturing = false;
                    if (code != 0) {
                        done({}, code == 1 ? "Screenshot cancelled."
                                           : "Screenshot portal unavailable or timed out.");
                        return;
                    }
                    const auto uri = result.value("uri").toString();
                    done(uri, uri.isEmpty() ? "Screenshot portal returned no image." : QString());
                });
    };
    auto message = QDBusMessage::createMethodCall(service, desktop,
                                                  "org.freedesktop.DBus.Properties", "GetAll");
    message << QString("org.freedesktop.portal.Screenshot");
    auto *watcher =
        new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message, 1000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [capture, watcher] {
        QDBusPendingReply<QVariantMap> reply = *watcher;
        const auto properties = reply.isError() ? QVariantMap{} : reply.value();
        capture(properties.value("version").toUInt(),
                properties.value("AvailableTargets").toUInt());
        watcher->deleteLater();
    });
}
void Portal::bindShortcuts(const QJsonObject &config, std::function<void(QString)> triggered,
                           std::function<void(QString)> error) {
    trigger = std::move(triggered);
    const auto generation = ++shortcutGeneration;
    if (!session.isEmpty()) {
        closePortalObject(session, "org.freedesktop.portal.Session");
        session.clear();
    }
    request(
        "org.freedesktop.portal.GlobalShortcuts", "CreateSession",
        {QVariantMap{{"session_handle_token", "s" + QUuid::createUuid().toString(QUuid::Id128)}}},
        [this, generation, config, error](uint code, QVariantMap result) {
            if (generation != shortcutGeneration) {
                closePortalObject(result.value("session_handle").toString(),
                                  "org.freedesktop.portal.Session");
                return;
            }
            if (code != 0) {
                error("Global shortcuts are unavailable here. Use the tray or widget controls.");
                return;
            }
            session = result.value("session_handle").toString();
            if (!session.startsWith("/org/freedesktop/portal/desktop/session/")) {
                error("The desktop portal returned an invalid shortcut session.");
                session.clear();
                return;
            }
            QList<PortalShortcut> shortcuts;
            for (const auto &id : shortcutActions) {
                QString sequence = config[id].toObject()["sequence"].toString();
                if (sequence.trimmed().isEmpty())
                    continue;
                auto parts = sequence.split('+');
                bool modifiers = true;
                for (auto &part : parts) {
                    part = part.trimmed();
                    if (!QStringList{"Ctrl", "Alt", "Shift", "Meta", "Super", "Control"}.contains(
                            part, Qt::CaseInsensitive))
                        modifiers = false;
                }
                if (modifiers) {
                    error("Modifier-only shortcuts require desktop integration on this Wayland "
                          "compositor. Choose a chord such as Ctrl+Alt+R in Shortcuts.");
                    continue;
                }
                for (auto &part : parts) {
                    if (part.compare("Ctrl", Qt::CaseInsensitive) == 0 ||
                        part.compare("Control", Qt::CaseInsensitive) == 0)
                        part = "CTRL";
                    else if (part.compare("Alt", Qt::CaseInsensitive) == 0)
                        part = "ALT";
                    else if (part.compare("Shift", Qt::CaseInsensitive) == 0)
                        part = "SHIFT";
                    else if (part.compare("Meta", Qt::CaseInsensitive) == 0 ||
                             part.compare("Super", Qt::CaseInsensitive) == 0)
                        part = "LOGO";
                }
                sequence = parts.join('+');
                shortcuts.append({id,
                                  {{"description", QString("Ranslator: %1").arg(id)},
                                   {"preferred_trigger", sequence}}});
            }
            request("org.freedesktop.portal.GlobalShortcuts", "BindShortcuts",
                    {QVariant::fromValue(QDBusObjectPath(session)), QVariant::fromValue(shortcuts),
                     QString(), QVariantMap{}},
                    [this, generation, shortcuts, error](uint code, QVariantMap results) {
                        if (generation != shortcutGeneration)
                            return;
                        if (code != 0) {
                            error("Shortcut registration was cancelled. Re-save Shortcuts in "
                                  "Settings to retry.");
                        } else if (results.contains("shortcuts")) {
                            const auto bound =
                                qdbus_cast<QList<PortalShortcut>>(results["shortcuts"]);
                            QSet<QString> ids;
                            for (const auto &shortcut : bound)
                                ids.insert(shortcut.id);
                            QStringList missing;
                            for (const auto &shortcut : shortcuts)
                                if (!ids.contains(shortcut.id))
                                    missing.append(shortcut.id);
                            if (!missing.isEmpty())
                                error("The desktop did not bind these shortcuts: " +
                                      missing.join(", ") + ". Review the system shortcut dialog.");
                        }
                    });
        });
}
void Portal::activated(const QDBusObjectPath &path, const QString &id, qulonglong,
                       const QVariantMap &) {
    if (path.path() == session && trigger)
        trigger(id);
}
#else
Portal::Portal(QObject *parent) : QObject(parent) {}
Portal::~Portal() = default;
void Portal::setAutostart(bool, std::function<void(bool, QString)> done, int) {
    done(false, "Autostart requires the native platform adapter.");
}
void Portal::cancelRequests() {}
void Portal::cancelScreenshots() {}
void Portal::screenshot(std::function<void(QString, QString)> done, const QString &) {
    done({}, "Screen capture requires the native platform adapter.");
}
void Portal::bindShortcuts(const QJsonObject &, std::function<void(QString)>,
                           std::function<void(QString)> error) {
    error("Global shortcuts require the native platform adapter.");
}
#endif
