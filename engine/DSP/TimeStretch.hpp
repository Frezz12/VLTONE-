#pragma once

#include "Audio/SampleBuffer.hpp"
#include <memory>

namespace daw::engine::dsp {

/// Random-access source. All boundaries and positions are decoded-file frames.
struct StretchSource {
    const SampleBuffer* audio = nullptr;
    double begin = 0, end = 0;
    double loopBegin = 0, loopEnd = 0;
    int loopMode = 0;
    friend bool operator==(const StretchSource&, const StretchSource&) = default;
};

/// Phase-coherent stereo stretching of an already decoded source. Preparation
/// allocates on the control thread; render/reset allocate nothing. Look-ahead
/// comes from the source, so notes/clips do not acquire an audible start delay.
class TimeStretch {
public:
    TimeStretch(double sampleRate, int mode, double maximumRatio = 4.0);
    ~TimeStretch();
    TimeStretch(const TimeStretch&) = delete;
    TimeStretch& operator=(const TimeStretch&) = delete;
    double sampleRate() const noexcept;
    int mode() const noexcept;
    double maximumRatio() const noexcept;
    void reset() noexcept;

    /// sourcePosition is relative to source.begin, before looping. speed is
    /// source seconds per output second; pitch is independent, in semitones.
    /// The caller supplies two output channels (mono sources are duplicated).
    void render(const StretchSource& source, double sourcePosition, double speed,
                double pitch, double formant, float* left, float* right,
                FrameCount frames) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace daw::engine::dsp
