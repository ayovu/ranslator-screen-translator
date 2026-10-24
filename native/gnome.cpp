#include "gnome.hpp"
#include "actions.hpp"
#include <utility>
#ifdef Q_OS_LINUX
#include <QtDBus>
#endif

namespace {
const QString unavailable = QStringLiteral("GNOME desktop integration is not enabled.");
QJsonObject parseContext(const QString &raw) {
    const auto object = QJsonDocument::fromJson(raw.toUtf8()).object();
    if (!object.contains("index") || object.value("width").toInt() <= 0 ||
        object.value("height").toInt() <= 0)
        return {};
    return object;
}
#ifdef Q_OS_LINUX
constexpr auto service = "io.github.ranslator.Ranslator.Desktop";
constexpr auto client = "io.github.ranslator.Ranslator.Integration";
constexpr auto path = "/io/github/ranslator/Ranslator/Desktop";
constexpr auto interface = "io.github.ranslator.Desktop1";
QDBusMessage method(const QString &name, const QVariantList &arguments = {}) {
    auto message = QDBusMessage::createMethodCall(service, path, interface, name);
    message.setArguments(arguments);
    return message;
}
#endif
} // namespace

GnomeDesktop::GnomeDesktop(QObject *parent, bool enabled) : QObject(parent) {
    if (!enabled)
        return;
#ifdef Q_OS_LINUX
    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return;
    ownsClientName = bus.registerService(client);
    if (!ownsClientName)
        return;
    auto *watcher =
        new QDBusServiceWatcher(service, bus, QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
            &GnomeDesktop::serviceChanged);
    bus.connect(service, path, interface, "ShortcutActivated", this,
                SLOT(shortcutActivated(QString, QString)));
    bus.connect(service, path, interface, "SelectionChanged", this,
                SLOT(selectionChanged(uint, QString)));
    const auto registered = bus.interface()->isServiceRegistered(service);
    ready = registered.isValid() && registered.value();
#endif
}
GnomeDesktop::~GnomeDesktop() {
#ifdef Q_OS_LINUX
    // Losing this name releases all grabs and selection watches in the companion.
    if (ownsClientName)
        QDBusConnection::sessionBus().unregisterService(client);
#endif
}
bool GnomeDesktop::available() const { return ready; }
void GnomeDesktop::serviceChanged(const QString &, const QString &, const QString &owner) {
    ready = ownsClientName && !owner.isEmpty();
    ++bindingGeneration;
    ++captureGeneration;
    ++selectionGeneration;
    auto captureFailed = std::move(captureDisconnected);
    auto error = std::move(selectionError);
    const bool wasArmed = std::exchange(selectionArmed, false);
    selected = {};
    trigger = {};
    if (captureFailed)
        captureFailed();
    if (wasArmed && error)
        error("GNOME desktop integration disconnected. Selection listening was stopped.");
    // Notify old listener failure before the UI can rearm against a new extension instance.
    emit availabilityChanged(ready);
}
void GnomeDesktop::bind(const QJsonObject &shortcuts,
                        std::function<void(QString, QJsonObject)> triggered,
                        std::function<void(QString)> error) {
    trigger = std::move(triggered);
    const auto generation = ++bindingGeneration;
    if (!ready) {
        if (error)
            error(unavailable);
        return;
    }
#ifdef Q_OS_LINUX
    // The companion needs shortcut sequences only; never transmit provider configuration or keys.
    QJsonObject sequences;
    for (const auto &id : shortcutActions)
        sequences.insert(
            id, QJsonObject{{"sequence", shortcuts.value(id).toObject().value("sequence")}});
    const auto raw = QString::fromUtf8(QJsonDocument(sequences).toJson(QJsonDocument::Compact));
    auto *watcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(method("Configure", {raw}), 3000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, generation, error = std::move(error)](QDBusPendingCallWatcher *call) {
                const QDBusPendingReply<QString> reply = *call;
                call->deleteLater();
                if (generation != bindingGeneration || !error)
                    return;
                if (reply.isError()) {
                    error(reply.error().message());
                    return;
                }
                const auto result = QJsonDocument::fromJson(reply.value().toUtf8()).object();
                if (result.value("version").toInt() != 1) {
                    error("GNOME desktop integration has an incompatible protocol version.");
                    return;
                }
                QStringList errors;
                for (const auto &entry : result.value("errors").toArray())
                    errors << entry.toString();
                if (!errors.isEmpty())
                    error(errors.join('\n'));
            });
#else
    Q_UNUSED(shortcuts)
    Q_UNUSED(generation)
#endif
}
void GnomeDesktop::context(std::function<void(QJsonObject, QString)> done) {
    if (!ready) {
        done({}, unavailable);
        return;
    }
#ifdef Q_OS_LINUX
    auto *watcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(method("GetContext"), 1000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [done = std::move(done)](QDBusPendingCallWatcher *call) {
                const QDBusPendingReply<QString> reply = *call;
                call->deleteLater();
                if (reply.isError()) {
                    done({}, reply.error().message());
                    return;
                }
                const auto context = parseContext(reply.value());
                done(context,
                     context.isEmpty() ? "GNOME returned invalid monitor geometry." : QString{});
            });
#endif
}
void GnomeDesktop::capture(const QString &mode, int monitorIndex,
                           std::function<void(QImage, QJsonObject, QString)> done) {
    const auto generation = ++captureGeneration;
    captureDisconnected = {};
    if (!ready) {
        done({}, {}, unavailable);
        return;
    }
#ifdef Q_OS_LINUX
    captureDisconnected = [done] {
        done({}, {}, "GNOME desktop integration disconnected during Snapshot.");
    };
    auto *watcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(method("Capture", {mode, monitorIndex}), 10000),
        this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, generation, done = std::move(done)](QDBusPendingCallWatcher *call) {
                const QDBusPendingReply<QByteArray, QString> reply = *call;
                call->deleteLater();
                if (generation != captureGeneration)
                    return;
                captureDisconnected = {};
                if (reply.isError()) {
                    done({}, {}, reply.error().message());
                    return;
                }
                const auto context = parseContext(reply.argumentAt<1>());
                QByteArray png = reply.argumentAt<0>();
                QBuffer input(&png);
                QImageReader reader(&input, "png");
                const auto size = reader.size();
                if (context.isEmpty() || png.size() > 64 * 1024 * 1024 || !size.isValid() ||
                    qint64(size.width()) * size.height() > 128 * 1024 * 1024) {
                    done({}, {}, "GNOME returned invalid Snapshot data.");
                    return;
                }
                const auto image = reader.read();
                done(image, context,
                     image.isNull() ? "Could not decode the GNOME Snapshot." : QString{});
            });
#else
    Q_UNUSED(mode)
    Q_UNUSED(monitorIndex)
    Q_UNUSED(generation)
#endif
}
void GnomeDesktop::cancelCapture() {
    ++captureGeneration;
    captureDisconnected = {};
}
void GnomeDesktop::armSelection(bool active, bool watchClipboard,
                                std::function<void(QString)> callback,
                                std::function<void(QString)> error) {
    const auto generation = ++selectionGeneration;
    selectionArmed = active && ready;
    selected = active ? std::move(callback) : std::function<void(QString)>{};
    selectionError = active ? error : std::function<void(QString)>{};
    if (!ready) {
        if (active && error)
            error(unavailable);
        return;
    }
#ifdef Q_OS_LINUX
    auto *watcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(
            method("ArmSelection", {active, watchClipboard, generation}), 1000),
        this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, generation, error = std::move(error)](QDBusPendingCallWatcher *call) {
                const QDBusPendingReply<> reply = *call;
                call->deleteLater();
                if (generation != selectionGeneration || !reply.isError())
                    return;
                selectionArmed = false;
                selected = {};
                selectionError = {};
                if (error)
                    error(reply.error().message());
            });
#else
    Q_UNUSED(watchClipboard)
    Q_UNUSED(generation)
#endif
}
void GnomeDesktop::shortcutActivated(const QString &id, const QString &raw) {
    const auto context = parseContext(raw);
    if (ready && trigger && !context.isEmpty())
        trigger(id, context);
}
void GnomeDesktop::selectionChanged(uint generation, const QString &text) {
    if (ready && selectionArmed && generation == selectionGeneration && selected &&
        text.size() <= 65536)
        selected(text);
}
QScreen *GnomeDesktop::screenForContext(const QJsonObject &context) {
    const QRect geometry(context.value("x").toInt(), context.value("y").toInt(),
                         context.value("width").toInt(), context.value("height").toInt());
    for (auto *screen : QGuiApplication::screens())
        if (screen->geometry() == geometry)
            return screen;
    if (geometry.isValid())
        return QGuiApplication::screenAt(geometry.center());
    return nullptr;
}
