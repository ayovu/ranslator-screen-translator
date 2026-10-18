#include "i18n.hpp"
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTranslator>

#ifdef RANSLATOR_BUNDLED_TRANSLATIONS
static void initializeTranslations() { Q_INIT_RESOURCE(ranslator_translations); }
#endif

namespace {
QStringList languageNames(const QString &language, QString *warning) {
    const auto requested = language.trimmed();
    if (requested == "C" || requested == "POSIX")
        return {};
    const bool system = requested.isEmpty() || requested == "system";
    static const QRegularExpression tag("^[A-Za-z]{2,3}(?:[-_][A-Za-z0-9]{2,8}){0,2}$");
    if (!system &&
        (!tag.match(requested).hasMatch() || QLocale(requested).language() == QLocale::C)) {
        *warning = "Invalid RANSLATOR_UI_LANGUAGE; using English. Use an installed locale code, "
                   "for example fr or pt_BR, or system.";
        return {};
    }
    const auto preferences =
        system ? QLocale::system().uiLanguages() : QLocale(requested).uiLanguages();
    QStringList result;
    for (const auto &preference : preferences) {
        const QLocale locale(preference);
        if (locale.language() == QLocale::English || locale.language() == QLocale::C)
            break;
        // Qt already expands UI preferences with script/territory fallback.
        // Expanding "zh" again would introduce Hans into a Hant preference list.
        auto name = preference;
        name.replace('-', '_');
        if (tag.match(name).hasMatch() && !result.contains(name))
            result.append(name);
    }
    return result;
}
} // namespace

I18n::Result I18n::load(QTranslator &translator, const QString &language,
                        const QStringList &directories) {
    Result result;
    // Reset a reused translator without ever looking for a file in the cwd.
    (void)translator.load(reinterpret_cast<const uchar *>(""), 0);
    const auto names = languageNames(language, &result.warning);
    for (const auto &directory : directories) {
        if (!QDir::isAbsolutePath(directory) && !directory.startsWith(":/")) {
            result.warning = "Ignoring a relative translation directory; use an absolute path.";
            continue;
        }
        for (const auto &name : names) {
            const auto path = QDir(directory).filePath("ranslator_" + name + ".qm");
            const QFileInfo file(path);
            if (!file.exists())
                continue;
            // Catalogs are trusted data. Bound accidental corrupt/oversized input,
            // and never discover files through the current working directory.
            if (!file.isFile() || !file.isReadable() || file.size() <= 0 ||
                file.size() > 8 * 1024 * 1024 || !translator.load(path)) {
                result.warning = "A UI translation catalog could not be loaded; using available "
                                 "translations or English. Recompile it with Qt 6 lrelease.";
                continue;
            }
            result.loaded = true;
            result.locale = name;
            result.file = path;
            return result;
        }
    }
    return result;
}

I18n::Result I18n::install(QCoreApplication &application) {
#ifdef RANSLATOR_BUNDLED_TRANSLATIONS
    initializeTranslations();
#endif
    QStringList directories;
    QString warning;
    const auto external = qEnvironmentVariable("RANSLATOR_TRANSLATIONS_DIR");
    if (!external.isEmpty()) {
        if (QDir::isAbsolutePath(external) && !external.startsWith(":/"))
            directories.append(external);
        else
            warning = "RANSLATOR_TRANSLATIONS_DIR must be an absolute filesystem path; ignored.";
    }
    directories.append(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
                       "/ranslator/translations");
    directories.append(":/i18n");
    const auto executable = QCoreApplication::applicationDirPath();
    directories.append(QDir::cleanPath(executable + "/../share/ranslator/translations"));
#ifdef Q_OS_MACOS
    directories.append(QDir::cleanPath(executable + "/../Resources/translations"));
#endif
    auto *translator = new QTranslator(&application);
    auto result = load(*translator, qEnvironmentVariable("RANSLATOR_UI_LANGUAGE"), directories);
    if (!warning.isEmpty())
        result.warning = warning + (result.warning.isEmpty() ? QString() : " " + result.warning);
    if (result.loaded)
        application.installTranslator(translator);
    else
        delete translator;
    if (!result.warning.isEmpty())
        qWarning().noquote() << result.warning;
    return result;
}
