#pragma once
#include <QtWidgets>

namespace Design {
struct Theme {
    bool dark = false;
    bool reducedMotion = false;
    bool reducedTransparency = false;
    bool blur = false;
    QColor ink() const { return dark ? QColor("#f2f5fc") : QColor("#202b3c"); }
    QColor secondary() const { return dark ? QColor("#a8b4c9") : QColor("#65758b"); }
};
Theme &theme();
void initialize();
QFont textFont(QFont font, const QString &language = {});
void configure(const QJsonObject &config);
void drawGlyph(QPainter &p, const QString &name, QRectF rect, QColor color);
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
  protected:
    void paintEvent(QPaintEvent *) override;

  public:
    explicit GlassSurface(QWidget *parent = nullptr);
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
