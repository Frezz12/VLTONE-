#pragma once
#include "DSP/Curve.hpp"
#include <cstdint>

namespace daw::engine {
/// A slice of a pitch segment. Keeping its original phase avoids changing the
/// curve when an audio block, a seek or a tempo change splits the segment.
inline std::size_t pitchEventCapacity(std::uint32_t frames, double sampleRate) noexcept {
    // A curve may cross several short slide notes in one buffer. The actual
    // upper bound is one segment boundary per sample and voice, plus the
    // external 1 kHz sampling grid; 256 points per slide is not a block bound.
    return 2048 + 128 * (std::size_t(frames) +
                         std::size_t(std::ceil(frames * 1000.0 / std::max(1.0, sampleRate))));
}
struct PitchRamp {
    double from = 0, to = 0;
    double phaseFrom = 0, phaseTo = 1;
    double tension = 0;
    double segmentStart = -1e300; ///< Stable first-segment identity across blocks.
    double shapeFrom = 0, shapeTo = 1, priority = -1e300;
    std::uint32_t frames = 0;
    curve::Shape shape = curve::Shape::Linear;
    bool active = false;
    bool edited = false; ///< Smooth a live edit, never ordinary segment boundaries.
    double at(std::uint32_t frame) const noexcept {
        const double t = frames ? std::min(1.0, double(frame) / frames) : 1.0;
        const double shaped = curve::shapeT(phaseFrom + (phaseTo - phaseFrom) * t, shape, tension);
        return from +
               (to - from) *
                   (shapeTo != shapeFrom ? (shaped - shapeFrom) / (shapeTo - shapeFrom) : 0.0);
    }
};
} // namespace daw::engine
