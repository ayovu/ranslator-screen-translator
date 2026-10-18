#pragma once
#include <QJsonObject>
#include <QObject>
#include <QSharedPointer>
#include <functional>
#ifdef Q_OS_LINUX
#include <QDBusArgument>
#include <QDBusMessage>

struct PortalShortcut {
    QString id;
    QVariantMap options;
};
Q_DECLARE_METATYPE(PortalShortcut)
Q_DECLARE_METATYPE(QList<PortalShortcut>)
QDBusArgument &operator<<(QDBusArgument &, const PortalShortcut &);
const QDBusArgument &operator>>(const QDBusArgument &, PortalShortcut &);
#endif

class Portal : public QObject {
    Q_OBJECT
  public:
    explicit Portal(QObject *parent = nullptr);
    ~Portal() override;
    void cancelRequests();
    void cancelScreenshots();
    void screenshot(std::function<void(QString, QString)> done, const QString &mode = "picker");
    void setAutostart(bool enabled, std::function<void(bool, QString)> done,
                      int timeoutMs = 120000);
    void bindShortcuts(const QJsonObject &config, std::function<void(QString)> triggered,
                       std::function<void(QString)> error);

#ifdef Q_OS_LINUX
  private:
    using Reply = std::function<void(uint, QVariantMap)>;
    struct Request {
        QString path, interface;
        Reply reply;
        bool active = true;
    };
    QHash<QString, QSharedPointer<Request>> pending;
    QHash<QString, QPair<uint, QVariantMap>> earlyResponses;
    QString session;
    bool capturing = false;
    quint64 shortcutGeneration = 0, screenshotGeneration = 0;
    std::function<void(QString)> trigger;
    void request(QString interface, QString method, QVariantList args, Reply reply,
                 int timeoutMs = 120000);
    void finish(const QSharedPointer<Request> &, uint code, const QVariantMap &results);
    void cancelInterface(const QString &interface);
  private slots:
    void response(uint code, const QVariantMap &results, const QDBusMessage &message);
    void activated(const QDBusObjectPath &session, const QString &id, qulonglong timestamp,
                   const QVariantMap &options);
#endif
};
