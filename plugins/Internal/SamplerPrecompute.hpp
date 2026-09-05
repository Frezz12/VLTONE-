#pragma once

#include "Audio/SampleBuffer.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <functional>

/// The sampler's precomputed effects: baked into the sample once, on a
/// background worker, instead of costing anything per voice or GUI tick.
///
/// This is what "precomputed" means in the FL sampler this page is modelled on
/// — the effects are part of the *sampler*, not plugins in the chain, and they
/// are applied when the sample is loaded. Two consequences are inherent and
/// deliberate: they cannot be automated (nothing here can run per block), and
/// "Keep on disk" turns them off, because there is no in-memory copy to bake
/// them into.
namespace daw::plugins::sampler {

struct PrecomputeSettings {
    double boost = 0.0;         ///< 0…1, gain with hard clipping
    double eqLow = 0.0;         ///< −1…1 → ∓12 dB shelf at 200 Hz
    double eqMid = 0.0;         ///< −1…1 → ∓12 dB peak at 1.5 kHz
    double eqHigh = 0.0;        ///< −1…1 → ∓12 dB shelf at 6 kHz
    double ringMix = 0.0;
    double ringFreq = 0.5;      ///< 0…1 → 20 Hz … 8 kHz
    double cut = 1.0;           ///< 0…1 lowpass cutoff, 1 = open
    double res = 0.0;
    int reverbType = 0;         ///< 0 room, 1 hall
    double reverb = 0.0;
    double stereoDelay = 0.0;   ///< pseudo-stereo width
    double pogo = 0.0;          ///< −1…1, a pitch drop (or rise) at the start
    bool removeDc = false;
    bool reversePolarity = false;
    bool normalize = false;
    bool fadeStereo = false;
    bool reverse = false;
    bool swapStereo = false;

    /// True when every control is at its neutral value, in which case the raw
    /// sample can be used as-is and no copy is made at all.
    bool isNeutral() const noexcept;

    /// Equal settings render to an identical sample, so this — and nothing
    /// else — decides whether a cached bake is still the right one. Comparing
    /// the settings themselves rather than a hash of some hand-written list of
    /// fields is what keeps a cache from either colliding or, far worse,
    /// re-rendering the whole sample because a *playback* control moved.
    friend bool operator==(const PrecomputeSettings&,
                           const PrecomputeSettings&) noexcept = default;
};

/// Cooperative cancellation for a background bake. The owner increments the
/// generation whenever newer settings or a different sample supersede the
/// request. Long DSP loops check this token at bounded intervals and return
/// without constructing/publishing a stale SampleBuffer.
struct PrecomputeCancellation {
    const std::atomic<std::uint64_t>* generation = nullptr;
    std::uint64_t expected = 0;
    const std::function<bool()>* keepGoing = nullptr;

    bool requested() const {
        return (generation && generation->load(std::memory_order_acquire) != expected) ||
               (keepGoing && *keepGoing && !(*keepGoing)());
    }
};

/// Render `raw` through the settings. `baseFrames` comes back as the length of
/// the *musical* material — the reverb appends a tail past it, and the loop and
/// start-offset fractions are measured against the base, never the tail.
std::shared_ptr<const engine::SampleBuffer> precompute(
    const engine::SampleBuffer& raw, const PrecomputeSettings& settings,
    engine::FrameCount& baseFrames,
    PrecomputeCancellation cancellation = {});

} // namespace daw::plugins::sampler
