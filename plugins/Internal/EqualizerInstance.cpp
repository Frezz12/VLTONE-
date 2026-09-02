#include "Internal/EqualizerInstance.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>

namespace daw::plugins::equalizer {
namespace {

using json = nlohmann::json;
constexpr std::string_view kUid = "daw.equalizer";
constexpr int kStateVersion = 1;
constexpr double kPi = std::numbers::pi_v<double>;
constexpr double kSilenceDb = -120.0;

double dbToGain(double db) noexcept { return std::pow(10.0, db / 20.0); }
double gainToDb(double gain) noexcept {
    return gain > 1.0e-12 ? 20.0 * std::log10(gain) : kSilenceDb;
}

const std::array<ParameterInfo, kParameterCount>& parametersImpl() {
    static const std::array<ParameterInfo, kParameterCount> table = [] {
        std::array<ParameterInfo, kParameterCount> out{};
        auto add = [&](std::uint32_t index, std::string id, std::string name,
                       std::string unit, double minimum, double maximum,
                       double initial, bool stepped = false,
                       bool automatable = true) {
            out[index] = ParameterInfo{index, std::move(id), std::move(name),
                                       std::move(unit), minimum, maximum,
                                       initial, automatable, stepped, false};
        };

        add(globalParameter(GlobalParam::ProcessingMode), "processing.mode",
            "Processing Mode", {}, 0.0, 2.0, 0.0, true, false);
        add(globalParameter(GlobalParam::LinearResolution), "linear.resolution",
            "Linear Resolution", {}, 0.0, 2.0, 1.0, true, false);
        add(globalParameter(GlobalParam::OutputGain), "output.gain",
            "Output Gain", "dB", -24.0, 24.0, 0.0);
        add(globalParameter(GlobalParam::OutputBalance), "output.balance",
            "Output Balance", {}, -1.0, 1.0, 0.0);
        add(globalParameter(GlobalParam::PolarityInvert), "output.polarity",
            "Polarity Invert", {}, 0.0, 1.0, 0.0, true);
        add(globalParameter(GlobalParam::GainScale), "gain.scale", "Gain Scale",
            "%", 0.0, 2.0, 1.0);
        add(globalParameter(GlobalParam::AutoGain), "auto.gain", "Auto Gain", {},
            0.0, 1.0, 0.0, true);

        for (std::uint32_t band = 0; band < kBandCount; ++band) {
            char prefix[24]{};
            std::snprintf(prefix, sizeof(prefix), "band.%02u.", band + 1);
            const std::string p(prefix);
            const std::string label = "Band " + std::to_string(band + 1) + " ";
            const double t = double(band) / double(kBandCount - 1);
            const double frequency = 20.0 * std::pow(1000.0, t);
            add(bandParameter(band, BandParam::Enabled), p + "enabled",
                label + "Enabled", {}, 0.0, 1.0, 0.0, true);
            add(bandParameter(band, BandParam::Type), p + "type",
                label + "Type", {}, 0.0, 8.0, 0.0, true);
            add(bandParameter(band, BandParam::Frequency), p + "frequency",
                label + "Frequency", "Hz", 10.0, 30000.0, frequency);
            add(bandParameter(band, BandParam::Gain), p + "gain",
                label + "Gain", "dB", -30.0, 30.0, 0.0);
            add(bandParameter(band, BandParam::Q), p + "q", label + "Q", {},
                0.025, 40.0, 1.0);
            add(bandParameter(band, BandParam::Slope), p + "slope",
                label + "Slope", "dB/oct", 0.0, 7.0, 1.0, true);
            add(bandParameter(band, BandParam::Placement), p + "placement",
                label + "Placement", {}, 0.0, 4.0, 0.0, true);
            add(bandParameter(band, BandParam::DynamicEnabled), p + "dynamic.enabled",
                label + "Dynamics", {}, 0.0, 1.0, 0.0, true);
            add(bandParameter(band, BandParam::DynamicRange), p + "dynamic.range",
                label + "Dynamic Range", "dB", -30.0, 30.0, 0.0);
            add(bandParameter(band, BandParam::DynamicAuto), p + "dynamic.auto",
                label + "Dynamic Auto", {}, 0.0, 1.0, 1.0, true);
            add(bandParameter(band, BandParam::DynamicThreshold),
                p + "dynamic.threshold", label + "Threshold", "dB",
                -90.0, 0.0, -24.0);
            add(bandParameter(band, BandParam::DynamicAttack), p + "dynamic.attack",
                label + "Attack", "ms", 0.1, 500.0, 20.0);
            add(bandParameter(band, BandParam::DynamicRelease), p + "dynamic.release",
                label + "Release", "ms", 5.0, 5000.0, 160.0);
            add(bandParameter(band, BandParam::DynamicExternal),
                p + "dynamic.external", label + "External Sidechain", {},
                0.0, 1.0, 0.0, true);
            add(bandParameter(band, BandParam::DetectorMode), p + "detector.mode",
                label + "Detector Mode", {}, 0.0, 1.0, 0.0, true);
            add(bandParameter(band, BandParam::DetectorLow), p + "detector.low",
                label + "Detector Low", "Hz", 10.0, 30000.0, 20.0);
            add(bandParameter(band, BandParam::DetectorHigh), p + "detector.high",
                label + "Detector High", "Hz", 10.0, 30000.0, 20000.0);
        }
        return out;
    }();
    return table;
}

void setPresetBand(FactoryPreset& preset, std::uint32_t band, FilterType type,
                   double frequency, double gain, double q,
                   Slope slope = Slope::Db12,
                   Placement placement = Placement::Stereo) {
    preset.values[bandParameter(band, BandParam::Enabled)] = 1.0;
    preset.values[bandParameter(band, BandParam::Type)] = double(type);
    preset.values[bandParameter(band, BandParam::Frequency)] = frequency;
    preset.values[bandParameter(band, BandParam::Gain)] = gain;
    preset.values[bandParameter(band, BandParam::Q)] = q;
    preset.values[bandParameter(band, BandParam::Slope)] = double(slope);
    preset.values[bandParameter(band, BandParam::Placement)] = double(placement);
}

void makeDynamic(FactoryPreset& preset, std::uint32_t band, double range,
                 double threshold = -24.0, bool external = false) {
    preset.values[bandParameter(band, BandParam::DynamicEnabled)] = 1.0;
    preset.values[bandParameter(band, BandParam::DynamicRange)] = range;
    preset.values[bandParameter(band, BandParam::DynamicThreshold)] = threshold;
    preset.values[bandParameter(band, BandParam::DynamicExternal)] = external ? 1.0 : 0.0;
}

const std::array<FactoryPreset, 18>& presetsImpl() {
    static const std::array<FactoryPreset, 18> presets = [] {
        std::array<FactoryPreset, 18> out{};
        constexpr std::array<std::string_view, 18> names{
            "Flat", "Low Cut Clean", "Vocal Cleanup", "Vocal Presence",
            "Dynamic De-Ess", "Kick Tight", "Snare Crack", "Bass Control",
            "Drum Bus Punch", "Acoustic Polish", "Guitar Bite", "Piano Clarity",
            "Synth Tame", "Pad Space", "Sidechain Unmask", "Mix Bus Gentle",
            "Master Transparent", "Master Linear"};
        const auto params = parameterTable();
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i].name = names[i];
            for (const ParameterInfo& info : params)
                out[i].values[info.index] = info.defaultValue;
        }
        setPresetBand(out[1], 0, FilterType::LowCut, 32.0, 0.0, 0.707,
                      Slope::Db24);
        setPresetBand(out[2], 0, FilterType::LowCut, 85.0, 0.0, 0.707,
                      Slope::Db18);
        setPresetBand(out[2], 1, FilterType::Bell, 280.0, -2.5, 1.2);
        setPresetBand(out[2], 2, FilterType::Bell, 950.0, -1.5, 2.0);
        setPresetBand(out[3], 0, FilterType::Bell, 3200.0, 2.5, 0.8);
        setPresetBand(out[3], 1, FilterType::HighShelf, 11000.0, 2.0, 0.7);
        setPresetBand(out[4], 0, FilterType::Bell, 7200.0, 0.0, 3.5);
        makeDynamic(out[4], 0, -7.0, -30.0);
        setPresetBand(out[5], 0, FilterType::LowCut, 28.0, 0.0, 0.7, Slope::Db24);
        setPresetBand(out[5], 1, FilterType::Bell, 64.0, 3.0, 1.0);
        setPresetBand(out[5], 2, FilterType::Bell, 340.0, -3.0, 1.3);
        setPresetBand(out[6], 0, FilterType::Bell, 190.0, 2.0, 1.1);
        setPresetBand(out[6], 1, FilterType::Bell, 4800.0, 3.0, 1.4);
        setPresetBand(out[7], 0, FilterType::LowCut, 30.0, 0.0, 0.7, Slope::Db18);
        setPresetBand(out[7], 1, FilterType::Bell, 95.0, 2.0, 0.8);
        setPresetBand(out[7], 2, FilterType::Bell, 250.0, -2.5, 1.1);
        makeDynamic(out[7], 1, -3.0, -20.0);
        setPresetBand(out[8], 0, FilterType::LowShelf, 90.0, 1.0, 0.7);
        setPresetBand(out[8], 1, FilterType::Bell, 420.0, -1.5, 1.0);
        setPresetBand(out[8], 2, FilterType::HighShelf, 8000.0, 1.5, 0.7);
        setPresetBand(out[9], 0, FilterType::LowCut, 70.0, 0.0, 0.7, Slope::Db18);
        setPresetBand(out[9], 1, FilterType::Bell, 220.0, -2.0, 1.0);
        setPresetBand(out[9], 2, FilterType::HighShelf, 9000.0, 2.0, 0.7);
        setPresetBand(out[10], 0, FilterType::Bell, 2800.0, 2.5, 1.3);
        setPresetBand(out[10], 1, FilterType::Bell, 6200.0, -1.5, 2.0);
        setPresetBand(out[11], 0, FilterType::Bell, 230.0, -1.5, 1.0);
        setPresetBand(out[11], 1, FilterType::Bell, 2500.0, 1.5, 0.9);
        setPresetBand(out[11], 2, FilterType::HighShelf, 10000.0, 1.0, 0.7);
        setPresetBand(out[12], 0, FilterType::Bell, 2600.0, 0.0, 2.3);
        makeDynamic(out[12], 0, -5.0, -22.0);
        setPresetBand(out[12], 1, FilterType::HighShelf, 9500.0, -1.0, 0.7);
        setPresetBand(out[13], 0, FilterType::LowCut, 110.0, 0.0, 0.7,
                      Slope::Db24, Placement::Side);
        setPresetBand(out[13], 1, FilterType::HighShelf, 9000.0, -2.0, 0.7,
                      Slope::Db12, Placement::Side);
        setPresetBand(out[14], 0, FilterType::Bell, 1800.0, 0.0, 1.1);
        makeDynamic(out[14], 0, -6.0, -26.0, true);
        setPresetBand(out[15], 0, FilterType::LowShelf, 90.0, 0.7, 0.7);
        setPresetBand(out[15], 1, FilterType::Bell, 320.0, -0.8, 0.9);
        setPresetBand(out[15], 2, FilterType::HighShelf, 10000.0, 0.8, 0.7);
        setPresetBand(out[16], 0, FilterType::LowCut, 24.0, 0.0, 0.7, Slope::Db18);
        setPresetBand(out[16], 1, FilterType::Bell, 280.0, -0.5, 0.8);
        setPresetBand(out[16], 2, FilterType::HighShelf, 12000.0, 0.5, 0.7);
        out[17] = out[16];
        out[17].name = names[17];
        out[17].values[globalParameter(GlobalParam::ProcessingMode)] =
            double(ProcessingMode::LinearPhase);
        out[17].values[globalParameter(GlobalParam::LinearResolution)] =
            double(LinearResolution::Medium);
        return out;
    }();
    return presets;
}

template <typename Enum>
Enum enumValue(double value, Enum maximum) noexcept {
    return Enum(std::clamp(int(std::lround(value)), 0, int(maximum)));
}

BandState stateFromValues(const std::array<double, kParameterCount>& values,
                          std::uint32_t band) noexcept {
    BandState state;
    auto value = [&](BandParam parameter) {
        return values[bandParameter(band, parameter)];
    };
    state.enabled = value(BandParam::Enabled) >= 0.5;
    state.type = enumValue(value(BandParam::Type), FilterType::AllPass);
    state.frequency = value(BandParam::Frequency);
    state.gainDb = value(BandParam::Gain);
    state.q = value(BandParam::Q);
    state.slope = enumValue(value(BandParam::Slope), Slope::Db96);
    state.placement = enumValue(value(BandParam::Placement), Placement::Side);
    state.dynamicEnabled = value(BandParam::DynamicEnabled) >= 0.5;
    state.dynamicRangeDb = value(BandParam::DynamicRange);
    state.dynamicAuto = value(BandParam::DynamicAuto) >= 0.5;
    state.thresholdDb = value(BandParam::DynamicThreshold);
    state.attackMs = value(BandParam::DynamicAttack);
    state.releaseMs = value(BandParam::DynamicRelease);
    state.externalSidechain = value(BandParam::DynamicExternal) >= 0.5;
    state.detectorMode = enumValue(value(BandParam::DetectorMode), DetectorMode::Free);
    state.detectorLow = value(BandParam::DetectorLow);
    state.detectorHigh = value(BandParam::DetectorHigh);
    return state;
}

} // namespace

std::span<const ParameterInfo> parameterTable() noexcept { return parametersImpl(); }
std::span<const FactoryPreset> factoryPresets() noexcept { return presetsImpl(); }

std::string parameterId(std::uint32_t index) {
    return index < kParameterCount ? parametersImpl()[index].id : std::string{};
}

std::string parameterText(std::uint32_t index, double value) {
    if (index >= kParameterCount) return {};
    char text[64]{};
    if (index == globalParameter(GlobalParam::ProcessingMode)) {
        static constexpr const char* names[]{"Zero Latency", "Analog Phase", "Linear Phase"};
        return names[std::clamp(int(std::lround(value)), 0, 2)];
    }
    if (index == globalParameter(GlobalParam::LinearResolution)) {
        static constexpr const char* names[]{"Low", "Medium", "High"};
        return names[std::clamp(int(std::lround(value)), 0, 2)];
    }
    if (index == globalParameter(GlobalParam::OutputBalance)) {
        if (std::abs(value) < 0.005) return "Center";
        std::snprintf(text, sizeof(text), "%d%% %c", int(std::lround(std::abs(value) * 100.0)),
                      value < 0.0 ? 'L' : 'R');
        return text;
    }
    if (index < kGlobalParameterCount) {
        if (index == globalParameter(GlobalParam::PolarityInvert) ||
            index == globalParameter(GlobalParam::AutoGain))
            return value >= 0.5 ? "On" : "Off";
        if (index == globalParameter(GlobalParam::GainScale))
            std::snprintf(text, sizeof(text), "%.0f%%", value * 100.0);
        else
            std::snprintf(text, sizeof(text), "%+.2f dB", value);
        return text;
    }

    const auto field = BandParam((index - kGlobalParameterCount) % kBandParameterCount);
    switch (field) {
        case BandParam::Enabled:
        case BandParam::DynamicEnabled:
        case BandParam::DynamicAuto:
        case BandParam::DynamicExternal:
            return value >= 0.5 ? "On" : "Off";
        case BandParam::Type: {
            static constexpr const char* names[]{"Bell", "Low Shelf", "High Shelf",
                "Low Cut", "High Cut", "Notch", "Band Pass", "Tilt", "All Pass"};
            return names[std::clamp(int(std::lround(value)), 0, 8)];
        }
        case BandParam::Frequency:
        case BandParam::DetectorLow:
        case BandParam::DetectorHigh:
            if (value >= 1000.0)
                std::snprintf(text, sizeof(text), "%.2f kHz", value / 1000.0);
            else
                std::snprintf(text, sizeof(text), "%.1f Hz", value);
            return text;
        case BandParam::Gain:
        case BandParam::DynamicRange:
        case BandParam::DynamicThreshold:
            std::snprintf(text, sizeof(text), "%+.2f dB", value);
            return text;
        case BandParam::Q:
            std::snprintf(text, sizeof(text), "Q %.3g", value);
            return text;
        case BandParam::Slope: {
            static constexpr int slopes[]{6, 12, 18, 24, 36, 48, 72, 96};
            std::snprintf(text, sizeof(text), "%d dB/oct",
                          slopes[std::clamp(int(std::lround(value)), 0, 7)]);
            return text;
        }
        case BandParam::Placement: {
            static constexpr const char* names[]{"Stereo", "Left", "Right", "Mid", "Side"};
            return names[std::clamp(int(std::lround(value)), 0, 4)];
        }
        case BandParam::DynamicAttack:
        case BandParam::DynamicRelease:
            std::snprintf(text, sizeof(text), "%.1f ms", value);
            return text;
        case BandParam::DetectorMode:
            return value >= 0.5 ? "Free" : "Band";
        case BandParam::Count:
            break;
    }
    return {};
}

EqualizerInstance::EqualizerInstance() : m_descriptor(staticDescriptor()) {
    for (const ParameterInfo& info : parameterTable()) {
        m_values[info.index].store(info.defaultValue, std::memory_order_relaxed);
        m_targets[info.index] = info.defaultValue;
        m_smoothed[info.index] = info.defaultValue;
        m_comparisonA[info.index] = info.defaultValue;
        m_comparisonB[info.index] = info.defaultValue;
    }
    m_autoLevelsDb.fill(-48.0);
}

const PluginDescriptor& EqualizerInstance::staticDescriptor() noexcept {
    static const PluginDescriptor descriptor = [] {
        PluginDescriptor d;
        d.format = Format::Internal;
        d.uid = std::string(kUid);
        d.path = std::string(kUid);
        d.name = "VLT Equalizer";
        d.vendor = "VLT Studio Pro";
        d.version = "1.0";
        d.stateSchemaVersion = kStateVersion;
        d.category = "Effect|EQ";
        d.isInstrument = false;
        d.hasEditor = false;
        d.wantsMidi = false;
        d.mainInputChannels = 2;
        d.mainOutputChannels = 2;
        return d;
    }();
    return descriptor;
}

std::string_view EqualizerInstance::uid() noexcept { return kUid; }

bool EqualizerInstance::setBusLayout(const PluginBusLayout& wanted,
                                     PluginBusLayout& accepted) {
    if (wanted.inputs.size() > 2 || wanted.outputs.size() > 1) return false;
    std::uint16_t channels = wanted.inputs.empty() ? 2 : wanted.inputs.front();
    if (channels != 1 && channels != 2) return false;
    if (!wanted.outputs.empty() && wanted.outputs.front() != channels) return false;
    m_layout.inputs = {channels};
    if (wanted.inputs.size() > 1) {
        const auto side = wanted.inputs[1];
        if (side != 1 && side != 2) return false;
        m_layout.inputs.push_back(side);
    }
    m_layout.outputs = {channels};
    accepted = m_layout;
    return true;
}

std::size_t EqualizerInstance::linearFftSize() const noexcept {
    const int resolution = std::clamp(int(std::lround(parameterValue(
        globalParameter(GlobalParam::LinearResolution)))), 0, 2);
    std::size_t size = std::size_t(2048) << resolution;
    if (m_sampleRate > 72000.0) size <<= 1;
    if (m_sampleRate > 144000.0) size <<= 1;
    return size;
}

std::uint32_t EqualizerInstance::latencySamples() const noexcept {
    const auto mode = enumValue(parameterValue(globalParameter(GlobalParam::ProcessingMode)),
                                ProcessingMode::LinearPhase);
    if (mode == ProcessingMode::AnalogPhase) return 16;
    if (mode == ProcessingMode::LinearPhase)
        return std::uint32_t(std::min<std::size_t>(
            linearFftSize(), std::numeric_limits<std::uint32_t>::max()));
    return 0;
}

bool EqualizerInstance::activate(const PluginProcessInfo& info) {
    m_sampleRate = info.sampleRate > 0.0 ? info.sampleRate : 48000.0;
    m_maxBlockSize = std::max<std::uint32_t>(1, info.maxBlockSize);
    for (std::uint32_t i = 0; i < kParameterCount; ++i) {
        m_targets[i] = m_values[i].load(std::memory_order_relaxed);
        m_smoothed[i] = m_targets[i];
    }

    double sum = 0.0;
    for (std::size_t i = 0; i < m_halfband.size(); ++i) {
        const double x = double(i) - 16.0;
        const double sinc = std::abs(x) < 1.0e-12
            ? 0.5 : std::sin(0.5 * kPi * x) / (kPi * x);
        const double window = 0.54 - 0.46 * std::cos(2.0 * kPi * double(i) / 32.0);
        m_halfband[i] = sinc * window;
        sum += m_halfband[i];
    }
    for (double& coefficient : m_halfband) coefficient /= sum;

    m_linearSize = linearFftSize();
    // Keep spectral dynamics responsive independently of the selected FFT
    // resolution.  The sqrt-Hann analysis/synthesis pair is overlap-add
    // normalised below, so non-power-of-two hop lengths are fine here.
    m_linearHop = std::clamp<std::size_t>(
        std::size_t(std::lround(m_sampleRate * 0.005)), 1, m_linearSize / 2);
    for (int channel = 0; channel < 2; ++channel) {
        m_linearInput[channel].assign(m_linearSize, 0.0);
        m_linearOutput[channel].assign(m_linearSize * 2, 0.0);
        m_linearSpectrum[channel].assign(m_linearSize, {});
    }
    m_linearWindow.resize(m_linearSize);
    for (std::size_t i = 0; i < m_linearSize; ++i) {
        const double hann = 0.5 - 0.5 * std::cos(2.0 * kPi * double(i) /
                                                double(m_linearSize));
        m_linearWindow[i] = std::sqrt(std::max(0.0, hann));
    }
    m_analyzerWork.assign(kAnalyzerSize, {});
    m_active = true;
    resetDsp();
    return true;
}

void EqualizerInstance::deactivate() {
    m_active = false;
    m_processing = false;
    resetDsp();
    for (auto& channel : m_linearInput) channel.clear();
    for (auto& channel : m_linearOutput) channel.clear();
    for (auto& channel : m_linearSpectrum) channel.clear();
    m_linearWindow.clear();
    m_analyzerWork.clear();
}

std::span<const ParameterInfo> EqualizerInstance::parameters() const noexcept {
    return parameterTable();
}

std::int32_t EqualizerInstance::parameterIndexForId(std::string_view id) const noexcept {
    for (const ParameterInfo& info : parameterTable())
        if (info.id == id) return std::int32_t(info.index);
    return -1;
}

double EqualizerInstance::parameterValue(std::uint32_t index) const noexcept {
    return index < kParameterCount
        ? m_values[index].load(std::memory_order_relaxed) : 0.0;
}

std::string EqualizerInstance::parameterText(std::uint32_t index,
                                             double plainValue) const {
    return equalizer::parameterText(index, plainValue);
}

double EqualizerInstance::clampParameter(std::uint32_t index, double value) noexcept {
    if (index >= kParameterCount || !std::isfinite(value)) return 0.0;
    const ParameterInfo& info = parametersImpl()[index];
    const double clamped = std::clamp(value, info.minValue, info.maxValue);
    return info.isStepped ? std::round(clamped) : clamped;
}

void EqualizerInstance::setParameterFromHost(std::uint32_t index,
                                             double plainValue) {
    if (index >= kParameterCount) return;
    const std::uint32_t beforeLatency = latencySamples();
    m_values[index].store(clampParameter(index, plainValue),
                          std::memory_order_relaxed);
    if ((index == globalParameter(GlobalParam::ProcessingMode) ||
         index == globalParameter(GlobalParam::LinearResolution)) &&
        latencySamples() != beforeLatency && m_listener) {
        m_listener->onLatencyChanged();
    }
}

double EqualizerInstance::Biquad::process(double input) noexcept {
    const double output = c.b0 * input + z1;
    z1 = c.b1 * input - c.a1 * output + z2;
    z2 = c.b2 * input - c.a2 * output;
    if (std::abs(z1) < 1.0e-30) z1 = 0.0;
    if (std::abs(z2) < 1.0e-30) z2 = 0.0;
    return std::isfinite(output) ? output : 0.0;
}

double EqualizerInstance::FilterChain::process(double input) noexcept {
    for (std::uint32_t i = 0; i < count; ++i) input = sections[i].process(input);
    return input;
}

void EqualizerInstance::FilterChain::reset() noexcept {
    for (Biquad& section : sections) section.reset();
}

double EqualizerInstance::FirState::push(
    double input, const std::array<double, 33>& coefficients) noexcept {
    history[cursor] = input;
    double output = 0.0;
    std::size_t at = cursor;
    for (double coefficient : coefficients) {
        output += coefficient * history[at];
        at = at == 0 ? history.size() - 1 : at - 1;
    }
    if (++cursor == history.size()) cursor = 0;
    return output;
}

int EqualizerInstance::slopeOrder(Slope slope) noexcept {
    static constexpr int orders[]{1, 2, 3, 4, 6, 8, 12, 16};
    return orders[std::clamp(int(slope), 0, 7)];
}

bool EqualizerInstance::supportsDynamics(FilterType type) noexcept {
    return type == FilterType::Bell || type == FilterType::LowShelf ||
           type == FilterType::HighShelf || type == FilterType::Tilt;
}

EqualizerInstance::Coefficients EqualizerInstance::design(
    FilterType type, double frequency, double q, double gainDb,
    double sampleRate) noexcept {
    Coefficients out;
    if (!(sampleRate > 0.0)) return out;
    frequency = std::clamp(frequency, 5.0, sampleRate * 0.475);
    q = std::clamp(q, 0.025, 40.0);
    const double omega = 2.0 * kPi * frequency / sampleRate;
    const double sn = std::sin(omega);
    const double cs = std::cos(omega);
    const double alpha = sn / (2.0 * q);
    const double a = std::pow(10.0, gainDb / 40.0);
    double b0 = 1.0, b1 = 0.0, b2 = 0.0;
    double a0 = 1.0, a1 = 0.0, a2 = 0.0;

    switch (type) {
        case FilterType::Bell:
            b0 = 1.0 + alpha * a;
            b1 = -2.0 * cs;
            b2 = 1.0 - alpha * a;
            a0 = 1.0 + alpha / a;
            a1 = -2.0 * cs;
            a2 = 1.0 - alpha / a;
            break;
        case FilterType::LowShelf: {
            const double rootA = std::sqrt(a);
            const double shelfAlpha = sn * std::sqrt(2.0) * 0.5;
            const double two = 2.0 * rootA * shelfAlpha;
            b0 = a * ((a + 1.0) - (a - 1.0) * cs + two);
            b1 = 2.0 * a * ((a - 1.0) - (a + 1.0) * cs);
            b2 = a * ((a + 1.0) - (a - 1.0) * cs - two);
            a0 = (a + 1.0) + (a - 1.0) * cs + two;
            a1 = -2.0 * ((a - 1.0) + (a + 1.0) * cs);
            a2 = (a + 1.0) + (a - 1.0) * cs - two;
            break;
        }
        case FilterType::HighShelf: {
            const double rootA = std::sqrt(a);
            const double shelfAlpha = sn * std::sqrt(2.0) * 0.5;
            const double two = 2.0 * rootA * shelfAlpha;
            b0 = a * ((a + 1.0) + (a - 1.0) * cs + two);
            b1 = -2.0 * a * ((a - 1.0) + (a + 1.0) * cs);
            b2 = a * ((a + 1.0) + (a - 1.0) * cs - two);
            a0 = (a + 1.0) - (a - 1.0) * cs + two;
            a1 = 2.0 * ((a - 1.0) - (a + 1.0) * cs);
            a2 = (a + 1.0) - (a - 1.0) * cs - two;
            break;
        }
        case FilterType::LowCut:
            b0 = (1.0 + cs) * 0.5;
            b1 = -(1.0 + cs);
            b2 = b0;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cs;
            a2 = 1.0 - alpha;
            break;
        case FilterType::HighCut:
            b0 = (1.0 - cs) * 0.5;
            b1 = 1.0 - cs;
            b2 = b0;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cs;
            a2 = 1.0 - alpha;
            break;
        case FilterType::Notch:
            b0 = 1.0;
            b1 = -2.0 * cs;
            b2 = 1.0;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cs;
            a2 = 1.0 - alpha;
            break;
        case FilterType::BandPass:
            b0 = alpha;
            b1 = 0.0;
            b2 = -alpha;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cs;
            a2 = 1.0 - alpha;
            break;
        case FilterType::AllPass:
            b0 = 1.0 - alpha;
            b1 = -2.0 * cs;
            b2 = 1.0 + alpha;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cs;
            a2 = 1.0 - alpha;
            break;
        case FilterType::Tilt:
            return design(FilterType::Bell, frequency, q, gainDb, sampleRate);
    }
    if (std::abs(a0) < 1.0e-12 || !std::isfinite(a0)) return {};
    out.b0 = b0 / a0;
    out.b1 = b1 / a0;
    out.b2 = b2 / a0;
    out.a1 = a1 / a0;
    out.a2 = a2 / a0;
    return out;
}

void EqualizerInstance::buildChain(FilterChain& chain, const BandState& band,
                                   double dynamicGainDb, double gainScale,
                                   double sampleRate) noexcept {
    const double gain = (band.gainDb + dynamicGainDb) * gainScale;
    chain.count = 0;
    if (band.type == FilterType::Tilt) {
        chain.count = 2;
        chain.sections[0].c = design(FilterType::LowShelf, band.frequency,
                                     band.q, -gain * 0.5, sampleRate);
        chain.sections[1].c = design(FilterType::HighShelf, band.frequency,
                                     band.q, gain * 0.5, sampleRate);
        return;
    }
    if (band.type != FilterType::LowCut && band.type != FilterType::HighCut &&
        band.type != FilterType::LowShelf && band.type != FilterType::HighShelf) {
        chain.count = 1;
        chain.sections[0].c = design(band.type, band.frequency, band.q,
                                     gain, sampleRate);
        return;
    }

    const int order = slopeOrder(band.slope);
    if (band.type == FilterType::LowShelf || band.type == FilterType::HighShelf) {
        const int sections = std::clamp((order + 1) / 2, 1, 8);
        chain.count = std::uint32_t(sections);
        for (int section = 0; section < sections; ++section)
            chain.sections[section].c = design(
                band.type, band.frequency, 0.70710678118,
                gain / double(sections), sampleRate);
        return;
    }

    std::uint32_t section = 0;
    if ((order & 1) != 0) {
        const double k = std::tan(kPi * std::clamp(
            band.frequency, 5.0, sampleRate * 0.475) / sampleRate);
        Coefficients first;
        if (band.type == FilterType::LowCut) {
            first.b0 = 1.0 / (1.0 + k);
            first.b1 = -first.b0;
        } else {
            first.b0 = k / (1.0 + k);
            first.b1 = first.b0;
        }
        first.a1 = (k - 1.0) / (k + 1.0);
        chain.sections[section++].c = first;
    }
    const int pairs = order / 2;
    for (int pair = 0; pair < pairs && section < chain.sections.size(); ++pair) {
        const double q = 1.0 / (2.0 * std::cos(
            kPi * double(2 * pair + 1) / double(2 * order)));
        chain.sections[section++].c = design(band.type, band.frequency, q,
                                             0.0, sampleRate);
    }
    chain.count = section;
}

std::complex<double> EqualizerInstance::coefficientResponse(
    const Coefficients& c, double omega) noexcept {
    const std::complex<double> z1 = std::polar(1.0, -omega);
    const std::complex<double> z2 = z1 * z1;
    const std::complex<double> denominator = 1.0 + c.a1 * z1 + c.a2 * z2;
    if (std::abs(denominator) < 1.0e-15) return {};
    return (c.b0 + c.b1 * z1 + c.b2 * z2) / denominator;
}

double EqualizerInstance::responseMagnitude(const BandState& band,
                                            double frequency,
                                            double dynamicGainDb,
                                            double gainScale,
                                            double sampleRate) noexcept {
    FilterChain chain;
    buildChain(chain, band, dynamicGainDb, gainScale, sampleRate);
    const double omega = 2.0 * kPi * std::clamp(frequency, 0.0,
                                                sampleRate * 0.5) / sampleRate;
    std::complex<double> response{1.0, 0.0};
    for (std::uint32_t section = 0; section < chain.count; ++section)
        response *= coefficientResponse(chain.sections[section].c, omega);
    return std::max(1.0e-12, std::abs(response));
}

BandState EqualizerInstance::bandState(std::uint32_t band) const noexcept {
    BandState state;
    if (band >= kBandCount) return state;
    auto value = [&](BandParam parameter) {
        return parameterValue(bandParameter(band, parameter));
    };
    state.enabled = value(BandParam::Enabled) >= 0.5;
    state.type = enumValue(value(BandParam::Type), FilterType::AllPass);
    state.frequency = value(BandParam::Frequency);
    state.gainDb = value(BandParam::Gain);
    state.q = value(BandParam::Q);
    state.slope = enumValue(value(BandParam::Slope), Slope::Db96);
    state.placement = enumValue(value(BandParam::Placement), Placement::Side);
    state.dynamicEnabled = value(BandParam::DynamicEnabled) >= 0.5;
    state.dynamicRangeDb = value(BandParam::DynamicRange);
    state.dynamicAuto = value(BandParam::DynamicAuto) >= 0.5;
    state.thresholdDb = value(BandParam::DynamicThreshold);
    state.attackMs = value(BandParam::DynamicAttack);
    state.releaseMs = value(BandParam::DynamicRelease);
    state.externalSidechain = value(BandParam::DynamicExternal) >= 0.5;
    state.detectorMode = enumValue(value(BandParam::DetectorMode), DetectorMode::Free);
    state.detectorLow = value(BandParam::DetectorLow);
    state.detectorHigh = value(BandParam::DetectorHigh);
    return state;
}

double EqualizerInstance::bandResponseDb(std::uint32_t band,
                                         double frequency) const noexcept {
    const BandState state = bandState(band);
    if (!state.enabled) return 0.0;
    const auto mode = enumValue(parameterValue(globalParameter(GlobalParam::ProcessingMode)),
                                ProcessingMode::LinearPhase);
    if (mode == ProcessingMode::LinearPhase && state.type == FilterType::AllPass)
        return 0.0;
    const double rate = mode == ProcessingMode::AnalogPhase
        ? m_sampleRate * 2.0 : m_sampleRate;
    const double scale = parameterValue(globalParameter(GlobalParam::GainScale));
    return gainToDb(responseMagnitude(state, frequency, 0.0, scale, rate));
}

double EqualizerInstance::responseDb(double frequency) const noexcept {
    double db = 0.0;
    for (std::uint32_t band = 0; band < kBandCount; ++band)
        db += bandResponseDb(band, frequency);
    return std::clamp(db, -120.0, 120.0);
}

void EqualizerInstance::applyEvent(const PluginEvent& event) noexcept {
    if (event.kind != PluginEvent::Kind::ParamValue ||
        event.paramIndex >= kParameterCount) return;
    m_targets[event.paramIndex] = clampParameter(event.paramIndex, event.value);
}

void EqualizerInstance::updateSmoothedParameters() noexcept {
    const auto table = parameterTable();
    for (std::uint32_t i = 0; i < kParameterCount; ++i) {
        if (table[i].isStepped) {
            m_smoothed[i] = m_targets[i];
        } else {
            m_smoothed[i] += (m_targets[i] - m_smoothed[i]) * 0.22;
            if (std::abs(m_targets[i] - m_smoothed[i]) < 1.0e-8)
                m_smoothed[i] = m_targets[i];
        }
    }
}

void EqualizerInstance::updateFilterCoefficients(double sampleRate) noexcept {
    const double gainScale = m_smoothed[globalParameter(GlobalParam::GainScale)];
    for (std::uint32_t band = 0; band < kBandCount; ++band) {
        const BandState state = stateFromValues(m_smoothed, band);
        for (int channel = 0; channel < 2; ++channel)
            buildChain(m_filters[band][channel], state, m_dynamicGainDb[band],
                       gainScale, sampleRate);

        double detectorFrequency = state.frequency;
        double detectorQ = std::clamp(state.q, 0.2, 12.0);
        if (state.detectorMode == DetectorMode::Free) {
            const double low = std::min(state.detectorLow, state.detectorHigh);
            const double high = std::max(state.detectorLow, state.detectorHigh);
            detectorFrequency = std::sqrt(std::max(10.0, low) *
                                          std::max(low + 1.0, high));
            detectorQ = std::clamp(detectorFrequency /
                                   std::max(10.0, high - low), 0.2, 12.0);
        }
        const Coefficients detector = design(FilterType::BandPass,
            detectorFrequency, detectorQ, 0.0, m_sampleRate);
        m_detectors[band][0].c = detector;
        m_detectors[band][1].c = detector;
    }
}

void EqualizerInstance::updateDynamics(const PluginProcessContext& context,
                                       std::uint32_t frame,
                                       double left, double right) noexcept {
    const bool hasSidechain = context.sidechainInputs &&
                              context.sidechainInputChannels > 0;
    double sideLeft = 0.0, sideRight = 0.0;
    if (hasSidechain) {
        sideLeft = context.sidechainInputs[0][frame];
        sideRight = context.sidechainInputs[
            std::min<std::uint16_t>(1, context.sidechainInputChannels - 1)][frame];
    }

    for (std::uint32_t band = 0; band < kBandCount; ++band) {
        const BandState state = stateFromValues(m_smoothed, band);
        double targetGain = 0.0;
        if (state.enabled && state.dynamicEnabled && supportsDynamics(state.type) &&
            (!state.externalSidechain || hasSidechain)) {
            double detectorLeft = state.externalSidechain ? sideLeft : left;
            double detectorRight = state.externalSidechain ? sideRight : right;
            if (m_layout.outputs.front() == 1) detectorRight = detectorLeft;
            if (state.placement == Placement::Mid) {
                detectorLeft = detectorRight =
                    (detectorLeft + detectorRight) * 0.70710678118;
            } else if (state.placement == Placement::Side) {
                detectorLeft = detectorRight =
                    (detectorLeft - detectorRight) * 0.70710678118;
            } else if (state.placement == Placement::Left) {
                detectorRight = detectorLeft;
            } else if (state.placement == Placement::Right) {
                detectorLeft = detectorRight;
            }

            const double filteredLeft = m_detectors[band][0].process(detectorLeft);
            const double filteredRight = m_detectors[band][1].process(detectorRight);
            const double energy = 0.5 * (filteredLeft * filteredLeft +
                                         filteredRight * filteredRight);
            const double rmsStep = 1.0 - std::exp(-1.0 / (0.010 * m_sampleRate));
            m_envelopes[band] += (energy - m_envelopes[band]) * rmsStep;
            const double levelDb = gainToDb(std::sqrt(std::max(0.0, m_envelopes[band])));
            const double baselineStep = 1.0 - std::exp(-1.0 / m_sampleRate);
            m_autoLevelsDb[band] += (levelDb - m_autoLevelsDb[band]) * baselineStep;
            const double threshold = state.dynamicAuto
                ? std::clamp(m_autoLevelsDb[band] - 6.0, -72.0, -6.0)
                : state.thresholdDb;
            const double over = levelDb - threshold;
            const double activity = over <= -3.0 ? 0.0
                : over >= 15.0 ? 1.0 : (over + 3.0) / 18.0;
            const double soft = activity * activity * (3.0 - 2.0 * activity);
            targetGain = state.dynamicRangeDb * soft;
        }

        double attackMs = state.attackMs;
        double releaseMs = state.releaseMs;
        if (state.dynamicAuto) {
            attackMs = std::clamp(18.0 * std::sqrt(1000.0 /
                std::max(20.0, state.frequency)), 2.0, 80.0);
            releaseMs = std::clamp(attackMs * 8.0, 40.0, 800.0);
        }
        const bool movingAway = std::abs(targetGain) > std::abs(m_dynamicGainDb[band]);
        const double timeMs = movingAway ? attackMs : releaseMs;
        const double step = 1.0 - std::exp(-1.0 /
            (std::max(0.1, timeMs) * 0.001 * m_sampleRate));
        m_dynamicGainDb[band] += (targetGain - m_dynamicGainDb[band]) * step;
        if (std::abs(m_dynamicGainDb[band]) < 1.0e-9) m_dynamicGainDb[band] = 0.0;
        m_dynamicTelemetry[band].store(float(m_dynamicGainDb[band]),
                                       std::memory_order_relaxed);
    }
}

void EqualizerInstance::processIirFrame(double& left, double& right,
                                        double /*sampleRate*/) noexcept {
    const int audition = m_auditionBand.load(std::memory_order_relaxed);
    const bool mono = m_layout.outputs.empty() || m_layout.outputs.front() == 1;
    for (std::uint32_t band = 0; band < kBandCount; ++band) {
        const BandState state = stateFromValues(m_smoothed, band);
        if (!state.enabled || (audition >= 0 && audition != int(band))) continue;
        if (mono) {
            left = m_filters[band][0].process(left);
            right = left;
            continue;
        }
        switch (state.placement) {
            case Placement::Stereo:
                left = m_filters[band][0].process(left);
                right = m_filters[band][1].process(right);
                break;
            case Placement::Left:
                left = m_filters[band][0].process(left);
                break;
            case Placement::Right:
                right = m_filters[band][1].process(right);
                break;
            case Placement::Mid: {
                double mid = (left + right) * 0.70710678118;
                const double side = (left - right) * 0.70710678118;
                mid = m_filters[band][0].process(mid);
                left = (mid + side) * 0.70710678118;
                right = (mid - side) * 0.70710678118;
                break;
            }
            case Placement::Side: {
                const double mid = (left + right) * 0.70710678118;
                double side = (left - right) * 0.70710678118;
                side = m_filters[band][1].process(side);
                left = (mid + side) * 0.70710678118;
                right = (mid - side) * 0.70710678118;
                break;
            }
        }
    }
}

void EqualizerInstance::fft(std::vector<std::complex<double>>& values,
                            bool inverse) noexcept {
    const std::size_t n = values.size();
    if (n < 2) return;
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(values[i], values[j]);
    }
    for (std::size_t length = 2; length <= n; length <<= 1) {
        const double angle = (inverse ? 2.0 : -2.0) * kPi / double(length);
        const std::complex<double> step{std::cos(angle), std::sin(angle)};
        for (std::size_t start = 0; start < n; start += length) {
            std::complex<double> phase{1.0, 0.0};
            for (std::size_t i = 0; i < length / 2; ++i) {
                const auto even = values[start + i];
                const auto odd = values[start + i + length / 2] * phase;
                values[start + i] = even + odd;
                values[start + i + length / 2] = even - odd;
                phase *= step;
            }
        }
    }
    if (inverse)
        for (auto& value : values) value /= double(n);
}

void EqualizerInstance::renderLinearTransform() noexcept {
    if (m_linearSize == 0 || m_linearSpectrum[0].size() != m_linearSize) return;
    for (int channel = 0; channel < 2; ++channel) {
        for (std::size_t i = 0; i < m_linearSize; ++i) {
            const std::size_t source = (m_linearInputPosition + i) % m_linearSize;
            m_linearSpectrum[channel][i] =
                m_linearInput[channel][source] * m_linearWindow[i];
        }
        fft(m_linearSpectrum[channel], false);
    }

    std::array<FilterChain, kBandCount> responses{};
    std::array<BandState, kBandCount> states{};
    const double gainScale = m_smoothed[globalParameter(GlobalParam::GainScale)];
    const int audition = m_auditionBand.load(std::memory_order_relaxed);
    for (std::uint32_t band = 0; band < kBandCount; ++band) {
        states[band] = stateFromValues(m_smoothed, band);
        if (states[band].enabled && states[band].type != FilterType::AllPass &&
            (audition < 0 || audition == int(band))) {
            buildChain(responses[band], states[band], m_dynamicGainDb[band],
                       gainScale, m_sampleRate);
        }
    }

    for (std::size_t bin = 0; bin <= m_linearSize / 2; ++bin) {
        const double omega = 2.0 * kPi * double(bin) / double(m_linearSize);
        std::complex<double> left = m_linearSpectrum[0][bin];
        std::complex<double> right = m_linearSpectrum[1][bin];
        for (std::uint32_t band = 0; band < kBandCount; ++band) {
            const BandState& state = states[band];
            if (!state.enabled || state.type == FilterType::AllPass ||
                (audition >= 0 && audition != int(band))) continue;
            std::complex<double> transfer{1.0, 0.0};
            for (std::uint32_t section = 0; section < responses[band].count; ++section)
                transfer *= coefficientResponse(responses[band].sections[section].c,
                                                omega);
            const double magnitude = std::abs(transfer);
            switch (state.placement) {
                case Placement::Stereo:
                    left *= magnitude; right *= magnitude; break;
                case Placement::Left:
                    left *= magnitude; break;
                case Placement::Right:
                    right *= magnitude; break;
                case Placement::Mid: {
                    auto mid = (left + right) * 0.70710678118;
                    const auto side = (left - right) * 0.70710678118;
                    mid *= magnitude;
                    left = (mid + side) * 0.70710678118;
                    right = (mid - side) * 0.70710678118;
                    break;
                }
                case Placement::Side: {
                    const auto mid = (left + right) * 0.70710678118;
                    auto side = (left - right) * 0.70710678118;
                    side *= magnitude;
                    left = (mid + side) * 0.70710678118;
                    right = (mid - side) * 0.70710678118;
                    break;
                }
            }
        }
        m_linearSpectrum[0][bin] = left;
        m_linearSpectrum[1][bin] = right;
        if (bin > 0 && bin < m_linearSize / 2) {
            m_linearSpectrum[0][m_linearSize - bin] = std::conj(left);
            m_linearSpectrum[1][m_linearSize - bin] = std::conj(right);
        }
    }

    for (int channel = 0; channel < 2; ++channel) {
        fft(m_linearSpectrum[channel], true);
        const std::size_t outputSize = m_linearOutput[channel].size();
        for (std::size_t i = 0; i < m_linearSize; ++i) {
            const std::size_t destination = (m_linearOutputPosition + i) % outputSize;
            const double overlapNormalisation =
                2.0 * double(m_linearHop) / double(m_linearSize);
            m_linearOutput[channel][destination] +=
                m_linearSpectrum[channel][i].real() * m_linearWindow[i] *
                overlapNormalisation;
        }
    }
}

void EqualizerInstance::processLinearFrame(double inputLeft, double inputRight,
                                           double& outputLeft,
                                           double& outputRight) noexcept {
    if (m_linearSize == 0 || m_linearOutput[0].empty()) {
        outputLeft = inputLeft;
        outputRight = inputRight;
        return;
    }
    m_linearInput[0][m_linearInputPosition] = inputLeft;
    m_linearInput[1][m_linearInputPosition] = inputRight;
    outputLeft = m_linearOutput[0][m_linearOutputPosition];
    outputRight = m_linearOutput[1][m_linearOutputPosition];
    m_linearOutput[0][m_linearOutputPosition] = 0.0;
    m_linearOutput[1][m_linearOutputPosition] = 0.0;
    m_linearInputPosition = (m_linearInputPosition + 1) % m_linearSize;
    m_linearOutputPosition = (m_linearOutputPosition + 1) %
                             m_linearOutput[0].size();
    ++m_linearSamples;
    if (m_linearHopCountdown > 0) --m_linearHopCountdown;
    if (m_linearHopCountdown == 0) {
        m_linearHopCountdown = m_linearHop;
        // The ring is zero-initialised, so the early frames naturally contain
        // left padding. Rendering them is required to preserve transients that
        // arrive during the first FFT window while still producing exactly
        // one-window latency.
        renderLinearTransform();
    }
}

void EqualizerInstance::updateAutoGain() noexcept {
    if (m_smoothed[globalParameter(GlobalParam::AutoGain)] < 0.5) {
        m_autoGainDb += (0.0 - m_autoGainDb) * 0.08;
        return;
    }
    double sum = 0.0;
    double weight = 0.0;
    for (int i = 0; i < 64; ++i) {
        const double frequency = 20.0 * std::pow(1000.0, double(i) / 63.0);
        const double pink = 1.0 / std::sqrt(std::max(20.0, frequency));
        sum += responseDb(frequency) * pink;
        weight += pink;
    }
    const double target = std::clamp(-(weight > 0.0 ? sum / weight : 0.0),
                                     -12.0, 12.0);
    m_autoGainDb += (target - m_autoGainDb) * 0.08;
}

void EqualizerInstance::renderSlice(const PluginProcessContext& context,
                                    std::uint32_t offset,
                                    std::uint32_t frames) noexcept {
    const bool sidePresent = context.sidechainInputs &&
                             context.sidechainInputChannels > 0;
    m_sidechainPresent.store(sidePresent, std::memory_order_relaxed);
    float inputPeak[2]{}, outputPeak[2]{};
    const auto mode = enumValue(m_smoothed[globalParameter(GlobalParam::ProcessingMode)],
                                ProcessingMode::LinearPhase);
    for (std::uint32_t i = 0; i < frames; ++i) {
        const std::uint32_t at = offset + i;
        double left = context.inputs && context.inputChannels > 0
            ? context.inputs[0][at] : 0.0;
        double right = context.inputs && context.inputChannels > 1
            ? context.inputs[1][at] : left;
        const double preLeft = left;
        const double preRight = right;
        inputPeak[0] = std::max(inputPeak[0], float(std::abs(left)));
        inputPeak[1] = std::max(inputPeak[1], float(std::abs(right)));

        if (m_controlCountdown == 0) {
            updateSmoothedParameters();
            updateFilterCoefficients(mode == ProcessingMode::AnalogPhase
                                         ? m_sampleRate * 2.0 : m_sampleRate);
            m_controlCountdown = 16;
        }
        --m_controlCountdown;
        if (m_autoGainCountdown == 0) {
            updateAutoGain();
            m_autoGainCountdown = 2048;
        }
        --m_autoGainCountdown;
        updateDynamics(context, at, left, right);

        if (mode == ProcessingMode::LinearPhase) {
            double outLeft = 0.0, outRight = 0.0;
            processLinearFrame(left, right, outLeft, outRight);
            left = outLeft;
            right = outRight;
        } else if (mode == ProcessingMode::AnalogPhase) {
            const double highInputs[2][2]{
                {m_analogFir[0][0].push(left * 2.0, m_halfband),
                 m_analogFir[0][1].push(right * 2.0, m_halfband)},
                {m_analogFir[0][0].push(0.0, m_halfband),
                 m_analogFir[0][1].push(0.0, m_halfband)}};
            double selectedLeft = 0.0, selectedRight = 0.0;
            for (int high = 0; high < 2; ++high) {
                double highLeft = highInputs[high][0];
                double highRight = highInputs[high][1];
                processIirFrame(highLeft, highRight, m_sampleRate * 2.0);
                selectedLeft = m_analogFir[1][0].push(highLeft, m_halfband);
                selectedRight = m_analogFir[1][1].push(highRight, m_halfband);
            }
            left = selectedLeft;
            right = selectedRight;
        } else {
            processIirFrame(left, right, m_sampleRate);
        }

        const double outputGain = dbToGain(
            m_smoothed[globalParameter(GlobalParam::OutputGain)] + m_autoGainDb);
        left *= outputGain;
        right *= outputGain;
        const double balance = m_smoothed[globalParameter(GlobalParam::OutputBalance)];
        if (balance > 0.0) left *= 1.0 - balance;
        if (balance < 0.0) right *= 1.0 + balance;
        if (m_smoothed[globalParameter(GlobalParam::PolarityInvert)] >= 0.5) {
            left = -left;
            right = -right;
        }
        if (!std::isfinite(left)) left = 0.0;
        if (!std::isfinite(right)) right = 0.0;

        const std::uint16_t channels = std::min<std::uint16_t>(
            context.outputChannels, 2);
        if (channels > 0) context.outputs[0][at] = float(left);
        if (channels > 1) context.outputs[1][at] = float(right);
        for (std::uint16_t channel = channels; channel < context.outputChannels; ++channel)
            context.outputs[channel][at] = channels ? context.outputs[0][at] : 0.0f;
        outputPeak[0] = std::max(outputPeak[0], float(std::abs(left)));
        outputPeak[1] = std::max(outputPeak[1], float(std::abs(right)));

        double sideLeft = 0.0, sideRight = 0.0;
        if (sidePresent) {
            sideLeft = context.sidechainInputs[0][at];
            sideRight = context.sidechainInputs[
                std::min<std::uint16_t>(1, context.sidechainInputChannels - 1)][at];
        }
        collectAnalyzer(preLeft, preRight, left, right, sideLeft, sideRight);
    }

    auto publishPeak = [](std::atomic<float>& destination, float value) noexcept {
        float before = destination.load(std::memory_order_relaxed);
        while (value > before && !destination.compare_exchange_weak(
            before, value, std::memory_order_relaxed)) {}
    };
    publishPeak(m_inputPeakLeft, inputPeak[0]);
    publishPeak(m_inputPeakRight, inputPeak[1]);
    publishPeak(m_outputPeakLeft, outputPeak[0]);
    publishPeak(m_outputPeakRight, outputPeak[1]);
}

PluginProcessDisposition EqualizerInstance::process(
    const PluginProcessContext& context) noexcept {
    if (!context.outputs || context.outputChannels == 0)
        return PluginProcessDisposition::Continue;
    std::uint32_t cursor = 0;
    for (const PluginEvent& event : context.inputEvents) {
        const std::uint32_t at = std::min(event.frameOffset, context.frames);
        if (at > cursor) {
            renderSlice(context, cursor, at - cursor);
            cursor = at;
        }
        applyEvent(event);
    }
    if (cursor < context.frames) renderSlice(context, cursor, context.frames - cursor);
    return PluginProcessDisposition::Continue;
}

void EqualizerInstance::resetDsp() noexcept {
    for (auto& band : m_filters)
        for (FilterChain& filter : band) filter.reset();
    for (auto& band : m_detectors)
        for (Biquad& detector : band) detector.reset();
    for (auto& stage : m_analogFir)
        for (FirState& state : stage) state.reset();
    m_analogPrevious = {};
    m_envelopes = {};
    m_dynamicGainDb = {};
    m_autoLevelsDb.fill(-48.0);
    m_autoGainDb = 0.0;
    m_controlCountdown = 0;
    m_autoGainCountdown = 0;
    for (auto& channel : m_linearInput) std::fill(channel.begin(), channel.end(), 0.0);
    for (auto& channel : m_linearOutput) std::fill(channel.begin(), channel.end(), 0.0);
    m_linearInputPosition = 0;
    m_linearOutputPosition = 0;
    m_linearHopCountdown = m_linearHop;
    m_linearSamples = 0;
    m_analyzerRing = {};
    m_analyzerPosition = 0;
    m_analyzerCountdown = kAnalyzerSize / 2;
    for (auto& value : m_preSpectrum) value.store(kSilenceDb, std::memory_order_relaxed);
    for (auto& value : m_postSpectrum) value.store(kSilenceDb, std::memory_order_relaxed);
    for (auto& value : m_sideSpectrum) value.store(kSilenceDb, std::memory_order_relaxed);
    for (auto& value : m_dynamicTelemetry) value.store(0.0f, std::memory_order_relaxed);
    updateFilterCoefficients(
        enumValue(m_smoothed[globalParameter(GlobalParam::ProcessingMode)],
                  ProcessingMode::LinearPhase) == ProcessingMode::AnalogPhase
            ? m_sampleRate * 2.0 : m_sampleRate);
}

void EqualizerInstance::reset() noexcept { resetDsp(); }

void EqualizerInstance::setAnalyzerConfig(const AnalyzerConfig& config) noexcept {
    std::uint32_t flags = 0;
    if (config.enabled) flags |= 1u;
    if (config.pre) flags |= 2u;
    if (config.post) flags |= 4u;
    if (config.sidechain) flags |= 8u;
    if (config.frozen) flags |= 16u;
    m_analyzerSpeed.store(std::clamp(config.speed, 0, 2),
                          std::memory_order_relaxed);
    m_analyzerTilt.store(std::clamp(config.tiltDbPerOctave, 0.0, 6.0),
                         std::memory_order_relaxed);
    m_analyzerFlags.store(flags, std::memory_order_release);
}

AnalyzerConfig EqualizerInstance::analyzerConfig() const noexcept {
    const std::uint32_t flags = m_analyzerFlags.load(std::memory_order_acquire);
    AnalyzerConfig config;
    config.enabled = (flags & 1u) != 0;
    config.pre = (flags & 2u) != 0;
    config.post = (flags & 4u) != 0;
    config.sidechain = (flags & 8u) != 0;
    config.frozen = (flags & 16u) != 0;
    config.speed = m_analyzerSpeed.load(std::memory_order_relaxed);
    config.tiltDbPerOctave = m_analyzerTilt.load(std::memory_order_relaxed);
    return config;
}

void EqualizerInstance::setAuditionBand(int band) noexcept {
    m_auditionBand.store(std::clamp(band, -1, int(kBandCount) - 1),
                         std::memory_order_release);
}

void EqualizerInstance::collectAnalyzer(double preLeft, double preRight,
                                        double postLeft, double postRight,
                                        double sideLeft, double sideRight) noexcept {
    const std::uint32_t flags = m_analyzerFlags.load(std::memory_order_acquire);
    if ((flags & 1u) == 0 || (flags & 16u) != 0 || m_analyzerWork.empty()) return;
    const double values[6]{preLeft, preRight, postLeft, postRight,
                           sideLeft, sideRight};
    for (int channel = 0; channel < 6; ++channel)
        m_analyzerRing[channel][m_analyzerPosition] = values[channel];
    m_analyzerPosition = (m_analyzerPosition + 1) % kAnalyzerSize;
    if (m_analyzerCountdown > 0) --m_analyzerCountdown;
    if (m_analyzerCountdown == 0) {
        m_analyzerCountdown = kAnalyzerSize / 2;
        publishAnalyzerFrame();
    }
}

void EqualizerInstance::publishAnalyzerFrame() noexcept {
    const std::uint32_t flags = m_analyzerFlags.load(std::memory_order_relaxed);
    const int speed = m_analyzerSpeed.load(std::memory_order_relaxed);
    const double smoothing = speed == 0 ? 0.75 : speed == 1 ? 0.48 : 0.24;
    const double tilt = m_analyzerTilt.load(std::memory_order_relaxed);
    auto publish = [&](int first, std::array<std::atomic<float>, kSpectrumBinCount>& target,
                       bool enabled) noexcept {
        if (!enabled) return;
        for (std::size_t i = 0; i < kAnalyzerSize; ++i) {
            const std::size_t source = (m_analyzerPosition + i) % kAnalyzerSize;
            const double mono = 0.5 * (m_analyzerRing[first][source] +
                                       m_analyzerRing[first + 1][source]);
            const double window = 0.5 - 0.5 * std::cos(
                2.0 * kPi * double(i) / double(kAnalyzerSize));
            m_analyzerWork[i] = mono * window;
        }
        fft(m_analyzerWork, false);
        for (std::size_t i = 0; i < kSpectrumBinCount; ++i) {
            const double frequency = 20.0 * std::pow(1000.0,
                double(i) / double(kSpectrumBinCount - 1));
            const std::size_t bin = std::clamp<std::size_t>(
                std::size_t(std::lround(frequency * double(kAnalyzerSize) /
                                        m_sampleRate)), 0, kAnalyzerSize / 2);
            const double magnitude = std::abs(m_analyzerWork[bin]) *
                                     (4.0 / double(kAnalyzerSize));
            const double tilted = gainToDb(magnitude) +
                                  tilt * std::log2(frequency / 1000.0);
            const float wanted = float(std::clamp(tilted, -120.0, 18.0));
            const float before = target[i].load(std::memory_order_relaxed);
            const float amount = wanted > before ? float(smoothing)
                                                  : float(smoothing * 0.35);
            target[i].store(before + (wanted - before) * amount,
                            std::memory_order_relaxed);
        }
    };
    publish(0, m_preSpectrum, (flags & 2u) != 0);
    publish(2, m_postSpectrum, (flags & 4u) != 0);
    publish(4, m_sideSpectrum, (flags & 8u) != 0);
}

Telemetry EqualizerInstance::consumeTelemetry() noexcept {
    Telemetry telemetry;
    for (std::size_t i = 0; i < kSpectrumBinCount; ++i) {
        telemetry.pre[i] = m_preSpectrum[i].load(std::memory_order_relaxed);
        telemetry.post[i] = m_postSpectrum[i].load(std::memory_order_relaxed);
        telemetry.sidechain[i] = m_sideSpectrum[i].load(std::memory_order_relaxed);
    }
    for (std::size_t i = 0; i < kBandCount; ++i)
        telemetry.dynamicGainDb[i] =
            m_dynamicTelemetry[i].load(std::memory_order_relaxed);
    telemetry.inputLeft = m_inputPeakLeft.exchange(0.0f, std::memory_order_relaxed);
    telemetry.inputRight = m_inputPeakRight.exchange(0.0f, std::memory_order_relaxed);
    telemetry.outputLeft = m_outputPeakLeft.exchange(0.0f, std::memory_order_relaxed);
    telemetry.outputRight = m_outputPeakRight.exchange(0.0f, std::memory_order_relaxed);
    telemetry.sidechainPresent = m_sidechainPresent.load(std::memory_order_relaxed);
    return telemetry;
}

void EqualizerInstance::captureComparison(char slot) {
    const std::scoped_lock lock(m_stateMutex);
    auto& target = slot == 'B' ? m_comparisonB : m_comparisonA;
    for (std::uint32_t i = 0; i < kParameterCount; ++i)
        target[i] = parameterValue(i);
}

std::array<double, kParameterCount> EqualizerInstance::comparison(char slot) const {
    const std::scoped_lock lock(m_stateMutex);
    return slot == 'B' ? m_comparisonB : m_comparisonA;
}

void EqualizerInstance::copyComparison(char from, char to) {
    const std::scoped_lock lock(m_stateMutex);
    const auto& source = from == 'B' ? m_comparisonB : m_comparisonA;
    auto& destination = to == 'B' ? m_comparisonB : m_comparisonA;
    destination = source;
}

void EqualizerInstance::setActiveComparison(char slot) noexcept {
    const std::scoped_lock lock(m_stateMutex);
    m_activeComparison = slot == 'B' ? 'B' : 'A';
}

char EqualizerInstance::activeComparison() const noexcept {
    const std::scoped_lock lock(m_stateMutex);
    return m_activeComparison;
}

void EqualizerInstance::setPresetReference(std::string kind, std::string name) {
    if (kind != "factory" && kind != "user" && kind != "custom") kind = "custom";
    if (name.empty()) name = "Custom";
    const std::scoped_lock lock(m_stateMutex);
    m_presetKind = std::move(kind);
    m_presetName = std::move(name);
}

std::pair<std::string, std::string> EqualizerInstance::presetReference() const {
    const std::scoped_lock lock(m_stateMutex);
    return {m_presetKind, m_presetName};
}

bool EqualizerInstance::saveState(std::vector<std::uint8_t>& out) const {
    json document;
    document["version"] = kStateVersion;
    json values = json::object();
    for (const ParameterInfo& info : parameterTable())
        values[info.id] = parameterValue(info.index);
    document["params"] = std::move(values);
    {
        const std::scoped_lock lock(m_stateMutex);
        document["preset"] = {{"kind", m_presetKind}, {"name", m_presetName}};
        document["comparison"]["active"] = std::string(1, m_activeComparison);
        document["comparison"]["a"] = m_comparisonA;
        document["comparison"]["b"] = m_comparisonB;
    }
    const std::string text = document.dump();
    out.assign(text.begin(), text.end());
    return true;
}

bool EqualizerInstance::loadState(std::span<const std::uint8_t> state) {
    if (state.empty()) return false;
    const json document = json::parse(state.begin(), state.end(), nullptr, false);
    if (document.is_discarded() || !document.is_object()) return false;
    const std::uint32_t oldLatency = latencySamples();
    for (const ParameterInfo& info : parameterTable())
        m_values[info.index].store(info.defaultValue, std::memory_order_relaxed);
    if (const auto params = document.find("params");
        params != document.end() && params->is_object()) {
        for (const auto& [id, value] : params->items()) {
            if (!value.is_number()) continue;
            const std::int32_t index = parameterIndexForId(id);
            if (index < 0) continue;
            m_values[std::uint32_t(index)].store(
                clampParameter(std::uint32_t(index), value.get<double>()),
                std::memory_order_relaxed);
        }
    }
    {
        const std::scoped_lock lock(m_stateMutex);
        if (const auto preset = document.find("preset");
            preset != document.end() && preset->is_object()) {
            m_presetKind = preset->value("kind", "custom");
            m_presetName = preset->value("name", "Custom");
        } else {
            m_presetKind = "custom";
            m_presetName = "Custom";
        }
        for (std::uint32_t i = 0; i < kParameterCount; ++i) {
            m_comparisonA[i] = parameterValue(i);
            m_comparisonB[i] = parameterValue(i);
        }
        if (const auto comparisonState = document.find("comparison");
            comparisonState != document.end() && comparisonState->is_object()) {
            const std::string active = comparisonState->value("active", "A");
            m_activeComparison = active == "B" ? 'B' : 'A';
            auto readSnapshot = [&](const char* key,
                                    std::array<double, kParameterCount>& snapshot) {
                const auto found = comparisonState->find(key);
                if (found == comparisonState->end() || !found->is_array()) return;
                const std::size_t count = std::min<std::size_t>(found->size(),
                                                               kParameterCount);
                for (std::size_t i = 0; i < count; ++i)
                    if ((*found)[i].is_number()) snapshot[i] =
                        clampParameter(std::uint32_t(i), (*found)[i].get<double>());
            };
            readSnapshot("a", m_comparisonA);
            readSnapshot("b", m_comparisonB);
        }
    }
    if (latencySamples() != oldLatency && m_listener) m_listener->onLatencyChanged();
    return true;
}

} // namespace daw::plugins::equalizer
