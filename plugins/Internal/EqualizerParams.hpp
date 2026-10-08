#pragma once

#include "Host/PluginTypes.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace daw::plugins::equalizer {

enum class ProcessingMode : std::uint32_t { ZeroLatency = 0, AnalogPhase, LinearPhase };
enum class LinearResolution : std::uint32_t { Low = 0, Medium, High };
enum class FilterType : std::uint32_t {
    Bell = 0,
    LowShelf,
    HighShelf,
    LowCut,
    HighCut,
    Notch,
    BandPass,
    Tilt,
    AllPass,
};
enum class Slope : std::uint32_t {
    Db6 = 0, Db12, Db18, Db24, Db36, Db48, Db72, Db96,
};
enum class Placement : std::uint32_t { Stereo = 0, Left, Right, Mid, Side };
enum class DetectorMode : std::uint32_t { Band = 0, Free };

enum class GlobalParam : std::uint32_t {
    ProcessingMode = 0,
    LinearResolution,
    OutputGain,
    OutputBalance,
    PolarityInvert,
    GainScale,
    AutoGain,
    Count,
};

enum class BandParam : std::uint32_t {
    Enabled = 0,
    Type,
    Frequency,
    Gain,
    Q,
    Slope,
    Placement,
    DynamicEnabled,
    DynamicRange,
    DynamicAuto,
    DynamicThreshold,
    DynamicAttack,
    DynamicRelease,
    DynamicExternal,
    DetectorMode,
    DetectorLow,
    DetectorHigh,
    Count,
};

inline constexpr std::uint32_t kBandCount = 24;
inline constexpr std::uint32_t kGlobalParameterCount =
    std::uint32_t(GlobalParam::Count);
inline constexpr std::uint32_t kBandParameterCount =
    std::uint32_t(BandParam::Count);
inline constexpr std::uint32_t kParameterCount =
    kGlobalParameterCount + kBandCount * kBandParameterCount;
inline constexpr std::size_t kSpectrumBinCount = 128;

constexpr std::uint32_t globalParameter(GlobalParam parameter) noexcept {
    return std::uint32_t(parameter);
}

constexpr std::uint32_t bandParameter(std::uint32_t band,
                                      BandParam parameter) noexcept {
    return kGlobalParameterCount + band * kBandParameterCount +
           std::uint32_t(parameter);
}

struct BandState {
    bool enabled = false;
    FilterType type = FilterType::Bell;
    double frequency = 1000.0;
    double gainDb = 0.0;
    double q = 1.0;
    Slope slope = Slope::Db12;
    Placement placement = Placement::Stereo;
    bool dynamicEnabled = false;
    double dynamicRangeDb = 0.0;
    bool dynamicAuto = true;
    double thresholdDb = -24.0;
    double attackMs = 20.0;
    double releaseMs = 160.0;
    bool externalSidechain = false;
    DetectorMode detectorMode = DetectorMode::Band;
    double detectorLow = 20.0;
    double detectorHigh = 20000.0;
};

struct AnalyzerConfig {
    bool enabled = false;
    bool pre = true;
    bool post = true;
    bool sidechain = false;
    bool frozen = false;
    int speed = 1;
    double tiltDbPerOctave = 3.0;
};

struct Telemetry {
    std::array<float, kSpectrumBinCount> pre{};
    std::array<float, kSpectrumBinCount> post{};
    std::array<float, kSpectrumBinCount> sidechain{};
    std::array<float, kBandCount> dynamicGainDb{};
    float inputLeft = 0.0f;
    float inputRight = 0.0f;
    float outputLeft = 0.0f;
    float outputRight = 0.0f;
    bool sidechainPresent = false;
};

struct FactoryPreset {
    std::string_view name;
    std::array<double, kParameterCount> values{};
};

std::span<const ParameterInfo> parameterTable() noexcept;
std::string parameterText(std::uint32_t index, double value);
std::span<const FactoryPreset> factoryPresets() noexcept;
std::string parameterId(std::uint32_t index);

} // namespace daw::plugins::equalizer
