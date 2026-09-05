#include "WaveformPaint.hpp"

#include <QImage>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <vector>

namespace ui {

void paintPeaks(QPainter& p, const daw::WaveformPeaks* peaks, const QRectF& area,
                const PeakPaint& how) {
    if (area.height() < 6.0 || area.width() < 2.0) return;
    const double left = std::max(area.left(), how.clipLeft);
    const double right = std::min(area.right(), how.clipRight);
    if (right <= left) return;

    // The zero-amplitude axis is part of the waveform, not empty decoration.
    // Draw it first so silent clips and gaps remain visible, while real signal
    // grows seamlessly above and below the same one-pixel line.
    const double mid = area.center().y();
    p.setPen(QPen(how.color, 1.0));
    p.drawLine(QPointF(left, mid), QPointF(right, mid));

    if (!peaks || !peaks->isValid() || peaks->bucketCount() == 0) return;
    if (!(how.secondsPerPixel > 0.0)) return;

    const double halfHeight = area.height() / 2.0 - 1.0;

    const double secondsPerPixel = how.secondsPerPixel;
    const std::vector<float>* minima = &peaks->minima;
    const std::vector<float>* maxima = &peaks->maxima;
    double bps = peaks->bucketsPerSecond;
    for (const auto& level : peaks->levels) {
        if (secondsPerPixel * level.bucketsPerSecond < 1.0) break;
        minima = &level.minima;
        maxima = &level.maxima;
        bps = level.bucketsPerSecond;
    }
    const size_t bucketCount = minima->size();

    // Build the upper (maxima) and lower (minima) edges of the envelope, one
    // sample per screen pixel. When the pixel spans several buckets we take the
    // extremes over them; when a bucket is wider than a pixel (zoomed in) we
    // linearly interpolate between neighbouring buckets, so the outline stays a
    // smooth continuous wave instead of stair-stepped blocks.
    std::vector<QPointF> topEdge;
    std::vector<QPointF> bottomEdge;
    const int x0 = int(std::floor(left));
    const int x1 = int(std::ceil(right));
    topEdge.reserve(size_t(std::max(0, x1 - x0 + 1)));
    bottomEdge.reserve(topEdge.capacity());

    for (int xi = x0; xi <= x1; ++xi) {
        const double x = double(xi);
        const double sourceSeconds =
            how.reversed
                ? peaks->durationSeconds - how.sourceStartSeconds -
                      (x - area.left()) * secondsPerPixel
                : how.sourceStartSeconds +
                      (x - area.left()) * secondsPerPixel;
        const double adjacentSeconds =
            sourceSeconds + (how.reversed ? -secondsPerPixel : secondsPerPixel);
        if (std::max(sourceSeconds, adjacentSeconds) < 0.0 ||
            std::min(sourceSeconds, adjacentSeconds) > peaks->durationSeconds)
            continue;

        const double bStart = std::min(sourceSeconds, adjacentSeconds) * bps;
        const double bEnd = std::max(sourceSeconds, adjacentSeconds) * bps;

        double hi;
        double lo;
        if (bEnd - bStart >= 1.0) {
            // Zoomed out: this pixel covers a run of buckets — take the extremes.
            size_t f = std::min(bucketCount - 1, size_t(std::max(0.0, bStart)));
            size_t l = std::min(bucketCount, size_t(std::ceil(bEnd)));
            l = std::max(l, f + 1);
            float loF = 0.0f;
            float hiF = 0.0f;
            for (size_t b = f; b < l; ++b) {
                loF = std::min(loF, (*minima)[b]);
                hiF = std::max(hiF, (*maxima)[b]);
            }
            lo = loF;
            hi = hiF;
        } else {
            // Zoomed in: interpolate between the two nearest buckets.
            const double bp = std::max(0.0, bStart);
            const size_t b0 = std::min(bucketCount - 1, size_t(bp));
            const size_t b1 = std::min(bucketCount - 1, b0 + 1);
            const double frac = bp - double(b0);
            hi = (*maxima)[b0] + ((*maxima)[b1] - (*maxima)[b0]) * frac;
            lo = (*minima)[b0] + ((*minima)[b1] - (*minima)[b0]) * frac;
        }

        // The wave's height follows the gain, so dragging a clip's gain handle
        // visibly swells or shrinks it. Clamped so it never overflows the area.
        const double g = std::clamp(double(how.gain), 0.0, 8.0);
        const double top = mid - std::clamp(hi * g, -1.0, 1.0) * halfHeight;
        const double bottom = mid - std::clamp(lo * g, -1.0, 1.0) * halfHeight;
        topEdge.emplace_back(x, top);
        bottomEdge.emplace_back(x, std::max(bottom, top));
    }
    if (topEdge.size() < 2) return;

    // One closed shape: forward along the top edge, back along the bottom.
    QPainterPath path;
    path.moveTo(topEdge.front());
    for (size_t i = 1; i < topEdge.size(); ++i) path.lineTo(topEdge[i]);
    for (size_t i = bottomEdge.size(); i-- > 0;) path.lineTo(bottomEdge[i]);
    path.closeSubpath();

    p.setPen(Qt::NoPen);
    p.setBrush(how.color);
    p.drawPath(path);
}

bool checkWaveformBaselineForTest() {
    QImage image(24, 12, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    PeakPaint how;
    how.secondsPerPixel = 0.01;
    how.clipLeft = 2.0;
    how.clipRight = 22.0;
    how.color = Qt::white;
    paintPeaks(painter, nullptr, QRectF(0.0, 0.0, 24.0, 12.0), how);
    painter.end();
    return qAlpha(image.pixel(12, 6)) > 0 &&
           qAlpha(image.pixel(12, 2)) == 0;
}

} // namespace ui
