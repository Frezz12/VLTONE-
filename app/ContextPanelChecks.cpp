#include "ContextPanel.hpp"
#include "EngineController.hpp"
#include "PluginQuickAdder.hpp"
#include "SelectionModel.hpp"
#include "ToolPanel.hpp"

#include <QAbstractButton>
#include <QApplication>
#include <QEventLoop>
#include <QTimer>

#include <cstdio>

bool ContextPanel::checkAdaptiveLayoutForTest() {
    ui::ClipSel audio;
    ui::ClipSel midi;
    for (const auto& track : m_controller->project().tracks) {
        for (const auto& clip : track.clips) {
            if (audio.clipId.isEmpty() && clip.kind == daw::ClipKind::Audio)
                audio = {QString::fromStdString(track.id), QString::fromStdString(clip.id)};
            if (midi.clipId.isEmpty() && clip.kind == daw::ClipKind::Midi)
                midi = {QString::fromStdString(track.id), QString::fromStdString(clip.id)};
        }
    }
    if (audio.clipId.isEmpty()) return false;
    ToolPanel strip;
    strip.setBrowserVisible(false);
    strip.setInspectorZoneWidth(30);
    strip.resize(1000, 44);
    ui::SelectionModel selection;
    ContextPanel panel(m_controller, &selection, &strip);
    strip.watchContextPanel(&panel);
    strip.show();
    QApplication::processEvents();
    int right = strip.contextRightEdge();
    int left = right - 620;
    int anchor = left + 200;
    panel.setBoundsProvider([&](int& from, int& to) { from = left; to = right; return true; });
    panel.setAnchorProvider([&](int& centre) { centre = anchor; return true; });
    selection.setClips({audio});
    panel.relayout();
    const auto check = [](bool passed, const char* detail) {
        std::fprintf(stderr, "%s Context layout: %s\n", passed ? "PASS" : "FAIL", detail);
        return passed;
    };
    const auto visibleActions = [&] {
        int count = 0;
        for (auto* widget : panel.findChildren<QWidget*>())
            if (widget->property("contextPriority").isValid() && widget->isVisible()) ++count;
        return count;
    };
    const auto fits = [&] {
        if (panel.x() < left || panel.x() + panel.width() > right) return false;
        for (auto* widget : panel.findChildren<QWidget*>()) {
            if (!widget->property("contextPriority").isValid() || !widget->isVisible()) continue;
            const int x = widget->mapTo(&panel, QPoint()).x();
            if (x < 0 || x + widget->width() > panel.width()) return false;
        }
        return true;
    };
    auto* wave = strip.findChild<QAbstractButton*>("WaveformScaleButton");
    const int fullCount = visibleActions();
    if (!check(fits() && fullCount >= 6 && wave && wave->isVisible(),
               "wide strip shows all actions and the waveform control")) return false;
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
    if (!check(wave->isHidden() && strip.contextRightEdge() == right,
               "waveform hides near the panel without moving its reserved boundary")) return false;
    for (int i = 0; i < 8; ++i) {
        panel.relayout();
        QApplication::processEvents();
        if (!check(wave->isHidden(), "crowded waveform control stays hidden")) return false;
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
    const auto centred = [&] {
        return fits() && std::abs(2 * panel.x() + panel.width() - left - right) <= 1;
    };
    // Keep returning the previous clip's position, as a timeline with its own
    // selection can do. The current panel context must outrank that stale span.
    anchor = left;
    panel.relayout();
    if (!check(panel.x() == left, "selected clip follows its left-edge anchor")) return false;
    anchor = right;
    panel.followSelection();
    selection.setTracks({audio.trackId});
    settle();
    if (!check(centred(), "clip-to-track selection interrupts drift and returns to the available centre")) return false;
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
        if (!check(centred(), "multiple selected tracks stay centred")) return false;
        break;
    }
    selection.setClips({audio});
    panel.setRecordEngaged(true);
    settle();
    if (!check(centred(), "recording stays centred even with an underlying selected clip")) return false;
    panel.setRecordEngaged(false);
    settle();
    if (!check(fits() && panel.x() == left, "returning to clip tools restores following")) return false;

    left = right - 180;
    panel.relayout();
    panel.openPluginSearch();
    { QEventLoop loop; QTimer::singleShot(360, &loop, &QEventLoop::quit); loop.exec(); }
    auto* search = panel.findChild<PluginQuickAdder*>();
    if (!check(search && search->isExpanded() && search->isVisible() && fits() &&
               search->width() <= right - left - 46,
               "hidden plugin action opens a search that fits the narrow strip")) return false;
    search->closeSearch();
    panel.relayout();

    // Change context, then narrow the bounds before the swap has completed.
    selection.setTracks({audio.trackId});
    left = right - 140;
    panel.relayout();
    { QEventLoop loop; QTimer::singleShot(380, &loop, &QEventLoop::quit); loop.exec(); }
    if (!check(fits(), "interrupted swap cannot restore obsolete wider geometry")) return false;
    if (!midi.clipId.isEmpty()) {
        selection.setClips({midi});
        panel.relayout();
        if (!check(fits(), "MIDI context fits the same limits")) return false;
    }
    panel.setRecordEngaged(true);
    panel.relayout();
    if (!check(fits(), "recording context fits the same limits")) return false;
    panel.setPanelEnabled(false);
    { QEventLoop loop; QTimer::singleShot(380, &loop, &QEventLoop::quit); loop.exec(); }
    return check(wave->isVisible(), "waveform returns when context panel closes");
}
