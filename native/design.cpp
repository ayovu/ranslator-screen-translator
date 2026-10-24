#include "design.hpp"
#ifdef Q_OS_LINUX
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusVariant>
#endif
#ifdef Q_OS_MACOS
#include "macos.hpp"
#endif
#include <cmath>
#include <cstdio>
#ifdef HAVE_KWINDOWSYSTEM
#include <KWindowEffects>
#endif

static void initializeResources() { Q_INIT_RESOURCE(resources); }
namespace {
// Opt-in rendering diagnostics for compositor-specific regressions. Never record
// widget text, images, configuration, input events, or provider responses.
class RenderTrace : public QObject {
    int remaining = 800;
    void record(QWidget *widget, const char *event) {
        if (remaining-- <= 0)
            return;
        const auto palette = widget->palette();
        const auto *handle = widget->window()->windowHandle();
        QString line;
        QDebug(&line).noquote() << "ranslator-render" << event << widget->metaObject()->className()
                                << widget->objectName() << widget->geometry() << "visible"
                                << widget->isVisible() << "fill" << widget->autoFillBackground()
                                << "styled" << widget->testAttribute(Qt::WA_StyledBackground)
                                << "opaque" << widget->testAttribute(Qt::WA_OpaquePaintEvent)
                                << "no-system" << widget->testAttribute(Qt::WA_NoSystemBackground)
                                << "tinted" << widget->testAttribute(Qt::WA_TintedBackground)
                                << "native" << widget->testAttribute(Qt::WA_NativeWindow)
                                << "window-color"
                                << palette.color(QPalette::Window).name(QColor::HexArgb)
                                << "base-color"
                                << palette.color(QPalette::Base).name(QColor::HexArgb) << "effect"
                                << (widget->graphicsEffect()
                                        ? widget->graphicsEffect()->metaObject()->className()
                                        : "none")
                                << "dpr" << widget->devicePixelRatioF() << "alpha-buffer"
                                << (handle ? handle->format().alphaBufferSize() : -1);
        const auto bytes = line.toUtf8();
        std::fwrite(bytes.constData(), 1, bytes.size(), stderr);
        std::fputc('\n', stderr);
    }
    void tree(QWidget *widget, const char *event) {
        record(widget, event);
        for (auto *child : widget->findChildren<QWidget *>())
            if (child->window() == widget)
                record(child, event);
    }
    bool eventFilter(QObject *object, QEvent *event) override {
        auto *widget = qobject_cast<QWidget *>(object);
        if (remaining <= 0 || !widget || !widget->window()->property("glassWindow").toBool())
            return false;
        if (widget->isWindow() && event->type() == QEvent::Show) {
            QTimer::singleShot(250, widget, [this, widget] {
                if (widget->isVisible())
                    tree(widget, "shown");
            });
        } else if (widget->isWindow() && event->type() == QEvent::Hide) {
            record(widget, "hidden");
        } else if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) {
            record(widget, event->type() == QEvent::PaletteChange ? "palette" : "style");
        }
        return false;
    }

  public:
    explicit RenderTrace(QObject *parent) : QObject(parent) {
        std::fprintf(stderr, "ranslator-render enabled Qt=%s platform=%s\n", qVersion(),
                     qPrintable(QGuiApplication::platformName()));
    }
};
} // namespace
namespace Design {
static Theme current;
static bool systemDark = false;
Theme &theme() { return current; }

QFont textFont(QFont font, const QString &language) {
    const auto code = language.toLower().replace('_', '-');
    if (macStyle) {
        // San Francisco, with the Han variant each language expects; Core Text covers the rest.
        const auto han = code == "ja" || code.startsWith("ja-")   ? "Hiragino Sans"
                         : code == "ko" || code.startsWith("ko-") ? "Apple SD Gothic Neo"
                         : code == "zh-tw" || code == "zh-hk" || code.startsWith("zh-hant")
                             ? "PingFang TC"
                             : "PingFang SC";
        font.setFamilies({QFontDatabase::systemFont(QFontDatabase::GeneralFont).family(), han});
        return font;
    }
    const auto cjk = code == "ja" || code.startsWith("ja-")   ? "Noto Sans CJK JP"
                     : code == "ko" || code.startsWith("ko-") ? "Noto Sans CJK KR"
                     : code == "zh-tw" || code == "zh-hk" || code.startsWith("zh-hant")
                         ? "Noto Sans CJK TC"
                         : "Noto Sans CJK SC";
    font.setFamilies({"Inter Variable", "Noto Sans Arabic", "Noto Sans Devanagari",
                      "Noto Sans Hebrew", "Noto Sans Bengali", "Noto Sans Thai", cjk});
    return font;
}

void initialize() {
    initializeResources();
    systemDark = qApp->palette().color(QPalette::Window).lightness() < 128;
    // These controls are drawn by our own material/style sheet. Keep the underlying style
    // deterministic: desktop theme plugins must not add opaque native control backgrounds.
    qApp->setStyle(QStyleFactory::create("Fusion"));
    if (qEnvironmentVariableIntValue("RANSLATOR_RENDER_TRACE") == 1)
        qApp->installEventFilter(new RenderTrace(qApp));
    QFontDatabase::addApplicationFont(":/design/InterVariable.ttf");
    for (const auto &name : QDir(":/design/fonts").entryList({"*.ttf", "*.ttc"}, QDir::Files))
        QFontDatabase::addApplicationFont(":/design/fonts/" + name);
    QFont font = textFont(QFont("Inter Variable"));
    font.setPixelSize(macStyle ? 13 : 14);
    qApp->setFont(font);
#ifdef Q_OS_LINUX
    if (QGuiApplication::platformName() != "offscreen") {
        QDBusInterface settings("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                                "org.freedesktop.portal.Settings");
        settings.setTimeout(500);
        QDBusReply<QDBusVariant> result =
            settings.call("Read", "org.freedesktop.appearance", "color-scheme");
        if (result.isValid()) {
            const auto value = result.value().variant().toUInt();
            if (value == 1 || value == 2)
                systemDark = value == 1;
        }
    }
#endif
}

void configure(const QJsonObject &config) {
    const auto appearance = config["appearance"].toString("system");
    const bool dark = appearance == "dark" || (appearance == "system" && systemDark);
    bool motion = config["reduce_motion"].toBool();
    bool opaque = config["reduce_transparency"].toBool();
#ifdef Q_OS_MACOS
    // The system accessibility settings apply as well as Ranslator's own switches.
    motion = motion || NativeMac::reduceMotion();
    opaque = opaque || NativeMac::reduceTransparency();
    current.accent = NativeMac::accentColor(dark);
#endif
    static bool configured = false;
    if (configured && current.dark == dark && current.reducedMotion == motion &&
        current.reducedTransparency == opaque)
        return;
    current.dark = dark;
    current.reducedMotion = motion;
    current.reducedTransparency = opaque;
    configured = true;
    const QString ink = current.ink().name(), muted = current.secondary().name();
    const QString field = current.dark ? "rgba(255,255,255,10)" : "rgba(255,255,255,110)";
    const QString edge = current.dark ? "rgba(255,255,255,26)" : "rgba(255,255,255,160)";
    const QString popup = current.dark ? "#1d2431" : "#f3f6fb";
    // Do not combine QApplication::setPalette with style sheets. Reapplying an opaque
    // Window/Base palette to existing translucent children can leave rectangular fills.
    QString sheet = QString(R"(
      QWidget { color: %1; background: transparent; font-size: 14px; selection-color: white; }
      QLabel { background: transparent; border: none; }
      QLabel#heading { font-size: 26px; font-weight: 600; letter-spacing: -0.7px; }
      QLabel#brand { font-size: 17px; font-weight: 600; letter-spacing: -0.4px; }
      QLabel#muted, QLabel#status { color: %2; font-size: 12px; }
      QLabel#eyebrow { color: %2; font-size: 10px; font-weight: 600; letter-spacing: 1.5px; }
      QLabel#error { color: #d65c75; font-size: 12px; }
      QLabel#fieldLabel { font-size: 13px; font-weight: 500; }
      QLabel#provider { color: %2; font-size: 11px; }
      QPlainTextEdit { background: transparent; border: none; font-size: 14px; padding: 0; selection-background-color: #487ff2; }
      QPlainTextEdit#quickTranslation { font-size: 18px; }
      QPlainTextEdit#translation { font-size: 23px; }
      QComboBox, QLineEdit, QSpinBox { background: %3; border: 1px solid %4; border-radius: 10px; padding: 9px 11px; selection-background-color: #487ff2; }
      QSpinBox QLineEdit { background: transparent; border: none; padding: 0 0 0 10px; }
      QSpinBox { min-height: 22px; padding: 8px 40px 8px 12px; }
      QSpinBox::up-button, QSpinBox::down-button { subcontrol-origin: border; width: 30px; height: 18px; background: transparent; border: none; border-radius: 6px; margin-right: 5px; }
      QSpinBox::up-button { subcontrol-position: top right; margin-top: 4px; }
      QSpinBox::down-button { subcontrol-position: bottom right; margin-bottom: 4px; }
      QSpinBox::up-button:hover, QSpinBox::down-button:hover { background: rgba(110,145,205,40); }
      QSpinBox::up-arrow { image: url(:/design/chevron-up.svg); width: 10px; height: 6px; }
      QSpinBox::down-arrow { image: url(:/design/chevron.svg); width: 10px; height: 6px; }
      QSlider::groove:horizontal { height: 4px; background: rgba(125,145,175,50); border-radius: 2px; }
      QSlider::sub-page:horizontal { background: #729afa; border-radius: 2px; }
      QSlider::handle:horizontal { width: 18px; margin: -7px 0; background: #f0f5ff; border: 1px solid rgba(125,145,175,80); border-radius: 9px; }
      QComboBox:focus, QLineEdit:focus, QSpinBox:focus { border: 1px solid #729afa; }
      QComboBox::drop-down { border: none; width: 25px; }
      QComboBox::down-arrow { image: url(:/design/chevron.svg); width: 10px; height: 8px; }
      QComboBox QAbstractItemView, QMenu { background: %5; color: %1; border: 1px solid %4; border-radius: 10px; padding: 6px; selection-background-color: #487ff2; outline: 0; }
      QComboBox QAbstractItemView::item { min-height: 24px; padding: 4px 10px; border-radius: 6px; }
      QComboBox QAbstractItemView::item:selected { background: rgba(70,125,235,105); color: %1; }
      QComboBox#language, QComboBox[compactLanguage="true"] { background: transparent; border: none; padding: 7px 18px 7px 6px; font-size: 13px; font-weight: 500; }
      QComboBox#language QLineEdit, QComboBox[compactLanguage="true"] QLineEdit { background: transparent; border: none; padding: 0; }
      QComboBox#language:focus, QComboBox[compactLanguage="true"]:focus { background: %3; border-radius: 14px; }
      QScrollArea, QScrollArea > QWidget > QWidget, QStackedWidget { background: transparent; border: none; }
      QScrollBar:vertical { background: transparent; width: 5px; margin: 4px 0; }
      QScrollBar::handle:vertical { background: rgba(128,144,170,100); border-radius: 2px; min-height: 24px; }
      QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
      QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
      QCheckBox { background: transparent; spacing: 10px; padding: 6px 0; }
      QCheckBox::indicator { width: 18px; height: 18px; border: 1px solid rgba(125,145,175,150); border-radius: 6px; background: %3; }
      QCheckBox::indicator:checked { background: #487ff2; border-color: #487ff2; image: url(:/design/check.svg); }
      QProgressBar { border: none; background: rgba(100,130,190,25); border-radius: 2px; }
      QProgressBar::chunk { background: #6c99ff; border-radius: 2px; }
      QPushButton { border: 1px solid %4; border-radius: 12px; background: %3; padding: 9px 16px; }
      QPushButton:hover { background: rgba(125,155,215,28); }
      QPushButton:default { background: #487ff2; color: white; border: none; }
      QToolTip { background: %5; color: %1; border: 1px solid %4; padding: 7px; }
    )")
                        .arg(ink, muted, field, edge, popup);
    if (macStyle) {
        // Later rules win: macOS type scale, control radii and the user's accent color.
        const QString accent = current.accent.name();
        const QString macField = current.dark ? "rgba(255,255,255,14)" : "rgba(255,255,255,190)";
        const QString macEdge = current.dark ? "rgba(255,255,255,22)" : "rgba(0,0,0,26)";
        const QString macPopup = current.dark ? "#2a2a2e" : "#fbfbfd";
        sheet += QString(R"(
      QWidget { font-size: 13px; selection-background-color: %1; }
      QLabel#heading { font-size: 22px; font-weight: 700; letter-spacing: -0.4px; }
      QLabel#brand { font-size: 15px; font-weight: 600; letter-spacing: -0.2px; }
      QLabel#eyebrow { font-size: 10px; font-weight: 600; letter-spacing: 0.8px; }
      QLabel#fieldLabel { font-size: 13px; font-weight: 500; }
      QPlainTextEdit { font-size: 14px; selection-background-color: %1; }
      QPlainTextEdit#quickTranslation { font-size: 17px; }
      QPlainTextEdit#translation { font-size: 21px; }
      QComboBox, QLineEdit, QSpinBox { background: %2; border: 1px solid %3; border-radius: 8px; padding: 7px 10px; selection-background-color: %1; }
      QSpinBox { padding: 6px 40px 6px 10px; }
      QComboBox:focus, QLineEdit:focus, QSpinBox:focus { border: 1px solid %1; }
      QComboBox QAbstractItemView, QMenu { background: %4; border: 1px solid %3; border-radius: 10px; padding: 5px; selection-background-color: %1; }
      QComboBox QAbstractItemView::item { min-height: 22px; padding: 3px 9px; border-radius: 5px; }
      QComboBox QAbstractItemView::item:selected { background: %1; color: white; }
      QCheckBox::indicator { width: 16px; height: 16px; border: 1px solid %3; border-radius: 4px; background: %2; }
      QCheckBox::indicator:checked { background: %1; border-color: %1; }
      QProgressBar { background: %2; border-radius: 1px; }
      QProgressBar::chunk { background: %1; border-radius: 1px; }
      QSlider::sub-page:horizontal { background: %1; }
      QPushButton:default { background: %1; }
      QToolTip { background: %4; border: 1px solid %3; border-radius: 6px; padding: 5px 7px; }
      QComboBox[compactLanguage="true"] { padding: 6px 14px 6px 3px; }
      QComboBox[compactLanguage="true"]::drop-down { width: 14px; }
    )")
                     .arg(accent, macField, macEdge, macPopup);
    }
    qApp->setStyleSheet(sheet);
    for (auto *widget : qApp->topLevelWidgets()) {
        if (widget->property("glassWindow").toBool())
            applyBackdrop(widget);
        widget->update();
    }
}

static qreal springProgress(qreal t) {
    // Underdamped spring, damping 0.86: settles with a barely visible 1% overshoot.
    constexpr qreal zeta = 0.86, omega = 13;
    const qreal damped = omega * std::sqrt(1 - zeta * zeta);
    const auto at = [&](qreal x) {
        return 1 - std::exp(-zeta * omega * x) *
                       (std::cos(damped * x) + zeta * omega / damped * std::sin(damped * x));
    };
    return at(t) / at(1);
}
QEasingCurve spring() {
    QEasingCurve curve;
    curve.setCustomType(springProgress);
    return curve;
}

static QPainterPath rounded(QRectF rect, double radius) {
    QPainterPath path;
    path.addRoundedRect(rect, radius, radius);
    return path;
}

void applyBackdrop(QWidget *widget) {
    if (!widget->windowHandle())
        return;
    bool available = false;
#ifdef Q_OS_MACOS
    available = NativeMac::applyBackdrop(widget, !current.reducedTransparency, current.dark,
                                         windowInset, windowRadius);
#endif
#ifdef HAVE_KWINDOWSYSTEM
    available = QGuiApplication::platformName() != "offscreen" &&
                KWindowEffects::isEffectAvailable(KWindowEffects::BlurBehind);
    const QRegion region(
        rounded(QRectF(widget->rect()).adjusted(20, 20, -20, -20), 30).toFillPolygon().toPolygon());
    KWindowEffects::enableBlurBehind(widget->windowHandle(),
                                     available && !current.reducedTransparency, region);
    if (KWindowEffects::isEffectAvailable(KWindowEffects::BackgroundContrast))
        KWindowEffects::enableBackgroundContrast(widget->windowHandle(),
                                                 available && !current.reducedTransparency, 1.02,
                                                 current.dark ? .96 : 1.04, 1.15, region);
#endif
    current.blur = available;
    widget->setProperty("desktopBlur", available);
    widget->update();
}

// A deterministic, code-drawn backdrop used only by the explicit design preview renderer.
void paintPreviewBackdrop(QPainter &p, QRectF r) {
    QLinearGradient sky(r.topLeft(), r.bottomRight());
    sky.setColorAt(0, QColor("#8bafe0"));
    sky.setColorAt(.47, QColor("#d6b9cf"));
    sky.setColorAt(1, QColor("#f1d3b6"));
    p.fillRect(r, sky);
    for (int i = 0; i < 4; ++i) {
        QPainterPath hill;
        const qreal y = r.top() + r.height() * (.34 + i * .15);
        hill.moveTo(r.left() - 100, r.bottom() + 100);
        hill.lineTo(r.left() - 100, y + 140);
        hill.cubicTo(r.left() + r.width() * .35, y - 260 + i * 30, r.left() + r.width() * .58,
                     y + 240, r.right() + 100, y - 80);
        hill.lineTo(r.right() + 100, r.bottom() + 100);
        hill.closeSubpath();
        QLinearGradient gradient(r.topLeft(), r.bottomLeft());
        gradient.setColorAt(0, QColor(92 + i * 14, 115 + i * 8, 179 + i * 7, 120));
        gradient.setColorAt(1, QColor(48 + i * 21, 78 + i * 15, 137 + i * 17, 220));
        p.fillPath(hill, gradient);
    }
}

// Paints the synthetic blurred backdrop the preview renderer stands in for desktop blur.
static void paintPreviewMaterial(QPainter &p, QRectF body) {
    QImage sample(192, 216, QImage::Format_ARGB32_Premultiplied);
    sample.fill(Qt::transparent);
    QPainter backdrop(&sample);
    paintPreviewBackdrop(backdrop, sample.rect());
    backdrop.end();
    QGraphicsScene scene;
    auto *item = scene.addPixmap(QPixmap::fromImage(sample));
    auto *blur = new QGraphicsBlurEffect;
    blur->setBlurRadius(16);
    blur->setBlurHints(QGraphicsBlurEffect::QualityHint);
    item->setGraphicsEffect(blur);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    scene.render(&p, body, sample.rect(), Qt::IgnoreAspectRatio);
}

// macOS: the system draws the shadow and the glass; Ranslator adds only a legibility tint and
// the hairline edge Apple windows have.
static void paintMacWindow(QWidget *widget, QPainter &p) {
    const QRectF body =
        QRectF(widget->rect()).adjusted(windowInset, windowInset, -windowInset, -windowInset);
    const bool preview = widget->property("designPreview").toBool();
    if (preview) {
        // Stand-in for the window shadow WindowServer draws around the real window.
        for (int i = 16; i > 0; --i) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0, 0, 0, current.dark ? 4 : 3));
            p.drawRoundedRect(body.adjusted(-i, -i / 2.0, i, i).translated(0, 6), windowRadius + i,
                              windowRadius + i);
        }
    }
    const auto path = rounded(body, windowRadius);
    const bool glass =
        !current.reducedTransparency && (widget->property("desktopBlur").toBool() || preview);
    p.save();
    p.setClipPath(path);
    if (preview && glass)
        paintPreviewMaterial(p, body);
    QColor tint = current.dark ? QColor(22, 22, 26) : QColor(248, 248, 250);
    if (glass)
        tint.setAlpha(current.dark ? 182 : 150);
    p.fillPath(path, tint);
    p.restore();
    p.setBrush(Qt::NoBrush);
    // Outer hairline separates the window from any background; the inner one catches light from
    // above, as Liquid Glass and vibrant windows do.
    p.setPen(QPen(QColor(0, 0, 0, current.dark ? 140 : 34), 1));
    p.drawPath(path);
    QLinearGradient light(body.topLeft(), body.bottomLeft());
    light.setColorAt(0, QColor(255, 255, 255, current.dark ? 52 : 190));
    light.setColorAt(.35, QColor(255, 255, 255, current.dark ? 14 : 70));
    light.setColorAt(1, QColor(255, 255, 255, current.dark ? 10 : 40));
    p.setPen(QPen(light, 1));
    p.drawPath(rounded(body.adjusted(1, 1, -1, -1), windowRadius - 1));
}

void paintWindow(QWidget *widget) {
    QPainter p(widget);
    // Repaints must replace the old translucent material, including when the platform
    // reuses a populated backing buffer. SourceOver alone darkens each damaged child
    // rectangle on successive paints. Qt's system clip limits this clear to the damage.
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.fillRect(widget->rect(), Qt::transparent);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    p.setRenderHint(QPainter::Antialiasing);
    if (macStyle) {
        paintMacWindow(widget, p);
        return;
    }
    const QRectF body = QRectF(widget->rect()).adjusted(20, 20, -20, -20);
    for (int i = 18; i > 0; --i) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(10, 24, 49, 2));
        p.drawRoundedRect(body.adjusted(-i, -i / 2.0, i, i).translated(0, 4), 30 + i, 30 + i);
    }
    const auto path = rounded(body, 30);
    const bool preview = widget->property("designPreview").toBool();
    const bool transparent =
        !current.reducedTransparency && (widget->property("desktopBlur").toBool() || preview);
    p.save();
    p.setClipPath(path);
    if (preview) {
        // The preview has a synthetic backdrop; the running app uses compositor blur.
        paintPreviewMaterial(p, body);
    }
    QLinearGradient material(body.topLeft(), body.bottomRight());
    if (current.dark) {
        material.setColorAt(0, QColor(22, 29, 42, transparent ? 212 : 255));
        material.setColorAt(.5, QColor(16, 22, 32, transparent ? 205 : 255));
        material.setColorAt(1, QColor(21, 27, 39, transparent ? 222 : 255));
    } else {
        material.setColorAt(0, QColor(255, 255, 255, transparent ? 148 : 255));
        material.setColorAt(.48, QColor(238, 246, 255, transparent ? 120 : 255));
        material.setColorAt(1, QColor(231, 239, 252, transparent ? 156 : 255));
    }
    p.fillPath(path, material);
    QRadialGradient sheen(body.topLeft() + QPointF(body.width() * .22, 0), body.width() * .85);
    sheen.setColorAt(0, QColor(255, 255, 255, current.dark ? 6 : 100));
    sheen.setColorAt(1, Qt::transparent);
    p.fillPath(path, sheen);
    p.restore();
    QLinearGradient rim(body.topLeft(), body.bottomRight());
    rim.setColorAt(0, QColor(255, 255, 255, current.dark ? 145 : 245));
    rim.setColorAt(.25, QColor(255, 255, 255, current.dark ? 45 : 60));
    rim.setColorAt(.52, QColor(100, 135, 190, current.dark ? 22 : 35));
    rim.setColorAt(.8, QColor(255, 255, 255, current.dark ? 60 : 90));
    rim.setColorAt(1, QColor(255, 255, 255, current.dark ? 125 : 210));
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(rim, 1.2));
    p.drawPath(path);
    p.setPen(QPen(QColor(255, 255, 255, current.dark ? 14 : 75), .8));
    p.drawRoundedRect(body.adjusted(2, 2, -2, -2), 28, 28);
}

// Two arcs of one circle with arrowheads on the tangent, symmetric under a half turn: the
// "continuous" glyph. `start`/`end` are degrees, counter-clockwise from three o'clock.
static void drawCycleArrow(QPainter &p, QColor color, qreal start, qreal end) {
    const QPointF center(12, 12);
    const qreal radius = 7.5;
    const QRectF circle(center - QPointF(radius, radius), QSizeF(2 * radius, 2 * radius));
    QPainterPath arc;
    arc.arcMoveTo(circle, start);
    arc.arcTo(circle, start, end - start);
    p.drawPath(arc);
    // Clockwise travel on screen (decreasing angle) has tangent (sin θ, cos θ) in y-down space.
    const qreal theta = qDegreesToRadians(end);
    const QPointF tip = center + radius * QPointF(std::cos(theta), -std::sin(theta));
    const QPointF tangent(std::sin(theta), std::cos(theta)),
        normal(std::cos(theta), -std::sin(theta));
    QPainterPath head;
    head.moveTo(tip + tangent * 2.4);
    head.lineTo(tip - tangent * 1.6 + normal * 2.7);
    head.lineTo(tip - tangent * 1.6 - normal * 2.7);
    head.closeSubpath();
    p.save();
    p.setBrush(color);
    p.setPen(QPen(color, 1.1, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawPath(head);
    p.restore();
}

#ifdef Q_OS_MACOS
// SF Symbols for each glyph name: the native icon language, drawn by AppKit in any color.
static QString symbolName(const QString &glyph) {
    static const QHash<QString, QString> names{{"close", "xmark"},
                                               {"expand", "arrow.up.left.and.arrow.down.right"},
                                               {"copy", "doc.on.doc"},
                                               {"arrow", "arrow.right"},
                                               {"swap", "arrow.left.arrow.right"},
                                               {"scan", "text.viewfinder"},
                                               {"select", "cursorarrow"},
                                               {"loop", "arrow.triangle.2.circlepath"},
                                               {"settings", "slider.horizontal.3"},
                                               {"spark", "character.bubble"},
                                               {"language", "globe"},
                                               {"keyboard", "keyboard"},
                                               {"appearance", "circle.lefthalf.filled"},
                                               {"pin", "pin"},
                                               {"power", "power"}};
    return names.value(glyph);
}
static bool drawSymbol(QPainter &p, const QString &glyph, QRectF rect, QColor color) {
    const auto name = symbolName(glyph);
    if (name.isEmpty())
        return false;
    const qreal scale = p.device() ? p.device()->devicePixelRatioF() : 2;
    const qreal points = rect.height() * .78;
    // Hover animations repaint every frame; render each symbol, size and color once.
    static QHash<QString, QImage> cache;
    const auto key = QString("%1|%2|%3|%4").arg(name).arg(points).arg(color.rgba()).arg(scale);
    auto image = cache.value(key);
    if (image.isNull()) {
        image = NativeMac::symbol(name, points, color, scale);
        if (image.isNull())
            return false;
        if (cache.size() > 256)
            cache.clear();
        cache.insert(key, image);
    }
    QSizeF size = image.deviceIndependentSize();
    if (size.width() > rect.width())
        size *= rect.width() / size.width();
    p.save();
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawImage(QRectF(rect.center() - QPointF(size.width(), size.height()) / 2, size), image);
    p.restore();
    return true;
}
#endif

void drawGlyph(QPainter &p, const QString &name, QRectF rect, QColor color, bool native) {
#ifdef Q_OS_MACOS
    if (native && drawSymbol(p, name, rect, color))
        return;
#else
    Q_UNUSED(native);
#endif
    p.save();
    p.translate(rect.topLeft());
    p.scale(rect.width() / 24, rect.height() / 24);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(color, 1.65, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    if (name == "close") {
        p.drawLine(QPointF(7, 7), QPointF(17, 17));
        p.drawLine(QPointF(17, 7), QPointF(7, 17));
    } else if (name == "expand") {
        p.drawLine(QPointF(5, 19), QPointF(19, 5));
        p.drawLine(QPointF(11, 5), QPointF(19, 5));
        p.drawLine(QPointF(19, 5), QPointF(19, 13));
        p.drawLine(QPointF(5, 11), QPointF(5, 19));
        p.drawLine(QPointF(5, 19), QPointF(13, 19));
    } else if (name == "copy") {
        p.drawRoundedRect(QRectF(8, 8, 12, 13), 3, 3);
        QPainterPath a;
        a.moveTo(15, 4);
        a.lineTo(7, 4);
        a.quadTo(4, 4, 4, 7);
        a.lineTo(4, 15);
        p.drawPath(a);
    } else if (name == "arrow") {
        p.drawLine(QPointF(5, 12), QPointF(19, 12));
        p.drawLine(QPointF(14, 7), QPointF(19, 12));
        p.drawLine(QPointF(14, 17), QPointF(19, 12));
    } else if (name == "swap") {
        p.drawLine(QPointF(4, 8), QPointF(20, 8));
        p.drawLine(QPointF(16, 4), QPointF(20, 8));
        p.drawLine(QPointF(16, 12), QPointF(20, 8));
        p.drawLine(QPointF(20, 16), QPointF(4, 16));
        p.drawLine(QPointF(8, 12), QPointF(4, 16));
        p.drawLine(QPointF(8, 20), QPointF(4, 16));
    } else if (name == "scan") {
        for (int i = 0; i < 4; ++i) {
            p.save();
            p.translate(12, 12);
            p.rotate(i * 90);
            QPainterPath a;
            a.moveTo(-8, -3);
            a.lineTo(-8, -6);
            a.quadTo(-8, -8, -6, -8);
            a.lineTo(-3, -8);
            p.drawPath(a);
            p.restore();
        }
        p.drawLine(QPointF(7, 10), QPointF(17, 10));
        p.drawLine(QPointF(7, 14), QPointF(14, 14));
    } else if (name == "select") {
        QPainterPath a;
        a.moveTo(6, 3);
        a.lineTo(6, 19);
        a.lineTo(10, 15);
        a.lineTo(13, 21);
        a.lineTo(16, 19.5);
        a.lineTo(13, 13.5);
        a.lineTo(19, 13);
        a.closeSubpath();
        p.drawPath(a);
    } else if (name == "loop") {
        // Upper arc runs clockwise from the left over the top; the lower arc is its half turn.
        drawCycleArrow(p, color, 168, 16);
        drawCycleArrow(p, color, 348, 196);
    } else if (name == "power") {
        // A ring open at the top, clockwise from one o'clock to eleven, with the bar through
        // the opening.
        QPainterPath ring;
        const QRectF circle(4.5, 5, 15, 15);
        ring.arcMoveTo(circle, 62);
        ring.arcTo(circle, 62, -304);
        p.drawPath(ring);
        p.drawLine(QPointF(12, 3), QPointF(12, 11.5));
    } else if (name == "settings") {
        p.drawLine(QPointF(4, 7), QPointF(20, 7));
        p.drawLine(QPointF(4, 17), QPointF(20, 17));
        p.setBrush(color);
        p.drawEllipse(QPointF(9, 7), 2.5, 2.5);
        p.drawEllipse(QPointF(15, 17), 2.5, 2.5);
    } else if (name == "spark") {
        QPainterPath a;
        a.moveTo(12, 2);
        a.quadTo(13, 10, 21, 12);
        a.quadTo(13, 14, 12, 22);
        a.quadTo(10, 14, 2, 12);
        a.quadTo(10, 10, 12, 2);
        p.drawPath(a);
    } else if (name == "language") {
        p.drawEllipse(QRectF(3, 3, 18, 18));
        p.drawEllipse(QRectF(8, 3, 8, 18));
        p.drawLine(QPointF(3, 12), QPointF(21, 12));
    } else if (name == "keyboard") {
        p.drawRoundedRect(QRectF(2, 5, 20, 14), 3, 3);
        for (int x = 6; x <= 18; x += 4)
            p.drawPoint(x, 9);
        p.drawLine(QPointF(7, 15), QPointF(17, 15));
    } else if (name == "appearance") {
        p.drawEllipse(QRectF(3, 3, 18, 18));
        QPainterPath a;
        a.moveTo(12, 3);
        a.arcTo(QRectF(3, 3, 18, 18), 90, -180);
        a.closeSubpath();
        p.fillPath(a, color);
    } else if (name == "pin") {
        QPainterPath a;
        a.moveTo(8, 3);
        a.lineTo(16, 3);
        a.lineTo(15, 10);
        a.lineTo(19, 14);
        a.lineTo(5, 14);
        a.lineTo(9, 10);
        a.closeSubpath();
        p.drawPath(a);
        p.drawLine(QPointF(12, 14), QPointF(12, 21));
    }
    p.restore();
}
QImage glyphSheet() {
    const QStringList names{"select",   "loop",     "scan",       "arrow",    "swap",
                            "copy",     "close",    "expand",     "settings", "spark",
                            "language", "keyboard", "appearance", "power",    "pin"};
    const QList<int> sizes{16, 20, 24, 48};
    const int cell = 60, label = 96, scale = 2;
    const int columns = sizes.size() * (macStyle ? 2 : 1);
    QImage sheet((label + columns * cell) * scale, 2 * names.size() * cell * scale,
                 QImage::Format_ARGB32_Premultiplied);
    sheet.setDevicePixelRatio(scale);
    QPainter p(&sheet);
    p.setRenderHint(QPainter::Antialiasing);
    for (int theme = 0; theme < 2; ++theme) {
        const bool dark = theme == 1;
        const QColor ink = dark ? QColor("#f5f5f7") : QColor("#1d1d1f");
        const int top = theme * names.size() * cell;
        p.fillRect(QRect(0, top, label + columns * cell, names.size() * cell),
                   dark ? QColor("#1e1e22") : QColor("#f5f5f7"));
        for (int row = 0; row < names.size(); ++row) {
            const int y = top + row * cell;
            p.setPen(dark ? QColor("#98989f") : QColor("#6e6e73"));
            p.drawText(QRect(10, y, label - 10, cell), Qt::AlignVCenter, names[row]);
            for (int column = 0; column < columns; ++column) {
                const int size = sizes[column % sizes.size()];
                const QRectF box(label + column * cell + (cell - size) / 2.0,
                                 y + (cell - size) / 2.0, size, size);
                drawGlyph(p, names[row], box, ink, column >= sizes.size());
            }
        }
    }
    return sheet;
}

QIcon icon(const QString &name) {
    QPixmap image(48, 48);
    image.fill(Qt::transparent);
    image.setDevicePixelRatio(2);
    QPainter p(&image);
    drawGlyph(p, name, QRectF(0, 0, 24, 24), current.ink());
    return QIcon(image);
}
void fadeIn(QWidget *widget) {
    if (current.reducedMotion || QGuiApplication::platformName() == "offscreen")
        return;
    auto *effect = new QGraphicsOpacityEffect(widget);
    widget->setGraphicsEffect(effect);
    auto *animation = new QPropertyAnimation(effect, "opacity", effect);
    animation->setDuration(macStyle ? 220 : 180);
    animation->setStartValue(macStyle ? 0.0 : .35);
    animation->setEndValue(1.0);
    animation->setEasingCurve(QEasingCurve::OutCubic);
    QObject::connect(animation, &QPropertyAnimation::finished, widget,
                     [widget] { widget->setGraphicsEffect(nullptr); });
    animation->start();
}
QImage preview(QWidget *widget) {
    widget->ensurePolished();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    if (widget->layout())
        widget->layout()->activate();
    widget->setProperty("designPreview", true);
    const auto scale = 1.5;
    QImage image(widget->size() * scale, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(scale);
    image.fill(Qt::transparent);
    QPainter p(&image);
    paintPreviewBackdrop(p, widget->rect());
    // paintWindow owns and clears its surface. Composite that surface over the preview
    // backdrop, just as a compositor does, rather than painting into the backdrop itself.
    QImage surface(image.size(), QImage::Format_ARGB32_Premultiplied);
    surface.setDevicePixelRatio(scale);
    surface.fill(Qt::transparent);
    widget->render(&surface);
    p.drawImage(QPoint(), surface);
    p.end();
    widget->setProperty("designPreview", false);
    return image;
}
} // namespace Design

GlassComboBox::GlassComboBox(QWidget *parent) : QComboBox(parent) {
    auto *list = new QListView;
    list->setItemDelegate(new QStyledItemDelegate(list));
    setView(list);
}

void GlassComboBox::showPopup() {
    // The popup container is a separate native window. Styling only the item view
    // leaves its rectangular palette background visible around the rounded list.
    auto *popup = view()->window();
    popup->setAttribute(Qt::WA_TranslucentBackground);
    popup->setAutoFillBackground(false);
    popup->setObjectName("comboPopupContainer");
    popup->setStyleSheet(
        "QFrame#comboPopupContainer { background: transparent; border: none; padding: 0; }");
    view()->viewport()->setAutoFillBackground(false);
    QComboBox::showPopup();
    if (auto *frame = qobject_cast<QFrame *>(popup))
        frame->setFrameShape(QFrame::NoFrame);
}

GlassButton::GlassButton(const QString &text, QWidget *parent) : QPushButton(text, parent) {
    setCursor(Qt::PointingHandCursor);
    setMinimumHeight(36);
    setAttribute(Qt::WA_Hover);
    hoverAnimation.setDuration(150);
    hoverAnimation.setEasingCurve(QEasingCurve::OutCubic);
    connect(&hoverAnimation, &QVariantAnimation::valueChanged, this, [this](QVariant value) {
        hover = value.toReal();
        update();
    });
    connect(this, &QPushButton::toggled, this, qOverload<>(&QWidget::update));
}
QSize GlassButton::sizeHint() const {
    return QSize(fontMetrics().horizontalAdvance(text()) +
                     (property("glyph").toString().isEmpty() ? 32 : 60),
                 40);
}
void GlassButton::enterEvent(QEnterEvent *event) {
    QPushButton::enterEvent(event);
    hoverAnimation.stop();
    if (Design::theme().reducedMotion) {
        hover = 1;
        update();
        return;
    }
    hoverAnimation.setStartValue(hover);
    hoverAnimation.setEndValue(1.0);
    hoverAnimation.start();
}
void GlassButton::leaveEvent(QEvent *event) {
    QPushButton::leaveEvent(event);
    hoverAnimation.stop();
    if (Design::theme().reducedMotion) {
        hover = 0;
        update();
        return;
    }
    hoverAnimation.setStartValue(hover);
    hoverAnimation.setEndValue(0.0);
    hoverAnimation.start();
}
// macOS controls: capsule buttons, accent-filled primary and selected sidebar rows, borderless
// toolbar icons whose glyph brightens on hover, and segments that leave the thumb to the bar.
static void paintMacButton(GlassButton *button, QPainter &p, qreal hover) {
    const auto &t = Design::theme();
    const bool primary = button->objectName() == "primary",
               quiet = button->property("quiet").toBool(),
               nav = button->property("navigation").toBool(),
               segment = button->property("segment").toBool();
    const bool selected = button->isChecked() && !segment;
    QRectF r = QRectF(button->rect()).adjusted(1, 1, -1, -1);
    if (!button->isEnabled())
        p.setOpacity(.35);
    if (button->isDown()) {
        // Pressing sinks the control slightly instead of shifting its edges.
        p.translate(r.center());
        p.scale(.97, .97);
        p.translate(-r.center());
    }
    const qreal radius = nav ? 8 : r.height() / 2;
    QColor fill = Qt::transparent, edge = Qt::transparent;
    if (primary) {
        fill = t.accent.lighter(button->isDown() ? 90 : 100 + int(hover * 8));
    } else if (nav && selected) {
        fill = t.accent;
    } else if (selected) {
        fill = QColor(255, 255, 255, t.dark ? 42 : 235);
    } else if (quiet || nav) {
        fill = t.dark ? QColor(255, 255, 255, int(hover * 20)) : QColor(0, 0, 0, int(hover * 12));
    } else {
        fill = t.dark ? QColor(255, 255, 255, 22 + int(hover * 14))
                      : QColor(255, 255, 255, 200 + int(hover * 40));
        edge = t.dark ? QColor(255, 255, 255, 20) : QColor(0, 0, 0, 30);
    }
    p.setBrush(fill);
    p.setPen(edge.alpha() ? QPen(edge, 1) : Qt::NoPen);
    p.drawRoundedRect(r, radius, radius);
    if (primary) {
        // A faint top light gives the accent fill the depth of a native default button.
        QLinearGradient light(r.topLeft(), r.bottomLeft());
        light.setColorAt(0, QColor(255, 255, 255, 46));
        light.setColorAt(.55, QColor(255, 255, 255, 0));
        p.setBrush(light);
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(r, radius, radius);
    }
    if (button->hasFocus() && button->window()->testAttribute(Qt::WA_KeyboardFocusChange)) {
        QColor ring = t.accent;
        ring.setAlpha(150);
        p.setPen(QPen(ring, 2));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r.adjusted(1, 1, -1, -1), radius - 1, radius - 1);
    }
    const bool onAccent = primary || (nav && selected);
    QColor ink = onAccent ? QColor(Qt::white) : t.ink();
    if (quiet && button->text().isEmpty() && !selected) {
        // Toolbar icons stay quiet until they are hovered.
        QColor muted = t.secondary();
        ink = QColor::fromRgbF(muted.redF() + (ink.redF() - muted.redF()) * hover,
                               muted.greenF() + (ink.greenF() - muted.greenF()) * hover,
                               muted.blueF() + (ink.blueF() - muted.blueF()) * hover);
    }
    const auto glyph = button->property("glyph").toString();
    QFont font = button->font();
    if (primary || segment || nav)
        font.setWeight(QFont::Medium);
    p.setFont(font);
    const QFontMetricsF metrics(font);
    const qreal textWidth = metrics.horizontalAdvance(button->text());
    const qreal icon = button->text().isEmpty() ? std::min<qreal>(17, r.height() * .62) : 17;
    const qreal gap = button->text().isEmpty() || glyph.isEmpty() ? 0 : 7;
    const qreal content = (glyph.isEmpty() ? 0 : icon) + gap + textWidth;
    qreal x = nav ? 12 : (button->width() - content) / 2.0;
    if (!glyph.isEmpty()) {
        Design::drawGlyph(p, glyph, QRectF(x, (button->height() - icon) / 2.0, icon, icon),
                          nav && !selected ? t.accent : ink);
        x += icon + gap;
    }
    p.setPen(ink);
    p.drawText(QRectF(x, 0, button->width() - x - 8, button->height()),
               Qt::AlignVCenter | Qt::AlignLeft, button->text());
}

void GlassButton::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    if (Design::macStyle) {
        paintMacButton(this, p, hover);
        return;
    }
    const auto &t = Design::theme();
    const bool primary = objectName() == "primary", quiet = property("quiet").toBool(),
               nav = property("navigation").toBool();
    QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
    if (isDown())
        r.adjust(1.5, 1.5, -1.5, -1.5);
    QColor ink = primary ? Qt::white : t.ink();
    if (!isEnabled())
        p.setOpacity(.38);
    QLinearGradient fill(r.topLeft(), r.bottomRight());
    if (primary) {
        fill.setColorAt(0, QColor(isDown() ? "#3971db" : "#6297ff"));
        fill.setColorAt(1, QColor("#3b6fe8"));
    } else {
        const auto alpha = quiet ? 0 : (t.dark ? 10 : 48);
        fill.setColorAt(0, QColor(255, 255, 255, alpha + int(hover * 35) + (isChecked() ? 65 : 0)));
        fill.setColorAt(
            1, QColor(255, 255, 255, alpha / 2 + int(hover * 16) + (isChecked() ? 35 : 0)));
    }
    p.setBrush(fill);
    p.setPen(QPen(QColor(255, 255, 255,
                         primary  ? 85
                         : quiet  ? int(hover * 60)
                         : t.dark ? 26
                                  : 145),
                  1));
    p.drawRoundedRect(r, nav ? 12 : r.height() / 2, nav ? 12 : r.height() / 2);
    if (hasFocus() && window()->testAttribute(Qt::WA_KeyboardFocusChange)) {
        p.setPen(QPen(QColor(105, 153, 255, 180), 1.5));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r.adjusted(1, 1, -1, -1), r.height() / 2, r.height() / 2);
    }
    const auto glyph = property("glyph").toString();
    const auto width = fontMetrics().horizontalAdvance(text());
    qreal x = nav ? 14
                  : (this->width() - width -
                     (glyph.isEmpty()    ? 0
                      : text().isEmpty() ? 20
                                         : 28)) /
                        2.0;
    if (!glyph.isEmpty()) {
        Design::drawGlyph(p, glyph, QRectF(x, (height() - 20) / 2.0, 20, 20), ink);
        x += 28;
    }
    p.setPen(ink);
    p.drawText(QRectF(x, 0, this->width() - x - 10, height()), Qt::AlignVCenter | Qt::AlignLeft,
               text());
}

GlassSurface::GlassSurface(QWidget *parent) : QWidget(parent) {
    setAttribute(Qt::WA_TranslucentBackground);
    thumbAnimation.setDuration(420);
    thumbAnimation.setEasingCurve(Design::spring());
    thumbAnimation.setStartValue(0.0);
    thumbAnimation.setEndValue(1.0);
    connect(&thumbAnimation, &QVariantAnimation::valueChanged, this, [this](QVariant value) {
        const qreal t = value.toReal();
        thumb = QRectF(thumbFrom.topLeft() + (thumbTo.topLeft() - thumbFrom.topLeft()) * t,
                       thumbFrom.size() + (thumbTo.size() - thumbFrom.size()) * t);
        thumbOpacity = std::clamp(opacityFrom + (opacityTo - opacityFrom) * t, 0.0, 1.0);
        update();
    });
}
void GlassSurface::setSegments(const QList<QAbstractButton *> &buttons) {
    segments.clear();
    for (auto *button : buttons) {
        segments.append(button);
        button->setProperty("segment", true);
        connect(button, &QAbstractButton::toggled, this, [this] { updateThumb(true); });
    }
    updateThumb(false);
}
void GlassSurface::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    if (!segments.isEmpty())
        QTimer::singleShot(0, this, [this] { updateThumb(false); });
}
QRectF GlassSurface::checkedSegment() const {
    for (const auto &button : segments)
        if (button && button->isChecked())
            return QRectF(button->geometry());
    return {};
}
void GlassSurface::updateThumb(bool animate) {
    QRectF target = checkedSegment();
    const qreal opacity = target.isNull() ? 0 : 1;
    if (target.isNull())
        target = thumbOpacity > 0 ? thumbTo : QRectF();
    else if (thumbOpacity == 0)
        thumb = target.adjusted(4, 2, -4, -2); // Appear in place, growing into the segment.
    // Hidden widgets have nothing to animate; they show the final state when they appear.
    if (!animate || !isVisible() || Design::theme().reducedMotion) {
        thumbAnimation.stop();
        thumb = thumbTo = target;
        thumbOpacity = opacity;
        update();
        return;
    }
    thumbFrom = thumb;
    thumbTo = target;
    opacityFrom = thumbOpacity;
    opacityTo = opacity;
    thumbAnimation.stop();
    thumbAnimation.start();
}
void GlassSurface::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const bool dark = Design::theme().dark;
    QRectF r = QRectF(rect()).adjusted(.7, .7, -.7, -.7);
    if (Design::macStyle) {
        // Bars with a large radius are capsules; cards share one concentric-looking radius.
        const double requested = property("radius").isValid() ? property("radius").toDouble() : 0;
        const double radius = requested >= 23 ? r.height() / 2 : 14;
        QColor fill, edge = dark ? QColor(255, 255, 255, 20) : QColor(0, 0, 0, 16);
        if (property("overlayToolbar").toBool())
            fill = dark ? QColor(38, 38, 42, 246) : QColor(250, 250, 252, 246);
        else
            fill = dark ? QColor(255, 255, 255, 12) : QColor(255, 255, 255, 150);
        p.setBrush(fill);
        p.setPen(QPen(edge, 1));
        p.drawRoundedRect(r, radius, radius);
        // At rest the thumb follows the checked segment's current geometry; only a running
        // animation uses the interpolated rectangle.
        // Design previews render the settled state; they do not advance animations.
        const bool moving = thumbAnimation.state() == QAbstractAnimation::Running &&
                            !window()->property("designPreview").toBool();
        const QRectF at = moving ? thumb : checkedSegment();
        const qreal opacity = moving ? thumbOpacity : (at.isEmpty() ? 0 : 1);
        if (opacity > 0 && !at.isEmpty()) {
            p.setOpacity(opacity);
            const QRectF knob = at.adjusted(.5, .5, -.5, -.5);
            const qreal round = knob.height() / 2;
            if (!dark) {
                // Light thumbs float on a soft shadow, like a native segmented control.
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(0, 0, 0, 14));
                p.drawRoundedRect(knob.translated(0, 1.2), round, round);
            }
            p.setBrush(dark ? QColor(255, 255, 255, 40) : QColor(255, 255, 255));
            p.setPen(QPen(dark ? QColor(255, 255, 255, 30) : QColor(0, 0, 0, 18), .8));
            p.drawRoundedRect(knob, round, round);
        }
        return;
    }
    const double radius = property("radius").isValid() ? property("radius").toDouble() : 22;
    QLinearGradient fill(r.topLeft(), r.bottomRight());
    if (property("overlayToolbar").toBool()) {
        fill.setColorAt(0, dark ? QColor(30, 42, 62, 245) : QColor(245, 250, 255, 245));
        fill.setColorAt(1, dark ? QColor(23, 32, 48, 240) : QColor(229, 239, 253, 240));
    } else {
        fill.setColorAt(0, QColor(255, 255, 255, dark ? 8 : 125));
        fill.setColorAt(1, QColor(255, 255, 255, dark ? 5 : 48));
    }
    p.setBrush(fill);
    p.setPen(QPen(QColor(255, 255, 255, dark ? 35 : 205), 1));
    p.drawRoundedRect(r, radius, radius);
}

static void initializeWindow(QWidget *w) {
    w->setWindowFlags(w->windowFlags() | Qt::FramelessWindowHint);
    w->setAttribute(Qt::WA_TranslucentBackground);
    w->setProperty("glassWindow", true);
}
static void showMaterial(QWidget *w) {
    Design::applyBackdrop(w);
    QTimer::singleShot(150, w, [w] { Design::applyBackdrop(w); });
    if (Design::macStyle && !Design::theme().reducedMotion &&
        QGuiApplication::platformName() == "cocoa") {
        // Windows fade in over a few frames, as AppKit panels do.
        w->setWindowOpacity(0);
        auto *appear = new QPropertyAnimation(w, "windowOpacity", w);
        appear->setDuration(160);
        appear->setStartValue(0.0);
        appear->setEndValue(1.0);
        appear->setEasingCurve(QEasingCurve::OutCubic);
        appear->start(QAbstractAnimation::DeleteWhenStopped);
    }
}
static void moveMaterial(QWidget *w, QMouseEvent *e) {
    if (e->button() != Qt::LeftButton || !w->windowHandle())
        return;
    const auto pos = e->position();
    Qt::Edges edges;
    if (pos.x() < 24)
        edges |= Qt::LeftEdge;
    if (pos.x() > w->width() - 24)
        edges |= Qt::RightEdge;
    if (pos.y() < 24)
        edges |= Qt::TopEdge;
    if (pos.y() > w->height() - 24)
        edges |= Qt::BottomEdge;
    if (edges)
        w->windowHandle()->startSystemResize(edges);
    else if (pos.y() < 90)
        w->windowHandle()->startSystemMove();
}
GlassWindow::GlassWindow(QWidget *parent) : QWidget(parent) { initializeWindow(this); }
void GlassWindow::showEvent(QShowEvent *e) {
    QWidget::showEvent(e);
    showMaterial(this);
}
void GlassWindow::resizeEvent(QResizeEvent *e) {
    QWidget::resizeEvent(e);
    Design::applyBackdrop(this);
}
void GlassWindow::mousePressEvent(QMouseEvent *e) { moveMaterial(this, e); }
GlassDialog::GlassDialog(QWidget *parent) : QDialog(parent) { initializeWindow(this); }
void GlassDialog::showEvent(QShowEvent *e) {
    QDialog::showEvent(e);
    showMaterial(this);
}
void GlassDialog::resizeEvent(QResizeEvent *e) {
    QDialog::resizeEvent(e);
    Design::applyBackdrop(this);
}
void GlassDialog::mousePressEvent(QMouseEvent *e) { moveMaterial(this, e); }
