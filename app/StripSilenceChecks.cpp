#include "StripSilenceDialog.hpp"
#include "StripSilencePreferences.hpp"
#include "ContextPanel.hpp"
#include "Controls.hpp"
#include "SelectionModel.hpp"
#include "TimelineWidget.hpp"
#include "Core/AudioBuffer.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <cmath>
#include <cstdio>

bool StripSilenceDialog::checkForTest(const QString& screenshot) {
    bool ok = true;
    const auto check = [&](bool value, const char* what) {
        std::fprintf(stderr, "%s Strip Silence UI: %s\n", value ? "PASS" : "FAIL", what);
        ok &= value; return value;
    };
    daw::EngineController controller{};
    if (!controller.initialize(48000, 256, false)) return false;
    controller.setTempo(120);
    auto prefs = controller.recordingPrefs();
    prefs.autoSilence = false;
    prefs.stripSilence = {};
    controller.setRecordingPrefs(prefs);
    QSettings().remove("contextPanel");
    QTemporaryDir directory;
    const auto wav = directory.filePath("Percussion.wav").toStdString();
    audio::AudioBuffer audio(2, 240000);
    for (unsigned i = 0; i < 240000; ++i) {
        const double t = double(i) / 48000;
        double value = .00005 * std::sin(i * .4);
        for (const auto onset : {.5, 1.5, 2.5, 3.5})
            if (t >= onset && t < onset + .6)
                value += .72 * std::exp(-(t - onset) * 10) * std::sin(i * .029);
        audio.getChannel(0)[i] = float(value);
        audio.getChannel(1)[i] = float(-value);
    }
    audio::AudioRecorder writer; writer.initialize(48000, 2);
    if (!writer.writeWAVFile(wav, audio, 48000)) return false;
    const auto track = controller.importAudioToNewTrack(wav, 0, "Percussion");
    const auto clip = controller.project().findTrack(track)->clips.front().id;
    const auto depth = controller.undoDepth();
    daw::StripSilenceSettings remembered;
    {
        StripSilenceDialog dialog(controller, {{track, clip}});
        dialog.show();
        QElapsedTimer elapsed; elapsed.start();
        while (!dialog.m_ready && elapsed.elapsed() < 10000) {
            QApplication::processEvents(); QThread::msleep(5);
        }
        if (!check(dialog.m_ready, "background analysis completes")) return false;
        auto* threshold = dialog.findChild<QDoubleSpinBox*>("SilenceThreshold");
        auto* slider = dialog.findChild<QSlider*>("SilenceThresholdSlider");
        slider->setValue(-72);
        check(threshold->value() == -36 && dialog.m_settings.thresholdDb == -36,
              "slider and numeric entry update the same threshold");
        threshold->setValue(-32);
        check(slider->value() == -64, "numeric entry updates the slider");
        if (!screenshot.isEmpty()) {
            QApplication::processEvents();
            check(dialog.grab().save(screenshot), "saves dialog screenshot");
            dialog.resize(640, 600); QApplication::processEvents();
            const QFileInfo file(screenshot);
            check(dialog.grab().save(file.path() + "/" + file.completeBaseName() + "-compact.png"), "saves compact dialog screenshot");
            for (auto* field : dialog.findChildren<QDoubleSpinBox*>())
                check(dialog.rect().contains(QRect(field->mapTo(&dialog, QPoint()), field->size())), "numeric control fits compact window");
        }
        dialog.m_auto->setChecked(true);
        check(controller.recordingPrefs().autoSilence, "automatic switch writes through");
        remembered = dialog.m_settings;
        dialog.m_apply->click();
        check(dialog.applied() && dialog.createdClips().size() == 4, "Apply creates the four previewed regions");
        check(controller.undoDepth() == depth + 1, "manual apply is one undo step");
    }
    controller.undo();
    check(controller.project().findTrack(track)->clips.size() == 1 &&
          controller.project().findTrack(track)->clips.front().id == clip, "Undo restores the source clip");
    prefs.stripSilence = {}; prefs.autoSilence = false;
    controller.setRecordingPrefs(prefs);
    ui::silence::restore(controller);
    check(controller.recordingPrefs().stripSilence == remembered && controller.recordingPrefs().autoSilence,
          "last settings and Auto Silence survive preference restore");
    {
        StripSilenceDialog settings(controller, {});
        check(settings.m_settings == remembered && settings.m_settingsOnly && settings.m_ready,
              "recording settings open without requiring a selected clip");
        settings.m_apply->click();
        check(!settings.applied() && controller.undoDepth() == depth, "saving settings does not edit audio");
    }

    ui::SelectionModel selection;
    ContextPanel panel(&controller, &selection);
    panel.resize(900, 60); panel.show();
    selection.setClips({{QString::fromStdString(track), QString::fromStdString(clip)}});
    panel.refresh(); QApplication::processEvents();
    auto* strip = panel.findChild<ui::IconButton*>("ContextPanelStripSilence");
    bool requested = false;
    QObject::connect(&panel, &ContextPanel::stripSilenceRequested, &panel, [&] { requested = true; });
    if (check(strip && strip->isEnabled(), "selected audio exposes the context-panel tool")) strip->click();
    check(requested, "context-panel tool requests the dialog");
    panel.setRecordEngaged(true); panel.refresh(); QApplication::processEvents();
    auto* automatic = panel.findChild<ui::IconButton*>("ContextPanelAutoSilence");
    if (check(automatic && automatic->isChecked(), "recording panel reflects Auto Silence")) automatic->click();
    check(!controller.recordingPrefs().autoSilence, "recording panel toggles Auto Silence");
    check(panel.findChild<ui::IconButton*>("ContextPanelSilenceSettings") != nullptr, "recording panel exposes settings");

    TimelineWidget timeline(&controller);
    timeline.selectClips({{QString::fromStdString(track), QString::fromStdString(clip)}});
    QMenu menu;
    timeline.populateSelectedClipActionsMenu(menu);
    auto* action = menu.findChild<QAction*>("StripSilenceAction");
    check(action && action->isEnabled(), "audio context menu exposes Strip Silence");
    requested = false;
    QObject::connect(&timeline, &TimelineWidget::stripSilenceRequested, &timeline, [&] { requested = true; });
    if (action) action->trigger();
    check(requested, "audio context menu requests the dialog");
    check(panel.checkAdaptiveLayoutForTest(), "clip and recording tools fit a narrow context panel");
    const auto second = controller.importAudio(wav, track, 6);
    {
        StripSilenceDialog batch(controller, {{track, clip}, {track, second}});
        batch.show();
        QElapsedTimer elapsed; elapsed.start();
        while (!batch.m_ready && elapsed.elapsed() < 10000) {
            QApplication::processEvents(); QThread::msleep(5);
        }
        check(batch.m_ready && batch.m_clip->isVisible() && batch.m_clip->count() == 2,
              "multi-clip dialog exposes individual previews");
        batch.m_clip->setCurrentIndex(1);
        batch.m_apply->click();
        check(batch.applied() && batch.createdClips().size() == 8, "multi-clip Apply processes the complete selection");
    }
    return ok;
}
