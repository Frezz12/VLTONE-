#pragma once
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QList>
#include <QElapsedTimer>
#include <QPoint>
#include <QTimer>
#include <memory>
#include <list>
#include <unordered_map>
#include "SceneSnapshot.hpp"
#include "GraphicsQualityPolicy.hpp"
#include <functional>

class QWidget;
class QQuickWindow;
class QQuickItem;
class QQmlEngine;
class QEvent;
class QPointingDevice;
namespace ui::graphics {
class SceneItem;
class QuickVisual;
struct SceneRecordingCache;
// Transitional presentation host. C++ widgets retain input/layout/commands;
// their canvas primitives become immutable layers in ONE Quick scene. Nothing
// in the render thread accesses the compatibility widget hierarchy.
class WorkspaceSurface final : public QObject {
    Q_OBJECT
public:
    explicit WorkspaceSurface(QWidget* source);
    ~WorkspaceSurface() override;
    QQuickWindow* quickWindow() const { return m_window; }
    /// Explicit snapshot for project previews; includes pending widget changes.
    QImage grabImage();
    void invalidate();
    void refreshPresentationMode();
    void setFrameContextProvider(std::function<void(FrameStats&)> provider) { m_frameContext = std::move(provider); }
    void setProjectRevisionProvider(std::function<quint64()> provider) { m_projectRevision = std::move(provider); }
    static bool checkPointerRoutingForTest();
signals:
    void failed(const QString& reason);
    void frameMeasured(double preparationMs, double synchronizationMs, double renderCpuMs,
                       double submissionIntervalMs, double renderThreadCpuMs);
protected:
    bool eventFilter(QObject*, QEvent*) override;
private:
    void requestFrame();
    void requestCapture(bool hierarchyChanged = true);
    void capture();
    void present(std::shared_ptr<const SceneSnapshot>);
    struct ScrollContext {
        std::shared_ptr<const SceneScrollGroup> group;
        QRect parentClip;
        QRegion parentMask;
    };
    void visit(QWidget*, std::shared_ptr<SceneSnapshot>&, QSet<quintptr>&,
               QPoint parentOrigin, QRect parentClip, const QRegion& parentMask,
               bool descendants = true, const ScrollContext& scroll = {});
    bool forwardInput(QEvent*);
    void updateHover(QWidget* target, const QPointF& globalPosition,
                     Qt::KeyboardModifiers modifiers = Qt::NoModifier,
                     const QPointingDevice* device = nullptr);
    void resizeSurface();
    void fail(const QString& reason);
    QPointer<QWidget> m_source, m_container, m_pressed, m_hover, m_dragTarget;
    QPointF m_lastHoverPosition;
    QQuickWindow* m_window = nullptr;
    SceneItem* m_item = nullptr;
    QQuickItem* m_framePreparation = nullptr;
    QQmlEngine* m_qml = nullptr;
    std::vector<SceneItem*> m_segments;
    struct VisualItem {
        QPointer<QuickVisual> source;
        QQuickItem* clip = nullptr;
        QQuickItem* item = nullptr;
    };
    std::unordered_map<quint64, VisualItem> m_visuals;
    QSet<QWidget*> m_dirty;
    bool m_geometryExposure = false;
    bool m_collectedWidgetUpdates = false;
    struct CachedLayer {
        QPointer<QWidget> widget;
        std::shared_ptr<const SceneLayer> layer;
        std::shared_ptr<SceneRecordingCache> recording;
        QPoint viewportOffset;
        QPoint parentOrigin;
        QRect parentClip;
        QRegion parentMask;
        ScrollContext scroll;
    };
    std::unordered_map<quintptr, CachedLayer> m_layers;
    struct OffscreenLayer { CachedLayer cached; qint64 bytes; std::list<quintptr>::iterator recency; };
    std::list<quintptr> m_offscreenRecency;
    std::unordered_map<quintptr, OffscreenLayer> m_offscreenLayers;
    qint64 m_offscreenBytes = 0;
    void discardOffscreenLayer(quintptr id);
    std::shared_ptr<const SceneSnapshot> m_lastSnapshot;
    bool m_hierarchyDirty = true;
    int m_visitedWidgets = 0;
    // Written on the GUI thread, sampled only while it is blocked for sync.
    FrameStats m_preparedFrame;
    quint64 m_preparedRevision = 0;
    std::function<void(FrameStats&)> m_frameContext;
    std::function<quint64()> m_projectRevision;
    // The following timing state belongs exclusively to the render thread.
    FrameStats m_renderFrame;
    struct FrameMailbox;
    std::shared_ptr<FrameMailbox> m_frameMailbox;
    quint64 m_renderedRevision = 0;
    double m_renderThreadCpuStart = -1;
    QElapsedTimer m_syncClock, m_renderClock, m_submissionClock;
    QElapsedTimer m_syntheticContextMenuClock;
    QPoint m_syntheticContextMenuGlobal;
    QTimer m_presentationTimer;
    quint64 m_revision = 0;
    quint64 m_quickGrabVisual = 0;
    bool m_capturePending = false, m_capturing = false, m_preparingFrame = false;
    bool m_stopping = false, m_nativeGesture = false;
};
} // namespace ui::graphics
