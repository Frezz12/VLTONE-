#include "WaveformPaint.hpp"
#include "graphics/SceneRecordingTag.hpp"

#include <QImage>
#include <QPainter>
#include <algorithm>
#include <array>
#include <cmath>
#include <list>
#include <limits>
#include <unordered_map>

namespace ui {
namespace {
constexpr int kTilePixels = 256;
// Geometry is cheap to clip on the GPU. Larger vector tiles keep scene-node
// and stencil submissions bounded without changing the CPU raster tile size.
constexpr int kGeometryTilePixels = 1024;
constexpr std::size_t kRasterBudget = 64 * 1024 * 1024;
struct RasterKey {
    std::uint64_t source;
    std::array<double, 6> values;
    qint64 tile;
    QRgb color;
    bool reversed;
    bool operator==(const RasterKey&) const = default;
};
struct RasterHash {
    std::size_t operator()(const RasterKey& key) const {
        std::size_t hash = std::hash<std::uint64_t>{}(key.source);
        const auto add = [&](std::size_t v) { hash ^= v + 0x9e3779b9 + (hash << 6) + (hash >> 2); };
        for (double v : key.values) add(std::hash<double>{}(v));
        add(std::hash<qint64>{}(key.tile)); add(key.color); add(key.reversed);
        return hash;
    }
};
std::size_t assetBytes(const QImage& image) { return std::size_t(image.sizeInBytes()); }
std::size_t assetBytes(const QVector<graphics::SceneVertex>& vertices) {
    return std::size_t(vertices.size()) * sizeof(graphics::SceneVertex);
}
template<class Asset> struct TileCache {
    struct Entry { RasterKey key; Asset image; std::size_t bytes; };
    std::list<Entry> entries;
    std::unordered_map<RasterKey, typename std::list<Entry>::iterator, RasterHash> index;
    WaveformPaintStats stats;
    const Asset* find(const RasterKey& key) {
        const auto found = index.find(key);
        if (found == index.end()) return nullptr;
        entries.splice(entries.begin(), entries, found->second);
        ++stats.tileHits;
        return &found->second->image;
    }
    void insert(const RasterKey& key, const Asset& image) {
        const auto bytes = assetBytes(image) + sizeof(Entry);
        if (bytes > kRasterBudget || !assetBytes(image)) return;
        while (!entries.empty() && (stats.bytes + bytes > kRasterBudget || entries.size() >= 2048)) {
            stats.bytes -= entries.back().bytes;
            index.erase(entries.back().key); entries.pop_back();
        }
        entries.push_front({key, image, bytes});
        index.emplace(key, entries.begin()); stats.bytes += bytes;
    }
};
thread_local TileCache<QImage> rasterCache;
thread_local TileCache<QVector<graphics::SceneVertex>> geometryCache;

// Average coverage of a straight envelope edge across one physical pixel.
// Integrating the clipped line preserves antialiasing without sending a many-
// thousand-vertex filled polygon through QPainter's general path rasterizer.
double edgeCoverage(double a, double b) {
    if (a >= 1.0 && b >= 1.0) return 1.0;
    if (a <= 0.0 && b <= 0.0) return 0.0;
    if (std::abs(a - b) < 1e-9) return std::clamp((a + b) * 0.5, 0.0, 1.0);
    const auto integral = [](double v) {
        return v <= 0.0 ? 0.0 : v >= 1.0 ? v - 0.5 : v * v * 0.5;
    };
    return (integral(b) - integral(a)) / (b - a);
}

struct PeakSource {
    std::span<const float> minima, maxima;
    std::span<const daw::WaveformPeaks::Level> levels;
    double bucketsPerSecond;
    bool magnitudes = false;
};
QVector<graphics::SceneVertex> makeGeometryTile(const PeakSource& source, double duration, const RasterKey& key) {
    const auto [offset, secondsPerPixel, gain, height, dpr, reverseDuration] = key.values;
    (void)reverseDuration;
    auto minima = source.minima, maxima = source.maxima;
    double bps = source.bucketsPerSecond;
    const double step = secondsPerPixel / dpr;
    for (const auto& level : source.levels) {
        if (step * level.bucketsPerSecond < 1.) break;
        minima = level.minima; maxima = level.maxima; bps = level.bucketsPerSecond;
    }
    const auto count = std::min(minima.size(), maxima.size());
    if (!count) return {};
    const double half = std::max(0., height * .5 - 1.);
    const auto edge = [&](double pixel) {
        double at = offset + pixel * step;
        if (key.reversed) at = duration - at;
        double lo = 0, hi = 0;
        if (at >= 0 && at <= duration) {
            const double bucket = std::clamp(at * bps, 0., double(count - 1));
            const auto a = std::size_t(bucket), b = std::min(count - 1, a + 1);
            const double mix = bucket - a;
            lo = minima[a] * (1 - mix) + minima[b] * mix;
            hi = maxima[a] * (1 - mix) + maxima[b] * mix;
            const double nextAt = at + (key.reversed ? -step : step);
            const auto other = std::size_t(std::clamp(nextAt * bps, 0., double(count - 1)));
            for (auto i = std::min(a, other); i <= std::max(a, other); ++i) {
                lo = std::min(lo, double(minima[i])); hi = std::max(hi, double(maxima[i]));
            }
            if (source.magnitudes) lo = -hi;
        }
        return std::pair{float(height * .5 - std::clamp(hi * gain, -1., 1.) * half),
                         float(height * .5 - std::clamp(lo * gain, -1., 1.) * half)};
    };
    QVector<graphics::SceneVertex> triangles;
    triangles.reserve(kGeometryTilePixels * 6);
    auto previous = edge(double(key.tile) * kGeometryTilePixels);
    for (int pixel = 0; pixel < kGeometryTilePixels; ++pixel) {
        const auto next = edge(double(key.tile) * kGeometryTilePixels + pixel + 1);
        const auto x = float(pixel / dpr), nextX = float((pixel + 1) / dpr);
        triangles << graphics::SceneVertex{x, previous.first, 0, 0} << graphics::SceneVertex{x, previous.second, 0, 0}
                  << graphics::SceneVertex{nextX, next.first, 0, 0} << graphics::SceneVertex{nextX, next.first, 0, 0}
                  << graphics::SceneVertex{x, previous.second, 0, 0} << graphics::SceneVertex{nextX, next.second, 0, 0};
        previous = next;
    }
    ++geometryCache.stats.tileBuilds;
    return triangles;
}
QImage makeTile(const PeakSource& peaks, double duration, const RasterKey& key) {
    const auto [offset, secondsPerPixel, gain, height, dpr, reverseDuration] = key.values;
    (void)reverseDuration;
    const int pixelHeight = int(std::ceil(height * dpr));
    QImage image(kTilePixels, pixelHeight, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr); image.fill(Qt::transparent);
    if (image.isNull()) return image;
    auto minima = peaks.minima;
    auto maxima = peaks.maxima;
    double bps = peaks.bucketsPerSecond;
    const double step = secondsPerPixel / dpr;
    for (const auto& level : peaks.levels) {
        if (step * level.bucketsPerSecond < 1.0) break;
        minima = level.minima; maxima = level.maxima; bps = level.bucketsPerSecond;
    }
    const auto count = std::min(minima.size(), maxima.size());
    if (!count) return image;
    const double mid = height * dpr * 0.5;
    const double halfHeight = (height * 0.5 - 1.0) * dpr;
    const auto edge = [&](double pixel) {
        const double at = offset + pixel * step;
        const double seconds = key.reversed ? duration - at : at;
        const double next = seconds + (key.reversed ? -step : step);
        double lo = 0.0, hi = 0.0;
        if (std::max(seconds, next) >= 0.0 && std::min(seconds, next) <= duration) {
            const double start = std::max(0.0, std::min(seconds, next) * bps);
            const double end = std::max(seconds, next) * bps;
            if (end - start >= 1.0) {
                const auto first = std::min(count - 1, std::size_t(start));
                const auto last = std::max(first + 1, std::min(count, std::size_t(std::max(0.0, std::ceil(end)))));
                for (auto i = first; i < last; ++i) {
                    lo = std::min(lo, double(peaks.magnitudes ? -minima[i] : minima[i])); hi = std::max(hi, double(maxima[i]));
                }
            } else {
                const auto first = std::min(count - 1, std::size_t(start));
                const auto nextBucket = std::min(count - 1, first + 1);
                const double fraction = std::clamp(start - double(first), 0.0, 1.0);
                lo = (minima[first] + (minima[nextBucket] - minima[first]) * fraction) * (peaks.magnitudes ? -1.0 : 1.0);
                hi = maxima[first] + (maxima[nextBucket] - maxima[first]) * fraction;
            }
        }
        const double top = mid - std::clamp(hi * gain, -1.0, 1.0) * halfHeight;
        return std::pair{top, std::max(top, mid - std::clamp(lo * gain, -1.0, 1.0) * halfHeight)};
    };
    const double startPixel = double(key.tile) * kTilePixels;
    auto previous = edge(startPixel);
    const QRgb opaque = qPremultiply(key.color);
    for (int x = 0; x < kTilePixels; ++x) {
        const auto next = edge(startPixel + x + 1.0);
        const int first = std::clamp(int(std::floor(std::min(previous.first, next.first))), 0, pixelHeight);
        const int last = std::clamp(int(std::ceil(std::max(previous.second, next.second))), 0, pixelHeight);
        for (int y = first; y < last; ++y) {
            const double coverage = std::clamp(
                edgeCoverage(previous.second - y, next.second - y) -
                edgeCoverage(previous.first - y, next.first - y), 0.0, 1.0);
            const int alpha = int(std::lround(qAlpha(key.color) * coverage));
            reinterpret_cast<QRgb*>(image.scanLine(y))[x] = coverage >= 1.0 ? opaque
                : qPremultiply(qRgba(qRed(key.color), qGreen(key.color), qBlue(key.color), alpha));
        }
        previous = next;
    }
    ++rasterCache.stats.tileBuilds;
    return image;
}
} // namespace

WaveformPaintStats waveformPaintStatsForTest() { return rasterCache.stats; }
WaveformPaintStats waveformGeometryStatsForTest() { return geometryCache.stats; }
void resetWaveformPaintCacheForTest() { rasterCache = {}; geometryCache = {}; }

static void paintEnvelope(QPainter& p, const PeakSource& source, std::uint64_t id,
                          double duration, double stableUntil,
                          const QRectF& area, const PeakPaint& how) {
    if (area.height() < 6.0 || area.width() < 2.0) return;
    double left = std::max(area.left(), how.clipLeft);
    double right = std::min(area.right(), how.clipRight);
    if (p.hasClipping()) {
        const QRectF clip = p.clipBoundingRect();
        if (area.bottom() < clip.top() || area.top() > clip.bottom()) return;
        left = std::max(left, clip.left()); right = std::min(right, clip.right());
    }
    if (!(right > left)) return;
    p.setPen(QPen(how.color, 1.0));
    p.drawLine(QPointF(left, area.center().y()), QPointF(right, area.center().y()));
    if (source.minima.empty() || !(source.bucketsPerSecond > 0.0) || !(how.secondsPerPixel > 0.0)) return;
    const double dpr = p.device() ? p.device()->devicePixelRatioF() : 1.0;
    if (!std::isfinite(how.sourceStartSeconds) || !std::isfinite(how.secondsPerPixel) ||
        !std::isfinite(area.height()) || !std::isfinite(how.gain) ||
        !std::isfinite(source.bucketsPerSecond) || !std::isfinite(duration) ||
        !std::isfinite(dpr) || !(dpr > 0.0) || area.height() * dpr > 32768) return;
    if (auto* scene = graphics::sceneGeometrySink(p)) {
        const double tileWidth = kGeometryTilePixels / dpr;
        const auto first = qint64(std::floor((left - area.left()) / tileWidth));
        const auto last = qint64(std::ceil((right - area.left()) / tileWidth));
        p.save();
        p.setClipRect(QRectF(left, area.top(), right - left, area.height()), Qt::IntersectClip);
        for (auto tile = first; tile < last; ++tile) {
            const double readsUntil = how.sourceStartSeconds +
                ((tile + 1) * kGeometryTilePixels + 1.) * how.secondsPerPixel / dpr;
            const bool cacheable = id && readsUntil < stableUntil;
            const RasterKey key{id,
                {how.sourceStartSeconds, how.secondsPerPixel, std::clamp(double(how.gain), 0., 8.),
                 area.height(), dpr, how.reversed ? duration : 0.}, tile, how.color.rgba(), how.reversed};
            const QPointF at(area.left() + tile * tileWidth, area.top());
            if (cacheable) if (const auto* cached = geometryCache.find(key)) {
                scene->appendLocalGeometry(*cached, at, how.color);
                continue;
            }
            const auto vertices = makeGeometryTile(source, duration, key);
            if (cacheable) geometryCache.insert(key, vertices);
            scene->appendLocalGeometry(vertices, at, how.color);
        }
        p.restore();
        return;
    }
    const double tileWidth = kTilePixels / dpr;
    const auto first = qint64(std::floor((left - area.left()) / tileWidth));
    const auto last = qint64(std::ceil((right - area.left()) / tileWidth));
    p.save();
    p.setClipRect(QRectF(left, area.top(), right - left, area.height()), Qt::IntersectClip);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    for (auto tile = first; tile < last; ++tile) {
        const double readsUntil = how.sourceStartSeconds +
            ((tile + 1) * kTilePixels + 1.0) * how.secondsPerPixel / dpr;
        const bool cacheable = id && readsUntil < stableUntil;
        const RasterKey key{id,
            {how.sourceStartSeconds, how.secondsPerPixel, std::clamp(double(how.gain), 0.0, 8.0),
             area.height(), dpr, how.reversed ? duration : 0.0}, tile, how.color.rgba(), how.reversed};
        const QPointF at(area.left() + tile * tileWidth, area.top());
        if (cacheable) {
            if (const auto* cached = rasterCache.find(key)) { p.drawImage(at, *cached); continue; }
        }
        const QImage image = makeTile(source, duration, key);
        if (cacheable) rasterCache.insert(key, image);
        p.drawImage(at, image);
    }
    p.restore();
}

void paintPeaks(QPainter& p, const daw::WaveformPeaks* peaks, const QRectF& area,
                const PeakPaint& how) {
    const PeakSource source = peaks ? PeakSource{peaks->minima, peaks->maxima,
        peaks->levels, peaks->bucketsPerSecond} : PeakSource{{}, {}, {}, 0.0};
    paintEnvelope(p, source, peaks ? peaks->geometryId : 0,
        peaks ? peaks->durationSeconds : 0.0, std::numeric_limits<double>::infinity(), area, how);
}
void paintRecordingPeaks(QPainter& p, std::span<const float> envelope,
                         double bucketSeconds, std::uint64_t id,
                         const QRectF& area, const PeakPaint& how) {
    if (!(bucketSeconds > 0.0)) return;
    const PeakSource source{envelope, envelope, {}, 1.0 / bucketSeconds, true};
    const double stableUntil = std::max(0.0, double(envelope.size()) - 2.0) * bucketSeconds;
    paintEnvelope(p, source, id, envelope.size() * bucketSeconds, stableUntil, area, how);
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
    if (!(qAlpha(image.pixel(12, 6)) > 0 && qAlpha(image.pixel(12, 2)) == 0)) return false;
    daw::engine::SampleBuffer samples(1, 960, 48000.0);
    for (std::size_t i = 0; i < samples.frames(); ++i)
        samples.writableChannel(0)[i] = float(std::sin(double(i) * 0.19));
    daw::WaveformPeaks peaks;
    daw::buildPeaks(samples, peaks);
    const auto id = peaks.geometryId;
    if (!id) return false;
    const auto render = [&](bool cached, double left, float gain, bool reverse) {
        QImage result(144, 64, QImage::Format_ARGB32_Premultiplied);
        result.fill(Qt::transparent);
        QPainter paint(&result);
        paint.setRenderHint(QPainter::Antialiasing);
        peaks.geometryId = cached ? id : 0;
        how.clipLeft = 0; how.clipRight = 144;
        how.secondsPerPixel = 0.0003; how.gain = gain; how.reversed = reverse;
        paintPeaks(paint, &peaks, QRectF(left, 3.25, 130, 54), how);
        return result;
    };
    for (bool reversed : {false, true})
        for (float gain : {0.5f, 1.0f, 2.0f})
            for (double left : {1.25, 5.25, -4.5}) {
                const auto reference = render(false, left, gain, reversed);
                if (render(true, left, gain, reversed) != reference ||
                    render(true, left, gain, reversed) != reference) return false;
            }
    daw::WaveformPeaks second;
    daw::buildPeaks(samples, second);
    return second.geometryId && second.geometryId != id;
}

} // namespace ui
