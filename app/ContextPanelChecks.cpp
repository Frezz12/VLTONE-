#include "ContextPanel.hpp"
#include "Controls.hpp"
#include "EngineController.hpp"
#include "PluginQuickAdder.hpp"
#include "SelectionModel.hpp"
#include "Theme.hpp"
#include "ToolPanel.hpp"
#include "UiConstants.hpp"

#include <QAbstractButton>
#include <QApplication>
#include <QEventLoop>
#include <QPropertyAnimation>
#include <QSettings>
#include <QSlider>
#include <QTimer>

#include <cstdio>
#include <cmath>

bool ContextPanel::checkAdaptiveLayoutForTest() {
    // The check owns an isolated headless preference store. Start from the
    // moving mode so prior user settings cannot make geometry assertions vary.
    QSettings().setValue(QStringLiteral("contextPanel/followSelection"), true);
    ui::ClipSel audio;
    ui::ClipSel secondAudio;
    ui::ClipSel midi;
    for (const auto& track : m_controller->project().tracks) {
        for (const auto& clip : track.clips) {
            if (clip.kind == daw::ClipKind::Audio) {
                if (audio.clipId.isEmpty())
                    audio = {QString::fromStdString(track.id), QString::fromStdString(clip.id)};
                else if (secondAudio.clipId.isEmpty())
                    secondAudio = {QString::fromStdString(track.id), QString::fromStdString(clip.id)};
            }
            if (midi.clipId.isEmpty() && clip.kind == daw::ClipKind::Midi)
                midi = {QString::fromStdString(track.id), QString::fromStdString(clip.id)};
        }
    }
    if (audio.clipId.isEmpty()) return false;
    ToolPanel strip;
    strip.setBrowserVisible(false);
    strip.setInspectorZoneWidth(30);
    strip.resize(1000, ui::kToolPanelHeight);
    QWidget navigationHost;
    auto* viewControls = strip.takeTimelineViewControls();
    if (!viewControls) return false;
    viewControls->setParent(&navigationHost);
    navigationHost.resize(viewControls->size());
    viewControls->show();
    navigationHost.show();
    ui::SelectionModel selection;
    ContextPanel panel(m_controller, &selection, &strip);
    strip.show();
    QApplication::processEvents();
    int right = strip.contextRightEdge();
    int left = right - 620;
    int anchor = left + 200;
    panel.setBoundsProvider([&](int& from, int& to) { from = left; to = right; return true; });
    panel.setAnchorProvider([&](int& centre) { centre = anchor; return true; });
    const int home = strip.width() / 2 + 64;
    panel.setHomeAnchorProvider(
        [&](int& centre) { centre = home; return true; });
    QPointer<QWidget> retiredRow = panel.m_content;
    selection.setClips({audio});
    panel.relayout();
    const auto check = [](bool passed, const char* detail) {
        std::fprintf(stderr, "%s Context layout: %s\n", passed ? "PASS" : "FAIL", detail);
        return passed;
    };
    if (!check(!retiredRow || !retiredRow->isVisible(),
               "interrupted transition hides the retired row before deferred deletion")) return false;
    const auto visibleActions = [&] {
        int count = 0;
        for (auto* widget : panel.findChildren<QWidget*>())
            if (widget->property("contextPriority").isValid() && widget->isVisible()) ++count;
        return count;
    };
    const auto fits = [&] {
        if (panel.x() < left || panel.x() + panel.width() > right) {
            std::fprintf(stderr, "Context bounds: panel=%d..%d allowed=%d..%d\n",
                         panel.x(), panel.x() + panel.width(), left, right);
            return false;
        }
        for (auto* widget : panel.findChildren<QWidget*>()) {
            if (!widget->property("contextPriority").isValid() || !widget->isVisible()) continue;
            const int x = widget->mapTo(&panel, QPoint()).x();
            if (x < 0 || x + widget->width() > panel.width()) {
                std::fprintf(stderr, "Context action overflow: %s/%s x=%d width=%d panel=%d currentRow=%d\n",
                             widget->metaObject()->className(), qPrintable(widget->objectName()),
                             x, widget->width(), panel.width(),
                             int(panel.m_content && panel.m_content->isAncestorOf(widget)));
                return false;
            }
        }
        return true;
    };
    auto* wave = viewControls->findChild<QAbstractButton*>("WaveformScaleButton");
    auto* trackHeight = viewControls->findChild<QSlider*>("TimelineTrackHeightSlider");
    auto* timelineZoom = viewControls->findChild<QSlider*>("TimelineZoomSlider");
    const int fullCount = visibleActions();
    const bool utilitiesOrdered =
        wave && trackHeight && timelineZoom &&
        wave->isVisible() && timelineZoom->isVisible() && trackHeight->isVisible() &&
        wave->y() < timelineZoom->y() && timelineZoom->y() < trackHeight->y() &&
        !strip.findChild<QWidget*>("TimelineViewControls");
    if (!check(fits() && fullCount >= 6 && utilitiesOrdered,
               "waveform, zoom and track height live outside the context strip")) return false;
    QPointer<QWidget> primary;
    QPointer<QWidget> colour;
    for (auto* widget : panel.findChildren<QWidget*>()) {
        if (widget->property("contextPriority").toInt() == 110) primary = widget;
        if (widget->property("contextPriority").toInt() == 10) colour = widget;
    }
    for (const int width : {260, 210, 160, 120, 80, 50}) {
        left = right - width;
        anchor = right;
        panel.relayout();
        QApplication::processEvents();
        if (!check(fits(), "narrow bounds contain every surviving action")) return false;
        if (width == 120 && !check(primary && primary->isVisible() && colour && colour->isHidden() &&
                                  visibleActions() < fullCount,
                                  "gain survives after secondary controls disappear")) return false;
    }
    if (!check(wave->isVisible() && strip.contextRightEdge() == right &&
                   viewControls->isVisible() &&
                   panel.x() + panel.width() <= strip.contextRightEdge(),
               "context keeps its boundary without hiding navigation controls")) return false;
    for (int i = 0; i < 8; ++i) {
        panel.relayout();
        QApplication::processEvents();
        if (!check(wave->isVisible(), "context relayout leaves waveform control visible")) return false;
    }
    left = right;
    panel.relayout();
    if (!check(fits() && panel.width() == 0, "exhausted strip never falls back across neighbours")) return false;
    left = right - 620;
    anchor = left + 200;
    panel.relayout();
    if (!check(fits() && visibleActions() == fullCount && primary && colour->isVisible() &&
               wave->isVisible(), "widening restores the same controls and waveform button")) return false;

    const auto settle = [] {
        QEventLoop loop;
        QTimer::singleShot(380, &loop, &QEventLoop::quit);
        loop.exec();
    };
    const auto centredOnReadout = [&] {
        return panel.x() >= 12 && panel.x() + panel.width() <= strip.width() - 12 &&
               std::abs(2 * panel.x() + panel.width() - 2 * home) <= 1;
    };
    // Keep returning the previous clip's position, as a timeline with its own
    // selection can do. The current panel context must outrank that stale span.
    anchor = left;
    panel.relayout();
    if (!check(panel.x() == left, "selected clip follows its left-edge anchor")) return false;
    if (!check(!secondAudio.clipId.isEmpty(), "same-kind motion fixture has two audio clips")) return false;
    const QRect initial = panel.geometry();
    anchor = right;
    selection.setClips({secondAudio});
    if (!check(panel.geometry().topLeft() == initial.topLeft() && panel.m_drift &&
                   panel.m_drift->state() == QAbstractAnimation::Running &&
                   panel.m_content->isVisible() && !panel.m_contentSliding && !panel.m_outgoing,
               "same-kind clip selection moves the plate without delaying its controls")) return false;
    const QRect destination = panel.targetGeometry();
    panel.m_drift->setCurrentTime(panel.m_drift->duration() / 2);
    const QRect halfway = panel.geometry();
    if (!check(halfway.x() > initial.x() && halfway.x() < destination.x(),
               "same-kind panel movement passes through intermediate positions")) return false;
    anchor = left;
    selection.setClips({audio});
    if (!check(panel.geometry() == halfway && panel.m_drift &&
                   panel.m_drift->startValue().toRect() == halfway,
               "rapid reselection continues from the current position")) return false;
    panel.m_drift->setCurrentTime(panel.m_drift->duration());
    if (!check(panel.geometry() == panel.targetGeometry(),
               "interrupted movement settles above the latest selected clip")) return false;
    anchor = right;
    panel.followSelection();
    selection.setTracks({audio.trackId});
    if (!check(centredOnReadout() && panel.m_content &&
                   panel.m_content->isVisible() && !panel.m_contentSliding &&
                   !panel.m_outgoing,
               "track tools are ready during selection without a delayed slide")) return false;
    settle();
    if (!check(centredOnReadout(), "clip-to-track selection returns under the transport readout")) return false;
    const QRect trackGeometry = panel.geometry();
    anchor = left;
    panel.followSelection();
    settle();
    if (!check(panel.geometry() == trackGeometry, "timeline movement cannot move the track context")) return false;
    for (const auto& track : m_controller->project().tracks) {
        const QString id = QString::fromStdString(track.id);
        if (id == audio.trackId) continue;
        selection.setTracks({audio.trackId, id});
        settle();
        if (!check(centredOnReadout(), "multiple selected tracks stay under the transport readout")) return false;
        break;
    }
    selection.setClips({audio});
    panel.setRecordEngaged(true);
    settle();
    if (!check(centredOnReadout(), "recording stays under the transport readout even with an underlying selected clip")) return false;
    if (!check(panel.accentColor() == th().accent,
               "recording context keeps the theme accent")) return false;
    panel.setRecordEngaged(false);
    settle();
    if (!check(fits() && panel.x() == left, "returning to clip tools restores following")) return false;
    if (!check(panel.accentColor() == th().accent,
               "audio context keeps the same theme accent")) return false;

    QSettings().setValue(QStringLiteral("contextPanel/followSelection"), false);
    panel.reloadFollowSetting();
    settle();
    if (!check(centredOnReadout(), "pinned clip context stays under the transport readout")) return false;
    QSettings().setValue(QStringLiteral("contextPanel/followSelection"), true);
    panel.reloadFollowSetting();
    settle();
    if (!check(fits() && panel.x() == left, "re-enabled clip following restores its anchor")) return false;

    left = right - 180;
    panel.relayout();
    panel.openPluginSearch();
    if (!check(panel.m_quickAdder && panel.m_quickAdder->isExpanded() &&
                   panel.m_quickAdder->isVisible(),
               "hidden plugin action becomes available in the shortcut event")) return false;
    { QEventLoop loop; QTimer::singleShot(360, &loop, &QEventLoop::quit); loop.exec(); }
    auto* search = panel.findChild<PluginQuickAdder*>();
    // Eight-pixel flare and fourteen-pixel end padding remain on each side.
    if (!check(search && search->isExpanded() && search->isVisible() && fits() &&
               search->width() <= right - left - 44,
               "hidden plugin action opens a search that fits the narrow strip")) return false;
    search->closeSearch();
    settle();
    panel.relayout();
    if (!check(!search->isExpanded() && fits() && visibleActions() > 1,
               "closing plugin search restores actions within the same narrow bounds")) return false;

    // Change context, then narrow the bounds before the swap has completed.
    selection.setTracks({audio.trackId});
    left = right - 140;
    panel.relayout();
    { QEventLoop loop; QTimer::singleShot(380, &loop, &QEventLoop::quit); loop.exec(); }
    if (!check(centredOnReadout(), "interrupted swap returns under the transport readout")) return false;
    if (!midi.clipId.isEmpty()) {
        selection.setClips({midi});
        panel.relayout();
        settle();
        if (!check(fits(), "MIDI context fits the same limits")) return false;
    }
    panel.setRecordEngaged(true);
    panel.relayout();
    settle();
    if (!check(centredOnReadout(), "recording context stays under the transport readout")) return false;
    panel.setPanelEnabled(false);
    { QEventLoop loop; QTimer::singleShot(380, &loop, &QEventLoop::quit); loop.exec(); }
    if (!check(wave->isVisible(), "waveform returns when context panel closes")) return false;

    strip.setBrowserVisible(true);
    strip.setBrowserZoneWidth(520);
    QApplication::processEvents();
    if (!check(viewControls->isVisible(),
               "widening the browser keeps navigation controls available")) return false;
    strip.setBrowserZoneWidth(30);
    QApplication::processEvents();
    panel.setRecordEngaged(false); selection.clear(); panel.setPanelEnabled(true); settle();
    const auto* importButton = panel.findChild<QAbstractButton*>("global.import");
    const auto* create = panel.findChild<QAbstractButton*>("global.track");
    const auto* alignment = panel.findChild<QAbstractButton*>("global.alignment");
    if (!check(panel.isVisible() && importButton && create && alignment &&
        importButton->isVisible() && create->isVisible() && alignment->isVisible(),
        "empty selection shows import, create track and vocal alignment")) return false;

    {
        daw::EngineController controller;
        if (!controller.initialize(48000.0, 512, false)) return false;
        const auto first = controller.addTrack(daw::TrackKind::Audio, "Context first");
        const auto second = controller.addTrack(daw::TrackKind::Audio, "Context second");
        const auto folder = controller.addFolder(false, "Context folder");
        ui::SelectionModel tracks;
        QWidget host;
        host.resize(1000, ui::kToolPanelHeight);
        ContextPanel reused(&controller, &tracks, &host);
        host.show();
        tracks.setTracks({QString::fromStdString(first)});
        QWidget* row = reused.m_content;
        auto* volume = row->findChild<ui::MiniSlider*>("ContextPanelTrackVolume");
        auto* pan = row->findChild<ui::MiniSlider*>("ContextPanelTrackPan");
        auto* mute = row->findChild<QAbstractButton*>("ContextPanelTrackMute");
        auto* adder = reused.m_quickAdder.data();
        if (!check(volume && pan && mute && adder, "track fixture exposes level, pan, mute and plugins")) return false;
        tracks.setTracks({QString::fromStdString(second)});
        if (!check(reused.m_content == row &&
                   reused.m_content->findChild<ui::MiniSlider*>("ContextPanelTrackVolume") == volume &&
                   reused.m_content->findChild<ui::MiniSlider*>("ContextPanelTrackPan") == pan &&
                   reused.m_content->findChild<QAbstractButton*>("ContextPanelTrackMute") == mute &&
                   reused.m_quickAdder == adder && adder->trackId() == QString::fromStdString(second),
                   "compatible track selection retains the controls and updates the plugin target")) return false;
        volume->editStarted();
        volume->valueChanged(-6.0);
        volume->editFinished();
        pan->editStarted();
        pan->valueChanged(35.0);
        pan->editFinished();
        mute->click();
        const auto unchanged = [&] {
            const auto* track = controller.project().findTrack(first);
            return track && track->volume == 1.0f && track->pan == 0.0f && !track->muted;
        };
        const auto* edited = controller.project().findTrack(second);
        if (!check(unchanged() && edited &&
                   std::abs(edited->volume - std::pow(10.0, -6.0 / 20.0)) < 1e-6 &&
                   std::abs(edited->pan - 0.35f) < 1e-6 && edited->muted,
                   "retained volume, pan and mute edit only the newly selected track")) return false;

        // A press before the first value change is already a gesture.
        volume->editStarted();
        tracks.setTracks({QString::fromStdString(first)});
        if (!check(reused.m_content != row,
                   "active control press rebuilds instead of retargeting its gesture")) return false;

        tracks.setTracks({QString::fromStdString(second)});
        QPointer<ui::MiniSlider> retiring =
            reused.m_content->findChild<ui::MiniSlider*>("ContextPanelTrackVolume");
        retiring->editStarted();
        retiring->valueChanged(-12.0);
        tracks.setTracks({QString::fromStdString(first)});
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        if (!check(unchanged() &&
                   retiring.isNull() &&
                   std::abs(controller.project().findTrack(second)->volume -
                            std::pow(10.0, -12.0 / 20.0)) < 1e-6,
                   "retiring control commits before deletion without a physical release")) return false;
        controller.undo();
        if (!check(unchanged() &&
                   std::abs(controller.project().findTrack(second)->volume -
                            std::pow(10.0, -6.0 / 20.0)) < 1e-6,
                   "retiring control gesture preserves undo for its original track")) return false;

        tracks.setTracks({QString::fromStdString(second)});
        reused.openPluginSearch();
        row = reused.m_content;
        adder = reused.m_quickAdder;
        if (!check(adder && adder->isExpanded(), "plugin search is open before track switch")) return false;
        tracks.setTracks({QString::fromStdString(first)});
        if (!check(reused.m_content != row &&
                   adder->trackId() == QString::fromStdString(second) &&
                   reused.m_quickAdder->trackId() == QString::fromStdString(first),
                   "open plugin search keeps its target when a replacement row is created")) return false;
        row = reused.m_content;
        tracks.setTracks({QString::fromStdString(folder)});
        if (!check(reused.m_content != row &&
                   !reused.m_content->findChild<ui::MiniSlider*>("ContextPanelTrackVolume"),
                   "a folder with no channel rebuilds the compatible tool layout")) return false;
    }
    return check(viewControls->isVisible(),
                 "navigation controls remain visible after the arrangement widens");
}
