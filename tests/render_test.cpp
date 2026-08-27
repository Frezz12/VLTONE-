// Offline render: formats, ranges, tails, stems, and the state the render
// borrows and has to give back.
//
// The load-bearing check is "stems sum to the mixdown". Stems are captured by
// leaf taps hung off the live graph during the very same pass that writes the
// mix, so if that identity ever stops holding, the taps have started measuring
// a different graph than the one being mixed.
#include "EngineController.hpp"
#include "Core/AudioBuffer.hpp"
#include "Recording/RecordingEngine.hpp"
#include "platform/AudioFileDecoder.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace ap = audio::platform;

static int failures = 0;
static bool check(bool cond, const std::string& what) {
    std::printf("%s  %s\n", cond ? "PASS" : "FAIL", what.c_str());
    if (!cond) ++failures;
    return cond;
}

// Generated in double throughout. In float the phase argument reaches a few
// thousand radians by the end of a one-second tone, where float's precision is
// already about 2e-4 — a drifting error that looks exactly like a resampler
// defect and swamps any measurement of one.
static void writeTone(const std::string& path, double rate, uint32_t frames,
                      double hz, float amplitude) {
    audio::AudioBuffer tone(2, frames);
    for (uint32_t f = 0; f < frames; ++f) {
        const float s = float(double(amplitude) *
                              std::sin(2.0 * 3.14159265358979 * hz * double(f) /
                                       rate));
        tone.getChannel(0)[f] = s;
        tone.getChannel(1)[f] = s;
    }
    audio::AudioRecorder rec;
    rec.initialize(rate, 2);
    rec.writeWAVFile(path, tone, rate);
}

static void writeConstant(const std::string& path, double rate, uint32_t frames,
                          float value) {
    audio::AudioBuffer flat(2, frames);
    for (uint32_t f = 0; f < frames; ++f) {
        flat.getChannel(0)[f] = value;
        flat.getChannel(1)[f] = value;
    }
    audio::AudioRecorder rec;
    rec.initialize(rate, 2);
    rec.writeWAVFile(path, flat, rate);
}

static bool decode(const std::string& path, ap::DecodedAudio& out) {
    return ap::decodeAudioFile(path, out).isOk();
}

static float peakOf(const ap::DecodedAudio& audio) {
    float peak = 0.0f;
    for (float sample : audio.interleaved) peak = std::max(peak, std::fabs(sample));
    return peak;
}

int main() {
    const fs::path dir = fs::temp_directory_path() / "daw_render_test";
    fs::remove_all(dir);
    fs::create_directories(dir);

    const double kRate = 48000.0;
    const uint32_t kFrames = 48000;  // one second
    const std::string toneA = (dir / "toneA.wav").string();
    const std::string toneB = (dir / "toneB.wav").string();
    writeTone(toneA, kRate, kFrames, 440.0, 0.5f);
    writeTone(toneB, kRate, kFrames, 660.0, 0.3f);

    // A project used by most of the cases below: two tracks, different levels
    // and pans, so a stem is distinguishable from the mix.
    auto buildProject = [&](daw::EngineController& c, std::string& a,
                            std::string& b) {
        c.initialize(kRate, 512, /*openDevice=*/false);
        a = c.addTrack(daw::TrackKind::Audio, "Bass");
        b = c.addTrack(daw::TrackKind::Audio, "Lead");
        c.importAudio(toneA, a, 0.0);
        c.importAudio(toneB, b, 0.0);
        c.setTrackVolume(a, 0.8f);
        c.setTrackVolume(b, 0.6f);
        c.setTrackPan(a, -0.5f);
        c.setTrackPan(b, 0.4f);
    };

    // ── Formats ────────────────────────────────────────────────────────────
    {
        daw::EngineController c;
        std::string a, b;
        buildProject(c, a, b);

        struct Case {
            ap::Container container;
            ap::Encoding encoding;
            const char* label;
        };
        const Case cases[] = {
            {ap::Container::Wav, ap::Encoding::Int16, "WAV 16-bit"},
            {ap::Container::Wav, ap::Encoding::Int24, "WAV 24-bit"},
            {ap::Container::Wav, ap::Encoding::Float32, "WAV float"},
            {ap::Container::Aiff, ap::Encoding::Int24, "AIFF 24-bit"},
            {ap::Container::Flac, ap::Encoding::Int24, "FLAC 24-bit"},
            {ap::Container::OggVorbis, ap::Encoding::Vorbis, "Ogg Vorbis"},
            {ap::Container::Mp3, ap::Encoding::Mp3, "MP3"},
        };

        for (const Case& one : cases) {
            daw::rendering::Spec spec;
            spec.outputDir = (dir / "formats").string();
            spec.baseName = one.label;
            spec.file.container = one.container;
            spec.file.encoding = one.encoding;
            spec.file.vbrQuality = 0.9;

            if (!ap::isWriteSpecSupported(spec.file, 2, kRate)) {
                std::printf("SKIP  %s (not in this libsndfile build)\n", one.label);
                continue;
            }
            daw::rendering::Report report;
            const bool ok = c.renderProject(spec, {}, report).isOk();
            if (!check(ok && report.files.size() == 1,
                       std::string(one.label) + " renders")) {
                continue;
            }
            ap::DecodedAudio decoded;
            check(decode(report.files.front(), decoded),
                  std::string(one.label) + " reads back");
            check(decoded.sampleRate == kRate && decoded.channels == 2,
                  std::string(one.label) + " has the right rate and channels");
            // Lossy codecs pad, so an exact frame count only holds for PCM.
            const bool lossless = one.container != ap::Container::OggVorbis &&
                                  one.container != ap::Container::Mp3;
            check(lossless ? decoded.frames == kFrames
                           : decoded.frames >= kFrames,
                  std::string(one.label) + " has the expected length");
            check(peakOf(decoded) > 0.2f,
                  std::string(one.label) + " carries signal");
        }
    }

    // ── Stems sum to the mixdown ───────────────────────────────────────────
    {
        daw::EngineController c;
        std::string a, b;
        buildProject(c, a, b);

        daw::rendering::Spec spec;
        spec.outputDir = (dir / "stems").string();
        spec.baseName = "song";
        spec.file.encoding = ap::Encoding::Float32;  // no quantisation to hide behind
        spec.writeMixdown = true;
        spec.stemChannelIds = {a, b};

        daw::rendering::Report report;
        check(c.renderProject(spec, {}, report).isOk(), "stem render succeeds");
        if (check(report.files.size() == 3, "one mixdown plus two stems")) {
            ap::DecodedAudio mix, stemA, stemB;
            check(decode(report.files[0], mix), "mixdown reads back");
            check(decode(report.files[1], stemA), "first stem reads back");
            check(decode(report.files[2], stemB), "second stem reads back");

            check(peakOf(stemA) > 0.1f && peakOf(stemB) > 0.1f,
                  "both stems carry signal");

            double worst = 0.0;
            const std::size_t n =
                std::min(mix.interleaved.size(),
                         std::min(stemA.interleaved.size(),
                                  stemB.interleaved.size()));
            for (std::size_t i = 0; i < n; ++i) {
                worst = std::max(worst,
                                 std::fabs(double(mix.interleaved[i]) -
                                           (stemA.interleaved[i] +
                                            stemB.interleaved[i])));
            }
            std::printf("      largest stem-sum error: %.3e\n", worst);
            check(n > 0 && worst < 1e-6, "stems sum back to the mixdown");
        }
    }

    // ── Pre-fader stems ignore the fader and the pan ────────────────────────
    {
        daw::EngineController c;
        std::string a, b;
        buildProject(c, a, b);
        c.setTrackVolume(a, 0.5f);
        c.setTrackPan(a, 0.0f);

        auto stemPeak = [&](bool preFader) {
            daw::rendering::Spec spec;
            spec.outputDir = (dir / (preFader ? "pre" : "post")).string();
            spec.baseName = "s";
            spec.file.encoding = ap::Encoding::Float32;
            spec.writeMixdown = false;
            spec.stemChannelIds = {a};
            spec.stemsPreFader = preFader;
            daw::rendering::Report report;
            if (!c.renderProject(spec, {}, report).isOk() || report.files.empty()) {
                return 0.0f;
            }
            ap::DecodedAudio decoded;
            return decode(report.files.front(), decoded) ? peakOf(decoded) : 0.0f;
        };

        const float post = stemPeak(false);
        const float pre = stemPeak(true);
        std::printf("      post-fader %.3f, pre-fader %.3f\n", post, pre);
        check(std::fabs(post - 0.25f) < 0.02f,
              "post-fader stem carries the fader (0.5 tone x 0.5 fader)");
        check(std::fabs(pre - 0.5f) < 0.02f,
              "pre-fader stem arrives at unity");
    }

    // ── Mute, and rendering as though nothing were muted ────────────────────
    {
        daw::EngineController c;
        std::string a, b;
        buildProject(c, a, b);
        c.setTrackMuted(a, true);

        auto peakWith = [&](bool ignoreMuteSolo, const char* folder) {
            daw::rendering::Spec spec;
            spec.outputDir = (dir / folder).string();
            spec.baseName = "m";
            spec.file.encoding = ap::Encoding::Float32;
            spec.writeMixdown = false;
            spec.stemChannelIds = {a};
            spec.ignoreMuteSolo = ignoreMuteSolo;
            daw::rendering::Report report;
            if (!c.renderProject(spec, {}, report).isOk() || report.files.empty()) {
                return 0.0f;
            }
            ap::DecodedAudio decoded;
            return decode(report.files.front(), decoded) ? peakOf(decoded) : 0.0f;
        };

        check(peakWith(false, "muted") < 0.001f, "a muted track renders silent");
        check(peakWith(true, "unmuted") > 0.2f,
              "ignoring mute brings the track back");
        check(c.project().findTrack(a)->muted,
              "the mute flag survives the render that ignored it");
    }

    // ── Range: the cycle region ────────────────────────────────────────────
    {
        daw::EngineController c;
        std::string a, b;
        buildProject(c, a, b);
        c.setLoopRangeSeconds(0.25, 0.75);

        daw::rendering::Spec spec;
        spec.outputDir = (dir / "cycle").string();
        spec.baseName = "loop";
        spec.range = daw::rendering::Range::CycleRegion;
        daw::rendering::Report report;
        check(c.renderProject(spec, {}, report).isOk(), "cycle-region render");
        ap::DecodedAudio decoded;
        if (!report.files.empty() && decode(report.files.front(), decoded)) {
            check(decoded.frames == uint32_t(0.5 * kRate),
                  "the cycle region renders exactly its own length");
        } else {
            check(false, "cycle-region render is readable");
        }
    }

    // ── Tail ───────────────────────────────────────────────────────────────
    {
        daw::EngineController c;
        std::string a, b;
        buildProject(c, a, b);

        auto frames = [&](daw::rendering::Tail tail,
                          const char* folder) -> uint64_t {
            daw::rendering::Spec spec;
            spec.outputDir = (dir / folder).string();
            spec.baseName = "t";
            spec.tail = tail;
            spec.tailSeconds = 1.5;
            spec.tailMaxSeconds = 4.0;
            spec.tailHoldSeconds = 0.2;
            daw::rendering::Report report;
            if (!c.renderProject(spec, {}, report).isOk() || report.files.empty()) {
                return uint64_t(0);
            }
            ap::DecodedAudio decoded;
            return decode(report.files.front(), decoded) ? uint64_t(decoded.frames)
                                                         : uint64_t(0);
        };

        check(frames(daw::rendering::Tail::None, "tail_none") == kFrames,
              "no tail stops at the end of the range");
        check(frames(daw::rendering::Tail::Fixed, "tail_fixed") ==
                  kFrames + uint32_t(1.5 * kRate),
              "a fixed tail adds exactly its own length");
        // Nothing rings in this project, so the decay test should end almost at
        // once — and well short of the ceiling.
        const uint64_t decayed = frames(daw::rendering::Tail::UntilSilence, "tail_decay");
        std::printf("      decay tail ran to %llu frames (range %u, ceiling %u)\n",
                    (unsigned long long)decayed, kFrames,
                    kFrames + uint32_t(4.0 * kRate));
        check(decayed >= kFrames && decayed < kFrames + uint32_t(1.0 * kRate),
              "a decay tail stops once the mix goes quiet");
    }

    // ── Bypassing the effects, and putting them back ────────────────────────
    {
        daw::EngineController c;
        std::string a, b;
        buildProject(c, a, b);
        // No plugin host in this test, so the observable part is the promise
        // that the flags are exactly as they were afterwards.
        daw::rendering::Spec spec;
        spec.outputDir = (dir / "dry").string();
        spec.baseName = "dry";
        spec.bypassChannelInserts = true;
        spec.bypassMasterChain = true;
        daw::rendering::Report report;
        check(c.renderProject(spec, {}, report).isOk(), "dry render succeeds");

        bool anyBypassed = false;
        for (const auto& track : c.project().tracks) {
            for (const auto& insert : track.inserts) anyBypassed |= insert.bypassed;
        }
        for (const auto& insert : c.project().masterInserts) {
            anyBypassed |= insert.bypassed;
        }
        check(!anyBypassed, "no insert is left bypassed after a dry render");
    }

    // ── Mono ───────────────────────────────────────────────────────────────
    {
        daw::EngineController c;
        std::string a, b;
        buildProject(c, a, b);
        daw::rendering::Spec spec;
        spec.outputDir = (dir / "mono").string();
        spec.baseName = "mono";
        spec.channels = daw::rendering::Channels::Mono;
        daw::rendering::Report report;
        check(c.renderProject(spec, {}, report).isOk(), "mono render succeeds");
        ap::DecodedAudio decoded;
        if (!report.files.empty() && decode(report.files.front(), decoded)) {
            check(decoded.channels == 1, "mono render has one channel");
            check(decoded.frames == kFrames, "mono render keeps its length");
        } else {
            check(false, "mono render is readable");
        }
    }

    // ── A different sample rate ────────────────────────────────────────────
    {
        daw::EngineController c;
        std::string a, b;
        buildProject(c, a, b);

        daw::rendering::Spec spec;
        spec.outputDir = (dir / "rate").string();
        spec.baseName = "at44";
        spec.sampleRate = 44100.0;
        daw::rendering::Report report;
        check(c.renderProject(spec, {}, report).isOk(), "44.1 kHz render succeeds");
        ap::DecodedAudio decoded;
        if (!report.files.empty() && decode(report.files.front(), decoded)) {
            check(decoded.sampleRate == 44100.0, "the file is at 44.1 kHz");
            check(std::fabs(double(decoded.frames) / decoded.sampleRate - 1.0) <
                      0.01,
                  "and still one second long");
            check(peakOf(decoded) > 0.2f, "and still carries signal");
        } else {
            check(false, "44.1 kHz render is readable");
        }
        check(std::fabs(c.sampleRate() - kRate) < 0.01,
              "the session is back at its own rate afterwards");

        // End to end, not just the DSP unit: a pure tone rendered from a 48 kHz
        // project to 44.1 kHz has to come back a pure tone. This is the check
        // that catches the whole chain — decode, resample, graph, encode —
        // shifting the audio rather than merely reporting the right header.
        {
            daw::EngineController pure;
            pure.initialize(kRate, 512, /*openDevice=*/false);
            const std::string t = pure.addTrack(daw::TrackKind::Audio, "Tone");
            pure.importAudio(toneA, t, 0.0);

            daw::rendering::Spec clean;
            clean.outputDir = (dir / "purity").string();
            clean.baseName = "tone44";
            clean.sampleRate = 44100.0;
            clean.file.encoding = ap::Encoding::Float32;
            daw::rendering::Report pureReport;
            check(pure.renderProject(clean, {}, pureReport).isOk(),
                  "a tone renders at 44.1 kHz");

            ap::DecodedAudio got;
            if (!pureReport.files.empty() && decode(pureReport.files.front(), got)) {
                // toneA is a 440 Hz sine at amplitude 0.5, panned centre through
                // a unity fader, so the balance law passes it at unity.
                const std::size_t guard = 2048;
                double error = 0.0;
                double reference = 0.0;
                std::size_t counted = 0;
                for (std::size_t f = guard; f + guard < got.frames; ++f) {
                    const double ideal =
                        0.5 * std::sin(2.0 * 3.14159265358979 * 440.0 *
                                       double(f) / got.sampleRate);
                    const double actual = got.interleaved[f * got.channels];
                    error += (actual - ideal) * (actual - ideal);
                    reference += ideal * ideal;
                    ++counted;
                }
                const double snr =
                    counted > 0 && error > 0.0
                        ? 10.0 * std::log10(reference / error)
                        : 999.0;
                std::printf("      440 Hz rendered 48k->44.1k: %.1f dB SNR\n", snr);
                // Measured at 111 dB, flat across the file. The four-point
                // cubic this replaced managed about 35 dB. The threshold sits
                // well above anything an interpolator can reach, so a
                // regression to one cannot slip through, while leaving room for
                // the clamped filter edges the guard does not fully exclude.
                check(snr > 100.0,
                      "the rate conversion is transparent end to end");
            } else {
                check(false, "the 44.1 kHz tone is readable");
            }
        }

        // The clip caches were dropped for the rate change; the session has to
        // still render at its own rate afterwards.
        daw::rendering::Spec again;
        again.outputDir = (dir / "rate_back").string();
        again.baseName = "back";
        daw::rendering::Report backReport;
        check(c.renderProject(again, {}, backReport).isOk(),
              "the project still renders after a rate round-trip");
        ap::DecodedAudio back;
        if (!backReport.files.empty() && decode(backReport.files.front(), back)) {
            check(back.sampleRate == kRate && back.frames == kFrames,
                  "and comes back at the original rate and length");
            check(peakOf(back) > 0.2f, "and is not silent");
        } else {
            check(false, "the follow-up render is readable");
        }
    }

    // ── Cancelling ─────────────────────────────────────────────────────────
    {
        daw::EngineController c;
        std::string a, b;
        buildProject(c, a, b);

        daw::rendering::Spec spec;
        spec.outputDir = (dir / "cancel").string();
        spec.baseName = "gone";
        spec.stemChannelIds = {a, b};
        daw::rendering::Report report;
        const bool ok = c.renderProject(
                             spec,
                             [](const daw::rendering::Progress& p) {
                                 return p.fraction < 0.3;
                             },
                             report)
                            .isOk();
        check(ok, "a cancelled render is not an error");
        check(report.cancelled, "and says it was cancelled");
        check(report.files.empty(), "and reports no files");
        std::size_t left = 0;
        if (fs::exists(spec.outputDir)) {
            for (const auto& entry : fs::directory_iterator(spec.outputDir)) {
                (void)entry;
                ++left;
            }
        }
        check(left == 0, "and leaves nothing half-written on disk");
    }

    // ── Refusals ───────────────────────────────────────────────────────────
    {
        daw::EngineController c;
        std::string a, b;
        buildProject(c, a, b);

        daw::rendering::Spec nothing;
        nothing.outputDir = (dir / "refuse").string();
        nothing.writeMixdown = false;
        daw::rendering::Report report;
        check(!c.renderProject(nothing, {}, report).isOk(),
              "rendering nothing is refused");

        daw::rendering::Spec empty;
        empty.outputDir = (dir / "refuse").string();
        empty.range = daw::rendering::Range::Custom;
        empty.customStartSeconds = 2.0;
        empty.customEndSeconds = 2.0;
        check(!c.renderProject(empty, {}, report).isOk(),
              "an empty range is refused");

        daw::rendering::Spec opus;
        opus.outputDir = (dir / "refuse").string();
        opus.file.container = ap::Container::Opus;
        opus.file.encoding = ap::Encoding::Opus;
        opus.sampleRate = 44100.0;
        check(!c.renderProject(opus, {}, report).isOk(),
              "Opus at a rate it cannot encode is refused up front");
    }

    // ── The click stays out of the file ────────────────────────────────────
    {
        // MetronomeNode gates its click on `context.playing`, which an offline
        // pass asserts, so this only holds because the render leaves it out of
        // the graph entirely.
        daw::EngineController c;
        std::string a, b;
        buildProject(c, a, b);

        auto renderTo = [&](const char* folder) {
            daw::rendering::Spec spec;
            spec.outputDir = (dir / folder).string();
            spec.baseName = "click";
            spec.file.encoding = ap::Encoding::Float32;
            daw::rendering::Report report;
            c.renderProject(spec, {}, report);
            ap::DecodedAudio got;
            return (!report.files.empty() && decode(report.files.front(), got))
                       ? got
                       : ap::DecodedAudio{};
        };

        c.setMetronomeEnabled(false);
        const ap::DecodedAudio quiet = renderTo("click_off");
        c.setMetronomeEnabled(true);
        const ap::DecodedAudio clicking = renderTo("click_on");

        double worst = 0.0;
        const std::size_t n = std::min(quiet.interleaved.size(),
                                       clicking.interleaved.size());
        for (std::size_t i = 0; i < n; ++i) {
            worst = std::max(worst, std::fabs(double(quiet.interleaved[i]) -
                                              clicking.interleaved[i]));
        }
        std::printf("      metronome on vs off: %.6f\n", worst);
        check(n > 0 && worst < 1e-6, "an enabled metronome does not reach the file");
        check(c.isMetronomeEnabled(), "and is still enabled afterwards");
    }

    // ── Latency compensation ───────────────────────────────────────────────
    {
        // Without compensation a graph that reports latency shifts the whole
        // render late by exactly that much and truncates the end. There is no
        // latency-reporting plugin in this test, so the observable guarantee is
        // that the range still renders to its exact length and starts on time;
        // the shift itself is covered where a latency node can be built, in
        // engine_graph_test.
        daw::EngineController c;
        std::string a, b;
        buildProject(c, a, b);
        daw::rendering::Spec spec;
        spec.outputDir = (dir / "aligned").string();
        spec.baseName = "aligned";
        spec.file.encoding = ap::Encoding::Float32;
        daw::rendering::Report report;
        check(c.renderProject(spec, {}, report).isOk(), "aligned render succeeds");
        ap::DecodedAudio got;
        if (!report.files.empty() && decode(report.files.front(), got)) {
            check(got.frames == kFrames, "the file is exactly the range length");
            check(std::fabs(got.interleaved[0]) < 0.01f &&
                      std::fabs(got.interleaved[200 * got.channels]) > 0.05f,
                  "and starts at the top of the range, not later");
        } else {
            check(false, "aligned render is readable");
        }
    }

    // ── Pre-roll ───────────────────────────────────────────────────────────
    {
        daw::EngineController c;
        std::string a, b;
        buildProject(c, a, b);

        auto framesWith = [&](double preRoll, const char* folder) {
            daw::rendering::Spec spec;
            spec.outputDir = (dir / folder).string();
            spec.baseName = "pre";
            spec.file.encoding = ap::Encoding::Float32;
            spec.range = daw::rendering::Range::Custom;
            spec.customStartSeconds = 0.5;
            spec.customEndSeconds = 1.0;
            spec.preRollSeconds = preRoll;
            daw::rendering::Report report;
            if (!c.renderProject(spec, {}, report).isOk() || report.files.empty()) {
                return ap::DecodedAudio{};
            }
            ap::DecodedAudio got;
            return decode(report.files.front(), got) ? got : ap::DecodedAudio{};
        };

        const ap::DecodedAudio none = framesWith(0.0, "preroll_none");
        const ap::DecodedAudio rolled = framesWith(0.25, "preroll_quarter");
        check(none.frames == uint32_t(0.5 * kRate),
              "a mid-project range renders its own length");
        check(rolled.frames == none.frames,
              "and pre-roll does not change that length");
        // Nothing in this project rings, so the two renders must be identical:
        // pre-roll adds state, never content.
        double worst = 0.0;
        const std::size_t n = std::min(none.interleaved.size(),
                                       rolled.interleaved.size());
        for (std::size_t i = 0; i < n; ++i) {
            worst = std::max(worst, std::fabs(double(none.interleaved[i]) -
                                              rolled.interleaved[i]));
        }
        std::printf("      pre-roll changed the audio by %.3e\n", worst);
        check(n > 0 && worst < 1e-6,
              "pre-roll leaves a project with no tails bit-identical");
    }

    // ── Dither ─────────────────────────────────────────────────────────────
    {
        // The point of dither is not that a quiet signal survives — libsndfile
        // rounds rather than truncates, so a sub-LSB sine survives as a hard
        // square at plus or minus one LSB, which is the distortion dither
        // exists to prevent. The point is that resolution below the LSB is
        // recovered *on average*. A constant at 0.3 LSB shows it plainly:
        // rounded on its own it becomes exactly zero, dithered its mean comes
        // back where it started.
        const float lsb = 1.0f / 32768.0f;
        const float level = 0.3f * lsb;
        daw::EngineController c;
        c.initialize(kRate, 512, /*openDevice=*/false);
        const std::string t = c.addTrack(daw::TrackKind::Audio, "Faint");
        const std::string faint = (dir / "faint.wav").string();
        writeConstant(faint, kRate, kFrames, level);
        c.importAudio(faint, t, 0.0);

        auto meanAt16 = [&](bool dither, const char* folder) {
            daw::rendering::Spec spec;
            spec.outputDir = (dir / folder).string();
            spec.baseName = "d";
            spec.file.encoding = ap::Encoding::Int16;
            spec.file.dither = dither;
            daw::rendering::Report report;
            if (!c.renderProject(spec, {}, report).isOk() || report.files.empty()) {
                return 0.0;
            }
            ap::DecodedAudio got;
            if (!decode(report.files.front(), got)) return 0.0;
            double sum = 0.0;
            for (float sample : got.interleaved) sum += sample;
            return got.interleaved.empty()
                       ? 0.0
                       : sum / double(got.interleaved.size());
        };

        const double plain = meanAt16(false, "dither_off");
        const double dithered = meanAt16(true, "dither_on");
        std::printf("      0.3 LSB constant -> rounded %.3f LSB, dithered %.3f LSB\n",
                    plain / lsb, dithered / lsb);
        check(std::fabs(plain) < 0.05 * lsb,
              "rounding alone loses a signal below one LSB completely");
        check(std::fabs(dithered - level) < 0.1 * lsb,
              "dither recovers it on average, which is the whole point");

        // Rounding, separately from dither. libsndfile's own float-to-integer
        // path floors — a level of -0.4 LSB lands on -1 — which puts a
        // systematic half-LSB offset on every integer export. The writer
        // quantises itself to avoid that, and this is what holds it to it.
        auto meanOfConstant = [&](float value, const char* folder) {
            daw::EngineController q;
            q.initialize(kRate, 512, /*openDevice=*/false);
            const std::string tr = q.addTrack(daw::TrackKind::Audio, "Flat");
            const std::string file =
                (dir / (std::string(folder) + ".wav")).string();
            writeConstant(file, kRate, 4800, value);
            q.importAudio(file, tr, 0.0);
            daw::rendering::Spec spec;
            spec.outputDir = (dir / folder).string();
            spec.baseName = "r";
            spec.file.encoding = ap::Encoding::Int16;
            daw::rendering::Report report;
            if (!q.renderProject(spec, {}, report).isOk() || report.files.empty()) {
                return 999.0;
            }
            ap::DecodedAudio got;
            if (!decode(report.files.front(), got)) return 999.0;
            double sum = 0.0;
            for (float sample : got.interleaved) sum += sample;
            return got.interleaved.empty()
                       ? 999.0
                       : sum / double(got.interleaved.size()) / lsb;
        };

        const double up = meanOfConstant(0.6f * lsb, "round_up");
        const double down = meanOfConstant(-0.4f * lsb, "round_down");
        std::printf("      +0.6 LSB -> %.2f, -0.4 LSB -> %.2f\n", up, down);
        check(std::fabs(up - 1.0) < 0.05, "+0.6 LSB rounds up to 1, not down to 0");
        check(std::fabs(down) < 0.05, "-0.4 LSB rounds to 0, not down to -1");
    }

    // ── Metadata ───────────────────────────────────────────────────────────
    {
        daw::EngineController c;
        std::string a, b;
        buildProject(c, a, b);
        daw::rendering::Spec spec;
        spec.outputDir = (dir / "tags").string();
        spec.baseName = "tagged";
        spec.tags.artist = "Test Artist";
        spec.tags.comment = "rendered by the test";
        daw::rendering::Report report;
        check(c.renderProject(spec, {}, report).isOk(), "tagged render succeeds");
        if (!report.files.empty()) {
            ap::AudioFileInfo info{};
            check(ap::probeAudioFile(report.files.front(), info).isOk(),
                  "a tagged file is still a readable audio file");
            check(info.frames == kFrames, "and holds the audio it should");
        } else {
            check(false, "tagged render produced a file");
        }
    }

    std::printf(failures == 0 ? "\nALL RENDER TESTS PASSED\n"
                              : "\nFAILURES PRESENT\n");
    return failures == 0 ? 0 : 1;
}
