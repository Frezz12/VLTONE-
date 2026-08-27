#pragma once

#include <QWidget>

#include "daw/time/TempoMap.h"

class TimelineRuler : public QWidget {
    Q_OBJECT
public:
    explicit TimelineRuler(QWidget* parent = nullptr);

    void setTempoMap(const daw::time::TempoMap& map);
    void setZoom(double samplesPerPixel);
    void setScrollOffset(double samples);
    void setSampleRate(double rate);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent*) override;
    void wheelEvent(QWheelEvent*) override;

signals:
    void zoomChanged(double samplesPerPixel);

private:
    void paintBars(QPainter& p, int width);

    daw::time::TempoMap tempoMap_;
    double samplesPerPixel_ = 1000.0;
    double scrollOffset_ = 0.0;
    double sampleRate_ = 48000.0;
};
