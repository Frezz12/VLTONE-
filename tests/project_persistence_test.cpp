#include "EngineController.hpp"
#include "ProjectSerializer.hpp"
#include "Recording/RecordingEngine.hpp"
#include "fixtures/test_clap/RecoveryTestControl.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {
using namespace daw;
namespace fs = std::filesystem;
int failures = 0;
bool check(bool value, const char* message) {
    std::printf("%s %s\n", value ? "PASS" : "FAIL", message);
    failures += !value;
    return value;
}
std::string read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}
void process(EngineController& controller) {
    audio::AudioBuffer input(2, 64), output(2, 64);
    controller.processDeviceBlockForTest(input, output, 64);
}
void writeTone(const fs::path& path, unsigned frames) {
    audio::AudioBuffer samples(2, frames);
    for (unsigned i = 0; i < frames; ++i) {
        const float sample = .25f * std::sin(float(i) * .0576f);
        samples.getChannel(0)[i] = samples.getChannel(1)[i] = sample;
    }
    audio::AudioRecorder recorder;
    recorder.initialize(48000, 2);
    recorder.writeWAVFile(path.string(), samples, 48000);
}
void samplerRetry(const fs::path& root) {
    const auto sample = root / "sampler.wav";
    writeTone(sample, 4800);
    EngineController controller;
    controller.initialize(48000, 64, false);
    const auto track = controller.addTrack(TrackKind::Instrument, "Sampler");
    if (!check(controller.loadInstrumentSampler(track, sample.string()), "load sampler source for retry test")) return;
    auto snapshot = controller.prepareProjectSave();
    const auto originalStates = snapshot.pluginStates;
    const auto package = (root / "Retry").string();
    const auto manifest = fs::path(ProjectSerializer::manifestPath(package));
    fs::create_directories(manifest); // Cannot replace a directory with the manifest.
    check(!EngineController::writePreparedProject(snapshot, package), "manifest failure is returned after sampler packaging");
    check(snapshot.pluginStates.front().bytes == originalStates.front().bytes,
          "failed write keeps the original sampler source in the prepared snapshot");
    fs::remove(manifest);
    check(bool(EngineController::writePreparedProject(snapshot, package)), "same prepared sampler save can be retried");
    EngineController reopened;
    reopened.initialize(48000, 64, false);
    check(bool(reopened.openProject(package)), "retried sampler package opens with its embedded source");
}
void audioHistory(const fs::path& root, bool timed) {
    const auto package = root / "AudioHistory";
    const auto content = package / "Content";
    fs::create_directories(content);
    const auto active = content / "active.wav";
    const auto previous = content / "version-0.wav";
    writeTone(active, 4800);
    writeTone(previous, 96000);
    ProjectModel project;
    TrackModel track;
    track.id = "audio"; track.kind = TrackKind::Audio;
    ClipModel clip;
    clip.id = "audio-clip"; clip.kind = ClipKind::Audio;
    clip.filePath = active.string(); clip.durationSeconds = .1; clip.channels = 2;
    for (int i = 0; i < 24; ++i) {
        const auto file = content / ("version-" + std::to_string(i) + ".wav");
        if (i) fs::copy_file(previous, file);
        OfflineRenderVersion version;
        version.id = "version-" + std::to_string(i);
        if (i) version.parentId = "version-" + std::to_string(i - 1);
        version.source.filePath = file.string(); version.source.durationSeconds = 2;
        clip.offlineHistory.push_back(std::move(version));
    }
    track.clips.push_back(std::move(clip)); project.tracks.push_back(std::move(track));
    check(bool(ProjectSerializer::save(project, package.string())), "save audio with 24 previous render versions");
    const int rounds = timed ? 3 : 1;
    for (int pass = 0; pass < rounds; ++pass) {
        EngineController::PreparedProject prepared;
        const auto begin = std::chrono::steady_clock::now();
        const auto result = EngineController::prepareProjectOpen(package.string(), 48000, prepared);
        const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        check(bool(result) && prepared.document.tracks.front().clips.front().offlineHistory.size() == 24,
              "opening retains every previous audio version in the document");
        if (timed) std::printf("BENCH audio_history_open_ms=%.3f decoded=%zu pass=%d\n", elapsed, prepared.audio.size(), pass);
        else check(prepared.audio.size() == 1 && prepared.audio.front().path == active.string(),
                   "open decodes the active audio, deferring unused history versions");
    }
}
void persistence(RecoveryTestControl& control, const plugins::PluginDescriptor& descriptor,
                 const fs::path& root) {
    const auto package = (root / "Project").string();
    EngineController source;
    check(bool(source.initialize(48000, 64, false)), "initialize state test");
    const auto track = source.addTrack(TrackKind::Audio, "State");
    const auto slot = source.addInsert(track, descriptor);
    if (!check(!slot.empty(), "insert native CLAP fixture")) return;
    const auto parameter = source.insertParameters(track, slot).front().id;
    check(bool(source.saveProject(package)), "save initial native state");
    const auto manifest = ProjectSerializer::manifestPath(package);
    const auto before = read(manifest);
    control.failSave = true;
    check(!source.saveProject(package), "manual Save reports failed native state capture");
    check(read(manifest) == before, "failed capture leaves last manifest unchanged");
    const auto recovery = source.captureRecoverySnapshot();
    check(!recovery.pluginStates.empty(), "recovery retains last good state after native capture failure");
    control.failSave = false;

    source.setInsertParameter(track, slot, parameter, 1.25);
    check(bool(source.saveProject(package)), "save parameter edit before its first DSP block");
    EngineController reopened;
    reopened.initialize(48000, 64, false);
    check(bool(reopened.openProject(package)), "reopen native plugin project");
    process(reopened);
    check(std::abs(reopened.insertInstance(track, slot)->parameterValue(0) - 1.25) < 1e-6,
          "pending host edit survives state restore and processing");

    process(source);
    float preset = .875f;
    std::vector<std::uint8_t> bytes(sizeof(preset));
    std::memcpy(bytes.data(), &preset, sizeof(preset));
    check(source.insertInstance(track, slot)->loadState(bytes), "load preset without host parameter notification");
    source.refreshRecoveryPluginStates(std::numeric_limits<std::size_t>::max());
    const auto incremental = source.captureIncrementalRecoverySnapshot({}, true);
    const auto& values = incremental.trackParts.front()->inserts.front().parameters;
    const auto found = std::find_if(values.begin(), values.end(), [&](const auto& p) { return p.id == parameter; });
    check(found != values.end() && std::abs(found->value - preset) < 1e-6 && !found->restoreAfterState,
          "incremental recovery refreshes native values and retires stale overrides");

    check(bool(source.saveProject(package)), "save latest preset");
    auto state = fs::path(ProjectSerializer::statePath(package)) / source.insertModel(track, slot)->stateFile;
    const auto good = read(state);
    { std::ofstream damaged(state, std::ios::binary | std::ios::trunc); damaged << "bad"; }
    check(bool(source.saveProject(package)) && read(state) == good,
          "save repairs a damaged existing content-addressed state file");
    fs::remove(state);
    const auto identity = reopened.insertIdentity(track, slot);
    check(!reopened.openProject(package) && reopened.insertIdentity(track, slot) == identity,
          "missing named state rejects open without replacing the current plugin");

    auto incomplete = source.prepareProjectSave();
    incomplete.pluginStates.clear();
    const auto completeManifest = read(manifest);
    check(!EngineController::writePreparedProject(incomplete, package) && read(manifest) == completeManifest,
          "incomplete state snapshot cannot publish a dangling manifest");
    check(bool(source.saveProject(package)), "healthy Save succeeds after storage failures");
    check(source.setInsertChannelMode(track, slot, PluginChannelMode::DualMono) &&
          bool(source.saveProject(package)), "save both sides of a dual-mono plugin");
    check(source.setInsertChannelMode(track, slot, PluginChannelMode::Stereo) &&
          bool(source.saveProject(package)), "save after returning dual mono to stereo");
    check(bool(reopened.openProject(package)) && bool(reopened.saveProject((root / "StereoCopy").string())),
          "dormant right-side state survives reopening and Save As");
}
void automationRoundTrip() {
    ProjectModel project;
    TrackModel lane;
    lane.id = "automation"; lane.kind = TrackKind::Automation;
    for (int i = 0; i < 2; ++i) {
        ClipModel clip;
        clip.id = "placement-" + std::to_string(i); clip.contentId = "curve";
        clip.kind = ClipKind::Automation; clip.durationSeconds = 4;
        clip.startSeconds = i * 4.;
        clip.automation.target.kind = AutomationTargetKind::PluginParameter;
        clip.automation.target.channelId = "channel-" + std::to_string(i);
        clip.automation.target.slotId = "slot-" + std::to_string(i);
        clip.automation.target.parameterId = "gain-" + std::to_string(i);
        clip.automation.points = {{0, .25}, {4, .75}};
        lane.clips.push_back(std::move(clip));
    }
    project.tracks.push_back(std::move(lane));
    project.resolveClipContents();
    std::string bytes;
    ProjectModel loaded;
    if (!check(bool(ProjectSerializer::serializeDocument(project, bytes)) &&
               bool(ProjectSerializer::deserializeDocument(loaded, bytes)), "linked automation document round trip")) return;
    const auto& first = loaded.tracks.front().clips[0];
    const auto& second = loaded.tracks.front().clips[1];
    check(first.contentStorage() == second.contentStorage() && first.automation.points.size() == 2,
          "linked automation retains shared curve");
    check(first.automation.target.channelId == "channel-0" && second.automation.target.channelId == "channel-1" &&
          second.automation.target.slotId == "slot-1" && second.automation.target.parameterId == "gain-1",
          "each linked automation placement retains its own plugin target");
}
void benchmark(const fs::path& root) {
    using json = nlohmann::json;
    json notes = json::array(), clips = json::array();
    for (int i = 0; i < 2048; ++i)
        notes.push_back({{"id", "note-" + std::to_string(i)}, {"pitch", 60 + i % 12},
                        {"startBeats", i * .25}, {"lengthBeats", .125}, {"velocity", 100}});
    for (int i = 0; i < 48; ++i)
        clips.push_back({{"id", "clip-" + std::to_string(i)}, {"contentId", "phrase"}, {"kind", "midi"},
                        {"startSeconds", i * 256.}, {"durationSeconds", 256.}});
    json document{{"format", "vlt-project"}, {"version", ProjectSerializer::kFormatVersion},
                  {"clipContents", {{"phrase", {{"kind", "midi"}, {"notes", notes}}}}},
                  {"tracks", json::array({{{"id", "midi"}, {"kind", "midi"}, {"clips", clips}}})}};
    const auto bytes = document.dump();
    const auto package = root / "Benchmark";
    fs::create_directories(package);
    { std::ofstream file(ProjectSerializer::manifestPath(package.string())); file << bytes; }
    for (int pass = 0; pass < 3; ++pass) {
        EngineController::PreparedProject prepared;
        const auto begin = std::chrono::steady_clock::now();
        const auto result = EngineController::prepareProjectOpen(package.string(), 48000, prepared);
        const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        check(bool(result) && prepared.document.tracks.front().clips.back().notes.size() == 2048,
              "prepare 48 linked MIDI placements with 2048 notes each");
        std::printf("BENCH linked_project_open_ms=%.3f pass=%d\n", elapsed, pass);
    }
}
}
int main(int argc, char**) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto root = fs::temp_directory_path() / ("vlt-persistence-" + newUuid());
    fs::create_directories(root);
    if (argc > 1) { benchmark(root); audioHistory(root, true); }
    else {
#ifdef _WIN32
        const auto library = LoadLibraryA(DAW_TEST_RECOVERY_CLAP_PATH);
        const auto control = library ? reinterpret_cast<RecoveryTestControl*(*)()>(GetProcAddress(library, "recoveryTestControl")) : nullptr;
#else
        const auto library = dlopen(DAW_TEST_RECOVERY_CLAP_PATH, RTLD_NOW | RTLD_LOCAL);
        const auto control = library ? reinterpret_cast<RecoveryTestControl*(*)()>(dlsym(library, "recoveryTestControl")) : nullptr;
#endif
        if (!check(control != nullptr, "load persistence test fixture")) return 1;
        const auto descriptors = plugins::factoryFor(plugins::Format::Clap)->inspect(DAW_TEST_RECOVERY_CLAP_PATH);
        if (!check(!descriptors.empty(), "inspect persistence test fixture")) return 1;
        persistence(*control(), descriptors.front(), root);
        automationRoundTrip();
        samplerRetry(root);
        audioHistory(root, false);
    }
    std::error_code error;
    fs::remove_all(root, error);
    return failures ? 1 : 0;
}
