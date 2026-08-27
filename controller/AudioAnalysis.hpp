#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/// Measuring audio, so decisions about it can rest on numbers.
///
/// Written for the AI assistant, which cannot hear: told "the bass sits 9 dB
/// above everything else and the master clips 240 times", a model can mix.
/// Told nothing, it can only guess from the track's name. Everything here is a
/// pure function over sample data — no engine, no files, no Qt — so it is
/// testable against signals a test synthesises itself.
///
/// Deliberately coarse. Three energy bands from one-pole filters, not an FFT:
/// the question being answered is "is this boomy or thin", and a spectrum would
/// be more numbers without being a better answer.
namespace daw::analysis {

/// What one stretch of audio is doing.
struct Metrics {
    double peak = 0.0;          ///< linear, 0 … 1+
    double peakDb = -120.0;
    double rms = 0.0;
    double rmsDb = -120.0;
    /// Share of the total energy below ~200 Hz, between, and above ~4 kHz.
    /// They sum to 1 for any signal with energy in it.
    double lowFraction = 0.0;
    double midFraction = 0.0;
    double highFraction = 0.0;
    /// Samples at or past full scale. Non-zero means the mix is being damaged,
    /// which is the one thing worth saying without being asked.
    std::uint64_t clipped = 0;
    double seconds = 0.0;
    bool silent = true;
};

Metrics measure(const float* const* channels, int channelCount,
                std::size_t frames, double sampleRate);

/// Stateful version of `measure` for block-by-block renders. Filter histories
/// and energy totals cross block boundaries, so its result matches measuring a
/// single contiguous buffer without retaining that buffer in memory.
class MetricsAccumulator {
public:
    MetricsAccumulator(double sampleRate, int channelCount);
    ~MetricsAccumulator();
    MetricsAccumulator(MetricsAccumulator&&) noexcept;
    MetricsAccumulator& operator=(MetricsAccumulator&&) noexcept;
    MetricsAccumulator(const MetricsAccumulator&) = delete;
    MetricsAccumulator& operator=(const MetricsAccumulator&) = delete;

    void add(const float* const* channels, std::size_t frames);
    Metrics result() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// What a sample file is, as far as it can be told without listening.
struct SampleTraits {
    Metrics level;
    /// Time from the start to the loudest point. Short means percussive.
    double attackSeconds = 0.0;
    /// True when the sample holds a steady pitch — a note rather than a hit.
    bool tonal = false;
    int pitch = -1;              ///< MIDI note when `tonal`, else −1
    double pitchHz = 0.0;
    double confidence = 0.0;     ///< 0 … 1, how sure the pitch is
    /// "kick", "snare", "hat", "tonal one-shot", "sustained tone", "loop" —
    /// a word for what the numbers add up to, since that is what a model will
    /// reason with.
    std::string character;
};

SampleTraits describe(const float* const* channels, int channelCount,
                      std::size_t frames, double sampleRate);

/// Decibels from a linear amplitude, floored so silence is a number and not
/// negative infinity.
double toDb(double linear);

} // namespace daw::analysis
