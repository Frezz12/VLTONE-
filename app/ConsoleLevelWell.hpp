#pragma once

#include "Controls.hpp"
#include "Theme.hpp"

#include <QPainter>

namespace ui {

// Shared by mixer channels and the sampler's private FX channel. Model
// bindings belong to the owner; the console material and scale stay identical.
class ConsoleLevelWell : public QWidget {
public:
    using QWidget::QWidget;

protected:
    void paintEvent(QPaintEvent*) override {
        const auto& t = th();
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF face = QRectF(rect()).adjusted(.5, .5, -.5, -.5);
        QLinearGradient recess(face.topLeft(), face.bottomLeft());
        recess.setColorAt(0, t.wellTop());
        recess.setColorAt(1, t.wellBottom());
        QLinearGradient rim(face.topLeft(), face.bottomLeft());
        rim.setColorAt(0, t.edgeDark(t.well()));
        rim.setColorAt(0.18, t.separator());
        rim.setColorAt(1, t.edgeLight(t.surface));
        p.setBrush(recess);
        p.setPen(QPen(QBrush(rim), 1));
        p.drawRoundedRect(face, 4, 4);
    }
};

inline void configureConsoleLevel(FaderWidget* fader, LevelMeter* meter) {
    fader->setWheelEnabled(false);
    fader->setMinimumHeight(60);
    fader->setScaleVisible(true);
    fader->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    meter->setMeterStyle(LevelMeter::Style::Console);
    meter->setMinimumHeight(60);
    meter->setScaleInsets(fader->scaleInsets());
    meter->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
}

} // namespace ui
