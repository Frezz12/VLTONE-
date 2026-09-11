#include "UiFrameClock.hpp"
#include "UiFrameCadence.hpp"
#include "UiPerformance.hpp"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <QApplication>
#include <QEventLoop>
#include <QElapsedTimer>
#include <cmath>
#include <QSettings>
#include <QScreen>
#include <QTemporaryDir>
#include <QTimer>
#include <QWindow>
#include <QPaintEvent>
#include <QPainter>
#include <iostream>

class PaintProbe : public ui::FrameWidget {
public:
    using ui::FrameWidget::FrameWidget;
    int paints = 0;
    QRegion damage;
    void reset() { paints = 0; damage = {}; }
protected:
    void paintEvent(QPaintEvent* event) override {
        ++paints;
        damage += event->region();
        QPainter painter(this);
        painter.fillRect(event->rect(), Qt::black);
    }
};

static double run(int ms) {
    QElapsedTimer elapsed; elapsed.start();
    QEventLoop loop; QTimer::singleShot(ms, &loop, &QEventLoop::quit); loop.exec();
    return double(elapsed.nsecsElapsed()) / 1e9;
}
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QTemporaryDir prefs;
    qputenv("VLT_UI_PROFILE", (prefs.path() + "/frames.json").toUtf8());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, prefs.path());
    QCoreApplication::setOrganizationName("VltTest");
    QCoreApplication::setApplicationName("FrameClock");
    int failures = 0;
    const auto check = [&](bool ok, const char* what) {
        if (!ok) { ++failures; std::cerr << "FAIL: " << what << '\n'; }
    };
    // Model native vsync deterministically, including deliveries a little
    // early. A strict `last delivered + period` test loses alternate frames.
    for (double displayHz : {59.94, 60.0, 120.0, 144.0}) {
        for (int limit : {30, 60, 120, 144, 240}) {
          for (qint64 jitter : {100000, 1250000}) {
            ui::detail::FrameCadence cadence;
            const qint64 period = qint64(1e9 / limit);
            int frames = 0;
            qint64 first = 0, last = 0;
            for (int i = 0; i < int(displayHz * 10); ++i) {
                const qint64 now = 1000000000 + qint64(i * 1e9 / displayHz) +
                                   (i % 2 ? -jitter : jitter);
                if (!cadence.due(now, period)) continue;
                if (!frames) first = now;
                last = now;
                ++frames;
                cadence.advance(now, period);
            }
            const double seconds = double(last - first) / 1e9;
            const double expected = seconds * std::min(displayHz, double(limit));
            check(frames >= int(expected) - 1 && frames <= int(std::ceil(expected)) + 2,
                  "vsync jitter preserves the requested cadence without exceeding its average cap");
          }
        }
    }
    ui::detail::FrameCadence stalled;
    stalled.advance(1000000000, 16666667);
    check(stalled.due(5000000000, 16666667), "a delayed GUI resumes immediately");
    stalled.advance(5000000000, 16666667);
    check(!stalled.due(5001000000, 16666667), "a delayed GUI never bursts catch-up frames");
    check(stalled.due(5001000000, 0), "unlimited bypasses a previous deadline");
    QWidget first, second;
    first.resize(200, 100); second.resize(200, 100);
    const QPoint origin = app.primaryScreen()->availableGeometry().topLeft() + QPoint(40, 40);
    first.move(origin); second.move(origin + QPoint(240, 0));
    first.show(); second.show();
    auto& clock = ui::FrameClock::instance();
    check(clock.mode() == ui::FrameMode::Display, "new preferences follow the display");
    clock.setPreference(ui::FrameMode::Fixed, 60);
    ui::FrameTimer a(&first), b(&second);
    int ticksA = 0, ticksB = 0;
    QObject::connect(&a, &ui::FrameTimer::timeout, &first, [&] { ++ticksA; check(a.deltaSeconds() > 0, "positive elapsed time"); });
    QObject::connect(&b, &ui::FrameTimer::timeout, &second, [&] { ++ticksB; });
    // Native window exposure can take longer than the offscreen platform,
    // especially at process launch. Keep it outside the steady cadence sample.
    for (int i = 0; i < 100 &&
         (!first.windowHandle()->isExposed() || !second.windowHandle()->isExposed()); ++i)
        run(20);
    run(80);
    a.start(); b.start();
    const double firstElapsed = run(320);
    const int firstCap = int(std::ceil(firstElapsed * 60)) + 1;
    std::cout << "60 FPS: " << ticksA << "/" << ticksB << " frames in " << firstElapsed << " s\n";
    check(ticksA > 0 && ticksA <= firstCap && ticksB > 0 && ticksB <= firstCap,
          "both windows obey 60 FPS cap over measured monotonic time");
    first.hide(); run(50); const int hidden = ticksA; const int visible = ticksB; run(100);
    check(ticksA == hidden && ticksB > visible, "hidden window sleeps independently");
    first.show(); run(80); check(ticksA > hidden, "shown window resumes");
    clock.setPreference(ui::FrameMode::Fixed, 30);
    const int before = ticksA;
    const double changedElapsed = run(350);
    check(ticksA > before && ticksA - before <= int(std::ceil(changedElapsed * 30)) + 1,
          "changed cap applies immediately");
    for (int fps : {120, 144, 240, 360, 87}) {
        clock.setPreference(ui::FrameMode::Fixed, fps);
        const int start = ticksA;
        const double elapsed = run(180);
        std::cout << fps << " FPS: " << ticksA - start << " frames in " << elapsed << " s\n";
        check(ticksA > start && ticksA - start <= int(std::ceil(fps * elapsed)) + 1,
              "high/custom cadence runs within its cap");
    }
    clock.setPreference(ui::FrameMode::Unlimited, 144);
    const int unlimited = ticksA; run(100);
    check(clock.periodSeconds(&first) == 0 && ticksA > unlimited, "unlimited has no application cap");
    clock.setPreference(ui::FrameMode::Display, 144);
    check(clock.periodSeconds(&first) > 0, "display cadence resolves a screen");
    check(QSettings().value("ui/frameMode").toString() == "display" && QSettings().value("ui/frameLimit").toInt() == 144, "mode and numeric preference persisted");
    a.stop(); b.stop(); run(50); const int stopped = ticksA; run(100); check(ticksA == stopped, "stopped timer stays idle");
    auto* temporary = new ui::FrameTimer(&first);
    QObject::connect(temporary, &ui::FrameTimer::timeout, &first, [temporary] { delete temporary; });
    temporary->start(); run(70); // deletion from callback must not invalidate dispatch

    PaintProbe window;
    PaintProbe canvas(&window), sibling(&window);
    window.resize(300, 150);
    canvas.setGeometry(0, 0, 140, 150);
    sibling.setGeometry(160, 0, 140, 150);
    window.show(); run(150);
    ui::FrameTimer cursorClock(&canvas);
    int cursorTicks = 0;
    const QRect strip(30, 10, 12, 100);
    QObject::connect(&cursorClock, &ui::FrameTimer::timeout, &canvas, [&] {
        ++cursorTicks;
        canvas.update(strip);
    });
    canvas.reset(); sibling.reset();
    cursorClock.start(); run(220); cursorClock.stop(); run(50);
    check(cursorTicks > 0 && canvas.paints > 0 && canvas.damage == QRegion(strip),
          "native frame pulses preserve the exact dirty strip");
    check(sibling.paints == 0, "a cursor frame never repaints unaffected sibling widgets");
    canvas.reset(); sibling.reset();
    ui::FrameTimer timerOnly(&canvas);
    timerOnly.start(); run(100); timerOnly.stop(); run(50);
    check(canvas.paints == 0 && sibling.paints == 0,
          "callbacks without damage never trigger a window repaint");

    QObject presenter;
    int requests = 0;
    QRegion presented;
    clock.setPresenter(&window, &presenter, window.windowHandle(), [&] { ++requests; },
        [&](QWidget* widget, const QRegion& region) {
            if (widget != &canvas) return false;
            presented += region; return true;
        });
    canvas.reset(); sibling.reset();
    canvas.update(strip); canvas.update(QRect(45, 10, 5, 100));
    run(40);
    check(requests == 1 && canvas.paints == 0 && presented.isEmpty(),
          "Quick owns one pending frame; QWidget does not consume its cadence");
    clock.presentationFrame(&window, &presenter);
    check(presented == (QRegion(strip) | QRect(45, 10, 5, 100)) && canvas.paints == 0,
          "damage reaches the current Quick frame without a QWidget paint round trip");
    run(80);
    check(requests == 1, "a static presented scene requests no successor frame");
    clock.clearPresenter(&window, &presenter);
    canvas.update(strip); run(80);
    check(canvas.paints > 0 && canvas.damage == QRegion(strip),
          "detaching a presenter restores QWidget scheduling and exact damage");

    auto* shortLived = new QObject;
    clock.setPresenter(&window, shortLived, window.windowHandle(), [&] { ++requests; },
        [](QWidget*, const QRegion&) { return true; });
    canvas.reset(); canvas.update(strip); // destroy before the queued request executes
    delete shortLived; run(80);
    check(canvas.paints > 0, "presenter destruction preserves pending damage without a dangling callback");
    // A blocked GUI really misses frames, even when the pause exceeds the old
    // 250 ms filter. Hiding/stopping a window is not the same kind of stall.
    const auto metric = [&](const char* name, const char* field) {
        ui::perf::flush();
        QFile file(prefs.path() + "/frames.json");
        if (!file.open(QIODevice::ReadOnly)) return -1.0;
        return QJsonDocument::fromJson(file.readAll()).object()[name].toObject()[field].toDouble();
    };
    clock.setPreference(ui::FrameMode::Fixed, 60);
    a.start(); run(100);
    const double oldStalls = metric("frame.stall.ms", "count");
    QThread::msleep(320); run(100);
    check(metric("frame.stall.ms", "count") > oldStalls &&
          metric("frame.interval.ms", "max") >= 300.0,
          "a GUI stall over 250 ms remains visible in frame diagnostics");
    first.hide(); run(60);
    const double beforeHide = metric("frame.stall.ms", "count");
    run(350); first.show(); run(100); a.stop();
    check(metric("frame.stall.ms", "count") == beforeHide,
          "an intentionally hidden window does not manufacture a stalled frame on resume");
    return failures ? 1 : 0;
}
