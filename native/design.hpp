#pragma once
#include <QtWidgets>

namespace Design {
// macOS builds follow the platform's visual language (system font, accent color, materials,
// SF Symbols and springs); other platforms keep Ranslator's own glass style.
#ifdef Q_OS_MACOS
constexpr bool macStyle = true;
#else
constexpr bool macStyle = false;
#endif
struct Theme {
    bool dark = false;
    bool reducedMotion = false;
    bool reducedTransparency = false;
    bool blur = false;
    QColor accent{"#487ff2"};
    QColor ink() const {
        if (macStyle)
            return dark ? QColor("#f5f5f7") : QColor("#1d1d1f");
        return dark ? QColor("#f2f5fc") : QColor("#202b3c");
    }
    QColor secondary() const {
        if (macStyle)
            return dark ? QColor("#98989f") : QColor("#6e6e73");
        return dark ? QColor("#a8b4c9") : QColor("#65758b");
    }
};
// Window body geometry inside the transparent shadow canvas.
constexpr qreal windowInset = 20;
constexpr qreal windowRadius = macStyle ? 26 : 30;
// A critically damped-ish spring (slight settle, no visible bounce) for interface motion.
QEasingCurve spring();
Theme &theme();
void initialize();
QFont textFont(QFont font, const QString &language = {});
void configure(const QJsonObject &config);
// `native` uses the platform's own symbol when it has one (SF Symbols on macOS).
void drawGlyph(QPainter &p, const QString &name, QRectF rect, QColor color, bool native = true);
// Every glyph at several sizes in both themes, drawn and native side by side, for review.
QImage glyphSheet();
QIcon icon(const QString &name);
void fadeIn(QWidget *widget);
void applyBackdrop(QWidget *widget);
void paintWindow(QWidget *widget);
void paintPreviewBackdrop(QPainter &p, QRectF rect);
QImage preview(QWidget *widget);
} // namespace Design

class GlassComboBox : public QComboBox {
  public:
    explicit GlassComboBox(QWidget *parent = nullptr);
    void showPopup() override;
};

class GlassButton : public QPushButton {
    QVariantAnimation hoverAnimation;
    qreal hover = 0;

  protected:
    void paintEvent(QPaintEvent *) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *) override;

  public:
    explicit GlassButton(const QString &text = {}, QWidget *parent = nullptr);
    QSize sizeHint() const override;
};

class GlassSurface : public QWidget {
    QList<QPointer<QAbstractButton>> segments;
    QVariantAnimation thumbAnimation;
    QRectF thumb, thumbFrom, thumbTo;
    qreal thumbOpacity = 0, opacityFrom = 0, opacityTo = 0;
    void updateThumb(bool animate);
    QRectF checkedSegment() const;

  protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;

  public:
    explicit GlassSurface(QWidget *parent = nullptr);
    // Segmented-control behaviour: a shared thumb slides to whichever button is checked.
    void setSegments(const QList<QAbstractButton *> &buttons);
};

class GlassWindow : public QWidget {
  protected:
    void paintEvent(QPaintEvent *) override { Design::paintWindow(this); }
    void showEvent(QShowEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void mousePressEvent(QMouseEvent *) override;

  public:
    explicit GlassWindow(QWidget *parent = nullptr);
};

class GlassDialog : public QDialog {
  protected:
    void paintEvent(QPaintEvent *) override { Design::paintWindow(this); }
    void showEvent(QShowEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void mousePressEvent(QMouseEvent *) override;

  public:
    explicit GlassDialog(QWidget *parent = nullptr);
};
