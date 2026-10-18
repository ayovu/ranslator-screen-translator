#include "x11.hpp"
#include "capture.hpp"
#include <QApplication>
#include <QProcess>
#include <QTest>

// Only synthetic fixture windows on a private Xvfb server are used by this test.
class X11Test : public QObject {
    Q_OBJECT
    static QJsonObject config(QString once = "Ctrl+Alt+R") {
        return {{"once", QJsonObject{{"sequence", once}}},
                {"continuous", QJsonObject{{"sequence", "Ctrl+Alt+Y"}}},
                {"snapshot", QJsonObject{{"sequence", "Ctrl+Alt+O"}}}};
    }
    static void key(const QString &sequence) {
        QCOMPARE(QProcess::execute("xdotool", {"key", "--clearmodifiers", sequence}), 0);
    }
  private slots:
    void initTestCase() {
        QVERIFY2(qEnvironmentVariable("RANSLATOR_TEST_X11") == "1",
                 "Run only on the isolated Xvfb display supplied by the test script.");
        QVERIFY(Capture::x11Available());
    }
    void shortcutsWorkWithoutKdeOrAPortalAndIgnoreLockKeys() {
        X11Shortcuts shortcuts;
        QVERIFY(shortcuts.available());
        QStringList triggered, errors;
        shortcuts.bind(
            config(), [&](QString id) { triggered.append(id); },
            [&](QString error) { errors.append(error); });
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join('\n')));
        key("ctrl+alt+r");
        QTRY_COMPARE_WITH_TIMEOUT(triggered, QStringList{"once"}, 2000);
        key("Caps_Lock");
        key("Num_Lock");
        key("ctrl+alt+o");
        QTRY_COMPARE_WITH_TIMEOUT(triggered, (QStringList{"once", "snapshot"}), 2000);
        key("Caps_Lock");
        key("Num_Lock");
    }
    void conflictingGrabsAreReportedAndRebindingReleasesOldKeys() {
        X11Shortcuts first, second;
        QStringList triggered, errors;
        first.bind(config(), [&](QString id) { triggered.append(id); }, [](QString) {});
        second.bind(config(), [](QString) {}, [&](QString error) { errors.append(error); });
        QCOMPARE(errors.size(), 3);
        QVERIFY(errors.first().contains("already used"));
        first.bind(config("Ctrl+Alt+T"), [&](QString id) { triggered.append(id); }, [](QString) {});
        key("ctrl+alt+r");
        QTest::qWait(100);
        QVERIFY(triggered.isEmpty());
        key("ctrl+alt+t");
        QTRY_COMPARE_WITH_TIMEOUT(triggered, QStringList{"once"}, 2000);
    }
    void repeatedRealShortcutPressesWorkButHoldingDoesNotRepeat() {
        X11Shortcuts shortcuts;
        QStringList triggered, errors;
        shortcuts.bind(
            config(), [&](QString id) { triggered.append(id); },
            [&](QString error) { errors.append(error); });
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join('\n')));
        key("ctrl+alt+r");
        QTRY_COMPARE_WITH_TIMEOUT(triggered.size(), 1, 2000);
        key("ctrl+alt+r");
        QTRY_COMPARE_WITH_TIMEOUT(triggered.size(), 2, 2000);

        // The server is private: shorten its repeat delay and prove that a sustained
        // real key hold produces only one activation while the event loop is running.
        QCOMPARE(QProcess::execute("xset", {"r", "rate", "150", "30"}), 0);
        QCOMPARE(QProcess::execute("xdotool", {"keydown", "ctrl", "alt", "r"}), 0);
        QTest::qWait(650);
        const int whileHeld = triggered.size();
        QCOMPARE(QProcess::execute("xdotool", {"keyup", "r", "alt", "ctrl"}), 0);
        QTest::qWait(80);
        QCOMPARE(whileHeld, 3);
        QCOMPARE(triggered.size(), 3);
        key("ctrl+alt+r");
        QTRY_COMPARE_WITH_TIMEOUT(triggered.size(), 4, 2000);
        QCOMPARE(triggered, (QStringList{"once", "once", "once", "once"}));
    }
    void modifierTapsDoNotConsumeTypingOrTriggerOnOrdinaryChords() {
        X11Shortcuts shortcuts;
        QStringList triggered, errors;
        shortcuts.bind(
            config("Shift"), [&](QString id) { triggered.append(id); },
            [&](QString error) { errors.append(error); });
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join('\n')));
        QLineEdit fixture;
        fixture.show();
        fixture.activateWindow();
        fixture.setFocus();
        QVERIFY(QTest::qWaitForWindowExposed(&fixture));
        key("Shift_L");
        QTRY_COMPARE_WITH_TIMEOUT(triggered, QStringList{"once"}, 2000);
        key("shift+a");
        QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), QString("A"), 2000);
        QCOMPARE(triggered, QStringList{"once"});
        key("Shift_R");
        QTRY_COMPARE_WITH_TIMEOUT(triggered.size(), 2, 2000);
        QCOMPARE(QProcess::execute("xdotool",
                                   {"keydown", "Shift_L", "sleep", "0.5", "keyup", "Shift_L"}),
                 0);
        QTest::qWait(100);
        QCOMPARE(triggered.size(), 2);
    }
    void modifierOnlyAdapterCoexistsWithAnExistingShortcutService() {
        X11Shortcuts ordinary, modifiers;
        QStringList triggered, errors;
        ordinary.bind(config(), [](QString) {}, [&](QString error) { errors.append(error); });
        modifiers.bind(
            config("Shift"), [&](QString id) { triggered.append(id); },
            [&](QString error) { errors.append(error); }, true);
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join('\n')));
        key("Shift_L");
        QTRY_COMPARE_WITH_TIMEOUT(triggered, QStringList{"once"}, 2000);
    }
    void snapshotsCaptureTheSyntheticScreenWithoutExternalTools() {
        QWidget fixture;
        fixture.setStyleSheet("background: rgb(35, 107, 181)");
        Capture::showOnScreen(&fixture, QGuiApplication::primaryScreen(), true);
        QVERIFY(QTest::qWaitForWindowExposed(&fixture));
        QTest::qWait(80);
        for (const auto &mode : {"auto", "desktop"}) {
            const auto image = Capture::x11Image(mode, fixture.screen());
            QVERIFY(!image.isNull());
            QCOMPARE(image.pixelColor(image.width() / 2, image.height() / 2), QColor(35, 107, 181));
            if (QString(mode) == "auto")
                QCOMPARE(image.text("screen"), fixture.screen()->name());
        }
    }
    void regionSelectionCropsWithoutASeparateScreenshotApplication() {
        QWidget fixture;
        fixture.setStyleSheet("background: rgb(35, 107, 181)");
        Capture::showOnScreen(&fixture, QGuiApplication::primaryScreen(), true);
        QVERIFY(QTest::qWaitForWindowExposed(&fixture));
        QImage result;
        bool finished = false;
        Capture::captureX11(this, "region", fixture.screen(), [&](QImage image) {
            result = image;
            finished = true;
        });
        QWidget *selector = nullptr;
        for (auto *widget : QApplication::topLevelWidgets())
            if (widget->windowTitle() == "Select Snapshot area")
                selector = widget;
        QVERIFY(selector);
        QTest::mousePress(selector, Qt::LeftButton, Qt::NoModifier, QPoint(40, 50));
        QTest::mouseMove(selector, QPoint(140, 100));
        QTest::mouseRelease(selector, Qt::LeftButton, Qt::NoModifier, QPoint(140, 100));
        QTRY_VERIFY(finished);
        const auto scale = fixture.screen()->devicePixelRatio();
        QCOMPARE(result.size(), QSize(qCeil(101 * scale), qCeil(51 * scale)));
        QCOMPARE(result.pixelColor(10, 10), QColor(35, 107, 181));
    }
};

int main(int argc, char **argv) {
    if (qEnvironmentVariable("RANSLATOR_TEST_X11") != "1")
        return 2;
    QApplication app(argc, argv);
    X11Test test;
    return QTest::qExec(&test, argc, argv);
}
#include "x11.moc"
