#include "EngineController.hpp"
#include "MediaWorker.hpp"
#include "Internal/EqualizerInstance.hpp"
#include "Internal/SamplerInstance.hpp"
#include "Internal/SlicerInstance.hpp"
#include "platform/AudioFileDecoder.hpp"
#include "process_test_utils.hpp"
#include "Platform/PathUtils.hpp"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

namespace daw {
struct EngineControllerProcessTest {
    static AudioRuntimeEndpoint& endpoint(EngineController& controller) { return controller.m_runtime; }
    static std::string capturePath(EngineController& controller) {
        return controller.m_captures.empty() ? std::string{} : controller.m_captures.back().path;
    }
};
}
namespace {
using namespace daw;
int failures = 0;
bool check(bool value, const char* description) {
    std::printf("%s %s\n", value ? "PASS" : "FAIL", description); std::fflush(stdout);
    failures += !value; return value;
}
template<class F> bool eventually(F&& condition) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    do { if (condition()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    while (std::chrono::steady_clock::now() < until);
    return false;
}
}
int main(int argc, char** argv) try {
    using namespace daw;
    MediaWorker::install(PluginManager::helperPath("daw_worker"));
    // Read-only reproduction with the production process endpoint and the
    // user's plugin registry, never an audio device or project save. @file
    // accepts a UTF-8 path without the Windows console ANSI argv conversion.
    if (argc == 3 && std::string_view(argv[1]) == "--open-project") {
        std::string path = argv[2];
        if (path.starts_with('@')) {
            std::ifstream input(platform::pathFromUtf8(path.substr(1)), std::ios::binary);
            std::getline(input, path);
            if (!input || path.empty()) return 2;
            if (path.back() == '\r') path.pop_back();
        }
        auto package = platform::pathFromUtf8(path);
        if (std::filesystem::is_regular_file(package)) package = package.parent_path();
        EngineController controller;
        if (!check(bool(controller.initialize(44100, 512, false)), "headless production engine initialized")) return 1;
        controller.pluginManager().load();
        const auto started = std::chrono::steady_clock::now();
        const auto opened = controller.openProject(platform::pathToUtf8(package));
        std::printf("PROJECT_LOAD seconds=%.3f result=%s\n",
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(), opened.message().c_str());
        if (!check(bool(opened), "existing project opens without changing its files")) return 1;
        auto& endpoint = EngineControllerProcessTest::endpoint(controller);
        unsigned loaded = 0, failed = 0;
        for (const auto& address : endpoint.pluginAddresses()) {
            const auto state = endpoint.pluginStateSnapshot(address, false);
            loaded += state.exists && !state.failed;
            failed += !state.exists || state.failed;
            std::printf("PLUGIN loaded=%d failed=%d name=%s\n", state.exists, state.failed, state.descriptor.name.c_str());
        }
        unsigned expected = 0, unavailable = 0;
        const auto inspectSlot = [&](const std::string& channel, const InsertModel& slot) {
            if (!slot.isLoaded()) return;
            ++expected;
            const auto status = endpoint.pluginRuntimeStatus(channel, slot.id);
            unavailable += status.state == AudioPluginRuntimeState::Missing;
            if (status.state == AudioPluginRuntimeState::Missing || status.state == AudioPluginRuntimeState::Failed)
                std::printf("PLUGIN_UNAVAILABLE channel=%s name=%s reason=%s\n",
                    channel.c_str(), slot.name.c_str(), status.detail.c_str());
        };
        for (const auto& slot : controller.project().masterInserts) inspectSlot(AudioGraphSpec::masterChannelId, slot);
        for (const auto& track : controller.project().tracks) {
            inspectSlot(track.id, track.instrument);
            for (const auto& slot : track.inserts) inspectSlot(track.id, slot);
            for (const auto& slot : track.samplerFx.inserts) inspectSlot(track.id, slot);
            for (const auto& clip : track.clips) for (const auto& slot : clip.inserts) inspectSlot(track.id, slot);
        }
        std::printf("PROJECT tracks=%zu expected_slots=%u loaded_sides=%u failed_sides=%u unavailable_slots=%u\n",
            controller.project().tracks.size(), expected, loaded, failed, unavailable);
        check(endpoint.metadata().connected && !controller.isPlaying(), "loaded project remains connected and stopped");
        return failures ? 1 : 0;
    }
    const auto root = std::filesystem::temp_directory_path() /
        ("vlt-controller-process-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(root);
    struct Cleanup { std::filesystem::path root; ~Cleanup() { std::error_code e; std::filesystem::remove_all(root, e); } } cleanup{root};
    audio::AudioBuffer tone(2, 4800);
    std::fill_n(tone.getChannel(0), 4800, .125f); std::fill_n(tone.getChannel(1), 4800, -.25f);
    const auto source = (root / "source.wav").string();
    if (!check(bool(audio::AudioRecorder::writeWAVFile(source, tone, 48000)), "prepare source audio")) return 1;
    EngineController controller;
    auto& endpoint = EngineControllerProcessTest::endpoint(controller);
    if (!check(bool(controller.initialize(48000, 64, false)) && endpoint.isRemote() && endpoint.processId(),
        "ordinary controller starts an audio process without a local fallback")) return 1;
    controller.newProject();
    const auto track = controller.addTrack(TrackKind::Audio, "Recorded Audio");
    const auto clip = controller.importAudio(source, track, .2);
    const auto eq = controller.addInsert(track, plugins::equalizer::EqualizerInstance::staticDescriptor());
    if (!check(!track.empty() && !clip.empty() && !eq.empty(), "document edits create audio and plugins through the endpoint")) return 1;
    controller.setInsertParameter(track, eq, "output.gain", -6);
    check(bool(endpoint.advanceForTest(64, 4)) && bool(endpoint.captureRecoveryCheckpoint()), "plugin edits reach DSP and a confirmed recovery checkpoint");
    const auto identity = controller.insertIdentity(track, eq).instance;
    const auto instrument = controller.addTrack(TrackKind::Instrument, "Sampler");
    check(controller.setTrackInstrumentPlugin(instrument, plugins::sampler::SamplerInstance::staticDescriptor()), "instrument projection is published in the audio process");
    const auto sampler = controller.project().findTrack(instrument)->instrument.id;
    check(controller.loadSamplerSample(instrument, sampler, source), "sampler source uses the same process endpoint");
    controller.clearSamplerSample(instrument, sampler);
    check(endpoint.pluginStateSnapshot({instrument, sampler}).sample == nullptr,
          "immediate sample clearing does not depend on the UI polling cache");
    controller.undo();
    check(endpoint.pluginStateSnapshot({instrument, sampler}).sample != nullptr,
          "Undo retains the source before an immediate clear");
    controller.setInsertParameter(track, eq, "output.gain", -9);
    controller.commitInsertParameterEdit(track, eq, "output.gain", -6, "Gain");
    controller.undo();
    check(std::abs(endpoint.pluginParameter({track, eq}, "output.gain", AudioRuntimeEndpoint::Readout::Current) + 6) < 1e-6,
          "parameter Undo captures the current edit without waiting for UI refresh");
    const auto sliced = controller.addTrack(TrackKind::Instrument, "Slicer");
    check(controller.setTrackInstrumentPlugin(sliced, plugins::slicer::SlicerInstance::staticDescriptor()),
          "slicer is published in the audio process");
    const auto slicer = controller.project().findTrack(sliced)->instrument.id;
    check(controller.loadSlicerSample(sliced, slicer, source), "slicer editing reads the acknowledged source state");
    controller.clearSlicerSample(sliced, slicer);
    controller.undo();
    check(endpoint.pluginStateSnapshot({sliced, slicer}).sample != nullptr, "slicer source Undo survives immediate edits");
    controller.play();
    check(controller.isPlaying(), "controller observes acknowledged playback immediately");
    controller.pause();
    controller.setLoopRangeSeconds(.1, .3);
    check(bool(controller.setSampleRateHz(96000)) && controller.sampleRate() == 96000 &&
          controller.insertIdentity(track, eq).instance != identity, "sample-rate change rebuilds one complete generation");
    check(eventually([&] { return std::abs(endpoint.pluginParameter({track, eq}, "output.gain") + 6) < 1e-6; }) &&
          endpoint.pluginStateSnapshot({instrument, sampler}).sample != nullptr, "format replacement retains opaque plugin state and sampler PCM");
    const auto package = (root / "project").string();
    check(bool(controller.saveProject(package)), "remote project saves native state through owned values");
    controller.newProject();
    check(controller.project().tracks.empty() && bool(controller.openProject(package)), "project replacement and reopen use the sole process path");
    check(controller.project().findTrack(track) && endpoint.pluginStateSnapshot({instrument, sampler}).sample,
          "reopened project restores content and sampler source");
    const auto added = controller.addInsert(track, plugins::equalizer::EqualizerInstance::staticDescriptor());
    controller.undo();
    check(!endpoint.hasPlugin({track, added}), "Undo retires a remote plugin");
    controller.redo();
    check(endpoint.hasPlugin({track, added}), "Redo restores a remote plugin before publication");
    const auto output = (root / "mix.wav").string();
    check(bool(controller.exportMixdown(output)), "remote controller exports through the offline worker");
    audio::platform::DecodedAudio rendered;
    check(bool(audio::platform::decodeAudioFile(output, rendered)) && rendered.frames > 0 && rendered.sampleRate == 96000,
          "offline result has the current session format");
    {
        const auto slotsBefore = controller.project().findTrack(track)->inserts.size();
        const auto historyBefore = controller.undoDepth();
        std::shared_ptr<EngineController> draft;
        if (!check(bool(controller.createPluginBatchDraft({track, {}}, draft)),
                   "batch editor creates a secondary process session")) return 1;
        const auto effect = draft->addInsert(track, plugins::equalizer::EqualizerInstance::staticDescriptor());
        draft->setInsertParameter(track, effect, "output.gain", -4);
        EngineController::ChannelSnapshot batch;
        check(bool(draft->capturePluginBatchChain({track, {}}, {effect}, batch)) && !batch.inserts.empty(),
              "batch draft captures current plugin state through IPC");
        check(bool(controller.startPluginAudition(draft)) && bool(endpoint.advanceForTest(64, 4)),
              "primary process callback renders the secondary audition session");
        controller.stopPluginAudition();
        check(controller.project().findTrack(track)->inserts.size() == slotsBefore && controller.undoDepth() == historyBefore,
              "remote audition leaves the document and history unchanged");
        std::vector<std::vector<std::string>> inserted;
        const auto applied = controller.appendPluginBatch({{track, {}}, {instrument, {}}}, batch, inserted);
        if (!check(bool(applied) && inserted.size() == 2 && inserted[0].size() == 1 && inserted[1].size() == 1,
                   "batch settings publish to multiple channels through the process endpoint")) return 1;
        check(std::abs(endpoint.pluginParameter({track, inserted[0][0]}, "output.gain", AudioRuntimeEndpoint::Readout::Current) + 4) < 1e-6 &&
              std::abs(endpoint.pluginParameter({instrument, inserted[1][0]}, "output.gain", AudioRuntimeEndpoint::Readout::Current) + 4) < 1e-6,
              "remote batch restores the captured state on both independent instances");
        controller.undo();
        check(!endpoint.hasPlugin({track, inserted[0][0]}) && !endpoint.hasPlugin({instrument, inserted[1][0]}),
              "one Undo retires the whole remote batch");
    }
    const auto recording = controller.addTrack(TrackKind::Audio, "Input Recording");
    controller.setRecordDirectory((root / "captures").string());
    controller.setTrackInputChannel(recording, 0);
    controller.setTrackInputChannelCount(recording, 1);
    controller.setTrackInputEnabled(recording, true);
    controller.setLoopEnabled(false);
    controller.seekSeconds(0);
    const float input[]{.125f, -.375f};
    check(controller.startRecording(recording) && bool(endpoint.advanceForTest(64, 64, input)),
          "production controller records through the audio process");
    check(!controller.stopRecording().empty() && !controller.isRecording() &&
          controller.project().findTrack(recording)->clips.size() == 1,
          "normal recording finalization lands a clip in the document");
    controller.seekSeconds(2);
    check(controller.startRecording(recording) && bool(endpoint.advanceForTest(64, 1024, input)),
          "controller starts a second take before the crash");
    const auto interrupted = EngineControllerProcessTest::capturePath(controller);
    check(!interrupted.empty() && eventually([&] { return std::filesystem::file_size(interrupted) > 100000; }),
          "interrupted take has a durable disk prefix");
    const auto process = endpoint.processId();
    check(test::killChild(process) && eventually([&] { return !endpoint.metadata().connected; }),
          "whole-engine failure leaves the controller and document alive");
    bool rejected = false;
    try { controller.newProject(); } catch (const AudioEndpointError&) { rejected = true; }
    check(rejected && controller.canUndo() && controller.project().findTrack(track) && controller.project().findTrack(instrument),
          "failed project replacement preserves the previous document and history");
    check(!controller.recoverAudioDevice(), "controller cannot restart before finalizing the interrupted take");
    check(!controller.stopRecording().empty() && !controller.isRecording() &&
          controller.project().findTrack(recording)->clips.size() == 2,
          "crashed recording is repaired and landed without a live audio graph");
    check(bool(controller.recoverAudioDevice()) && endpoint.processId() != process &&
          !controller.isPlaying() && endpoint.pluginStateSnapshot({instrument, sampler}).sample,
          "controller recovers the complete document into a fresh stopped audio process");
    controller.shutdown();
    return failures ? 1 : 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL controller exception: %s\n", error.what()); return 1;
}
