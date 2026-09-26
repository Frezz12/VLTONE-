#pragma once

#include <QPainter>
#include <QPainterPath>
#include <QProxyStyle>
#include <QComboBox>
#include <QPushButton>
#include <QToolButton>
#include <QStyleOption>
#include <QLinearGradient>
#include <QRadialGradient>

// Share crisp disclosure chevrons between ordinary menus, combos and the
// scrollable plugin picker. Fusion's tiny filled triangles look like dots.
class PopupStyle : public QProxyStyle {
public:
    using QProxyStyle::QProxyStyle;

    void polish(QWidget* widget) override {
        QProxyStyle::polish(widget);
        if (qobject_cast<QComboBox*>(widget) ||
            qobject_cast<QToolButton*>(widget) || qobject_cast<QPushButton*>(widget))
            widget->setAttribute(Qt::WA_MacShowFocusRect, false);
    }

    int pixelMetric(PixelMetric metric, const QStyleOption* option = nullptr,
                    const QWidget* widget = nullptr) const override {
        if (metric == PM_IndicatorWidth) return 34;
        if (metric == PM_IndicatorHeight) return 18;
        return QProxyStyle::pixelMetric(metric, option, widget);
    }

    int styleHint(StyleHint hint, const QStyleOption* option = nullptr,
                  const QWidget* widget = nullptr,
                  QStyleHintReturn* returnData = nullptr) const override {
        // Use Qt's list delegate so combo rows honour the shared item styling.
        if (hint == SH_ComboBox_Popup) return 0;
        return QProxyStyle::styleHint(hint, option, widget, returnData);
    }

    void drawPrimitive(PrimitiveElement element, const QStyleOption* option,
                       QPainter* painter, const QWidget* widget = nullptr) const override {
        const auto* toolButton = qobject_cast<const QToolButton*>(widget);
        const auto* pushButton = qobject_cast<const QPushButton*>(widget);
        const bool disclosure = qobject_cast<const QComboBox*>(widget) ||
            (toolButton && toolButton->menu()) || (pushButton && pushButton->menu());
        if (element == PE_FrameFocusRect && disclosure && option) {
            // Opening a list with the mouse should not leave a selected ring.
            // Keep a quiet focus cue for users moving through controls with Tab.
            if (option->state & State_KeyboardFocusChange) {
                painter->save();
                painter->setPen(QPen(option->palette.color(QPalette::WindowText), 1,
                                     Qt::DotLine));
                painter->setBrush(Qt::NoBrush);
                painter->drawRect(option->rect.adjusted(1, 1, -2, -2));
                painter->restore();
            }
            return;
        }
        if (element == PE_IndicatorCheckBox && option) {
            drawToggle(option, painter);
            return;
        }
        if (!option || (element != PE_IndicatorArrowLeft &&
                        element != PE_IndicatorArrowRight &&
                        element != PE_IndicatorArrowUp &&
                        element != PE_IndicatorArrowDown)) {
            QProxyStyle::drawPrimitive(element, option, painter, widget);
            return;
        }
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->translate(QRectF(option->rect).center());
        if (element == PE_IndicatorArrowLeft) painter->rotate(180);
        else if (element == PE_IndicatorArrowUp) painter->rotate(-90);
        else if (element == PE_IndicatorArrowDown) painter->rotate(90);
        const auto group = option->state & State_Enabled ? QPalette::Active : QPalette::Disabled;
        painter->setPen(QPen(option->palette.color(group, QPalette::WindowText),
                             1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->setBrush(Qt::NoBrush);
        QPainterPath chevron;
        chevron.moveTo(-2, -3.5);
        chevron.lineTo(1.5, 0);
        chevron.lineTo(-2, 3.5);
        painter->drawPath(chevron);
        painter->restore();
    }

private:
    static QColor mix(const QColor& a, const QColor& b, qreal amount) {
        return QColor::fromRgbF(
            a.redF() * (1.0 - amount) + b.redF() * amount,
            a.greenF() * (1.0 - amount) + b.greenF() * amount,
            a.blueF() * (1.0 - amount) + b.blueF() * amount);
    }

    static void drawToggle(const QStyleOption* option, QPainter* painter) {
        const bool enabled = option->state & State_Enabled;
        const bool checked = option->state & State_On;
        const bool partial = option->state & State_NoChange;
        const bool hovered = enabled && (option->state & State_MouseOver);
        const auto& palette = option->palette;
        const QColor background = palette.color(QPalette::Window);
        const QColor surface = palette.color(QPalette::Button);
        const QColor accent = palette.color(QPalette::Highlight);
        const QColor ink = palette.color(QPalette::WindowText);
        const QColor white(255, 255, 255);
        QColor track = checked ? mix(accent, surface, 0.12)
                               : mix(surface, ink, 0.12);
        if (partial) track = mix(accent, surface, 0.45);
        if (hovered) track = mix(track, white, 0.08);
        if (!enabled) track = mix(track, background, 0.58);

        const QRectF bounds = QRectF(option->rect);
        const QRectF rail(bounds.left() + 1.0, bounds.center().y() - 8.0,
                          32.0, 16.0);
        const qreal thumbX = partial ? rail.center().x() - 7.0
                                    : checked ? rail.right() - 15.0
                                              : rail.left() + 1.0;
        const QRectF thumb(thumbX, rail.top() + 1.0, 14.0, 14.0);

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(QPen(mix(track, ink, enabled ? 0.18 : 0.08), 0.8));
        painter->setBrush(track);
        painter->drawRoundedRect(rail, 8.0, 8.0);

        QColor shadowColor = QColor(0, 0, 0, enabled ? 75 : 28);
        QRadialGradient shadow(thumb.center() + QPointF(0.2, 1.2), 9.0);
        shadow.setColorAt(0.0, shadowColor);
        shadowColor.setAlpha(0);
        shadow.setColorAt(1.0, shadowColor);
        painter->setPen(Qt::NoPen);
        painter->setBrush(shadow);
        painter->drawEllipse(thumb.adjusted(-2.0, -1.0, 2.0, 2.0));

        QColor glass = checked ? mix(white, accent, 0.42)
                               : mix(surface, ink, 0.28);
        if (!enabled) glass = mix(glass, background, 0.5);
        QLinearGradient fill(thumb.topLeft(), thumb.bottomLeft());
        fill.setColorAt(0.0, mix(glass, white, 0.55));
        fill.setColorAt(0.48, mix(glass, white, 0.16));
        fill.setColorAt(1.0, mix(glass, ink, 0.08));
        painter->setPen(QPen(mix(glass, ink, 0.14), 0.7));
        painter->setBrush(fill);
        painter->drawEllipse(thumb);

        QRadialGradient glint(thumb.topLeft() + QPointF(4.0, 3.0), 7.0);
        glint.setColorAt(0.0, QColor(255, 255, 255, enabled ? 170 : 75));
        glint.setColorAt(1.0, QColor(255, 255, 255, 0));
        painter->setPen(Qt::NoPen);
        painter->setBrush(glint);
        painter->drawEllipse(thumb.adjusted(1.0, 0.5, -1.0, -0.5));

        if (option->state & State_HasFocus) {
            QColor ring = accent;
            ring.setAlpha(enabled ? 190 : 80);
            painter->setPen(QPen(ring, 1.5));
            painter->setBrush(Qt::NoBrush);
            painter->drawRoundedRect(rail.adjusted(-0.5, -0.5, 0.5, 0.5),
                                     8.5, 8.5);
        }
        painter->restore();
    }
};
