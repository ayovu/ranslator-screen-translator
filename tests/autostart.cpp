#include "autostart.hpp"
#include <QCoreApplication>
#include <QDBusContext>
#include <QDBusInterface>
#include <QDBusReply>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

class BackgroundFixture : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Background")
    uint mode = 0;
    int delay = 20;
    bool enabled = false;
    QVariantMap options;
    int calls = 0;
  public slots:
    void Configure(uint nextMode, int nextDelay, bool nextEnabled) {
        mode = nextMode;
        delay = nextDelay;
        enabled = nextEnabled;
    }
    QVariantMap State() { return {{"options", options}, {"calls", calls}}; }
    QDBusObjectPath RequestBackground(const QString &, const QVariantMap &nextOptions) {
        options = nextOptions;
        ++calls;
        const auto caller = message().service().mid(1).replace('.', '_');
        const auto path = "/org/freedesktop/portal/desktop/request/" + caller + "/" +
                          options["handle_token"].toString();
        const auto responseMode = mode;
        const auto responseEnabled = enabled;
        if (delay >= 0)
            QTimer::singleShot(delay, this, [path, responseMode, responseEnabled] {
                QVariantMap result{{"background", true}};
                if (responseMode == 0)
                    result["autostart"] = responseEnabled;
                if (responseMode == 4)
                    result["autostart"] = "false";
                auto response =
                    QDBusMessage::createSignal(path, "org.freedesktop.portal.Request", "Response");
                response << (responseMode < 3 ? responseMode : 0u) << result;
                QDBusConnection::sessionBus().send(response);
            });
        return QDBusObjectPath(path);
    }
};
static void writeExecutable(const QString &path) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("#!/bin/sh\nexit 0\n");
    file.close();
    QVERIFY(file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                QFileDevice::ExeOwner));
}
static QByteArray readFile(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}
static Autostart::Environment environment(const QString &root) {
    Autostart::Environment env;
    env.configHome = root + "/config";
    env.configDirs = {root + "/system"};
    env.desktops = {"KDE"};
    env.executable = root + "/ranslator";
    return env;
}
class AutostartTests : public QObject {
    Q_OBJECT
    QProcess service;
    void configure(uint code, int delay, bool enabled) {
        QDBusInterface portal("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                              "org.freedesktop.portal.Background");
        const auto reply = portal.call("Configure", code, delay, enabled);
        QVERIFY(reply.type() != QDBusMessage::ErrorMessage);
    }
    QVariantMap portalState() {
        QDBusInterface portal("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                              "org.freedesktop.portal.Background");
        QDBusReply<QVariantMap> reply = portal.call("State");
        return reply.value();
    }
  private slots:
    void initTestCase() {
        service.start(QCoreApplication::applicationFilePath(), {"--fake"});
        QVERIFY(service.waitForStarted());
        QVERIFY(service.waitForReadyRead());
    }
    void cleanupTestCase() {
        service.terminate();
        QVERIFY(service.waitForFinished());
    }
    void native_registration_reads_external_state_and_overrides_system_entry() {
        QTemporaryDir dir;
        auto env = environment(dir.path());
        writeExecutable(env.executable);
        const auto path = env.configHome + "/autostart/io.github.ranslator.Ranslator.desktop";
        Autostart startup(env);
        QVERIFY(!startup.state().enabled);
        QVERIFY(!QFile::exists(path)); // Merely reading settings never creates registration.
        startup.setEnabled(true);
        QVERIFY(startup.isBusy());
        QTRY_VERIFY(!startup.isBusy());
        QVERIFY(startup.state().enabled);
        QVERIFY(readFile(path).contains(" --background\n"));
        QDir().mkpath(env.configDirs[0] + "/autostart");
        QVERIFY(QFile::copy(path, env.configDirs[0] +
                                      "/autostart/io.github.ranslator.Ranslator.desktop"));
        startup.setEnabled(false);
        QTRY_VERIFY(!startup.isBusy());
        QVERIFY(readFile(path).contains("Hidden=true"));
        QVERIFY(!startup.state().enabled);
        QVERIFY(QFile::remove(path));
        startup.refresh();
        QVERIFY(startup.state().enabled);
        QVERIFY(QFile::remove(env.executable));
        startup.refresh();
        QVERIFY(!startup.state().enabled);
    }
    void launcher_uses_own_original_appimage_not_inherited_editor_environment() {
        QTemporaryDir dir;
        auto env = environment(dir.path());
        writeExecutable(env.executable);
        env.executable = dir.path() + "/.local/bin/ranslator";
        writeExecutable(env.executable);
        writeExecutable(dir.path() + "/AppRun"); // An unrelated ancestor is not our bundle.
        env.appDir = dir.path() + "/other-editor";
        env.appImage = dir.path() + "/editor.AppImage";
        writeExecutable(env.appDir + "/usr/bin/ranslator");
        writeExecutable(env.appImage);
        QString error;
        QCOMPARE(Autostart::launcher(env, &error), env.executable);
        QVERIFY(error.isEmpty());
        env.executable = env.appDir + "/usr/bin/ranslator";
        QCOMPARE(Autostart::launcher(env, &error), env.appImage);
        QVERIFY(QFile::remove(env.appImage));
        QVERIFY(Autostart::launcher(env, &error).isEmpty());
        QVERIFY(!error.isEmpty());
        env.appImage.clear();
        writeExecutable(env.appDir + "/AppRun");
        error.clear();
        QCOMPARE(Autostart::launcher(env, &error), env.appDir + "/AppRun");
        env.appImage = dir.path() + "/.mount_test/AppRun";
        writeExecutable(env.appImage);
        error.clear();
        QVERIFY(Autostart::launcher(env, &error).isEmpty());
        QVERIFY(!error.isEmpty());
    }
    void tryexec_roundtrips_spaces_backslashes_and_percent_paths() {
        QTemporaryDir dir;
        auto env = environment(dir.path());
        env.executable = dir.path() + "/Chinese 中文 with \\slash $dollar `tick` %f\"quote'";
        writeExecutable(env.executable);
        Autostart startup(env);
        startup.setEnabled(true);
        QTRY_VERIFY(!startup.isBusy());
        QVERIFY(startup.state().enabled);
        startup.refresh();
        QVERIFY(startup.state().enabled);
        const auto entry =
            readFile(env.configHome + "/autostart/io.github.ranslator.Ranslator.desktop");
        QVERIFY(entry.contains("Exec=/usr/bin/env -- \""));
        QVERIFY(entry.contains("%%f"));
        QVERIFY(QFile::remove(env.executable));
        startup.refresh();
        QVERIFY(!startup.state().enabled);
    }
    void failed_write_and_unsafe_path_leave_registration_unchanged() {
        QTemporaryDir dir;
        auto env = environment(dir.path());
        writeExecutable(env.executable);
        Autostart startup(env);
        QSignalSpy failure(&startup, &Autostart::failed);
        QVERIFY(QFile::remove(env.executable));
        startup.setEnabled(true);
        QTRY_VERIFY(!startup.isBusy());
        QCOMPARE(failure.size(), 1);
        QVERIFY(!startup.state().enabled);
        QVERIFY(
            !QFile::exists(env.configHome + "/autostart/io.github.ranslator.Ranslator.desktop"));
        env.executable = dir.path() + "/bad\nHidden=false";
        writeExecutable(env.executable);
        QString error;
        QVERIFY(Autostart::launcher(env, &error).isEmpty());
        env.executable = dir.path() + "/bad=launcher";
        writeExecutable(env.executable);
        error.clear();
        QVERIFY(Autostart::launcher(env, &error).isEmpty());
        env.configHome = dir.path() + "/file";
        writeExecutable(env.configHome);
        env.executable = env.configHome;
        Autostart unwritable(env);
        QSignalSpy diskFailure(&unwritable, &Autostart::failed);
        unwritable.setEnabled(true);
        QTRY_VERIFY(!unwritable.isBusy());
        QCOMPARE(diskFailure.size(), 1);
        QVERIFY(!unwritable.state().enabled);
    }
    void flatpak_read_is_side_effect_free_and_requests_use_packaged_launcher() {
        QTemporaryDir dir;
        auto env = environment(dir.path());
        env.flatpak = true;
        const auto before = portalState()["calls"].toInt();
        Autostart startup(env);
        startup.refresh();
        QCOMPARE(portalState()["calls"].toInt(), before);
        QVERIFY(!startup.state().known);
        configure(0, 30, true);
        startup.setEnabled(true);
        startup.refresh();
        startup.setEnabled(false);
        QTRY_VERIFY(!startup.isBusy());
        QVERIFY(startup.state().enabled);
        QCOMPARE(portalState()["calls"].toInt(), before + 1);
        const auto options = qdbus_cast<QVariantMap>(portalState()["options"]);
        QCOMPARE(options["commandline"].toStringList(),
                 QStringList({"ranslator-launch", "--background"}));
        QVERIFY(options["autostart"].toBool());
        Autostart reopened(env);
        QVERIFY(reopened.state().enabled);
        QVERIFY(!reopened.state().known);
        QVERIFY(reopened.state().detail.contains("Last confirmed"));
        QVERIFY(
            !QFile::exists(env.configHome + "/autostart/io.github.ranslator.Ranslator.desktop"));
        configure(0, 10, false);
        startup.setEnabled(false);
        QTRY_VERIFY(!startup.isBusy());
        QVERIFY(!startup.state().enabled);
    }
    void flatpak_cancellation_missing_and_malformed_response_preserve_confirmed_state() {
        QTemporaryDir dir;
        auto env = environment(dir.path());
        env.flatpak = true;
        Autostart startup(env);
        QSignalSpy failures(&startup, &Autostart::failed);
        configure(0, 10, true);
        startup.setEnabled(true);
        QTRY_VERIFY(!startup.isBusy());
        for (uint mode : {1u, 2u, 3u, 4u}) {
            configure(mode, 10, false);
            startup.setEnabled(false);
            QTRY_VERIFY(!startup.isBusy());
            QVERIFY(startup.state().enabled);
            Autostart reopened(env);
            QVERIFY(reopened.state().enabled);
        }
        QCOMPARE(failures.size(), 4);
        // An opposite *typed* response is authoritative, even when the request was refused.
        configure(0, 10, false);
        startup.setEnabled(true);
        QTRY_VERIFY(!startup.isBusy());
        QVERIFY(!startup.state().enabled);
        QCOMPARE(failures.size(), 5);
    }
    void portal_timeout_is_single_completion_and_ignores_late_response() {
        configure(0, 90, true);
        Portal portal;
        int calls = 0;
        QString failure;
        portal.setAutostart(
            true,
            [&](bool, QString error) {
                ++calls;
                failure = error;
            },
            20);
        QTRY_COMPARE(calls, 1);
        QVERIFY(!failure.isEmpty());
        QTest::qWait(130);
        QCOMPARE(calls, 1);
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (!qEnvironmentVariableIsSet("RANSLATOR_TEST_BUS"))
        return 77;
    if (app.arguments().contains("--fake")) {
        BackgroundFixture fixture;
        auto bus = QDBusConnection::sessionBus();
        if (!bus.registerService("org.freedesktop.portal.Desktop") ||
            !bus.registerObject("/org/freedesktop/portal/desktop", &fixture,
                                QDBusConnection::ExportAllSlots))
            return 2;
        puts("ready");
        fflush(stdout);
        return app.exec();
    }
    if (app.arguments().value(1) == "--write-entry") {
        const auto args = app.arguments();
        auto env = environment(args.value(2));
        env.executable = args.value(3);
        env.appDir = args.value(4);
        env.appImage = args.value(5);
        Autostart startup(env);
        QObject::connect(&startup, &Autostart::changed, &app, [&] {
            if (!startup.isBusy())
                app.exit(startup.state().enabled ? 0 : 2);
        });
        startup.setEnabled(true);
        return app.exec();
    }
    AutostartTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "autostart.moc"
