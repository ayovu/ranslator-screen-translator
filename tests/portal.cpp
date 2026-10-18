#include "portal.hpp"
#include <QDBusContext>
#include <QDBusInterface>
#include <QDBusMetaType>
#include <QDBusReply>
#include <QProcess>
#include <QTest>
#include <QTimer>

// Runs in a separate process on a private bus; no real desktop content or permissions are used.
class FakePortal : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Screenshot")
    Q_PROPERTY(uint version READ portalVersion)
    Q_PROPERTY(uint AvailableTargets READ targets)
    uint code = 0;
    uint version = 2, availableTargets = 0;
    bool legacy = false, immediate = false;
    QVariantMap lastOptions;

  public:
    uint portalVersion() const { return version; }
    uint targets() const { return availableTargets; }

  public slots:
    void Configure(uint nextCode) { code = nextCode; }
    void Behavior(bool useLegacyPath, bool respondImmediately, uint apiVersion, uint targetFlags) {
        legacy = useLegacyPath;
        immediate = respondImmediately;
        version = apiVersion;
        availableTargets = targetFlags;
    }
    QVariantMap LastOptions() { return lastOptions; }
    QDBusObjectPath Screenshot(const QString &, const QVariantMap &options) {
        lastOptions = options;
        const auto caller = message().service().mid(1).replace('.', '_');
        const auto path = "/org/freedesktop/portal/desktop/request/" + caller + "/" +
                          (legacy ? "legacy_" : "") + options.value("handle_token").toString();
        auto reply = [path, response = code] {
            auto signal =
                QDBusMessage::createSignal(path, "org.freedesktop.portal.Request", "Response");
            signal << response << QVariantMap{{"uri", "file:///synthetic-screenshot.png"}};
            QDBusConnection::sessionBus().send(signal);
        };
        if (immediate)
            reply();
        else
            QTimer::singleShot(60, this, reply);
        return QDBusObjectPath(path);
    }
};

class FakeSession : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Session")
    std::function<void()> closed;

  public:
    FakeSession(QObject *parent, std::function<void()> callback)
        : QObject(parent), closed(std::move(callback)) {}
  public slots:
    void Close() { closed(); }
};
class FakeShortcuts : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.GlobalShortcuts")
    QStringList sessions, bound, closed;
    QDBusObjectPath response(const QVariantMap &options, const QVariantMap &results, int delay) {
        const auto caller = message().service().mid(1).replace('.', '_');
        const auto path = "/org/freedesktop/portal/desktop/request/" + caller + "/" +
                          options.value("handle_token").toString();
        QTimer::singleShot(delay, this, [path, results] {
            auto signal =
                QDBusMessage::createSignal(path, "org.freedesktop.portal.Request", "Response");
            signal << 0u << results;
            QDBusConnection::sessionBus().send(signal);
        });
        return QDBusObjectPath(path);
    }
  public slots:
    QDBusObjectPath CreateSession(const QVariantMap &options) {
        const auto caller = message().service().mid(1).replace('.', '_');
        const auto path = "/org/freedesktop/portal/desktop/session/" + caller + "/" +
                          options.value("session_handle_token").toString();
        sessions.append(path);
        auto *session = new FakeSession(this, [this, path] { closed.append(path); });
        QDBusConnection::sessionBus().registerObject(path, session,
                                                     QDBusConnection::ExportAllSlots);
        return response(options, {{"session_handle", path}}, sessions.size() == 1 ? 150 : 20);
    }
    QDBusObjectPath BindShortcuts(const QDBusObjectPath &session,
                                  const QList<PortalShortcut> &shortcuts, const QString &,
                                  const QVariantMap &options) {
        bound.append(session.path());
        return response(options, {{"shortcuts", QVariant::fromValue(shortcuts)}}, 20);
    }
    QVariantMap State() { return {{"sessions", sessions}, {"bound", bound}, {"closed", closed}}; }
    void ActivateAll() {
        for (const auto &session : sessions) {
            auto signal =
                QDBusMessage::createSignal("/org/freedesktop/portal/desktop",
                                           "org.freedesktop.portal.GlobalShortcuts", "Activated");
            signal << QVariant::fromValue(QDBusObjectPath(session)) << QString("once")
                   << qulonglong(1) << QVariantMap{};
            QDBusConnection::sessionBus().send(signal);
        }
    }
};

class PortalTest : public QObject {
    Q_OBJECT
    QProcess service;
    void restart(const QString &mode = "--fake") {
        if (service.state() != QProcess::NotRunning) {
            service.terminate();
            QVERIFY(service.waitForFinished(3000));
        }
        service.start(QCoreApplication::applicationFilePath(), {mode});
        QVERIFY(service.waitForStarted(3000));
        QVERIFY(service.waitForReadyRead(3000));
        QCOMPARE(service.readLine(), QByteArray("READY\n"));
    }
    void configure(uint code) {
        QDBusInterface remote("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                              "org.freedesktop.portal.Screenshot");
        QCOMPARE(remote.call("Configure", code).type(), QDBusMessage::ReplyMessage);
    }
    void behavior(bool legacy, bool immediate, uint version = 2, uint targets = 0) {
        QDBusInterface remote("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                              "org.freedesktop.portal.Screenshot");
        QCOMPARE(remote.call("Behavior", legacy, immediate, version, targets).type(),
                 QDBusMessage::ReplyMessage);
    }

  private slots:
    void initTestCase() { restart(); }
    void acceptedScreenshotReturnsUri() {
        configure(0);
        Portal portal;
        bool done = false;
        QString uri, error;
        portal.screenshot([&](QString result, QString problem) {
            uri = result;
            error = problem;
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
        QCOMPARE(uri, "file:///synthetic-screenshot.png");
        QVERIFY(error.isEmpty());
    }
    void requestHandlesAreFollowedIncludingResponsesBeforeTheMethodReply() {
        for (bool legacy : {false, true})
            for (bool immediate : {false, true}) {
                behavior(legacy, immediate);
                configure(0);
                Portal portal;
                bool done = false;
                portal.screenshot([&](QString uri, QString error) {
                    QVERIFY2(error.isEmpty(), qPrintable(error));
                    QCOMPARE(uri, "file:///synthetic-screenshot.png");
                    done = true;
                });
                QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
            }
        behavior(false, false);
    }
    void captureModesUsePortalCapabilitiesWithoutInventingMonitorSupport() {
        for (uint version : {2u, 3u}) {
            behavior(false, false, version, 5u);
            configure(0);
            for (const auto &mode : {"auto", "desktop", "region", "picker"}) {
                Portal portal;
                bool done = false;
                portal.screenshot(
                    [&](QString uri, QString error) {
                        QVERIFY(!uri.isEmpty());
                        QVERIFY(error.isEmpty());
                        done = true;
                    },
                    mode);
                QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
                QDBusInterface remote("org.freedesktop.portal.Desktop",
                                      "/org/freedesktop/portal/desktop",
                                      "org.freedesktop.portal.Screenshot");
                QDBusReply<QVariantMap> reply = remote.call("LastOptions");
                QVERIFY(reply.isValid());
                const auto options = reply.value();
                QCOMPARE(options.value("interactive").toBool(),
                         QString(mode) == "region" || QString(mode) == "picker");
                if (version == 3 && QString(mode) != "picker")
                    QCOMPARE(options.value("target").toUInt(), QString(mode) == "region" ? 4u : 1u);
                else
                    QVERIFY(!options.contains("target"));
            }
        }
        behavior(false, false);
    }
    void cancellationAndFailureAreReported() {
        for (uint code : {1u, 2u}) {
            configure(code);
            Portal portal;
            bool done = false;
            QString error;
            portal.screenshot([&](QString uri, QString problem) {
                QVERIFY(uri.isEmpty());
                error = problem;
                done = true;
            });
            QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
            QVERIFY(error.contains(code == 1 ? "cancelled" : "unavailable"));
        }
    }
    void concurrentCaptureIsRejectedAndClosedRequestIgnoresLateReply() {
        configure(0);
        Portal portal;
        bool first = false, second = false;
        portal.screenshot([&](QString, QString) { first = true; });
        portal.screenshot([&](QString, QString error) { second = error.contains("already open"); });
        QVERIFY(second);
        portal.cancelRequests();
        QTest::qWait(180);
        QVERIFY(!first);
        bool next = false;
        portal.screenshot(
            [&](QString uri, QString error) { next = !uri.isEmpty() && error.isEmpty(); });
        QTRY_VERIFY_WITH_TIMEOUT(next, 3000);
    }
    void replacingShortcutSessionsIgnoresStaleRepliesAndScreenshotCancellation() {
        restart("--fake-shortcuts");
        QDBusInterface remote("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                              "org.freedesktop.portal.GlobalShortcuts");
        auto state = [&] {
            QDBusReply<QVariantMap> reply = remote.call("State");
            return reply.value();
        };
        QStringList triggered, errors;
        const QJsonObject config{{"once", QJsonObject{{"sequence", "Ctrl+Alt+R"}}},
                                 {"continuous", QJsonObject{{"sequence", "Ctrl+Alt+Y"}}},
                                 {"snapshot", QJsonObject{{"sequence", "Ctrl+Alt+O"}}}};
        {
            Portal portal;
            auto trigger = [&](QString id) { triggered.append(id); };
            auto error = [&](QString text) { errors.append(text); };
            portal.bindShortcuts(config, trigger, error);
            QTRY_COMPARE_WITH_TIMEOUT(state()["sessions"].toStringList().size(), 1, 3000);
            portal.bindShortcuts(config, trigger, error);
            portal.cancelScreenshots();
            QTRY_COMPARE_WITH_TIMEOUT(state()["bound"].toStringList().size(), 1, 3000);
            QTRY_COMPARE_WITH_TIMEOUT(state()["closed"].toStringList().size(), 1, 3000);
            const auto result = state();
            QCOMPARE(result["bound"].toStringList().first(),
                     result["sessions"].toStringList().last());
            QVERIFY2(errors.isEmpty(), qPrintable(errors.join('\n')));
            QCOMPARE(remote.call("ActivateAll").type(), QDBusMessage::ReplyMessage);
            QTRY_COMPARE_WITH_TIMEOUT(triggered, QStringList{"once"}, 3000);
            // Switching to a native adapter must release completed portal grabs as well.
            portal.cancelRequests();
            QTRY_COMPARE_WITH_TIMEOUT(state()["closed"].toStringList().size(), 2, 3000);
            QCOMPARE(remote.call("ActivateAll").type(), QDBusMessage::ReplyMessage);
            QTest::qWait(80);
            QCOMPARE(triggered, QStringList{"once"});
        }
        QTRY_COMPARE_WITH_TIMEOUT(state()["closed"].toStringList().size(), 2, 3000);
        restart();
    }
    void cleanupTestCase() {
        service.terminate();
        service.waitForFinished(3000);
    }
};

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (qEnvironmentVariable("RANSLATOR_TEST_BUS") != "1")
        return 2;
    qDBusRegisterMetaType<PortalShortcut>();
    qDBusRegisterMetaType<QList<PortalShortcut>>();
    if (app.arguments().contains("--fake-shortcuts")) {
        FakeShortcuts portal;
        auto bus = QDBusConnection::sessionBus();
        if (!bus.registerService("org.freedesktop.portal.Desktop") ||
            !bus.registerObject("/org/freedesktop/portal/desktop", &portal,
                                QDBusConnection::ExportAllSlots))
            return 3;
        fprintf(stdout, "READY\n");
        fflush(stdout);
        return app.exec();
    }
    if (app.arguments().contains("--fake")) {
        FakePortal portal;
        auto bus = QDBusConnection::sessionBus();
        if (!bus.registerService("org.freedesktop.portal.Desktop") ||
            !bus.registerObject("/org/freedesktop/portal/desktop", &portal,
                                QDBusConnection::ExportAllSlots |
                                    QDBusConnection::ExportAllProperties))
            return 3;
        fprintf(stdout, "READY\n");
        fflush(stdout);
        return app.exec();
    }
    PortalTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "portal.moc"
