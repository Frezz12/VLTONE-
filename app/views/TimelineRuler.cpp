#include "TimelineRuler.h"

#include <QPainter>
#include <QWheelEvent>

#include <cmath>

using namespace daw::time;

TimelineRuler::TimelineRuler(QWidget* parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFixedHeight(28);
}

void TimelineRuler::setTempoMap(const daw::time::TempoMap& map)
{
    tempoMap_ = map;
    update();
}

void TimelineRuler::setZoom(double samplesPerPixel)
{
    samplesPerPixel_ = std::max(1.0, samplesPerPixel);
    update();
}

void TimelineRuler::setScrollOffset(double samples)
{
    scrollOffset_ = std::max(0.0, samples);
    update();
}

void TimelineRuler::setSampleRate(double rate)
{
    sampleRate_ = rate;
    update();
}

QSize TimelineRuler::sizeHint() const { return QSize(400, 28); }
QSize TimelineRuler::minimumSizeHint() const { return QSize(100, 28); }

void TimelineRuler::paintEvent(QPaintEvent*)
{
    if (sampleRate_ <= 0)
        return;

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);

    const int w = width();
    const int h = height();

    p.fillRect(rect(), QColor(38, 40, 43));

    const double sampleToPx = 1.0 / samplesPerPixel_;
    const std::int64_t firstSample = static_cast<std::int64_t>(scrollOffset_);
    const std::int64_t lastSample = firstSample + static_cast<std::int64_t>(w * samplesPerPixel_);

    const Tick firstTick = tempoMap_.sampleToTick(firstSample);
    const Tick lastTick = tempoMap_.sampleToTick(lastSample);

    if (firstTick < 0 || lastTick < 0)
        return;

    const auto firstBb = tempoMap_.tickToBarBeat(firstTick);
    const auto lastBb = tempoMap_.tickToBarBeat(lastTick);

    // Сколько пикселей между тактами — выбираем шаг для меток
    const Tick tickPerBar = tempoMap_.ticksPerBarAt(firstTick);
    if (tickPerBar <= 0)
        return;

    const double barPx = (tempoMap_.tickToSample(firstTick + tickPerBar)
                        - tempoMap_.tickToSample(firstTick)) * sampleToPx;

    int barStep = 1;
    if (barPx < 20.0) {
        barStep = static_cast<int>(std::ceil(20.0 / barPx));
        if (barStep < 1) barStep = 1;
    }

    // Первый такт, который виден
    Tick tick = tempoMap_.barBeatToTick(firstBb.bar, 1, 0);
    if (tick < firstTick)
        tick += tickPerBar;
    int bar = static_cast<int>((tick - tempoMap_.barBeatToTick(1, 1, 0)) / tickPerBar) + 1;

    while (tick <= lastTick) {
        const std::int64_t samplePos = tempoMap_.tickToSample(tick);
        const double px = (static_cast<double>(samplePos) - scrollOffset_) * sampleToPx;

        if (px >= -5 && px < w + 5) {
            if ((bar - firstBb.bar) % barStep == 0) {
                p.setPen(QColor(120, 125, 132));
                p.drawText(QPointF(px + 3, h - 8), QString::number(bar));

                p.setPen(QColor(55, 57, 62));
                p.drawLine(QPointF(px, 0), QPointF(px, h));
            } else {
                p.setPen(QColor(45, 47, 52));
                p.drawLine(QPointF(px, h - 8), QPointF(px, h));
            }
        }

        tick += tickPerBar;
        ++bar;
    }
}

void TimelineRuler::wheelEvent(QWheelEvent* event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        const double factor = (event->angleDelta().y() > 0) ? 1.0 / 1.15 : 1.15;
        double newZoom = samplesPerPixel_ * factor;
        newZoom = std::clamp(newZoom, 1.0, 100000.0);
        if (newZoom != samplesPerPixel_) {
            setZoom(newZoom);
            emit zoomChanged(samplesPerPixel_);
        }
        event->accept();
    } else {
        QWidget::wheelEvent(event);
    }
}
