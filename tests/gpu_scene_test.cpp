#include "graphics/SceneRecorder.hpp"
#include "graphics/WorkspaceSurface.hpp"
#include "graphics/GraphicsPreferences.hpp"
#include "graphics/RetainedScene.hpp"
#include "UiFrameClock.hpp"
#include <QApplication>
#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QSettings>
#include <QScrollArea>
#include <QScrollBar>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QWidget>
#include <atomic>
#include <cmath>
#include <iostream>

class Canvas final : public ui::FrameWidget {
public:
    using ui::FrameWidget::FrameWidget;
    QPointF lastMouse;
    QPoint wheelPixels;
    QPoint contextPosition;
    int contextMenus = 0;
    Qt::ScrollPhase wheelPhase = Qt::NoScrollPhase;
    ulong inputTimestamp = 0;
    bool overflow = false;
protected:
    void mousePressEvent(QMouseEvent* event) override { lastMouse = event->position(); inputTimestamp = event->timestamp(); }
    void wheelEvent(QWheelEvent* event) override { wheelPixels = event->pixelDelta(); wheelPhase = event->phase(); }
    void contextMenuEvent(QContextMenuEvent* event) override {
        contextPosition = event->pos();
        ++contextMenus;
        event->accept();
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        auto* scene = ui::graphics::sceneGeometrySink(p);
        if (!scene || scene->beginRetainedSection(101, false)) {
            p.fillRect(rect(), palette().color(QPalette::Window));
            if (scene) scene->endRetainedSection();
        }
        p.setPen(Qt::NoPen); p.setBrush(QColor(80, 160, 240));
        p.drawRoundedRect(QRectF(20, 20, 120, 60), 10, 10);
        p.setPen(Qt::white); p.drawText(QPointF(25, 50), QStringLiteral("Timeline 01"));
        p.setClipRect(QRectF(160, 20, 50, 60));
        p.fillRect(QRectF(150, 10, 80, 90), QColor(240, 100, 60));
        if (scene) {
            QPainterPath rounded; rounded.addRoundedRect(QRectF(220, 90, 30, 30), 10, 10);
            p.setClipPath(rounded);
            static const QVector<ui::graphics::SceneVertex> tile = {
                {0, 0, 0, 0}, {30, 0, 0, 0}, {0, 30, 0, 0},
                {30, 0, 0, 0}, {30, 30, 0, 0}, {0, 30, 0, 0}};
            scene->appendLocalGeometry(tile, QPointF(220, 90), QColor(60, 200, 120));
        }
        p.setClipping(false);
        if (overflow) p.fillRect(QRectF(305, 60, 40, 20), Qt::magenta);
    }
};
class PlainControl final : public QWidget {
public:
    using QWidget::QWidget;
    QColor color = Qt::red;
    int paints = 0;
protected:
    void paintEvent(QPaintEvent*) override { ++paints; QPainter painter(this); painter.fillRect(rect(), color); }
};
class HoverRow final : public QWidget {
public:
    using QWidget::QWidget;
    int enters = 0, leaves = 0;
protected:
    void enterEvent(QEnterEvent*) override { ++enters; }
    void leaveEvent(QEvent*) override { ++leaves; }
};
class DragControl final : public QWidget {
public:
    using QWidget::QWidget;
    int ungrabs = 0, moves = 0, drops = 0;
protected:
    bool event(QEvent* event) override {
        if (event->type() == QEvent::UngrabMouse) ++ungrabs;
        return QWidget::event(event);
    }
    void mousePressEvent(QMouseEvent* event) override { event->accept(); }
    void mouseMoveEvent(QMouseEvent* event) override { ++moves; event->accept(); }
    void dragEnterEvent(QDragEnterEvent* event) override { event->acceptProposedAction(); }
    void dropEvent(QDropEvent* event) override { ++drops; event->acceptProposedAction(); }
};
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QTemporaryDir preferences;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, preferences.path());
    app.setOrganizationName("VltGpuTest"); app.setApplicationName("Scene");
    const bool overrideWasSet = qEnvironmentVariableIsSet("VLT_GPU_WORKSPACE");
    const auto savedOverride = qgetenv("VLT_GPU_WORKSPACE");
    qunsetenv("VLT_GPU_WORKSPACE");
    if (ui::graphics::gpuWorkspaceEnabled()) return 1;
    QSettings().setValue("ui/frameMode", "fixed");
    QSettings().setValue("ui/frameLimit", 144);
    QSettings().setValue("ui/gpuWorkspace", true);
    if (!ui::graphics::gpuWorkspaceEnabled()) return 1;
    qputenv("VLT_GPU_WORKSPACE", "0");
    if (ui::graphics::gpuWorkspaceEnabled() || !QSettings().value("ui/gpuWorkspace").toBool() ||
        QSettings().value("ui/frameMode").toString() != "fixed" || QSettings().value("ui/frameLimit").toInt() != 144)
        return 1;
    if (overrideWasSet) qputenv("VLT_GPU_WORKSPACE", savedOverride); else qunsetenv("VLT_GPU_WORKSPACE");
    QSettings().clear();
    ui::graphics::SceneRecorder recorder(QSize(320, 140), 2);
    {
        QPainter p(&recorder);
        p.setPen(Qt::NoPen); p.setBrush(Qt::red);
        p.drawRoundedRect(QRectF(10, 10, 30, 20), 4, 4);
        p.setPen(Qt::white); p.drawText(QPointF(5, 70), QStringLiteral("MIDI"));
    }
    const auto meshes = recorder.takeMeshes();
    if (meshes.empty() || !recorder.supported() || meshes.front().vertices.isEmpty() || !meshes.front().texture.isNull()) return 1;
    ui::graphics::SceneRecorder hidden(QSize(320, 140), 2);
    {
        QPainter p(&hidden);
        QImage asset(16, 16, QImage::Format_ARGB32_Premultiplied); asset.fill(Qt::red);
        p.setClipRect(QRectF(200, 100, 50, 30));
        p.drawImage(QPointF(1, 2), asset);
        p.setClipRect(QRectF());
        p.fillRect(QRectF(1, 2, 20, 30), Qt::green);
    }
    if (!hidden.takeMeshes().empty()) { std::cerr << "A fully clipped primitive must produce no geometry\n"; return 1; }
    // Clip state stays fixed in device coordinates through transforms and is
    // restored after nested clipping. Image UVs must still select the source
    // subrectangle after CPU rectangle clipping (and later atlas placement).
    ui::graphics::SceneRecorder clippedImage(QSize(100, 100), 2);
    {
        QPainter p(&clippedImage);
        QImage asset(40, 40, QImage::Format_ARGB32_Premultiplied); asset.fill(Qt::red);
        p.setClipRect(QRectF(10, 10, 20, 20));
        p.drawImage(QRectF(0, 0, 40, 40), asset, QRectF(8, 8, 24, 24));
        p.save(); p.translate(10, 0);
        p.drawImage(QRectF(0, 0, 40, 40), asset, QRectF(8, 8, 24, 24));
        p.setClipRect(QRectF()); p.drawImage(QPointF(10, 10), asset);
        p.restore();
        p.drawImage(QRectF(0, 0, 40, 40), asset, QRectF(8, 8, 24, 24));
    }
    const auto clippedImages = clippedImage.takeMeshes();
    if (clippedImages.size() != 3 || clippedImages[0].vertices != clippedImages[2].vertices) return 1;
    for (std::size_t i = 0; i < clippedImages.size(); ++i) for (const auto& v : clippedImages[i].vertices) {
        const float expectedU = (8 + (v.x - (i == 1 ? 10 : 0)) * .6f) / 40;
        if (v.x < 10 || v.x > 30 || v.y < 10 || v.y > 30 || std::abs(v.u - expectedU) > .00001f) {
            std::cerr << "Image clipping/UV mapping changed across painter state changes\n"; return 1;
        }
    }
    ui::graphics::SceneLayer clipLayer;
    clipLayer.clip = QRectF(0, 0, 30, 30);
    clipLayer.meshes = {clippedImages.front()};
    if (ui::graphics::requiresLayerClip(clipLayer)) return 1;
    clipLayer.meshes.front().transform.translate(1, 0);
    if (!ui::graphics::requiresLayerClip(clipLayer)) return 1;
    ui::graphics::SceneRecorder gradientPass(QSize(320, 140), 2);
    {
        QPainter p(&gradientPass);
        QLinearGradient gradient(0, 20, 0, 80);
        gradient.setColorAt(0, Qt::red); gradient.setColorAt(1, Qt::blue);
        p.fillRect(QRectF(10, 20, 100, 60), gradient);
        p.fillRect(QRectF(35, 20, 100, 60), gradient);
        p.fillRect(QRectF(35, 40, 100, 40), gradient);
    }
    const auto ramps = gradientPass.takeMeshes();
    if (ramps.size() != 3 || ramps[0].texture.isNull() || ramps[0].texture.cacheKey() != ramps[1].texture.cacheKey() ||
        ramps[0].texture.cacheKey() != ramps[2].texture.cacheKey()) {
        std::cerr << "Scrolling an identical gradient recreated its texture\n"; return 1;
    }
    auto retained = std::make_shared<ui::graphics::SceneRecordingCache>();
    const ui::graphics::SceneVertex* original = nullptr;
    for (int frame = 0; frame < 2; ++frame) {
        ui::graphics::SceneRecorder pass(QSize(320, 140), 2, retained);
        {
            QPainter p(&pass);
            auto* sink = ui::graphics::sceneGeometrySink(p);
            const bool needsDrawing = sink->beginRetainedSection(42, frame == 0);
            if (needsDrawing != (frame == 0)) return 1;
            if (needsDrawing) { p.fillRect(QRectF(1, 2, 20, 30), Qt::red); sink->endRetainedSection(); }
        }
        const auto recorded = pass.takeMeshes();
        if (recorded.size() != 1) return 1;
        if (!original) original = recorded.front().vertices.constData();
        else if (recorded.front().vertices.constData() != original) return 1;
    }
    for (const auto viewport : {std::pair{QSize(321, 140), 2.}, std::pair{QSize(321, 140), 1.}}) {
        ui::graphics::SceneRecorder pass(viewport.first, viewport.second, retained);
        QPainter p(&pass);
        auto* sink = ui::graphics::sceneGeometrySink(p);
        if (!sink->beginRetainedSection(42, false)) {
            std::cerr << "A size/DPI change replayed stale geometry\n"; return 1;
        }
        p.fillRect(QRect(0, 0, 20, 20), Qt::red);
        sink->endRetainedSection();
    }
    ui::graphics::RetainedScene tiles;
    std::shared_ptr<const ui::graphics::SceneMeshGroup> firstGroup;
    for (int pan = 0; pan < 100; ++pan) {
        ui::graphics::SceneRecorder pass(QSize(320, 140), 2);
        QPainter p(&pass);
        p.setClipRect(QRectF(20, 20, 100, 80));
        tiles.paint(p, 123, QSize(320, 140), QPointF(-pan * 0.125, 0), [](QPainter& tile) {
            tile.fillRect(QRectF(1, 2, 200, 100), Qt::green);
        });
        p.end();
        const auto result = pass.takeMeshes();
        if (result.size() != 1 || !result.front().group || !result.front().clip ||
            result.front().transform.dx() != -pan * 0.125) return 1;
        if (!firstGroup) firstGroup = result.front().group;
        if (firstGroup != result.front().group) return 1;
    }
    if (tiles.builds() != 1) return 1;
    tiles.clear();
    ui::graphics::SceneRecorder rebuilt(QSize(320, 140), 2);
    {
        QPainter p(&rebuilt);
        tiles.paint(p, 123, QSize(320, 140), {}, [](QPainter& tile) { tile.fillRect(QRect(0, 0, 20, 20), Qt::red); });
    }
    if (rebuilt.takeMeshes().front().group == firstGroup || tiles.builds() != 2) return 1;
    const bool softwareFallback = app.arguments().contains("--software-fallback");
    if (!app.arguments().contains("--hardware") && !softwareFallback) return 0;
    QWidget window;
    window.resize(320, 140);
    auto* canvas = new Canvas(&window); canvas->setGeometry(window.rect());
    // Qt promotes ordinary widgets when a native sibling/ancestor is present.
    // That must not be mistaken for a foreign plugin surface.
    QWidget ordinaryNative(canvas);
    ordinaryNative.setGeometry(260, 5, 40, 10);
    ordinaryNative.setAttribute(Qt::WA_NativeWindow);
    auto palette = canvas->palette(); palette.setColor(QPalette::Window, QColor(18, 22, 28)); canvas->setPalette(palette);
    auto* surface = new ui::graphics::WorkspaceSurface(canvas);
    std::atomic<bool> separateThread{false};
    QObject::connect(surface->quickWindow(), &QQuickWindow::sceneGraphInitialized, surface, [&] {
        separateThread.store(QThread::currentThread() != app.thread());
    }, Qt::DirectConnection);
    bool failed = false;
    QObject::connect(surface, &ui::graphics::WorkspaceSurface::failed, &app, [&](const QString& message) {
        std::cerr << message.toStdString() << '\n'; failed = true;
    });
    window.show();
    QEventLoop loop; QTimer::singleShot(1200, &loop, &QEventLoop::quit); loop.exec();
    if (softwareFallback) {
        if (!failed || surface->quickWindow()->isVisible() || !canvas->isVisible()) {
            std::cerr << "Unsupported backend did not restore the existing canvas\n"; return 1;
        }
        return 0;
    }
    std::cout << "layers=" << surface->quickWindow()->property("vlt.sceneLayers").toInt() << " meshes=" << surface->quickWindow()->property("vlt.sceneMeshes").toInt() << std::endl;
    const QImage frame = surface->quickWindow()->grabWindow(); // test-only readback
    if (frame.isNull() || failed || !separateThread.load()) {
        std::cerr << "No hardware frame or threaded scene graph\n"; return 1;
    }
    const double dpr = frame.width() / 320.;
    auto color = [&](int x, int y) { return frame.pixelColor(int(x * dpr), int(y * dpr)); };
    if (color(30, 30) != QColor(80, 160, 240) || color(170, 30) != QColor(240, 100, 60) ||
        color(155, 30) != QColor(18, 22, 28) || color(230, 100) != QColor(60, 200, 120) ||
        color(221, 91) != QColor(18, 22, 28)) {
        frame.save("/private/tmp/vlt-gpu-scene-failure.png");
        std::cerr << "GPU geometry, clipping or color mismatch\n"; return 1;
    }
    const QPointF point(38.25, 40.5);
    QMouseEvent press(QEvent::MouseButtonPress, point, canvas->mapToGlobal(point),
        Qt::LeftButton, Qt::LeftButton, Qt::ControlModifier);
    press.setTimestamp(12345);
    QCoreApplication::sendEvent(surface->quickWindow(), &press);
    QSettings().setValue("ui/frameMode", "unlimited");
    surface->refreshPresentationMode();
    if (surface->quickWindow()->requestedFormat().swapInterval() != 1) {
        std::cerr << "Swapchain changed during a mouse gesture\n"; return 1;
    }
    QMouseEvent release(QEvent::MouseButtonRelease, point, canvas->mapToGlobal(point),
        Qt::LeftButton, Qt::NoButton, Qt::ControlModifier);
    QCoreApplication::sendEvent(surface->quickWindow(), &release);
    surface->refreshPresentationMode();
    if (surface->quickWindow()->requestedFormat().swapInterval() != 0) return 1;
    QWheelEvent wheel(point, canvas->mapToGlobal(point), QPoint(-7, 3), {}, Qt::NoButton,
        Qt::NoModifier, Qt::ScrollMomentum, false);
    QCoreApplication::sendEvent(surface->quickWindow(), &wheel);
    if (canvas->lastMouse != point || canvas->inputTimestamp != 12345 ||
        canvas->wheelPixels != QPoint(-7, 3) || canvas->wheelPhase != Qt::ScrollMomentum) {
        std::cerr << "Input coordinates, timestamp or scroll phase changed\n"; return 1;
    }
#ifdef Q_OS_MACOS
    QMouseEvent secondaryPress(QEvent::MouseButtonPress, point, canvas->mapToGlobal(point),
        Qt::RightButton, Qt::RightButton, Qt::NoModifier);
    QCoreApplication::sendEvent(surface->quickWindow(), &secondaryPress);
    if (canvas->contextMenus != 1 || canvas->contextPosition != point.toPoint()) {
        std::cerr << "Secondary click did not reach the QWidget context menu\n"; return 1;
    }
    QContextMenuEvent duplicate(QContextMenuEvent::Mouse, point.toPoint(),
        canvas->mapToGlobal(point).toPoint());
    QCoreApplication::sendEvent(surface->quickWindow(), &duplicate);
    if (canvas->contextMenus != 1) {
        std::cerr << "Secondary click generated a duplicate context menu\n"; return 1;
    }
#endif
    canvas->update();
    QTimer::singleShot(150, &loop, &QEventLoop::quit); loop.exec();
    if (surface->quickWindow()->grabWindow() != frame) {
        std::cerr << "Retained nodes changed an unchanged frame\n"; return 1;
    }
    int idleFrames = 0;
    auto idleConnection = QObject::connect(surface, &ui::graphics::WorkspaceSurface::frameMeasured, &app,
        [&](double, double, double, double, double) { ++idleFrames; });
    QTimer::singleShot(100, &loop, &QEventLoop::quit); loop.exec(); idleFrames = 0;
    QTimer::singleShot(150, &loop, &QEventLoop::quit); loop.exec();
    QObject::disconnect(idleConnection);
    if (idleFrames) { std::cerr << "A static GPU workspace keeps rendering\n"; return 1; }
    canvas->overflow = true; canvas->update();
    QTimer::singleShot(150, &loop, &QEventLoop::quit); loop.exec();
    if (surface->quickWindow()->grabWindow().pixelColor(int(310*dpr), int(70*dpr)) != QColor(Qt::magenta)) {
        std::cerr << "Adding a required layer clip lost the retained content\n"; return 1;
    }
    canvas->overflow = false; canvas->update();
    QTimer::singleShot(150, &loop, &QEventLoop::quit); loop.exec();
    if (surface->quickWindow()->grabWindow() != frame) {
        std::cerr << "Removing a redundant layer clip changed retained content\n"; return 1;
    }
    palette.setColor(QPalette::Window, QColor(32, 46, 58));
    canvas->setPalette(palette);
    QTimer::singleShot(150, &loop, &QEventLoop::quit); loop.exec();
    const auto recolored = surface->quickWindow()->grabWindow();
    if (recolored.pixelColor(int(155 * dpr), int(30 * dpr)) != QColor(32, 46, 58)) {
        std::cerr << "A palette change did not invalidate retained geometry\n"; return 1;
    }
    if (qEnvironmentVariableIntValue("VLT_GPU_TIMESTAMPS") &&
        surface->quickWindow()->rendererInterface()->graphicsApi() == QSGRendererInterface::Metal &&
        !(surface->quickWindow()->property("vlt.completedGpuMs").toDouble() > 0)) {
        std::cerr << "Opt-in Metal GPU timing did not return a completed frame\n"; return 1;
    }
    PlainControl control(canvas);
    control.setGeometry(260, 110, 30, 20); control.show();
    QTimer::singleShot(150, &loop, &QEventLoop::quit); loop.exec();
    control.color = Qt::blue; control.update();
    QTimer::singleShot(150, &loop, &QEventLoop::quit); loop.exec();
    if (surface->quickWindow()->grabWindow().pixelColor(int(270*dpr), int(120*dpr)) != QColor(Qt::blue)) {
        std::cerr << "Ordinary QWidget damage was hidden behind the Quick container\n"; return 1;
    }
    control.hide();
    HoverRow firstRow(canvas), secondRow(canvas);
    firstRow.setGeometry(255, 80, 50, 22);
    secondRow.setGeometry(255, 108, 50, 22);
    firstRow.show(); secondRow.show();
    QToolButton first(&firstRow), second(&secondRow);
    first.setGeometry(firstRow.rect()); second.setGeometry(secondRow.rect());
    for (auto* button : {&first, &second}) {
        button->setStyleSheet("QToolButton { background: #204060; border: none; }"
                             "QToolButton:hover { background: #e08020; }");
        button->show();
    }
    const auto moveTo = [&](const QPointF& position) {
        QMouseEvent move(QEvent::MouseMove, position, canvas->mapToGlobal(position),
                         Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(surface->quickWindow(), &move);
        QTimer::singleShot(80, &loop, &QEventLoop::quit); loop.exec();
    };
    moveTo(QPointF(270, 90));
    const auto firstHover = surface->quickWindow()->grabWindow();
    moveTo(QPointF(270, 118));
    const auto secondHover = surface->quickWindow()->grabWindow();
    if (firstRow.enters != 1 || firstRow.leaves != 1 || secondRow.enters != 1) {
        std::cerr << "Slot rows did not receive their child's hover boundary events\n"; return 1;
    }
    if (firstHover.pixelColor(int(270*dpr), int(90*dpr)) != QColor("#e08020") ||
        secondHover.pixelColor(int(270*dpr), int(90*dpr)) != QColor("#204060") ||
        secondHover.pixelColor(int(270*dpr), int(118*dpr)) != QColor("#e08020")) {
        std::cerr << "Moving between slots left stale hover pixels in the GPU scene\n";
        return 1;
    }
    QEvent leave(QEvent::Leave);
    QCoreApplication::sendEvent(surface->quickWindow(), &leave);
    QTimer::singleShot(80, &loop, &QEventLoop::quit); loop.exec();
    if (surface->quickWindow()->grabWindow().pixelColor(int(270*dpr), int(118*dpr)) != QColor("#204060")) {
        std::cerr << "Leaving the workspace retained a slot highlight\n"; return 1;
    }
    firstRow.hide(); secondRow.hide();

    QScrollArea scroll(canvas);
    scroll.setFrameShape(QFrame::NoFrame);
    scroll.setGeometry(10, 10, 200, 100);
    scroll.setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* page = new PlainControl;
    page->color = QColor("#203040");
    page->setFixedSize(180, 240);
    auto* tile = new PlainControl(page);
    tile->setGeometry(10, 45, 80, 60);
    scroll.setWidget(page);
    page->setAutoFillBackground(false); // same transparent content page as the mixer
    scroll.show();
    QTimer::singleShot(150, &loop, &QEventLoop::quit); loop.exec();
    page->paints = tile->paints = 0;
    scroll.verticalScrollBar()->setValue(20);
    QTimer::singleShot(80, &loop, &QEventLoop::quit); loop.exec();
    if (page->paints || tile->paints) {
        std::cerr << "Scrolling rerecorded unchanged widget content: page=" << page->paints
                  << " tile=" << tile->paints << '\n'; return 1;
    }
    const auto scrolled = surface->quickWindow()->grabWindow();
    // The tile's bottom was clipped in the previous frame and is now visible.
    const QPoint revealed = tile->mapTo(canvas, QPoint(20, 58));
    if (scrolled.pixelColor(int(revealed.x()*dpr), int(revealed.y()*dpr)) != QColor(Qt::red)) {
        std::cerr << "Scrolling did not update the retained layer's clip\n"; return 1;
    }
    tile->color = Qt::blue;
    tile->update();
    scroll.verticalScrollBar()->setValue(30);
    QTimer::singleShot(80, &loop, &QEventLoop::quit); loop.exec();
    const QPoint changed = tile->mapTo(canvas, QPoint(20, 20));
    if (!tile->paints || surface->quickWindow()->grabWindow().pixelColor(
            int(changed.x()*dpr), int(changed.y()*dpr)) != QColor(Qt::blue)) {
        std::cerr << "An explicit widget update was lost during scrolling\n"; return 1;
    }
    // Parent update() must also preserve custom children depending on it.
    tile->color = Qt::green;
    page->update();
    scroll.verticalScrollBar()->setValue(40);
    QTimer::singleShot(80, &loop, &QEventLoop::quit); loop.exec();
    const QPoint childChanged = tile->mapTo(canvas, QPoint(20, 20));
    if (surface->quickWindow()->grabWindow().pixelColor(
            int(childChanged.x()*dpr), int(childChanged.y()*dpr)) != QColor(Qt::green)) {
        std::cerr << "A parent update lost its child's changed content during scrolling\n"; return 1;
    }
    tile->paints = 0;
    scroll.verticalScrollBar()->setValue(80);
    QTimer::singleShot(80, &loop, &QEventLoop::quit); loop.exec();
    const auto clipped = surface->quickWindow()->grabWindow();
    const auto top = scroll.viewport()->mapTo(canvas, QPoint(30, 1));
    if (tile->paints || clipped.pixelColor(int(top.x()*dpr), int(top.y()*dpr)) != QColor(Qt::green) ||
        clipped.pixelColor(int(top.x()*dpr), int((top.y()-3)*dpr)) == QColor(Qt::green)) {
        std::cerr << "A retained layer escaped its new viewport clip\n"; return 1;
    }

    // Wheel navigation must update hover even without a new mouse movement.
    QToolButton upper(page), lower(page);
    upper.setGeometry(100, 100, 60, 20); lower.setGeometry(100, 130, 60, 20);
    upper.show(); lower.show();
    const QPointF hoverPoint = upper.mapTo(canvas, QPoint(10, 10));
    moveTo(hoverPoint);
    if (!upper.underMouse()) { std::cerr << "Scroll hover fixture was not entered\n"; return 1; }
    QWheelEvent hoverWheel(hoverPoint, canvas->mapToGlobal(hoverPoint), QPoint(0, -30), {},
                          Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false);
    QCoreApplication::sendEvent(surface->quickWindow(), &hoverWheel);
    if (upper.underMouse() || !lower.underMouse()) {
        std::cerr << "Scrolling retained hover on the slot that moved away\n"; return 1;
    }
    scroll.hide();
    DragControl dragSource(canvas), dropTarget(canvas);
    dragSource.setGeometry(5, 5, 60, 35);
    dropTarget.setGeometry(70, 5, 60, 35);
    dropTarget.setAcceptDrops(true);
    dragSource.show(); dropTarget.show();
    const QPointF start = dragSource.mapTo(canvas, QPoint(10, 10));
    const QPointF end = dropTarget.mapTo(canvas, QPoint(10, 10));
    QMouseEvent dragPress(QEvent::MouseButtonPress, start, canvas->mapToGlobal(start),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(surface->quickWindow(), &dragPress);
    QMimeData payload;
    payload.setText("insert");
    QDragEnterEvent enterDrop(end.toPoint(), Qt::MoveAction, &payload,
                              Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(surface->quickWindow(), &enterDrop);
    QDropEvent finishDrop(end, Qt::MoveAction, &payload, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(surface->quickWindow(), &finishDrop);
    // Native drag consumes the release; Qt can still deliver a queued move
    // with the old button flags. It must not return to the former source.
    QMouseEvent trailingMove(QEvent::MouseMove, end, canvas->mapToGlobal(end),
                             Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(surface->quickWindow(), &trailingMove);
    if (!finishDrop.isAccepted() || dropTarget.drops != 1 ||
        dragSource.ungrabs != 1 || dragSource.moves != 0 || dropTarget.moves != 1) {
        std::cerr << "Native drop retained the source's implicit mouse capture\n"; return 1;
    }
    dragSource.hide(); dropTarget.hide();
    QWidget nativeEditor(canvas);
    nativeEditor.setGeometry(260, 90, 40, 30);
    nativeEditor.setProperty("vlt.foreignSurface", true);
    nativeEditor.setAttribute(Qt::WA_NativeWindow);
    nativeEditor.winId();
    nativeEditor.show();
    QTimer::singleShot(150, &loop, &QEventLoop::quit); loop.exec();
    if (!failed || surface->quickWindow()->isVisible() || !nativeEditor.isVisible() || !canvas->isVisible()) {
        std::cerr << "A foreign native editor was covered instead of restoring compatibility\n"; return 1;
    }
    std::cout << "Hardware scene: " << frame.width() << "x" << frame.height() << ", render thread separate\n";
    return 0;
}
