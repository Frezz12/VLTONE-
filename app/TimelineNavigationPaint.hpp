#pragma once

#include "Theme.hpp"
#include <QPainter>
#include <QPainterPath>

namespace ui::navigation {

inline QColor railBase() {
    return mixColors(th().panelBottom(), th().well(), .65);
}

inline void setWellDepth(QGradient& depth) {
    const auto base = mixColors(railBase(), th().well(), .45);
    depth.setColorAt(0, mixColors(base, th().edgeDark(base), .35));
    depth.setColorAt(.4, base);
    depth.setColorAt(1, mixColors(base, th().edgeLight(base), .25));
}

/// The controls continue into the roller; only a free leading end is rounded.
inline void paintRailSurface(QPainter& p, const QRectF& rect,
                              Qt::Orientation axis, bool roundStart = true) {
    p.fillRect(rect, railBase());
    QPainterPath well;
    const double start = roundStart ? 2. : -9.;
    well.addRoundedRect(axis == Qt::Horizontal ? rect.adjusted(start, 2, 9, -2)
                                               : rect.adjusted(2, start, -2, 9), 5, 5);
    QLinearGradient depth(rect.topLeft(), axis == Qt::Horizontal ? rect.bottomLeft() : rect.topRight());
    setWellDepth(depth);
    p.fillPath(well, depth);
}

} // namespace ui::navigation
