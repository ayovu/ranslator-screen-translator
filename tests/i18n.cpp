#include "i18n.hpp"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QTranslator>

class I18nTests : public QObject {
    Q_OBJECT
  private slots:
    void locale_fallback_and_unfinished_messages_keep_source_english() {
        QTranslator translator;
        const auto loaded = I18n::load(translator, "fr_CA", {I18N_FIXTURE_DIR});
        QVERIFY2(loaded.loaded, qPrintable(loaded.warning));
        QCOMPARE(loaded.locale, "fr");
        QVERIFY(QCoreApplication::installTranslator(&translator));
        QCOMPARE(UiText::tr("Settings"), QString::fromUtf8("Paramètres"));
        QCOMPARE(UiText::tr("Hello %1").arg("Synthetic"), "Bonjour Synthetic");
        QCOMPARE(UiText::tr("%n item(s)", nullptr, 1), QString::fromUtf8("1 élément"));
        QCOMPARE(UiText::tr("%n item(s)", nullptr, 3), QString::fromUtf8("3 éléments"));
        QCOMPARE(UiText::tr("Unfinished label"), "Unfinished label");
        QCOMPARE(UiText::tr("No catalog entry"), "No catalog entry");
        QCoreApplication::removeTranslator(&translator);
    }
    void english_override_clears_reused_translator() {
        QTranslator translator;
        QVERIFY(I18n::load(translator, "fr", {I18N_FIXTURE_DIR}).loaded);
        QVERIFY(!I18n::load(translator, "en", {I18N_FIXTURE_DIR}).loaded);
        QVERIFY(translator.isEmpty());
        QVERIFY(!I18n::load(translator, "C", {I18N_FIXTURE_DIR}).loaded);
        QVERIFY(!I18n::load(translator, "POSIX", {I18N_FIXTURE_DIR}).loaded);
    }
    void locale_or_directory_cannot_select_relative_files() {
        QTranslator translator;
        const auto badLocale = I18n::load(translator, "../../fr", {I18N_FIXTURE_DIR});
        QVERIFY(!badLocale.loaded);
        QVERIFY(!badLocale.warning.isEmpty());
        const auto badDirectory = I18n::load(translator, "fr", {"tests/fixtures/i18n"});
        QVERIFY(!badDirectory.loaded);
        QVERIFY(!badDirectory.warning.isEmpty());
    }
    void invalid_catalog_falls_back_to_next_trusted_directory() {
        QTemporaryDir directory;
        QFile invalid(directory.filePath("ranslator_fr.qm"));
        QVERIFY(invalid.open(QIODevice::WriteOnly));
        invalid.write("not a Qt catalog");
        invalid.close();
        QTranslator translator;
        const auto result = I18n::load(translator, "fr", {directory.path(), I18N_FIXTURE_DIR});
        QVERIFY(result.loaded);
        QVERIFY(!result.warning.isEmpty());
        QVERIFY(result.file.startsWith(I18N_FIXTURE_DIR));
    }
    void missing_catalog_does_not_change_source_text() {
        QTemporaryDir directory;
        QTranslator translator;
        const auto result = I18n::load(translator, "ja", {directory.path()});
        QVERIFY(!result.loaded);
        QVERIFY(result.warning.isEmpty());
        QCOMPARE(UiText::tr("Settings"), "Settings");
    }
    void script_fallback_does_not_invent_another_scripts_territory() {
        QTemporaryDir directory;
        const QString fixture = QString(I18N_FIXTURE_DIR) + "/ranslator_fr.qm";
        for (const auto *name : {"zh_Hans_CN", "zh_CN", "zh_Hans"})
            QVERIFY(QFile::copy(fixture, directory.filePath(QString("ranslator_%1.qm").arg(name))));
        QTranslator translator;
        // A Traditional Chinese preference must not invent Simplified Chinese
        // candidates by expanding its generic "zh" fallback again.
        QVERIFY(!I18n::load(translator, "zh_Hant_HK", {directory.path()}).loaded);
    }
};
QTEST_GUILESS_MAIN(I18nTests)
#include "i18n.moc"
