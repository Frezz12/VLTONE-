#pragma once
#include "SceneRecorder.hpp"
#include <QCache>
#include <QDataStream>
#include <QIODevice>
#include <QPainter>
#include <array>
#include <cmath>

namespace ui::graphics {
struct RoundedRectGeometryKey {
    qreal width, height, radius, stroke, dpr, miterLimit;
    int cap, join;
    bool outline;
    bool operator==(const RoundedRectGeometryKey&) const = default;
    friend size_t qHash(const RoundedRectGeometryKey& key, size_t seed = 0) noexcept {
        return qHashMulti(seed, key.width, key.height, key.radius, key.stroke,
                          key.dpr, key.miterLimit, key.cap, key.join, key.outline);
    }
};
// Preserve corner radii and outline thickness during horizontal zoom. Only
// the flat middle changes its GPU transform; text and handles stay separate.
// GUI-owned and bounded across projects, themes, velocities and track heights.
// Dense cells use retain=false to share geometry and batch adjacent fills.
inline bool paintStretchableRoundedRect(QPainter& painter, const QRectF& rect,
                                       qreal radius, bool retain = true) {
    auto* sink = sceneGeometrySink(painter);
    const auto pen = painter.pen();
    const auto brush = painter.brush();
    if (!sink || (brush.style() != Qt::SolidPattern && brush.style() != Qt::NoBrush) || radius <= 0 ||
        (pen.style() != Qt::NoPen && (pen.style() != Qt::SolidLine || pen.isCosmetic() ||
                                    pen.brush().style() != Qt::SolidPattern))) return false;
    radius = std::min(radius, rect.height() / 2.);
    const qreal stroke = pen.style() == Qt::NoPen ? 0. : pen.widthF();
    const qreal edge = radius + stroke;
    if (rect.width() <= 0 || rect.height() <= 0 ||
        (retain && rect.width() < 2. * edge + 1.)) return false;
    if (!retain) {
        // A dense row shares whole-cell geometry at the current step width.
        // Colours/velocities do not belong in its geometry key. Emit one fill
        // and one outline so adjacent equal colours can share a GPU batch.
        struct Geometry { QVector<QPointF> fill, outline; };
        static thread_local QCache<RoundedRectGeometryKey, Geometry> cells(4 * 1024);
        const qreal dpr = painter.device()->devicePixelRatioF();
        const RoundedRectGeometryKey key{rect.width(), rect.height(), radius, stroke, dpr,
                                        pen.miterLimit(), int(pen.capStyle()), int(pen.joinStyle()),
                                        pen.style() != Qt::NoPen};
        auto* geometry = cells.object(key);
        if (!geometry) {
            auto canonicalKey = key;
            canonicalKey.width = std::min(rect.width(), 2. * edge + 1.);
            auto* canonical = cells.object(canonicalKey);
            if (!canonical) {
                SceneRecorder recorder(QSize(int(std::ceil(canonicalKey.width)), int(std::ceil(rect.height()))), dpr);
                QPainter local(&recorder);
                auto outline = pen; outline.setColor(Qt::blue);
                local.setPen(outline); local.setBrush(Qt::red);
                local.drawRoundedRect(QRectF(0,0,canonicalKey.width,rect.height()), radius, radius);
                local.end();
                if (!recorder.supported()) return false;
                auto entry = std::make_unique<Geometry>();
                for (const auto& mesh : recorder.takeMeshes()) {
                    auto& vertices = mesh.color == QColor(Qt::red) ? entry->fill : entry->outline;
                    for (const auto& v : mesh.vertices) vertices.append(QPointF(v.x,v.y));
                }
                const int cost = int((sizeof(key) + (entry->fill.size()+entry->outline.size())*sizeof(QPointF))/1024+1);
                if (cost > cells.maxCost()) return false;
                cells.insert(canonicalKey, entry.release(), cost);
                canonical = cells.object(canonicalKey);
            }
            if (key != canonicalKey) {
                // The rounded caps and stroke keep their screen-space size.
                // Zoom only extends the straight middle; no new tessellation.
                auto entry = std::make_unique<Geometry>(*canonical);
                for (auto* vertices : {&entry->fill, &entry->outline}) for (auto& v : *vertices) {
                    if (v.x() >= edge + 1.) v.rx() += rect.width() - canonicalKey.width;
                    else if (v.x() > edge) v.rx() = edge + (v.x()-edge)*(rect.width()-2.*edge);
                }
                const int cost = int((sizeof(key) + (entry->fill.size()+entry->outline.size())*sizeof(QPointF))/1024+1);
                cells.insert(key, entry.release(), cost);
            }
            geometry = cells.object(key);
        }
        if (brush.style() != Qt::NoBrush)
            sink->appendTriangles(geometry->fill.constData(), int(geometry->fill.size()), brush.color(), rect.topLeft());
        if (pen.style() != Qt::NoPen)
            sink->appendTriangles(geometry->outline.constData(), int(geometry->outline.size()), pen.color(), rect.topLeft());
        return true;
    }
    struct Parts { std::array<std::shared_ptr<const SceneMeshGroup>, 3> groups; };
    static thread_local QCache<QByteArray, Parts> cache(8 * 1024);
    QByteArray key;
    QDataStream stream(&key, QIODevice::WriteOnly);
    const qreal dpr = painter.device()->devicePixelRatioF();
    stream << rect.height() << radius << pen << brush << dpr;
    auto* parts = cache.object(key);
    if (!parts) {
        auto entry = std::make_unique<Parts>();
        qint64 bytes = 0;
        const qreal width = 2. * edge + 1.;
        const qreal outset = stroke + 1.;
        for (int i = 0; i < 3; ++i) {
            SceneRecorder recorder(QSize(int(std::ceil(width + 2 * outset)),
                                         int(std::ceil(rect.height() + 2 * outset))), dpr);
            QPainter local(&recorder);
            local.setPen(pen); local.setBrush(brush);
            const qreal x = i == 0 ? 0. : i == 1 ? edge : edge + 1.;
            local.translate(-x, 0);
            local.setClipRect(QRectF(i == 0 ? -outset : x, -outset,
                i == 1 ? 1. : edge + outset, rect.height() + 2 * outset));
            local.drawRoundedRect(QRectF(0, 0, width, rect.height()), radius, radius);
            local.end();
            if (!recorder.supported()) return false;
            auto group = std::make_shared<SceneMeshGroup>();
            group->meshes = recorder.takeMeshes();
            for (const auto& mesh : group->meshes) {
                group->bytes += qint64(mesh.vertices.size()) * sizeof(SceneVertex);
                if (mesh.group) group->bytes += mesh.group->bytes;
                if (mesh.clip) group->bytes += qint64(mesh.clip->triangles.size()) * sizeof(SceneVertex);
            }
            bytes += group->bytes;
            entry->groups[i] = std::move(group);
        }
        const int cost = int(bytes / 1024 + 1);
        if (cost > cache.maxCost()) return false;
        cache.insert(key, entry.release(), cost);
        parts = cache.object(key);
    }
    for (int i = 0; i < 3; ++i) {
        const qreal x = rect.left() + (i == 0 ? 0. : i == 1 ? edge : rect.width() - edge);
        const qreal stretch = i == 1 ? rect.width() - 2. * edge : 1.;
        painter.save();
        painter.translate(x, rect.top());
        if (i == 1) painter.scale(stretch, 1.);
        // Each part shares an exact boundary with its neighbour. Independent
        // rounding of those translations would produce seams at fractional DPI.
        sink->appendGroup(parts->groups[i], {}, false);
        painter.restore();
    }
    return true;
}
} // namespace ui::graphics
