// SPDX-License-Identifier: MIT
#include <vlt/spectre/Analyzer.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <limits>

namespace vlt::spectre {
namespace {
constexpr double pi = 3.14159265358979323846;
constexpr std::uint32_t dirtyBit = 4;
constexpr std::uint32_t indexMask = 3;
static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
              "Spectre requires lock-free 32-bit atomics for audio capture");

struct StereoSample { float left = 0.0f, right = 0.0f; };

float cleanSample(float value) noexcept {
    return std::isfinite(value) ? value : 0.0f;
}

double responseCoefficient(double elapsedSeconds, double timeMs) noexcept {
    return timeMs > 0.0 ? std::exp(-elapsedSeconds * 1000.0 / timeMs) : 0.0;
}

bool validBallistics(const Ballistics& b) noexcept {
    return std::isfinite(b.attackMs) && b.attackMs >= 0.0 && b.attackMs <= 10000.0 &&
           std::isfinite(b.releaseMs) && b.releaseMs >= 0.0 && b.releaseMs <= 10000.0 &&
           std::isfinite(b.peakHoldMs) && b.peakHoldMs >= 0.0 && b.peakHoldMs <= 60000.0 &&
           std::isfinite(b.peakFallDbPerSecond) && b.peakFallDbPerSecond >= 0.0 &&
           b.peakFallDbPerSecond <= 1000.0;
}
} // namespace

struct Analyzer::State {
    struct CapturedWindow {
        std::vector<StereoSample> samples;
        std::uint64_t position = 0;
        std::uint64_t generation = 0;
        bool ready = false;
    };
    struct DisplayBin {
        double fractionalBin = 0.0;
        std::size_t first = 0, last = 0;
        bool inRange = false;
        bool hasBins = false;
    };

    AnalyzerSettings settings;
    double sampleRate;

    // Producer-owned history and counters. Each hop copies a complete, chronological
    // window into its private back buffer. This is O(fftSize), but only a memory copy.
    std::vector<StereoSample> history;
    std::size_t writePosition = 0, filled = 0, untilPublish;
    std::uint64_t samplePosition = 0, generation = 1;
    std::uint32_t back = 0;

    // Triple-buffer ownership: producer owns back, consumer owns front, and the
    // atomic mailbox owns the third slot. Exchanges transfer ownership, so neither
    // thread ever reads a vector the other thread is writing. No retry loop needed.
    std::array<CapturedWindow, 3> windows;
    std::atomic<std::uint32_t> mailbox{2};
    std::uint32_t front = 1;

    // Everything below belongs exclusively to the consumer after prepare().
    SpectrumFrame output;
    Ballistics ballistics;
    bool frozen = false, initialized = false;
    std::uint64_t lastPosition = 0, lastGeneration = 0;
    std::vector<double> hann, power, peakRemaining;
    std::vector<std::size_t> bitReverse;
    std::vector<std::complex<double>> work, twiddles;
    std::vector<DisplayBin> projection;
    double windowSum = 0.0;

    State(double rate, const AnalyzerSettings& config)
        : settings(config), sampleRate(rate), history(config.fftSize),
          untilPublish(config.fftSize), hann(config.fftSize),
          power(config.fftSize / 2 + 1), peakRemaining(config.pointCount),
          bitReverse(config.fftSize), work(config.fftSize),
          twiddles(config.fftSize / 2), projection(config.pointCount) {
        const auto n = settings.fftSize;
        for (auto& window : windows) window.samples.resize(n);
        output.sampleRate = rate;
        output.fftSize = n;
        output.strongestLevelDb = settings.floorDb;
        output.frequenciesHz.resize(settings.pointCount);
        output.instantaneousDb.assign(settings.pointCount, settings.floorDb);
        output.levelsDb = output.peaksDb = output.instantaneousDb;

        // Periodic Hann, with its actual coherent gain used for amplitude calibration.
        for (std::size_t i = 0; i < n; ++i) {
            hann[i] = 0.5 - 0.5 * std::cos(2.0 * pi * double(i) / double(n));
            windowSum += hann[i];
            std::size_t value = i, reversed = 0;
            for (auto remaining = n; remaining > 1; remaining >>= 1) {
                reversed = (reversed << 1) | (value & 1);
                value >>= 1;
            }
            bitReverse[i] = reversed;
        }
        for (std::size_t i = 0; i < twiddles.size(); ++i)
            twiddles[i] = std::polar(1.0, -2.0 * pi * double(i) / double(n));

        const double ratio = std::pow(settings.maximumFrequency / settings.minimumFrequency,
                                      1.0 / double(settings.pointCount - 1));
        const double halfStep = std::sqrt(ratio);
        for (std::size_t i = 0; i < projection.size(); ++i) {
            const double frequency = settings.minimumFrequency * std::pow(ratio, double(i));
            output.frequenciesHz[i] = float(frequency);
            auto& bin = projection[i];
            bin.inRange = frequency <= rate * 0.5;
            bin.fractionalBin = std::clamp(frequency * double(n) / rate, 0.0, double(n / 2));
            const double low = std::clamp(frequency / halfStep * double(n) / rate, 0.0, double(n / 2));
            const double high = std::clamp(frequency * halfStep * double(n) / rate, 0.0, double(n / 2));
            bin.first = std::size_t(std::ceil(low));
            bin.last = std::size_t(std::floor(high));
            bin.hasBins = bin.first <= bin.last;
        }
    }

    void publish(bool ready) noexcept {
        auto& destination = windows[back];
        if (ready) {
            // The write cursor points to the oldest sample in a full circular history.
            const auto split = history.begin() + std::ptrdiff_t(writePosition);
            auto next = std::copy(split, history.end(), destination.samples.begin());
            std::copy(history.begin(), split, next);
        }
        destination.ready = ready;
        destination.position = samplePosition;
        destination.generation = generation;
        back = mailbox.exchange(back | dirtyBit, std::memory_order_acq_rel) & indexMask;
    }

    void push(float left, float right) noexcept {
        history[writePosition] = {cleanSample(left), cleanSample(right)};
        writePosition = (writePosition + 1) & (settings.fftSize - 1);
        ++samplePosition;
        filled = std::min(filled + 1, settings.fftSize);
        if (--untilPublish == 0) {
            untilPublish = settings.hopSize;
            if (filled == settings.fftSize) publish(true);
        }
    }

    void transform(const CapturedWindow& window) noexcept {
        const auto n = settings.fftSize;
        // Pack L into the real part and R into the imaginary part. A single complex
        // FFT contains both real transforms; separation below preserves stereo power.
        for (std::size_t i = 0; i < n; ++i)
            work[bitReverse[i]] = std::complex<double>(window.samples[i].left,
                                                       window.samples[i].right) * hann[i];
        for (std::size_t length = 2; length <= n; length <<= 1) {
            const auto half = length / 2;
            const auto stride = n / length;
            for (std::size_t start = 0; start < n; start += length) {
                for (std::size_t j = 0; j < half; ++j) {
                    const auto even = work[start + j];
                    const auto odd = work[start + j + half] * twiddles[j * stride];
                    work[start + j] = even + odd;
                    work[start + j + half] = even - odd;
                }
            }
        }
        for (std::size_t k = 0; k <= n / 2; ++k) {
            const auto mirrored = std::conj(work[(n - k) & (n - 1)]);
            const auto left = (work[k] + mirrored) * 0.5;
            const auto right = (work[k] - mirrored) * std::complex<double>(0.0, -0.5);
            // DC and Nyquist have no negative-frequency counterpart to double.
            const double scale = (k == 0 || k == n / 2 ? 1.0 : 2.0) / windowSum;
            power[k] = 0.5 * (std::norm(left) + std::norm(right)) * scale * scale;
        }
    }

    float toDb(double value) const noexcept {
        const double floorPower = std::pow(10.0, double(settings.floorDb) / 10.0);
        return float(std::clamp(10.0 * std::log10(std::max(value, floorPower)),
                                double(settings.floorDb), 60.0));
    }

    void findStrongestPeak() noexcept {
        output.strongestFrequencyHz = 0.0f;
        output.strongestLevelDb = settings.floorDb;
        const auto n = settings.fftSize;
        std::size_t strongest = 0;
        double maximum = 0.0;
        for (std::size_t k = 1; k <= n / 2; ++k) {
            const double frequency = double(k) * sampleRate / double(n);
            if (frequency < settings.minimumFrequency || frequency > settings.maximumFrequency) continue;
            if (power[k] > maximum) { maximum = power[k]; strongest = k; }
        }
        if (toDb(maximum) <= settings.floorDb || strongest == 0) return;
        double offset = 0.0, level = toDb(maximum);
        if (strongest > 0 && strongest < n / 2) {
            const double before = toDb(power[strongest - 1]);
            const double after = toDb(power[strongest + 1]);
            const double curvature = before - 2.0 * level + after;
            if (curvature < -1e-9) {
                offset = std::clamp(0.5 * (before - after) / curvature, -0.5, 0.5);
                level -= 0.25 * (before - after) * offset;
            }
        }
        output.strongestFrequencyHz = float(std::clamp((double(strongest) + offset) *
            sampleRate / double(n), settings.minimumFrequency,
            std::min(settings.maximumFrequency, sampleRate * 0.5)));
        output.strongestLevelDb = float(level);
    }

    void analyze(const CapturedWindow& window) noexcept {
        transform(window);
        const bool continuous = initialized && window.generation == lastGeneration &&
                                window.position > lastPosition;
        const double elapsed = continuous ? double(window.position - lastPosition) / sampleRate : 0.0;
        const double attack = responseCoefficient(elapsed, ballistics.attackMs);
        const double release = responseCoefficient(elapsed, ballistics.releaseMs);
        for (std::size_t i = 0; i < projection.size(); ++i) {
            const auto& bin = projection[i];
            float measured = settings.floorDb;
            if (bin.inRange) {
                const auto low = std::size_t(bin.fractionalBin);
                const auto high = std::min(low + 1, power.size() - 1);
                const double fraction = bin.fractionalBin - double(low);
                double value = power[low] + (power[high] - power[low]) * fraction;
                // Represent the DISPLAY cell by its maximum, not one nearest bin.
                // This preserves narrow high-frequency peaks when bins outnumber pixels.
                // It is an amplitude envelope, NOT a power spectral density estimator.
                if (bin.hasBins)
                    for (auto k = bin.first; k <= bin.last; ++k) value = std::max(value, power[k]);
                measured = toDb(value);
            }
            output.instantaneousDb[i] = measured;
            const float previous = output.levelsDb[i];
            const double coefficient = measured > previous ? attack : release;
            output.levelsDb[i] = continuous
                ? float(measured + (previous - measured) * coefficient) : measured;

            const float level = output.levelsDb[i];
            if (!continuous || level >= output.peaksDb[i]) {
                output.peaksDb[i] = level;
                peakRemaining[i] = ballistics.peakHoldMs * 0.001;
            } else {
                const double fallingTime = std::max(0.0, elapsed - peakRemaining[i]);
                peakRemaining[i] = std::max(0.0, peakRemaining[i] - elapsed);
                output.peaksDb[i] = std::max(level, output.peaksDb[i] -
                    float(fallingTime * ballistics.peakFallDbPerSecond));
            }
        }
        findStrongestPeak();
        output.samplePosition = window.position;
        output.ready = initialized = true;
        lastPosition = window.position;
        lastGeneration = window.generation;
    }

    void clearOutput() noexcept {
        std::fill(output.instantaneousDb.begin(), output.instantaneousDb.end(), settings.floorDb);
        std::fill(output.levelsDb.begin(), output.levelsDb.end(), settings.floorDb);
        std::fill(output.peaksDb.begin(), output.peaksDb.end(), settings.floorDb);
        std::fill(peakRemaining.begin(), peakRemaining.end(), 0.0);
        output.ready = initialized = false;
        output.samplePosition = 0;
        output.strongestFrequencyHz = 0.0f;
        output.strongestLevelDb = settings.floorDb;
    }
};

Analyzer::Analyzer() = default;
Analyzer::~Analyzer() = default;

bool Analyzer::prepare(double sampleRate, const AnalyzerSettings& settings, std::string* error) {
    const char* problem = nullptr;
    if (!std::isfinite(sampleRate) || sampleRate < 1000.0 || sampleRate > 768000.0)
        problem = "Sample rate must be finite and between 1000 and 768000 Hz.";
    else if (settings.fftSize < 256 || settings.fftSize > 65536 ||
             (settings.fftSize & (settings.fftSize - 1)) != 0)
        problem = "FFT size must be a power of two between 256 and 65536.";
    else if (settings.hopSize == 0 || settings.hopSize > settings.fftSize)
        problem = "Hop size must be between 1 and FFT size.";
    else if (settings.pointCount < 2 || settings.pointCount > 8192)
        problem = "Display point count must be between 2 and 8192.";
    else if (!std::isfinite(settings.minimumFrequency) || !std::isfinite(settings.maximumFrequency) ||
             settings.minimumFrequency < 1.0 || settings.maximumFrequency <= settings.minimumFrequency ||
             settings.maximumFrequency > 384000.0 || settings.minimumFrequency >= sampleRate * 0.5)
        problem = "Frequency range must be increasing, positive and start below Nyquist.";
    else if (!std::isfinite(settings.floorDb) || settings.floorDb < -200.0f || settings.floorDb > -20.0f)
        problem = "Noise floor must be between -200 and -20 dBFS.";
    if (problem) { if (error) *error = problem; return false; }

    // Build first, replace second: invalid input/allocation failure never destroys a
    // previously prepared analyzer. Neither thread may run during this operation.
    auto next = std::make_unique<State>(sampleRate, settings);
    if (m_state) next->ballistics = m_state->ballistics;
    m_state = std::move(next);
    if (error) error->clear();
    return true;
}

void Analyzer::pushSample(float left, float right) noexcept {
    if (m_state) m_state->push(left, right);
}
void Analyzer::pushStereo(const float* left, const float* right, std::size_t frames) noexcept {
    if (!m_state) return;
    for (std::size_t i = 0; i < frames; ++i) {
        const float l = left ? left[i] : 0.0f;
        m_state->push(l, right ? right[i] : l);
    }
}
void Analyzer::pushInterleaved(const float* data, std::size_t frames, std::size_t channels) noexcept {
    if (!m_state || !data || channels == 0 || frames > std::numeric_limits<std::size_t>::max() / channels) return;
    for (std::size_t i = 0; i < frames; ++i)
        m_state->push(data[i * channels], data[i * channels + (channels > 1 ? 1 : 0)]);
}
void Analyzer::reset() noexcept {
    if (!m_state) return;
    auto& s = *m_state;
    s.writePosition = s.filled = 0;
    s.samplePosition = 0;
    s.untilPublish = s.settings.fftSize;
    ++s.generation;
    s.publish(false);
}
bool Analyzer::read() noexcept {
    if (!m_state || m_state->frozen) return false;
    auto& s = *m_state;
    if ((s.mailbox.load(std::memory_order_acquire) & dirtyBit) == 0) return false;
    s.front = s.mailbox.exchange(s.front, std::memory_order_acq_rel) & indexMask;
    const auto& window = s.windows[s.front];
    if (window.ready) s.analyze(window);
    else s.clearOutput();
    return true;
}
const SpectrumFrame& Analyzer::frame() const noexcept {
    static const SpectrumFrame empty;
    return m_state ? m_state->output : empty;
}
void Analyzer::setFrozen(bool frozen) noexcept {
    if (m_state) {
        if (m_state->frozen && !frozen) m_state->initialized = false;
        m_state->frozen = frozen;
    }
}
bool Analyzer::frozen() const noexcept { return m_state && m_state->frozen; }
bool Analyzer::setBallistics(const Ballistics& settings) noexcept {
    if (!m_state || !validBallistics(settings)) return false;
    m_state->ballistics = settings;
    return true;
}
} // namespace vlt::spectre