#pragma once
#include "autostart.hpp"
#include <QtWidgets>
#include <functional>
#include <memory>
#ifdef __OBJC__
@class NSPasteboard;
#endif

class MacShortcuts : public QObject {
  public:
    explicit MacShortcuts(QObject *parent = nullptr);
    ~MacShortcuts() override;
    void bind(const QJsonObject &, std::function<void(QString)> triggered,
              std::function<void(QString)> error);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

namespace NativeMac {
// Each request is one frame. No background screen recording or persistent capture stream.
void capture(QObject *owner, const QString &mode, QScreen *screen,
             std::function<void(QImage, QString)> done);
// System material behind the window's rounded body: Liquid Glass on macOS 26 and later, a
// vibrant NSVisualEffectView before that. The window shadow follows the body's shape.
bool applyBackdrop(QWidget *widget, bool enabled, bool dark, qreal inset, qreal radius);
// The user's accent color, and the Reduce motion / Reduce transparency accessibility settings.
QColor accentColor(bool dark);
bool reduceMotion();
bool reduceTransparency();
// An SF Symbol rendered in one color; null when the symbol is unavailable.
QImage symbol(const QString &name, qreal pointSize, const QColor &color, qreal scale);
void prepareOverlay(QWidget *widget);
AutostartState autostartState();
void setAutostart(QObject *owner, bool enabled, std::function<void(AutostartState, QString)> done);
void openLoginItemsSettings();
// Apple Vision OCR in Tesseract's TSV layout, so the shared OCR parser reads both engines.
// `languages` holds the configured Tesseract codes; they are recognition hints.
QString recognizeText(const QString &imagePath, const QString &languages, QString *error);
// Vision compiles its recognizer on first use per app and macOS build; this pays that cost early.
bool warmUpTextRecognition(const QString &languages);
// Translate selection: the focused element's AX selection, else ⌘C in the frontmost app with the
// pasteboard restored afterwards. `done` runs on the UI thread with the text or an error; both are
// empty when nothing is selected.
void readSelection(QObject *owner, std::function<void(QString text, QString error)> done);
#ifdef __OBJC__
// Snapshot of every pasteboard item and representation; the returned function restores it.
std::function<void()> preservePasteboard(NSPasteboard *board);
#endif
} // namespace NativeMac
