#pragma once
#include <QCoreApplication>
#include <QStringList>

class QTranslator;

// One stable context even when controls move between widget classes/files.
// lupdate understands this declaration without aliases or a custom extractor.
class UiText {
    Q_DECLARE_TR_FUNCTIONS(Ranslator)
};

namespace I18n {
struct Result {
    bool loaded = false;
    QString locale;
    QString file;
    QString warning;
};

// Testable loader. Directories must be absolute paths or Qt resource paths.
// An empty language uses OS UI preferences; "en"/"C" forces source English.
Result load(QTranslator &translator, const QString &language, const QStringList &directories);

// Call after application metadata is set, before creating any widgets.
// The installed translator is owned by the application for its whole lifetime.
Result install(QCoreApplication &application);
} // namespace I18n
