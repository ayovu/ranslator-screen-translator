#include "appearance_linux.hpp"
#include <QDBusArgument>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <cmath>

namespace {
constexpr auto service = "org.freedesktop.portal.Desktop";
constexpr auto path = "/org/freedesktop/portal/desktop";
constexpr auto interface = "org.freedesktop.portal.Settings";
constexpr auto appearanceGroup = "org.freedesktop.appearance";
using Settings = QMap<QString, QVariantMap>;

QVariant unwrap(QVariant value) {
    while (value.metaType() == QMetaType::fromType<QDBusVariant>())
        value = value.value<QDBusVariant>().variant();
    return value;
}
uint preference(const QVariant &value) {
    const auto unwrapped = unwrap(value);
    return unwrapped.metaType().id() == QMetaType::UInt ? unwrapped.toUInt() : 0;
}
QColor accent(const QVariant &value) {
    const auto unwrapped = unwrap(value);
    if (unwrapped.metaType() != QMetaType::fromType<QDBusArgument>())
        return {};
    const auto tuple = unwrapped.value<QDBusArgument>();
    if (tuple.currentSignature() != "(ddd)")
        return {};
    double red = 0, green = 0, blue = 0;
    tuple.beginStructure();
    tuple >> red >> green >> blue;
    tuple.endStructure();
    for (const auto channel : {red, green, blue})
        if (!std::isfinite(channel) || channel < 0 || channel > 1)
            return {};
    return QColor::fromRgbF(red, green, blue);
}
} // namespace

namespace NativeLinux {
AppearanceSettings::AppearanceSettings(QObject *parent, QDBusConnection connection)
    : QObject(parent), bus(std::move(connection)) {
    qDBusRegisterMetaType<Settings>();
    bus.connect(service, path, interface, "SettingChanged", this,
                SLOT(settingChanged(QString, QString, QDBusVariant)));
    auto *watcher =
        new QDBusServiceWatcher(service, bus, QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
            [this](const QString &, const QString &, const QString &owner) {
                ++revision;
                values.clear();
                publish();
                if (!owner.isEmpty())
                    refresh();
            });
    refresh();
}

void AppearanceSettings::refresh() {
    const auto requested = ++revision;
    auto message = QDBusMessage::createMethodCall(service, path, interface, "ReadAll");
    message << QStringList{appearanceGroup};
    // Do not block first paint or require a portal service to exist.
    auto *pending = new QDBusPendingCallWatcher(bus.asyncCall(message, 500), this);
    connect(pending, &QDBusPendingCallWatcher::finished, this, [this, requested, pending] {
        const QDBusPendingReply<Settings> reply = *pending;
        pending->deleteLater();
        if (requested != revision || reply.isError())
            return;
        values = reply.value().value(appearanceGroup);
        publish();
    });
}

void AppearanceSettings::settingChanged(const QString &group, const QString &key,
                                        const QDBusVariant &value) {
    if (group != appearanceGroup)
        return;
    values.insert(key, value.variant());
    publish();
    // A reply requested before this signal must not undo the newer preference.
    refresh();
}

void AppearanceSettings::publish() {
    Appearance next;
    const auto scheme = preference(values.value("color-scheme"));
    if (scheme == 1 || scheme == 2)
        next.dark = scheme == 1;
    next.accent = accent(values.value("accent-color"));
    next.reducedMotion = preference(values.value("reduced-motion")) == 1;
    next.highContrast = preference(values.value("contrast")) == 1;
    if (next.dark == current.dark && next.accent == current.accent &&
        next.reducedMotion == current.reducedMotion && next.highContrast == current.highContrast)
        return;
    current = next;
    emit changed();
}
} // namespace NativeLinux
