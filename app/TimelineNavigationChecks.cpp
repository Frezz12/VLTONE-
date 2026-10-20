#include "TimelineWidget.hpp"
#include "TimelineNavigation.hpp"
#include "ScrollMotion.hpp"
#include "TrackListWidget.hpp"
#include "EngineController.hpp"
#include "Theme.hpp"
#include "UiConstants.hpp"
#include "graphics/WorkspaceSurface.hpp"
#include "graphics/GraphicsPreferences.hpp"
#include <QApplication>
#include <QEventLoop>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QTimer>
#include <QWheelEvent>
#include <cmath>
#include <cstdio>
#include <memory>

bool TimelineWidget::checkNavigationRailsForTest() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    ThemeManager::instance().setThemeId(qEnvironmentVariable("VLT_RAILS_THEME", "dark"), false);
    ThemeManager::instance().apply();
    auto& preferences = ui::ScrollPreferences::instance();
    const bool reduced = preferences.reducedMotion(), smoothing = preferences.enabled();
    const int strength = preferences.strength();
    const auto restore = qScopeGuard([&] {
        preferences.setReducedMotion(reduced); preferences.setEnabled(smoothing); preferences.setStrength(strength);
    });
    preferences.setReducedMotion(true);
    bool ok = true;
    const auto check = [&](bool result, const char* label) {
        std::printf("%s %s\n", result ? "PASS" : "FAIL", label); ok &= result;
    };
    const auto settle = [](int ms = 70) {
        QEventLoop loop; QTimer::singleShot(ms, &loop, &QEventLoop::quit); loop.exec();
    };
    const auto mouse = [](QWidget* widget, QEvent::Type type, QPointF point) {
        const auto button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
        const auto buttons = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
        QMouseEvent event(type, point, widget->mapToGlobal(point), button, buttons, Qt::NoModifier);
        QApplication::sendEvent(widget, &event);
    };
    daw::EngineController controller;
    if (!controller.initialize(48000, 256, false)) return false;
    auto& project = const_cast<daw::ProjectModel&>(controller.project());
    project.tracks.clear(); project.invalidateStructure();
    const uint32_t colors[] = {0xB67E5D, 0x6A9EB8, 0x89A27B, 0xA28FC2, 0xBE829C};
    for (int i = 0; i < 34; ++i) {
        daw::TrackModel track;
        track.id = "nav-" + std::to_string(i);
        track.name = i == 0 ? "Drums" : i == 4 ? "Bass" : i == 5 ? "Lead vocal" : "Layer " + std::to_string(i);
        track.color = colors[i % 5]; track.height = i % 4 == 0 ? 96 : 64;
        track.kind = i == 0 ? daw::TrackKind::Folder : daw::TrackKind::Instrument;
        track.expanded = false;
        if (i > 0 && i < 4) track.parentId = "nav-0";
        for (int j = 0; j < 6 && i > 0; ++j) {
            daw::ClipModel clip;
            clip.id = track.id + "-" + std::to_string(j); clip.name = track.name;
            clip.kind = daw::ClipKind::Midi; clip.color = track.color;
            clip.startSeconds = j * 12 + (i % 3) * 2; clip.durationSeconds = 8;
            track.clips.push_back(std::move(clip));
        }
        project.tracks.push_back(std::move(track));
    }
    project.invalidateStructure();
    QWidget host; auto* layout = new QHBoxLayout(&host);
    layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0);
    TrackListWidget headers(&controller, &host);
    TimelineWidget timeline(&controller, &host);
    layout->addWidget(&headers); layout->addWidget(&timeline, 1);
    headers.rebuild();
    QObject::connect(&timeline, &TimelineWidget::verticalScrollChanged, &headers, &TrackListWidget::setVerticalScroll);
    timeline.m_pixelsPerSecond = 28;
    host.resize(1100, 620);
    std::unique_ptr<ui::graphics::WorkspaceSurface> surface;
    int presentedFrames = 0;
    if (ui::graphics::gpuWorkspaceEnabled()) {
        surface = std::make_unique<ui::graphics::WorkspaceSurface>(&host);
        QObject::connect(surface.get(), &ui::graphics::WorkspaceSurface::failed, &host,
                         [&](const QString& reason) { check(false, qPrintable(reason)); });
        QObject::connect(surface.get(), &ui::graphics::WorkspaceSurface::frameMeasured, &host,
                         [&] { ++presentedFrames; });
    }
    host.show(); host.activateWindow();
    if (surface) surface->quickWindow()->requestActivate();
    settle(150);
    if (surface && (presentedFrames == 0 || !surface->quickWindow()->isExposed())) {
        check(false, "GPU surface is not presenting; input and screenshot checks are invalid");
        host.close(); controller.shutdown();
        return false;
    }
    auto* h = timeline.m_horizontalScrollBar;
    auto* v = timeline.m_verticalScrollBar;
    check(h->cursor().shape() == Qt::SizeHorCursor && v->cursor().shape() == Qt::SizeVerCursor,
          "hover cursors show the movement axis");
    const auto screenshot = [&](const QString& suffix) {
        const QString prefix = qEnvironmentVariable("VLT_RAILS_SCREENSHOT");
        if (prefix.isEmpty()) return;
        settle();
        const auto image = surface ? surface->quickWindow()->grabWindow() : host.grab().toImage();
        check(image.save(prefix + suffix + ".png"), "save navigation screenshot");
    };
    int edits = 0;
    QObject::connect(&timeline, &TimelineWidget::projectEdited, &host, [&] { ++edits; });
    const auto undo = controller.undoDepth();
    const double playhead = controller.presentationPositionSeconds();
    timeline.setSelectedTrack("nav-5");
    timeline.m_selection = {{"nav-5", "nav-5-0"}};
    const auto selection = timeline.m_selection;
    check(h->height() == 18 && v->width() == 18 && h->geometry().right() + 1 == v->x(),
          "18 px rails retain their shared bottom-right joint");
    check(timeline.visibleRows().size() == 31, "navigation uses the collapsed folder layout");
    timeline.setHorizontalScroll(12); timeline.setVerticalScroll(160); settle();
    const double start = timeline.m_scrollSeconds;
    mouse(h, QEvent::MouseButtonPress, {60, 9});
    check(timeline.m_scrollSeconds == start, "pickup at an arbitrary point does not jump");
    const auto ticksBefore = h->grab().toImage();
    mouse(h, QEvent::MouseMove, {62, 9});
    const auto ticksRight = h->grab().toImage();
    const auto tickError = [](const QImage& before, const QImage& after, int direction) {
        const int shift = qRound(2 * before.devicePixelRatio()) * direction;
        const int margin = qRound(40 * before.devicePixelRatio());
        int error = 0;
        for (int x = margin; x < before.width() - margin; ++x)
            error += std::abs(qGray(after.pixel(x, after.height() / 2)) -
                              qGray(before.pixel(x - shift, before.height() / 2)));
        return error;
    };
    check(tickError(ticksBefore, ticksRight, 1) < tickError(ticksBefore, ticksRight, -1),
          "horizontal tick marks follow a rightward drag");
    const double afterRight = timeline.m_scrollSeconds;
    mouse(h, QEvent::MouseMove, {60, 9});
    const auto ticksLeft = h->grab().toImage();
    check(timeline.m_scrollSeconds < afterRight &&
          tickError(ticksRight, ticksLeft, -1) < tickError(ticksRight, ticksLeft, 1),
          "leftward drag moves both the viewport and roller ticks left immediately");
    mouse(h, QEvent::MouseMove, {160, 9});
    const double expected = start + double(h->pageStep()) / 1000 * 100 / h->width();
    check(std::abs(timeline.m_scrollSeconds - expected) <= .001, "rightward drag moves to later time by one viewport per rail length");
    mouse(h, QEvent::MouseButtonRelease, {170, 9});
    const double stopped = timeline.m_scrollSeconds;
    settle(100);
    check(timeline.m_scrollSeconds == stopped && !h->isSliderDown(), "reduced motion consumes final delta and stops immediately");
    screenshot("-timeline");

    h->setValue(0); settle();
    mouse(h, QEvent::MouseButtonPress, {60, 9});
    mouse(h, QEvent::MouseMove, {-500, 9});
    mouse(h, QEvent::MouseMove, {-490, 9});
    check(h->value() > 0, "reversal at the start has no overscroll dead zone");
    mouse(h, QEvent::MouseButtonRelease, {-490, 9});
    h->setValue(h->maximum()); settle();
    mouse(h, QEvent::MouseButtonPress, {60, 9});
    mouse(h, QEvent::MouseMove, {600, 9});
    const int end = h->value();
    mouse(h, QEvent::MouseMove, {590, 9});
    check(h->value() < end, "reversal at the end is immediate");
    mouse(h, QEvent::MouseButtonRelease, {590, 9});

    timeline.setHorizontalScroll(12); settle();
    const double denseStart = timeline.m_scrollSeconds;
    const double perPixel = double(h->pageStep()) / 1000 / h->width();
    mouse(h, QEvent::MouseButtonPress, {60, 9});
    bool exact = true;
    for (int sample = 1; sample <= 40; ++sample) {
        const double distance = sample * .25;
        mouse(h, QEvent::MouseMove, {60 + distance, 9});
        const auto value = h->value();
        mouse(h, QEvent::MouseMove, {60 + distance, 9});
        exact &= value == h->value() &&
                 std::abs(timeline.m_scrollSeconds - denseStart - distance * perPixel) <= .001;
    }
    QMouseEvent lostRelease(QEvent::MouseMove, QPointF(90, 9), h->mapToGlobal(QPointF(90, 9)),
                            Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(h, &lostRelease);
    mouse(h, QEvent::MouseButtonRelease, {100, 9});
    check(exact && !h->isSliderDown() &&
          std::abs(timeline.m_scrollSeconds - denseStart - 10 * perPixel) <= .001,
          "fractional and repeated input stays precise; a lost release stops the gesture");

    timeline.setHorizontalScroll(12); settle();
    const int verticalBefore = timeline.m_scrollY;
    const double timeBeforeVertical = timeline.m_scrollSeconds;
    mouse(v, QEvent::MouseButtonPress, {9, 90});
    mouse(v, QEvent::MouseMove, {9, 120});
    mouse(v, QEvent::MouseButtonRelease, {9, 120});
    check(timeline.m_scrollY > verticalBefore && timeline.m_scrollSeconds == timeBeforeVertical,
          "vertical roller changes only track position");
    mouse(h, QEvent::MouseButtonPress, {80, 9});
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(h, &escape);
    mouse(h, QEvent::MouseMove, {120, 9});
    check(!h->isSliderDown() && timeline.m_scrollSeconds == timeBeforeVertical,
          "Escape ends a captured gesture immediately");
    QKeyEvent home(QEvent::KeyPress, Qt::Key_Home, Qt::NoModifier);
    QApplication::sendEvent(h, &home);
    check(h->value() == 0, "native scrollbar keyboard navigation remains available");

    host.resize(740, 450); timeline.setBottomInset(120); settle();
    mouse(h, QEvent::MouseButtonPress, {h->width() - 2., 9});
    mouse(h, QEvent::MouseButtonRelease, {h->width() - 2., 9});
    check(h->geometry().bottom() < timeline.height() - 120 && v->geometry().bottom() < h->y(),
          "compact layout keeps both rollers above the mixer");
    screenshot("-compact");
    mouse(v, QEvent::MouseButtonPress, {9, 40});
    const int beforeDeactivate = timeline.m_scrollY;
    QEvent deactivate(QEvent::WindowDeactivate); QApplication::sendEvent(v, &deactivate);
    mouse(v, QEvent::MouseMove, {9, 80});
    check(!v->isSliderDown() && timeline.m_scrollY == beforeDeactivate,
          "window deactivation ends dragging without residual movement");
    check(controller.undoDepth() == undo && edits == 0 && timeline.m_selection.size() == selection.size() &&
          timeline.m_selection.front().trackId == selection.front().trackId &&
          timeline.m_selection.front().clipId == selection.front().clipId &&
          controller.presentationPositionSeconds() == playhead, "navigation preserves edits, selection, Undo and playback");

    project.tracks.back().clips.back().startSeconds = 36000;
    project.invalidateStructure(); timeline.update(); timeline.syncNavigationControls(); settle();
    timeline.setHorizontalScroll(12); settle();
    const double longStart = timeline.m_scrollSeconds, page = h->pageStep() / 1000.;
    mouse(h, QEvent::MouseButtonPress, {50, 9});
    mouse(h, QEvent::MouseButtonRelease, {150, 9});
    check(std::abs(timeline.m_scrollSeconds - longStart - page * 100 / h->width()) < .001,
          "long projects retain the same precise roller sensitivity");

    preferences.setEnabled(true); preferences.setStrength(65); preferences.setReducedMotion(false);
    const auto throwRail = [&](QScrollBar* bar) {
        timeline.setHorizontalScroll(12); timeline.setVerticalScroll(160); settle();
        const bool horizontal = bar == h;
        const auto point = [horizontal](double travel) {
            return horizontal ? QPointF(150 + travel, 9) : QPointF(9, 40 + travel);
        };
        mouse(bar, QEvent::MouseButtonPress, point(0));
        settle(16); mouse(bar, QEvent::MouseMove, point(16));
        settle(16); mouse(bar, QEvent::MouseMove, point(32));
        const int atRelease = bar->value();
        mouse(bar, QEvent::MouseButtonRelease, point(32));
        // Normal native mouse capture ends after release. It must not cancel
        // the intended tail; an interrupted active drag is tested separately.
        QEvent ungrab(QEvent::UngrabMouse); QApplication::sendEvent(bar, &ungrab);
        check(bar->value() == atRelease, "release starts easing from the displayed value without a jump");
        return atRelease;
    };
    for (auto* bar : {h, v}) {
        const int released = throwRail(bar);
        settle(60);
        const int intermediate = bar->value();
        settle(140);
        const int endpoint = bar->value();
        settle(90);
        const double maxTail = bar->pageStep() * 12. / (bar == h ? bar->width() : bar->height());
        check(intermediate > released && endpoint >= intermediate &&
              endpoint - released <= std::ceil(maxTail) && bar->value() == endpoint,
              "both axes coast briefly in the release direction, then settle within 12 pixels");
    }
    throwRail(h); settle(35);
    const int caught = h->value();
    mouse(h, QEvent::MouseButtonPress, {120, 9});
    settle(60);
    check(h->value() == caught, "grabbing a moving roller cancels its tail without a jump");
    mouse(h, QEvent::MouseMove, {115, 9});
    check(h->value() < caught, "a new drag reverses a caught roller immediately");
    preferences.setReducedMotion(true);
    mouse(h, QEvent::MouseButtonRelease, {115, 9});
    preferences.setReducedMotion(false);
    throwRail(h);
    const QPointF wheelPoint(100, 9);
    QWheelEvent wheel(wheelPoint, h->mapToGlobal(wheelPoint), {}, {0, 120}, Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(h, &wheel);
    const int afterWheel = h->value(); settle(200);
    check(h->value() == afterWheel, "wheel input cancels residual roller movement");
    throwRail(h);
    preferences.setReducedMotion(true);
    const int afterReduced = h->value(); settle(200);
    check(h->value() == afterReduced, "enabling reduced motion cancels an active tail");
    preferences.setReducedMotion(false);
    timeline.setHorizontalScroll(12); settle();
    mouse(h, QEvent::MouseButtonPress, {120, 9});
    settle(16); mouse(h, QEvent::MouseMove, {100, 9}); settle(110);
    mouse(h, QEvent::MouseButtonRelease, {100, 9});
    const int pausedRelease = h->value(); settle(200);
    check(h->value() == pausedRelease, "holding still before release prevents an unwanted throw");

    // Inspect the rendered marker, including fractional device-pixel scales.
    host.resize(1100, 620); timeline.setBottomInset(0);
    timeline.setHorizontalScroll(12); settle();
    const auto markerX = [&](QScrollBar* bar) {
        const QImage image = bar->grab().toImage();
        double total = 0; int count = 0;
        for (int x = 0; x < image.width(); ++x) {
            const auto color = image.pixelColor(x, image.height() / 2);
            const int hue = std::abs(color.hsvHue() - th().accent.hsvHue());
            if (color.hsvSaturation() > 40 && std::min(hue, 360 - hue) < 12) {
                total += x + .5; ++count;
            }
        }
        return count ? total / count / image.devicePixelRatio() : -1.;
    };
    controller.seekSeconds(16); timeline.refreshPlaybackFrame(); settle();
    check(std::abs(markerX(h) - 4 * timeline.m_pixelsPerSecond) < 1.5 && markerX(v) == -1,
          "only the horizontal rail has an accent marker, aligned with the playhead");
    controller.seekSeconds(18); emit timeline.playheadMoved(); settle();
    check(std::abs(markerX(h) - 6 * timeline.m_pixelsPerSecond) < 1.5,
          "the marker follows seeking while transport is stopped");
    timeline.setHorizontalScroll(14); settle();
    check(std::abs(markerX(h) - 4 * timeline.m_pixelsPerSecond) < 1.5,
          "panning keeps the marker aligned with timeline content");
    timeline.setHorizontalZoom(40); settle();
    check(std::abs(markerX(h) - (18 - timeline.m_scrollSeconds) * timeline.m_pixelsPerSecond) < 1.5,
          "zoom keeps the marker aligned with the playhead");
    screenshot("-playhead");
    timeline.setHorizontalScroll(30); settle();
    check(markerX(h) == -1, "offscreen playhead does not leave a false centre marker");
    project.tracks.clear(); project.invalidateStructure(); timeline.update();
    timeline.setVerticalScroll(0); timeline.setHorizontalScroll(0); timeline.syncNavigationControls();
    mouse(v, QEvent::MouseButtonPress, {9, 30}); mouse(v, QEvent::MouseButtonRelease, {9, 50});
    check(timeline.m_scrollY == 0, "empty project cannot scroll beyond its track bounds");
    screenshot("-empty");
    host.close(); controller.shutdown();
    return ok;
}
