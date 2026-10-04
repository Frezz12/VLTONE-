#pragma once

#include "Audio/SampleBuffer.hpp"

#include <functional>

namespace daw::analysis {

enum class SamplePitchStatus { Unavailable, TooShort, Unstable, Detected };

struct SamplePitchEstimate {
    SamplePitchStatus status = SamplePitchStatus::Unavailable;
    int midiNote = -1;
    double frequencyHz = 0;
    double cents = 0; // Offset from the nearest equal-tempered note, A = 440 Hz.
};

/// A single sustained note in [first, end), before playback transposition.
/// Bounded windows cover the selected range; memory does not grow with file
/// length. Control/worker thread only. Cancellation returns Unavailable.
SamplePitchEstimate detectSamplePitch(const engine::SampleBuffer& audio,
    engine::FrameCount first, engine::FrameCount end,
    const std::function<bool()>& keepGoing = {});

} // namespace daw::analysis
