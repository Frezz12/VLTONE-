#include "WaveformPaint.hpp"
#include "graphics/SceneRecordingTag.hpp"

#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>
#include <algorithm>
#include <array>
#include <cmath>
#include <list>
#include <limits>
#include <unordered_map>

namespace ui {
namespace {
WaveformStyle& cachedWaveformStyle() {
    static WaveformStyle style = QSettings().value(kWaveformStyleSetting).toInt() ==
            int(WaveformStyle::Smooth) ? WaveformStyle::Smooth : WaveformStyle::Crisp;
    return style;
}

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
    bool sampleDetail;
    WaveformStyle style;
    bool operator==(const RasterKey&) const = default;
};
struct RasterHash {
    std::size_t operator()(const RasterKey& key) const {
        std::size_t hash = std::hash<std::uint64_t>{}(key.source);
        const auto add = [&](std::size_t v) { hash ^= v + 0x9e3779b9 + (hash << 6) + (hash >> 2); };
        for (double v : key.values) add(std::hash<double>{}(v));
        add(std::hash<qint64>{}(key.tile)); add(key.color); add(key.reversed); add(key.sampleDetail);
        add(std::size_t(key.style));
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

// Integrate a sloping edge over a physical pixel without blurring the body.
double edgeCoverage(double a, double b) {
    if (a >= 1.0 && b >= 1.0) return 1.0;
    if (a <= 0.0 && b <= 0.0) return 0.0;
    if (std::abs(a - b) < 1e-9) return std::clamp((a + b) * .5, 0.0, 1.0);
    const auto integral = [](double v) {
        return v <= 0.0 ? 0.0 : v >= 1.0 ? v - .5 : v * v * .5;
    };
    return (integral(b) - integral(a)) / (b - a);
}

struct PeakSource {
    std::span<const float> minima, maxima;
    std::span<const daw::WaveformPeaks::Level> levels;
    double bucketsPerSecond;
    bool magnitudes = false;
    const daw::engine::SampleBuffer* samples = nullptr;
};

// Both renderers use the same source extrema and style at each pixel boundary.
struct PeakColumns {
    PeakSource source;
    double duration, offset, step, gain, height, dpr;
    bool reversed;
    bool smooth;
    bool readSamples;
    int aggregateLevel = -1;
    std::size_t aggregateSpan = 1;

    PeakColumns(PeakSource input, double seconds, const RasterKey& key)
        : source(input), duration(seconds), offset(key.values[0]),
          step(key.values[1] / key.values[4]), gain(key.values[2]),
          height(key.values[3]), dpr(key.values[4]), reversed(key.reversed),
          smooth(key.style == WaveformStyle::Smooth),
          readSamples(input.samples && input.samples->channels() &&
                      step * input.bucketsPerSecond < 1.0) {
        for (const auto& level : input.levels) {
            if (step * level.bucketsPerSecond < 4.0) break;
            ++aggregateLevel;
            aggregateSpan *= 4;
        }
    }

    std::pair<double, double> at(double pixel) const {
        const double mid = height * dpr * .5;
        const std::pair<double, double> silence = smooth ? std::pair{mid, mid} : std::pair{0., 0.};
        const auto count = readSamples ? source.samples->frames()
                                      : std::min(source.minima.size(), source.maxima.size());
        if (!count) return silence;
        const double a = offset + pixel * step, b = a + step;
        const double from = reversed ? duration - b : a;
        const double to = reversed ? duration - a : b;
        if (to <= 0.0 || from >= duration) return silence;
        const double rate = readSamples ? source.samples->sampleRate() : source.bucketsPerSecond;
        const auto first = std::min(count - 1, std::size_t(std::max(0.0, from) * rate));
        const auto last = std::min(count, std::max(first + 1,
            std::size_t(std::ceil(std::min(duration, to) * rate))));
        const auto value = [&](std::size_t frame) {
            double sum = 0.0;
            for (std::size_t ch = 0; ch < source.samples->channels(); ++ch)
                sum += source.samples->channel(ch)[frame];
            return sum / source.samples->channels();
        };
        double lo = readSamples ? value(first) : source.minima[first];
        double hi = readSamples ? lo : source.maxima[first];
        if (smooth && step * rate < 1.0) {
            const auto next = std::min(count - 1, first + 1);
            const double fraction = std::clamp(std::max(0.0, from) * rate - first, 0.0, 1.0);
            lo += ((readSamples ? value(next) : source.minima[next]) - lo) * fraction;
            hi += ((readSamples ? value(next) : source.maxima[next]) - hi) * fraction;
        } else for (auto i = first + 1; i < last;) {
            // Coarse peaks are exact only for complete aligned blocks. Refine
            // either edge instead of borrowing extrema from the next pixel.
            // This keeps the contour identical on both sides of an LOD switch.
            int level = readSamples ? -1 : aggregateLevel;
            std::size_t span = readSamples ? 1 : aggregateSpan;
            while (level >= 0 && (i % span != 0 || span > last - i)) {
                --level;
                span /= 4;
            }
            const double low = readSamples ? value(i) : level < 0 ? source.minima[i]
                : source.levels[level].minima[i / span];
            const double high = readSamples ? low : level < 0 ? source.maxima[i]
                : source.levels[level].maxima[i / span];
            lo = std::min(lo, low);
            hi = std::max(hi, high);
            i += span;
        }
        if (source.magnitudes) lo = -hi;
        if (lo == 0.0 && hi == 0.0) return silence;
        const double scale = std::max(0.0, height * .5 - 1.0) * dpr;
        const double limit = std::ceil(height * dpr);
        const double top = std::clamp(mid - std::clamp(hi * gain, -1.0, 1.0) * scale, 0.0, limit - 1.0);
        const double bottom = std::clamp(mid - std::clamp(lo * gain, -1.0, 1.0) * scale,
                                         top + (smooth ? 0.0 : 1.0), limit);
        return {top, bottom};
    }
};

QVector<graphics::SceneVertex> makeGeometryTile(const PeakSource& source, double duration, const RasterKey& key) {
    const PeakColumns columns(source, duration, key);
    QVector<graphics::SceneVertex> triangles;
    triangles.reserve(kGeometryTilePixels * 6);
    for (int pixel = 0; pixel < kGeometryTilePixels; ++pixel) {
        const auto [top, bottom] = columns.at(double(key.tile) * kGeometryTilePixels + pixel);
        const auto [endTop, endBottom] = columns.smooth
            ? columns.at(double(key.tile) * kGeometryTilePixels + pixel + 1) : std::pair{top, bottom};
        if (top == bottom && endTop == endBottom) continue;
        const auto x = float(pixel / columns.dpr), nextX = float((pixel + 1) / columns.dpr);
        const auto y = float(top / columns.dpr), nextY = float(bottom / columns.dpr);
        const auto endY = float(endTop / columns.dpr), endNextY = float(endBottom / columns.dpr);
        triangles << graphics::SceneVertex{x, y, 0, 0} << graphics::SceneVertex{x, nextY, 0, 0}
                  << graphics::SceneVertex{nextX, endY, 0, 0} << graphics::SceneVertex{nextX, endY, 0, 0}
                  << graphics::SceneVertex{x, nextY, 0, 0} << graphics::SceneVertex{nextX, endNextY, 0, 0};
    }
    ++geometryCache.stats.tileBuilds;
    return triangles;
}

QImage makeTile(const PeakSource& source, double duration, const RasterKey& key) {
    const PeakColumns columns(source, duration, key);
    QImage image(kTilePixels, int(std::ceil(columns.height * columns.dpr)), QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(columns.dpr); image.fill(Qt::transparent);
    if (image.isNull()) return image;
    const QRgb color = qPremultiply(key.color);
    for (int x = 0; x < kTilePixels; ++x) {
        const auto [top, bottom] = columns.at(double(key.tile) * kTilePixels + x);
        const auto [endTop, endBottom] = columns.smooth
            ? columns.at(double(key.tile) * kTilePixels + x + 1) : std::pair{top, bottom};
        for (int y = int(std::floor(std::min(top, endTop))); y < int(std::ceil(std::max(bottom, endBottom))); ++y) {
            const double coverage = columns.smooth
                ? std::clamp(edgeCoverage(bottom - y, endBottom - y) -
                             edgeCoverage(top - y, endTop - y), 0.0, 1.0)
                : std::clamp(std::min(bottom, double(y + 1)) -
                                               std::max(top, double(y)), 0.0, 1.0);
            reinterpret_cast<QRgb*>(image.scanLine(y))[x] = coverage >= 1.0
                ? color : qPremultiply(qRgba(qRed(key.color), qGreen(key.color),
                    qBlue(key.color), int(std::lround(qAlpha(key.color) * coverage))));
        }
    }
    ++rasterCache.stats.tileBuilds;
    return image;
}

// At sample zoom, show the actual held sample values with horizontal treads
// and vertical risers. The caller supplies already decoded audio; painting
// never loads a file or allocates another copy of it.
bool paintSampleTrace(QPainter& p, const QRectF& area, const PeakPaint& how) {
    const auto* samples = how.samples;
    const double dpr = p.device() ? p.device()->devicePixelRatioF() : 1.0;
    if (!samples || !samples->frames() || !samples->channels() ||
        !(how.secondsPerPixel > 0.0) || !std::isfinite(how.sourceStartSeconds) ||
        !std::isfinite(how.gain) || !(samples->sampleRate() > 0.0) ||
        how.secondsPerPixel * samples->sampleRate() / dpr > 1.0) return false;
    QRectF visible = area.intersected(QRectF(how.clipLeft, area.top(), how.clipRight - how.clipLeft, area.height()));
    if (p.hasClipping()) visible = visible.intersected(p.clipBoundingRect());
    if (visible.isEmpty()) return true;
    const double rate = samples->sampleRate();
    const double duration = samples->frames() / rate;
    const auto sourceAt = [&](double x) {
        const double t = how.sourceStartSeconds + (x - area.left()) * how.secondsPerPixel;
        return how.reversed ? duration - t : t;
    };
    const double a = sourceAt(visible.left()), b = sourceAt(visible.right());
    const auto first = std::size_t(std::clamp(std::floor(std::min(a, b) * rate) - 1.0, 0.0, double(samples->frames())));
    const auto last = std::size_t(std::clamp(std::ceil(std::max(a, b) * rate) + 1.0, 0.0, double(samples->frames())));
    p.save();
    p.setClipRect(visible, Qt::IntersectClip);
    const bool smooth = how.style == WaveformStyle::Smooth;
    p.setRenderHint(QPainter::Antialiasing, smooth);
    p.setPen(QPen(how.color, 1.0 / dpr, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin));
    p.drawLine(QPointF(visible.left(), area.center().y()), QPointF(visible.right(), area.center().y()));
    QPainterPath line;
    for (auto frame = first; frame < last; ++frame) {
        double value = 0.0;
        for (std::size_t ch = 0; ch < samples->channels(); ++ch) value += samples->channel(ch)[frame];
        value = std::clamp(value * how.gain / samples->channels(), -1.0, 1.0);
        const double position = area.center().y() - value * (area.height() * .5 - 1.0);
        const double y = smooth ? position : std::round(position * dpr) / dpr;
        const auto x = [&](std::size_t index) {
            const double t = how.reversed ? duration - index / rate : index / rate;
            return area.left() + (t - how.sourceStartSeconds) / how.secondsPerPixel;
        };
        if (frame == first) line.moveTo(x(frame), y);
        else line.lineTo(x(frame), y);
        if (!smooth || frame + 1 == last) line.lineTo(x(frame + 1), y);
    }
    p.setBrush(Qt::NoBrush);
    p.drawPath(line);
    p.restore();
    return true;
}
} // namespace

WaveformStyle waveformStyle() { return cachedWaveformStyle(); }
void setWaveformStyle(WaveformStyle style) {
    cachedWaveformStyle() = style == WaveformStyle::Smooth ? style : WaveformStyle::Crisp;
    QSettings().setValue(kWaveformStyleSetting, int(cachedWaveformStyle()));
}

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
                 area.height(), dpr, how.reversed ? duration : 0.}, tile, how.color.rgba(), how.reversed, source.samples != nullptr, how.style};
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
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    // Snap once so every tile shares the same device-pixel phase, including
    // tiles on opposite sides of the viewport origin during fractional pans.
    const QTransform device = p.deviceTransform();
    const QPointF pixel = device.map(area.topLeft());
    const QPointF origin = device.inverted().map(
        QPointF(std::floor(pixel.x() + .5), std::floor(pixel.y() + .5)));
    for (auto tile = first; tile < last; ++tile) {
        const double readsUntil = how.sourceStartSeconds +
            ((tile + 1) * kTilePixels + 1.0) * how.secondsPerPixel / dpr;
        const bool cacheable = id && readsUntil < stableUntil;
        const RasterKey key{id,
            {how.sourceStartSeconds, how.secondsPerPixel, std::clamp(double(how.gain), 0.0, 8.0),
             area.height(), dpr, how.reversed ? duration : 0.0}, tile, how.color.rgba(), how.reversed, source.samples != nullptr, how.style};
        const QPointF at = origin + QPointF(tile * tileWidth, 0.0);
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
    if (paintSampleTrace(p, area, how)) return;
    const PeakSource source = peaks ? PeakSource{peaks->minima, peaks->maxima,
        peaks->levels, peaks->bucketsPerSecond, false, how.samples} : PeakSource{{}, {}, {}, 0.0};
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
