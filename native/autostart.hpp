#pragma once
#include "portal.hpp"
#include <QObject>
#include <QStringList>

struct AutostartState {
    bool enabled = false;
    bool known = true;
    bool supported = true;
    bool requiresApproval = false;
    QString detail;
};

// OS registration is deliberately separate from translation preferences and Save.
class Autostart : public QObject {
    Q_OBJECT
  public:
    struct Environment {
        QString configHome, executable, appImage, appDir;
        QStringList configDirs, desktops;
        bool flatpak = false;
        static Environment current();
    };
    explicit Autostart(QObject *parent = nullptr, bool allowChanges = true);
    Autostart(Environment environment, QObject *parent = nullptr, bool allowChanges = true);
    const AutostartState &state() const { return current; }
    bool isBusy() const { return pending; }
    void refresh();
    void setEnabled(bool enabled);
    void openSystemSettings();
    static QString launcher(const Environment &, QString *error);
    static QString desktopExec(const QString &executable);
  signals:
    void changed();
    void failed(const QString &message);

  private:
    Environment environment;
    Portal portal;
    AutostartState current;
    bool allowed, pending = false;
    QString entryPath() const;
    QString confirmationPath() const;
    void finish(AutostartState state, const QString &error = {});
    AutostartState readNative() const;
    QString writeNative(bool enabled);
};
