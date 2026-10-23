// SPDX-License-Identifier: MIT
#pragma once
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QPainter>
#include <QPainterPath>
#include <QStyleOptionButton>
#include <QStyleOptionSpinBox>

// Qt stylesheets can suppress native arrows/indicators on some platforms. Draw
// these small affordances explicitly in the DEMO only, using the local palette.
// Keyboard input, focus, accessibility and hit testing remain Qt's responsibility.
inline void drawDemoArrow(QPainter& painter, QPointF centre, bool up) {
    const double sign = up ? -1.0 : 1.0;
    QPainterPath arrow;
    arrow.moveTo(centre + QPointF(-3, -1.5 * sign));
    arrow.lineTo(centre + QPointF(0, 1.5 * sign));
    arrow.lineTo(centre + QPointF(3, -1.5 * sign));
    painter.drawPath(arrow);
}
class DemoChoice final : public QComboBox {
public:
    using QComboBox::QComboBox;
protected:
    void paintEvent(QPaintEvent* event) override {
        QComboBox::paintEvent(event);
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(palette().color(QPalette::Text), 1.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        drawDemoArrow(painter, QPointF(width() - 14, height() / 2.0), false);
    }
};
class DemoNumber final : public QDoubleSpinBox {
public:
    using QDoubleSpinBox::QDoubleSpinBox;
protected:
    void paintEvent(QPaintEvent* event) override {
        QDoubleSpinBox::paintEvent(event);
        QStyleOptionSpinBox options; initStyleOption(&options);
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(palette().color(QPalette::Text), 1.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        drawDemoArrow(painter, style()->subControlRect(QStyle::CC_SpinBox, &options, QStyle::SC_SpinBoxUp, this).center(), true);
        drawDemoArrow(painter, style()->subControlRect(QStyle::CC_SpinBox, &options, QStyle::SC_SpinBoxDown, this).center(), false);
    }
};
class DemoCheck final : public QCheckBox {
public:
    using QCheckBox::QCheckBox;
protected:
    void paintEvent(QPaintEvent* event) override {
        QCheckBox::paintEvent(event);
        QStyleOptionButton options; initStyleOption(&options);
        const auto box = QRectF(style()->subElementRect(QStyle::SE_CheckBoxIndicator, &options, this)).adjusted(.5, .5, -.5, -.5);
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
        const auto colors = window()->palette();
        painter.setPen(QPen(colors.color(QPalette::Mid), 1));
        painter.setBrush(colors.color(QPalette::Base));
        painter.drawRoundedRect(box, 2, 2);
        if (isChecked()) {
            painter.setPen(QPen(colors.color(QPalette::Text), 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            QPainterPath tick;
            tick.moveTo(box.left() + 3, box.center().y());
            tick.lineTo(box.center().x() - 1, box.bottom() - 3);
            tick.lineTo(box.right() - 3, box.top() + 3);
            painter.drawPath(tick);
        }
    }
};
