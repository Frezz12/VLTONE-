#include "SceneRecorder.hpp"
#include <QCache>
#include <QDataStream>
#include <QIODevice>
#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPathStroker>
#include <QPixmap>
#include <private/qtriangulator_p.h>
#include <cmath>

namespace ui::graphics {
namespace {
// GUI-thread-owned immutable glyph/brush assets. This never caches full canvases.
thread_local QCache<QByteArray, QImage> assets(16 * 1024);
struct GlyphAsset { QImage image; QRectF bounds; };
thread_local QCache<QByteArray, GlyphAsset> glyphAssets(16 * 1024);
QVector<SceneVertex> rectangleTriangles(const QRectF& r) {
    const float l = r.left(), t = r.top(), b = r.bottom(), right = r.right();
    return {{l,t,0,0}, {right,t,0,0}, {l,b,0,0},
            {right,t,0,0}, {right,b,0,0}, {l,b,0,0}};
}
QVector<SceneVertex> triangulate(const QPainterPath& path, qreal dpr) {
    // Qt's private triangulator expects a non-null internal vector path. A
    // completely clipped image/text run can legitimately have no path at all.
    if (path.isEmpty()) return {};
    const auto triangles = qTriangulate(path, QTransform(), dpr);
    QVector<SceneVertex> result;
    result.reserve(triangles.indices.size());
    for (int i = 0; i < triangles.indices.size(); ++i) {
        const auto index = triangles.indices.type() == QVertexIndexVector::UnsignedInt
            ? static_cast<const quint32*>(triangles.indices.data())[i]
            : static_cast<const quint16*>(triangles.indices.data())[i];
        result.append({float(triangles.vertices[index * 2]), float(triangles.vertices[index * 2 + 1]), 0, 0});
    }
    return result;
}
}
class SceneRecorder::Engine final : public QPaintEngine, public SceneGeometrySink {
public:
    explicit Engine(qreal dpr, std::shared_ptr<SceneRecordingCache> cache)
        : QPaintEngine(AllFeatures), dpr(dpr), cache(std::move(cache)) {}
    bool beginRetainedSection(quint64 id, bool invalidated) override {
        Q_ASSERT(!sectionOpen);
        if (cache && !invalidated) {
            const auto found = cache->sections.find(id);
            if (found != cache->sections.end()) {
                for (const auto& mesh : found->second) {
                    const qint64 cost = qint64(mesh.vertices.size()) * sizeof(SceneVertex) +
                        (mesh.group ? mesh.group->bytes : 0) +
                        (mesh.clip ? qint64(mesh.clip->triangles.size()) * sizeof(SceneVertex) : 0);
                    if (!admit(cost)) return false;
                }
                meshes.insert(meshes.end(), found->second.begin(), found->second.end());
                mergeBarrier = meshes.size();
                return false;
            }
        }
        sectionOpen = true; sectionId = id; sectionStart = mergeBarrier = meshes.size();
        return true;
    }
    void endRetainedSection() override {
        Q_ASSERT(sectionOpen);
        if (cache) cache->sections[sectionId] = {meshes.begin() + sectionStart, meshes.end()};
        mergeBarrier = meshes.size();
        sectionOpen = false;
    }
    bool begin(QPaintDevice* device) override { setPaintDevice(device); setActive(true); return true; }
    bool end() override { setActive(false); return true; }
    Type type() const override { return Type(QPaintEngine::User + 17); }
    void updateState(const QPaintEngineState& state) override {
        if (state.state() & (DirtyClipPath | DirtyClipRegion | DirtyClipEnabled)) clipValid = false;
        if (state.state().testFlag(DirtyCompositionMode) &&
            state.compositionMode() != QPainter::CompositionMode_SourceOver)
            allSupported = false;
    }
    bool admit(qint64 bytes) {
        if (!allSupported || bytes < 0 || bytes > 64 * 1024 * 1024 - geometryBytes) {
            allSupported = false; return false;
        }
        geometryBytes += bytes;
        return true;
    }
    QVector<SceneVertex> makeTriangles(const QPainterPath& path) {
        // Bound each tessellation input before entering Qt's triangulator.
        // Larger curves must be split into visible tiles by their scene source.
        if (!allSupported || path.elementCount() > 32768) { allSupported = false; return {}; }
        return triangulate(path, dpr);
    }
    void appendMesh(SceneMesh mesh) {
        if (mesh.vertices.isEmpty()) return;
        if (!admit(qint64(mesh.vertices.size()) * sizeof(SceneVertex))) return;
        // Coalesce consecutive equal fills without reordering translucent
        // primitives or merging across a retained section boundary.
        if (mesh.texture.isNull() && !mesh.clip && mesh.transform.isIdentity() && meshes.size() > mergeBarrier) {
            auto& previous = meshes.back();
            if (!previous.group && !previous.visualId && previous.texture.isNull() && !previous.clip && previous.transform.isIdentity() &&
                previous.color == mesh.color && previous.opacity == mesh.opacity &&
                previous.vertices.size() + mesh.vertices.size() <= 256 * 1024) {
                previous.vertices += mesh.vertices;
                return;
            }
        }
        meshes.push_back(std::move(mesh));
    }
    void appendGroup(std::shared_ptr<const SceneMeshGroup> group, QPointF origin) override {
        if (!group) { allSupported = false; return; }
        if (!admit(group->bytes)) return;
        SceneMesh mesh;
        mesh.group = std::move(group);
        mesh.transform = painter()->worldTransform();
        mesh.transform.translate(origin.x(), origin.y());
        mesh.clip = currentClip();
        mesh.opacity = float(painter()->opacity());
        meshes.push_back(std::move(mesh));
        mergeBarrier = meshes.size();
    }
    void appendVisual(quint64 id, const QRectF& bounds) override {
        SceneMesh mesh;
        mesh.visualId = id;
        mesh.visualRect = painter()->worldTransform().mapRect(bounds);
        mesh.opacity = float(painter()->opacity());
        mesh.clip = currentClip();
        meshes.push_back(std::move(mesh));
        mergeBarrier = meshes.size();
    }
    void appendTriangles(const QPointF* vertices, int count, const QColor& color) override {
        if (count < 0 || qint64(count) * sizeof(SceneVertex) > 64 * 1024 * 1024 - geometryBytes) {
            allSupported = false; return;
        }
        SceneMesh mesh;
        mesh.color = color; mesh.opacity = float(painter()->opacity());
        mesh.vertices.reserve(count);
        const auto transform = painter()->worldTransform();
        for (int i = 0; i < count; ++i) {
            const auto point = transform.map(vertices[i]);
            mesh.vertices.append({float(point.x()), float(point.y()), 0, 0});
        }
        mesh.clip = currentClip();
        if (mesh.clip && mesh.clip->bounds.isEmpty()) return;
        appendMesh(std::move(mesh));
    }
    std::shared_ptr<const SceneClip> currentClip() {
        if (clipValid) return activeClip;
        clipValid = true;
        activeClip.reset();
        activeClipPath = {};
        if (!painter()->hasClipping()) return {};
        const auto path = painter()->worldTransform().map(painter()->clipPath());
        activeClipPath = path;
        for (const auto& entry : clips) if (entry.first == path) return activeClip = entry.second;
        auto clip = std::make_shared<SceneClip>();
        clip->bounds = path.boundingRect();
        QPainterPath rectangle; rectangle.addRect(clip->bounds);
        clip->rectangular = path == rectangle;
        if (!clip->rectangular) {
            clip->triangles = makeTriangles(path);
            admit(qint64(clip->triangles.size()) * sizeof(SceneVertex));
        }
        clips.emplace_back(path, clip);
        return activeClip = clip;
    }
    void appendLocalGeometry(const QVector<SceneVertex>& vertices, QPointF origin, const QColor& color) override {
        SceneMesh mesh;
        mesh.vertices = vertices;
        mesh.color = color; mesh.opacity = float(painter()->opacity());
        mesh.transform = painter()->worldTransform();
        mesh.transform.translate(origin.x(), origin.y());
        mesh.clip = currentClip();
        if (mesh.clip && mesh.clip->bounds.isEmpty()) return;
        // Keep each retained tile separate, including an identity-positioned
        // first tile: merging would detach and copy its shared vertex buffer.
        if (!mesh.vertices.isEmpty() && admit(qint64(mesh.vertices.size()) * sizeof(SceneVertex)))
            meshes.push_back(std::move(mesh));
        mergeBarrier = meshes.size();
    }
    QPainterPath clipped(QPainterPath path) {
        if (const auto clip = currentClip()) {
            const auto bounds = path.controlPointRect();
            if (clip->bounds.isEmpty() || !clip->bounds.intersects(bounds)) return {};
            // Most controls and grid primitives lie entirely inside a simple
            // viewport clip. Boolean path clipping adds no information there.
            if (!clip->rectangular || !clip->bounds.contains(bounds)) path = path.intersected(activeClipPath);
        }
        return path;
    }
    void fill(const QPainterPath& path, const QBrush& brush) {
        if (brush.style() == Qt::NoBrush || path.isEmpty()) return;
        const auto transformed = clipped(painter()->worldTransform().map(path));
        if (transformed.isEmpty()) return;
        SceneMesh mesh;
        mesh.vertices = makeTriangles(transformed);
        mesh.color = brush.color();
        mesh.opacity = float(painter()->opacity());
        if (brush.style() != Qt::SolidPattern) {
            const auto bounds = path.boundingRect();
            if (bounds.isEmpty()) return;
            QByteArray key;
            QDataStream stream(&key, QIODevice::WriteOnly);
            const auto* gradient = brush.gradient();
            if (gradient && gradient->type() == QGradient::LinearGradient &&
                gradient->coordinateMode() == QGradient::LogicalMode &&
                gradient->spread() == QGradient::PadSpread && brush.transform().isIdentity()) {
                const auto& linear = *static_cast<const QLinearGradient*>(gradient);
                const QPointF delta = linear.finalStop() - linear.start();
                const double length = QPointF::dotProduct(delta, delta);
                const auto inverse = painter()->worldTransform().inverted();
                bool inRamp = length > 0;
                for (auto& vertex : mesh.vertices) {
                    const QPointF point = inverse.map(QPointF(vertex.x, vertex.y)) - linear.start();
                    const double position = length > 0 ? QPointF::dotProduct(point, delta) / length : 0;
                    inRamp &= position >= -1e-6 && position <= 1.000001;
                    vertex.u = .5f;
                    vertex.v = float((std::clamp(position, 0., 1.) * 1023. + .5) / 1024.);
                }
                if (inRamp) {
                    // One immutable colour ramp serves every meter height,
                    // note size and translated clip. Interpolation is GPU work;
                    // changing a peak no longer creates a 128x128 CPU image.
                    QLinearGradient rampGradient(linear);
                    rampGradient.setStart(0, .5); rampGradient.setFinalStop(0, 1023.5);
                    stream << quint8(4) << QBrush(rampGradient);
                    if (auto* cached = assets.object(key)) mesh.texture = *cached;
                    else {
                        QImage ramp(1, 1024, QImage::Format_ARGB32_Premultiplied);
                        ramp.fill(Qt::transparent);
                        QPainter rampPainter(&ramp);
                        rampPainter.fillRect(ramp.rect(), rampGradient); rampPainter.end();
                        mesh.texture = ramp;
                        assets.insert(key, new QImage(ramp), 4);
                    }
                    appendMesh(std::move(mesh));
                    return;
                }
            }
            if (gradient && gradient->type() == QGradient::LinearGradient &&
                gradient->coordinateMode() == QGradient::LogicalMode && brush.transform().isIdentity()) {
                auto local = *static_cast<const QLinearGradient*>(gradient);
                auto start = local.start() - bounds.topLeft();
                const auto delta = local.finalStop() - local.start();
                const auto lengthSquared = QPointF::dotProduct(delta, delta);
                // A linear ramp is invariant perpendicular to its direction.
                // Moving a clip horizontally must not create a new vertical
                // gradient texture, even if the brush's start.x stays zero.
                if (lengthSquared > 0)
                    start = delta * (QPointF::dotProduct(start, delta) / lengthSquared);
                local.setStart(start); local.setFinalStop(start + delta);
                stream << quint8(3) << QBrush(local) << bounds.size();
            } else {
                stream << quint8(1) << brush << bounds;
            }
            if (auto* cached = assets.object(key)) mesh.texture = *cached;
            else {
                QImage ramp(128, 128, QImage::Format_ARGB32_Premultiplied);
                ramp.fill(Qt::transparent);
                QPainter p(&ramp);
                p.scale(128. / bounds.width(), 128. / bounds.height());
                p.translate(-bounds.topLeft());
                p.fillRect(bounds, brush);
                p.end();
                mesh.texture = ramp;
                assets.insert(key, new QImage(ramp), int(ramp.sizeInBytes() / 1024 + 1));
            }
            const auto inverse = painter()->worldTransform().inverted();
            for (auto& vertex : mesh.vertices) {
                const auto local = inverse.map(QPointF(vertex.x, vertex.y));
                vertex.u = float((local.x() - bounds.x()) / bounds.width());
                vertex.v = float((local.y() - bounds.y()) / bounds.height());
            }
        }
        appendMesh(std::move(mesh));
    }
    void drawPath(const QPainterPath& path) override {
        fill(path, painter()->brush());
        const auto pen = painter()->pen();
        if (pen.style() == Qt::NoPen) return;
        QPainterPathStroker stroker;
        stroker.setWidth(pen.widthF() > 0 ? pen.widthF() : 1. / dpr);
        stroker.setCapStyle(pen.capStyle());
        stroker.setJoinStyle(pen.joinStyle());
        stroker.setMiterLimit(pen.miterLimit());
        if (pen.style() == Qt::CustomDashLine) stroker.setDashPattern(pen.dashPattern());
        else stroker.setDashPattern(pen.style());
        stroker.setDashOffset(pen.dashOffset());
        fill(stroker.createStroke(path), pen.brush());
    }
    void drawRects(const QRectF* rects, int count) override {
        const auto transform = painter()->worldTransform();
        const auto clip = currentClip();
        if (painter()->pen().style() != Qt::NoPen || painter()->brush().style() != Qt::SolidPattern ||
            transform.type() > QTransform::TxScale || (clip && !clip->rectangular)) {
            QPaintEngine::drawRects(rects, count);
            return;
        }
        for (int i = 0; i < count; ++i) {
            auto visible = transform.mapRect(rects[i]);
            if (clip) visible &= clip->bounds;
            if (visible.isEmpty()) continue;
            SceneMesh mesh;
            mesh.vertices = rectangleTriangles(visible);
            mesh.color = painter()->brush().color();
            mesh.opacity = float(painter()->opacity());
            appendMesh(std::move(mesh));
        }
    }
    void drawRects(const QRect* rects, int count) override {
        for (int i = 0; i < count; ++i) { const QRectF r(rects[i]); drawRects(&r, 1); }
    }
    void drawPolygon(const QPointF* points, int count, PolygonDrawMode mode) override {
        if (!count) return;
        QPainterPath path(points[0]);
        for (int i = 1; i < count; ++i) path.lineTo(points[i]);
        path.setFillRule(mode == WindingMode ? Qt::WindingFill : Qt::OddEvenFill);
        if (mode != PolylineMode) path.closeSubpath();
        if (mode == PolylineMode) {
            const auto brush = painter()->brush();
            painter()->setBrush(Qt::NoBrush); drawPath(path); painter()->setBrush(brush);
        } else drawPath(path);
    }
    void drawPixmap(const QRectF& target, const QPixmap& pixmap, const QRectF& source) override {
        drawImage(target, pixmap.toImage(), source, Qt::AutoColor);
    }
    void drawImage(const QRectF& target, const QImage& image, const QRectF& source,
                   Qt::ImageConversionFlags) override {
        if (target.isEmpty() || image.isNull()) return;
        SceneMesh mesh;
        const auto transform = painter()->worldTransform();
        const auto clip = currentClip();
        if (transform.type() <= QTransform::TxScale && (!clip || clip->rectangular)) {
            auto visible = transform.mapRect(target);
            if (clip) visible &= clip->bounds;
            if (visible.isEmpty()) return;
            mesh.vertices = rectangleTriangles(visible);
        } else {
            QPainterPath path; path.addRect(target);
            mesh.vertices = makeTriangles(clipped(transform.map(path)));
        }
        if (mesh.vertices.isEmpty()) return;
        mesh.texture = image;
        mesh.opacity = float(painter()->opacity());
        const QRectF sourceRect = source.isEmpty() ? QRectF(image.rect()) : source;
        const auto inverse = painter()->worldTransform().inverted();
        for (auto& vertex : mesh.vertices) {
            const auto local = inverse.map(QPointF(vertex.x, vertex.y));
            vertex.u = float((sourceRect.x() + (local.x() - target.x()) / target.width() * sourceRect.width()) / image.width());
            vertex.v = float((sourceRect.y() + (local.y() - target.y()) / target.height() * sourceRect.height()) / image.height());
        }
        appendMesh(std::move(mesh));
    }
    void drawTextItem(const QPointF& baseline, const QTextItem& item) override {
        const auto color = painter()->pen().color();
        QByteArray key;
        QDataStream stream(&key, QIODevice::WriteOnly);
        stream << quint8(2) << item.text() << item.font() << color << dpr << int(item.renderFlags());
        stream << item.width() << item.ascent() << item.descent();
        QRectF bounds;
        QImage glyphs;
        if (auto* cached = glyphAssets.object(key)) { glyphs = cached->image; bounds = cached->bounds; }
        else {
            const QFontMetricsF metrics(item.font());
            bounds = metrics.boundingRect(item.text()).united(QRectF(0, -item.ascent(), item.width(),
                                        item.ascent() + item.descent())).adjusted(-2, -2, 2, 2);
            glyphs = QImage(QSize(std::max(1, int(std::ceil(bounds.width() * dpr))),
                                  std::max(1, int(std::ceil(bounds.height() * dpr)))), QImage::Format_ARGB32_Premultiplied);
            glyphs.setDevicePixelRatio(dpr);
            glyphs.fill(Qt::transparent);
            QPainter p(&glyphs);
            p.setFont(item.font()); p.setPen(color);
            p.drawText(-bounds.topLeft(), item.text()); p.end();
            glyphAssets.insert(key, new GlyphAsset{glyphs, bounds}, int(glyphs.sizeInBytes() / 1024 + 1));
        }
        drawImage(QRectF(baseline + bounds.topLeft(), glyphs.size() / dpr), glyphs,
                  glyphs.rect(), Qt::AutoColor);
    }
    std::vector<SceneMesh> meshes;
    std::vector<std::pair<QPainterPath, std::shared_ptr<const SceneClip>>> clips;
    std::shared_ptr<const SceneClip> activeClip;
    QPainterPath activeClipPath;
    bool clipValid = false;
    bool allSupported = true;
    qreal dpr;
    std::shared_ptr<SceneRecordingCache> cache;
    std::size_t sectionStart = 0;
    std::size_t mergeBarrier = 0;
    quint64 sectionId = 0;
    bool sectionOpen = false;
    qint64 geometryBytes = 0;
};

SceneRecorder::SceneRecorder(QSize size, qreal dpr, std::shared_ptr<SceneRecordingCache> cache)
    : m_engine(std::make_unique<Engine>(dpr, cache)), m_size(size), m_dpr(dpr) {
    if (cache && (cache->size != size || cache->dpr != dpr)) {
        cache->sections.clear(); cache->size = size; cache->dpr = dpr;
    }
}
SceneRecorder::~SceneRecorder() = default;
QPaintEngine* SceneRecorder::paintEngine() const { return m_engine.get(); }
std::vector<SceneMesh> SceneRecorder::takeMeshes() { return std::move(m_engine->meshes); }
bool SceneRecorder::supported() const { return m_engine->allSupported; }
int SceneRecorder::metric(PaintDeviceMetric metric) const {
    switch (metric) {
    case PdmWidth: return m_size.width();
    case PdmHeight: return m_size.height();
    case PdmWidthMM: return int(m_size.width() * 25.4 / 96);
    case PdmHeightMM: return int(m_size.height() * 25.4 / 96);
    case PdmDpiX: case PdmDpiY: case PdmPhysicalDpiX: case PdmPhysicalDpiY: return 96;
    case PdmDepth: return 32;
    case PdmNumColors: return 0;
    case PdmDevicePixelRatio: return int(m_dpr);
    case PdmDevicePixelRatioScaled: return int(m_dpr * devicePixelRatioFScale());
    default: return QPaintDevice::metric(metric);
    }
}
} // namespace ui::graphics
