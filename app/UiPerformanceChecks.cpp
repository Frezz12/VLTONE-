#include "UiPerformanceChecks.hpp"
#include "UiPerformance.hpp"
#include "TrackListWidget.hpp"
#include "MixerWidget.hpp"
#include "TimelineWidget.hpp"
#include "Controls.hpp"
#include "UiFrameClock.hpp"
#include "UiConstants.hpp"
#include "Theme.hpp"
#include "EngineController.hpp"
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPointer>
#include <QPointingDevice>
#include <QPaintEvent>
#include <QPainter>
#include <QScreen>
#include <QScrollBar>
#include <QTimer>
#include <QWheelEvent>
#include <cmath>
#include <cstdio>

bool TimelineWidget::checkAdaptiveGridForTest() {
    auto& project = const_cast<daw::ProjectModel&>(m_controller->project());
    const double savedTempo = project.tempo;
    const int savedNumerator = project.timeSigNumerator;
    const int savedDenominator = project.timeSigDenominator;
    const double savedScale = m_pixelsPerSecond, savedScroll = m_scrollSeconds;
    const double savedGrid = m_gridBeats;
    const bool savedSnap = m_snapEnabled, savedBars = m_showBars;
    bool ok = true;
    m_gridBeats = -1.0; m_snapEnabled = true; m_showBars = true;
    for (double tempo : {120.0, 240.0, 300.0}) {
        project.tempo = tempo;
        for (const auto meter : {std::pair{4, 4}, std::pair{3, 4}, std::pair{6, 8}, std::pair{7, 8}}) {
            project.timeSigNumerator = meter.first;
            project.timeSigDenominator = meter.second;
            const double barBeats = meter.first * 4.0 / meter.second;
            for (double scale : {4.0, 8.0, 80.0, 320.0}) {
                m_pixelsPerSecond = scale;
                const double barPixels = barBeats * 60.0 / tempo * scale;
                const double step = effectiveGridBeats();
                const double stepSeconds = step * 60.0 / tempo;
                if (barPixels >= 32.0) continue;
                const double bars = step / barBeats;
                const double stepPixels = stepSeconds * scale;
                ok &= bars >= 2 && std::abs(std::log2(bars) - std::round(std::log2(bars))) < 1e-9;
                ok &= stepPixels >= 32.0 && stepPixels < 64.0;
                ok &= std::abs(snapSeconds() - stepSeconds) < 1e-9;
                ok &= std::abs(snap(2.7 * stepSeconds, true) - 3.0 * stepSeconds) < 1e-9;

                // Inspect actual grid pixels after a fractional viewport pan.
                // Beat/bar overlays must not secretly reintroduce dense lines.
                m_scrollSeconds = stepSeconds * 0.375;
                QImage raster(width(), ui::kRulerHeight + 5, QImage::Format_ARGB32_Premultiplied);
                raster.fill(Qt::transparent);
                { QPainter p(&raster); p.setClipRect(raster.rect()); drawGrid(p); }
                int lastLine = -1, lines = 0;
                bool inside = false;
                for (int x = 0; x < raster.width(); ++x) {
                    const bool ink = qAlpha(raster.pixel(x, ui::kRulerHeight + 2)) > 0;
                    if (ink && !inside) {
                        if (lastLine >= 0) ok &= x - lastLine >= stepPixels - 1.01;
                        // Snapping and rendered lines share the same bar-1 origin.
                        const double nearest = snap(xToSeconds(x), true);
                        ok &= std::abs((nearest - m_scrollSeconds) * scale - x) <= 1.01;
                        lastLine = x; ++lines;
                    }
                    inside = ink;
                }
                ok &= lines >= int(width() / stepPixels) - 1 && lines <= int(width() / stepPixels) + 1;
            }
        }
    }
    project.tempo = 120.0; project.timeSigNumerator = 4; project.timeSigDenominator = 4;
    m_pixelsPerSecond = 4.0; m_scrollSeconds = 0.0;
    ok &= effectiveGridBeats() == 16.0 && gridBarStride() == 4; // 4-bar overview
    const QString screenshot = qEnvironmentVariable("VLT_GRID_SCREENSHOT");
    if (!screenshot.isEmpty()) {
        QImage overview(width(), 200, QImage::Format_RGB32);
        overview.fill(th().background);
        { QPainter p(&overview); p.setClipRect(overview.rect()); drawGrid(p); drawRuler(p); }
        ok &= overview.save(screenshot);
    }
    m_pixelsPerSecond = 80.0;
    ok &= effectiveGridBeats() == 0.25; // existing fine-editing step
    m_pixelsPerSecond = 4.0;
    m_gridBeats = 0.25;
    ok &= effectiveGridBeats() == 0.25 && std::abs(snapSeconds() - 0.125) < 1e-9;
    m_gridBeats = 0.0;
    ok &= snapSeconds() == 0.0;
    m_gridBeats = savedGrid; m_pixelsPerSecond = savedScale; m_scrollSeconds = savedScroll;
    m_snapEnabled = savedSnap; m_showBars = savedBars;
    project.tempo = savedTempo; project.timeSigNumerator = savedNumerator;
    project.timeSigDenominator = savedDenominator;
    if (!ok) std::fprintf(stderr, "adaptive grid spacing, raster alignment or snap check failed\n");
    return ok;
}

namespace ui {
namespace {
class NavigationTimeline : public TimelineWidget {
public:
    using TimelineWidget::TimelineWidget;
    bool profileNavigation = false;
    bool profilePlayback = false;
    int playbackPaints = 0;
    int playbackFullPaints = 0;
    std::uint64_t playbackNarrowStaticPaints = 0;
    std::vector<double> playbackPositions;
    QRegion lastPaintRegion;
    QElapsedTimer playbackPaintTime;
protected:
    void paintEvent(QPaintEvent* event) override {
        perf::Scope timing(profileNavigation ? "timeline.navigation.paint.ms" : nullptr);
        perf::Scope playbackTiming(profilePlayback ? "timeline.playback.paint.ms" : nullptr);
        const auto staticBefore = staticFramePaintCountForTest();
        TimelineWidget::paintEvent(event);
        lastPaintRegion = event->region();
        if (profilePlayback) {
            if (event->region().boundingRect() == rect()) ++playbackFullPaints;
            else playbackNarrowStaticPaints += staticFramePaintCountForTest() - staticBefore;
            if (playbackPaintTime.isValid())
                perf::sample("timeline.playback.interval.ms", playbackPaintTime.nsecsElapsed() / 1e6);
            playbackPaintTime.restart();
            ++playbackPaints;
            playbackPositions.push_back(displayedPlayheadSubpixelXForTest());
        }
    }
};
}
bool checkUiScaling() {
    bool ok = true;
    const auto check = [&](bool result, const char* name) {
        std::printf("%s  %s\n", result ? "PASS" : "FAIL", name); ok = ok && result;
    };
    const auto settle = [] { QEventLoop loop; QTimer::singleShot(80, &loop, &QEventLoop::quit); loop.exec(); };
    daw::EngineController controller;
    controller.initialize(48000.0, 512, false);
    // This benchmark isolates UI cost: no real device or 500 DSP channels.
    auto& project = const_cast<daw::ProjectModel&>(controller.project());
    project.tracks.clear(); project.invalidateTrackIndex();
    for (int t = 0; t < 500; ++t) {
        daw::TrackModel track;
        track.id = "ui-perf-track-" + std::to_string(t);
        track.name = "Track " + std::to_string(t + 1);
        track.kind = daw::TrackKind::Midi;
        for (int c = 0; c < 40; ++c) {
            daw::ClipModel clip;
            clip.id = track.id + "-clip-" + std::to_string(c);
            clip.kind = daw::ClipKind::Midi;
            clip.notes.resize(5);
            clip.startSeconds = c * 2.0; clip.durationSeconds = 1.5;
            track.clips.push_back(std::move(clip));
        }
        project.tracks.push_back(std::move(track));
    }
    TrackListWidget tracks(&controller);
    MixerWidget mixer(&controller);
    NavigationTimeline timeline(&controller);
    tracks.resize(280, 640); mixer.resize(1250, 760); timeline.resize(1250, 640);
    tracks.rebuild(); tracks.show(); mixer.show(); timeline.show(); settle();
    check(timeline.checkAdaptiveGridForTest(),
          "adaptive overview uses sparse whole-bar groups, aligned snapping and correct meters");
    const QString first = QString::fromStdString(project.tracks.front().id);
    const QPointer<FaderWidget> fader = tracks.rowFaderForTest(first);
    check(fader, "visible track has a fader");
    check(tracks.findChildren<FaderWidget*>().size() < 64, "500 track headers create only viewport controls");
    check(mixer.findChildren<FaderWidget*>().size() < 64, "500 mixer channels create only viewport controls");
    tracks.setSelectedTracks({first, QString::fromStdString(project.tracks[1].id)}, first);
    QElapsedTimer time; time.start();
    for (int i = 0; i < 20; ++i) {
        project.tracks.front().name = "Rename " + std::to_string(i);
        tracks.rebuild(); mixer.rebuild();
    }
    std::printf("20 UI structure synchronizations: %.3f ms\n", time.nsecsElapsed() / 1e6);
    check(fader && tracks.rowFaderForTest(first) == fader, "unchanged row controls survive repeated sync");
    check(tracks.selectedTrackIds().size() == 2, "multi-selection survives structure sync");
    tracks.setVerticalScroll(72 * 492); settle();
    check(tracks.rowFaderForTest(QString::fromStdString(project.tracks.back().id)), "scroll materializes final rows");
    check(tracks.findChildren<FaderWidget*>().size() < 64, "scroll releases distant controls");
    tracks.setVerticalScroll(0); settle();
    check(tracks.rowFaderForTest(first), "scroll back restores rows");
    check(timeline.checkClipIndexForTest(), "interval queries cover visible clip bodies and preserve ID lookup");
    controller.setClipStartSeconds(project.tracks.front().id, project.tracks.front().clips.front().id, 5000.0);
    check(timeline.checkClipIndexForTest(), "live placement invalidates interval geometry before undo commit");
    const auto unknown = project.findTrack("missing-track");
    check(!unknown && !project.findTrack(""), "missing and master IDs do not resolve to a track");
    std::swap(project.tracks[0], project.tracks[1]); project.invalidateTrackIndex();
    tracks.rebuild(); mixer.rebuild(); timeline.update(); settle();
    check(tracks.rowRectForTrack(first).top() > tracks.rowRectForTrack(QString::fromStdString(project.tracks[0].id)).top(), "same-size reorder updates row positions");
    time.start();
    for (int i = 0; i < 200; ++i) project.findTrack("");
    std::printf("200 empty ID lookups: %.3f ms\n", time.nsecsElapsed() / 1e6);
    std::vector<daw::EngineController::ClipStartChange> move;
    for (int t = 0; t < 25; ++t) for (const auto& clip : project.tracks[t].clips)
        move.push_back({project.tracks[t].id, clip.id, clip.startSeconds});
    controller.beginClipPositionEdit();
    for (int frame = 0; frame < 40; ++frame) {
        for (auto& clip : move) clip.startSeconds += 0.125;
        perf::Scope timing("gesture.group1000.ms");
        controller.setClipStartsSeconds(move);
    }
    controller.endClipPositionEdit("Scaling gesture");
    check(timeline.checkClipIndexForTest(), "1000-clip gesture preserves interval correctness");
    const auto wheel = [](QWidget& surface, QPoint pixels, QPoint angle,
                          Qt::KeyboardModifiers modifiers = Qt::NoModifier,
                          Qt::ScrollPhase phase = Qt::ScrollUpdate) {
        const QPointF position(400.75, 100.25);
        QWheelEvent event(position, surface.mapToGlobal(position), pixels, angle,
                          Qt::NoButton, modifiers, phase, false);
        QApplication::sendEvent(&surface, &event);
    };
    const auto near = [](double a, double b) { return std::abs(a - b) < 1e-8; };
    timeline.setFollowPlayhead(false);
    timeline.setVerticalScroll(100);
    const double startScroll = timeline.horizontalScrollForTest();
    const double startScale = timeline.pixelsPerSecondForTest();
    wheel(timeline, {-3, -2}, {-120, -120});
    check(near(timeline.horizontalScrollForTest(), startScroll + 3.0 / startScale) &&
          timeline.verticalScroll() == 102, "pixel scrolling preserves both axes and overrides wheel angles");
    wheel(timeline, {-1, -1}, {}, Qt::NoModifier, Qt::ScrollMomentum);
    check(near(timeline.horizontalScrollForTest(), startScroll + 4.0 / startScale) &&
          timeline.verticalScroll() == 103, "one-pixel momentum is not lost");
    wheel(timeline, {}, {0, -1}); wheel(timeline, {}, {0, -1});
    check(timeline.verticalScroll() == 104, "fractional mouse-wheel distances accumulate");
    const double shiftStart = timeline.horizontalScrollForTest();
    wheel(timeline, {0, -3}, {}, Qt::ShiftModifier);
    wheel(timeline, {}, {0, -120}, Qt::ShiftModifier);
    check(near(timeline.horizontalScrollForTest(), shiftStart + 33.0 / startScale) &&
          timeline.verticalScroll() == 104, "Shift preserves pixel precision and legacy mouse-wheel distance");
    // Zoom focus is independent of the pointer and the old viewport position.
    const auto* horizontalBar = timeline.findChild<QScrollBar*>("TimelineHorizontalScroll");
    const auto centreTime = [&] {
        return timeline.horizontalScrollForTest() +
               horizontalBar->width() * 0.5 / timeline.pixelsPerSecondForTest();
    };
    controller.seekSeconds(20.125);
    const double anchor = controller.presentationPositionSeconds();
    wheel(timeline, {}, {0, 1}, Qt::ControlModifier);
    const double fineScale = timeline.pixelsPerSecondForTest();
    check(fineScale > startScale && fineScale < startScale * 1.01 &&
          near(centreTime(), anchor),
          "fine wheel zoom is proportional and centres the playhead");
    wheel(timeline, {}, {0, -1}, Qt::ControlModifier);
    check(near(timeline.pixelsPerSecondForTest(), startScale), "opposite fine zoom restores the scale");
    wheel(timeline, {}, {0, 120}, Qt::ControlModifier);
    check(near(timeline.pixelsPerSecondForTest(), startScale * 1.15), "one wheel notch retains 15 percent zoom");
    wheel(timeline, {}, {0, -120}, Qt::ControlModifier);
    const double zoomScroll = timeline.horizontalScrollForTest();
    wheel(timeline, {}, {}, Qt::ControlModifier, Qt::ScrollBegin);
    wheel(timeline, {}, {}, Qt::ControlModifier, Qt::ScrollEnd);
    check(near(timeline.pixelsPerSecondForTest(), startScale) &&
          near(timeline.horizontalScrollForTest(), zoomScroll), "zero-delta gesture boundaries never zoom or move the anchor");
    for (int i = 0; i < 100; ++i) {
        wheel(timeline, {0, 1}, {}, Qt::ControlModifier);
        wheel(timeline, {0, -1}, {}, Qt::ControlModifier);
    }
    check(near(timeline.horizontalScrollForTest(), zoomScroll) &&
          near(timeline.pixelsPerSecondForTest(), startScale) && near(centreTime(), anchor),
          "repeated pixel zoom keeps the playhead centred without drift");

    timeline.zoomBy(1.3);
    check(near(centreTime(), anchor), "toolbar and keyboard zoom use the same playhead focus");
    timeline.zoomBy(1.0 / 1.3);
    const auto pinch = [&](double value, const QPointF& position) {
        QNativeGestureEvent event(Qt::ZoomNativeGesture, QPointingDevice::primaryPointingDevice(), 2,
                                  position, position, timeline.mapToGlobal(position), value, {});
        QApplication::sendEvent(&timeline, &event);
    };
    pinch(0.1, {70.25, 100.5});
    check(near(centreTime(), anchor), "native pinch centres the playhead regardless of pointer position");
    pinch(1.0 / 1.1 - 1.0, {1100.75, 200.25});
    check(near(centreTime(), anchor) && near(timeline.pixelsPerSecondForTest(), startScale),
          "reversing pinch preserves the focus and restores scale");

    const auto& focusTrack = project.tracks[0];
    const auto& focusClip = focusTrack.clips.back();
    const ui::ClipSel focusRef{QString::fromStdString(focusTrack.id), QString::fromStdString(focusClip.id)};
    const double clipEnd = focusClip.startSeconds + controller.clipDisplayDuration(focusClip);
    const double clipCentre = (focusClip.startSeconds + clipEnd) * 0.5;
    timeline.selectClips({focusRef});
    wheel(timeline, {}, {0, 120}, Qt::ControlModifier);
    check(near(centreTime(), clipCentre) && !near(centreTime(), anchor),
          "an off-screen selected clip takes zoom priority over the playhead");
    wheel(timeline, {}, {0, -120}, Qt::ControlModifier);
    pinch(0.1, {42.25, 110.5});
    check(near(centreTime(), clipCentre), "native pinch uses the selected clip focus");
    pinch(1.0 / 1.1 - 1.0, {1130.5, 140.75});

    const auto& otherTrack = project.tracks[1];
    const auto& otherClip = otherTrack.clips[12];
    const ui::ClipSel otherRef{QString::fromStdString(otherTrack.id), QString::fromStdString(otherClip.id)};
    const double selectionCentre =
        (std::min(focusClip.startSeconds, otherClip.startSeconds) +
         std::max(clipEnd, otherClip.startSeconds + controller.clipDisplayDuration(otherClip))) * 0.5;
    timeline.selectClips({focusRef, otherRef});
    timeline.zoomBy(1.3);
    check(near(centreTime(), selectionCentre), "multiple selected clips zoom around their full time span");
    timeline.selectClips({otherRef, focusRef});
    timeline.zoomBy(1.0 / 1.3);
    check(near(centreTime(), selectionCentre), "selection order does not change the zoom focus");
    timeline.clearClipSelection();
    timeline.zoomBy(1.3);
    check(near(centreTime(), anchor), "clearing clip selection returns zoom focus to the playhead");
    timeline.zoomBy(1.0 / 1.3);
    controller.seekSeconds(0);
    timeline.zoomBy(1.1);
    check(near(timeline.horizontalScrollForTest(), 0), "zoom at project start respects the left boundary");
    controller.seekSeconds(anchor);
    timeline.zoomBy(1.0 / 1.1);

    int headerScroll = 0;
    const auto scrollConnection = QObject::connect(&tracks, &TrackListWidget::verticalScrollRequested,
        &tracks, [&](int delta) { headerScroll += delta; });
    wheel(tracks, {0, -2}, {0, -120});
    wheel(tracks, {}, {0, -1}); wheel(tracks, {}, {0, -1});
    check(headerScroll == 3, "track headers use the same pixel and fractional-wheel distances");
    QObject::disconnect(scrollConnection);

    const auto pan = [&](QEvent::Type type, const QPointF& position) {
        QMouseEvent event(type, position, timeline.mapToGlobal(position),
                          type == QEvent::MouseMove ? Qt::NoButton : Qt::MiddleButton,
                          type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MiddleButton,
                          Qt::NoModifier);
        QApplication::sendEvent(&timeline, &event);
    };
    const QPointF grab(400.75, 100.25);
    const double panStart = timeline.horizontalScrollForTest();
    const int panY = timeline.verticalScroll();
    pan(QEvent::MouseButtonPress, grab);
    pan(QEvent::MouseMove, grab - QPointF(0.25, 0.25));
    check(near(timeline.horizontalScrollForTest(), panStart + 0.25 / startScale),
          "middle-button pan preserves fractional pointer movement");
    for (int step = 2; step <= 4; ++step)
        pan(QEvent::MouseMove, grab - QPointF(step * 0.25, step * 0.25));
    check(near(timeline.horizontalScrollForTest(), panStart + 1.0 / startScale) &&
          timeline.verticalScroll() == panY + 1, "fractional pan accumulates without losing either axis");
    pan(QEvent::MouseMove, grab);
    pan(QEvent::MouseButtonRelease, grab);
    check(near(timeline.horizontalScrollForTest(), panStart) && timeline.verticalScroll() == panY,
          "reversing a pan restores the original viewport");
    timeline.setVerticalScroll(0);
    pan(QEvent::MouseButtonPress, grab);
    pan(QEvent::MouseMove, grab + QPointF(0, 0.75));
    pan(QEvent::MouseMove, grab - QPointF(0, 0.25));
    pan(QEvent::MouseButtonRelease, grab - QPointF(0, 0.25));
    check(timeline.verticalScroll() == 1, "panning away from a bound has no overscroll dead zone");

    // Actual Qt paint delivery at a speed far below one logical pixel/frame.
    // Feed deterministic transport time without opening an audio device; this
    // isolates rendering from DSP and the audio block interpolation tests.
    FrameClock::instance().setPreference(FrameMode::Fixed, 60);
    controller.seekSeconds(200.0);
    timeline.clearClipSelection();
    timeline.zoomBy(4.0 / timeline.pixelsPerSecondForTest());
    timeline.setVerticalScroll(0);
    controller.play();
    timeline.refreshPlaybackFrame(); settle();
    const auto playbackStaticBefore = timeline.staticFramePaintCountForTest();
    FrameTimer playbackClock(&timeline);
    QElapsedTimer playbackTime; playbackTime.start();
    int playbackCallbacks = 0;
    QObject::connect(&playbackClock, &FrameTimer::timeout, &timeline, [&] {
        ++playbackCallbacks;
        controller.seekSeconds(200.0 + playbackTime.nsecsElapsed() / 1e9);
        timeline.refreshPlaybackFrame();
    });
    QEventLoop playback;
    QTimer::singleShot(1200, &playback, &QEventLoop::quit);
    timeline.profilePlayback = true;
    playbackClock.start(); playback.exec(); playbackClock.stop(); settle();
    timeline.profilePlayback = false;
    bool subpixel = false, monotonic = true;
    int distinctPixels = 0;
    double previous = -1.0;
    for (double x : timeline.playbackPositions) {
        subpixel |= std::abs(x - std::round(x)) > 0.01;
        if (previous >= 0.0) monotonic &= x >= previous;
        if (previous < 0.0 || int(x) != int(previous)) ++distinctPixels;
        previous = x;
    }
    std::printf("Slow playback (4 px/s): %d callbacks, %d paints, %d distinct whole pixels in %.3f s\n",
                playbackCallbacks, timeline.playbackPaints, distinctPixels, playbackTime.nsecsElapsed() / 1e9);
    check(subpixel && monotonic, "playback preserves fractional positions without stepping backwards");
    check(playbackCallbacks > 0 && timeline.playbackPaints >= playbackCallbacks * 0.85 &&
          timeline.playbackPaints > distinctPixels * 3,
          "zoomed-out playback paints intermediate frames within the same logical pixel");
    // Cocoa may expose the whole window while this real-window benchmark runs.
    // Such exposure may rebuild the cache; a cursor strip must never do so.
    const auto playbackStaticPaints = timeline.staticFramePaintCountForTest() - playbackStaticBefore;
    std::printf("Playback static paints: %llu (%d full window exposures)\n",
                static_cast<unsigned long long>(playbackStaticPaints), timeline.playbackFullPaints);
    check(timeline.playbackNarrowStaticPaints == 0 && timeline.playbackFullPaints <= 2 &&
          playbackStaticPaints <= std::uint64_t(timeline.playbackFullPaints),
          "cursor-only frames reuse the static timeline without whole-window repainting");

    timeline.zoomBy(80.0 / timeline.pixelsPerSecondForTest());
    timeline.refreshPlaybackFrame(); settle();
    const double steadyTrail = timeline.displayedPlayheadTrailForTest();
    wheel(timeline, {-200, 0}, {});
    timeline.refreshPlaybackFrame(); settle();
    const double forwardPanTrail = timeline.displayedPlayheadTrailForTest();
    wheel(timeline, {400, 0}, {});
    timeline.refreshPlaybackFrame(); settle();
    check(steadyTrail > 0 && near(steadyTrail, forwardPanTrail) &&
          near(steadyTrail, timeline.displayedPlayheadTrailForTest()),
          "panning either way leaves the playback trail length unchanged");
    const double headBeforeSeek = timeline.displayedPlayheadSubpixelXForTest();
    controller.seekSeconds(controller.positionSeconds() - 0.5);
    timeline.refreshPlaybackFrame(); settle();
    check(near(timeline.displayedPlayheadSubpixelXForTest(), headBeforeSeek - 40.0) &&
          near(steadyTrail, timeline.displayedPlayheadTrailForTest()),
          "a backward seek jumps directly without stretching or reversing the trail");
    // Compare rasterized playback/paused images at one exact transport time.
    // The only difference must be behind (left of) the cursor.
    const QImage rollingImage = timeline.grab().toImage();
    const double trailHeadX = timeline.displayedPlayheadSubpixelXForTest();
    controller.pause(); timeline.refreshPlaybackFrame(); settle();
    const QImage parkedImage = timeline.grab().toImage();
    int leftDifference = 0, rightDifference = 0;
    const double dpr = rollingImage.devicePixelRatio();
    for (int y = int(100 * dpr); y < int(130 * dpr); ++y) {
        for (int x = int((trailHeadX - 55) * dpr); x < int((trailHeadX + 55) * dpr); ++x) {
            if (x < 0 || x >= rollingImage.width()) continue;
            if (rollingImage.pixel(x, y) == parkedImage.pixel(x, y)) continue;
            if (x < int(trailHeadX * dpr)) ++leftDifference;
            else ++rightDifference;
        }
    }
    check(leftDifference > 0 && rightDifference == 0,
          "after reverse pan the rendered trail extends only left from the cursor");
    check(near(timeline.displayedPlayheadTrailForTest(), 0.0), "pause removes the trail immediately");

    controller.play();
    timeline.zoomBy(1200.0 / timeline.pixelsPerSecondForTest());
    timeline.refreshPlaybackFrame(); settle();
    const double wideHeadX = timeline.displayedPlayheadSubpixelXForTest();
    const double wideTrail = timeline.displayedPlayheadTrailForTest();
    const auto stopStaticBefore = timeline.staticFramePaintCountForTest();
    controller.stop(); timeline.refreshPlaybackFrame(); settle();
    check(near(wideTrail, 52.0) && timeline.lastPaintRegion.contains(
              QPoint(int(wideHeadX - wideTrail + 1), 100)) &&
          timeline.staticFramePaintCountForTest() == stopStaticBefore,
          "stopping erases the whole previous trail with a cached narrow repaint");
    controller.seekSeconds(20.0);
    timeline.zoomBy(80.0 / timeline.pixelsPerSecondForTest());
    timeline.refreshPlaybackFrame(); settle();

    // Sustained navigation through a populated viewport (no delay per input).
    // Timers/painting still run through the native event loop and frame clock.
    FrameClock::instance().setPreference(FrameMode::Fixed, 60);
    timeline.setVerticalScroll(0); settle();
    QTimer gesture;
    gesture.setTimerType(Qt::PreciseTimer);
    gesture.setInterval(8);
    int gestureSteps = 0;
    QObject::connect(&gesture, &QTimer::timeout, &timeline, [&] {
        const int direction = (gestureSteps++ / 40) % 2 ? 1 : -1;
        wheel(timeline, {direction * 2, direction}, {});
        wheel(timeline, {0, direction}, {}, Qt::ControlModifier);
    });
    const auto paintsBefore = timeline.staticFramePaintCountForTest();
    FrameTimer navigationClock(&timeline);
    int navigationFrames = 0;
    QObject::connect(&navigationClock, &FrameTimer::timeout, &timeline,
                     [&] { ++navigationFrames; });
    QEventLoop navigation;
    QTimer::singleShot(1200, &navigation, &QEventLoop::quit);
    timeline.profileNavigation = true;
    navigationClock.start();
    gesture.start(); navigation.exec(); gesture.stop(); settle();
    navigationClock.stop();
    timeline.profileNavigation = false;
    const auto navigationPaints = timeline.staticFramePaintCountForTest() - paintsBefore;
    std::printf("Continuous navigation: %d inputs, %d frames, %llu static paints (%dx%d, DPR %.1f, %.2f Hz)\n",
                gestureSteps, navigationFrames, static_cast<unsigned long long>(navigationPaints),
                timeline.width(), timeline.height(), timeline.devicePixelRatioF(), timeline.screen()->refreshRate());
    check(gestureSteps > 0 && timeline.staticFramePaintCountForTest() > paintsBefore,
          "continuous scroll and zoom paint while input is active");
    check(navigationFrames > 0 && navigationPaints <= std::uint64_t(navigationFrames * 2 + 4),
          "scrollbars do not repaint the canvas at input frequency");
    for (int fps : {60, 120, 144, 240}) {
        FrameClock::instance().setPreference(FrameMode::Fixed, fps);
        for (int step = 0; step < 4; ++step) {
            timeline.setVerticalScroll((step * 123) * 72);
            timeline.zoomBy(step % 2 ? 0.8 : 1.25);
            timeline.update(); settle();
        }
    }
    FrameClock::instance().setPreference(FrameMode::Unlimited, 60);
    timeline.update(); settle();
    FrameClock::instance().setPreference(FrameMode::Fixed, 60);
    perf::flush();
    return ok;
}
} // namespace ui
