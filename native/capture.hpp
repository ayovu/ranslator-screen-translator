#pragma once
#include <QtWidgets>
#include <functional>

namespace Capture {
QScreen *screenNamed(const QString &name);
QScreen *fallbackScreen();
// Resolve before hiding any windows. QCursor::pos() is not a global cursor query on Wayland.
void invocationScreen(QObject *owner, std::function<void(QScreen *)> done);
QScreen *imageScreen(const QImage &image, QScreen *invocation, bool useMetadata);
void showOnScreen(QWidget *widget, QScreen *screen, bool fullscreen);
bool directCaptureAvailable();
QStringList spectacleArguments(const QString &mode, const QString &path);
struct Command {
    QString program;
    QStringList arguments;
    QProcessEnvironment environment;
};
Command spectacleCommand(const QString &mode, const QString &path);
// Host processes cannot see a Flatpak's private /tmp. The app cache is shared with its host.
QString temporaryDirectoryTemplate();
bool x11Available();
QImage x11Image(const QString &mode, QScreen *screen);
// Region mode uses an in-process frozen-screen selector; cancelling returns an empty image.
QWidget *captureX11(QObject *owner, const QString &mode, QScreen *screen,
                    std::function<void(QImage)> done);
QWidget *selectRegion(QObject *owner, QImage image, QScreen *screen,
                      std::function<void(QImage)> done);
} // namespace Capture
