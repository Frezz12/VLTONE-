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
    std::shared_ptr<const SceneClip> clip; // layer coordinates, before mesh transform
    std::shared_ptr<const SceneMeshGroup> group;
    quint64 visualId = 0;
    QRectF visualRect;
};
struct SceneMeshGroup {
    std::vector<SceneMesh> meshes;
    qint64 bytes = 0;
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
};

inline void cacheLayerBounds(SceneLayer& layer) {
    layer.contentBounds.reset();
    // Retained canvases already have their own clipping. Do not walk large
    // timeline tile groups on every recording of a moving playhead.
    for (const auto& mesh : layer.meshes) if (mesh.group) return;
    QRectF content;
    for (const auto& mesh : layer.meshes) {
        if (mesh.vertices.isEmpty()) continue;
        const auto first = mesh.transform.map(QPointF(mesh.vertices.front().x, mesh.vertices.front().y));
        QRectF bounds(first, QSizeF());
        for (const auto& vertex : mesh.vertices) {
            const auto point = mesh.transform.map(QPointF(vertex.x, vertex.y));
            bounds.setLeft(std::min(bounds.left(), point.x()));
            bounds.setRight(std::max(bounds.right(), point.x()));
            bounds.setTop(std::min(bounds.top(), point.y()));
            bounds.setBottom(std::max(bounds.bottom(), point.y()));
        }
        if (mesh.clip) bounds &= mesh.clip->bounds;
        content |= bounds;
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
