#pragma once
#include <QObject>
#include <QString>
#include <memory>
#ifdef Q_OS_LINUX
class QDBusMessage;
#endif

// Selected text from accessible applications: Linux AT-SPI or macOS AX.
// Listening is scoped to a user-armed selection session, never permanently on.
class AccessibilitySelection : public QObject {
    Q_OBJECT
  public:
    explicit AccessibilitySelection(QObject *parent = nullptr, QString busAddress = {});
    ~AccessibilitySelection() override;
    void setArmed(bool armed, quint64 generation = 0);
    bool isArmed() const;
  signals:
    void selected(const QString &text, quint64 generation);
    void availability(bool available, const QString &detail, quint64 generation);

  private:
    class Impl;
    std::unique_ptr<Impl> impl;
#ifdef Q_OS_LINUX
  private slots:
    void selectionChanged(const QDBusMessage &message);
    void busDisconnected();
#endif
};
