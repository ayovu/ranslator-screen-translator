#pragma once
#include <QColor>
#include <QDBusConnection>
#include <QDBusVariant>
#include <QObject>
#include <QVariantMap>
#include <optional>

namespace NativeLinux {
struct Appearance {
    std::optional<bool> dark;
    QColor accent;
    bool reducedMotion = false;
    bool highContrast = false;
};

// Read-only desktop preferences, including updates from sandbox-compatible Settings portals.
// Missing keys leave the Qt palette and application preferences in control.
class AppearanceSettings final : public QObject {
    Q_OBJECT
    QDBusConnection bus;
    QVariantMap values;
    Appearance current;
    unsigned revision = 0;
    void refresh();
    void publish();

  private slots:
    void settingChanged(const QString &group, const QString &key, const QDBusVariant &value);

  public:
    explicit AppearanceSettings(QObject *parent = nullptr,
                                QDBusConnection connection = QDBusConnection::sessionBus());
    const Appearance &appearance() const { return current; }

  signals:
    void changed();
};
} // namespace NativeLinux
