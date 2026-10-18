#include "accessibility.hpp"
#include <QApplication>
#include <QFile>
#include <QPointer>
#include <QTimer>
#include <QUuid>
#ifdef Q_OS_LINUX
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#include <functional>

namespace {
constexpr auto accessibleInterface = "org.a11y.atspi.Accessible";
constexpr auto textInterface = "org.a11y.atspi.Text";
constexpr auto eventInterface = "org.a11y.atspi.Event.Object";
constexpr auto registryService = "org.a11y.atspi.Registry";
constexpr auto registryPath = "/org/a11y/atspi/registry";
constexpr auto registryInterface = "org.a11y.atspi.Registry";
constexpr auto selectionEvent = "object:text-selection-changed";
constexpr int maxCharacters = 64 * 1024;
constexpr uint passwordRole = 40; // ATSPI_ROLE_PASSWORD_TEXT, stable wire value.

struct ObjectReference {
    QString service;
    QDBusObjectPath path;
};
ObjectReference reference(const QVariant &value) {
    ObjectReference result;
    const auto argument = qvariant_cast<QDBusArgument>(value);
    if (argument.currentType() == QDBusArgument::StructureType) {
        argument.beginStructure();
        argument >> result.service >> result.path;
        argument.endStructure();
    }
    return result;
}
} // namespace

class AccessibilitySelection::Impl {
  public:
    AccessibilitySelection *owner;
    QString fixedAddress, connectionName;
    QDBusConnection bus = QDBusConnection(QString());
    std::unique_ptr<QDBusServiceWatcher> registryWatcher;
    QTimer debounce;
    QDBusMessage pendingEvent;
    bool armed = false, registered = false;
    quint64 generation = 0, epoch = 0, serial = 0;

    struct Read {
        QString service, path, text;
        quint64 epoch, serial;
        int selections = 0, index = 0;
    };
    using Reading = std::shared_ptr<Read>;
    using Reply = std::function<void(const QDBusMessage &)>;

    Impl(AccessibilitySelection *owner, QString address)
        : owner(owner), fixedAddress(std::move(address)) {
        debounce.setSingleShot(true);
        debounce.setInterval(220);
        QObject::connect(&debounce, &QTimer::timeout, owner, [this] { readPending(); });
    }
    ~Impl() { disconnect(); }
    bool current(const Reading &reading) const {
        return armed && registered && epoch == reading->epoch && serial == reading->serial;
    }
    void request(const QDBusConnection &connection, const QString &service, const QString &path,
                 const QString &interface, const QString &method, const QVariantList &arguments,
                 Reply reply) {
        auto message = QDBusMessage::createMethodCall(service, path, interface, method);
        message.setArguments(arguments);
        auto watcher = new QDBusPendingCallWatcher(connection.asyncCall(message, 1200), owner);
        const auto requestEpoch = epoch;
        QObject::connect(watcher, &QDBusPendingCallWatcher::finished, owner,
                         [this, watcher, requestEpoch, reply = std::move(reply)] {
                             const auto message = watcher->reply();
                             watcher->deleteLater();
                             if (armed && epoch == requestEpoch)
                                 reply(message);
                         });
    }
    void request(const Reading &reading, const QString &interface, const QString &method,
                 const QVariantList &arguments, Reply reply) {
        request(bus, reading->service, reading->path, interface, method, arguments,
                [this, reading, reply = std::move(reply)](const QDBusMessage &message) {
                    if (current(reading) && message.type() == QDBusMessage::ReplyMessage)
                        reply(message);
                });
    }
    void fail(const QString &detail) {
        disconnect();
        emit owner->availability(false, detail, generation);
    }
    void disconnect() {
        debounce.stop();
        registryWatcher.reset();
        registered = false;
        ++serial;
        if (bus.isConnected()) {
            bus.disconnect({}, {}, eventInterface, "TextSelectionChanged", owner,
                           SLOT(selectionChanged(QDBusMessage)));
            bus.disconnect({}, {}, "org.freedesktop.DBus.Local", "Disconnected", owner,
                           SLOT(busDisconnected()));
            auto message = QDBusMessage::createMethodCall(registryService, registryPath,
                                                          registryInterface, "DeregisterEvent");
            message << QString(selectionEvent) << QString();
            bus.send(message);
        }
        bus = QDBusConnection(QString());
        if (!connectionName.isEmpty()) {
            QDBusConnection::disconnectFromBus(connectionName);
            connectionName.clear();
        }
    }
    void connectAddress(const QString &address) {
        if (address.isEmpty()) {
            fail("Accessibility service did not provide a connection. Enable desktop accessibility "
                 "or use Snapshot.");
            return;
        }
        connectionName =
            "ranslator-selection-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        bus = QDBusConnection::connectToBus(address, connectionName);
        if (!bus.isConnected() || !bus.connect({}, {}, eventInterface, "TextSelectionChanged",
                                               owner, SLOT(selectionChanged(QDBusMessage)))) {
            fail(QFile::exists("/.flatpak-info")
                     ? "Flatpak cannot reach host accessibility. Allow org.a11y.Bus and "
                       "xdg-run/at-spi. If your desktop uses a custom accessibility socket, allow "
                       "its directory."
                     : "Cannot connect to desktop accessibility. Check accessibility permissions "
                       "or use Snapshot.");
            return;
        }
        bus.connect({}, {}, "org.freedesktop.DBus.Local", "Disconnected", owner,
                    SLOT(busDisconnected()));
        registryWatcher = std::make_unique<QDBusServiceWatcher>(
            registryService, bus, QDBusServiceWatcher::WatchForUnregistration);
        QObject::connect(registryWatcher.get(), &QDBusServiceWatcher::serviceUnregistered, owner,
                         [this] {
                             if (armed)
                                 fail("Desktop accessibility stopped. Invoke selection again to "
                                      "reconnect, or use Snapshot.");
                         });
        request(
            bus, registryService, registryPath, registryInterface, "RegisterEvent",
            {QString(selectionEvent), QStringList(), QString()}, [this](const QDBusMessage &reply) {
                if (reply.type() != QDBusMessage::ReplyMessage) {
                    fail("Cannot listen for accessible text selections. Enable desktop "
                         "accessibility or use Snapshot.");
                    return;
                }
                registered = true;
                emit owner->availability(true,
                                         "Listening for selected text in accessible applications. "
                                         "Use Snapshot for images or unsupported applications.",
                                         generation);
            });
    }
    void arm(bool value, quint64 nextGeneration) {
        if (armed == value && generation == nextGeneration)
            return;
        ++epoch;
        disconnect();
        armed = value;
        generation = nextGeneration;
        if (!armed)
            return;
        if (!fixedAddress.isEmpty()) {
            connectAddress(fixedAddress);
            return;
        }
        // Flatpak's default proxy only lets an app export its own accessibility
        // tree; it denies selection monitoring. The trusted desktop build has
        // explicit access to org.a11y.Bus and xdg-run/at-spi for this feature.
        const auto address = qEnvironmentVariable("AT_SPI_BUS_ADDRESS");
        if (!QFile::exists("/.flatpak-info") && !address.isEmpty()) {
            connectAddress(address);
            return;
        }
        request(QDBusConnection::sessionBus(), "org.a11y.Bus", "/org/a11y/bus", "org.a11y.Bus",
                "GetAddress", {}, [this](const QDBusMessage &reply) {
                    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
                        fail(QFile::exists("/.flatpak-info")
                                 ? "Flatpak cannot query desktop accessibility. Allow org.a11y.Bus "
                                   "in its permissions, or use Snapshot."
                                 : "Desktop accessibility is unavailable. Install or enable the "
                                   "AT-SPI service, or use Snapshot.");
                        return;
                    }
                    connectAddress(reply.arguments().first().toString());
                });
    }
    void event(const QDBusMessage &message) {
        if (!armed || !registered || message.service().isEmpty() || message.path().isEmpty())
            return;
        // Do not translate our own editor/settings, including in a PID namespace
        // where the accessibility bus's host PIDs differ from our local PID.
        if (QApplication::activeWindow())
            return;
        ++serial;
        pendingEvent = message;
        debounce.start();
    }
    void readPending() {
        if (!armed || !registered || QApplication::activeWindow())
            return;
        auto reading = std::make_shared<Read>();
        reading->service = pendingEvent.service();
        reading->path = pendingEvent.path();
        reading->epoch = epoch;
        reading->serial = serial;
        request(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                "GetConnectionUnixProcessID", {reading->service},
                [this, reading](const QDBusMessage &reply) {
                    if (!current(reading) || reply.type() != QDBusMessage::ReplyMessage ||
                        reply.arguments().isEmpty())
                        return;
                    if (reply.arguments().first().toLongLong() ==
                        QCoreApplication::applicationPid())
                        return;
                    checkRole(reading);
                });
    }
    void checkRole(const Reading &reading) {
        request(reading, accessibleInterface, "GetRole", {},
                [this, reading](const QDBusMessage &reply) {
                    if (reply.arguments().isEmpty() ||
                        reply.arguments().first().toUInt() == passwordRole)
                        return;
                    request(reading, accessibleInterface, "GetApplication", {},
                            [this, reading](const QDBusMessage &reply) {
                                if (reply.arguments().isEmpty())
                                    return;
                                const auto app = reference(reply.arguments().first());
                                if (app.service.isEmpty() || app.path.path().isEmpty())
                                    return;
                                request(bus, app.service, app.path.path(),
                                        "org.freedesktop.DBus.Properties", "Get",
                                        {QString(accessibleInterface), QString("Name")},
                                        [this, reading](const QDBusMessage &reply) {
                                            if (!current(reading) ||
                                                reply.type() != QDBusMessage::ReplyMessage ||
                                                reply.arguments().isEmpty())
                                                return;
                                            const auto name = qvariant_cast<QDBusVariant>(
                                                                  reply.arguments().first())
                                                                  .variant()
                                                                  .toString();
                                            if (name.compare(QCoreApplication::applicationName(),
                                                             Qt::CaseInsensitive) == 0 ||
                                                name.compare("ranslator", Qt::CaseInsensitive) == 0)
                                                return;
                                            readCount(reading);
                                        });
                            });
                });
    }
    void readCount(const Reading &reading) {
        request(reading, textInterface, "GetNSelections", {},
                [this, reading](const QDBusMessage &reply) {
                    if (reply.arguments().isEmpty())
                        return;
                    reading->selections = reply.arguments().first().toInt();
                    if (reading->selections > 0 && reading->selections <= 8)
                        readRange(reading);
                });
    }
    void readRange(const Reading &reading) {
        request(reading, textInterface, "GetSelection", {reading->index},
                [this, reading](const QDBusMessage &reply) {
                    if (reply.arguments().size() != 2)
                        return;
                    const auto start = reply.arguments()[0].toInt(),
                               end = reply.arguments()[1].toInt();
                    if (start < 0 || end <= start || qint64(end) - start > maxCharacters)
                        return;
                    request(reading, textInterface, "GetText", {start, end},
                            [this, reading](const QDBusMessage &reply) {
                                if (reply.arguments().size() != 1)
                                    return;
                                if (!reading->text.isEmpty())
                                    reading->text += '\n';
                                reading->text += reply.arguments().first().toString();
                                if (reading->text.toUtf8().size() > maxCharacters)
                                    return;
                                if (++reading->index < reading->selections)
                                    readRange(reading);
                                else if (!reading->text.trimmed().isEmpty() &&
                                         !QApplication::activeWindow())
                                    emit owner->selected(reading->text, generation);
                            });
                });
    }
};

AccessibilitySelection::AccessibilitySelection(QObject *parent, QString address)
    : QObject(parent), impl(std::make_unique<Impl>(this, std::move(address))) {}
AccessibilitySelection::~AccessibilitySelection() = default;
void AccessibilitySelection::setArmed(bool value, quint64 generation) {
    impl->arm(value, generation);
}
bool AccessibilitySelection::isArmed() const { return impl->armed; }
void AccessibilitySelection::selectionChanged(const QDBusMessage &message) { impl->event(message); }
void AccessibilitySelection::busDisconnected() {
    if (impl->armed)
        impl->fail("The accessibility connection closed. Invoke selection again to reconnect, or "
                   "use Snapshot.");
}

#elif !defined(Q_OS_MACOS)
class AccessibilitySelection::Impl {};
AccessibilitySelection::AccessibilitySelection(QObject *parent, QString) : QObject(parent) {}
AccessibilitySelection::~AccessibilitySelection() = default;
void AccessibilitySelection::setArmed(bool value, quint64 generation) {
    if (value)
        emit availability(false, "AT-SPI is only available on Linux.", generation);
}
bool AccessibilitySelection::isArmed() const { return false; }
#endif
