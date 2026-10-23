#pragma once
#include <QApplication>
#include <QFocusFrame>
#include <QPainter>
#include <QPalette>
#include <QPointer>
#include <QTabBar>
#include <QVariant>

namespace ui {
// The app's editing surfaces suppress decorative focus outlines. Extension
// tools still need an unambiguous keyboard focus indicator for native controls.
class ScriptFocusFrame final : public QFocusFrame {
  public:
    static void install(QWidget *root) {
        // QFocusFrame reparents itself into the control's container. A tree
        // replacement may delete it; keep the observer on the surviving window.
        auto follow = [root, frame = QPointer<ScriptFocusFrame>()](QWidget *,
                                                                   QWidget *focus) mutable {
            auto *target = focus && focus != root && root->isAncestorOf(focus) ? focus : nullptr;
            if (!frame) {
                if (!target)
                    return;
                frame = new ScriptFocusFrame(root);
            }
            frame->setWidget(target);
            if (target) {
                frame->clearMask();
                frame->show();
                frame->raise();
            }
        };
        follow(nullptr, QApplication::focusWidget());
        QObject::connect(qApp, &QApplication::focusChanged, root, std::move(follow));
    }

  private:
    explicit ScriptFocusFrame(QWidget *root) : QFocusFrame(root) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        hide();
    }
    bool eventFilter(QObject *watched, QEvent *event) override {
        const bool handled = QFocusFrame::eventFilter(watched, event);
        if (widget()) {
            switch (event->type()) {
            case QEvent::Move:
            case QEvent::Resize:
            case QEvent::Show:
            case QEvent::StyleChange:
            case QEvent::PaletteChange:
            case QEvent::ZOrderChange:
                // Qt supplies a mask for its own outside-only focus drawing and
                // hides it on style changes. Our inset outline has different bounds.
                clearMask();
                if (widget()->hasFocus() && widget()->isVisible()) {
                    show();
                    raise();
                    update();
                }
                break;
            default:
                break;
            }
        }
        return handled;
    }
    void paintEvent(QPaintEvent *) override {
        if (!widget())
            return;
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        auto focusColor = palette().highlight().color();
        for (auto *control = widget(); control; control = control->parentWidget())
            if (control->property("vlt.scriptPaletteData").isValid()) {
                focusColor = control->property("vlt.scriptPaletteData")
                                 .value<QPalette>()
                                 .highlight()
                                 .color();
                break;
            }
        painter.setPen(QPen(focusColor, 2));
        painter.setBrush(Qt::NoBrush);
        // Draw inside the focused control: tightly packed rows otherwise clip
        // the top, bottom and outer edge of a frame outside its parent container.
        auto focusRect = QRectF(widget()->rect());
        focusRect.translate(mapFromGlobal(widget()->mapToGlobal(QPoint())));
        focusRect.adjust(1, 1, -1, -1);
        if (auto *tabs = qobject_cast<QTabBar *>(widget())) {
            // Highlight the keyboard's current tab, not the entire strip of tabs.
            focusRect = tabs->tabRect(tabs->currentIndex());
            focusRect.translate(mapFromGlobal(tabs->mapToGlobal(QPoint())));
            focusRect.adjust(2, 2, -2, -2);
        }
        painter.drawRoundedRect(focusRect, 4, 4);
    }
};
} // namespace ui
