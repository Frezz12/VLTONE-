#include "WorkspaceSurface.hpp"
#include "SceneItem.hpp"
#include "SceneRecorder.hpp"
#include "ScenePaintSource.hpp"
#include "WidgetUpdates.hpp"
#include "QuickVisual.hpp"
#include "GraphicsPreferences.hpp"
#include "UiFrameClock.hpp"
#include "UiPerformance.hpp"
#include "GpuTiming.hpp"
#include "Transport/AudioPresentationClock.hpp"
#include <QApplication>
#include <QChildEvent>
#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEnterEvent>
#include <QHoverEvent>
#include <QInputMethodEvent>
#include <QNativeGestureEvent>
#include <QHelpEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QQuickWindow>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSurfaceFormat>
#include <QWheelEvent>
#include <QWidget>
#include <QWindow>
#include <functional>
#include <ctime>
#include <mutex>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ui::graphics {
namespace {
double threadCpuMs() {
#ifdef Q_OS_WIN
    FILETIME created, exited, kernel, user;
    if (GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) {
        const auto ns100 = [](FILETIME time) { return (quint64(time.dwHighDateTime) << 32) | time.dwLowDateTime; };
        return double(ns100(kernel) + ns100(user)) / 10000.;
    }
#elif defined(CLOCK_THREAD_CPUTIME_ID)
    timespec time{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time) == 0)
        return double(time.tv_sec) * 1000. + time.tv_nsec / 1e6;
#endif
    return -1;
}
bool nativeControlAsset(const QWidget* widget) {
    // Native platform styles can bypass QPainter and require a CGContext/HDC.
    // Cache these small controls at full DPI; never use this for editor canvases.
    return widget->inherits("QFocusFrame") || widget->inherits("QLineEdit") ||
           widget->inherits("QScrollBar") || widget->inherits("QAbstractButton") ||
           widget->inherits("QComboBox") || widget->inherits("QAbstractSpinBox") ||
           widget->inherits("QSlider") || widget->inherits("QDial") ||
           widget->inherits("QProgressBar") || widget->inherits("QSizeGrip");
}
bool belongsToNativeOverlay(const QWidget* widget, const QWidget* source) {
    for (const QWidget* current = widget; current;
         current = current->parentWidget()) {
        if (current->property("vlt.nativeOverlay").toBool()) return true;
        if (current == source) break;
    }
    return false;
}
bool startsFreshPointerRoute(QEvent::Type type, Qt::MouseButtons buttons) {
    return type == QEvent::MouseButtonPress || type == QEvent::MouseButtonDblClick ||
           (type == QEvent::MouseMove && buttons == Qt::NoButton);
}
QWidget* pointerTarget(QPointer<QWidget>& pressed, QEvent::Type type,
                       Qt::MouseButtons buttons, QWidget* hit) {
    // A combo box/menu popup is a separate native window. Its release can be
    // consumed by that window, leaving our compatibility-surface grab stale.
    // A new physical press always starts a new gesture and must be hit-tested
    // at its current position. A no-button move also repairs a lost release.
    if (startsFreshPointerRoute(type, buttons))
        pressed = nullptr;
    return pressed ? pressed.data() : hit;
}

// A native QWidget window lets an ignored wheel event climb from the leaf to
// its parents. Here the physical event first lands in a QQuickWindow, so the
// compatibility bridge must preserve that propagation explicitly.
bool routeWheelThroughWidgets(QWidget* source, QWidget* target,
                              const QWheelEvent* wheel) {
    if (!source || !target || !wheel) return false;
    for (QWidget* receiver = target; receiver;
         receiver = receiver->parentWidget()) {
        if (receiver != source && !source->isAncestorOf(receiver)) break;
        const QPointF local = receiver->mapFromGlobal(wheel->globalPosition());
        QWheelEvent forwarded(local, wheel->globalPosition(), wheel->pixelDelta(),
                              wheel->angleDelta(), wheel->buttons(),
                              wheel->modifiers(), wheel->phase(),
                              wheel->inverted(), wheel->source(),
                              wheel->pointingDevice());
        forwarded.setTimestamp(wheel->timestamp());
        forwarded.ignore();
        QCoreApplication::sendEvent(receiver, &forwarded);
        if (forwarded.isAccepted()) return true;
        // QAbstractScrollArea's QWidget path consumes angle deltas, while
        // native precision scrolling is normally handled one layer earlier by
        // QWidgetWindow. That layer is bypassed by this bridge, so apply the
        // pixel distance directly when the viewport did not handle it.
        if (auto* scroll = qobject_cast<QAbstractScrollArea*>(receiver);
            scroll && !wheel->pixelDelta().isNull()) {
            bool moved = false;
            const auto apply = [&moved](QScrollBar* bar, int delta) {
                if (!bar || delta == 0) return;
                const int before = bar->value();
                bar->setValue(before - delta);
                moved |= bar->value() != before;
            };
            apply(scroll->horizontalScrollBar(), wheel->pixelDelta().x());
            apply(scroll->verticalScrollBar(), wheel->pixelDelta().y());
            if (moved) return true;
        }
        if (receiver == source) break;
    }
    return false;
}
}
struct WorkspaceSurface::FrameMailbox {
    std::mutex mutex;
    FrameStats latest;
    double interval = -1, cpu = -1;
    quint64 coalesced = 0;
    bool queued = false;
    WorkspaceSurface* owner = nullptr;
};
bool WorkspaceSurface::checkPointerRoutingForTest() {
    QWidget combo, apply;
    QPointer<QWidget> pressed = &combo;
    const bool freshPress = pointerTarget(pressed, QEvent::MouseButtonPress,
                                          Qt::LeftButton, &apply) == &apply && !pressed;

    pressed = &combo;
    const bool staleReleaseHeals = pointerTarget(pressed, QEvent::MouseMove,
                                                 Qt::NoButton, &apply) == &apply && !pressed;

    pressed = &combo;
    const bool activeDragKeepsGrab = pointerTarget(pressed, QEvent::MouseMove,
                                                   Qt::LeftButton, &apply) == &combo && pressed == &combo;
    QScrollArea scroll;
    auto* page = new QWidget;
    auto* leaf = new QWidget(page);
    page->setFixedSize(180, 600);
    leaf->setGeometry(20, 20, 80, 24);
    scroll.setWidget(page);
    scroll.resize(200, 140);
    scroll.show();
    QApplication::processEvents();
    const QPointF leafPoint(leaf->rect().center());
    const QPointF global = leaf->mapToGlobal(leafPoint);
    QWheelEvent angleWheel(leaf->mapTo(&scroll, leafPoint), global, {},
                           QPoint(0, -120), Qt::NoButton, Qt::NoModifier,
                           Qt::ScrollUpdate, false);
    const bool angleRouted = routeWheelThroughWidgets(&scroll, leaf, &angleWheel);
    const int afterAngle = scroll.verticalScrollBar()->value();
    QWheelEvent pixelWheel(leaf->mapTo(&scroll, leafPoint), global,
                           QPoint(0, -24), {}, Qt::NoButton, Qt::NoModifier,
                           Qt::ScrollUpdate, false);
    const bool pixelsRouted = routeWheelThroughWidgets(&scroll, leaf, &pixelWheel);
    const bool wheelClimbsToScroller = angleRouted && pixelsRouted &&
        afterAngle > 0 && scroll.verticalScrollBar()->value() > afterAngle;
    return freshPress && staleReleaseHeals && activeDragKeepsGrab &&
           wheelClimbsToScroller;
}
WorkspaceSurface::WorkspaceSurface(QWidget* source) : QObject(source), m_source(source),
    m_frameMailbox(std::make_shared<FrameMailbox>()) {
    m_frameMailbox->owner = this;
    for (auto* ancestor = source->parentWidget(); ancestor; ancestor = ancestor->parentWidget()) {
        const auto surfaces = ancestor->findChildren<WorkspaceSurface*>(QString(), Qt::FindDirectChildrenOnly);
        if (surfaces.isEmpty()) continue;
        m_frameContext = surfaces.front()->m_frameContext;
        m_projectRevision = surfaces.front()->m_projectRevision;
        break;
    }
    source->setProperty("vlt.gpuSurfaceActive", true);
    m_window = new QQuickWindow;
    const bool gpuTiming = configureGpuTiming(m_window);
    auto format = m_window->requestedFormat();
    format.setSamples(4);
    format.setSwapInterval(QSettings().value("ui/frameMode").toString() == "unlimited" ? 0 : 1);
    m_window->setFormat(format);
    m_window->setColor(Qt::transparent);
    m_item = new SceneItem(m_window->contentItem());
    m_segments.push_back(m_item);
    connect(m_item, &SceneItem::resourceError, this, &WorkspaceSurface::fail, Qt::QueuedConnection);
    m_container = QWidget::createWindowContainer(m_window, source);
    m_container->setObjectName("GpuWorkspaceSurface");
    // Do not mark the overlay opaque to QWidget: its ordinary controls still
    // use Paint events to publish damage, even though the scene draws them.
    m_container->setAttribute(Qt::WA_NoSystemBackground);
    m_container->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_container->setFocusPolicy(Qt::NoFocus);
    m_container->setAcceptDrops(true);
    connect(m_window, &QWindow::screenChanged, this, [this] { invalidate(); });
    // The application filter below also receives window events. Installing
    // twice would process Quick pointer grabs/releases twice.
    m_presentationTimer.setSingleShot(true);
    connect(&m_presentationTimer, &QTimer::timeout, this, &WorkspaceSurface::refreshPresentationMode);
    FrameClock::instance().setPresenter(source, this, m_window,
        [this] { if (!m_stopping) m_window->update(); },
        [this](QWidget* widget, const QRegion&) {
            if (m_stopping || !m_source || (widget != m_source && !m_source->isAncestorOf(widget))) return false;
            m_dirty.insert(widget);
            m_capturePending = true;
            return true;
        });
    connect(m_window, &QQuickWindow::afterAnimating, this, [this] {
        if (m_stopping) return;
        // GUI thread, immediately before Quick sync. One cadence supplies
        // timers, the latest input state and recording for this same frame.
        const daw::engine::PresentationFrameTime frameTime;
        FrameClock::instance().presentationFrame(m_source, this);
        if (m_capturePending) capture();
        else {
            m_preparedFrame.preparationMs = 0;
            m_preparedFrame.displayHz = m_window->screen() ? m_window->screen()->refreshRate() : 60;
            if (m_frameContext) m_frameContext(m_preparedFrame);
            m_preparedFrame.frameBudgetMs = FrameClock::instance().periodSeconds(m_source) * 1000.;
        }
    });
    connect(m_window, &QQuickWindow::sceneGraphError, this, [this](QQuickWindow::SceneGraphError, const QString& reason) {
        fail(reason);
    });
    connect(m_window, &QQuickWindow::sceneGraphInitialized, this, [this] {
        const auto api = m_window->rendererInterface()->graphicsApi();
        if (api == QSGRendererInterface::Software || api == QSGRendererInterface::Unknown || api == QSGRendererInterface::Null)
            QMetaObject::invokeMethod(this, [this] {
                fail(tr("A hardware scene graph is not available."));
            }, Qt::QueuedConnection);
    }, Qt::DirectConnection);
    connect(m_window, &QQuickWindow::beforeFrameBegin, this, [this] {
        if (m_renderThreadCpuStart < 0) m_renderThreadCpuStart = threadCpuMs();
        m_renderFrame.preparationMs = m_renderFrame.synchronizationMs = m_renderFrame.renderCpuMs = 0;
    }, Qt::DirectConnection);
    connect(m_window, &QQuickWindow::beforeSynchronizing, this, [this] {
        m_syncClock.start();
        m_renderFrame = m_preparedFrame;
        if (m_renderedRevision == m_preparedRevision) m_renderFrame.preparationMs = 0;
        m_renderedRevision = m_preparedRevision;
    }, Qt::DirectConnection);
    connect(m_window, &QQuickWindow::afterSynchronizing, this, [this] {
        m_renderFrame.synchronizationMs = m_syncClock.nsecsElapsed() / 1e6;
    }, Qt::DirectConnection);
    connect(m_window, &QQuickWindow::beforeRendering, this, [this, gpuTiming] {
        m_renderFrame.gpuMs = gpuTiming ? completedGpuTimeMs(m_window) : -1;
        m_renderClock.start();
    }, Qt::DirectConnection);
    connect(m_window, &QQuickWindow::afterRendering, this, [this] {
        m_renderFrame.renderCpuMs = m_renderClock.nsecsElapsed() / 1e6;
    }, Qt::DirectConnection);
    connect(m_window, &QQuickWindow::frameSwapped, this, [this] {
        // frameSwapped means queued for presentation, not physical scanout.
        // Keep this diagnostic separate from the acceptance latency metric.
        const double interval = m_submissionClock.isValid() ? m_submissionClock.nsecsElapsed() / 1e6 : -1;
        m_submissionClock.start();
        const double nowCpu = threadCpuMs();
        const double renderThreadCpu = m_renderThreadCpuStart >= 0 && nowCpu >= 0 ? nowCpu - m_renderThreadCpuStart : -1;
        // Include render-loop work between frames, too. CPU time excludes
        // sleeping/waiting; a swap interval is not CPU work.
        m_renderThreadCpuStart = nowCpu;
        // Coalesce telemetry too: an uncapped web/video source must never
        // build an unbounded queue while the GUI is handling an edit.
        const auto mailbox = m_frameMailbox;
        std::lock_guard lock(mailbox->mutex);
        if (!mailbox->owner) return;
        mailbox->latest = m_renderFrame;
        if (mailbox->queued) {
            // Coalescing diagnostics must not make CPU totals smaller or hide
            // a long submission gap. Count dropped samples explicitly.
            mailbox->interval = std::max(mailbox->interval, interval);
            if (renderThreadCpu >= 0) mailbox->cpu = std::max(0., mailbox->cpu) + renderThreadCpu;
            ++mailbox->coalesced;
            return;
        }
        mailbox->interval = interval; mailbox->cpu = renderThreadCpu;
        mailbox->queued = true;
        QMetaObject::invokeMethod(this, [this, mailbox] {
            FrameStats frame;
            double interval, cpu;
            quint64 coalesced;
            {
                std::lock_guard lock(mailbox->mutex);
                frame = mailbox->latest; interval = mailbox->interval; cpu = mailbox->cpu;
                coalesced = mailbox->coalesced; mailbox->coalesced = 0;
                mailbox->queued = false;
            }
            if (m_stopping) return;
            if (coalesced) ui::perf::sample("gpu.scene.telemetry.coalesced", double(coalesced));
            if (frame.gpuMs >= 0) {
                ui::perf::sample("gpu.scene.completed.gpu.ms", frame.gpuMs);
                m_window->setProperty("vlt.completedGpuMs", frame.gpuMs);
            }
            GraphicsPreferences::instance().reportFrame(frame);
            emit frameMeasured(frame.preparationMs, frame.synchronizationMs, frame.renderCpuMs, interval, cpu);
        }, Qt::QueuedConnection);
    }, Qt::DirectConnection);
    qApp->installEventFilter(this);
    resizeSurface();
    m_container->show();
    invalidate();
}
void WorkspaceSurface::refreshPresentationMode() {
    if (m_stopping || !m_window) return;
    const int interval = QSettings().value("ui/frameMode").toString() == "unlimited" ? 0 : 1;
    if (m_window->requestedFormat().swapInterval() == interval) return;
    if (m_capturing || m_pressed || m_dragTarget || m_nativeGesture || QApplication::mouseButtons() != Qt::NoButton) {
        m_presentationTimer.start(50);
        return;
    }
    // Recreate the native swapchain outside a gesture; the C++ editing model
    // and QQuickItem tree survive, and audio is not stopped or reconfigured.
    const bool wasVisible = m_window->isVisible();
    m_window->hide();
    m_window->releaseResources();
    m_window->destroy();
    auto format = m_window->requestedFormat(); format.setSwapInterval(interval);
    m_window->setFormat(format);
    m_window->create();
    if (wasVisible) m_window->show();
    invalidate();
}
void WorkspaceSurface::fail(const QString& reason) {
    if (m_stopping) return;
    m_stopping = true;
    FrameClock::instance().clearPresenter(m_source, this);
    m_capturePending = false;
    m_presentationTimer.stop();
    if (m_container) m_container->hide();
    if (m_source) { m_source->setProperty("vlt.gpuSurfaceActive", false); m_source->update(); }
    qWarning().noquote() << "GPU workspace fallback:" << reason;
    emit failed(reason);
}
WorkspaceSurface::~WorkspaceSurface() {
    m_stopping = true;
    {
        std::lock_guard lock(m_frameMailbox->mutex);
        m_frameMailbox->owner = nullptr;
    }
    FrameStats inactive; inactive.windowId = reinterpret_cast<quintptr>(this); inactive.active = false;
    GraphicsPreferences::instance().reportFrame(inactive);
    if (m_source) m_source->setProperty("vlt.gpuSurfaceActive", false);
    FrameClock::instance().clearPresenter(m_source, this);
    qApp->removeEventFilter(this);
    for (auto& [id, visual] : m_visuals)
        if (visual.source && visual.item && visual.item->parentItem() == visual.clip)
            visual.source->releaseItem(visual.item);
    if (m_container) delete m_container.data(); // owns Quick window and scene graph
    m_window = nullptr;
    if (m_source) m_source->update();
}
void WorkspaceSurface::invalidate() {
    if (!m_source || m_stopping) return;
    m_layers.clear();
    requestCapture();
}
void WorkspaceSurface::requestCapture() {
    if (m_stopping || m_capturePending) return;
    m_capturePending = true;
    m_window->update();
}
void WorkspaceSurface::resizeSurface() {
    if (!m_source || !m_container) return;
    m_container->setGeometry(m_source->rect());
    m_container->raise();
    // Native plugin hosts and the Quick container are sibling native views.
    // Restore the internal editor above the scene after a workspace resize;
    // its own raise()/activation continues to order multiple editors.
    for (QWidget* widget : m_source->findChildren<QWidget*>()) {
        if (widget->property("vlt.nativeOverlay").toBool() &&
            widget->isVisible() && !widget->isWindow()) {
            widget->raise();
        }
    }
    m_item->setSize(m_source->size());
    invalidate();
}
void WorkspaceSurface::visit(QWidget* widget, std::shared_ptr<SceneSnapshot>& snapshot, QSet<quintptr>& wanted) {
    if (m_stopping || !widget || widget == m_container || !widget->isVisible() ||
        (widget != m_source && widget->isWindow())) return;
    // Foreign plugin pixels stay in their native child hierarchy. The frame
    // is deliberately omitted as one unit so the scene never draws a stale
    // copy beneath the live editor or tries to traverse the vendor surface.
    if (widget->property("vlt.nativeOverlay").toBool()) return;
    if (widget->property("vlt.foreignSurface").toBool() || widget->inherits("QWindowContainer") ||
        widget->inherits("QOpenGLWidget")) {
        // A hosted plugin can attach a foreign NSView/HWND below a QWidget.
        // Its contents cannot be recorded, and our native surface would cover
        // it. Restore the original hierarchy instead of hiding its editor.
        // WA_NativeWindow alone is not enough: Qt also promotes ordinary
        // QWidget ancestors/siblings to native windows; their paint is recordable.
        fail(tr("An embedded native window requires compatibility rendering."));
        return;
    }
    const auto id = reinterpret_cast<quintptr>(widget);
    const QPoint origin = widget->mapTo(m_source, QPoint());
    QRect visible(origin, widget->size());
    for (auto* ancestor = widget == m_source ? nullptr : widget->parentWidget(); ancestor && ancestor != m_source;
         ancestor = ancestor->parentWidget())
        visible &= QRect(ancestor->mapTo(m_source, QPoint()), ancestor->size());
    visible &= m_source->rect();
    if (visible.isEmpty()) return;
    wanted.insert(id);
    auto found = m_layers.find(id);
    const QRectF clip = visible.translated(-origin);
    const bool rebuild = found == m_layers.end() || !found->second.widget || m_dirty.contains(widget);
    if (rebuild) {
        ui::perf::Scope layerCost(nativeControlAsset(widget) && !dynamic_cast<ScenePaintSource*>(widget)
            ? "gpu.scene.native.control.ms" : "gpu.scene.vector.layer.ms");
        auto recording = found != m_layers.end() && found->second.widget ? found->second.recording :
                         std::make_shared<SceneRecordingCache>();
        SceneRecorder recorder(widget->size(), m_window->devicePixelRatio(), recording);
        // Native web content is migrated separately through WebEngineQuick.
        // Do not turn QWidget::grab()/render-to-texture readbacks into a GPU path.
        if (widget->inherits("QWebEngineView") || widget->inherits("QQuickWidget")) {
            fail(tr("This experimental surface does not yet support embedded web views."));
            return;
        }
        auto layer = std::make_shared<SceneLayer>();
        layer->id = id; layer->revision = ++m_revision;
        layer->origin = origin;
        layer->clip = visible.translated(-origin);
        if (nativeControlAsset(widget) && !dynamic_cast<ScenePaintSource*>(widget)) {
            const auto dpr = m_window->devicePixelRatio();
            const QSize pixels = (QSizeF(widget->size()) * dpr).toSize();
            if (qint64(pixels.width()) * pixels.height() > 512 * 1024) {
                fail(tr("A native control exceeds the experimental texture size limit."));
                return;
            }
            QImage control(pixels, QImage::Format_ARGB32_Premultiplied);
            control.setDevicePixelRatio(dpr); control.fill(Qt::transparent);
            widget->render(&control, QPoint(), QRegion(), QWidget::RenderFlags());
            SceneMesh mesh;
            const float w = widget->width(), h = widget->height();
            mesh.vertices = {{0, 0, 0, 0}, {w, 0, 1, 0}, {0, h, 0, 1},
                             {w, 0, 1, 0}, {w, h, 1, 1}, {0, h, 0, 1}};
            mesh.texture = std::move(control);
            layer->meshes.push_back(std::move(mesh));
        } else {
            if (auto* canvas = dynamic_cast<ScenePaintSource*>(widget)) {
                QPainter painter(&recorder);
                painter.setFont(widget->font());
                canvas->paintScene(painter, QRegion(widget->rect()));
            } else widget->render(&recorder, QPoint(), QRegion(), QWidget::RenderFlags());
            if (!recorder.supported()) {
                fail(tr("A canvas uses a composition operation that is not supported by the experimental renderer."));
                return;
            }
            layer->meshes = recorder.takeMeshes();
        }
        layer->clipRequired = requiresLayerClip(*layer);
        m_layers[id] = {widget, layer, std::move(recording)};
    } else if (found->second.layer->origin != origin || found->second.layer->clip != clip) {
        auto moved = std::make_shared<SceneLayer>(*found->second.layer);
        moved->origin = origin;
        // Recording always covers the widget's full local rect. Scrolling
        // changes visibility, not its pixels or geometry.
        if (moved->clip != clip) {
            moved->clip = clip;
            moved->clipRequired = requiresLayerClip(*moved);
        }
        found->second.layer = std::move(moved);
    }
    snapshot->layers.push_back(m_layers[id].layer);
    const auto children = widget->children();
    for (auto* child : children) if (auto* childWidget = qobject_cast<QWidget*>(child)) visit(childWidget, snapshot, wanted);
}
void WorkspaceSurface::capture() {
    if (!m_source || !m_source->isVisible() || m_stopping) return;
    m_capturePending = false;
    m_capturing = true;
    QElapsedTimer prepare; prepare.start();
    auto snapshot = std::make_shared<SceneSnapshot>();
    snapshot->revision = ++m_revision; snapshot->viewport = m_source->size();
    if (m_projectRevision) snapshot->projectRevision = m_projectRevision();
    QSet<quintptr> wanted;
    visit(m_source, snapshot, wanted);
    if (m_stopping) { m_capturing = false; return; }
    for (auto it = m_layers.begin(); it != m_layers.end();) {
        if (!wanted.contains(it->first)) it = m_layers.erase(it); else ++it;
    }
    m_dirty.clear();
    m_scrollExposure.clear();
    m_collectedScrollUpdates = false;
    m_capturing = false;
    m_window->setProperty("vlt.sceneLayers", int(snapshot->layers.size()));
    int meshCount = 0;
    qint64 geometryBytes = 0;
    for (const auto& layer : snapshot->layers) {
        meshCount += int(layer->meshes.size());
        for (const auto& mesh : layer->meshes) {
            geometryBytes += qint64(mesh.vertices.size()) * sizeof(SceneVertex);
            if (mesh.group) geometryBytes += mesh.group->bytes;
            if (mesh.clip) geometryBytes += qint64(mesh.clip->triangles.size()) * sizeof(SceneVertex);
        }
    }
    if (geometryBytes > 64 * 1024 * 1024) {
        fail(tr("The visible scene exceeds the experimental geometry budget."));
        return;
    }
    m_window->setProperty("vlt.sceneMeshes", meshCount);
    FrameStats frame;
    frame.windowId = reinterpret_cast<quintptr>(this);
    frame.preparationMs = prepare.nsecsElapsed() / 1e6;
    frame.displayHz = m_window->screen() ? m_window->screen()->refreshRate() : 60;
    if (m_frameContext) m_frameContext(frame);
    frame.frameBudgetMs = FrameClock::instance().periodSeconds(m_source) * 1000.;
    m_preparedFrame = frame;
    m_preparedRevision = snapshot->revision;
    present(std::move(snapshot));
}
void WorkspaceSurface::present(std::shared_ptr<const SceneSnapshot> snapshot) {
    // Split only at native Quick visuals. Ordinary vector layers remain in a
    // shared batching root. This keeps VideoOutput/WebEngine in the same scene
    // and preserves the exact painter order, including overlays above media.
    std::size_t segment = 0;
    qreal z = 0;
    QSet<quint64> wanted;
    auto batch = std::make_shared<SceneSnapshot>();
    batch->revision = snapshot->revision;
    batch->projectRevision = snapshot->projectRevision;
    batch->viewport = snapshot->viewport;
    const auto flush = [&] {
        if (batch->layers.empty()) return;
        if (segment == m_segments.size()) {
            auto* item = new SceneItem(m_window->contentItem());
            connect(item, &SceneItem::resourceError, this, &WorkspaceSurface::fail, Qt::QueuedConnection);
            m_segments.push_back(item);
        }
        auto* item = m_segments[segment++];
        item->setSize(snapshot->viewport);
        item->setZ(z++);
        item->setVisible(true);
        item->setSnapshot(batch);
        batch = std::make_shared<SceneSnapshot>();
        batch->revision = snapshot->revision;
        batch->projectRevision = snapshot->projectRevision;
        batch->viewport = snapshot->viewport;
    };
    for (const auto& layer : snapshot->layers) {
        const bool hasVisual = std::any_of(layer->meshes.begin(), layer->meshes.end(),
            [](const SceneMesh& mesh) { return mesh.visualId != 0; });
        if (!hasVisual) { batch->layers.push_back(layer); continue; }
        auto part = std::make_shared<SceneLayer>(*layer);
        part->meshes.clear();
        for (const auto& mesh : layer->meshes) {
            if (!mesh.visualId) { part->meshes.push_back(mesh); continue; }
            if (!part->meshes.empty()) batch->layers.push_back(part);
            flush();
            part = std::make_shared<SceneLayer>(*layer);
            part->meshes.clear();
            auto* source = QuickVisual::find(mesh.visualId);
            if (!source) continue;
            wanted.insert(mesh.visualId);
            auto& visual = m_visuals[mesh.visualId];
            if (!visual.item) {
                if (!m_qml) m_qml = new QQmlEngine(this);
                visual.source = source;
                visual.clip = new QQuickItem(m_window->contentItem());
                visual.clip->setClip(true);
                visual.item = source->createItem(m_qml, visual.clip);
                if (!visual.item) { fail(tr("A GPU media component could not be created.")); return; }
            }
            QRectF clip = layer->clip.intersected(mesh.visualRect);
            if (mesh.clip) clip &= mesh.clip->bounds;
            visual.clip->setPosition(layer->origin + clip.topLeft());
            visual.clip->setSize(clip.size());
            visual.clip->setZ(z++);
            visual.clip->setOpacity(mesh.opacity);
            visual.clip->setVisible(!clip.isEmpty());
            visual.item->setPosition(mesh.visualRect.topLeft() - clip.topLeft());
            visual.item->setSize(mesh.visualRect.size());
        }
        if (!part->meshes.empty()) batch->layers.push_back(part);
    }
    flush();
    while (m_segments.size() > std::max<std::size_t>(1, segment)) {
        delete m_segments.back(); m_segments.pop_back();
    }
    if (!segment) { m_item->setVisible(false); m_item->setSnapshot({}); }
    for (auto it = m_visuals.begin(); it != m_visuals.end();) {
        if (wanted.contains(it->first)) { ++it; continue; }
        // A persistent browser page may already have joined a detached window
        // before this window consumes the removal. Only release our own item;
        // otherwise late cleanup hides/unparents the new window's live page.
        if (it->second.source && it->second.item && it->second.item->parentItem() == it->second.clip)
            it->second.source->releaseItem(it->second.item);
        delete it->second.clip;
        it = m_visuals.erase(it);
    }
}
void WorkspaceSurface::updateHover(QWidget* target, const QPointF& globalPosition,
                                   Qt::KeyboardModifiers modifiers, const QPointingDevice* device) {
    if (m_hover == target) return;
    // Enter/Leave alone do not repaint stylesheet hover states. Match the
    // QWidget boundary protocol, including HoverEnter/HoverLeave and ancestors
    // up to the common parent. Crossing a slot's action must not leave its row.
    std::vector<QPointer<QWidget>> leaving, entering;
    const auto path = [this](QWidget* leaf, auto& widgets) {
        for (auto* widget = leaf; widget; widget = widget->parentWidget()) {
            widgets.push_back(widget);
            if (widget == m_source) break;
        }
    };
    path(m_hover, leaving);
    path(target, entering);
    while (!leaving.empty() && !entering.empty() && leaving.back() == entering.back()) {
        leaving.pop_back(); entering.pop_back();
    }
    m_hover = target;
    if (!device) device = QPointingDevice::primaryPointingDevice();
    for (const auto& widget : leaving) {
        if (!widget) continue;
        QEvent leave(QEvent::Leave);
        QCoreApplication::sendEvent(widget, &leave);
        if (widget && widget->testAttribute(Qt::WA_Hover)) {
            QHoverEvent hover(QEvent::HoverLeave, QPointF(-1, -1), globalPosition,
                              widget->mapFromGlobal(globalPosition), modifiers, device);
            QCoreApplication::sendEvent(widget, &hover);
        }
        if (widget) m_dirty.insert(widget);
    }
    for (auto it = entering.rbegin(); it != entering.rend(); ++it) {
        const auto& widget = *it;
        if (!widget) continue;
        const auto local = widget->mapFromGlobal(globalPosition);
        QEnterEvent enter(local, widget->window()->mapFromGlobal(globalPosition), globalPosition, device);
        QCoreApplication::sendEvent(widget, &enter);
        if (widget && widget->testAttribute(Qt::WA_Hover)) {
            QHoverEvent hover(QEvent::HoverEnter, local, globalPosition, QPointF(-1, -1), modifiers, device);
            QCoreApplication::sendEvent(widget, &hover);
        }
        if (widget) m_dirty.insert(widget);
    }
    requestCapture();
}
bool WorkspaceSurface::forwardInput(QEvent* event) {
    if (!m_source) return false;
    if (event->type() == QEvent::DragEnter || event->type() == QEvent::UngrabMouse ||
        event->type() == QEvent::WindowDeactivate) {
        // Native QDrag takes over the gesture and consumes the release. The
        // compatibility widget must lose its implicit grab too; otherwise a
        // queued pressed move after the drop goes back to the old insert.
        const QPointer<QWidget> pressed = m_pressed;
        m_pressed = nullptr;
        m_quickGrabVisual = 0;
        if (pressed) {
            QEvent ungrab(QEvent::UngrabMouse);
            QCoreApplication::sendEvent(pressed, &ungrab);
        }
        if (event->type() == QEvent::DragEnter) updateHover(nullptr, QCursor::pos());
    }
    if (auto* mouse = dynamic_cast<QMouseEvent*>(event);
        mouse && startsFreshPointerRoute(mouse->type(), mouse->buttons())) {
        // Repair grabs before deciding whether an interactive Quick child owns
        // this event. Otherwise a popup-stale QWidget grab can also block a
        // fresh click from reaching an embedded browser or media control.
        m_pressed = nullptr;
        m_quickGrabVisual = 0;
    }
    if (m_quickGrabVisual && (event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonRelease)) {
        if (event->type() == QEvent::MouseButtonRelease &&
            static_cast<QMouseEvent*>(event)->buttons() == Qt::NoButton) m_quickGrabVisual = 0;
        return false; // Preserve Chromium's drag/selection grab outside its bounds.
    }
    // Interactive Quick children (notably Chromium) own their pointer, IME,
    // accessibility and keyboard delivery. Forwarding those events through a
    // placeholder QWidget would discard composition and page gestures.
    QPointF scenePosition;
    bool positional = false;
    if (auto* pointer = dynamic_cast<QSinglePointEvent*>(event)) {
        scenePosition = pointer->position(); positional = true;
    } else if (event->type() == QEvent::ContextMenu) {
        scenePosition = static_cast<QContextMenuEvent*>(event)->pos(); positional = true;
    }
    // WebEngine's standard menus/dialogs may live in Quick's window overlay,
    // outside the page rectangle. Give those Quick-owned roots their events.
    for (auto* child : m_window->contentItem()->childItems()) {
        if (!child->isVisible()) continue;
        bool known = std::find(m_segments.begin(), m_segments.end(), child) != m_segments.end();
        for (const auto& [id, visual] : m_visuals) known |= visual.clip == child;
        if (known) continue;
        auto* focus = m_window->activeFocusItem();
        if ((positional && child->contains(child->mapFromScene(scenePosition))) ||
            (focus && (focus == child || child->isAncestorOf(focus)))) return false;
    }
    if (event->type() == QEvent::NativeGesture) {
        const auto type = static_cast<QNativeGestureEvent*>(event)->gestureType();
        if (type == Qt::BeginNativeGesture) m_nativeGesture = true;
        if (type == Qt::EndNativeGesture) m_nativeGesture = false;
    }
    if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove || event->type() == QEvent::Drop) {
        scenePosition = static_cast<QDropEvent*>(event)->position(); positional = true;
    }
    if (!m_pressed && !m_dragTarget) for (const auto& [id, visual] : m_visuals) {
        if (!visual.source || !visual.source->interactive() || !visual.item || !visual.clip->isVisible()) continue;
        auto* focus = m_window->activeFocusItem();
        auto* owner = qobject_cast<QWidget*>(visual.source->parent());
        auto* hit = positional ? m_source->childAt(scenePosition.toPoint()) : nullptr;
        if (positional && !hit) hit = m_source;
        const bool ownsHit = owner && owner == hit;
        const bool inVisual = positional && ownsHit && visual.clip->contains(visual.clip->mapFromScene(scenePosition));
        const bool keyboard = event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease ||
            event->type() == QEvent::ShortcutOverride || event->type() == QEvent::InputMethod ||
            event->type() == QEvent::InputMethodQuery;
        auto* widgetFocus = QApplication::focusWidget();
        const bool ownsFocus = owner && owner == widgetFocus;
        if (inVisual || (keyboard && ownsFocus && focus && (focus == visual.item || visual.item->isAncestorOf(focus)))) {
            updateHover(nullptr, QCursor::pos());
            if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick) {
                m_quickGrabVisual = id;
                m_container->setFocusProxy(owner);
                owner->setFocus(Qt::MouseFocusReason);
                m_window->requestActivate();
                visual.item->forceActiveFocus(Qt::MouseFocusReason);
            }
            return false;
        }
    }
    const auto targetAt = [&](QPointF position) -> QWidget* {
        auto* target = m_source->childAt(position.toPoint());
        return target && target != m_container ? target : m_source.data();
    };
    switch (event->type()) {
    case QEvent::Leave: case QEvent::WindowDeactivate: {
        updateHover(nullptr, QCursor::pos());
        return false;
    }
    case QEvent::MouseButtonPress: case QEvent::MouseButtonDblClick:
    case QEvent::MouseMove: case QEvent::MouseButtonRelease: {
        auto* mouse = static_cast<QMouseEvent*>(event);
        QPointer<QWidget> target = pointerTarget(m_pressed, mouse->type(), mouse->buttons(),
                                                 targetAt(mouse->position()));
        if (!m_pressed) updateHover(target, mouse->globalPosition(), mouse->modifiers(), mouse->pointingDevice());
        if (!target) return true;
        if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick) {
            m_pressed = target;
            if (target->focusPolicy() & Qt::ClickFocus) {
                m_container->setFocusProxy(target);
                target->setFocus(Qt::MouseFocusReason);
            }
        }
        const auto local = target->mapFrom(m_source, mouse->position());
        QMouseEvent forwarded(mouse->type(), local, target->mapTo(target->window(), local), mouse->globalPosition(),
            mouse->button(), mouse->buttons(), mouse->modifiers(), mouse->source(), mouse->pointingDevice());
        forwarded.setTimestamp(mouse->timestamp());
        QCoreApplication::sendEvent(target, &forwarded);
#ifdef Q_OS_MACOS
        // A native QWidget window normally turns a secondary-button press into
        // QContextMenuEvent.  The Quick window is now the native receiver, so
        // forwarding only QMouseEvent silently skips that platform synthesis.
        // Deliver the context event at the same point and remember it so a
        // platform-generated duplicate can be discarded below.
        if (target && event->type() == QEvent::MouseButtonPress &&
            mouse->button() == Qt::RightButton) {
            const QPoint global = mouse->globalPosition().toPoint();
            m_pressed = nullptr;
            m_syntheticContextMenuGlobal = global;
            m_syntheticContextMenuClock.restart();
            QContextMenuEvent context(QContextMenuEvent::Mouse, local.toPoint(),
                                      global, mouse->modifiers());
            QCoreApplication::sendEvent(target, &context);
        }
#endif
        if (target && event->type() == QEvent::MouseMove && target->testAttribute(Qt::WA_Hover)) {
            QHoverEvent hover(QEvent::HoverMove, local, mouse->globalPosition(),
                target->mapFrom(m_source, m_lastHoverPosition), mouse->modifiers(), mouse->pointingDevice());
            QCoreApplication::sendEvent(target, &hover);
        }
        m_lastHoverPosition = mouse->position();
        if (target) m_window->setCursor(target->cursor());
        if (event->type() == QEvent::MouseButtonRelease && mouse->buttons() == Qt::NoButton) {
            m_pressed = nullptr;
            updateHover(m_source->rect().contains(mouse->position().toPoint()) ? targetAt(mouse->position()) : nullptr,
                        mouse->globalPosition(), mouse->modifiers(), mouse->pointingDevice());
        }
        return true;
    }
    case QEvent::Wheel: {
        auto* wheel = static_cast<QWheelEvent*>(event);
        auto* target = targetAt(wheel->position());
        event->setAccepted(routeWheelThroughWidgets(m_source, target, wheel));
        // The content can move under a stationary pointer during scrolling.
        if (!m_pressed) updateHover(targetAt(wheel->position()), wheel->globalPosition(),
                                   wheel->modifiers(), wheel->pointingDevice());
        return true;
    }
    case QEvent::ContextMenu: {
        auto* menu = static_cast<QContextMenuEvent*>(event);
#ifdef Q_OS_MACOS
        if (menu->reason() == QContextMenuEvent::Mouse &&
            m_syntheticContextMenuClock.isValid() &&
            m_syntheticContextMenuClock.elapsed() < 1000 &&
            (menu->globalPos() - m_syntheticContextMenuGlobal).manhattanLength() <= 2) {
            m_syntheticContextMenuClock.invalidate();
            return true;
        }
        m_syntheticContextMenuClock.invalidate();
#endif
        auto* target = targetAt(menu->pos());
        QContextMenuEvent forwarded(menu->reason(), target->mapFrom(m_source, menu->pos()), menu->globalPos(), menu->modifiers());
        QCoreApplication::sendEvent(target, &forwarded); return true;
    }
    case QEvent::NativeGesture: {
        auto* gesture = static_cast<QNativeGestureEvent*>(event);
        if (gesture->gestureType() == Qt::BeginNativeGesture) m_nativeGesture = true;
        if (gesture->gestureType() == Qt::EndNativeGesture) m_nativeGesture = false;
        auto* target = targetAt(gesture->position());
        const auto local = target->mapFrom(m_source, gesture->position());
        QNativeGestureEvent forwarded(gesture->gestureType(), gesture->pointingDevice(), gesture->fingerCount(),
            local, target->mapTo(target->window(), local), gesture->globalPosition(), gesture->value(), gesture->delta());
        forwarded.setTimestamp(gesture->timestamp());
        QCoreApplication::sendEvent(target, &forwarded);
        event->setAccepted(forwarded.isAccepted()); return true;
    }
    case QEvent::ToolTip: {
        auto* help = static_cast<QHelpEvent*>(event);
        auto* target = targetAt(help->pos());
        QHelpEvent forwarded(help->type(), target->mapFrom(m_source, help->pos()), help->globalPos());
        QCoreApplication::sendEvent(target, &forwarded); return true;
    }
    case QEvent::DragEnter: case QEvent::DragMove: case QEvent::Drop: {
        auto* drop = static_cast<QDropEvent*>(event);
        QPointer<QWidget> target = targetAt(drop->position());
        while (target && !target->acceptDrops()) {
            if (target == m_source) { target = nullptr; break; }
            target = target->parentWidget();
        }
        if (target != m_dragTarget) {
            if (m_dragTarget) { QDragLeaveEvent leave; QCoreApplication::sendEvent(m_dragTarget, &leave); }
            m_dragTarget = target;
            if (target && event->type() != QEvent::DragEnter) {
                QDragEnterEvent enter(target->mapFrom(m_source, drop->position()).toPoint(), drop->possibleActions(),
                    drop->mimeData(), drop->buttons(), drop->modifiers());
                QCoreApplication::sendEvent(target, &enter);
            }
        }
        if (!target) { event->ignore(); return true; }
        const auto local = target->mapFrom(m_source, drop->position());
        std::unique_ptr<QDropEvent> forwarded;
        if (event->type() == QEvent::DragEnter)
            forwarded = std::make_unique<QDragEnterEvent>(local.toPoint(), drop->possibleActions(), drop->mimeData(), drop->buttons(), drop->modifiers());
        else if (event->type() == QEvent::DragMove)
            forwarded = std::make_unique<QDragMoveEvent>(local.toPoint(), drop->possibleActions(), drop->mimeData(), drop->buttons(), drop->modifiers());
        else
            forwarded = std::make_unique<QDropEvent>(local, drop->possibleActions(), drop->mimeData(), drop->buttons(), drop->modifiers());
        forwarded->setDropAction(drop->dropAction());
        QCoreApplication::sendEvent(target, forwarded.get());
        drop->setDropAction(forwarded->dropAction());
        drop->setAccepted(forwarded->isAccepted());
        if (event->type() == QEvent::Drop) m_dragTarget = nullptr;
        return true;
    }
    case QEvent::DragLeave: {
        if (m_dragTarget) { QDragLeaveEvent leave; QCoreApplication::sendEvent(m_dragTarget, &leave); }
        m_dragTarget = nullptr; return true;
    }
    case QEvent::KeyPress: case QEvent::KeyRelease: case QEvent::ShortcutOverride:
    case QEvent::InputMethod: {
        auto* target = QApplication::focusWidget();
        if (target && target != m_container && (target == m_source || m_source->isAncestorOf(target))) {
            QCoreApplication::sendEvent(target, event); return true;
        }
        break;
    }
    case QEvent::InputMethodQuery: {
        auto* target = QApplication::focusWidget();
        if (target && target != m_container && (target == m_source || m_source->isAncestorOf(target))) {
            auto* query = static_cast<QInputMethodQueryEvent*>(event);
            QCoreApplication::sendEvent(target, query);
            if (query->queries().testFlag(Qt::ImCursorRectangle)) {
                const QRectF cursor = query->value(Qt::ImCursorRectangle).toRectF();
                query->setValue(Qt::ImCursorRectangle, cursor.translated(target->mapTo(m_source, QPoint())));
            }
            return true;
        }
        break;
    }
    default: break;
    }
    return false;
}
bool WorkspaceSurface::eventFilter(QObject* object, QEvent* event) {
    if (m_stopping || m_capturing || !m_source) return false;
    if (event->type() == QEvent::UpdateRequest && object == m_source->window() && !m_scrollExposure.isEmpty())
        m_collectedScrollUpdates = collectWidgetUpdates(m_source, m_dirty);
    if ((object == m_window || object == m_source) && event->type() == QEvent::DevicePixelRatioChange)
        invalidate();
    if (object == m_window || object == m_container) return forwardInput(event);
    auto* widget = qobject_cast<QWidget*>(object);
    if (!widget || widget == m_container || (widget != m_source && !m_source->isAncestorOf(widget))) return false;
    // These widgets paint and receive input through their real native child
    // view. Consuming Paint here would leave the embedded plugin transparent.
    if (belongsToNativeOverlay(widget, m_source)) return false;
    // Native menus, tooltips and detached editors retain their own renderer.
    if (widget->window() != m_source->window()) return false;
    // Tab navigation and programmatic focus changes must update the same proxy
    // as mouse input. Otherwise activating Quick restores the last clicked
    // QWidget and silently steals focus from a browser/notebook editor.
    if (event->type() == QEvent::FocusIn) m_container->setFocusProxy(widget);
    if (widget == m_source && event->type() == QEvent::Hide) {
        FrameStats inactive; inactive.windowId = reinterpret_cast<quintptr>(this); inactive.active = false;
        GraphicsPreferences::instance().reportFrame(inactive);
    }
    if (event->type() == QEvent::Paint) {
        bool exposureOnly = false;
        if (m_collectedScrollUpdates) {
            for (const auto& page : m_scrollExposure)
                if (page && (widget == page || page->isAncestorOf(widget))) { exposureOnly = true; break; }
        }
        if (!exposureOnly) m_dirty.insert(widget);
        requestCapture();
        return true; // vector recording occurs outside QWidget's active paint stack
    }
    if (event->type() == QEvent::Destroy) {
        m_dirty.remove(widget); m_scrollExposure.removeAll(widget);
        m_layers.erase(reinterpret_cast<quintptr>(widget));
    }
    if (event->type() == QEvent::Move) {
        auto* viewport = widget->parentWidget();
        auto* scroll = viewport ? qobject_cast<QScrollArea*>(viewport->parentWidget()) : nullptr;
        if (scroll && scroll->widget() == widget) {
            if (!m_scrollExposure.contains(widget)) m_scrollExposure.append(widget);
            m_collectedScrollUpdates = false;
        }
    }
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) {
        const auto found = m_layers.find(reinterpret_cast<quintptr>(widget));
        if (found != m_layers.end() && found->second.recording) found->second.recording->sections.clear();
    }
    if (event->type() == QEvent::Resize || event->type() == QEvent::Show || event->type() == QEvent::Move ||
        event->type() == QEvent::Hide || event->type() == QEvent::ZOrderChange || event->type() == QEvent::PaletteChange ||
        event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) {
        if (widget == m_source && event->type() == QEvent::Resize) resizeSurface();
        else {
            if (event->type() != QEvent::Move && event->type() != QEvent::ZOrderChange) m_dirty.insert(widget);
            requestCapture();
        }
    }
    return false;
}
} // namespace ui::graphics
