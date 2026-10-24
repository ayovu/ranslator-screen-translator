#pragma once
#include "autostart.hpp"
#include <QtWidgets>
#include <functional>
#include <memory>

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
bool applyBackdrop(QWidget *widget, bool enabled, bool dark);
void prepareOverlay(QWidget *widget);
AutostartState autostartState();
void setAutostart(QObject *owner, bool enabled, std::function<void(AutostartState, QString)> done);
void openLoginItemsSettings();
// Apple Vision OCR in Tesseract's TSV layout, so the shared OCR parser reads both engines.
// `languages` holds the configured Tesseract codes; they are recognition hints.
QString recognizeText(const QString &imagePath, const QString &languages, QString *error);
// Vision compiles its recognizer on first use per app and macOS build; this pays that cost early.
bool warmUpTextRecognition(const QString &languages);
} // namespace NativeMac
