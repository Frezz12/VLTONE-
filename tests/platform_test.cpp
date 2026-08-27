// Phase-1 verification: the de-Appled paths — libsndfile decode, the
// WavAudioSource wrapper, and PortAudio device enumeration. Device *playback*
// is validated separately (it needs real hardware); this stays deterministic
// so it is safe to run headless / in CI.
#include "platform/AudioFileDecoder.hpp"
#include "Recording/RecordingEngine.hpp"
#include "Audio/SampleBuffer.hpp"
#include "Device/AudioDeviceManager.hpp"
#include "Core/AudioBuffer.hpp"

#include <sndfile.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace audio;

static int failures = 0;
static bool check(bool cond, const char* what) {
    std::printf("%s  %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) ++failures;
    return cond;
}

int main() {
    const auto dir = std::filesystem::temp_directory_path() / "daw-platform-test";
    std::filesystem::create_directories(dir);
    const auto path = (dir / "tone.wav").string();

    // ── Write a known stereo tone ──
    constexpr SampleRate kRate = 48000;
    constexpr BufferSize kFrames = 24000;
    AudioBuffer tone(2, kFrames);
    for (BufferSize f = 0; f < kFrames; ++f) {
        const float s = 0.5f * std::sin(2.0f * 3.14159265f * 440.0f * f / kRate);
        tone.getChannel(0)[f] = s;
        tone.getChannel(1)[f] = -s;
    }
    AudioRecorder recorder;
    recorder.initialize(kRate, 2);
    check(recorder.writeWAVFile(path, tone, kRate).isOk(), "writes a WAV file");

    // ── libsndfile decode ──
    platform::DecodedAudio decoded;
    check(platform::decodeAudioFile(path, decoded).isOk(),
          "libsndfile decodes the WAV");
    check(decoded.channels == 2, "decoded channel count is correct");
    check(std::llround(decoded.sampleRate) == 48000,
          "decoded sample rate is correct");
    check(decoded.frames == kFrames, "decoded frame count is correct");

    // Interleaved layout: L at even indices, R (negated) at odd. Spot-check a
    // frame away from the zero crossing.
    const size_t probe = 100;
    const float l = decoded.interleaved[probe * 2 + 0];
    const float r = decoded.interleaved[probe * 2 + 1];
    check(std::fabs(l + r) < 1e-4f, "decoded L and R are opposite as written");
    check(std::fabs(l) > 0.05f, "decoded audio is non-trivial");

    // ── Decoded audio becomes an engine SampleBuffer ──
    // This is the seam clips are built on: decode once, then every clip that
    // uses the file shares the planar buffer.
    auto samples = daw::engine::SampleBuffer::fromInterleaved(
        decoded.interleaved, daw::engine::ChannelCount(decoded.channels),
        daw::engine::FrameCount(decoded.frames), decoded.sampleRate);
    check(samples != nullptr, "sample buffer built from the decoder output");
    check(samples->channels() == 2 && samples->frames() == decoded.frames,
          "sample buffer keeps the file's geometry");
    check(samples->sampleRate() == decoded.sampleRate,
          "sample buffer keeps the file's rate");
    {
        float peak = 0.0f;
        const float* left = samples->channel(0);
        for (daw::engine::FrameCount i = 0; i < samples->frames(); ++i) {
            peak = std::max(peak, std::fabs(left[i]));
        }
        check(peak > 0.4f && peak < 0.6f, "sample buffer carries the tone");
    }
    check(audio::platform::isDecodableExtension("wav"), "accepts .wav");
    check(audio::platform::isDecodableExtension("flac"), "accepts .flac");
    check(audio::platform::isDecodableExtension("aiff"), "accepts .aiff");
    check(!audio::platform::isDecodableExtension("m4a"), "rejects .m4a (no AAC)");
    check(!audio::platform::isDecodableExtension("txt"), "rejects .txt");

    // ── The extension list every dialog, drop target and the browser share ──
    {
        const auto extensions = audio::platform::decodableExtensions();
        check(!extensions.empty(), "the decodable list is not empty");
        bool allAccepted = true;
        for (std::string_view e : extensions) {
            if (!audio::platform::isDecodableExtension(std::string(e)))
                allAccepted = false;
        }
        check(allAccepted, "every listed extension is accepted by the predicate");
    }

    // ── Metadata without a decode ──
    {
        audio::platform::AudioFileInfo info;
        check(audio::platform::probeAudioFile(path, info).isOk(),
              "probing a WAV succeeds");
        check(info.frames == kFrames && info.channels == 2 &&
                  std::llround(info.sampleRate) == 48000,
              "the probe reports the file's geometry");
        check(std::fabs(info.durationSeconds() - 0.5) < 1e-6,
              "and its duration");
        check(!audio::platform::probeAudioFile((dir / "nope.wav").string(), info)
                   .isOk(),
              "probing a file that is not there fails rather than guessing");
    }

    // ── The containers the extension list claims but nothing else exercises ──
    //
    // `caf` and `w64` are offered by the sampler's file dialog, so the shared
    // list has to include them — but only if libsndfile on this machine really
    // reads them. Written and decoded here rather than taken on trust.
    {
        const std::pair<const char*, int> kContainers[] = {
            {"probe.caf", SF_FORMAT_CAF | SF_FORMAT_PCM_16},
            {"probe.w64", SF_FORMAT_W64 | SF_FORMAT_PCM_16},
        };
        for (const auto& [name, format] : kContainers) {
            const std::string file = (dir / name).string();
            SF_INFO info{};
            info.samplerate = 48000;
            info.channels = 1;
            info.format = format;
            SNDFILE* handle = sf_open(file.c_str(), SFM_WRITE, &info);
            if (!check(handle != nullptr,
                       (std::string("libsndfile writes ") + name).c_str())) {
                continue;
            }
            std::vector<float> samples(1000, 0.25f);
            sf_write_float(handle, samples.data(), sf_count_t(samples.size()));
            sf_close(handle);

            audio::platform::DecodedAudio out;
            check(audio::platform::decodeAudioFile(file, out).isOk() &&
                      out.frames == 1000,
                  (std::string("and decodes ") + name +
                   " — so the shared list may offer it")
                      .c_str());
        }
    }

    // ── PortAudio device enumeration (no stream opened) ──
    AudioDeviceManager dm;
    const auto outputs = dm.enumerateOutputDevices();
    const auto inputs = dm.enumerateInputDevices();
    std::printf("INFO  PortAudio sees %zu output, %zu input device(s)\n",
                outputs.size(), inputs.size());
    check(true, "enumerating devices does not crash");

    std::filesystem::remove_all(dir);
    std::printf("\n%s\n", failures == 0 ? "ALL PASSED" : "FAILURES PRESENT");
    return failures;
}
