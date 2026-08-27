// The windowed-sinc polyphase sample-rate converter.
//
// It sits on the clip-decode path, so a render at a rate the project does not
// run at sends every sample in the session through it. The numbers below are
// what justify its cost over an interpolator: a four-point cubic measures about
// -24 dB of error at 12 kHz, and this has to be two orders better.
#include "DSP/Resampler.hpp"

#include <cmath>
#include <cstdio>
#include <numbers>
#include <string>
#include <vector>

using namespace daw::engine::dsp;

static int failures = 0;
static bool check(bool cond, const std::string& what) {
    std::printf("%s  %s\n", cond ? "PASS" : "FAIL", what.c_str());
    if (!cond) ++failures;
    return cond;
}

static std::vector<float> tone(double hz, double rate, std::size_t frames,
                               float amplitude = 1.0f, double phase = 0.0) {
    std::vector<float> out(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        out[i] = amplitude *
                 float(std::sin(2.0 * std::numbers::pi * hz * double(i) / rate +
                                phase));
    }
    return out;
}

/// RMS error against the ideal tone at the target rate, in dB. The first and
/// last frames are skipped: the filter clamps at the edges by design, so they
/// are not expected to be exact.
static double passbandErrorDb(double hz, double from, double to) {
    const std::size_t frames = std::size_t(from);
    const std::vector<float> input = tone(hz, from, frames);
    const std::vector<float> output =
        resampleInterleaved(input, 1, frames, from, to);

    const std::size_t guard = 256;
    double error = 0.0;
    std::size_t counted = 0;
    for (std::size_t f = guard; f + guard < output.size(); ++f) {
        const double ideal =
            std::sin(2.0 * std::numbers::pi * hz * double(f) / to);
        error += (output[f] - ideal) * (output[f] - ideal);
        ++counted;
    }
    return 20.0 * std::log10(std::sqrt(error / double(counted)));
}

/// What survives of a full-scale tone placed above the target Nyquist, in dB
/// relative to that tone. Anything that comes through has aliased.
static double aliasResidualDb(double hz, double from, double to) {
    const std::size_t frames = std::size_t(from);
    const std::vector<float> input = tone(hz, from, frames);
    const std::vector<float> output =
        resampleInterleaved(input, 1, frames, from, to);

    const std::size_t guard = 512;
    double energy = 0.0;
    std::size_t counted = 0;
    for (std::size_t f = guard; f + guard < output.size(); ++f) {
        energy += double(output[f]) * output[f];
        ++counted;
    }
    return 20.0 * std::log10(std::sqrt(energy / double(counted)) / 0.70710678);
}

int main() {
    // ── Passband ──
    {
        struct Case {
            double hz;
            double ceilingDb;
        };
        // 15 kHz is the last frequency fully inside the passband; the corner
        // sits at 0.92 x 22050 Hz and the Kaiser transition starts below it.
        const Case cases[] = {{100.0, -100.0}, {1000.0, -100.0},
                              {5000.0, -100.0}, {10000.0, -100.0},
                              {12000.0, -100.0}, {15000.0, -100.0}};
        for (const Case& one : cases) {
            const double measured = passbandErrorDb(one.hz, 48000.0, 44100.0);
            std::printf("      48k->44.1k at %6.0f Hz: %7.2f dB\n", one.hz,
                        measured);
            check(measured < one.ceilingDb,
                  "48k->44.1k is clean at " + std::to_string(int(one.hz)) + " Hz");
        }
        for (double hz : {1000.0, 10000.0, 15000.0}) {
            const double measured = passbandErrorDb(hz, 44100.0, 48000.0);
            std::printf("      44.1k->48k at %6.0f Hz: %7.2f dB\n", hz, measured);
            check(measured < -100.0,
                  "44.1k->48k is clean at " + std::to_string(int(hz)) + " Hz");
        }
    }

    // ── Aliasing ──
    {
        // Well inside the stopband. Just above Nyquist is the transition band,
        // where partial rejection is the design, not a defect.
        const double near44 = aliasResidualDb(23000.0, 48000.0, 44100.0);
        std::printf("      23 kHz into 44.1k: %7.2f dB residual\n", near44);
        check(near44 < -90.0, "a tone above the target Nyquist does not alias");

        for (double hz : {25000.0, 40000.0}) {
            const double measured = aliasResidualDb(hz, 96000.0, 44100.0);
            std::printf("      %5.0f Hz 96k->44.1k: %7.2f dB residual\n", hz,
                        measured);
            check(measured < -90.0,
                  "96k->44.1k rejects " + std::to_string(int(hz)) + " Hz");
        }
    }

    // ── Frame counts ──
    {
        check(resampledFrameCount(48000, 48000.0, 44100.0) == 44100,
              "one second of 48k becomes one second of 44.1k");
        check(resampledFrameCount(44100, 44100.0, 48000.0) == 48000,
              "and the other way round");
        const std::vector<float> input(48000 * 2, 0.0f);
        const std::vector<float> output =
            resampleInterleaved(input, 2, 48000, 48000.0, 96000.0);
        check(output.size() == 96000 * 2,
              "a stereo buffer keeps its interleaving through a 2x conversion");
    }

    // ── Unity gain, edges included ──
    {
        // Constant in, constant out. This is what the per-phase weight
        // normalisation buys: without it the gain ripples with the phase, and
        // the clamped edges dip.
        const std::vector<float> flat(4800, 0.5f);
        const std::vector<float> output =
            resampleInterleaved(flat, 1, 4800, 48000.0, 44100.0);
        float lowest = 2.0f;
        float highest = -2.0f;
        for (float sample : output) {
            lowest = std::min(lowest, sample);
            highest = std::max(highest, sample);
        }
        std::printf("      DC 0.5 came back as %.6f … %.6f\n", lowest, highest);
        check(std::fabs(lowest - 0.5f) < 1e-4f && std::fabs(highest - 0.5f) < 1e-4f,
              "gain is unity at DC, at every phase and at the edges");
    }

    // ── Channels stay apart ──
    {
        const std::size_t frames = 24000;
        std::vector<float> stereo(frames * 2);
        const std::vector<float> left = tone(1000.0, 48000.0, frames);
        for (std::size_t f = 0; f < frames; ++f) {
            stereo[f * 2] = left[f];
            stereo[f * 2 + 1] = 0.0f;   // silence on the right
        }
        const std::vector<float> output =
            resampleInterleaved(stereo, 2, frames, 48000.0, 44100.0);
        float leftPeak = 0.0f;
        float rightPeak = 0.0f;
        for (std::size_t f = 0; f < output.size() / 2; ++f) {
            leftPeak = std::max(leftPeak, std::fabs(output[f * 2]));
            rightPeak = std::max(rightPeak, std::fabs(output[f * 2 + 1]));
        }
        check(leftPeak > 0.99f && rightPeak < 1e-6f,
              "a signal on one channel does not leak into the other");
    }

    // ── The fallback path ──
    {
        // A ratio whose reduced denominator fits the weight bank takes the
        // precomputed path; one that does not falls back to evaluating the
        // kernel per sample. 48000/44100 reduces to 160/147, so it is banked;
        // 48000 and 44099 are coprime, so 44099 phases blow past the limit and
        // the fallback runs. It is the same filter and has to measure the same.
        const double banked = passbandErrorDb(1000.0, 48000.0, 44100.0);
        const double fallback = passbandErrorDb(1000.0, 48000.0, 44099.0);
        std::printf("      banked %7.2f dB, per-sample fallback %7.2f dB\n",
                    banked, fallback);
        check(fallback < -100.0,
              "the per-sample fallback filters as well as the weight bank");
    }

    // ── Nothing silly on degenerate input ──
    {
        const std::vector<float> empty;
        check(resampleInterleaved(empty, 2, 0, 48000.0, 44100.0).empty(),
              "an empty buffer converts to an empty buffer");
        const std::vector<float> one(2, 0.25f);
        const std::vector<float> converted =
            resampleInterleaved(one, 2, 1, 48000.0, 44100.0);
        check(converted.size() == 2, "a single frame survives");
    }

    std::printf(failures == 0 ? "\nALL RESAMPLER TESTS PASSED\n"
                              : "\nFAILURES PRESENT\n");
    return failures == 0 ? 0 : 1;
}
