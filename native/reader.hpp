#pragma once
#include "i18n.hpp"
#include <QtWidgets>
#include <functional>
#include <utility>

// A child card cannot use QSizeGrip: that would resize its full-screen parent.
class TranslationResizeHandle final : public QWidget {
    QWidget *target;
    QPoint origin;
    QSize initialSize;
    bool dragging = false;

  protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        auto color = palette().color(QPalette::WindowText);
        color.setAlpha(140);
        painter.setPen(QPen(color, 1.3, Qt::SolidLine, Qt::RoundCap));
        for (int offset : {4, 9, 14})
            painter.drawLine(width() - offset, height() - 3, width() - 3, height() - offset);
    }
    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() != Qt::LeftButton)
            return;
        dragging = true;
        origin = event->globalPosition().toPoint();
        initialSize = target->size();
        grabMouse();
        event->accept();
    }
    void mouseMoveEvent(QMouseEvent *event) override {
        if (!dragging)
            return;
        auto size = initialSize + QSize(event->globalPosition().x() - origin.x(),
                                        event->globalPosition().y() - origin.y());
        QSize maximum = target->maximumSize();
        if (target->isWindow() && target->screen()) {
            const auto bounds = target->screen()->availableGeometry();
            maximum = maximum.boundedTo(
                QSize(bounds.right() - target->x() + 1, bounds.bottom() - target->y() + 1));
        } else if (target->parentWidget()) {
            maximum = maximum.boundedTo(target->parentWidget()->size() -
                                        QSize(target->x(), target->y()) - QSize(8, 8));
        }
        target->resize(size.boundedTo(maximum).expandedTo(target->minimumSize()));
        event->accept();
    }
    void mouseReleaseEvent(QMouseEvent *event) override {
        if (event->button() != Qt::LeftButton || !dragging)
            return;
        mouseMoveEvent(event);
        dragging = false;
        releaseMouse();
        event->accept();
    }

  public:
    explicit TranslationResizeHandle(QWidget *owner) : QWidget(owner), target(owner) {
        setFixedSize(20, 20);
        setObjectName("translationResizeHandle");
        setAccessibleName(UiText::tr("Resize translation"));
        setToolTip(UiText::tr("Drag to resize the translation"));
        setCursor(Qt::SizeFDiagCursor);
    }
};

// A bare Shift tap leaves reading mode. Shift+arrow and Shift+mouse still select text.
class ReaderKeyFilter final : public QObject {
    std::function<bool()> expanded;
    std::function<void()> restore;
    bool shiftPending = false;

  protected:
    bool eventFilter(QObject *, QEvent *event) override {
        if (!expanded()) {
            shiftPending = false;
            return false;
        }
        if (QApplication::activePopupWidget()) {
            shiftPending = false;
            return false;
        }
        if (event->type() == QEvent::ShortcutOverride && !QApplication::activePopupWidget()) {
            auto *key = static_cast<QKeyEvent *>(event);
            if (key->key() == Qt::Key_Escape) {
                event->accept();
                return true;
            }
        }
        if (event->type() == QEvent::KeyPress) {
            auto *key = static_cast<QKeyEvent *>(event);
            if (key->key() == Qt::Key_Escape && !QApplication::activePopupWidget()) {
                shiftPending = false;
                restore();
                return true;
            }
            if (!key->isAutoRepeat())
                shiftPending =
                    key->key() == Qt::Key_Shift && !(key->modifiers() & ~Qt::ShiftModifier);
        } else if (event->type() == QEvent::KeyRelease) {
            auto *key = static_cast<QKeyEvent *>(event);
            if (key->key() == Qt::Key_Shift && !key->isAutoRepeat()) {
                const bool leave = std::exchange(shiftPending, false);
                if (leave) {
                    restore();
                    return true;
                }
            }
        } else if (event->type() == QEvent::MouseButtonPress ||
                   event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::Wheel ||
                   event->type() == QEvent::FocusOut) {
            shiftPending = false;
        }
        return false;
    }

  public:
    ReaderKeyFilter(QWidget *owner, std::function<bool()> isExpanded, std::function<void()> leave)
        : QObject(owner), expanded(std::move(isExpanded)), restore(std::move(leave)) {
        owner->installEventFilter(this);
        for (auto *child : owner->findChildren<QWidget *>())
            child->installEventFilter(this);
    }
};
