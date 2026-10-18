#include "gnome.hpp"
#include <QDBusVirtualObject>
#include <QtDBus>
#include <QtTest>
#include <cstdio>

namespace {
constexpr auto service = "io.github.ranslator.Ranslator.Desktop";
constexpr auto path = "/io/github/ranslator/Ranslator/Desktop";
constexpr auto interface = "io.github.ranslator.Desktop1";
QString context(int index = 0) {
    return QString::fromUtf8(QJsonDocument(QJsonObject{{"index", index},
                                                       {"x", index * 800},
                                                       {"y", 0},
                                                       {"width", 800},
                                                       {"height", 800},
                                                       {"scale", 1},
                                                       {"cursor_x", 25},
                                                       {"cursor_y", 30}})
                                 .toJson(QJsonDocument::Compact));
}
} // namespace

class FakeDesktop : public QDBusVirtualObject {
    Q_OBJECT
    QString lastConfiguration;
    int captureDelay = 0;

  public:
    QString introspect(const QString &) const override { return {}; }
    bool handleMessage(const QDBusMessage &message, const QDBusConnection &bus) override {
        const auto args = message.arguments();
        QVariantList result;
        if (message.member() == "Configure") {
            lastConfiguration = args[0].toString();
            result << QStringLiteral("{\"version\":1,\"errors\":[]}");
        } else if (message.member() == "GetContext") {
            result << context();
        } else if (message.member() == "Capture") {
            QImage image(40, 20, QImage::Format_RGB32);
            image.fill(Qt::red);
            QByteArray png;
            QBuffer output(&png);
            image.save(&output, "PNG");
            result << png << context(args[1].toInt());
            QTimer::singleShot(captureDelay, this,
                               [bus, message, result] { bus.send(message.createReply(result)); });
            return true;
        } else if (message.member() == "ArmSelection") {
            // Signals below are explicit test commands, permitting deliberate stale messages.
        } else if (message.member() == "EmitSelection") {
            auto signal = QDBusMessage::createSignal(path, interface, "SelectionChanged");
            signal << args[0].toUInt() << args[1].toString();
            bus.send(signal);
        } else if (message.member() == "EmitShortcut") {
            auto signal = QDBusMessage::createSignal(path, interface, "ShortcutActivated");
            signal << QStringLiteral("snapshot") << context(1);
            bus.send(signal);
        } else if (message.member() == "LastConfiguration") {
            result << lastConfiguration;
        } else if (message.member() == "CaptureDelay") {
            captureDelay = args[0].toInt();
        } else {
            bus.send(message.createErrorReply(QDBusError::UnknownMethod, "Unknown test method"));
            return true;
        }
        bus.send(message.createReply(result));
        return true;
    }
};

class GnomeTest : public QObject {
    Q_OBJECT
    QProcess fake;
    void start() {
        fake.start(QCoreApplication::applicationFilePath(), {"--fake"});
        QVERIFY(fake.waitForStarted(3000));
        QVERIFY(fake.waitForReadyRead(3000));
        QCOMPARE(fake.readLine(), QByteArray("READY\n"));
    }
    QDBusMessage call(const QString &name, const QVariantList &args = {}) {
        auto message = QDBusMessage::createMethodCall(service, path, interface, name);
        message.setArguments(args);
        return QDBusConnection::sessionBus().call(message, QDBus::Block, 1000);
    }
  private slots:
    void init() { start(); }
    void cleanup() {
        fake.terminate();
        QVERIFY(fake.waitForFinished(3000));
    }
    void shortcutsRetainInvocationMonitorAndSendOnlySequences() {
        GnomeDesktop desktop;
        QVERIFY(desktop.available());
        QJsonObject config;
        for (auto id : {"once", "continuous", "snapshot"})
            config.insert(id, QJsonObject{{"sequence", "Ctrl+Alt+R"}, {"secret", "must-not-send"}});
        config.insert("provider_key", "must-not-send");
        QString action, error;
        QJsonObject invocation;
        desktop.bind(
            config,
            [&](QString id, QJsonObject info) {
                action = id;
                invocation = info;
            },
            [&](QString problem) { error = problem; });
        QTRY_VERIFY(!call("LastConfiguration").arguments().value(0).toString().isEmpty());
        const auto sent = call("LastConfiguration").arguments().value(0).toString();
        QVERIFY(!sent.contains("secret"));
        QVERIFY(!sent.contains("provider_key"));
        call("EmitShortcut");
        QTRY_COMPARE(action, QString("snapshot"));
        QCOMPARE(invocation.value("index").toInt(), 1);
        QCOMPARE(invocation.value("cursor_x").toInt(), 25);
        QVERIFY(error.isEmpty());
    }
    void capturesSelectedMonitorAndIgnoresCancelledResult() {
        GnomeDesktop desktop;
        QImage captured;
        QJsonObject monitor;
        QString error;
        bool complete = false;
        desktop.capture("auto", 1, [&](QImage image, QJsonObject context, QString problem) {
            captured = image;
            monitor = context;
            error = problem;
            complete = true;
        });
        QTRY_VERIFY(complete);
        QVERIFY(error.isEmpty());
        QCOMPARE(captured.size(), QSize(40, 20));
        QCOMPARE(captured.pixelColor(10, 10), QColor(Qt::red));
        QCOMPARE(monitor.value("index").toInt(), 1);
        call("CaptureDelay", {100});
        complete = false;
        desktop.capture("auto", 0, [&](QImage, QJsonObject, QString) { complete = true; });
        desktop.cancelCapture();
        QTest::qWait(200);
        QVERIFY(!complete);
    }
    void staleSelectionsCannotCrossDisarmAndRearm() {
        GnomeDesktop desktop;
        QStringList text;
        auto selected = [&](QString value) { text << value; };
        desktop.armSelection(true, false, selected);
        desktop.armSelection(false, false, {});
        desktop.armSelection(true, false, selected);
        call("EmitSelection", {1u, "old"});
        call("EmitSelection", {3u, "current"});
        QTRY_COMPARE(text.size(), 1);
        QCOMPARE(text[0], QString("current"));
        desktop.armSelection(false, false, {});
        call("EmitSelection", {3u, "after disarm"});
        QTest::qWait(80);
        QCOMPARE(text.size(), 1);
    }
    void extensionRestartNotifiesListenerAndRestoresAvailability() {
        GnomeDesktop desktop;
        QSignalSpy availability(&desktop, &GnomeDesktop::availabilityChanged);
        QString failure;
        desktop.armSelection(true, false, [](QString) {}, [&](QString error) { failure = error; });
        call("CaptureDelay", {500});
        QString captureFailure;
        desktop.capture("auto", 0,
                        [&](QImage, QJsonObject, QString error) { captureFailure = error; });
        fake.terminate();
        QVERIFY(fake.waitForFinished(3000));
        QTRY_VERIFY(!desktop.available());
        QVERIFY(!failure.isEmpty());
        QVERIFY(!captureFailure.isEmpty());
        QCOMPARE(availability.size(), 1);
        start();
        QTRY_VERIFY(desktop.available());
        QCOMPARE(availability.size(), 2);
    }
};

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    if (!qEnvironmentVariableIsSet("RANSLATOR_TEST_BUS"))
        return 2;
    if (app.arguments().contains("--fake")) {
        FakeDesktop desktop;
        auto bus = QDBusConnection::sessionBus();
        if (!bus.registerService(service) || !bus.registerVirtualObject(path, &desktop))
            return 3;
        std::puts("READY");
        std::fflush(stdout);
        return app.exec();
    }
    GnomeTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "gnome.moc"
