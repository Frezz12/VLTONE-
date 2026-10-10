#include "CreatorCanvas.hpp"
#include "ScrollMotion.hpp"
#include <QApplication>
#include <QEventLoop>
#include <QGraphicsScene>
#include <QNativeGestureEvent>
#include <QLineF>
#include <QPointingDevice>
#include <QScrollBar>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QWheelEvent>
#include <cmath>
#include <cstdio>

namespace {
int failures = 0;
void check(bool pass, const char* label) {
    std::printf("%s Creator navigation: %s\n", pass ? "PASS" : "FAIL", label);
    failures += !pass;
}
void wait(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QCoreApplication::setOrganizationName("VltTest");
    QCoreApplication::setApplicationName("CreatorNavigation");
    ui::ScrollMotion::install();
    auto& prefs = ui::ScrollPreferences::instance();
    prefs.setEnabled(true); prefs.setReducedMotion(false);
    prefs.setStrength(65); prefs.setSpeed(100);
    ui::CreatorCanvas canvas;
    canvas.resize(800, 600);
    canvas.scene()->addRect(QRectF(0, 0, 200, 120));
    canvas.show(); wait(30);
    canvas.restoreViewport({{200, 200}, 1});
    const QPointF anchor(83.5, 121.25);
    const auto scenePoint = [&] { return canvas.viewportTransform().inverted().map(anchor); };
    const auto wheel = [&](QPoint pixels, QPoint angles, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QWheelEvent event(anchor, canvas.viewport()->mapToGlobal(anchor), pixels, angles,
            Qt::NoButton, modifiers, Qt::NoScrollPhase, false);
        QApplication::sendEvent(canvas.viewport(), &event);
    };
    const auto native = [&](Qt::NativeGestureType type, double value = 0) {
        QNativeGestureEvent event(type, QPointingDevice::primaryPointingDevice(), 2,
            anchor, anchor, canvas.viewport()->mapToGlobal(anchor), value, {});
        QApplication::sendEvent(canvas.viewport(), &event);
        return event.isAccepted();
    };
    const auto beforePan = canvas.viewportState();
    wheel({}, {0, 1200}); wait(240);
    check(canvas.viewportState().center.y() < 0 &&
          canvas.viewportState().center.y() < beforePan.center.y(),
          "wheel scrolls into empty space above the uppermost nodes");
    const double above = canvas.viewportState().center.y();
    wheel({}, {0, 120}); wait(240);
    check(canvas.viewportState().center.y() < above, "wheel keeps moving above scene origin");
    wheel({}, {1200, 0}); wait(240);
    check(canvas.viewportState().center.x() < 0, "wheel also scrolls left of scene origin");
    wheel({24, 31}, {});
    const auto precise = canvas.viewportState().center;
    wait(240);
    check(canvas.viewportState().center == precise, "trackpad pan has no extra inertia tail");

    canvas.restoreViewport({{200, 200}, 1});
    const auto originalAnchor = scenePoint();
    int zoomSignals = 0;
    QObject::connect(&canvas, &ui::CreatorCanvas::zoomChanged, [&] { ++zoomSignals; });
    native(Qt::BeginNativeGesture);
    const bool accepted = native(Qt::ZoomNativeGesture, .25);
    check(accepted && std::abs(canvas.transform().m11() - 1.25) < 1e-9 && zoomSignals == 1,
          "two-finger pinch zooms in immediately and updates zoom readout");
    check(QLineF(canvas.viewportTransform().map(originalAnchor), anchor).length() < 2,
          "pinch keeps an off-center pointer anchor stable");
    native(Qt::ZoomNativeGesture, -.2);
    native(Qt::EndNativeGesture);
    check(std::abs(canvas.transform().m11() - 1) < 1e-9,
          "reversing the pinch zooms out immediately");
    wheel({0, 80}, {}, Qt::ControlModifier);
    check(canvas.transform().m11() > 1, "modifier zoom accepts pixel-only trackpad events");

    canvas.restoreViewport({{200, 200}, 1});
    const auto repeatAnchor = scenePoint();
    native(Qt::BeginNativeGesture);
    for (int i = 0; i < 20; ++i) native(Qt::ZoomNativeGesture, .02);
    native(Qt::EndNativeGesture);
    check(QLineF(canvas.viewportTransform().map(repeatAnchor), anchor).length() < 2,
          "a sustained pinch does not drift away from its anchor");
    native(Qt::ZoomNativeGesture, 100);
    check(canvas.transform().m11() == 2.5, "pinch respects maximum zoom");
    native(Qt::ZoomNativeGesture, -.99);
    check(canvas.transform().m11() == .25, "pinch respects minimum zoom");
    native(Qt::ZoomNativeGesture, -2);
    check(canvas.transform().m11() == .25, "invalid scale cannot invert the canvas");

    canvas.restoreViewport({{200, 200}, 1});
    wheel({}, {0, -120});
    native(Qt::BeginNativeGesture);
    native(Qt::ZoomNativeGesture, .1);
    native(Qt::EndNativeGesture);
    const auto afterPinch = canvas.viewportState();
    wait(240);
    check(canvas.viewportState() == afterPinch, "pinch cancels pending wheel animation");
    return failures ? 1 : 0;
}
