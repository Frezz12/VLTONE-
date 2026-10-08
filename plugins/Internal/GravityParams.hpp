#pragma once

#include "Host/PluginTypes.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace daw::plugins::gravity {

enum class Param : std::uint32_t {
    Gravity,
    Pitch,
    Feedback,
    Decay,
    Size,
    Algorithm,
    TimingSync,
    TimingDivision,
    TimingMs,
    Mass,
    Motion,
    Density,
    Diffusion,
    Damping,
    Reverse,
    StereoWidth,
    StereoInput,
    Ducking,
    Transient,
    Drive,
    PitchSpread,
    PitchSnap,
    FeedbackLowcut,
    DetectorSource,
    Count,
};

enum class Algorithm : int { Orbit = 0, Fall, Rise, Void, Collapse, ZeroG };
enum class Division : int {
    Sixteenth = 0,
    EighthTriplet,
    Eighth,
    EighthDotted,
    Quarter,
    ThirtySecond,
    SixteenthTriplet,
    QuarterTriplet,
    QuarterDotted,
    Half,
    Whole,
};
enum class PitchSnap : int { Off = 0, Chromatic, Perfect, Octave };
enum class DetectorSource : int { Main = 0, Sidechain, Auto };

inline constexpr std::uint32_t kParameterCount = std::uint32_t(Param::Count);

struct FactoryPreset {
    std::string_view name;
    std::array<double, kParameterCount> values;
};

std::span<const ParameterInfo> parameterTable() noexcept;
std::string parameterText(std::uint32_t index, double value);
std::span<const FactoryPreset> factoryPresets() noexcept;

struct Telemetry {
    float inputLeft = 0.0f;
    float inputRight = 0.0f;
    float outputLeft = 0.0f;
    float outputRight = 0.0f;
    float fieldEnergy = 0.0f;
    float orbitPhase = 0.0f;
    float duckGain = 1.0f;
    float transientPulse = 0.0f;
    std::uint64_t grainSerial = 0;
    std::uint32_t activeGrains = 0;
    bool frozen = false;
};

} // namespace daw::plugins::gravity
