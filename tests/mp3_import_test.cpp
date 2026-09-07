#include "EngineController.hpp"
#include "SampleLoader.hpp"
#include "platform/AudioFileDecoder.hpp"
#include "platform/PathUtils.hpp"
#include <sndfile.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

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
    const std::string path = DAW_TEST_MP3_PATH;
    // Independent decoder reference: clean EOF, not its preliminary estimate,
    // determines the sample count. The fixture must remain audible to the end.
    SF_INFO info{};
    SNDFILE* file = sf_open(path.c_str(), SFM_READ, &info);
    check(file != nullptr, "MP3 codec is available");
    std::vector<float> reference;
    std::vector<float> block(8192 * info.channels);
    sf_count_t count;
    while ((count = sf_readf_float(file, block.data(), 8192)) > 0)
        reference.insert(reference.end(), block.begin(), block.begin() + count * info.channels);
    check(sf_error(file) == SF_ERR_NO_ERROR, "fixture reaches clean EOF");
    sf_close(file);
    const auto frames = reference.size() / info.channels;
    check(frames == 89856, "fixture contains the full two-second MPEG stream");

    ap::DecodedAudio decoded;
    checkResult(ap::decodeAudioFile(path, decoded), "full MP3 decoding accepts clean early EOF");
    check(decoded.frames == frames && decoded.interleaved == reference,
          "interleaved output contains exactly the decoded audio, without a padded tail");
    std::shared_ptr<const daw::engine::SampleBuffer> sample;
    checkResult(daw::loadSampleBuffer(path, sample), "clip/sampler loader accepts the MP3");
    check(sample->frames() == frames, "planar sample uses the actual frame count");
    for (unsigned ch = 0; ch < sample->channels(); ++ch)
        for (unsigned frame = 0; frame < frames; ++frame)
            check(sample->channel(ch)[frame] == reference[frame * info.channels + ch],
                  "compacting the sample preserves each channel");
    std::shared_ptr<const daw::engine::SampleBuffer> cached;
    checkResult(daw::loadSampleBuffer(path, cached), "repeated MP3 load succeeds");
    check(cached == sample, "estimated length does not invalidate the decoded source cache");
    {
        // Catch overlapping plane compaction independently of MP3 stereo data.
        daw::engine::SampleBuffer planes(4, 20, 44100);
        for (unsigned ch = 0; ch < 4; ++ch)
            for (unsigned f = 0; f < 20; ++f) planes.writableChannel(ch)[f] = ch * 100 + f;
        planes.trimFrames(13);
        for (unsigned ch = 0; ch < 4; ++ch)
            for (unsigned f = 0; f < 13; ++f)
                check(planes.channel(ch)[f] == ch * 100 + f, "overlapping planes compact correctly");
    }
    ap::DecodeOptions limited;
    limited.maxBytes = 64;
    check(!ap::decodeAudioFile(path, decoded, limited), "MP3 still respects the memory budget");
    limited.maxBytes = 256 * 1024 * 1024;
    int iterations = 0;
    limited.keepGoing = [&] { return ++iterations < 3; };
    check(!ap::decodeAudioFile(path, decoded, limited), "MP3 decode remains cancellable");
    check(decoded.interleaved == reference, "failed decode does not publish partial audio");

    const auto directory = fs::temp_directory_path() / "vlt-mp3-import-test";
    fs::create_directories(directory);
    const auto invalid = (directory / "invalid.mp3").string();
    { std::ofstream out(invalid); out << "This is not MPEG audio."; }
    check(!ap::decodeAudioFile(invalid, decoded) && !daw::loadSampleBuffer(invalid, cached),
          "invalid MP3 is still rejected");

    // Tagged CBR output and WAV remain on their ordinary exact-length paths.
    for (const auto container : {ap::Container::Wav, ap::Container::Mp3}) {
        ap::WriteSpec spec;
        spec.container = container;
        spec.encoding = container == ap::Container::Wav ? ap::Encoding::Float32 : ap::Encoding::Mp3;
        spec.bitrateKbps = 192;
        const auto target = (directory / (std::string("roundtrip.") + std::string(ap::extensionFor(container)))).string();
        ap::AudioFileWriter writer;
        checkResult(writer.open(target, spec, 44100, 2, frames), "open roundtrip audio writer");
        const float* channels[] = {sample->channel(0), sample->channel(1)};
        checkResult(writer.write(channels, frames), "write roundtrip audio");
        checkResult(writer.close(), "finalize roundtrip audio");
        ap::DecodedAudio roundtrip;
        checkResult(ap::decodeAudioFile(target, roundtrip), "WAV and tagged CBR MP3 still decode");
        check(roundtrip.frames == frames && roundtrip.channels == 2, "roundtrip keeps audio geometry");
    }

    const auto package = (directory / "mp3-project.vlt").string();
    {
        daw::EngineController controller;
        checkResult(controller.initialize(48000, 512, false), "initialize offline import controller");
        daw::EngineController::PreparedAudio prepared;
        checkResult(daw::EngineController::prepareAudio(path, 48000, prepared), "prepare MP3 import and waveform");
        check(prepared.source->frames() == frames && controller.adoptPreparedAudio(std::move(prepared)),
              "adopt the prepared MP3");
        check(!controller.importAudioToNewTrack(path, 0).empty(), "import MP3 into the arrangement");
        checkResult(controller.saveProject(package), "save project containing original MP3 media");
    }
    {
        daw::EngineController::PreparedProject prepared;
        checkResult(daw::EngineController::prepareProjectOpen(package, 48000, prepared), "prepare existing MP3 project");
        check(prepared.failedPaths.empty() && prepared.audio.size() == 1 &&
              prepared.audio[0].source->frames() == frames,
              "project's MP3 is restored with full audio and no missing-media entry");
        daw::EngineController reopened;
        checkResult(reopened.initialize(48000, 512, false), "initialize offline reopen controller");
        checkResult(reopened.openPreparedProject(std::move(prepared)), "open the MP3 project");
    }
    fs::remove_all(directory);
    std::puts("mp3_import_test: PASS");
}
