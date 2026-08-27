#include "EngineController.hpp"
#include "Core/AudioBuffer.hpp"
#include "Internal/SamplerInstance.hpp"
#include "Internal/SamplerParams.hpp"
#include "ProjectSerializer.hpp"
#include "Recording/RecordingEngine.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

const daw::TrackModel* named(const daw::ProjectModel& project,
                             const std::string& name) {
    for (const auto& track : project.tracks) {
        if (track.name == name) return &track;
    }
    return nullptr;
}

bool writeTone(const fs::path& path, float frequency) {
    audio::AudioBuffer tone(2, 4800);
    for (audio::BufferSize frame = 0; frame < 4800; ++frame) {
        const float sample = 0.5f * std::sin(
            2.0f * 3.14159265f * frequency * float(frame) / 48000.0f);
        tone.getChannel(0)[frame] = sample;
        tone.getChannel(1)[frame] = sample;
    }
    audio::AudioRecorder recorder;
    return recorder.initialize(48000, 2) &&
           recorder.writeWAVFile(path.string(), tone, 48000);
}

} // namespace

int main() {
    const fs::path root =
        fs::temp_directory_path() / "daw_project_template_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    daw::EngineController source;
    check(source.initialize(48000.0, 512, false).isOk(),
          "source controller initializes");
    source.newProject();
    source.setProjectName("Working Song");
    source.setTempo(132.0);
    source.setTimeSignature(7, 8);
    source.setProjectKey(9, "minor");
    source.setMasterVolume(0.77f);
    source.setMasterPan(-0.15f);

    const std::string bus = source.addTrack(daw::TrackKind::Bus, "Drum Bus");
    const std::string audio = source.addTrack(daw::TrackKind::Audio, "Kick");
    const std::string midi = source.addTrack(daw::TrackKind::Midi, "Guide");
    const std::string pattern = source.addPattern("Beat Pattern");
    const std::string sampler =
        source.addTrack(daw::TrackKind::Instrument, "Sampler Lane");
    source.setTrackVolume(audio, 0.42f);
    source.setTrackPan(audio, 0.25f);
    source.setTrackMuted(audio, true);
    source.setTrackColor(audio, 0xC0504D);
    source.setTrackHeight(audio, 118.0);
    source.setTrackInputChannel(audio, 2);
    source.setTrackInputChannelCount(audio, 1);
    source.setTrackInputEnabled(audio, true);
    source.setTrackRecordMode(audio, daw::TrackRecordMode::Layers);
    check(source.setTrackOutputBus(audio, bus),
          "template source routes a track to its bus");
    const std::string send = source.addSend(audio, bus);
    check(!send.empty(), "template source has an internal send");
    const std::string midiClip = source.addMidiClip(midi, 0.0, 4.0);
    check(!midiClip.empty(), "template source has arrangement content to strip");
    const fs::path samplerAudio = root / "sampler-source.wav";
    const fs::path clipOnlyAudio = root / "clip-only.wav";
    check(writeTone(samplerAudio, 220.0f) && writeTone(clipOnlyAudio, 440.0f),
          "portable-audio fixtures are created");
    check(source.loadInstrumentSampler(sampler, samplerAudio.string()),
          "template source loads the built-in Sampler");
    const auto* samplerSourceTrack = source.project().findTrack(sampler);
    const std::string samplerSlot =
        samplerSourceTrack ? samplerSourceTrack->instrument.id : std::string{};
    source.setInsertParameter(sampler, samplerSlot, "vol", 0.37);
    check(!source.importAudio(clipOnlyAudio.string(), audio, 1.0).empty(),
          "template source has clip-only media to exclude");

    const std::size_t liveTrackCount = source.project().tracks.size();
    const std::size_t liveUndoDepth = source.undoDepth();
    const fs::path package = root / "Recording.vltt";
    check(source.saveProjectTemplate(package.string(), "Recording").isOk(),
          "template package saves");
    check(fs::is_directory(package), "VLTT is a package directory");
    check(fs::is_regular_file(package / daw::ProjectSerializer::kProjectFile),
          "VLTT contains the project manifest");
    check(fs::is_directory(package / daw::ProjectSerializer::kMediaDir) &&
              fs::is_directory(package / daw::ProjectSerializer::kStateDir),
          "VLTT has portable Content and State folders");
    check(fs::is_regular_file(package / daw::ProjectSerializer::kMediaDir /
                              samplerAudio.filename()),
          "Sampler content is embedded in the template");
    check(!fs::exists(package / daw::ProjectSerializer::kMediaDir /
                      clipOnlyAudio.filename()),
          "media referenced only by stripped clips is not embedded");

    // Saving is an export: it must not sanitize or rename the live document.
    check(source.projectName() == "Working Song",
          "saving a template keeps the current project name");
    check(source.project().tracks.size() == liveTrackCount &&
              named(source.project(), "Guide") &&
              !named(source.project(), "Guide")->clips.empty(),
          "saving a template leaves live clips and tracks untouched");
    const auto* liveSampler = named(source.project(), "Sampler Lane");
    const auto* liveSamplerInstance =
        liveSampler
            ? source.samplerInstance(liveSampler->id,
                                     liveSampler->instrument.id)
            : nullptr;
    check(liveUndoDepth == source.undoDepth() && liveSampler &&
              liveSampler->instrument.stateFile.empty() &&
              liveSamplerInstance &&
              liveSamplerInstance->samplePath() == samplerAudio.string(),
          "template export does not write state references or paths into the live document");

    daw::ProjectModel stored;
    check(daw::ProjectSerializer::load(stored, package.string()).isOk(),
          "template manifest round-trips through the project serializer");
    check(stored.name == "Recording" && stored.tempo == 132.0 &&
              stored.timeSigNumerator == 7 &&
              stored.timeSigDenominator == 8 && stored.keyRoot == 9 &&
              stored.scale == "minor",
          "project-wide template settings round-trip");
    check(stored.loopStartSeconds == 0.0 && stored.loopEndSeconds == 0.0 &&
              !stored.loopEnabled,
          "template has no arrangement loop region");
    bool anyClips = false;
    for (const auto& track : stored.tracks) anyClips |= !track.clips.empty();
    check(!anyClips, "template manifest contains no user clips");
    const auto* storedKick = named(stored, "Kick");
    const auto* storedBus = named(stored, "Drum Bus");
    const auto* storedSampler = named(stored, "Sampler Lane");
    check(storedKick && storedBus && storedKick->outputBusId == storedBus->id &&
              storedKick->sends.size() == 1 &&
              storedKick->sends.front().destinationTrackId == storedBus->id,
          "track settings and internal routing are preserved in the template");
    check(storedSampler && storedSampler->instrument.isLoaded() &&
              !storedSampler->instrument.stateFile.empty(),
          "Sampler slot and exact state chunk are referenced by the template");

    daw::EngineController opened;
    check(opened.initialize(48000.0, 512, false).isOk(),
          "open controller initializes");
    opened.newProject();
    opened.addTrack(daw::TrackKind::Audio, "Throwaway");
    check(opened.openProjectTemplate(package.string()).isOk(),
          "template opens as a fresh project");
    check(opened.projectName() == "Recording" &&
              !named(opened.project(), "Throwaway"),
          "opening replaces the document and uses the template name");
    const auto* openedPattern = named(opened.project(), "Beat Pattern");
    check(openedPattern && openedPattern->clips.size() == 1 &&
              openedPattern->clips.front().kind == daw::ClipKind::Pattern,
          "opening regenerates the structural Pattern container only");
    const auto* openedSampler = named(opened.project(), "Sampler Lane");
    daw::plugins::sampler::SamplerInstance* openedSamplerInstance =
        openedSampler
            ? opened.samplerInstance(openedSampler->id,
                                     openedSampler->instrument.id)
            : nullptr;
    check(openedSamplerInstance && openedSamplerInstance->rawSample() &&
              fs::path(openedSamplerInstance->samplePath()).parent_path() ==
                  package / daw::ProjectSerializer::kMediaDir &&
              std::abs(openedSamplerInstance->parameterValue(
                           std::uint32_t(daw::plugins::sampler::Param::Volume)) -
                       0.37) < 1e-6,
          "new project restores portable Sampler content and exact parameters");

    // A missing third-party slot is still part of the document. Add one to the
    // import fixture after the open round-trip so the remapper is tested even
    // on a machine that has no external test plugin installed.
    const std::string missingSlotId = "template-missing-plugin-slot";
    daw::TrackModel* mutableStoredKick =
        storedKick ? stored.findTrack(storedKick->id) : nullptr;
    if (mutableStoredKick && storedBus) {
        daw::InsertModel missing;
        missing.id = missingSlotId;
        missing.name = "Unavailable Compressor";
        missing.format = daw::PluginFormat::Vst3;
        missing.uid = "com.vlt.tests.unavailable-compressor";
        missing.sidechainTrackId = storedBus->id;
        missing.parameters.push_back({"threshold", -18.0});
        mutableStoredKick->inserts.push_back(std::move(missing));
    }
    check(mutableStoredKick &&
              daw::ProjectSerializer::save(stored, package.string()).isOk(),
          "missing-plugin import fixture saves without disturbing package state");

    daw::EngineController destination;
    check(destination.initialize(48000.0, 512, false).isOk(),
          "destination controller initializes");
    destination.newProject();
    destination.setTempo(90.0);
    destination.setMasterVolume(0.33f);
    const std::string existing =
        destination.addTrack(daw::TrackKind::Audio, "Existing");
    const std::size_t beforeImport = destination.project().tracks.size();

    std::vector<std::string> importedIds;
    check(destination.importProjectTemplateTracks(package.string(), importedIds)
              .isOk(),
          "template tracks import into a nonempty project");
    check(importedIds.size() == stored.tracks.size() &&
              destination.project().tracks.size() ==
                  beforeImport + stored.tracks.size(),
          "all template tracks append to the existing track set");
    check(destination.project().tracks.front().id == existing &&
              destination.tempo() == 90.0 &&
              destination.project().masterVolume == 0.33f,
          "import preserves existing tracks and project/master settings");

    const auto* importedKick = named(destination.project(), "Kick");
    const auto* importedBus = named(destination.project(), "Drum Bus");
    check(importedKick && importedBus && storedKick &&
              importedKick->id != storedKick->id &&
              importedKick->outputBusId == importedBus->id &&
              importedKick->sends.size() == 1 &&
              importedKick->sends.front().destinationTrackId == importedBus->id,
          "import assigns fresh ids and remaps internal routing");
    check(importedKick && importedBus && importedKick->inserts.size() == 1 &&
              importedKick->inserts.front().id != missingSlotId &&
              importedKick->inserts.front().sidechainTrackId == importedBus->id &&
              importedKick->inserts.front().parameters.size() == 1,
          "missing plugin fallback survives and its slot/sidechain ids are remapped");
    const auto* importedSampler = named(destination.project(), "Sampler Lane");
    daw::plugins::sampler::SamplerInstance* importedSamplerInstance =
        importedSampler
            ? destination.samplerInstance(importedSampler->id,
                                          importedSampler->instrument.id)
            : nullptr;
    check(importedSampler && storedSampler &&
              importedSampler->instrument.id != storedSampler->instrument.id &&
              importedSamplerInstance && importedSamplerInstance->rawSample() &&
              std::abs(importedSamplerInstance->parameterValue(
                           std::uint32_t(daw::plugins::sampler::Param::Volume)) -
                       0.37) < 1e-6,
          "track import remaps the Sampler slot and restores only its live state");

    check(destination.canUndo(), "template import creates an undo entry");
    destination.undo();
    check(destination.project().tracks.size() == beforeImport &&
              destination.project().tracks.front().id == existing,
          "one undo removes the complete imported track set");
    destination.redo();
    check(destination.project().tracks.size() ==
              beforeImport + stored.tracks.size() &&
              named(destination.project(), "Kick"),
          "one redo restores the complete imported track set");
    importedSampler = named(destination.project(), "Sampler Lane");
    importedSamplerInstance =
        importedSampler
            ? destination.samplerInstance(importedSampler->id,
                                          importedSampler->instrument.id)
            : nullptr;
    check(importedSamplerInstance && importedSamplerInstance->rawSample() &&
              std::abs(importedSamplerInstance->parameterValue(
                           std::uint32_t(daw::plugins::sampler::Param::Volume)) -
                       0.37) < 1e-6,
          "redo restores imported plugin state and Sampler content");

    const std::size_t beforeBadImport = destination.project().tracks.size();
    std::vector<std::string> badIds{"stale"};
    check(!destination
               .importProjectTemplateTracks((root / "Missing.vltt").string(),
                                            badIds)
               .isOk(),
          "missing template import reports an error");
    check(destination.project().tracks.size() == beforeBadImport &&
              badIds.empty(),
          "failed import does not partially mutate the project");

    const fs::path corrupt = root / "Corrupt.vltt";
    fs::create_directories(corrupt, ec);
    {
        std::ofstream manifest(corrupt / daw::ProjectSerializer::kProjectFile);
        manifest << "{ definitely-not-a-project";
    }
    const daw::ProjectModel beforeCorruptOpen = destination.project();
    check(!destination.openProjectTemplate(corrupt.string()).isOk(),
          "corrupt template open reports an error");
    check(destination.project().name == beforeCorruptOpen.name &&
              destination.project().tracks.size() ==
                  beforeCorruptOpen.tracks.size() &&
              destination.project().tracks.front().id ==
                  beforeCorruptOpen.tracks.front().id,
          "corrupt template open leaves the active document unchanged");

    fs::remove(package / daw::ProjectSerializer::kMediaDir /
                   samplerAudio.filename(),
               ec);
    const daw::ProjectModel beforeMissingContent = destination.project();
    const audio::Result missingContent =
        destination.openProjectTemplate(package.string());
    check(!missingContent.isOk() &&
              missingContent.message().find("Sampler") != std::string::npos,
          "missing mandatory Sampler content reports a specific error");
    check(destination.project().name == beforeMissingContent.name &&
              destination.project().tracks.size() ==
                  beforeMissingContent.tracks.size() &&
              destination.project().tracks.front().id ==
                  beforeMissingContent.tracks.front().id,
          "missing template content rolls the whole open operation back");

    source.shutdown();
    opened.shutdown();
    destination.shutdown();
    fs::remove_all(root, ec);

    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "project template tests passed\n";
    return 0;
}
