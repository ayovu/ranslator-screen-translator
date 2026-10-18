#pragma once
#include <QtWidgets>
#include <functional>

// Optional, explicitly enabled GNOME Shell companion. The normal portal path remains available.
class GnomeDesktop : public QObject {
    Q_OBJECT
  public:
    explicit GnomeDesktop(QObject *parent = nullptr, bool enabled = true);
    ~GnomeDesktop() override;
    bool available() const;
    void bind(const QJsonObject &shortcuts, std::function<void(QString, QJsonObject)> triggered,
              std::function<void(QString)> error);
    void context(std::function<void(QJsonObject, QString)> done);
    void capture(const QString &mode, int monitorIndex,
                 std::function<void(QImage, QJsonObject, QString)> done);
    void cancelCapture();
    void armSelection(bool active, bool watchClipboard, std::function<void(QString)> selected,
                      std::function<void(QString)> error = {});
    static QScreen *screenForContext(const QJsonObject &context);

  signals:
    void availabilityChanged(bool available);

  private:
    bool ready = false, ownsClientName = false, selectionArmed = false;
    quint64 bindingGeneration = 0, captureGeneration = 0;
    uint selectionGeneration = 0;
    std::function<void(QString, QJsonObject)> trigger;
    std::function<void(QString)> selected, selectionError;
    std::function<void()> captureDisconnected;
  private slots:
    void serviceChanged(const QString &, const QString &, const QString &);
    void shortcutActivated(const QString &, const QString &);
    void selectionChanged(uint generation, const QString &text);
};
