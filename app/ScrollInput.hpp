#pragma once
#include <QWheelEvent>
#include <cmath>

namespace ui {
inline QPointF scrollPixels(const QWheelEvent& event) {
    if (!event.pixelDelta().isNull()) return event.pixelDelta();
    // Preserve the mouse-wheel distances: 30 px sideways / 60 px vertically
    // per notch, including fractional notches from high-resolution wheels.
    return {event.angleDelta().x() / 4.0, event.angleDelta().y() / 2.0};
}
inline double wheelZoomFactor(const QWheelEvent& event) {
    return event.pixelDelta().isNull()
        ? std::pow(1.15, event.angleDelta().y() / 120.0)
        : std::pow(1.0015, event.pixelDelta().y());
}
inline int wholeScrollPixels(double delta, double& remainder) {
    remainder += delta;
    const int pixels = int(std::trunc(remainder));
    remainder -= pixels;
    return pixels;
}
} // namespace ui
