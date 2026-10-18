#include "capture.hpp"
#include "i18n.hpp"
#ifdef Q_OS_MACOS
#include "macos.hpp"
#endif
#ifdef Q_OS_LINUX
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#endif

namespace Capture {
QScreen *screenNamed(const QString &name) {
    for (auto *screen : QGuiApplication::screens())
        if (screen->name() == name)
            return screen;
    return nullptr;
}
QScreen *fallbackScreen() {
    auto *screen = QGuiApplication::screenAt(QCursor::pos());
    return screen ? screen : QGuiApplication::primaryScreen();
}
void invocationScreen(QObject *owner, std::function<void(QScreen *)> done) {
    QPointer<QScreen> fallback = fallbackScreen();
#ifdef Q_OS_LINUX
    if (!QGuiApplication::platformName().startsWith("wayland") ||
        !qEnvironmentVariable("XDG_CURRENT_DESKTOP").contains("KDE")) {
        done(fallback);
        return;
    }
    // This is the same output KWin's CaptureActiveScreen uses. The method is read-only.
    auto message =
        QDBusMessage::createMethodCall("org.kde.KWin", "/KWin", "org.kde.KWin", "activeOutputName");
    auto *watcher =
        new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message, 250), owner);
    QObject::connect(
        watcher, &QDBusPendingCallWatcher::finished, owner,
        [done = std::move(done), fallback](QDBusPendingCallWatcher *call) {
            QDBusPendingReply<QString> reply = *call;
            auto *screen = reply.isError() ? nullptr : screenNamed(reply.value());
            done(screen ? screen : fallback ? fallback.data() : QGuiApplication::primaryScreen());
            call->deleteLater();
        });
#else
    Q_UNUSED(owner)
    done(fallback);
#endif
}
QScreen *imageScreen(const QImage &image, QScreen *invocation, bool useMetadata) {
    // Spectacle preserves the captured output name in PNG metadata. Prefer the actual image
    // if the pointer changed outputs while the external screenshot process was starting.
    if (useMetadata)
        if (auto *screen = screenNamed(image.text("screen")))
            return screen;
    if (QGuiApplication::screens().contains(invocation))
        return invocation;
    return QGuiApplication::primaryScreen();
}
void showOnScreen(QWidget *widget, QScreen *screen, bool fullscreen) {
    if (!screen || !QGuiApplication::screens().contains(screen))
        screen = QGuiApplication::primaryScreen();
    // setGeometry alone cannot select a Wayland output. Bind the native window before mapping.
    widget->setScreen(screen);
    widget->winId();
    widget->windowHandle()->setScreen(screen);
#ifdef Q_OS_MACOS
    NativeMac::prepareOverlay(widget);
    if (fullscreen)
        widget->setGeometry(screen->geometry());
    // AppKit fullscreen creates a separate Space. Snapshot must stay above the current one.
    widget->show();
    widget->raise();
#else
    if (fullscreen) {
        widget->setGeometry(screen->geometry());
        widget->showFullScreen();
    } else {
        widget->show();
    }
#endif
}
bool directCaptureAvailable() {
    return qEnvironmentVariable("XDG_CURRENT_DESKTOP").contains("KDE") &&
           (QFile::exists("/.flatpak-info")
                ? !QStandardPaths::findExecutable("flatpak-spawn").isEmpty()
                : !QStandardPaths::findExecutable("spectacle").isEmpty());
}
QStringList spectacleArguments(const QString &mode, const QString &path) {
    return {"--background",
            "--nonotify",
            "--delay",
            "0",
            mode == "desktop"  ? "--fullscreen"
            : mode == "region" ? "--region"
                               : "--current",
            "--output",
            path};
}
Command spectacleCommand(const QString &mode, const QString &path) {
    Command command;
    command.environment = QProcessEnvironment::systemEnvironment();
    for (const auto &key : {"LD_LIBRARY_PATH", "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH",
                            "QML2_IMPORT_PATH", "QT_QPA_PLATFORMTHEME"})
        command.environment.remove(key);
    if (QFile::exists("/.flatpak-info")) {
        command.program = QStandardPaths::findExecutable("flatpak-spawn");
        command.arguments = {"--host", "--watch-bus", "spectacle"};
    } else {
        command.program = QStandardPaths::findExecutable("spectacle");
    }
    command.arguments.append(spectacleArguments(mode, path));
    return command;
}
QString temporaryDirectoryTemplate() {
    if (!QFile::exists("/.flatpak-info"))
        return QDir::tempPath() + "/ranslator-snapshot-XXXXXX";
    const auto cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (cache.isEmpty() || !QDir().mkpath(cache))
        return {};
    return cache + "/snapshot-XXXXXX";
}
bool x11Available() {
    return QGuiApplication::platformName() == "xcb" &&
           qEnvironmentVariable("XDG_SESSION_TYPE") != "wayland";
}
QImage x11Image(const QString &mode, QScreen *screen) {
    if (!x11Available())
        return {};
    if (!QGuiApplication::screens().contains(screen))
        screen = fallbackScreen();
    if (!screen)
        return {};
    if (mode != "desktop" && mode != "region") {
        auto image = screen->grabWindow(0).toImage();
        image.setText("screen", screen->name());
        return image;
    }
    QRect bounds;
    qreal scale = 1;
    const auto screens = QGuiApplication::screens();
    for (auto *output : screens) {
        bounds |= output->geometry();
        scale = qMax(scale, output->devicePixelRatio());
    }
    // Keep the sharpest display's resolution when composing mixed-DPI outputs.
    const auto size = QSize(qCeil(bounds.width() * scale), qCeil(bounds.height() * scale));
    if (qint64(size.width()) * size.height() > 128 * 1024 * 1024)
        return {};
    QImage image(size, QImage::Format_RGB32);
    image.fill(Qt::black);
    QPainter painter(&image);
    for (auto *output : screens) {
        const auto part = output->grabWindow(0);
        if (part.isNull())
            return {};
        const QRectF target((output->geometry().topLeft() - bounds.topLeft()) * scale,
                            QSizeF(output->size()) * scale);
        painter.drawImage(target, part.toImage());
    }
    painter.end();
    image.setDevicePixelRatio(scale);
    return image;
}
class RegionSelector final : public QWidget {
    QImage image;
    QPoint start;
    QRect selection;
    bool dragging = false;
    std::function<void(QImage)> done;
    void finish(QImage selected) {
        hide();
        auto callback = std::move(done);
        deleteLater();
        if (callback)
            callback(std::move(selected));
    }

  public:
    RegionSelector(QImage source, QScreen *screen, std::function<void(QImage)> callback)
        : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint),
          image(std::move(source)), done(std::move(callback)) {
        setAttribute(Qt::WA_DeleteOnClose);
        setWindowTitle(UiText::tr("Select Snapshot area"));
        setCursor(Qt::CrossCursor);
        if (screen && QGuiApplication::screens().contains(screen)) {
            showOnScreen(this, screen, true);
        } else {
            QRect bounds;
            for (auto *output : QGuiApplication::screens())
                bounds |= output->geometry();
            setGeometry(bounds);
#ifdef Q_OS_MACOS
            winId();
            NativeMac::prepareOverlay(this);
#endif
            show();
        }
        raise();
        activateWindow();
        grabKeyboard();
        grabMouse();
    }
    ~RegionSelector() override {
        if (done)
            done({});
    }

  protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.drawImage(rect(), image);
        painter.fillRect(rect(), QColor(0, 0, 0, 105));
        if (!selection.isEmpty()) {
            painter.save();
            painter.setClipRect(selection);
            painter.drawImage(rect(), image);
            painter.restore();
            painter.setPen(QPen(QColor(150, 208, 255), 2));
            painter.drawRect(selection.adjusted(1, 1, -1, -1));
        }
    }
    void keyPressEvent(QKeyEvent *event) override {
        if (event->key() == Qt::Key_Escape)
            finish({});
    }
    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() == Qt::RightButton) {
            finish({});
            return;
        }
        if (event->button() != Qt::LeftButton)
            return;
        start = event->position().toPoint();
        dragging = true;
    }
    void mouseMoveEvent(QMouseEvent *event) override {
        if (!dragging)
            return;
        selection = QRect(start, event->position().toPoint()).normalized().intersected(rect());
        update();
    }
    void mouseReleaseEvent(QMouseEvent *event) override {
        if (event->button() != Qt::LeftButton || !dragging)
            return;
        selection = QRect(start, event->position().toPoint()).normalized().intersected(rect());
        if (selection.width() < 3 || selection.height() < 3) {
            finish({});
            return;
        }
        const auto scaleX = qreal(image.width()) / width();
        const auto scaleY = qreal(image.height()) / height();
        auto selected = image.copy(
            QRect(qFloor(selection.x() * scaleX), qFloor(selection.y() * scaleY),
                  qCeil(selection.width() * scaleX), qCeil(selection.height() * scaleY)));
        if (auto *screen = QGuiApplication::screenAt(mapToGlobal(selection.center())))
            selected.setText("screen", screen->name());
        finish(std::move(selected));
    }
};
QWidget *captureX11(QObject *owner, const QString &mode, QScreen *screen,
                    std::function<void(QImage)> done) {
    auto image = x11Image(mode, screen);
    if (mode != "region" || image.isNull()) {
        done(std::move(image));
        return nullptr;
    }
    return selectRegion(owner, std::move(image), nullptr, std::move(done));
}
QWidget *selectRegion(QObject *owner, QImage image, QScreen *screen,
                      std::function<void(QImage)> done) {
    if (image.isNull()) {
        done({});
        return nullptr;
    }
    const QPointer<QObject> guard(owner);
    auto *selector =
        new RegionSelector(std::move(image), screen, [guard, done = std::move(done)](QImage image) {
            if (guard)
                done(std::move(image));
        });
    QObject::connect(owner, &QObject::destroyed, selector, &QWidget::close);
    return selector;
}
} // namespace Capture
