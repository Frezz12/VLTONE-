#include "MediaWorker.hpp"
#include "EngineController.hpp"
#include "SampleLoader.hpp"
#include "Recording/RecordingEngine.hpp"
#include "plugins/PluginManager.hpp"
#include "platform/PathUtils.hpp"
#include "SharedProcess.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {
int failures = 0;
bool check(bool value, const char* message) {
    std::printf("%s %s\n", value ? "PASS" : "FAIL", message); std::fflush(stdout);
    failures += !value; return value;
}
}
int main(int argc, char** argv) try {
    using namespace daw;
    if (argc > 1) {
        process::SharedProcess child;
        std::string error;
        if (!child.attach(argc, argv, error)) return 2;
        std::_Exit(71); // Test executable only; production workers have no fault switch.
    }
    const auto directory = std::filesystem::temp_directory_path() / ("vlt-media-test-" + newUuid());
    std::filesystem::create_directory(directory);
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code e; std::filesystem::remove_all(path, e); } } cleanup{directory};
    auto audio = std::make_shared<engine::SampleBuffer>(2, 96000, 48000);
    audio::AudioBuffer wave(2, 96000);
    for (unsigned frame = 0; frame < 96000; ++frame) for (unsigned ch = 0; ch < 2; ++ch) {
        const float sample = float(.2 * std::sin(frame * 440.0 * 6.283185307179586 / 48000));
        audio->writableChannel(ch)[frame] = wave.getChannel(ch)[frame] = sample;
    }
    const auto path = platform::pathToUtf8(directory / "source.wav");
    if (!check(bool(audio::AudioRecorder::writeWAVFile(path, wave, 48000)), "write media fixture")) return 1;
    const auto expectedPitch = analysis::detectSamplePitch(*audio, 0, audio->frames());
    slicer::SliceSettings settings; settings.mode = slicer::SliceMode::Grid; settings.targetCount = 8;
    const auto expectedSlices = slicer::cut(*audio, settings);
    const auto expectedTransients = analysis::detectWarpTransients(*audio, 0, 2);
    analysis::MusicalAnalysisRequest musical; musical.useNeuralModels = false;
    analysis::MusicalAnalysisResult expectedAnalysis;
    check(bool(analysis::analyzeAudioFile(path, musical, expectedAnalysis)), "capture native musical analysis reference");
    std::vector<std::pair<std::string, audio::platform::DecodedAudio>> compressed;
    for (const auto* name : {"mp3-vbr-estimated-length.mp3"
#if defined(_WIN32) || defined(__APPLE__)
                            , "aac-stereo.m4a", "aac-mono.aac"
#endif
         }) {
        const auto source = std::filesystem::path(DAW_TEST_AUDIO_FIXTURES) / name;
        const auto target = directory / platform::pathFromUtf8(std::string("Аудио ") + name);
        std::filesystem::copy_file(source, target);
        auto& [file, reference] = compressed.emplace_back();
        file = platform::pathToUtf8(target);
        if (!check(bool(audio::platform::decodeAudioFile(file, reference)), "capture native compressed audio reference")) return 1;
    }
    // Exercise an installed/staged release helper as well as the build-tree
    // worker; ordinary unit tests otherwise miss broken bundle load paths.
    const auto* packagedHelper = std::getenv("VLT_TEST_MEDIA_WORKER");
    const auto helper = packagedHelper && *packagedHelper
        ? std::string(packagedHelper) : PluginManager::helperPath("daw_worker");
    MediaWorker::install(helper);
    for (const auto& [file, reference] : compressed) {
        audio::platform::DecodedAudio actual;
        check(bool(audio::platform::decodeAudioFile(file, actual)) && actual.frames == reference.frames &&
            actual.channels == reference.channels && actual.sampleRate == reference.sampleRate &&
            actual.interleaved == reference.interleaved,
            "isolated compressed audio retains exact PCM and frame count on a Unicode path");
    }
    audio::platform::AudioFileInfo info;
    check(bool(audio::platform::probeAudioFile(path, info)) && info.frames == 96000 && info.channels == 2 && info.sampleRate == 48000,
        "metadata probing runs through the worker");
    audio::platform::DecodedAudio bounded;
    audio::platform::DecodeOptions budget;
    budget.maxBytes = 1024;
    check(!audio::platform::decodeAudioFile(path, bounded, budget) && bounded.interleaved.empty(),
        "decode budget is enforced before preparing an oversized source");
    std::shared_ptr<const engine::SampleBuffer> decoded;
    if (!check(bool(loadSampleBuffer(path, decoded)) && decoded && decoded->readOnly(),
        "worker publishes immutable mapped PCM")) return 1;
    bool exact = decoded->frames() == audio->frames() && decoded->channels() == audio->channels();
    for (unsigned ch = 0; ch < 2 && exact; ++ch)
        exact &= std::equal(audio->channel(ch), audio->channel(ch) + audio->frames(), decoded->channel(ch));
    check(exact, "PCM retains every sample after worker and temporary directory cleanup");
    auto cached = decoded;
    check(bool(loadSampleBuffer(path, cached)) && cached == decoded, "repeated source loads reuse immutable PCM");
    audio::platform::AudioFileReader reader;
    float frame[2]{};
    check(bool(reader.open(path)) && bool(reader.seek(1234)) && reader.read(frame, 1) == 1 &&
        frame[0] == audio->channel(0)[1234] && frame[1] == audio->channel(1)[1234],
        "reader facade seeks and interleaves the isolated source exactly");
    const auto actualPitch = analysis::detectSamplePitch(*decoded, 0, decoded->frames());
    check(actualPitch.status == expectedPitch.status && actualPitch.midiNote == expectedPitch.midiNote &&
        std::abs(actualPitch.frequencyHz - expectedPitch.frequencyHz) < 1e-9, "isolated pitch detection matches the native algorithm");
    const auto actualSlices = slicer::cut(*decoded, settings);
    check(actualSlices.count == expectedSlices.count && actualSlices.slices == expectedSlices.slices,
        "isolated slicing preserves exact boundaries and note mappings");
    const auto actualTransients = analysis::detectWarpTransients(*decoded, 0, 2);
    bool equalTransients = actualTransients.size() == expectedTransients.size();
    for (unsigned index = 0; equalTransients && index < actualTransients.size(); ++index)
        equalTransients &= actualTransients[index].sourceSeconds == expectedTransients[index].sourceSeconds &&
            actualTransients[index].strength == expectedTransients[index].strength;
    check(equalTransients, "isolated transient analysis matches the native algorithm");
    analysis::MusicalAnalysisResult actualAnalysis;
    check(bool(analysis::analyzeAudioFile(path, musical, actualAnalysis)) && actualAnalysis.key.root == expectedAnalysis.key.root &&
        actualAnalysis.key.scale == expectedAnalysis.key.scale && actualAnalysis.tempo.bpm == expectedAnalysis.tempo.bpm,
        "isolated tempo/key analysis matches the native algorithm");
    {
        EngineController controller;
        check(bool(controller.initialize(48000, 512, false)), "prepare timeline media fixture");
        controller.setRecordDirectory(directory.string());
        const auto track = controller.addTrack(TrackKind::Audio, "Capture");
        check(!controller.importAudio(path, track, 0).empty(), "worker-backed sample import creates a timeline clip");
        controller.seekSeconds(3);
        check(controller.startRecording(track), "start worker-backed take");
        controller.seedRecordingForShot(track, .1, [](double) { return .25f; });
        const auto captured = controller.stopRecording();
        const auto* recorded = controller.project().findTrack(track);
        check(!captured.empty() && controller.recordingWarning().empty() && recorded && recorded->clips.size() == 2,
            "Stop publishes the recorded WAV on the timeline through the packaged decoder");

        MediaWorker::install(helper + ".missing");
        controller.seekSeconds(4);
        check(controller.startRecording(track), "start take before decoder failure");
        controller.seedRecordingForShot(track, .1, [](double) { return .25f; });
        controller.stopRecording();
        check(controller.recordingWarning().find("recorded file could not be read") != std::string::npos,
            "decoder failure on Stop reports the retained WAV instead of silently dropping the take");
        MediaWorker::install(helper);
    }
    auto unchanged = decoded;
    check(!MediaWorker::decode(path, unchanged, [] { return false; }) && unchanged == decoded,
        "cancelled decode leaves the previous source unchanged");
    const auto broken = platform::pathToUtf8(directory / "broken.wav");
    { std::ofstream output(broken, std::ios::binary); output << "RIFFcorrupt"; }
    check(!MediaWorker::decode(broken, unchanged) && unchanged == decoded,
        "malformed media fails without replacing the previous source");
    MediaWorker::install(platform::pathToUtf8(std::filesystem::absolute(platform::pathFromUtf8(argv[0]))));
    check(!MediaWorker::decode(path, unchanged) && unchanged == decoded,
        "a crashed decoder process leaves the client and previous PCM intact");
    MediaWorker::install(helper + ".missing");
    check(!MediaWorker::decode(path, unchanged) && unchanged == decoded && !audio::platform::probeAudioFile(path, info),
        "missing helper reports failure without loading a codec in the caller");
    return failures ? 1 : 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL media exception: %s\n", error.what()); return 1;
}
