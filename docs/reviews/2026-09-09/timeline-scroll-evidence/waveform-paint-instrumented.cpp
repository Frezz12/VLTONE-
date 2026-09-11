#include "WaveformPaint.hpp"

#include <QImage>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <vector>
#include <array>
#include <list>
#include <unordered_map>

namespace ui {
bool skipRaster=false;
namespace {
struct GeometryKey {
    std::uint64_t source;
    std::array<double, 7> values;
    bool reversed;
    bool operator==(const GeometryKey&) const = default;
};
struct GeometryHash {
    std::size_t operator()(const GeometryKey& key) const {
        std::size_t hash = std::hash<std::uint64_t>{}(key.source);
        const auto add = [&](std::size_t value) { hash ^= value + 0x9e3779b9 + (hash << 6) + (hash >> 2); };
        for (double value : key.values) add(std::hash<double>{}(value));
        add(key.reversed);
        return hash;
    }
};
struct GeometryCache {
    struct Entry { GeometryKey key; QPainterPath path; std::size_t bytes; };
    std::list<Entry> entries;
    std::unordered_map<GeometryKey, std::list<Entry>::iterator, GeometryHash> index;
    std::size_t bytes = 0;
    static constexpr std::size_t budget = 16 * 1024 * 1024;
    const QPainterPath* find(const GeometryKey& key) {
        const auto found = index.find(key);
        if (found == index.end()) return nullptr;
        entries.splice(entries.begin(), entries, found->second);
        return &found->second->path;
    }
    void insert(const GeometryKey& key, const QPainterPath& path) {
        const std::size_t cost = sizeof(Entry) + path.elementCount() * sizeof(QPainterPath::Element);
        if (cost > budget) return;
        while (!entries.empty() && (entries.size() >= 256 || bytes + cost > budget)) {
            bytes -= entries.back().bytes;
            index.erase(entries.back().key);
            entries.pop_back();
        }
        entries.push_front({key, path, cost});
        index.emplace(key, entries.begin());
        bytes += cost;
    }
};
thread_local GeometryCache geometryCache;
} // namespace

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

    const int x0 = int(std::floor(left));
    const int x1 = int(std::ceil(right));
    const GeometryKey key{peaks->geometryId,
        {how.sourceStartSeconds, how.secondsPerPixel, double(how.gain), area.height(),
         x0 - area.left(), double(x1 - x0), peaks->durationSeconds}, how.reversed};
    const auto draw = [&](const QPainterPath& path) {
        p.save();
        p.translate(area.topLeft());
        p.setPen(Qt::NoPen);
        p.setBrush(how.color);
        if (!skipRaster) p.drawPath(path);
        p.restore();
    };
    if (key.source) {
        if (const auto* cached = geometryCache.find(key)) { draw(*cached); return; }
    }

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
    thread_local std::vector<QPointF> topEdge;
    thread_local std::vector<QPointF> bottomEdge;
    topEdge.clear(); bottomEdge.clear();
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
        topEdge.emplace_back(x - area.left(), top - area.top());
        bottomEdge.emplace_back(x - area.left(), std::max(bottom, top) - area.top());
    }
    if (topEdge.size() < 2) return;

    // One closed shape: forward along the top edge, back along the bottom.
    thread_local QPainterPath path;
    path.clear();
    path.moveTo(topEdge.front());
    for (size_t i = 1; i < topEdge.size(); ++i) path.lineTo(topEdge[i]);
    for (size_t i = bottomEdge.size(); i-- > 0;) path.lineTo(bottomEdge[i]);
    path.closeSubpath();

    if (key.source) geometryCache.insert(key, path);
    draw(path);
}

} // namespace ui
