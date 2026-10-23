// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vlt::spectre {

/// Structural settings. Changing these requires prepare(), with both threads stopped.
struct AnalyzerSettings {
    std::size_t fftSize = 8192;       ///< Power of two, 256 .. 65536.
    std::size_t hopSize = 512;        ///< Publish a window every this many samples.
    std::size_t pointCount = 768;     ///< Logarithmically spaced display points.
    double minimumFrequency = 20.0;
    double maximumFrequency = 20000.0;
    float floorDb = -120.0f;
};

/// Consumer-side settings; these do not change the audio capture or FFT resolution.
struct Ballistics {
    double attackMs = 12.0;          ///< Time constant, not time to reach the target.
    double releaseMs = 240.0;
    double peakHoldMs = 650.0;
    double peakFallDbPerSecond = 18.0;
};

/// Owned by the analyzer. All levels are peak-amplitude dBFS, without display tilt.
/// A bin-centred, full-scale sine in both channels reads 0 dBFS.
/// Stereo is the average of channel powers, so opposite phases never cancel.
struct SpectrumFrame {
    std::vector<float> frequenciesHz;
    std::vector<float> instantaneousDb; ///< Measured window, without temporal smoothing.
    std::vector<float> levelsDb;        ///< Attack/release envelope for the main trace.
    std::vector<float> peaksDb;         ///< Peak hold, followed by a decay in dB/second.
    double sampleRate = 0.0;
    std::size_t fftSize = 0;
    std::uint64_t samplePosition = 0;
    float strongestFrequencyHz = 0.0f; ///< Interpolated FFT peak estimate; 0 at silence.
    float strongestLevelDb = -120.0f;
    bool ready = false;               ///< False until one complete window is captured.
};

/// One producer (audio callback) and one consumer (UI or analysis worker).
///
/// prepare(): stopped threads only; allocates the buffers and FFT plan.
/// push*(), reset(): producer only; bounded, no allocation, locks, waiting or FFT.
/// read(), setBallistics(), setFrozen(), frame(): consumer only; no allocation.
///
/// read() processes the newest complete window. Intermediate windows may be
/// skipped if the consumer is slow; the audio callback never waits for a reader.
/// To serve several views, read once and share/copy the frame on the consumer side.
class Analyzer final {
public:
    Analyzer();
    ~Analyzer();
    Analyzer(const Analyzer&) = delete;
    Analyzer& operator=(const Analyzer&) = delete;
    Analyzer(Analyzer&&) = delete;
    Analyzer& operator=(Analyzer&&) = delete;

    /// Returns false for invalid settings (previous configuration remains intact).
    /// Allocation failure throws std::bad_alloc; call outside the audio callback.
    bool prepare(double sampleRate, const AnalyzerSettings& settings = {},
                 std::string* error = nullptr);

    /// Null left means silence; null right duplicates left (mono).
    void pushStereo(const float* left, const float* right, std::size_t frames) noexcept;
    /// For multichannel input, analyzes the first two channels. Mono is duplicated.
    /// Null data or zero channels is ignored. The caller owns the input buffer.
    void pushInterleaved(const float* data, std::size_t frames,
                         std::size_t channels = 2) noexcept;
    void pushSample(float left, float right) noexcept;

    /// Start a new stream. Safe concurrently with read(), but not another producer.
    /// Publishes an empty frame; old audio cannot leak into the next window.
    void reset() noexcept;

    /// Consume at most one latest window and run its FFT. True means frame changed.
    /// Call regularly (e.g. 60 Hz). This does NOT belong in the audio callback.
    bool read() noexcept;
    const SpectrumFrame& frame() const noexcept;

    /// Freeze the consumer display; capture continues, so unfreeze returns to live audio.
    void setFrozen(bool frozen) noexcept;
    bool frozen() const noexcept;
    /// Invalid/negative/nonfinite values are rejected without changing the settings.
    bool setBallistics(const Ballistics& settings) noexcept;

private:
    struct State;
    std::unique_ptr<State> m_state;
};

} // namespace vlt::spectre