#include "accessibility.hpp"
#include <QApplication>
#include <QDBusArgument>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusVariant>
#include <QDBusVirtualObject>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTextEdit>
#include <QTimer>
#include <cstdio>

namespace {
constexpr auto registry = "org.a11y.atspi.Registry";
constexpr auto controlPath = "/org/a11y/atspi/test";
constexpr auto textPath = "/org/a11y/atspi/accessible/text";
constexpr auto rootPath = "/org/a11y/atspi/accessible/root";

QString registryExecutable() {
    const auto override = qEnvironmentVariable("RANSLATOR_TEST_ATSPI_REGISTRY");
    if (!override.isEmpty())
        return QStandardPaths::findExecutable(override);
    auto executable = QStandardPaths::findExecutable("at-spi2-registryd");
    if (!executable.isEmpty())
        return executable;
    for (const auto &directory : {"/usr/libexec", "/usr/lib/at-spi2-core", "/usr/lib",
                                  "/usr/local/libexec", "/usr/local/lib/at-spi2-core"}) {
        executable = QStandardPaths::findExecutable("at-spi2-registryd", {directory});
        if (!executable.isEmpty())
            return executable;
    }
    return {};
}
} // namespace

class FakeAccessibility : public QDBusVirtualObject {
    Q_OBJECT
    QString text = "Hello", appName = "Fixture editor";
    uint role = 61;
    int delay = 0, start = 0, end = 5, count = 1, reads = 0, registrations = 0;
    bool failRegistry = false;

  public:
    QString introspect(const QString &) const override { return {}; }
    bool handleMessage(const QDBusMessage &message, const QDBusConnection &bus) override {
        const auto method = message.member();
        QVariantList output;
        if (method == "GetAddress") {
            output << qEnvironmentVariable("DBUS_SESSION_BUS_ADDRESS");
        } else if (message.interface() == "org.ranslator.Test") {
            if (method == "Configure") {
                const auto args = message.arguments();
                text = args[0].toString();
                role = args[1].toUInt();
                delay = args[2].toInt();
                appName = args[3].toString();
                start = args[4].toInt();
                end = args[5].toInt();
                count = args[6].toInt();
                failRegistry = args[7].toBool();
                reads = 0;
            } else if (method == "Emit") {
                auto event = QDBusMessage::createSignal(textPath, "org.a11y.atspi.Event.Object",
                                                        "TextSelectionChanged");
                event << QString() << 0 << 0 << QVariant::fromValue(QDBusVariant(QString()))
                      << QVariantMap();
                bus.send(event);
            } else if (method == "Reads") {
                output << reads;
            } else if (method == "Registrations") {
                output << registrations;
            }
        } else if (method == "RegisterEvent") {
            if (failRegistry) {
                bus.send(message.createErrorReply(QDBusError::AccessDenied,
                                                  "Fixture denied registration"));
                return true;
            }
            ++registrations;
        } else if (method == "DeregisterEvent") {
            --registrations;
        } else if (method == "GetRole") {
            output << role;
        } else if (method == "GetApplication") {
            QDBusArgument reference;
            reference.beginStructure();
            reference << bus.baseService() << QDBusObjectPath(rootPath);
            reference.endStructure();
            output << QVariant::fromValue(reference);
        } else if (method == "Get" && message.interface() == "org.freedesktop.DBus.Properties") {
            output << QVariant::fromValue(QDBusVariant(appName));
        } else if (method == "GetNSelections") {
            output << count;
        } else if (method == "GetSelection") {
            output << start << end;
        } else if (method == "GetText") {
            ++reads;
            output << text;
            QTimer::singleShot(delay, this,
                               [bus, message, output] { bus.send(message.createReply(output)); });
            return true;
        } else {
            bus.send(message.createErrorReply(QDBusError::UnknownMethod,
                                              "Fixture method is not implemented"));
            return true;
        }
        bus.send(message.createReply(output));
        return true;
    }
};

class AccessibilityTest : public QObject {
    Q_OBJECT
    QProcess service;
    QDBusMessage call(const QString &method, const QVariantList &args = {}) {
        auto message =
            QDBusMessage::createMethodCall(registry, controlPath, "org.ranslator.Test", method);
        message.setArguments(args);
        return QDBusConnection::sessionBus().call(message, QDBus::Block, 2000);
    }
    void configure(QString text = "Selected phrase", uint role = 61, int delay = 0,
                   QString name = "Fixture editor", int start = 0, int end = 15, int count = 1,
                   bool failedRegistry = false) {
        QCOMPARE(
            call("Configure", {text, role, delay, name, start, end, count, failedRegistry}).type(),
            QDBusMessage::ReplyMessage);
    }
    void fire() { QCOMPARE(call("Emit").type(), QDBusMessage::ReplyMessage); }
    int reads() { return call("Reads").arguments().value(0).toInt(); }
    QString address() const { return qEnvironmentVariable("DBUS_SESSION_BUS_ADDRESS"); }
    void arm(AccessibilitySelection &selection, quint64 generation = 1) {
        QSignalSpy available(&selection, &AccessibilitySelection::availability);
        selection.setArmed(true, generation);
        QTRY_VERIFY_WITH_TIMEOUT(!available.isEmpty(), 2500);
        QVERIFY2(available.last()[0].toBool(), qPrintable(available.last()[1].toString()));
        QCOMPARE(available.last()[2].toULongLong(), generation);
    }

  private slots:
    void initTestCase() {
        service.start(QCoreApplication::applicationFilePath(), {"--fake"});
        QVERIFY(service.waitForStarted(2000));
        QVERIFY(service.waitForReadyRead(2000));
        QCOMPARE(service.readLine(), QByteArray("READY\n"));
    }
    void init() { configure(); }
    void selection_is_emitted_only_while_armed_with_its_session_generation() {
        AccessibilitySelection selection(nullptr, address());
        QSignalSpy selected(&selection, &AccessibilitySelection::selected);
        fire();
        QTest::qWait(300);
        QCOMPARE(selected.size(), 0);
        arm(selection, 47);
        fire();
        QTRY_COMPARE_WITH_TIMEOUT(selected.size(), 1, 2500);
        QCOMPARE(selected.first()[0].toString(), "Selected phrase");
        QCOMPARE(selected.first()[1].toULongLong(), 47);
        selection.setArmed(false, 48);
        fire();
        QTest::qWait(350);
        QCOMPARE(selected.size(), 1);
    }
    void password_and_own_application_fields_are_never_read() {
        AccessibilitySelection selection(nullptr, address());
        QSignalSpy selected(&selection, &AccessibilitySelection::selected);
        arm(selection);
        configure("password", 40);
        fire();
        QTest::qWait(400);
        QCOMPARE(reads(), 0);
        configure("our editor", 61, 0, QCoreApplication::applicationName());
        fire();
        QTest::qWait(400);
        QCOMPARE(reads(), 0);
        QCOMPARE(selected.size(), 0);
    }
    void invalid_or_unbounded_ranges_are_not_requested() {
        AccessibilitySelection selection(nullptr, address());
        QSignalSpy selected(&selection, &AccessibilitySelection::selected);
        arm(selection);
        for (const auto &range :
             {QPair<int, int>(-1, 5), QPair<int, int>(5, 1), QPair<int, int>(0, 65537)}) {
            configure("bounded", 61, 0, "Fixture editor", range.first, range.second);
            fire();
            QTest::qWait(300);
            QCOMPARE(reads(), 0);
        }
        configure(QString(65537, 'x'));
        fire();
        QTRY_COMPARE_WITH_TIMEOUT(reads(), 1, 2000);
        QTest::qWait(100);
        QCOMPARE(selected.size(), 0);
    }
    void stale_text_replies_cannot_cross_disarm_and_rearm() {
        AccessibilitySelection selection(nullptr, address());
        QSignalSpy selected(&selection, &AccessibilitySelection::selected);
        arm(selection, 10);
        configure("old selection", 61, 600);
        fire();
        QTRY_COMPARE_WITH_TIMEOUT(reads(), 1, 2500);
        selection.setArmed(false, 11);
        arm(selection, 12);
        configure("fresh selection");
        fire();
        QTRY_COMPARE_WITH_TIMEOUT(selected.size(), 1, 2500);
        QCOMPARE(selected.first()[0].toString(), "fresh selection");
        QCOMPARE(selected.first()[1].toULongLong(), 12);
        QTest::qWait(650);
        QCOMPARE(selected.size(), 1);
    }
    void rapidly_changing_selection_discards_the_previous_reply() {
        AccessibilitySelection selection(nullptr, address());
        QSignalSpy selected(&selection, &AccessibilitySelection::selected);
        arm(selection);
        configure("old selection", 61, 600);
        fire();
        QTRY_COMPARE_WITH_TIMEOUT(reads(), 1, 2500);
        configure("latest selection");
        fire();
        QTRY_COMPARE_WITH_TIMEOUT(selected.size(), 1, 2500);
        QCOMPARE(selected.first()[0].toString(), "latest selection");
        QTest::qWait(650);
        QCOMPARE(selected.size(), 1);
    }
    void unavailable_registry_reports_a_useful_fallback() {
        configure("unused", 61, 0, "Fixture editor", 0, 5, 1, true);
        AccessibilitySelection selection(nullptr, address());
        QSignalSpy available(&selection, &AccessibilitySelection::availability);
        selection.setArmed(true, 81);
        QTRY_COMPARE_WITH_TIMEOUT(available.size(), 1, 2500);
        QVERIFY(!available.first()[0].toBool());
        QVERIFY(available.first()[1].toString().contains("Snapshot"));
        QCOMPARE(available.first()[2].toULongLong(), 81);
    }
    void real_qt_and_gtk_text_selections() {
        const auto registryProgram = registryExecutable();
        QVERIFY2(!registryProgram.isEmpty(),
                 "Install the at-spi2-core accessibility registry, or set "
                 "RANSLATOR_TEST_ATSPI_REGISTRY to its executable path.");
        // A second, private bus hosts the real registry and synthetic editors.
        // No accessible text or events from the user's desktop can enter it.
        QProcess daemon;
        daemon.start("dbus-daemon", {"--config-file=" + QString(RANSLATOR_SOURCE_DIR) +
                                         "/tests/fixtures/session-bus.conf",
                                     "--nofork", "--print-address"});
        QVERIFY(daemon.waitForStarted(2000));
        QVERIFY(daemon.waitForReadyRead(2000));
        const auto busAddress = QString::fromUtf8(daemon.readLine()).trimmed();
        QVERIFY(busAddress.startsWith("unix:"));
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("AT_SPI_BUS_ADDRESS", busAddress);
        environment.insert("DBUS_SESSION_BUS_ADDRESS", busAddress);
        environment.insert("QT_LINUX_ACCESSIBILITY_ALWAYS_ON", "1");
        environment.insert("NO_AT_BRIDGE", "0");
        QProcess registryProcess;
        registryProcess.setProcessEnvironment(environment);
        registryProcess.start(registryProgram, {});
        QVERIFY(registryProcess.waitForStarted(2000));
        auto registryBus = QDBusConnection::connectToBus(busAddress, "real-accessibility-test");
        QTRY_VERIFY_WITH_TIMEOUT(registryBus.interface()->isServiceRegistered(registry).value(),
                                 3000);
        {
            AccessibilitySelection selection(nullptr, busAddress);
            arm(selection, 92);
            QSignalSpy selected(&selection, &AccessibilitySelection::selected);
            for (bool gtk : {false, true}) {
                QProcess editor;
                editor.setProcessEnvironment(environment);
                if (gtk)
                    editor.start("/usr/bin/python3", {QString(RANSLATOR_SOURCE_DIR) +
                                                      "/tests/fixtures/accessibility-gtk.py"});
                else
                    editor.start(QCoreApplication::applicationFilePath(), {"--qt-fixture"});
                QVERIFY(editor.waitForStarted(2000));
                QVERIFY2(editor.waitForReadyRead(4000), editor.readAllStandardError().constData());
                QCOMPARE(editor.readLine(), QByteArray("READY\n"));
                // Allow the toolkit to register its accessibility application.
                QTest::qWait(350);
                selected.clear();
                editor.write("select\n");
                editor.waitForBytesWritten(1000);
                QTRY_COMPARE_WITH_TIMEOUT(selected.size(), 1, 4000);
                QCOMPARE(selected.first()[0].toString(),
                         gtk ? "GTK selected phrase" : "Qt selected phrase");
                QCOMPARE(selected.first()[1].toULongLong(), 92);
                editor.terminate();
                QVERIFY(editor.waitForFinished(2000));
            }
        }
        registryProcess.terminate();
        registryProcess.waitForFinished(2000);
        daemon.terminate();
        daemon.waitForFinished(2000);
        QDBusConnection::disconnectFromBus("real-accessibility-test");
    }
    void stopped_registry_is_reported_and_a_new_session_can_reconnect() {
        AccessibilitySelection selection(nullptr, address());
        QSignalSpy available(&selection, &AccessibilitySelection::availability);
        QSignalSpy selected(&selection, &AccessibilitySelection::selected);
        arm(selection, 101);
        service.terminate();
        QVERIFY(service.waitForFinished(2000));
        QTRY_VERIFY_WITH_TIMEOUT(available.size() >= 2, 2000);
        QVERIFY(!available.last()[0].toBool());
        QCOMPARE(available.last()[2].toULongLong(), 101);
        service.start(QCoreApplication::applicationFilePath(), {"--fake"});
        QVERIFY(service.waitForStarted(2000));
        QVERIFY(service.waitForReadyRead(2000));
        QCOMPARE(service.readLine(), QByteArray("READY\n"));
        configure("After registry restart");
        arm(selection, 102);
        fire();
        QTRY_COMPARE_WITH_TIMEOUT(selected.size(), 1, 2000);
        QCOMPARE(selected.first()[0].toString(), "After registry restart");
        QCOMPARE(selected.first()[1].toULongLong(), 102);
    }
    void cleanupTestCase() {
        service.terminate();
        QVERIFY(service.waitForFinished(2000));
    }
};

int main(int argc, char **argv) {
    if (argc > 1 && QByteArray(argv[1]) == "--qt-fixture") {
        QApplication app(argc, argv);
        app.setApplicationName("Selection Qt fixture");
        QTextEdit editor;
        editor.setPlainText("Qt selected phrase");
        editor.show();
        QSocketNotifier input(0, QSocketNotifier::Read);
        QObject::connect(&input, &QSocketNotifier::activated, &editor, [&] {
            char line[64];
            if (fgets(line, sizeof(line), stdin))
                editor.selectAll();
        });
        QTimer::singleShot(100, [&] {
            printf("READY\n");
            fflush(stdout);
        });
        return app.exec();
    }
    QCoreApplication app(argc, argv);
    if (argc > 1 && QByteArray(argv[1]) == "--fake") {
        FakeAccessibility fake;
        auto bus = QDBusConnection::sessionBus();
        if (!bus.registerService(registry) || !bus.registerService("org.a11y.Bus") ||
            !bus.registerVirtualObject("/org/a11y", &fake, QDBusConnection::SubPath))
            return 2;
        printf("READY\n");
        fflush(stdout);
        return app.exec();
    }
    AccessibilityTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "accessibility.moc"
