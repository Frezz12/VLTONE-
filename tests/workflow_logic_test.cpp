#include "EngineController.hpp"
#include "ProjectSerializer.hpp"
#include "ChannelStripPreset.hpp"
#include "recovery/SessionFile.hpp"
#include "platform/PathUtils.hpp"
#include "Recording/RecordingEngine.hpp"
#include "Core/AudioBuffer.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>

namespace fs = std::filesystem;
namespace {
int failures = 0;
bool check(bool ok, const char* message) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", message);
    failures += !ok;
    return ok;
}
std::unique_ptr<daw::EngineController> controller() {
    auto result = std::make_unique<daw::EngineController>();
    if (!result->initialize(48000, 512, false)) return {};
    return result;
}
void fillHistory(daw::EngineController& c, const std::string& track) {
    for (std::size_t i = 0; c.undoDepth() < c.undoLimit() && i <= c.undoLimit(); ++i)
        c.setTrackPan(track, (i & 1) ? .25f : -.25f);
    check(c.undoDepth() == c.undoLimit(), "fixture fills the bounded Undo history");
}
std::string readFile(const fs::path& file) {
    std::ifstream input(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
}

int main() {
    const auto root = fs::temp_directory_path() / ("vlt-workflow-" + daw::newUuid());
    fs::create_directories(root);
    const auto path = [](const fs::path& file) { return daw::platform::pathToUtf8(file); };
    const std::string tone = path(root / "tone.wav");
    audio::AudioBuffer samples(1, 4800);
    samples.clear();
    audio::AudioRecorder recorder;
    recorder.initialize(48000, 1);
    if (!check(bool(recorder.writeWAVFile(tone, samples, 48000)), "write import fixture")) return 1;

    {
        auto c = controller(); if (!c) return 1;
        const auto pattern = c->addPattern("Pattern");
        const auto child = c->addTrack(daw::TrackKind::Midi, "Source");
        c->moveTrackToFolder(child, pattern);
        const auto clip = c->addMidiClip(child, 0, 2);
        c->addNote(child, clip, 64, 0, 1, 100);
        fillHistory(*c, child);
        c->removeTrack(pattern);
        check(!c->project().findTrack(pattern) && !c->project().findTrack(child), "delete the complete Pattern");
        c->undo();
        const auto* restored = c->project().findTrack(child);
        check(c->project().findTrack(pattern) && restored && restored->parentId == pattern &&
              restored->outputBusId == pattern && !restored->clips.empty() &&
              restored->clips.front().id == clip && restored->clips.front().notes.size() == 1,
              "one Undo restores Pattern sources, routing and notes at capacity");
        c->redo();
        check(!c->project().findTrack(pattern) && !c->project().findTrack(child), "Redo deletes the same complete Pattern");
    }
    {
        auto c = controller(); if (!c) return 1;
        const auto pattern = c->addPattern("Extend Pattern");
        const auto child = c->addTrack(daw::TrackKind::Midi, "Source");
        c->moveTrackToFolder(child, pattern);
        const auto before = c->project().findTrack(pattern)->clips.front().durationSeconds;
        fillHistory(*c, child);
        const auto clip = c->addMidiClip(child, 0, before + 4);
        check(!clip.empty() && c->project().findTrack(pattern)->clips.front().durationSeconds > before,
              "MIDI insertion extends its Pattern owner");
        c->undo();
        check(c->project().findTrack(child)->clips.empty() &&
              c->project().findTrack(pattern)->clips.front().durationSeconds == before,
              "one Undo restores the Pattern boundary as well as removing the MIDI clip at capacity");
        c->redo();
        check(c->project().findTrack(child)->clips.size() == 1 &&
              c->project().findTrack(pattern)->clips.front().durationSeconds == before + 4,
              "Redo restores both the MIDI clip and its Pattern boundary");
    }
    {
        auto c = controller(); if (!c) return 1;
        const auto track = c->addTrack(daw::TrackKind::Audio, "Existing");
        fillHistory(*c, track);
        const auto count = c->project().tracks.size();
        const auto imported = c->importAudioToNewTrack(tone, 0);
        check(!imported.empty(), "import audio at history capacity");
        c->undo();
        check(c->project().tracks.size() == count && !c->project().findTrack(imported),
              "one Undo removes both the imported audio and its new track at capacity");
        c->redo();
        const auto* restored = c->project().findTrack(imported);
        check(restored && restored->clips.size() == 1, "Redo restores the imported track with its audio");
        const auto sampleCount = c->project().tracks.size();
        const auto sampleTrack = c->importSampleToMidiTrack(tone, 0);
        check(!sampleTrack.empty(), "import a sample as MIDI at history capacity");
        c->undo();
        check(c->project().tracks.size() == sampleCount && !c->project().findTrack(sampleTrack),
              "one Undo removes the imported sampler and its track at capacity");
        c->redo();
        const auto* samplerTrack = c->project().findTrack(sampleTrack);
        check(samplerTrack && samplerTrack->instrument.uid == "daw.sampler",
              "Redo restores the imported sampler instrument");
    }
    for (bool sample : {false, true}) {
        auto c = controller(); if (!c) return 1;
        const auto pattern = c->addPattern("Add Source");
        fillHistory(*c, pattern);
        const auto count = c->project().tracks.size();
        const auto sampler = c->pluginManager().find(daw::plugins::Format::Internal, "daw.sampler");
        if (!sampler) return 1;
        const auto child = sample ? c->addPatternSample(pattern, tone, 0)
                                  : c->addPatternInstrument(pattern, *sampler, 0);
        check(!child.empty(), "add a Pattern source at history capacity");
        c->undo();
        check(c->project().tracks.size() == count && !c->project().findTrack(child),
              "one Undo removes the complete added Pattern source at capacity");
        c->redo();
        const auto* restored = c->project().findTrack(child);
        check(restored && restored->parentId == pattern && !restored->clips.empty() &&
              restored->instrument.uid == "daw.sampler", "Redo restores the source, instrument and MIDI clip together");
    }
    {
        auto c = controller(); if (!c) return 1;
        const auto track = c->addTrack(daw::TrackKind::Audio, "Existing");
        fillHistory(*c, track);
        const auto label = c->undoLabel();
        const auto count = c->project().tracks.size();
        const auto missing = path(root / "missing.wav");
        check(c->importAudioToNewTrack(missing, 0).empty(), "missing import fails");
        check(c->undoDepth() == c->undoLimit() && c->undoLabel() == label && c->project().tracks.size() == count,
              "failed import preserves the full history without a phantom Add Track");
        c->undo();
        const auto redo = c->redoLabel();
        const auto depth = c->undoDepth();
        check(c->importAudioToNewTrack(missing, 0).empty(), "missing import also fails on a Redo branch");
        check(c->canRedo() && c->redoLabel() == redo && c->undoDepth() == depth,
              "failed audio import preserves the existing Redo branch");
        const auto pattern = c->addPattern("Failure");
        c->setTrackPan(pattern, .4f); c->undo();
        const auto patternRedo = c->redoLabel();
        const auto patternDepth = c->undoDepth();
        const auto patternCount = c->project().tracks.size();
        check(c->addPatternSample(pattern, missing, 0).empty(), "missing Pattern sample fails");
        check(c->canRedo() && c->redoLabel() == patternRedo && c->undoDepth() == patternDepth &&
              c->project().tracks.size() == patternCount, "failed Pattern import preserves history and leaves no source track");
        const auto sampler = c->pluginManager().find(daw::plugins::Format::Internal, "daw.sampler");
        if (!sampler) return 1;
        auto missingPlugin = *sampler;
        missingPlugin.uid = "daw.missing-test-instrument";
        check(c->addPatternInstrument(pattern, missingPlugin, 0).empty() &&
              c->canRedo() && c->redoLabel() == patternRedo && c->undoDepth() == patternDepth &&
              c->project().tracks.size() == patternCount,
              "failed Pattern instrument preserves history and leaves no source track");
        check(c->addPatternSample(pattern, tone, 0, "missing-owner").empty() &&
              c->addPatternInstrument(pattern, *sampler, 0, "missing-owner").empty() &&
              c->canRedo() && c->redoLabel() == patternRedo && c->undoDepth() == patternDepth &&
              c->project().tracks.size() == patternCount,
              "invalid Pattern owner fails without leaving an empty source or altering history");
        const auto outer = c->beginUndoGroup();
        const auto panBefore = c->project().findTrack(pattern)->pan;
        c->setTrackPan(pattern, -.2f);
        check(c->importAudioToNewTrack(missing, 0).empty(), "failed import inside an outer edit");
        c->setTrackPan(pattern, .2f);
        c->collapseUndo(outer, "Outer edit");
        c->undo();
        check(c->project().findTrack(pattern)->pan == panBefore,
              "rollback preserves the enclosing Undo group");
    }

    {
        auto c = controller(); if (!c) return 1;
        const auto unrelated = c->addTrack(daw::TrackKind::Midi, "Unrelated");
        const auto unrelatedClip = c->addMidiClip(unrelated, 0, 4);
        c->addNote(unrelated, unrelatedClip, 64, 0, 1, 100);
        const auto* untouched = c->project().findTrack(unrelated)->clips.front().notes.data();
        const auto noteId = untouched->id;
        const auto pattern = c->addPattern("Split part");
        const auto source = c->addTrack(daw::TrackKind::Midi, "Source");
        c->moveTrackToFolder(source, pattern);
        const auto midi = c->addMidiClip(source, 0, 2);
        c->addNote(source, midi, 60, 0, 4, 100);
        const auto right = c->splitClip(source, midi, 1);
        check(!right.empty() && c->project().findTrack(source)->clips.size() == 2,
              "split a Pattern part into two content windows");
        c->setNotePan(unrelated, unrelatedClip, noteId, .3f);
        c->undo();
        const auto& afterUndo = c->project().findTrack(unrelated)->clips.front().notes;
        check(c->project().findTrack(source)->clips.size() == 1 &&
              afterUndo.data() == untouched && afterUndo.front().pan == .3f,
              "Undo of a Pattern part split preserves unrelated live note edits and storage");
        c->redo();
        const auto& afterRedo = c->project().findTrack(unrelated)->clips.front().notes;
        const auto& parts = c->project().findTrack(source)->clips;
        check(parts.size() == 2 && parts.back().id == right &&
              parts.front().contentStorage() == parts.back().contentStorage() &&
              afterRedo.data() == untouched && afterRedo.front().pan == .3f,
              "Redo of a Pattern part split preserves identities, links and unrelated edits");
    }

    daw::ProjectModel project;
    project.name = "First";
    const auto document = root / "project.json";
    check(bool(daw::ProjectSerializer::saveDocument(project, path(document), daw::MediaPaths::Absolute)), "write initial project document");
    project.name = "Second";
    check(bool(daw::ProjectSerializer::saveDocument(project, path(document), daw::MediaPaths::Absolute)), "atomically replace an existing project");
    const auto original = readFile(document);
    project.name = std::string(1, char(0xff));
    bool threw = false, saved = false;
    try { saved = bool(daw::ProjectSerializer::saveDocument(project, path(document), daw::MediaPaths::Absolute)); }
    catch (const std::exception&) { threw = true; }
    check(!threw && !saved && readFile(document) == original,
          "invalid text returns a save error and preserves the previous project without throwing");
    const auto session = root / "session.json";
    check(daw::recovery::writeJsonAtomically({{"name", "valid"}}, session), "write recovery session");
    const auto sessionBefore = readFile(session);
    check(daw::recovery::writeJsonAtomically({{"name", "replacement"}}, session) &&
          readFile(session) != sessionBefore, "atomically replace an existing recovery session");
    const auto sessionReplacement = readFile(session);
    threw = false; saved = false;
    try { saved = daw::recovery::writeJsonAtomically({{"name", project.name}}, session); }
    catch (const std::exception&) { threw = true; }
    check(!threw && !saved && readFile(session) == sessionReplacement,
          "invalid recovery text returns failure without terminating the journal worker");
    project.name = "Valid";
    const auto blocked = root / "project-directory-target";
    fs::create_directory(blocked);
    check(!daw::ProjectSerializer::saveDocument(project, path(blocked), daw::MediaPaths::Absolute) && fs::is_directory(blocked),
          "project save cannot delete a destination directory on rename failure");
    daw::EngineController::ChannelSnapshot preset;
    preset.hasSettings = true;
    const auto presetFile = root / "preset.vlstrip";
    check(bool(daw::ChannelStripPreset::save(preset, path(presetFile))), "write initial preset");
    const auto presetBefore = readFile(presetFile);
    preset.pan = .2f;
    check(bool(daw::ChannelStripPreset::save(preset, path(presetFile))) &&
          readFile(presetFile) != presetBefore, "atomically replace an existing preset");
    const auto presetDirectory = root / "preset-directory-target";
    fs::create_directory(presetDirectory);
    check(!daw::ChannelStripPreset::save(preset, path(presetDirectory)) && fs::is_directory(presetDirectory),
          "preset save cannot delete a destination directory on rename failure");
    const auto recoveryDirectory = root / "recovery-directory-target";
    fs::create_directory(recoveryDirectory);
    check(!daw::recovery::writeJsonAtomically({{"name", "valid"}}, recoveryDirectory) && fs::is_directory(recoveryDirectory),
          "recovery save cannot delete a destination directory on rename failure");
    bool temporaryLeft = false;
    for (const auto& file : fs::directory_iterator(root))
        temporaryLeft |= file.path().filename().string().find(".tmp-") != std::string::npos;
    check(!temporaryLeft, "failed saves clean up temporary files");
    fs::remove_all(root);
    return failures ? 1 : 0;
}
