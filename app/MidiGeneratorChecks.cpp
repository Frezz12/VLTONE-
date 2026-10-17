#include "MidiGeneratorDialog.hpp"
#include "EngineController.hpp"
#include "ProjectSerializer.hpp"
#include "Internal/SamplerInstance.hpp"
#include "Recording/RecordingEngine.hpp"
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QScrollArea>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QThread>
#include <QToolButton>
#include <cmath>
#include <cstdio>

bool MidiGeneratorDialog::checkForTest(const QString& screenshots) {
    bool ok = true;
    const auto check = [&](bool value, const char* message) {
        std::printf("%s  %s\n", value ? "PASS" : "FAIL", message); ok &= value; return value;
    };
    daw::EngineController controller;
    if (!controller.initialize(48000, 256, false)) return false;
    const auto track = controller.addTrack(daw::TrackKind::Midi, "Melody");
    const auto id = controller.addMidiClip(track, 0, 2);
    const auto notes = [&] { return daw::midiNotes(controller.project().findTrack(track)->clips.front()); };
    const auto document = [&] { std::string data; daw::ProjectSerializer::serializeDocument(controller.project(), data); return data; };
    const auto wait = [](MidiGeneratorDialog& dialog) {
        QElapsedTimer timer; timer.start();
        while (timer.elapsed() < 5000 && (dialog.m_pending || dialog.m_job.valid())) {
            QApplication::processEvents(); dialog.poll(); QThread::msleep(2);
        }
        return dialog.m_candidates.size() == 3;
    };
    const auto original = document(); const auto originalDepth = controller.undoDepth();
    {
        MidiGeneratorDialog dialog(controller, track, id, 0); dialog.show();
        dialog.m_seed->setValue(817);
        check(dialog.m_bars->value() == 8 && wait(dialog), "generator opens with three eight-bar candidates");
        check(document() == original && controller.undoDepth() == originalDepth, "generating does not edit the project or undo stack");
        if (!dialog.m_candidates.empty()) {
            const auto first = dialog.m_candidates[0].id;
            dialog.m_generate->click(); check(wait(dialog) && first != dialog.m_candidates[0].id, "Generate more changes the seed and musical result");
            dialog.m_bars->setValue(64); dialog.generate(); dialog.m_bars->setValue(4); dialog.m_bars->setValue(8);
            check(wait(dialog) && dialog.m_request.bars == 8, "an outdated worker result cannot overwrite the latest parameters");
        }
        if (!screenshots.isEmpty()) {
            QDir().mkpath(screenshots); QApplication::processEvents();
            check(dialog.grab().save(QDir(screenshots).filePath("midi-generator.png")), "generator screenshot saved");
            for (auto* button : dialog.findChildren<QToolButton*>()) if (button->isCheckable()) button->setChecked(true);
            QApplication::processEvents();
            if (auto* scroll = dialog.findChild<QScrollArea*>()) scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
            check(dialog.grab().save(QDir(screenshots).filePath("midi-generator-settings.png")), "advanced settings screenshot saved");
        }
        dialog.reject();
    }
    check(document() == original && controller.undoDepth() == originalDepth, "Cancel leaves the original document untouched");
    {
        MidiGeneratorDialog dialog(controller, track, id, 0);
        if (check(wait(dialog), "apply fixture generates candidates")) {
            dialog.selectCandidate(2); const auto expected = dialog.m_prepared[2];
            dialog.m_apply->click();
            check(notes() == expected && controller.project().findTrack(track)->clips.front().durationSeconds == 16 &&
                  controller.undoDepth() == originalDepth + 1, "Apply inserts the selected preview exactly and grows the clip in one undo");
            controller.undo(); check(document() == original, "undo restores the whole pre-generation document");
            controller.redo(); check(notes() == expected, "redo restores the preview's note IDs and values"); controller.undo();
        }
    }
    {
        MidiGeneratorDialog dialog(controller, track, id, 0); wait(dialog);
        controller.setClipNotes(track, id, {{"manual", 62, 0, 1, 90}}, "Edit while open");
        const auto changed = notes(); dialog.apply();
        check(!dialog.m_apply->isEnabled() && notes() == changed && dialog.m_candidates.empty(), "stale candidates cannot overwrite an edit made while the generator is open");
        dialog.m_generate->click(); check(wait(dialog) && dialog.m_baseNotes == changed, "regeneration uses the updated target");
        dialog.reject(); controller.undo();
    }

    const auto harmonyTrack = controller.addTrack(daw::TrackKind::Midi, "Harmony");
    const auto harmonyClip = controller.addMidiClip(harmonyTrack, 1, 1);
    controller.setClipNotes(harmonyTrack, harmonyClip,
        {{"h1", 61, 0, 2, 90}, {"h2", 65, 0, 2, 90}, {"h3", 68, 0, 2, 90}}, "Harmony fixture");
    {
        MidiGeneratorDialog dialog(controller, track, id, 0);
        dialog.m_mode->setCurrentIndex(1); dialog.m_bars->setValue(2); dialog.m_expression->setValue(0);
        for (std::size_t i = 0; i < dialog.m_harmonySources.size(); ++i)
            if (dialog.m_harmonySources[i].second == harmonyClip) dialog.m_harmony->setCurrentIndex(int(i));
        bool follows = wait(dialog);
        for (const auto& candidate : dialog.m_candidates) for (const auto& note : candidate.notes) {
            const int pc = note.pitch % 12;
            if (note.startBeats >= 2 && note.startBeats < 4) follows &= pc == 1 || pc == 5 || pc == 8;
            else follows &= daw::miditools::inScale(note.pitch, 0, daw::miditools::Scale::Major);
        }
        check(follows, "selected harmony follows arrangement time and falls back to key outside its clip");
        check(!dialog.m_voicing->isEnabled(), "explicit harmony owns its chord tones");
        controller.beginClipTrimEdit({{harmonyTrack, harmonyClip}});
        controller.setClipTrim(harmonyTrack, harmonyClip, 2, 0, 1); dialog.poll();
        check(dialog.m_candidates.empty() && !dialog.m_apply->isEnabled(), "a live move of the harmony source invalidates its candidates");
        controller.endClipTrimEdit("Move harmony fixture"); controller.undo(); dialog.reject();
    }

    QTemporaryDir temp;
    const auto wave = QDir(temp.path()).filePath("tone.wav").toStdString();
    audio::AudioBuffer tone(2, 48000);
    for (unsigned n = 0; n < 48000; ++n) tone.getChannel(0)[n] = tone.getChannel(1)[n] = .2f * std::sin(float(n * 6.28318530718 * 261.6256 / 48000));
    audio::AudioRecorder recorder; recorder.initialize(48000, 2); recorder.writeWAVFile(wave, tone, 48000);
    check(controller.setTrackInstrumentPlugin(track, daw::plugins::sampler::SamplerInstance::staticDescriptor()), "listening fixture loads the sampler");
    const auto slot = controller.project().findTrack(track)->instrument.id;
    check(controller.loadSamplerSample(track, slot, wave), "listening fixture loads a tone");
    const auto beforeListening = document(); const auto listeningDepth = controller.undoDepth();
    {
        MidiGeneratorDialog dialog(controller, track, id, 0); dialog.show();
        if (check(wait(dialog), "audition fixture generates candidates")) {
            dialog.toggleListen();
            check(dialog.m_listening && dialog.m_draft &&
                  daw::midiNotes(dialog.m_draft->project().findTrack(track)->clips.front()) == dialog.m_prepared[0],
                  "audition plays the exact candidate in a separate controller");
            audio::AudioBuffer input(2, 256), output(2, 256); double energy = 0;
            for (int block = 0; block < 800; ++block) {
                controller.processDeviceBlockForTest(input, output, 256);
                for (unsigned n = 0; n < 256; ++n) energy += output.getChannel(0)[n] * output.getChannel(0)[n];
            }
            check(energy > .001, "generated notes produce audible audio through the track instrument");
            dialog.selectCandidate(1); check(!dialog.m_listening, "switching candidates stops the old audition");
            dialog.toggleListen(); dialog.hide(); check(!dialog.m_listening, "hiding the generator stops audition");
            dialog.reject();
        }
    }
    check(document() == beforeListening && controller.undoDepth() == listeningDepth, "listening and cancellation preserve the original project and undo history");
    return ok;
}
