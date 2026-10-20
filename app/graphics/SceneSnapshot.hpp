#pragma once
#include <QColor>
#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QRegion>
#include <QVector>
#include <QTransform>
#include <memory>
#include <optional>
#include <vector>
#include <algorithm>
#include <cmath>

namespace ui::graphics {
struct SceneMeshGroup;
struct SceneVertex {
    float x, y, u, v;
    bool operator==(const SceneVertex&) const = default;
};
struct SceneClip {
    QRectF bounds;
    QVector<SceneVertex> triangles;
    bool rectangular = false;
};
struct SceneMesh {
    QVector<SceneVertex> vertices; // triangles, viewport-local coordinates
    QColor color;
    QImage texture; // immutable assets/small native controls, never an editor canvas
    bool smoothTexture = true; // False for already-antialiased, device-resolution UI pixels.
    float opacity = 1;
    QTransform transform;
    bool snapTranslation = true;
    std::shared_ptr<const SceneClip> clip; // layer coordinates, before mesh transform
    std::shared_ptr<const SceneMeshGroup> group;
    quint64 visualId = 0;
    QRectF visualRect;
};
struct SceneMeshGroup {
    std::vector<SceneMesh> meshes;
    qint64 bytes = 0;
};
// A scrolling page moves as one scene-graph subtree. Its viewport clip stays
// stationary; the controls inside keep their local transforms and GPU batches.
struct SceneScrollGroup {
    quint64 id = 0;
    QPointF origin;
    QRectF clip;
};
struct SceneLayer {
    quint64 id = 0, revision = 0;
    QPointF origin;
    QRectF clip;
    QRegion mask; // Effective QWidget/ancestor mask in layer coordinates; empty means rectangular.
    bool clipRequired = true;
    qint64 geometryBytes = 0;
    std::vector<SceneMesh> meshes;
    // Recorded once with the geometry; viewport scrolling only changes clip.
    std::optional<QRectF> contentBounds;
    std::shared_ptr<const SceneScrollGroup> scrollGroup;
};

inline QTransform pixelAlignedTranslation(const QTransform& transform, qreal dpr) {
    if (transform.type() > QTransform::TxTranslate) return transform;
    // Use the same physical-pixel alignment for drawing and containment.
    return QTransform::fromTranslate(std::round(transform.dx() * dpr) / dpr,
                                     std::round(transform.dy() * dpr) / dpr);
}
inline std::optional<QRectF> sceneMeshBounds(const SceneMesh& mesh, int& budget, qreal dpr, bool includeClip = true) {
    if (--budget < 0) return {};
    const auto transform = mesh.snapTranslation ? pixelAlignedTranslation(mesh.transform, dpr) : mesh.transform;
    QRectF content;
    if (mesh.group) {
        // Never traverse large timeline/waveform tiles to optimize a clip.
        if (mesh.group->bytes > qint64(budget) * sizeof(SceneVertex)) return {};
        for (const auto& child : mesh.group->meshes) {
            const auto bounds = sceneMeshBounds(child, budget, dpr);
            if (!bounds) return {};
            content |= *bounds;
        }
        content = transform.mapRect(content);
    } else if (mesh.visualId) {
        content = mesh.visualRect;
    } else if (!mesh.vertices.isEmpty()) {
        budget -= int(mesh.vertices.size());
        if (budget < 0) return {};
        const auto first = transform.map(QPointF(mesh.vertices.front().x, mesh.vertices.front().y));
        content = QRectF(first, QSizeF());
        for (const auto& vertex : mesh.vertices) {
            const auto point = transform.map(QPointF(vertex.x, vertex.y));
            content.setLeft(std::min(content.left(), point.x()));
            content.setRight(std::max(content.right(), point.x()));
            content.setTop(std::min(content.top(), point.y()));
            content.setBottom(std::max(content.bottom(), point.y()));
        }
    }
    if (includeClip && mesh.clip) content &= mesh.clip->bounds;
    return content;
}
inline void cacheLayerBounds(SceneLayer& layer, qreal dpr = 1) {
    layer.contentBounds.reset();
    int budget = 4096;
    QRectF content;
    for (const auto& mesh : layer.meshes) {
        // Flat geometry was always measured once. Keep those bounds even for
        // large paths, so later scrolling never scans their vertices again.
        // Only recursive retained groups share the bounded traversal budget.
        int remaining = mesh.group ? budget : std::max(budget, int(mesh.vertices.size()) + 1);
        const auto bounds = sceneMeshBounds(mesh, remaining, dpr);
        if (!bounds) return;
        if (mesh.group) budget = remaining;
        content |= *bounds;
    }
    layer.contentBounds = content;
}
// A rectangular clip cannot affect triangles wholly inside it. Omitting that
// redundant clip lets Qt batch adjacent controls instead of switching scissor
// state for every widget. Mesh clips are already in layer coordinates.
inline bool requiresLayerClip(const SceneLayer& layer) {
    if (!layer.mask.isEmpty()) return true;
    if (layer.contentBounds)
        return !layer.contentBounds->isEmpty() && !layer.clip.contains(*layer.contentBounds);
    for (const auto& mesh : layer.meshes) {
        if (mesh.clip && layer.clip.contains(mesh.clip->bounds)) continue;
        if (mesh.group) return true;
        for (const auto& vertex : mesh.vertices)
            if (!layer.clip.contains(mesh.transform.map(QPointF(vertex.x, vertex.y)))) return true;
    }
    return false;
}
struct SceneSnapshot {
    quint64 revision = 0;
    quint64 projectRevision = 0;
    QSizeF viewport;
    std::vector<std::shared_ptr<const SceneLayer>> layers;
};
} // namespace ui::graphics
