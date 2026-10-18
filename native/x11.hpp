#pragma once
#include <QJsonObject>
#include <QObject>
#include <functional>
#include <memory>

// Uses a private X connection. It never grabs input when running through XWayland.
class X11Shortcuts : public QObject {
  public:
    explicit X11Shortcuts(QObject *parent = nullptr);
    ~X11Shortcuts() override;
    bool available() const;
    // A passive shortcut grab can deactivate Qt without moving X11 input focus.
    static bool retainsInputFocus(quintptr window);
    void bind(const QJsonObject &config, std::function<void(QString)> triggered,
              std::function<void(QString)> error, bool modifiersOnly = false);

  private:
    struct State;
    std::unique_ptr<State> state;
};
