#pragma once
#include <QPainter>
#include <QPaintEngine>
#include "SceneSnapshot.hpp"
namespace ui::graphics {
class SceneGeometrySink {
public:
    virtual ~SceneGeometrySink() = default;
    virtual void appendTriangles(const QPointF* vertices, int count, const QColor& color) = 0;
    // Shares immutable local geometry; scrolling changes only its GPU matrix.
    virtual void appendLocalGeometry(const QVector<SceneVertex>& vertices,
                                     QPointF origin, const QColor& color) = 0;
    virtual void appendGroup(std::shared_ptr<const SceneMeshGroup> group, QPointF origin) = 0;
    virtual void appendVisual(quint64 id, const QRectF& bounds) = 0;
    // Replays immutable geometry when the caller's static layer is unchanged.
    // Returning true asks the caller to draw, followed by endRetainedSection.
    virtual bool beginRetainedSection(quint64, bool) { return true; }
    virtual void endRetainedSection() {}
};
inline bool isSceneRecording(const QPainter& painter) {
    return painter.paintEngine() && painter.paintEngine()->type() == QPaintEngine::Type(QPaintEngine::User + 17);
}
inline SceneGeometrySink* sceneGeometrySink(QPainter& painter) {
    return isSceneRecording(painter) ? dynamic_cast<SceneGeometrySink*>(painter.paintEngine()) : nullptr;
}
} // namespace ui::graphics
