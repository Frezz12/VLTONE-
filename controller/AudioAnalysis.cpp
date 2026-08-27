#include "AudioAnalysis.hpp"

#include <algorithm>
#include <cmath>
#include <memory>

namespace daw::analysis {

namespace {

constexpr double kPi = 3.14159265358979323846;
/// Where the bands are split. Low is "is it boomy", high is "is it bright";
/// everything a mix decision needs from three numbers.
constexpr double kLowHz = 200.0;
constexpr double kHighHz = 3000.0;
/// Anything this quiet is silence for the purposes of an answer.
constexpr double kSilenceFloor = 1e-5;
/// A sample that clips is worth saying so; just under full scale, because a
/// normalised file sits exactly at 1.0 and is not damaged.
constexpr float kClipLevel = 0.999f;

/// A second-order Butterworth section, from the usual bilinear-transform
/// cookbook.
///
/// One-pole filters were tried first and are not good enough here: at 3 kHz
/// they leave a 9 kHz tone reading as two-thirds mid-range, which would have
/// the assistant call a hi-hat mid-heavy and cut the wrong thing. A biquad
/// gives the textbook response for fifteen lines of arithmetic.
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;

    static Biquad lowpass(double hz, double sampleRate) {
        return make(hz, sampleRate, /*high=*/false);
    }
    static Biquad highpass(double hz, double sampleRate) {
        return make(hz, sampleRate, /*high=*/true);
    }

    double operator()(double x) {
        const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x;
        y2 = y1; y1 = y;
        return y;
    }

private:
    static Biquad make(double hz, double sampleRate, bool high) {
        Biquad f;
        if (sampleRate <= 0.0) return f;
        const double w0 = 2.0 * kPi * std::min(hz, sampleRate * 0.45) / sampleRate;
        const double cosW = std::cos(w0);
        const double alpha = std::sin(w0) / std::sqrt(2.0);   // Q = 1/√2
        const double a0 = 1.0 + alpha;
        f.a1 = -2.0 * cosW / a0;
        f.a2 = (1.0 - alpha) / a0;
        if (high) {
            f.b0 = (1.0 + cosW) / 2.0 / a0;
            f.b1 = -(1.0 + cosW) / a0;
        } else {
            f.b0 = (1.0 - cosW) / 2.0 / a0;
            f.b1 = (1.0 - cosW) / a0;
        }
        f.b2 = f.b0;
        return f;
    }
};

/// Autocorrelation pitch over one window. Returns 0 when nothing periodic is
/// found. Cheap and good enough to tell C from G, which is the question.
double detectPitch(const std::vector<float>& mono, double sampleRate,
                   double& confidence) {
    confidence = 0.0;
    if (mono.size() < 1024 || sampleRate <= 0.0) return 0.0;

    // 40 Hz to 1500 Hz covers a bass note to the top of a lead.
    const std::size_t minLag = std::size_t(sampleRate / 1500.0);
    const std::size_t maxLag =
        std::min(mono.size() / 2, std::size_t(sampleRate / 40.0));
    if (maxLag <= minLag + 2) return 0.0;

    double energy = 0.0;
    for (float v : mono) energy += double(v) * v;
    if (energy < kSilenceFloor) return 0.0;

    double best = 0.0;
    std::size_t bestLag = 0;
    for (std::size_t lag = minLag; lag < maxLag; ++lag) {
        double sum = 0.0;
        for (std::size_t i = 0; i + lag < mono.size(); ++i)
            sum += double(mono[i]) * mono[i + lag];
        if (sum > best) {
            best = sum;
            bestLag = lag;
        }
    }
    if (bestLag == 0) return 0.0;

    // The peak measured against the signal's own energy: a periodic signal
    // correlates with itself almost perfectly, noise barely at all.
    confidence = std::clamp(best / energy, 0.0, 1.0);
    return sampleRate / double(bestLag);
}

} // namespace

double toDb(double linear) {
    return linear > kSilenceFloor ? 20.0 * std::log10(linear) : -120.0;
}

Metrics measure(const float* const* channels, int channelCount,
                std::size_t frames, double sampleRate) {
    MetricsAccumulator accumulator(sampleRate, channelCount);
    accumulator.add(channels, frames);
    return accumulator.result();
}

struct MetricsAccumulator::Impl {
    explicit Impl(double rate, int count)
        : sampleRate(rate), channelCount(std::max(count, 0)),
          low(std::size_t(channelCount)), high(std::size_t(channelCount)) {
        for (int channel = 0; channel < channelCount; ++channel) {
            low[std::size_t(channel)] = Biquad::lowpass(kLowHz, sampleRate);
            high[std::size_t(channel)] = Biquad::highpass(kHighHz, sampleRate);
        }
    }

    double sampleRate = 0.0;
    int channelCount = 0;
    std::vector<Biquad> low;
    std::vector<Biquad> high;
    std::uint64_t frames = 0;
    double peak = 0.0;
    std::uint64_t clipped = 0;
    double sumSquares = 0.0;
    double lowEnergy = 0.0;
    double highEnergy = 0.0;
    double totalEnergy = 0.0;
};

MetricsAccumulator::MetricsAccumulator(double sampleRate, int channelCount)
    : m_impl(std::make_unique<Impl>(sampleRate, channelCount)) {}
MetricsAccumulator::~MetricsAccumulator() = default;
MetricsAccumulator::MetricsAccumulator(MetricsAccumulator&&) noexcept = default;
MetricsAccumulator& MetricsAccumulator::operator=(MetricsAccumulator&&) noexcept = default;

void MetricsAccumulator::add(const float* const* channels, std::size_t frames) {
    if (!m_impl || !channels || frames == 0 || m_impl->sampleRate <= 0.0 ||
        m_impl->channelCount <= 0) {
        return;
    }
    m_impl->frames += frames;
    for (int channel = 0; channel < m_impl->channelCount; ++channel) {
        const float* data = channels[channel];
        if (!data) continue;
        Biquad& low = m_impl->low[std::size_t(channel)];
        Biquad& high = m_impl->high[std::size_t(channel)];
        for (std::size_t i = 0; i < frames; ++i) {
            const double x = double(data[i]);
            const double magnitude = std::abs(x);
            m_impl->peak = std::max(m_impl->peak, magnitude);
            if (magnitude >= kClipLevel) ++m_impl->clipped;
            const double energy = x * x;
            m_impl->sumSquares += energy;
            m_impl->totalEnergy += energy;
            const double lowOut = low(x);
            const double highOut = high(x);
            m_impl->lowEnergy += lowOut * lowOut;
            m_impl->highEnergy += highOut * highOut;
        }
    }
}

Metrics MetricsAccumulator::result() const {
    Metrics out;
    if (!m_impl || m_impl->sampleRate <= 0.0 || m_impl->channelCount <= 0) return out;
    out.peak = m_impl->peak;
    out.clipped = m_impl->clipped;
    out.seconds = double(m_impl->frames) / m_impl->sampleRate;
    const double samples = double(m_impl->frames) * m_impl->channelCount;
    out.rms = samples > 0.0 ? std::sqrt(m_impl->sumSquares / samples) : 0.0;
    out.peakDb = toDb(out.peak);
    out.rmsDb = toDb(out.rms);
    out.silent = out.peak < kSilenceFloor;
    if (m_impl->totalEnergy > 0.0) {
        out.lowFraction =
            std::clamp(m_impl->lowEnergy / m_impl->totalEnergy, 0.0, 1.0);
        out.highFraction =
            std::clamp(m_impl->highEnergy / m_impl->totalEnergy, 0.0, 1.0);
        out.midFraction =
            std::clamp(1.0 - out.lowFraction - out.highFraction, 0.0, 1.0);
    }
    return out;
}

SampleTraits describe(const float* const* channels, int channelCount,
                      std::size_t frames, double sampleRate) {
    SampleTraits out;
    out.level = measure(channels, channelCount, frames, sampleRate);
    if (out.level.silent || frames == 0) {
        out.character = "silence";
        return out;
    }

    // Fold to mono once: everything below reads one signal.
    std::vector<float> mono(frames, 0.0f);
    for (int ch = 0; ch < channelCount; ++ch) {
        if (!channels[ch]) continue;
        for (std::size_t i = 0; i < frames; ++i) mono[i] += channels[ch][i];
    }
    const float scale = 1.0f / float(std::max(1, channelCount));
    for (float& v : mono) v *= scale;

    std::size_t peakIndex = 0;
    float peak = 0.0f;
    for (std::size_t i = 0; i < frames; ++i) {
        const float magnitude = std::abs(mono[i]);
        if (magnitude > peak) {
            peak = magnitude;
            peakIndex = i;
        }
    }
    out.attackSeconds = double(peakIndex) / sampleRate;

    // Pitch is looked for just after the attack, where a note is steady and a
    // drum has already collapsed into noise.
    const std::size_t from = std::min(frames - 1, peakIndex + std::size_t(sampleRate * 0.02));
    const std::size_t window =
        std::min(frames - from, std::size_t(sampleRate * 0.25));
    if (window > 1024) {
        std::vector<float> slice(mono.begin() + std::ptrdiff_t(from),
                                 mono.begin() + std::ptrdiff_t(from + window));
        out.pitchHz = detectPitch(slice, sampleRate, out.confidence);
    }
    // 0.6 is where a decaying note still reads as periodic and a snare does not.
    out.tonal = out.confidence > 0.6 && out.pitchHz > 0.0;
    if (out.tonal)
        out.pitch = int(std::lround(69.0 + 12.0 * std::log2(out.pitchHz / 440.0)));
    if (out.pitch < 0 || out.pitch > 127) {
        out.tonal = false;
        out.pitch = -1;
    }

    // A word for the numbers. Rough on purpose — it is a hint to be checked
    // against the file's name, not a classifier.
    const double seconds = out.level.seconds;
    const bool percussive = out.attackSeconds < 0.02;
    if (out.tonal && seconds > 1.5) out.character = "sustained tone";
    else if (out.tonal) out.character = "tonal one-shot";
    else if (seconds > 2.0) out.character = "loop or texture";
    else if (percussive && out.level.lowFraction > 0.6) out.character = "kick";
    else if (percussive && out.level.highFraction > 0.5) out.character = "hat or cymbal";
    else if (percussive) out.character = "snare or percussion";
    else out.character = "untuned one-shot";
    return out;
}

} // namespace daw::analysis
