#pragma once
#include <QColor>
#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QVector>
#include <QTransform>
#include <memory>
#include <vector>

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
    bool clipRequired = true;
    std::vector<SceneMesh> meshes;
};
// A rectangular clip cannot affect triangles wholly inside it. Omitting that
// redundant clip lets Qt batch adjacent controls instead of switching scissor
// state for every widget. Mesh clips are already in layer coordinates.
inline bool requiresLayerClip(const SceneLayer& layer) {
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
