#include "EngineController.hpp"
#include "SampleLoader.hpp"
#include "platform/AudioFileDecoder.hpp"
#include "platform/PathUtils.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

namespace ap = audio::platform;
namespace fs = std::filesystem;
void check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
void checkResult(const audio::Result& result, const char* message) {
    if (!result) std::fprintf(stderr, "%s\n", result.message().c_str());
    check(result.isOk(), message);
}
int main() {
    const fs::path fixtures = DAW_TEST_AUDIO_FIXTURES;
    const auto directory = fs::temp_directory_path() / "vlt-m4a-import-test";
    fs::create_directories(directory);
    const auto source = (fixtures / "aac-stereo.m4a").string();
    ap::DecodedAudio reference;
    checkResult(ap::decodeAudioFile(source, reference), "decode stereo AAC in M4A");
    check(reference.channels == 2 && reference.sampleRate == 44100 &&
          std::abs(double(reference.frames) / reference.sampleRate - 1.25) < 0.06,
          "M4A retains rate, stereo channels and duration");
    float peak = 0;
    double difference = 0;
    for (std::size_t f = 0; f < reference.frames; ++f) {
        peak = std::max(peak, std::abs(reference.interleaved[f * 2]));
        difference += std::abs(reference.interleaved[f * 2] - reference.interleaved[f * 2 + 1]);
    }
    check(peak > 0.2 && peak < 0.5 && difference / reference.frames > 0.05,
          "decoded audio is audible and stereo channels stay distinct");
    for (const auto extension : {".m4a", ".mp4a", ".mp4", ".M4A", ".MP4A"}) {
        const auto target = directory / daw::platform::pathFromUtf8(std::string("Аудио тест ") + extension);
        fs::copy_file(source, target, fs::copy_options::overwrite_existing);
        const auto path = daw::platform::pathToUtf8(target);
        ap::AudioFileInfo info;
        checkResult(ap::probeAudioFile(path, info), "probe alias with Unicode path");
        check(info.channels == 2 && info.sampleRate == 44100, "probe matches decoded format");
        ap::DecodedAudio decoded;
        checkResult(ap::decodeAudioFile(path, decoded), "decode M4A/MP4A/MP4 and uppercase aliases");
        check(decoded.frames == reference.frames && decoded.interleaved == reference.interleaved,
              "extension aliases keep identical audio");
    }
    {
        ap::DecodedAudio mono;
        checkResult(ap::decodeAudioFile((fixtures / "aac-mono.aac").string(), mono), "decode standalone AAC");
        check(mono.channels == 1 && mono.sampleRate == 48000 &&
              std::abs(double(mono.frames) / mono.sampleRate - 0.75) < 0.06, "raw AAC keeps its geometry");
    }
    {
        ap::AudioFileReader reader;
        checkResult(reader.open(source), "open streaming native decoder");
        constexpr unsigned position = 17321;
        checkResult(reader.seek(position), "seek inside an AAC packet");
        std::vector<float> block(4096 * 2);
        check(reader.read(block.data(), 4096) == 4096, "read after seek");
        checkResult(reader.readStatus(), "native read reports success");
        double error = 0;
        for (unsigned i = 2048 * 2; i < block.size(); ++i)
            error = std::max(error, std::abs(double(block[i] - reference.interleaved[position * 2 + i])));
        check(error < 0.002, "seek returns audio at the requested frame");
        ap::AudioFileReader moved(std::move(reader));
        check(!reader.isOpen() && moved.isOpen(), "moving reader transfers native ownership");
        checkResult(moved.seek(0), "rewind native reader");
        check(moved.read(block.data(), 4096) == 4096, "read after rewind");
        moved.close();
        check(!moved.isOpen() && !moved.readStatus(), "closed reader is not usable");
    }
    {
        const auto invalid = (directory / "invalid.m4a").string();
        { std::ofstream out(invalid); out << "not an MP4 container"; }
        ap::DecodedAudio decoded = reference;
        check(!ap::decodeAudioFile(invalid, decoded), "invalid M4A is rejected");
        ap::DecodeOptions options;
        options.maxBytes = 64;
        check(!ap::decodeAudioFile(source, decoded, options), "native decode respects memory budget");
        options.maxBytes = 256 * 1024 * 1024;
        int polls = 0;
        options.keepGoing = [&] { return ++polls < 3; };
        check(!ap::decodeAudioFile(source, decoded, options), "native decode supports cancellation");
        check(decoded.interleaved == reference.interleaved, "failures do not publish partial audio");
    }
    const auto package = (directory / "aac-project.vlt").string();
    {
        daw::EngineController controller;
        checkResult(controller.initialize(48000, 512, false), "initialize offline controller");
        daw::EngineController::PreparedAudio prepared;
        audio::Result result = audio::Result::ok();
        std::thread worker([&] { result = daw::EngineController::prepareAudio(source, 48000, prepared); });
        worker.join();
        checkResult(result, "worker prepares AAC for imports/previews/sampler");
        check(prepared.source->frames() == reference.frames && controller.adoptPreparedAudio(std::move(prepared)),
              "publish prepared AAC sample and waveform");
        check(!controller.importAudioToNewTrack(source, 0).empty(), "import M4A to arrangement");
        checkResult(controller.saveProject(package), "save project with M4A media");
    }
    {
        daw::EngineController::PreparedProject prepared;
        checkResult(daw::EngineController::prepareProjectOpen(package, 48000, prepared), "reload AAC project");
        check(prepared.failedPaths.empty() && prepared.audio.size() == 1 &&
              prepared.audio[0].source->frames() == reference.frames, "M4A reloads with full audio");
        daw::EngineController controller;
        checkResult(controller.initialize(48000, 512, false), "initialize reopen controller");
        checkResult(controller.openPreparedProject(std::move(prepared)), "activate reopened AAC project");
    }
    fs::remove_all(directory);
    std::puts("m4a_import_test: PASS");
}
