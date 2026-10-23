#include "graphics/SceneRecorder.hpp"
#include "graphics/WorkspaceSurface.hpp"
#include "graphics/WidgetUpdates.hpp"
#include "graphics/GraphicsPreferences.hpp"
#include "graphics/RetainedScene.hpp"
#include "graphics/TextureStore.hpp"
#include "graphics/StretchableRect.hpp"
#include "UiFrameClock.hpp"
#include "UiPerformance.hpp"
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
#include <QScreen>
#include <QTableWidget>
#include <QHeaderView>
#include <QStyleHints>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QWidget>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>
#include <functional>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

static bool checkStretchableRects() {
    using namespace ui::graphics;
    const auto rasterize = [](const std::vector<SceneMesh>& meshes, qreal dpr) {
        QImage image(QSize(180 * dpr, 80 * dpr), QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(dpr); image.fill(Qt::transparent);
        QPainter p(&image);
        const auto triangles = [](const QVector<SceneVertex>& vertices) {
            QPainterPath path; path.setFillRule(Qt::WindingFill);
            for (int i = 0; i + 2 < vertices.size(); i += 3) {
                path.moveTo(vertices[i].x, vertices[i].y);
                path.lineTo(vertices[i + 1].x, vertices[i + 1].y);
                path.lineTo(vertices[i + 2].x, vertices[i + 2].y);
                path.closeSubpath();
            }
            return path;
        };
        std::function<void(const std::vector<SceneMesh>&)> draw = [&](const auto& list) {
            for (const auto& mesh : list) {
                p.save();
                if (mesh.clip) {
                    if (mesh.clip->rectangular) p.setClipRect(mesh.clip->bounds, Qt::IntersectClip);
                    else p.setClipPath(triangles(mesh.clip->triangles), Qt::IntersectClip);
                }
                p.setWorldTransform(mesh.transform, true);
                p.setOpacity(p.opacity() * mesh.opacity);
                if (mesh.group) draw(mesh.group->meshes);
                else { p.setPen(Qt::NoPen); p.setBrush(mesh.color); p.drawPath(triangles(mesh.vertices)); }
                p.restore();
            }
        };
        draw(meshes); p.end(); return image;
    };
    for (qreal dpr : {1., 1.25, 1.5, 2.}) for (int style = 0; style < 3; ++style) {
        const bool outline = style != 0;
        std::array<std::shared_ptr<const SceneMeshGroup>, 3> previous;
        for (qreal width : {1., 2.5, 8., 15.25, 80.25, 120.3}) {
            std::array<QImage, 3> images;
            for (int cached = 0; cached < 3; ++cached) {
                SceneRecorder recorder(QSize(180, 80), dpr);
                {
                    QPainter p(&recorder);
                    QPainterPath clip; clip.addRoundedRect(QRectF(8, 7, 100, 43), 4, 4);
                    p.setClipPath(clip); p.setOpacity(.75);
                    p.setPen(outline ? QPen(QColor(220, 240, 250), 1) : QPen(Qt::NoPen));
                    p.setBrush(style == 2 ? QBrush(Qt::NoBrush) : QBrush(QColor(60, 120, 210)));
                    const QRectF rect(8.37, 6.91, width, 38.25);
                    if (cached) {
                        if (!paintStretchableRoundedRect(p, rect, 2, cached == 1)) {
                            if (cached != 1 || width >= 8) return false;
                            p.drawRoundedRect(rect, 2, 2);
                        }
                    } else p.drawRoundedRect(rect, 2, 2);
                }
                if (!recorder.supported()) return false;
                const auto meshes = recorder.takeMeshes();
                if (cached == 1 && width >= 8) {
                    if (meshes.size() != 1 || !meshes[0].clip || !meshes[0].group) return false;
                    const auto& children = meshes[0].group->meshes;
                    if (children.size() != 3) return false;
                    for (int i = 0; i < 3; ++i) {
                        if (!children[i].group || children[i].clip || children[i].snapTranslation ||
                            (previous[i] && previous[i] != children[i].group)) return false;
                        previous[i] = children[i].group;
                    }
                } else if (cached == 2) {
                    for (const auto& mesh : meshes) if (mesh.group &&
                        (!mesh.clip || std::any_of(mesh.group->meshes.begin(), mesh.group->meshes.end(),
                            [](const auto& child) { return bool(child.group) || bool(child.clip); }))) return false;
                }
                images[cached] = rasterize(meshes, dpr);
            }
            for (int cached = 1; cached < 3; ++cached) {
                int different = 0;
                for (int y = 0; y < images[0].height(); ++y)
                    for (int x = 0; x < images[0].width(); ++x)
                        different += images[0].pixel(x, y) != images[cached].pixel(x, y);
                // Triangulation can choose a different boundary sample, but a gap,
                // doubled translucent seam or rescaled radius affects whole rows.
                if (different > 32 * dpr) {
                    std::cerr << "Stretchable rectangle differs: width=" << width
                              << " dpr=" << dpr << " mode=" << cached << " pixels=" << different << '\n';
                    return false;
                }
            }
        }
    }
    // Adjacent retained time tiles share one viewport mask while keeping
    // their own cached geometry and scrolling transforms. Check opacity,
    // rectangular/curved masks and reversals at fractional display scales.
    for (qreal dpr : {1., 1.25, 1.5, 2.}) for (bool rounded : {false, true}) {
        RetainedScene tiles;
        std::array<std::shared_ptr<const SceneMeshGroup>, 2> previous;
        for (int pan : {0, 13, 30, 0}) {
            QPainterPath mask;
            if (rounded) mask.addRoundedRect(QRectF(8, 7, 100, 43), 5, 5);
            else mask.addRect(QRectF(8, 7, 100, 43));
            SceneRecorder recorder(QSize(180, 80), dpr);
            {
                QPainter p(&recorder); p.setClipPath(mask);
                for (int i = 0; i < 2; ++i) {
                    p.setOpacity(i ? .65 : .8);
                    tiles.paint(p, i, QSize(100, 60), QPointF(i * 80 - pan, 0), [i](QPainter& tile) {
                        tile.fillRect(QRect(0, 0, 100, 60), i ? QColor(40, 160, 220, 190) : QColor(210, 90, 40));
                    });
                }
            }
            const auto meshes = recorder.takeMeshes();
            if (meshes.size() != 1 || !meshes[0].clip || !meshes[0].group ||
                meshes[0].group->meshes.size() != 2) return false;
            qint64 bytes = 0;
            for (int i = 0; i < 2; ++i) {
                const auto& child = meshes[0].group->meshes[i];
                if (!child.group || child.clip || child.transform.dx() != i * 80 - pan ||
                    (previous[i] && previous[i] != child.group)) return false;
                previous[i] = child.group; bytes += child.group->bytes;
            }
            if (meshes[0].group->bytes != bytes || tiles.builds() != 2) return false;
            auto separate = meshes[0].group->meshes;
            for (auto& mesh : separate) mesh.clip = meshes[0].clip;
            if (rasterize(meshes, dpr) != rasterize(separate, dpr)) {
                std::cerr << "A common viewport mask changed retained tile pixels\n"; return false;
            }
        }
    }
    // Dense MIDI previews are ordinary opaque rectangles under one curved
    // clip. They must retain overlap coverage and produce one geometry batch.
    for (qreal dpr : {1., 1.25, 1.5, 2.}) {
        const auto paint = [](QPainter& p) {
            QPainterPath mask; mask.addRoundedRect(QRectF(8,7,100,43),5,5);
            p.setClipPath(mask); p.translate(.37,.21);
            p.setPen(Qt::NoPen); p.setBrush(QColor(80,140,220));
            QVector<QRectF> notes;
            for (int i = 0; i < 128; ++i)
                notes.append(QRectF(i%16*8-10,i/16*7-6,13,5));
            p.drawRects(notes.constData(), int(notes.size()));
        };
        SceneRecorder recorder(QSize(180,80),dpr);
        { QPainter p(&recorder); paint(p); }
        const auto meshes = recorder.takeMeshes();
        if (!recorder.supported() || meshes.size()!=1 || !meshes[0].clip || meshes[0].clip->rectangular)
            return false;
        const auto actual = rasterize(meshes,dpr);
        QImage expected(actual.size(),actual.format());
        expected.setDevicePixelRatio(dpr); expected.fill(Qt::transparent);
        { QPainter p(&expected); paint(p); }
        int different = 0;
        for (int y = 0; y < actual.height(); ++y) for (int x = 0; x < actual.width(); ++x)
            different += actual.pixel(x,y)!=expected.pixel(x,y);
        if (different>32*dpr) {
            std::cerr << "Batched MIDI rectangles changed clip/overlap coverage at DPR " << dpr
                      << ": " << different << " pixels\n";
            return false;
        }
    }
    SceneRecorder dashed(QSize(50, 50));
    // Paths and axis-aligned lines share the GPU mask just like note batches.
    // Exercise holes, fractional placement and strokes crossing curved edges.
    for (qreal dpr : {1., 1.25, 1.5, 2.}) for (bool hole : {false, true}) {
        const auto paint = [hole](QPainter& p) {
            QPainterPath mask; mask.addRoundedRect(QRectF(8,7,100,43),9,9);
            if (hole) mask.addEllipse(QRectF(45,17,17,15));
            p.setClipPath(mask); p.translate(.37,.21);
            p.setPen(Qt::NoPen); p.setBrush(QColor(60,140,210,100));
            QPainterPath fill; fill.addRoundedRect(QRectF(2,2,115,53),12,12);
            p.drawPath(fill);
            p.setPen(QPen(QColor(220,180,90,190),1.4,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
            p.setBrush(Qt::NoBrush);
            QPainterPath curve; curve.moveTo(6,49); curve.cubicTo(25,4,66,50,109,6);
            p.drawPath(curve);
            p.setPen(QPen(Qt::white,1));
            p.drawLine(QPointF(2,10),QPointF(120,10));
            p.drawLine(QPointF(9,0),QPointF(9,60));
        };
        SceneRecorder recorder(QSize(180,80),dpr);
        { QPainter p(&recorder); paint(p); }
        if (!recorder.supported()) return false;
        const auto meshes = recorder.takeMeshes();
        for (const auto& mesh : meshes) if (!mesh.clip) return false;
        const auto actual = rasterize(meshes,dpr);
        QImage expected(actual.size(),actual.format());
        expected.setDevicePixelRatio(dpr); expected.fill(Qt::transparent);
        { QPainter p(&expected); paint(p); }
        int different = 0;
        for (int y=0;y<actual.height();++y) for (int x=0;x<actual.width();++x)
            different += actual.pixel(x,y)!=expected.pixel(x,y);
        if (different>80*dpr) {
            std::cerr << "Shared path mask differs at DPR " << dpr << " hole=" << hole
                      << ": " << different << " pixels\n";
            return false;
        }
    }
    QPainter p(&dashed); p.setBrush(Qt::red); p.setPen(QPen(Qt::white, 1, Qt::DashLine));
    return !paintStretchableRoundedRect(p, QRectF(5, 5, 30, 20), 2);
}

class Canvas final : public ui::FrameWidget {
public:
    using ui::FrameWidget::FrameWidget;
    QPointF lastMouse;
    QPoint wheelPixels;
    QPoint contextPosition;
    int contextMenus = 0;
    int paints = 0;
    int staticBuilds = 0;
    Qt::ScrollPhase wheelPhase = Qt::NoScrollPhase;
    ulong inputTimestamp = 0;
    bool overflow = false;
    QPointF pan;
protected:
    void mousePressEvent(QMouseEvent* event) override { lastMouse = event->position(); inputTimestamp = event->timestamp(); }
    void wheelEvent(QWheelEvent* event) override { wheelPixels = event->pixelDelta(); wheelPhase = event->phase(); }
    void contextMenuEvent(QContextMenuEvent* event) override {
        contextPosition = event->pos();
        ++contextMenus;
        event->accept();
    }
    void paintEvent(QPaintEvent*) override {
        ++paints;
        QPainter p(this);
        auto* scene = ui::graphics::sceneGeometrySink(p);
        if (!scene || scene->beginRetainedSection(101, false)) {
            ++staticBuilds;
            p.fillRect(rect(), palette().color(QPalette::Window));
            if (scene) scene->endRetainedSection();
        }
        p.translate(pan);
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
class FrameControl final : public ui::FrameWidget {
public:
    using ui::FrameWidget::FrameWidget;
    QColor color = Qt::red;
    int paints = 0;
protected:
    void paintEvent(QPaintEvent*) override { ++paints; QPainter painter(this); painter.fillRect(rect(), color); }
};
class TableControl final : public QTableWidget {
public:
    using QTableWidget::QTableWidget;
    int paints = 0;
protected:
    void paintEvent(QPaintEvent* event) override { ++paints; QTableWidget::paintEvent(event); }
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
bool checkContextMenuRouting(Qt::ContextMenuTrigger trigger) {
    QWidget window;
    window.resize(320, 140);
    Canvas canvas(&window);
    canvas.setGeometry(17, 23, 200, 100);
    canvas.show();
    ui::graphics::WorkspaceSurface surface(&window);
    const QPointF point(38.25, 40.5);
    const QPointF position = canvas.mapTo(&window, point);
    const QPointF global = canvas.mapToGlobal(point);
    for (const auto button : {Qt::LeftButton, Qt::RightButton}) {
        QMouseEvent press(QEvent::MouseButtonPress, position, global,
            button, button, Qt::NoModifier);
        QCoreApplication::sendEvent(surface.quickWindow(), &press);
        const int expectedAtPress = button == Qt::RightButton && trigger == Qt::ContextMenuTrigger::Press;
        if (canvas.contextMenus != expectedAtPress || canvas.lastMouse != point) {
            std::cerr << "Context menu press timing or target coordinates are incorrect\n"; return false;
        }
        QMouseEvent release(QEvent::MouseButtonRelease, position, global,
            button, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(surface.quickWindow(), &release);
        if (canvas.contextMenus != (button == Qt::RightButton ? 1 : 0)) {
            std::cerr << "Secondary click did not deliver exactly one context menu\n"; return false;
        }
    }
    if (canvas.contextPosition != point.toPoint()) return false;
    QContextMenuEvent duplicate(QContextMenuEvent::Mouse, position.toPoint(), global.toPoint());
    QCoreApplication::sendEvent(surface.quickWindow(), &duplicate);
    if (canvas.contextMenus != 1) {
        std::cerr << "Secondary click generated a duplicate context menu\n"; return false;
    }
    QContextMenuEvent keyboard(QContextMenuEvent::Keyboard, position.toPoint(), global.toPoint());
    QCoreApplication::sendEvent(surface.quickWindow(), &keyboard);
    if (canvas.contextMenus != 2) {
        std::cerr << "Keyboard context menu was discarded\n"; return false;
    }
    return true;
}
// Optional hardware diagnostic. Keep timings out of the deterministic/offscreen
// assertions: compositor, display and driver load all affect presentation.
static int checkCadence() {
    const auto run = [](int ms) {
        QEventLoop loop; QTimer::singleShot(ms, &loop, &QEventLoop::quit); loop.exec();
    };
    QWidget host;
    host.setWindowFlag(Qt::WindowStaysOnTopHint);
    host.setWindowTitle(QStringLiteral("GPU cadence check (closes automatically)"));
    host.resize(480, 280);
    Canvas canvas(&host); canvas.setGeometry(host.rect());
    ui::graphics::WorkspaceSurface surface(&canvas);
    bool failed = false;
    QObject::connect(&surface, &ui::graphics::WorkspaceSurface::failed, &host,
        [&](const QString& reason) { std::cerr << reason.toStdString() << '\n'; failed = true; });
    host.show(); host.raise(); host.activateWindow(); run(500);
    std::cout << "CADENCE active=" << host.isActiveWindow() << " platform=" << qApp->platformName().toStdString() << std::endl;
    ui::FrameTimer timer(&canvas);
    std::vector<double> ticks, submissions;
    QElapsedTimer tickTime, pathTime;
    bool sampling = false;
    QObject::connect(&timer, &ui::FrameTimer::timeout, &canvas, [&] {
        if (sampling && tickTime.isValid()) ticks.push_back(tickTime.nsecsElapsed() / 1e6);
        tickTime.start();
        canvas.pan = QPointF(std::fmod(pathTime.nsecsElapsed() / 1e7, 120.), 0);
        canvas.update();
    });
    QObject::connect(&surface, &ui::graphics::WorkspaceSurface::frameMeasured, &canvas,
        [&](double, double, double, double interval, double) {
            if (sampling && interval > 0) submissions.push_back(interval);
        });
    for (int fps : {60, 240, 0}) {
        ui::FrameClock::instance().setPreference(fps ? ui::FrameMode::Fixed : ui::FrameMode::Display,
                                                fps ? fps : 60);
        pathTime.start(); timer.start(); run(400);
        sampling = true; ticks.clear(); submissions.clear(); run(2000);
        sampling = false; timer.stop(); run(100);
        const auto print = [&](const char* metric, std::vector<double> values) {
            std::sort(values.begin(), values.end());
            const double target = 1000. / std::min(fps ? double(fps) : 10000., host.screen()->refreshRate());
            const auto gaps = std::count_if(values.begin(), values.end(), [&](double ms) { return ms > target * 1.5; });
            if (!values.empty()) std::cout << "CADENCE fps=" << fps << " active=" << host.isActiveWindow() << " display=" << host.screen()->refreshRate()
                << " metric=" << metric << " n=" << values.size() << " p50=" << values[values.size()/2]
                << " p95=" << values[values.size()*95/100] << " max=" << values.back() << " gaps=" << gaps << std::endl;
        };
        print("callback", ticks); print("submission", submissions);
        if (ticks.empty() || submissions.empty()) failed = true;
        sampling = true; submissions.clear(); run(150); sampling = false;
        if (!submissions.empty()) { std::cerr << "Cadence keeps an idle scene awake\n"; failed = true; }
    }
    ui::perf::flush();
    return failed ? 1 : 0;
}

class CacheTestTexture final : public QSGTexture {
public:
    qint64 comparisonKey() const override { return qint64(this); }
    QSize textureSize() const override { return {32, 32}; }
    bool hasAlphaChannel() const override { return true; }
    bool hasMipmaps() const override { return false; }
};
static bool checkTextureCacheAging(bool benchmark) {
    if (!benchmark) {
        constexpr qint64 cost = 32 * 32 * 4;
        ui::graphics::TextureStore store(cost * 3);
        const auto image = [] { return QImage(32, 32, QImage::Format_ARGB32_Premultiplied); };
        const auto create = [] { return std::make_shared<CacheTestTexture>(); };
        const auto a = image(), b = image(), c = image(), d = image();
        std::weak_ptr<QSGTexture> first = store.get(a, create), second = store.get(b, create);
        auto pinned = store.get(c, create);
        auto touch = store.get(a, create); touch.reset();
        if (!store.get(d, create) || first.expired() || !second.expired() || !pinned) {
            std::cerr << "Texture eviction lost LRU order or a live resource\n"; return false;
        }
        // All entries pinned: reject pressure without releasing live textures.
        auto holdA = store.get(a, create), holdD = store.get(d, create);
        if (store.get(image(), create) || store.retainedBytes() != cost*3) return false;
        holdA.reset(); holdD.reset();
        for (int i = 0; i < 100; ++i) if (!store.get(image(), create)) return false;
        if (store.get(c, create) != pinned) return false;
        pinned.reset();
        const auto count = store.retainedCount();
        QImage oversized(64, 64, QImage::Format_ARGB32_Premultiplied);
        if (store.get(oversized, create) || store.retainedCount() != count) return false;
        if (store.get(image(), [] { return std::shared_ptr<QSGTexture>{}; })) return false;
    }
    for (int side : {128, 32}) {
        ui::graphics::TextureStore store;
        const auto insert = [&] {
            QImage image(side, side, QImage::Format_ARGB32_Premultiplied);
            return store.get(image, [] { return std::make_shared<CacheTestTexture>(); });
        };
        const int capacity = int(ui::graphics::TextureStore::budget / (side * side * 4));
        for (int i = 0; i < capacity; ++i) if (!insert()) return false;
        const auto before = store.statistics;
        QElapsedTimer time; time.start();
        for (int i = 0; i < 1000; ++i) if (!insert()) return false;
        const auto visited = store.statistics.evictionVisits - before.evictionVisits;
        if (benchmark) std::cout << "CACHE_AGING entries=" << store.retainedCount()
            << " replacements=1000 visits=" << visited << " ms=" << time.nsecsElapsed()/1e6 << std::endl;
        if (store.retainedBytes() > ui::graphics::TextureStore::budget ||
            store.retainedCount() != capacity || store.statistics.evictions-before.evictions != 1000) return false;
        // Eviction must not become a scan of all historical textures.
        if (!benchmark && visited > 1000) {
            std::cerr << "A full GPU cache scans old entries on every replacement\n"; return false;
        }
    }
    return true;
}

class TextureChurnCanvas final : public ui::FrameWidget {
public:
    using ui::FrameWidget::FrameWidget;
    std::vector<QImage> images;
    int generation = 0;
    void advance() {
        images.clear();
        for (int i = 0; i < 16; ++i) {
            QImage image(32, 32, QImage::Format_ARGB32_Premultiplied);
            image.fill(QColor::fromHsv((generation+i*13)%360, 100, 180));
            images.push_back(std::move(image));
        }
        ++generation; update();
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this); p.fillRect(rect(), QColor(24,24,24));
        for (int i = 0; i < images.size(); ++i) p.drawImage(QPoint((i%4)*40+8,(i/4)*40+8), images[i]);
    }
};
static int checkTextureChurn() {
    const auto run = [](int ms) { QEventLoop loop; QTimer::singleShot(ms, &loop, &QEventLoop::quit); loop.exec(); };
    QWidget host; host.resize(200,200);
    host.setWindowFlag(Qt::WindowStaysOnTopHint);
    const auto screens = QGuiApplication::screens();
    if (!screens.empty()) {
        auto* screen = *std::max_element(screens.begin(), screens.end(),
            [](auto* a, auto* b) { return a->refreshRate() < b->refreshRate(); });
        host.setScreen(screen);
        host.move(screen->availableGeometry().topLeft() + QPoint(80, 80));
    }
    host.setWindowTitle("GPU cache aging check (closes automatically)");
    TextureChurnCanvas canvas(&host); canvas.setGeometry(host.rect()); canvas.advance();
    ui::graphics::WorkspaceSurface surface(&canvas);
    bool failed = false;
    QObject::connect(&surface, &ui::graphics::WorkspaceSurface::failed, &host,
        [&](const QString& reason) { std::cerr << reason.toStdString() << std::endl; failed = true; });
    std::atomic<quint64> entries{0}, bytes{0}, visits{0}, evictions{0};
    QObject::connect(surface.quickWindow(), &QQuickWindow::afterRendering, &surface, [&] {
        const auto store = ui::graphics::TextureStore::forWindow(surface.quickWindow());
        entries = store->retainedCount(); bytes = store->retainedBytes();
        visits = store->statistics.evictionVisits; evictions = store->statistics.evictions;
    }, Qt::DirectConnection);
    ui::FrameClock::instance().setPreference(ui::FrameMode::Fixed, 240);
    host.show(); host.raise(); host.activateWindow(); surface.quickWindow()->requestActivate(); run(500);
    ui::FrameTimer timer(&canvas);
    QObject::connect(&timer, &ui::FrameTimer::timeout, &canvas, [&] { canvas.advance(); });
    std::vector<double> syncs, intervals;
    int frames = 0;
    QObject::connect(&surface, &ui::graphics::WorkspaceSurface::frameMeasured, &host,
        [&](double, double sync, double, double interval, double) {
            ++frames; syncs.push_back(sync); if (interval > 0) intervals.push_back(interval);
        });
    timer.start();
    for (int phase = 0; phase < 3 && !failed; ++phase) {
        frames = 0; syncs.clear(); intervals.clear(); QElapsedTimer elapsed; elapsed.start();
        const quint64 oldVisits = visits, oldEvictions = evictions;
        if (phase == 1) {
            // Keep the same pressure even on a 60-Hz/background-throttled screen.
            // Do not claim an aging result before the cache actually fills.
            do { run(500); } while (elapsed.elapsed() < 20000 ||
                (evictions < 1000 && elapsed.elapsed() < 60000));
        } else run(3000);
        const double seconds = elapsed.nsecsElapsed()/1e9;
        std::sort(syncs.begin(),syncs.end()); std::sort(intervals.begin(),intervals.end());
        const auto p95 = [](const auto& values) { return values.empty() ? 0. : values[(values.size()-1)*95/100]; };
        std::cout << "TEXTURE_CHURN phase=" << phase << " fps=" << frames/seconds
            << " display_hz=" << host.screen()->refreshRate() << " active=" << host.isActiveWindow()
            << " sync_p95_ms=" << p95(syncs) << " interval_p95_ms=" << p95(intervals)
            << " entries=" << entries.load() << " bytes=" << bytes.load()
            << " visits=" << visits-oldVisits << " evictions=" << evictions-oldEvictions << std::endl;
    }
    timer.stop(); run(100);
    if (!frames || bytes > ui::graphics::TextureStore::budget || evictions == 0) failed = true;
    return failed ? 1 : 0;
}

int main(int argc, char** argv) {
    // Match the app's native hosting policy: editor frames must not promote
    // their complete QWidget sibling hierarchy above the Quick workspace.
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc, argv);
    if (!checkStretchableRects()) return 1;
    if (!ui::graphics::WorkspaceSurface::checkPointerRoutingForTest()) {
        std::cerr << "GPU input routing did not survive reentrant teardown\n";
        return 1;
    }
    QTemporaryDir preferences;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, preferences.path());
    app.setOrganizationName("VltGpuTest"); app.setApplicationName("Scene");
    if (app.arguments().contains("--cadence")) return checkCadence();
    if (app.arguments().contains("--cache-benchmark")) return checkTextureCacheAging(true) ? 0 : 1;
    if (app.arguments().contains("--texture-churn")) return checkTextureChurn();
    if (!checkTextureCacheAging(false)) return 1;
    const bool overrideWasSet = qEnvironmentVariableIsSet("VLT_GPU_WORKSPACE");
    const auto savedOverride = qgetenv("VLT_GPU_WORKSPACE");
    qunsetenv("VLT_GPU_WORKSPACE");
    if (!ui::graphics::gpuWorkspaceEnabled()) return 1;
    QSettings().setValue("ui/gpuWorkspace", false);
    if (ui::graphics::gpuWorkspaceEnabled()) return 1;
    qputenv("VLT_GPU_WORKSPACE", "1");
    if (!ui::graphics::gpuWorkspaceEnabled() || QSettings().value("ui/gpuWorkspace").toBool()) return 1;
    qunsetenv("VLT_GPU_WORKSPACE");
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
    // A glyph atlas is already rasterized at the target DPI. Its scene quad
    // must keep the exact logical size; rounding it stretches the texture and
    // makes all GPU-workspace text soft at fractional Windows scaling.
    constexpr qreal textDpr = 1.25;
    ui::graphics::SceneRecorder fractionalText(QSize(320, 140), textDpr);
    {
        QPainter p(&fractionalText);
        p.setPen(Qt::white);
        p.drawText(QPointF(5, 70), QStringLiteral("Pattern 01"));
    }
    const auto textMeshes = fractionalText.takeMeshes();
    // Qt's offscreen platform can omit glyph runs entirely. The native
    // Windows/macOS run covers the fractional-DPI geometry when fonts exist.
    if (!textMeshes.empty()) {
        if (textMeshes.size() != 1 || textMeshes.front().texture.isNull() ||
            textMeshes.front().vertices.size() < 3) return 1;
        const auto& glyph = textMeshes.front();
        const QSizeF expected = glyph.texture.deviceIndependentSize();
        const qreal width = glyph.vertices[1].x - glyph.vertices[0].x;
        const qreal height = glyph.vertices[2].y - glyph.vertices[0].y;
        if (std::abs(width - expected.width()) > 0.001 ||
            std::abs(height - expected.height()) > 0.001) {
            std::cerr << "Fractional-DPI text texture was rescaled in the scene\n";
            return 1;
        }
    }
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
    // Axis-aligned grid strokes must keep the same bounds as the general
    // QPainterPath stroker, including caps, reversal, clipping and transforms.
    for (auto cap : {Qt::FlatCap, Qt::SquareCap}) for (bool vertical : {false, true}) {
        ui::graphics::SceneRecorder lines(QSize(100, 100), 2);
        {
            QPainter p(&lines);
            p.setClipRect(QRectF(12, 10, 60, 65));
            p.translate(5, 7);
            p.scale(1.5, 2);
            p.setPen(QPen(Qt::cyan, 2, Qt::SolidLine, cap));
            p.drawLine(vertical ? QLineF(20, 40, 20, 10) : QLineF(40, 20, 10, 20));
        }
        const auto meshes = lines.takeMeshes();
        const qreal extension = cap == Qt::SquareCap ? 1 : 0;
        const QRectF local = vertical ? QRectF(19, 10 - extension, 2, 30 + 2 * extension)
                                      : QRectF(10 - extension, 19, 30 + 2 * extension, 2);
        QTransform transform; transform.translate(5, 7); transform.scale(1.5, 2);
        const QRectF expected = transform.mapRect(local).intersected(QRectF(12, 10, 60, 65));
        QRectF actual;
        if (meshes.size() != 1 || meshes.front().vertices.size() != 6) return 1;
        for (const auto& vertex : meshes.front().vertices) {
            if (!expected.contains(QPointF(vertex.x, vertex.y))) return 1;
            if (actual.isNull()) actual = QRectF(vertex.x, vertex.y, .000001, .000001);
            actual = actual.united(QRectF(vertex.x, vertex.y, .000001, .000001));
        }
        if (std::abs(actual.width() - expected.width()) > .00001 ||
            std::abs(actual.height() - expected.height()) > .00001) return 1;
    }
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
    // Geometry remains immutable while a mixer scrolls through different
    // clips. The cached bounds must preserve both containment and exposure.
    ui::graphics::cacheLayerBounds(clipLayer);
    for (int x = -30; x <= 30; ++x) {
        clipLayer.clip = QRectF(x, 0, 30, 30);
        auto uncached = clipLayer;
        uncached.contentBounds.reset();
        if (ui::graphics::requiresLayerClip(clipLayer) != ui::graphics::requiresLayerClip(uncached)) {
            std::cerr << "Cached mixer geometry changed scrolling containment\n"; return 1;
        }
    }
    clipLayer.clip = QRectF(0, 0, 100, 100);
    clipLayer.mask = QRegion(0, 0, 5, 5);
    if (!ui::graphics::requiresLayerClip(clipLayer)) return 1;
    clipLayer.mask = {};
    auto innerClip = std::make_shared<ui::graphics::SceneClip>();
    innerClip->bounds = QRectF(12, 12, 5, 5);
    clipLayer.meshes.front().clip = innerClip;
    ui::graphics::cacheLayerBounds(clipLayer);
    clipLayer.clip = innerClip->bounds;
    if (ui::graphics::requiresLayerClip(clipLayer)) return 1;
    auto boundedGroup = std::make_shared<ui::graphics::SceneMeshGroup>();
    boundedGroup->meshes = {clippedImages.front()};
    clipLayer.meshes.front().group = boundedGroup;
    clipLayer.meshes.front().clip.reset();
    clipLayer.meshes.front().transform = QTransform::fromTranslate(3, 7);
    ui::graphics::cacheLayerBounds(clipLayer);
    if (clipLayer.contentBounds != QRectF(13, 17, 20, 20)) {
        std::cerr << "Small retained controls lost their transformed bounds\n"; return 1;
    }
    clipLayer.clip = *clipLayer.contentBounds;
    if (ui::graphics::requiresLayerClip(clipLayer)) return 1;
    clipLayer.clip.adjust(0, 0, -1, 0);
    if (!ui::graphics::requiresLayerClip(clipLayer)) return 1;
    clipLayer.meshes.front().transform = QTransform::fromTranslate(1, 1);
    clipLayer.clip = QRectF(11, 11, 20, 20);
    ui::graphics::cacheLayerBounds(clipLayer, 1.5);
    if (!ui::graphics::requiresLayerClip(clipLayer)) {
        std::cerr << "Clip containment ignored fractional-DPI translation rounding\n"; return 1;
    }
    boundedGroup->bytes = 4097 * sizeof(ui::graphics::SceneVertex);
    ui::graphics::cacheLayerBounds(clipLayer);
    if (clipLayer.contentBounds) return 1;
    clipLayer.meshes = {clippedImages.front()};
    const auto triangle = clipLayer.meshes.front().vertices;
    for (int i = 0; i < 1000; ++i) clipLayer.meshes.front().vertices += triangle;
    ui::graphics::cacheLayerBounds(clipLayer);
    if (!clipLayer.contentBounds || *clipLayer.contentBounds != QRectF(10, 10, 20, 20)) {
        std::cerr << "Large flat paths lost their once-recorded bounds\n"; return 1;
    }
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
    if (ramps[0].texture.hasAlphaChannel()) {
        std::cerr << "An opaque gradient unnecessarily entered the alpha pass\n"; return 1;
    }
    // A shorter meter exposes the same part of the full-scale colour ramp;
    // its colours must not stretch to the changing level or clipped bounds.
    ui::graphics::SceneRecorder clippedGradient(QSize(320, 180), 1.5);
    {
        QPainter p(&clippedGradient);
        p.translate(7, 9); p.scale(2, 1.5);
        p.setClipRect(QRectF(20, 40, 25, 35));
        QLinearGradient gradient(0, 20, 0, 80);
        gradient.setColorAt(0, Qt::red); gradient.setColorAt(1, Qt::blue);
        p.fillRect(QRectF(10, 20, 100, 60), gradient);
    }
    const auto clippedRamp = clippedGradient.takeMeshes();
    if (clippedRamp.size() != 1 || clippedRamp[0].vertices.size() != 6 ||
        clippedRamp[0].texture.cacheKey() != ramps[0].texture.cacheKey()) return 1;
    for (const auto& vertex : clippedRamp[0].vertices) {
        const bool top = std::abs(vertex.y - 69.f) < .001f;
        const bool bottom = std::abs(vertex.y - 121.5f) < .001f;
        const double rampPosition = top ? 1./3 : 55./60;
        const double expectedV = (rampPosition * 1023. + .5) / 1024.;
        if ((!top && !bottom) || (vertex.x != 47.f && vertex.x != 97.f) ||
            std::abs(vertex.v - expectedV) > 1e-6 || vertex.u != .5f) {
            std::cerr << "Clipping/scaling a meter changed its colour scale\n"; return 1;
        }
    }
    ui::graphics::SceneRecorder transparentGradient(QSize(100, 100), 1);
    {
        QPainter p(&transparentGradient);
        p.setOpacity(.5);
        QLinearGradient gradient(0, 0, 0, 100);
        gradient.setColorAt(0, Qt::transparent); gradient.setColorAt(1, Qt::blue);
        p.fillRect(QRectF(0, 0, 100, 100), gradient);
    }
    const auto transparentRamps = transparentGradient.takeMeshes();
    if (transparentRamps.size() != 1 || !transparentRamps[0].texture.hasAlphaChannel() ||
        transparentRamps[0].texture.pixelColor(0, 0).alpha() != 0 || transparentRamps[0].opacity != .5) {
        std::cerr << "A translucent gradient lost alpha or painter opacity\n"; return 1;
    }
    auto retained = std::make_shared<ui::graphics::SceneRecordingCache>();
    std::shared_ptr<const ui::graphics::SceneMeshGroup> original;
    for (int frame = 0; frame < 2; ++frame) {
        ui::graphics::SceneRecorder pass(QSize(320, 140), 2, retained);
        {
            QPainter p(&pass);
            p.translate(7, 9);
            p.setOpacity(.5);
            auto* sink = ui::graphics::sceneGeometrySink(p);
            const bool needsDrawing = sink->beginRetainedSection(42, frame == 0);
            if (needsDrawing != (frame == 0)) return 1;
            if (needsDrawing) { p.fillRect(QRectF(1, 2, 20, 30), Qt::red); sink->endRetainedSection(); }
        }
        const auto recorded = pass.takeMeshes();
        if (recorded.size() != 1 || !recorded.front().group ||
            !recorded.front().transform.isIdentity() || recorded.front().opacity != 1) return 1;
        const auto group = recorded.front().group;
        if (group->meshes.size() != 1 || group->meshes.front().opacity != .5 ||
            group->meshes.front().vertices.front().x != 8 ||
            group->meshes.front().vertices.front().y != 11) return 1;
        if (!original) original = group;
        else if (group != original) {
            std::cerr << "Unchanged sections must reuse the complete immutable group\n"; return 1;
        }
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
    auto mediaSection = std::make_shared<ui::graphics::SceneRecordingCache>();
    for (int frame = 0; frame < 2; ++frame) {
        ui::graphics::SceneRecorder pass(QSize(320, 140), 1, mediaSection);
        {
            QPainter p(&pass);
            auto* sink = ui::graphics::sceneGeometrySink(p);
            if (!sink->beginRetainedSection(77, false)) return 1;
            p.fillRect(QRect(0, 0, 20, 20), Qt::red);
            sink->appendVisual(123, QRectF(20, 0, 20, 20));
            p.fillRect(QRect(40, 0, 20, 20), Qt::blue);
            sink->endRetainedSection();
        }
        const auto result = pass.takeMeshes();
        if (result.size() != 3 || result[1].visualId != 123 || result[0].group || result[2].group) {
            std::cerr << "A retained section hid a Quick visual or changed its stacking order\n"; return 1;
        }
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
    // Fill, touch and replace retained tiles: eviction must preserve recently
    // reused geometry, and invalidation must unlink its recency entry as well.
    tiles.clear();
    const auto retainTile = [&](quint64 key) {
        ui::graphics::SceneRecorder pass(QSize(32, 32), 2);
        QPainter painter(&pass);
        tiles.paint(painter, key, QSize(32, 32), {}, [](QPainter& local) {
            local.fillRect(QRect(0, 0, 16, 16), Qt::green);
        });
    };
    for (quint64 key = 0; key < 512; ++key) retainTile(key);
    const auto fullBuilds = tiles.builds();
    retainTile(0); retainTile(512); retainTile(0);
    if (tiles.builds() != fullBuilds + 1) return 1;
    retainTile(1); // Oldest untouched entry was evicted.
    if (tiles.builds() != fullBuilds + 2) return 1;
    tiles.invalidate([](quint64 key) { return key == 0 || key == 512; });
    retainTile(0); retainTile(512);
    if (tiles.builds() != fullBuilds + 4) return 1;
    const auto nativeTrigger = app.styleHints()->contextMenuTrigger();
    for (const auto trigger : {Qt::ContextMenuTrigger::Release, Qt::ContextMenuTrigger::Press}) {
        app.styleHints()->setContextMenuTrigger(trigger);
        const bool routed = checkContextMenuRouting(trigger);
        app.styleHints()->setContextMenuTrigger(nativeTrigger);
        if (!routed) return 1;
    }
    const bool softwareFallback = app.arguments().contains("--software-fallback");
    if (!app.arguments().contains("--hardware") && !softwareFallback) return 0;
    QWidget window;
    window.resize(320, 140);
    auto* canvas = new Canvas(&window); canvas->setGeometry(window.rect());
    // An ordinary native widget must not be mistaken for a foreign plugin
    // surface. Keep its ancestors alien, as the app's editor frames do.
    QWidget ordinaryNative(canvas);
    ordinaryNative.setGeometry(260, 5, 40, 10);
    ordinaryNative.setAttribute(Qt::WA_DontCreateNativeAncestors);
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
    {
        PlainControl early(canvas);
        early.setGeometry(260, 110, 30, 20); early.show();
        QTimer::singleShot(100, &loop, &QEventLoop::quit); loop.exec();
        early.color = Qt::blue; early.update();
        QTimer::singleShot(100, &loop, &QEventLoop::quit); loop.exec();
        if (surface->quickWindow()->grabWindow().pixelColor(int(270*dpr), int(120*dpr)) != QColor(Qt::blue)) {
            std::cerr << "Initial native exposure stranded ordinary widget damage\n"; return 1;
        }
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
    {
        // A single pointer position needs one new scene, not a second submit
        // caused by recording Quick item changes after its polish phase.
        const auto frames = std::make_shared<std::atomic<int>>(0);
        const auto connection = QObject::connect(surface->quickWindow(), &QQuickWindow::frameSwapped, &app,
            [frames] { ++*frames; }, Qt::DirectConnection);
        const int paintsBefore = canvas->paints;
        for (int sample = 1; sample <= 20; ++sample) {
            canvas->pan = QPointF(sample * .25, sample * .125);
            canvas->update();
        }
        QTimer::singleShot(150, &loop, &QEventLoop::quit); loop.exec();
        QObject::disconnect(connection);
        if (frames->load() != 1 || canvas->paints - paintsBefore != 1) {
            std::cerr << "Coalesced input produced duplicate GPU frames: paints=" << canvas->paints - paintsBefore
                      << " frames=" << frames->load() << '\n';
            return 1;
        }
        canvas->pan = {}; canvas->update();
        QTimer::singleShot(100, &loop, &QEventLoop::quit); loop.exec();
    }
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
    // A transparent child needs its ancestors painted in QWidget's backing
    // store. The GPU scene already retains those layers: local control edits
    // must not rerecord them, including synchronous repaint() calls.
    for (const bool immediate : {false, true}) {
        canvas->paints = control.paints = 0;
        control.color = immediate ? Qt::yellow : Qt::green;
        if (immediate) control.repaint(); else control.update();
        QTimer::singleShot(80, &loop, &QEventLoop::quit); loop.exec();
        if (canvas->paints || !control.paints ||
            surface->quickWindow()->grabWindow().pixelColor(int(270*dpr), int(120*dpr)) != control.color) {
            std::cerr << "Local control damage rerecorded its parent or lost its pixels: parent="
                      << canvas->paints << " control=" << control.paints << " immediate=" << immediate << '\n';
            return 1;
        }
    }
    // QWidget::scroll also invalidates newly exposed pixels without changing
    // the widget's geometry (for example a code editor's line-number gutter).
    control.color = Qt::cyan;
    control.scroll(0, 5);
    QTimer::singleShot(80, &loop, &QEventLoop::quit); loop.exec();
    if (surface->quickWindow()->grabWindow().pixelColor(int(270*dpr), int(120*dpr)) != control.color) {
        std::cerr << "In-place QWidget scrolling lost its newly exposed content\n"; return 1;
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
    scroll.viewport()->setProperty("vlt.staticScrollViewport", true);
    scroll.setGeometry(10, 10, 200, 100);
    scroll.setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* page = new PlainControl;
    page->setProperty("vlt.retainOffscreen", true);
    page->setProperty("vlt.scrollLayerGroup", true);
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

    // A console returning to a recent column reuses its recording, while
    // edits made beyond the viewport must still invalidate that recording.
    scroll.verticalScrollBar()->setValue(140);
    QTimer::singleShot(80, &loop, &QEventLoop::quit); loop.exec();
    tile->paints = 0;
    scroll.verticalScrollBar()->setValue(80);
    QTimer::singleShot(80, &loop, &QEventLoop::quit); loop.exec();
    if (tile->paints) {
        std::cerr << "Returning to a recent mixer column rerecorded unchanged controls\n"; return 1;
    }
    for (const bool parentUpdate : {false, true}) {
        scroll.verticalScrollBar()->setValue(140);
        QTimer::singleShot(80, &loop, &QEventLoop::quit); loop.exec();
        tile->color = parentUpdate ? Qt::cyan : Qt::yellow;
        if (parentUpdate) page->update(); else tile->update();
        QTimer::singleShot(80, &loop, &QEventLoop::quit); loop.exec();
        scroll.verticalScrollBar()->setValue(80);
        QTimer::singleShot(80, &loop, &QEventLoop::quit); loop.exec();
        if (surface->quickWindow()->grabWindow().pixelColor(int(top.x()*dpr), int(top.y()*dpr)) != tile->color) {
            std::cerr << "An offscreen mixer edit exposed a stale cached control\n"; return 1;
        }
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
    // The common viewport clip must be pixel-equivalent to flattened layers,
    // including a partial channel, child masks, and reversing both axes.
    moveTo(QPointF(280, 130));
    page->setFixedSize(480, 240);
    tile->setMask(QRegion(tile->rect()).subtracted(QRegion(0, 0, 20, 20)));
    for (const QPoint position : {QPoint(0, 0), QPoint(11, 37), QPoint(230, 140), QPoint(11, 37), QPoint(0, 0)}) {
        scroll.horizontalScrollBar()->setValue(position.x());
        scroll.verticalScrollBar()->setValue(position.y());
        QTimer::singleShot(80, &loop, &QEventLoop::quit); loop.exec();
        const QImage grouped = surface->quickWindow()->grabWindow();
        page->setProperty("vlt.scrollLayerGroup", false);
        surface->invalidate();
        QTimer::singleShot(80, &loop, &QEventLoop::quit); loop.exec();
        const QImage flat = surface->quickWindow()->grabWindow();
        page->setProperty("vlt.scrollLayerGroup", true);
        surface->invalidate();
        QTimer::singleShot(80, &loop, &QEventLoop::quit); loop.exec();
        bool samePixels = grouped.size() == flat.size();
        const QPoint viewportOrigin = scroll.viewport()->mapTo(canvas, QPoint());
        const QRectF viewportPixels(QPointF(viewportOrigin) * dpr, QSizeF(scroll.viewport()->size()) * dpr);
        for (int y = 0; samePixels && y < grouped.height(); ++y)
            for (int x = 0; samePixels && x < grouped.width(); ++x) {
                if (grouped.pixel(x, y) == flat.pixel(x, y)) continue;
                // The flattened path rounds each child transform separately,
                // allowing a half-pixel leak past the viewport at fractional
                // DPI. The common scissor fixes that; all interior pixels and
                // child masks must still match exactly.
                samePixels = viewportPixels.adjusted(-1, -1, 1, 1).contains(QPointF(x, y)) &&
                    (x <= viewportPixels.left() + 1 || x >= viewportPixels.right() - 1 ||
                     y <= viewportPixels.top() + 1 || y >= viewportPixels.bottom() - 1);
                if (!samePixels) std::cerr << "Difference " << x << ',' << y << " viewport "
                    << viewportPixels.x() << ',' << viewportPixels.y() << ',' << viewportPixels.width()
                    << ',' << viewportPixels.height() << '\n';
            }
        const QImage fresh = surface->quickWindow()->grabWindow();
        if (!samePixels || grouped != fresh) {
            std::cerr << "Grouped scroll changed pixels at " << position.x() << ',' << position.y()
                      << " flat=" << samePixels << " fresh=" << (grouped == fresh) << '\n';
            if (const auto shots = qEnvironmentVariable("VLT_SCROLL_TEST_SHOTS"); !shots.isEmpty()) {
                grouped.save(shots + "-grouped.png"); flat.save(shots + "-flat.png");
                fresh.save(shots + "-fresh.png");
            }
            return 1;
        }
    }
    std::cout << "PASS grouped scroll, content damage, offscreen edits, masks and reversal\n";
    scroll.hide();
    {
        // An item view scrolls both viewport pixels and embedded editors. Keep
        // those pixels current, without repainting it when an overlay moves.
        TableControl table(20, 1, canvas);
        table.setGeometry(5, 5, 210, 125);
        table.horizontalHeader()->hide(); table.verticalHeader()->hide();
        table.setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        table.setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        for (int row = 0; row < table.rowCount(); ++row) {
            auto* item = new QTableWidgetItem(QString::number(row));
            item->setBackground(row % 2 ? Qt::blue : Qt::yellow);
            table.setItem(row, 0, item);
            table.setRowHeight(row, 25);
        }
        table.setCellWidget(2, 0, new PlainControl);
        PlainControl overlay(canvas);
        overlay.setGeometry(120, 15, 60, 50);
        table.show(); overlay.show();
        QTimer::singleShot(100, &loop, &QEventLoop::quit); loop.exec();
        table.paints = 0;
        overlay.move(145, 30);
        QTimer::singleShot(80, &loop, &QEventLoop::quit); loop.exec();
        if (table.paints) {
            std::cerr << "Moving an overlay rerecorded an unchanged viewport\n"; return 1;
        }
        table.verticalScrollBar()->setValue(40);
        QTimer::singleShot(80, &loop, &QEventLoop::quit); loop.exec();
        const auto retained = surface->quickWindow()->grabWindow();
        if (!table.paints) { std::cerr << "Item-view scroll lost viewport damage\n"; return 1; }
        table.viewport()->update();
        QTimer::singleShot(80, &loop, &QEventLoop::quit); loop.exec();
        if (retained != surface->quickWindow()->grabWindow()) {
            std::cerr << "Scrolled item-view pixels differ from a fresh repaint\n"; return 1;
        }
    }
    // Moving an editor or resizing a mixer exposes QWidget backing-store
    // regions, but must not rerecord unchanged layers in the retained scene.
    PlainControl floating(canvas), child(&floating), stationary(canvas);
    floating.setGeometry(10, 10, 70, 45); floating.color = Qt::cyan;
    child.setGeometry(15, 15, 15, 15); child.color = Qt::magenta;
    stationary.setGeometry(110, 15, 30, 30); stationary.color = Qt::yellow;
    floating.show(); child.show(); stationary.show();
    QTimer::singleShot(150, &loop, &QEventLoop::quit); loop.exec();
    canvas->paints = floating.paints = child.paints = stationary.paints = 0;
    floating.move(20, 20);
    // The Quick frame may precede Qt's lower-priority backing-store update.
    // Consuming a frame must not forget the still-pending exposure damage.
    QMetaObject::invokeMethod(surface->quickWindow(), "afterAnimating", Qt::DirectConnection);
    QTimer::singleShot(80, &loop, &QEventLoop::quit); loop.exec();
    const auto movedFrame = surface->quickWindow()->grabWindow();
    if (canvas->paints || floating.paints || child.paints || stationary.paints ||
        movedFrame.pixelColor(int(25*dpr), int(25*dpr)) != QColor(Qt::cyan) ||
        movedFrame.pixelColor(int(40*dpr), int(40*dpr)) != QColor(Qt::magenta)) {
        std::cerr << "Moving an editor rerecorded unchanged layers or lost its transform\n"; return 1;
    }
    floating.resize(80, 50);
    QTimer::singleShot(80, &loop, &QEventLoop::quit); loop.exec();
    if (!floating.paints || child.paints || stationary.paints || canvas->paints ||
        surface->quickWindow()->grabWindow().pixelColor(int(95*dpr), int(65*dpr)) != QColor(Qt::cyan)) {
        std::cerr << "Resizing an overlay repainted unaffected widgets or lost new content\n"; return 1;
    }
    stationary.color = Qt::green;
    stationary.update();
    floating.move(25, 25);
    QTimer::singleShot(80, &loop, &QEventLoop::quit); loop.exec();
    if (!stationary.paints || surface->quickWindow()->grabWindow().pixelColor(
        int(120*dpr), int(25*dpr)) != QColor(Qt::green)) {
        std::cerr << "An explicit update was lost while moving an overlay\n"; return 1;
    }
    floating.hide();
    {
        PlainControl nativeFrame(canvas);
        nativeFrame.setGeometry(95, 10, 80, 55);
        nativeFrame.setAttribute(Qt::WA_OpaquePaintEvent);
        nativeFrame.setAttribute(Qt::WA_DontCreateNativeAncestors);
        nativeFrame.setAttribute(Qt::WA_NativeWindow);
        nativeFrame.setProperty("vlt.nativeOverlay", true);
        FrameControl nativeControl(&nativeFrame);
        nativeControl.setGeometry(4, 4, 40, 30);
        nativeFrame.show(); nativeControl.show(); nativeFrame.raise();
        QTimer::singleShot(150, &loop, &QEventLoop::quit); loop.exec();
        canvas->paints = stationary.paints = nativeControl.paints = 0;
        nativeControl.color = Qt::blue;
        nativeControl.update();
        QTimer::singleShot(150, &loop, &QEventLoop::quit); loop.exec();
        if (!nativeControl.paints || canvas->paints || stationary.paints) {
            std::cerr << "Native editor damage was swallowed or repainted the workspace beneath it: editor="
                      << nativeControl.paints << " canvas=" << canvas->paints << " stationary=" << stationary.paints << '\n'; return 1;
        }
        // A model change under an opaque native editor must survive until the
        // editor moves away, without rebuilding all other mixer-like controls.
        stationary.color = Qt::cyan; stationary.update();
        QTimer::singleShot(80, &loop, &QEventLoop::quit); loop.exec();
        if (!ui::graphics::moveNativeOverlay(&nativeFrame, QPoint(10, 70))) return 1;
        QTimer::singleShot(150, &loop, &QEventLoop::quit); loop.exec();
        if (surface->quickWindow()->grabWindow().pixelColor(int(120*dpr), int(25*dpr)) != QColor(Qt::cyan)) {
            std::cerr << "Moving an opaque editor exposed stale workspace content\n"; return 1;
        }
        canvas->paints = stationary.paints = 0;
        for (int i = 0; i < 12; ++i) {
            const QPoint position(15 + i * 3, 70 - i * 2);
            if (!ui::graphics::moveNativeOverlay(&nativeFrame, position)) return 1;
            if (nativeFrame.pos() != position) {
                std::cerr << "Native movement lost QWidget coordinates\n"; return 1;
            }
            QTimer::singleShot(20, &loop, &QEventLoop::quit); loop.exec();
        }
        if (canvas->paints || stationary.paints) {
            std::cerr << "Native editor movement rerecorded the workspace: canvas=" << canvas->paints
                      << " stationary=" << stationary.paints << '\n'; return 1;
        }
#ifdef Q_OS_WIN
        const QImage nativePixels = window.screen()->grabWindow(nativeFrame.winId()).toImage();
        if (nativePixels.isNull() || nativePixels.pixelColor(int(12*dpr), int(12*dpr)) != Qt::blue ||
            nativePixels.pixelColor(int(65*dpr), int(40*dpr)) != Qt::red) {
            std::cerr << "Native editor pixels were lost after movement\n"; return 1;
        }
#endif
        // Coalesced real updates, including a partial parent update, must still
        // reach the retained scene while the native editor is moving.
        stationary.color = Qt::green;
        nativeControl.color = Qt::yellow;
        nativeControl.paints = 0;
        if (!ui::graphics::moveNativeOverlay(&nativeFrame, QPoint(20, 70))) return 1;
        canvas->QWidget::update(stationary.geometry()); nativeControl.update();
        QTimer::singleShot(80, &loop, &QEventLoop::quit); loop.exec();
        if (!nativeControl.paints || !stationary.paints ||
            surface->quickWindow()->grabWindow().pixelColor(int(120*dpr), int(25*dpr)) != QColor(Qt::green)) {
            std::cerr << "Native movement suppressed a real content update: editor=" << nativeControl.paints
                      << " stationary=" << stationary.paints << '\n'; return 1;
        }
    }
    stationary.hide();
    // A rounded editor clips all descendants, including opaque children and
    // native control textures. Moving/resizing it must update that clip without
    // rerecording unchanged children or retaining the old rectangular corners.
    QTimer::singleShot(80, &loop, &QEventLoop::quit); loop.exec();
    const QImage behindRounded = surface->quickWindow()->grabWindow();
    PlainControl rounded(canvas), opaque(&rounded);
    QToolButton maskedControl(&rounded);
    rounded.color = Qt::cyan; opaque.color = Qt::magenta;
    rounded.setGeometry(10, 10, 90, 90);
    opaque.setGeometry(0, 0, 90, 90);
    maskedControl.setStyleSheet("background: yellow; border: none;");
    maskedControl.setGeometry(0, 0, 40, 40);
    const auto round = [&] {
        QPainterPath path; path.addRoundedRect(QRectF(rounded.rect()), 20, 20);
        rounded.setMask(QRegion(path.toFillPolygon().toPolygon()));
    };
    round(); rounded.show(); opaque.show(); maskedControl.show();
    for (int pass = 0; pass < 3; ++pass) {
        QTimer::singleShot(80, &loop, &QEventLoop::quit); loop.exec();
        const QImage pixels = surface->quickWindow()->grabWindow();
        const auto pixel = [&](QPoint point) { return pixels.pixelColor(int(point.x()*dpr), int(point.y()*dpr)); };
        for (const QPoint corner : {rounded.rect().topLeft(), rounded.rect().topRight(),
                                    rounded.rect().bottomLeft(), rounded.rect().bottomRight()}) {
            const QPoint position = rounded.mapTo(canvas, corner);
            if (pixel(position) != behindRounded.pixelColor(int(position.x()*dpr), int(position.y()*dpr))) {
                std::cerr << "A rounded ancestor leaked square child corners in the GPU scene\n"; return 1;
            }
        }
        if (pixel(rounded.mapTo(canvas, QPoint(25, 25))) != QColor(Qt::yellow) ||
            pixel(rounded.mapTo(canvas, QPoint(45, 45))) != QColor(Qt::magenta)) {
            std::cerr << "Ancestor mask clipped the interior content\n"; return 1;
        }
        rounded.move(15 + pass*5, 15 + pass*5);
        rounded.resize(85 - pass*5, 85 - pass*5);
        round();
    }
    // Mask changes do not require a move/resize. Cached descendant clips must
    // follow both removing and restoring the mask during a local update.
    QTimer::singleShot(80, &loop, &QEventLoop::quit); loop.exec();
    rounded.clearMask(); rounded.update();
    QTimer::singleShot(80, &loop, &QEventLoop::quit); loop.exec();
    const QPoint maskCorner = rounded.mapTo(canvas, QPoint(1, 1));
    if (surface->quickWindow()->grabWindow().pixelColor(int(maskCorner.x()*dpr), int(maskCorner.y()*dpr)) != QColor(Qt::yellow)) {
        std::cerr << "Removing a mask retained an old descendant clip\n"; return 1;
    }
    round(); rounded.update();
    QTimer::singleShot(80, &loop, &QEventLoop::quit); loop.exec();
    if (surface->quickWindow()->grabWindow().pixelColor(int(maskCorner.x()*dpr), int(maskCorner.y()*dpr)) !=
        behindRounded.pixelColor(int(maskCorner.x()*dpr), int(maskCorner.y()*dpr))) {
        std::cerr << "Restoring a mask leaked cached descendant pixels\n"; return 1;
    }
    rounded.hide();
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
    Canvas resizeStable(canvas);
    resizeStable.setGeometry(5, 5, 30, 20); resizeStable.show();
    QTimer::singleShot(80, &loop, &QEventLoop::quit); loop.exec();
    const int cachedBuilds = resizeStable.staticBuilds;
    canvas->paints = 0;
    window.resize(360, 160);
    canvas->setGeometry(window.rect());
    QTimer::singleShot(80, &loop, &QEventLoop::quit); loop.exec();
    const QImage resized = surface->quickWindow()->grabWindow();
    if (resizeStable.staticBuilds != cachedBuilds || !canvas->paints ||
        resized.pixelColor(int(350*dpr), int(150*dpr)) != palette.color(QPalette::Window)) {
        std::cerr << "Workspace resize rebuilt cached geometry or left exposed pixels\n"; return 1;
    }
    resizeStable.hide();
#ifdef Q_OS_WIN
    {
        QWidget editorHost(canvas);
        editorHost.setGeometry(canvas->rect()); editorHost.show();
        PlainControl nativeOverlay(&editorHost);
        nativeOverlay.setAttribute(Qt::WA_DontCreateNativeAncestors);
        nativeOverlay.setAttribute(Qt::WA_NativeWindow);
        nativeOverlay.setProperty("vlt.nativeOverlay", true);
        nativeOverlay.setGeometry(10, 10, 70, 50); nativeOverlay.show();
        window.raise(); window.activateWindow();
        nativeOverlay.windowHandle()->raise();
        QTimer::singleShot(80, &loop, &QEventLoop::quit); loop.exec();
        // The editor is already last in its QObject siblings. Resizing raises
        // the native Quick sibling, so QWidget::raise() alone is insufficient.
        canvas->resize(350, 155);
        QTimer::singleShot(80, &loop, &QEventLoop::quit); loop.exec();
        const HWND editor = reinterpret_cast<HWND>(nativeOverlay.winId());
        const HWND workspace = reinterpret_cast<HWND>(window.winId());
        POINT position{20, 20};
        MapWindowPoints(editor, workspace, &position, 1);
        const HWND hit = ChildWindowFromPointEx(workspace, position, CWP_SKIPINVISIBLE);
        if (!IsWindowVisible(editor) || hit == workspace ||
            (hit != editor && !IsChild(editor, hit) && !IsChild(hit, editor))) {
            std::cerr << "Workspace resize covered a live native editor\n"; return 1;
        }
        nativeOverlay.hide();
        if (IsWindowVisible(editor)) {
            std::cerr << "A hidden native editor remained visible behind the GPU scene\n"; return 1;
        }
    }
#endif
    {
        // Plain QWidget roots inherit an opaque window background even when
        // their own paintEvent is empty. The GPU surface must cover that area,
        // including newly exposed pixels before a resized scene is ready.
        QWidget opaqueRoot;
        opaqueRoot.resize(160, 90);
        opaqueRoot.setPalette(palette);
        ui::graphics::WorkspaceSurface opaqueSurface(&opaqueRoot);
        opaqueRoot.show();
        QTimer::singleShot(100, &loop, &QEventLoop::quit); loop.exec();
        if (opaqueSurface.quickWindow()->grabWindow().pixelColor(10, 10) != palette.color(QPalette::Window)) {
            std::cerr << "An opaque workspace retained a transparent GPU backdrop\n"; return 1;
        }
        auto changedPalette = palette;
        changedPalette.setColor(QPalette::Window, QColor(12, 30, 48));
        opaqueRoot.setPalette(changedPalette);
        QTimer::singleShot(80, &loop, &QEventLoop::quit); loop.exec();
        if (opaqueSurface.quickWindow()->grabWindow().pixelColor(10, 10) != changedPalette.color(QPalette::Window)) {
            std::cerr << "GPU backdrop did not follow the workspace palette\n"; return 1;
        }
        QWidget translucentRoot;
        translucentRoot.setAttribute(Qt::WA_TranslucentBackground);
        ui::graphics::WorkspaceSurface translucentSurface(&translucentRoot);
        if (translucentSurface.quickWindow()->color().alpha() != 0) {
            std::cerr << "A translucent editor lost its transparent corners\n"; return 1;
        }
    }
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
