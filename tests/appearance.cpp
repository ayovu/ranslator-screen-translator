#include "appearance_linux.hpp"
#include <QDBusVirtualObject>
#include <QtDBus>
#include <QtTest>
#include <limits>
#include <memory>

namespace {
constexpr auto service = "org.freedesktop.portal.Desktop";
constexpr auto path = "/org/freedesktop/portal/desktop";
constexpr auto interface = "org.freedesktop.portal.Settings";
constexpr auto group = "org.freedesktop.appearance";
using Settings = QMap<QString, QVariantMap>;
struct Rgb {
    double red, green, blue;
};
QDBusArgument &operator<<(QDBusArgument &argument, const Rgb &rgb) {
    argument.beginStructure();
    argument << rgb.red << rgb.green << rgb.blue;
    argument.endStructure();
    return argument;
}
const QDBusArgument &operator>>(const QDBusArgument &argument, Rgb &rgb) {
    argument.beginStructure();
    argument >> rgb.red >> rgb.green >> rgb.blue;
    argument.endStructure();
    return argument;
}
} // namespace
Q_DECLARE_METATYPE(Rgb)

class FakeSettings final : public QDBusVirtualObject {
    Q_OBJECT

  public:
    QVariantMap values;
    int reads = 0, firstDelay = 0;
    QStringList namespaces;
    QString introspect(const QString &) const override { return {}; }
    bool handleMessage(const QDBusMessage &message, const QDBusConnection &bus) override {
        if (message.member() != "ReadAll")
            return false;
        namespaces = qdbus_cast<QStringList>(message.arguments().first());
        const auto reply = message.createReply({QVariant::fromValue(Settings{{group, values}})});
        const auto delay = ++reads == 1 ? firstDelay : 0;
        QTimer::singleShot(delay, this, [bus, reply] { bus.send(reply); });
        return true;
    }
    void change(const QString &key, const QVariant &value) {
        values.insert(key, value);
        auto signal = QDBusMessage::createSignal(path, interface, "SettingChanged");
        signal << QString(group) << key << QVariant::fromValue(QDBusVariant(value));
        QDBusConnection::sessionBus().send(signal);
    }
};

class AppearanceTests final : public QObject {
    Q_OBJECT
    std::unique_ptr<FakeSettings> fake;

  private slots:
    void initTestCase() {
        QVERIFY2(qEnvironmentVariableIntValue("RANSLATOR_TEST_BUS") == 1,
                 "Run through ctest on the private test bus.");
        qDBusRegisterMetaType<Settings>();
        qDBusRegisterMetaType<Rgb>();
    }
    void init() {
        fake = std::make_unique<FakeSettings>();
        fake->values = {{"color-scheme", uint(1)},
                        {"accent-color", QVariant::fromValue(Rgb{.2, .4, .8})},
                        {"reduced-motion", uint(1)},
                        {"contrast", uint(1)}};
        auto bus = QDBusConnection::sessionBus();
        QVERIFY(bus.registerService(service));
        QVERIFY(bus.registerVirtualObject(path, fake.get()));
    }
    void cleanup() {
        auto bus = QDBusConnection::sessionBus();
        bus.unregisterObject(path);
        bus.unregisterService(service);
        fake.reset();
    }
    void reads_only_appearance_preferences_and_decodes_the_rgb_tuple() {
        NativeLinux::AppearanceSettings settings;
        QTRY_VERIFY(settings.appearance().dark.has_value());
        QCOMPARE(fake->namespaces, QStringList{group});
        QVERIFY(*settings.appearance().dark);
        QCOMPARE(settings.appearance().accent, QColor::fromRgbF(.2, .4, .8));
        QVERIFY(settings.appearance().reducedMotion);
        QVERIFY(settings.appearance().highContrast);
    }
    void changes_update_color_motion_and_contrast_without_restarting() {
        NativeLinux::AppearanceSettings settings;
        QTRY_VERIFY(settings.appearance().accent.isValid());
        fake->change("accent-color", QVariant::fromValue(Rgb{1, .9, .1}));
        fake->change("color-scheme", uint(2));
        fake->change("reduced-motion", uint(0));
        fake->change("contrast", uint(0));
        QTRY_COMPARE(settings.appearance().accent, QColor::fromRgbF(1, .9, .1));
        QTRY_COMPARE(settings.appearance().dark, std::optional<bool>(false));
        QTRY_VERIFY(!settings.appearance().reducedMotion && !settings.appearance().highContrast);
    }
    void invalid_values_restore_application_fallbacks() {
        NativeLinux::AppearanceSettings settings;
        QTRY_VERIFY(settings.appearance().accent.isValid());
        for (const auto invalid : {1.5, -1.0, std::numeric_limits<double>::quiet_NaN()}) {
            fake->change("accent-color", QVariant::fromValue(Rgb{invalid, .5, .5}));
            QTRY_VERIFY(!settings.appearance().accent.isValid());
            fake->change("accent-color", QVariant::fromValue(Rgb{.2, .4, .8}));
            QTRY_VERIFY(settings.appearance().accent.isValid());
        }
        fake->change("accent-color", QString("not an RGB tuple"));
        fake->change("color-scheme", uint(99));
        fake->change("reduced-motion", uint(99));
        fake->change("contrast", QString("1"));
        QTRY_VERIFY(!settings.appearance().accent.isValid());
        QTRY_VERIFY(!settings.appearance().dark.has_value());
        QTRY_VERIFY(!settings.appearance().reducedMotion && !settings.appearance().highContrast);
    }
    void a_delayed_initial_reply_cannot_undo_a_new_setting() {
        fake->firstDelay = 160;
        NativeLinux::AppearanceSettings settings;
        QTRY_COMPARE(fake->reads, 1);
        fake->change("accent-color", QVariant::fromValue(Rgb{.8, .2, .1}));
        QTRY_COMPARE(settings.appearance().accent, QColor::fromRgbF(.8, .2, .1));
        QTest::qWait(180);
        QCOMPARE(settings.appearance().accent, QColor::fromRgbF(.8, .2, .1));
    }
    void a_restarted_portal_restores_preferences() {
        NativeLinux::AppearanceSettings settings;
        QTRY_VERIFY(settings.appearance().accent.isValid());
        auto bus = QDBusConnection::sessionBus();
        QVERIFY(bus.unregisterService(service));
        QTRY_VERIFY(!settings.appearance().accent.isValid());
        QVERIFY(!settings.appearance().dark.has_value());
        QVERIFY(!settings.appearance().reducedMotion && !settings.appearance().highContrast);
        QVERIFY(bus.registerService(service));
        QTRY_VERIFY(settings.appearance().accent.isValid());
    }
    void an_absent_portal_does_not_block_or_require_preferences() {
        QVERIFY(QDBusConnection::sessionBus().unregisterService(service));
        NativeLinux::AppearanceSettings settings;
        QSignalSpy updates(&settings, &NativeLinux::AppearanceSettings::changed);
        QTest::qWait(100);
        QVERIFY(!settings.appearance().dark.has_value());
        QVERIFY(!settings.appearance().accent.isValid());
        QVERIFY(!settings.appearance().reducedMotion && !settings.appearance().highContrast);
        QCOMPARE(updates.size(), 0);
    }
};
QTEST_GUILESS_MAIN(AppearanceTests)
#include "appearance.moc"
