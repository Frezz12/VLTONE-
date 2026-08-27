#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <numbers>
#include <vector>

/// Sample-rate conversion good enough to render through.
///
/// The engine's playback path resamples clips once, when they are decoded, and
/// caches the result at the session rate. That made a cheap interpolator
/// tempting — until rendering gained a sample-rate choice, which sends every
/// sample in the session through this on the way to a 44.1 kHz master. A
/// four-point cubic measures about −24 dB of error at 12 kHz, which is audible
/// on cymbals; a windowed sinc is two orders of magnitude better and, since it
/// runs once per file rather than once per block, costs nothing that matters.
namespace daw::engine::dsp {

namespace resampler {

/// Filter half-width, in zero crossings. The tap count actually used grows as
/// `kHalfTaps / cutoff` when downsampling, because the kernel has to stretch in
/// time to lower its corner frequency.
inline constexpr int kHalfTaps = 24;
/// Table resolution per zero crossing. The kernel is read with linear
/// interpolation between entries, so this only has to be fine enough that the
/// interpolation error sits below the stopband.
inline constexpr int kStepsPerTap = 512;
/// Kaiser window parameter. Higher pushes the stopband down and widens the
/// transition band; picked by measurement, see `kCutoffScale`.
inline constexpr double kBeta = 10.0;
/// Where the passband ends, as a fraction of the lower of the two Nyquist
/// frequencies. The rest is the transition band the window needs. At 48 → 44.1
/// this puts the corner near 20.3 kHz.
inline constexpr double kCutoffScale = 0.92;
/// Above this many distinct phases the weight bank stops being worth its
/// memory and the kernel is evaluated per sample instead. Every pairing of the
/// rates a DAW offers reduces far below it — 48 kHz to 44.1 kHz needs 147.
inline constexpr long long kMaxPhases = 8192;

/// Modified Bessel function of the first kind, order zero — the Kaiser window's
/// only ingredient. Series form: the argument here never exceeds `kBeta`, where
/// it converges in a handful of terms.
inline double besselI0(double x) {
    double sum = 1.0;
    double term = 1.0;
    for (int k = 1; k < 64; ++k) {
        const double half = x / (2.0 * k);
        term *= half * half;
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

/// The windowed sinc, sampled once per process and shared by every conversion.
/// It is rate-independent: the cutoff is applied by scaling the lookup argument,
/// not by rebuilding the table.
inline const std::vector<float>& kernel() {
    static const std::vector<float> table = [] {
        std::vector<float> built(std::size_t(kHalfTaps) * kStepsPerTap + 2, 0.0f);
        const double norm = besselI0(kBeta);
        for (std::size_t i = 0; i < built.size(); ++i) {
            const double x = double(i) / kStepsPerTap;
            const double radius = x / kHalfTaps;
            if (radius >= 1.0) continue;
            const double pi = std::numbers::pi;
            const double sinc = x < 1e-12 ? 1.0 : std::sin(pi * x) / (pi * x);
            const double window =
                besselI0(kBeta * std::sqrt(1.0 - radius * radius)) / norm;
            built[i] = float(sinc * window);
        }
        return built;
    }();
    return table;
}

inline float kernelAt(double x) {
    const std::vector<float>& table = kernel();
    const double scaled = x * kStepsPerTap;
    const std::size_t index = std::size_t(scaled);
    if (index + 1 >= table.size()) return 0.0f;
    const float fraction = float(scaled - double(index));
    return table[index] + (table[index + 1] - table[index]) * fraction;
}

} // namespace resampler

/// How many frames `frames` becomes. Kept as its own function so callers size
/// buffers with exactly the arithmetic the conversion uses.
inline std::size_t resampledFrameCount(std::size_t frames, double sourceRate,
                                       double targetRate) {
    if (frames == 0 || sourceRate <= 0.0 || targetRate <= 0.0) return frames;
    const double step = sourceRate / targetRate;
    return std::size_t(std::max<double>(1.0, std::llround(double(frames) / step)));
}

/// Convert an interleaved buffer from one rate to another.
///
/// Polyphase in the literal sense: sample rates are integers, so the ratio is
/// rational and the fractional part of the read position takes only finitely
/// many values. 48 kHz to 44.1 kHz reduces to 160/147, so there are 147 distinct
/// phases and 147 sets of filter weights, each computed once. Evaluating the
/// kernel per output sample instead — which is what the obvious implementation
/// does — costs about eight times as much, and the kernel evaluation, not the
/// multiply-accumulate, is the bulk of the work.
///
/// Positions are stepped in exact integer arithmetic, so a long file cannot
/// accumulate the drift a repeatedly incremented double would.
///
/// Edges are handled by clamping to the first and last frame, the same as the
/// interpolator this replaces: a clip does not know what came before it, and
/// clamping is quieter than the ringing zero-padding would produce.
inline std::vector<float> resampleInterleaved(const std::vector<float>& input,
                                              std::size_t channels,
                                              std::size_t inputFrames,
                                              double sourceRate,
                                              double targetRate) {
    if (channels == 0 || inputFrames == 0) return input;
    if (sourceRate <= 0.0 || targetRate <= 0.0) return input;

    const std::size_t outputFrames =
        resampledFrameCount(inputFrames, sourceRate, targetRate);

    // Downsampling has to move the filter's corner down to the *output*
    // Nyquist, or everything above it folds back as aliasing. Upsampling needs
    // no such move: the input band already fits.
    const double cutoff =
        std::min(1.0, targetRate / sourceRate) * resampler::kCutoffScale;
    const int taps = int(std::ceil(double(resampler::kHalfTaps) / cutoff));
    const std::size_t width = std::size_t(2 * taps);

    // The rational form of the ratio. Device and file rates are integers in
    // practice; anything else falls back to computing weights per sample.
    const auto sourceHz = std::llround(sourceRate);
    const auto targetHz = std::llround(targetRate);
    const bool rational = std::fabs(sourceRate - double(sourceHz)) < 1e-6 &&
                          std::fabs(targetRate - double(targetHz)) < 1e-6 &&
                          sourceHz > 0 && targetHz > 0;
    long long step = 1;
    long long phases = 1;
    if (rational) {
        const long long divisor = std::gcd(sourceHz, targetHz);
        step = sourceHz / divisor;
        phases = targetHz / divisor;
    }
    const bool polyphase = rational && phases <= resampler::kMaxPhases;

    // One weight set per phase, already normalised. Normalising by the weights
    // actually used keeps unity gain at DC wherever the fractional position
    // lands, rather than letting it ripple with the phase.
    std::vector<float> bank;
    auto fillWeights = [&](double fraction, float* into) {
        double sum = 0.0;
        for (int j = -taps + 1; j <= taps; ++j) {
            const double distance = double(j) - fraction;
            const float weight =
                resampler::kernelAt(std::fabs(distance) * cutoff);
            into[std::size_t(j + taps - 1)] = weight;
            sum += weight;
        }
        const float scale = sum > 1e-12 ? float(1.0 / sum) : 0.0f;
        for (std::size_t i = 0; i < width; ++i) into[i] *= scale;
    };
    if (polyphase) {
        bank.resize(std::size_t(phases) * width);
        for (long long phase = 0; phase < phases; ++phase) {
            fillWeights(double(phase) / double(phases),
                        &bank[std::size_t(phase) * width]);
        }
    } else {
        bank.resize(width);
    }

    std::vector<float> output(outputFrames * channels, 0.0f);
    std::vector<double> accumulator(channels);
    const auto last = std::ptrdiff_t(inputFrames) - 1;

    for (std::size_t frame = 0; frame < outputFrames; ++frame) {
        std::ptrdiff_t base = 0;
        const float* weights = bank.data();
        if (polyphase) {
            const long long numerator = std::ptrdiff_t(frame) * step;
            base = std::ptrdiff_t(numerator / phases);
            weights = &bank[std::size_t(numerator % phases) * width];
        } else {
            const double position = double(frame) * (sourceRate / targetRate);
            base = std::ptrdiff_t(std::floor(position));
            fillWeights(position - double(base), bank.data());
        }

        const std::ptrdiff_t first = base - taps + 1;
        float* destination = &output[frame * channels];

        // The whole window is inside the buffer for all but the first and last
        // few dozen frames, so that case gets a straight loop with no clamping.
        // Mono and stereo are spelled out because the accumulator has to live in
        // registers: left as an indexed vector the compiler cannot prove it does
        // not alias the input, and reloads it on every tap — which measured as
        // most of the running time.
        if (first >= 0 && first + std::ptrdiff_t(width) <= last + 1) {
            const float* source = &input[std::size_t(first) * channels];
            if (channels == 1) {
                double sum = 0.0;
                for (std::size_t tap = 0; tap < width; ++tap) {
                    sum += double(source[tap]) * weights[tap];
                }
                destination[0] = float(sum);
                continue;
            }
            if (channels == 2) {
                double left = 0.0;
                double right = 0.0;
                for (std::size_t tap = 0; tap < width; ++tap) {
                    const float weight = weights[tap];
                    left += double(source[tap * 2]) * weight;
                    right += double(source[tap * 2 + 1]) * weight;
                }
                destination[0] = float(left);
                destination[1] = float(right);
                continue;
            }
            std::fill(accumulator.begin(), accumulator.end(), 0.0);
            for (std::size_t tap = 0; tap < width; ++tap) {
                const float weight = weights[tap];
                for (std::size_t channel = 0; channel < channels; ++channel) {
                    accumulator[channel] += double(source[channel]) * weight;
                }
                source += channels;
            }
        } else {
            std::fill(accumulator.begin(), accumulator.end(), 0.0);
            for (std::size_t tap = 0; tap < width; ++tap) {
                const float weight = weights[tap];
                const std::ptrdiff_t index = std::clamp<std::ptrdiff_t>(
                    first + std::ptrdiff_t(tap), 0, last);
                const float* source = &input[std::size_t(index) * channels];
                for (std::size_t channel = 0; channel < channels; ++channel) {
                    accumulator[channel] += double(source[channel]) * weight;
                }
            }
        }

        for (std::size_t channel = 0; channel < channels; ++channel) {
            destination[channel] = float(accumulator[channel]);
        }
    }
    return output;
}

} // namespace daw::engine::dsp
