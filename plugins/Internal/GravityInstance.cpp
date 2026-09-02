#include "Internal/GravityInstance.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>

namespace daw::plugins::gravity {
namespace {

using json = nlohmann::json;

constexpr std::string_view kUid = "daw.gravity";
constexpr int kStateVersion = 2;
constexpr double kPi = std::numbers::pi_v<double>;

const std::array<ParameterInfo, kParameterCount>& parametersImpl() {
    static const std::array<ParameterInfo, kParameterCount> table = [] {
        std::array<ParameterInfo, kParameterCount> out{};
        auto add = [&](Param parameter, std::string id, std::string name,
                       std::string unit, double minimum, double maximum,
                       double initial, bool stepped = false) {
            const std::uint32_t index = std::uint32_t(parameter);
            out[index] = ParameterInfo{index, std::move(id), std::move(name),
                                       std::move(unit), minimum, maximum,
                                       initial, true, stepped, false};
        };
        add(Param::Gravity, "gravity", "Gravity", "%", 0.0, 1.0, 0.5);
        add(Param::Pitch, "pitch", "Pitch", "st", -12.0, 12.0, 0.0);
        add(Param::Feedback, "feedback", "Feedback", "%", 0.0, 1.0, 0.5);
        add(Param::Decay, "decay", "Decay", "s", 0.1, 20.0, 6.0);
        add(Param::Size, "size", "Size", "%", 0.0, 1.0, 0.6);
        add(Param::Algorithm, "algorithm", "Algorithm", {}, 0.0, 5.0, 0.0, true);
        add(Param::TimingSync, "timing.sync", "Tempo Sync", {}, 0.0, 1.0, 1.0, true);
        add(Param::TimingDivision, "timing.division", "Division", {}, 0.0, 10.0, 2.0, true);
        add(Param::TimingMs, "timing.ms", "Delay", "ms", 20.0, 2000.0, 250.0);
        add(Param::Mass, "mass", "Mass", "%", 0.0, 1.0, 0.5);
        add(Param::Motion, "motion", "Motion", "%", 0.0, 1.0, 0.0);
        add(Param::Density, "density", "Density", "%", 0.0, 1.0, 0.5);
        add(Param::Diffusion, "diffusion", "Diffusion", "%", 0.0, 1.0, 0.5);
        add(Param::Damping, "damping", "Damping", "%", 0.0, 1.0, 0.5);
        add(Param::Reverse, "reverse", "Reverse", "%", 0.0, 1.0, 0.0);
        add(Param::StereoWidth, "stereo.width", "Stereo Width", "%", 0.0, 1.0, 0.5);
        add(Param::StereoInput, "stereo.input", "Source Stereo", "%", 0.0, 1.0, 0.0);
        add(Param::Ducking, "ducking", "Ducking", "%", 0.0, 1.0, 0.0);
        add(Param::Transient, "transient", "Transient", "%", 0.0, 1.0, 0.0);
        add(Param::Drive, "drive", "Drive", "%", 0.0, 1.0, 0.0);
        add(Param::PitchSpread, "pitch.spread", "Pitch Spread", "st", 0.0, 12.0, 0.0);
        add(Param::PitchSnap, "pitch.snap", "Pitch Snap", {}, 0.0, 3.0, 0.0, true);
        add(Param::FeedbackLowcut, "feedback.lowcut", "Feedback Low Cut", "Hz", 0.0, 1.0, 0.0);
        add(Param::DetectorSource, "detector.source", "Detector Source", {}, 0.0, 2.0, 2.0, true);
        return out;
    }();
    return table;
}

const std::array<FactoryPreset, 16>& presetsImpl() {
    using A = Algorithm;
    using D = Division;
    static const std::array<FactoryPreset, 16> presets = [] {
        std::array<FactoryPreset, 16> out{};
        auto core = [&](std::size_t index, std::string_view name,
                        double gravity, double pitch, double feedback,
                        double decay, double size, A algorithm, D division,
                        double milliseconds) -> FactoryPreset& {
            FactoryPreset& preset = out[index];
            preset.name = name;
            for (std::uint32_t i = 0; i < kParameterCount; ++i)
                preset.values[i] = parametersImpl()[i].defaultValue;
            auto put = [&](Param parameter, double value) {
                preset.values[std::uint32_t(parameter)] = value;
            };
            put(Param::Gravity, gravity);
            put(Param::Pitch, pitch);
            put(Param::Feedback, feedback);
            put(Param::Decay, decay);
            put(Param::Size, size);
            put(Param::Algorithm, double(algorithm));
            put(Param::TimingSync, 1.0);
            put(Param::TimingDivision, double(division));
            put(Param::TimingMs, milliseconds);
            return preset;
        };
        auto put = [](FactoryPreset& preset, Param parameter, double value) {
            preset.values[std::uint32_t(parameter)] = value;
        };

        core(0, "WAVEFARERS", 0.50, 0.0, 0.50, 6.0, 0.60, A::Orbit, D::Eighth, 250.0);
        core(1, "EVENT HORIZON", 0.78, -2.0, 0.68, 8.0, 0.72, A::Fall, D::Quarter, 500.0);
        core(2, "RED SHIFT", 0.62, -5.0, 0.55, 5.0, 0.48, A::Fall, D::EighthDotted, 375.0);
        core(3, "ASCENSION", 0.65, 3.0, 0.60, 7.0, 0.70, A::Rise, D::Eighth, 250.0);
        core(4, "HOLLOW LIGHT", 0.76, 0.0, 0.72, 12.0, 0.85, A::Void, D::Quarter, 500.0);
        core(5, "SINGULARITY", 0.88, -3.0, 0.80, 4.0, 0.28, A::Collapse, D::Sixteenth, 125.0);
        core(6, "WEIGHTLESS", 0.58, 1.0, 0.48, 14.0, 0.90, A::ZeroG, D::Quarter, 500.0);
        core(7, "VOCAL HALO", 0.28, 0.0, 0.25, 2.5, 0.35, A::Orbit, D::Eighth, 250.0);

        FactoryPreset& piano = core(8, "PIANO BLOOM", 0.42, 0.0, 0.38, 5.5, 0.72, A::Orbit, D::EighthDotted, 375.0);
        put(piano, Param::Mass, 0.60); put(piano, Param::Motion, 0.22);
        put(piano, Param::Density, 0.70); put(piano, Param::Diffusion, 0.72);
        put(piano, Param::Damping, 0.62); put(piano, Param::Reverse, 0.08);
        put(piano, Param::StereoWidth, 0.68); put(piano, Param::StereoInput, 0.85);
        put(piano, Param::Ducking, 0.60); put(piano, Param::Transient, 0.55);
        put(piano, Param::Drive, 0.05); put(piano, Param::PitchSpread, 7.0);
        put(piano, Param::PitchSnap, double(PitchSnap::Perfect));
        put(piano, Param::FeedbackLowcut, 0.18);

        FactoryPreset& drums = core(9, "DRUM AURA", 0.34, 0.0, 0.28, 2.2, 0.22, A::Orbit, D::Sixteenth, 125.0);
        put(drums, Param::Mass, 0.25); put(drums, Param::Motion, 0.32);
        put(drums, Param::Density, 0.38); put(drums, Param::Diffusion, 0.35);
        put(drums, Param::Damping, 0.42); put(drums, Param::Reverse, 0.12);
        put(drums, Param::StereoWidth, 0.72); put(drums, Param::StereoInput, 0.50);
        put(drums, Param::Ducking, 0.72); put(drums, Param::Transient, 0.82);
        put(drums, Param::Drive, 0.15); put(drums, Param::PitchSpread, 5.0);
        put(drums, Param::PitchSnap, double(PitchSnap::Perfect));
        put(drums, Param::FeedbackLowcut, 0.25);

        FactoryPreset& guitar = core(10, "GUITAR TIDES", 0.52, 0.0, 0.50, 7.0, 0.68, A::ZeroG, D::Quarter, 500.0);
        put(guitar, Param::Mass, 0.55); put(guitar, Param::Motion, 0.55);
        put(guitar, Param::Density, 0.62); put(guitar, Param::Diffusion, 0.72);
        put(guitar, Param::Damping, 0.58); put(guitar, Param::Reverse, 0.22);
        put(guitar, Param::StereoWidth, 0.76); put(guitar, Param::StereoInput, 1.0);
        put(guitar, Param::Ducking, 0.40); put(guitar, Param::Transient, 0.25);
        put(guitar, Param::Drive, 0.08); put(guitar, Param::PitchSpread, 7.0);
        put(guitar, Param::PitchSnap, double(PitchSnap::Perfect));
        put(guitar, Param::FeedbackLowcut, 0.15);

        FactoryPreset& bass = core(11, "BASS SHADOW", 0.28, -2.0, 0.30, 3.5, 0.38, A::Fall, D::Eighth, 250.0);
        put(bass, Param::Mass, 0.75); put(bass, Param::Motion, 0.18);
        put(bass, Param::Density, 0.35); put(bass, Param::Diffusion, 0.42);
        put(bass, Param::Damping, 0.72); put(bass, Param::Reverse, 0.02);
        put(bass, Param::StereoWidth, 0.35); put(bass, Param::StereoInput, 0.20);
        put(bass, Param::Ducking, 0.65); put(bass, Param::Transient, 0.40);
        put(bass, Param::Drive, 0.18); put(bass, Param::PitchSnap, double(PitchSnap::Chromatic));
        put(bass, Param::FeedbackLowcut, 0.55);

        FactoryPreset& vocal = core(12, "VOCAL AURORA", 0.32, 0.0, 0.25, 4.5, 0.58, A::Rise, D::Eighth, 250.0);
        put(vocal, Param::Mass, 0.45); put(vocal, Param::Motion, 0.20);
        put(vocal, Param::Density, 0.55); put(vocal, Param::Diffusion, 0.68);
        put(vocal, Param::Damping, 0.60); put(vocal, Param::Reverse, 0.06);
        put(vocal, Param::StereoWidth, 0.70); put(vocal, Param::StereoInput, 1.0);
        put(vocal, Param::Ducking, 0.70); put(vocal, Param::Transient, 0.35);
        put(vocal, Param::Drive, 0.02); put(vocal, Param::PitchSpread, 7.0);
        put(vocal, Param::PitchSnap, double(PitchSnap::Perfect));
        put(vocal, Param::FeedbackLowcut, 0.22);

        FactoryPreset& synth = core(13, "SYNTH NEBULA", 0.70, 0.0, 0.65, 13.0, 0.88, A::ZeroG, D::Quarter, 500.0);
        put(synth, Param::Mass, 0.55); put(synth, Param::Motion, 0.65);
        put(synth, Param::Density, 0.90); put(synth, Param::Diffusion, 0.92);
        put(synth, Param::Damping, 0.55); put(synth, Param::Reverse, 0.32);
        put(synth, Param::StereoWidth, 0.85); put(synth, Param::StereoInput, 1.0);
        put(synth, Param::Ducking, 0.25); put(synth, Param::Transient, 0.08);
        put(synth, Param::Drive, 0.10); put(synth, Param::PitchSpread, 12.0);
        put(synth, Param::PitchSnap, double(PitchSnap::Octave));
        put(synth, Param::FeedbackLowcut, 0.12);

        FactoryPreset& strings = core(14, "STRING DUST", 0.48, 0.0, 0.42, 9.0, 0.75, A::Void, D::EighthDotted, 375.0);
        put(strings, Param::Mass, 0.38); put(strings, Param::Motion, 0.48);
        put(strings, Param::Density, 0.72); put(strings, Param::Diffusion, 0.84);
        put(strings, Param::Damping, 0.60); put(strings, Param::Reverse, 0.48);
        put(strings, Param::StereoWidth, 0.80); put(strings, Param::StereoInput, 1.0);
        put(strings, Param::Ducking, 0.35); put(strings, Param::Transient, 0.20);
        put(strings, Param::Drive, 0.03); put(strings, Param::PitchSpread, 7.0);
        put(strings, Param::PitchSnap, double(PitchSnap::Perfect));
        put(strings, Param::FeedbackLowcut, 0.10);

        FactoryPreset& binary = core(15, "BINARY STARS", 0.72, 0.0, 0.58, 10.0, 0.65, A::Rise, D::EighthDotted, 375.0);
        put(binary, Param::Mass, 0.48); put(binary, Param::Motion, 0.42);
        put(binary, Param::Density, 0.75); put(binary, Param::Diffusion, 0.70);
        put(binary, Param::Damping, 0.48); put(binary, Param::Reverse, 0.15);
        put(binary, Param::StereoWidth, 0.90); put(binary, Param::StereoInput, 0.80);
        put(binary, Param::Ducking, 0.30); put(binary, Param::Transient, 0.25);
        put(binary, Param::Drive, 0.08); put(binary, Param::PitchSpread, 12.0);
        put(binary, Param::PitchSnap, double(PitchSnap::Octave));
        put(binary, Param::FeedbackLowcut, 0.20);
        return out;
    }();
    return presets;
}

const char* algorithmName(int value) noexcept {
    static constexpr const char* names[] = {
        "Orbit", "Fall", "Rise", "Void", "Collapse", "Zero G"};
    return names[std::clamp(value, 0, 5)];
}

const char* divisionName(int value) noexcept {
    static constexpr const char* names[] = {
        "1/16", "1/8T", "1/8", "1/8D", "1/4", "1/32", "1/16T",
        "1/4T", "1/4D", "1/2", "1/1"};
    return names[std::clamp(value, 0, 10)];
}

double divisionQuarters(int value) noexcept {
    static constexpr double quarters[] = {
        0.25, 1.0 / 3.0, 0.5, 0.75, 1.0, 0.125,
        1.0 / 6.0, 2.0 / 3.0, 1.5, 2.0, 4.0};
    return quarters[std::clamp(value, 0, 10)];
}

const char* pitchSnapName(int value) noexcept {
    static constexpr const char* names[] = {"Off", "Chromatic", "Perfect", "Octave"};
    return names[std::clamp(value, 0, 3)];
}

const char* detectorSourceName(int value) noexcept {
    static constexpr const char* names[] = {"Main", "Sidechain", "Auto"};
    return names[std::clamp(value, 0, 2)];
}

double lowcutFrequency(double value) noexcept {
    return value <= 0.0 ? 0.0 : 30.0 * std::pow(40.0, std::clamp(value, 0.0, 1.0));
}

double snapPitch(double pitch, PitchSnap mode) noexcept {
    if (mode == PitchSnap::Off) return pitch;
    if (mode == PitchSnap::Chromatic) return std::round(pitch);
    const auto nearest = [pitch](const auto& values) {
        double result = values.front();
        double distance = std::abs(pitch - result);
        for (double candidate : values) {
            const double next = std::abs(pitch - candidate);
            if (next < distance) {
                result = candidate;
                distance = next;
            }
        }
        return result;
    };
    if (mode == PitchSnap::Perfect)
        return nearest(std::array{-12.0, -7.0, 0.0, 7.0, 12.0});
    return nearest(std::array{-12.0, 0.0, 12.0});
}

} // namespace

std::span<const ParameterInfo> parameterTable() noexcept { return parametersImpl(); }

std::span<const FactoryPreset> factoryPresets() noexcept { return presetsImpl(); }

std::string parameterText(std::uint32_t index, double value) {
    if (index >= kParameterCount) return {};
    char text[48]{};
    switch (Param(index)) {
        case Param::Gravity:
        case Param::Feedback:
        case Param::Size:
        case Param::Mass:
        case Param::Motion:
        case Param::Density:
        case Param::Diffusion:
        case Param::Damping:
        case Param::Reverse:
        case Param::StereoWidth:
        case Param::StereoInput:
        case Param::Ducking:
        case Param::Transient:
        case Param::Drive:
            std::snprintf(text, sizeof(text), "%.0f%%", value * 100.0);
            break;
        case Param::Pitch:
            std::snprintf(text, sizeof(text), "%+.1f st", value);
            break;
        case Param::Decay:
            std::snprintf(text, sizeof(text), "%.2f s", value);
            break;
        case Param::Algorithm:
            return algorithmName(int(std::lround(value)));
        case Param::TimingSync:
            return value >= 0.5 ? "Sync" : "Free";
        case Param::TimingDivision:
            return divisionName(int(std::lround(value)));
        case Param::TimingMs:
            std::snprintf(text, sizeof(text), "%.0f ms", value);
            break;
        case Param::PitchSpread:
            std::snprintf(text, sizeof(text), "+/-%.1f st", value);
            break;
        case Param::PitchSnap:
            return pitchSnapName(int(std::lround(value)));
        case Param::FeedbackLowcut: {
            const double hz = lowcutFrequency(value);
            if (hz <= 0.0) return "Off";
            std::snprintf(text, sizeof(text), "%.0f Hz", hz);
            break;
        }
        case Param::DetectorSource:
            return detectorSourceName(int(std::lround(value)));
        case Param::Count:
            return {};
    }
    return text;
}

GravityInstance::GravityInstance() : m_descriptor(staticDescriptor()) {
    const auto table = parameterTable();
    for (std::uint32_t i = 0; i < kParameterCount; ++i) {
        m_values[i].store(table[i].defaultValue, std::memory_order_relaxed);
        m_smoothed[i] = table[i].defaultValue;
    }
}

const PluginDescriptor& GravityInstance::staticDescriptor() noexcept {
    static const PluginDescriptor descriptor = [] {
        PluginDescriptor d;
        d.format = Format::Internal;
        d.uid = std::string(kUid);
        d.path = std::string(kUid);
        d.name = "Gravity";
        d.vendor = "VLT Studio Pro";
        d.version = "2.0";
        d.stateSchemaVersion = kStateVersion;
        d.category = "Effect";
        d.isInstrument = false;
        d.hasEditor = false;
        d.wantsMidi = false;
        d.mainInputChannels = 2;
        d.mainOutputChannels = 2;
        return d;
    }();
    return descriptor;
}

std::string_view GravityInstance::uid() noexcept { return kUid; }

bool GravityInstance::setBusLayout(const PluginBusLayout& wanted,
                                   PluginBusLayout& accepted) {
    if (wanted.inputs.size() > 2 || wanted.outputs.size() > 1) return false;
    std::uint16_t channels = 2;
    if (!wanted.inputs.empty()) channels = wanted.inputs.front();
    if (channels != 1 && channels != 2) return false;
    if (!wanted.outputs.empty() && wanted.outputs.front() != channels) return false;
    m_layout.inputs = {channels};
    if (wanted.inputs.size() > 1) {
        const std::uint16_t sidechain = wanted.inputs[1];
        if (sidechain != 1 && sidechain != 2) return false;
        m_layout.inputs.push_back(sidechain);
    }
    m_layout.outputs = {channels};
    accepted = m_layout;
    return true;
}

bool GravityInstance::activate(const PluginProcessInfo& info) {
    m_sampleRate = info.sampleRate > 0.0 ? info.sampleRate : 48000.0;
    m_maxBlockSize = std::max<std::uint32_t>(1, info.maxBlockSize);
    const std::size_t captureFrames = std::size_t(std::ceil(m_sampleRate * 2.5)) +
                                      m_maxBlockSize + 8;
    for (auto& channel : m_capture) channel.assign(captureFrames, 0.0f);

    static constexpr double delays[] = {0.0297, 0.0371, 0.0411, 0.0437};
    for (int channel = 0; channel < 2; ++channel) {
        for (int i = 0; i < 4; ++i) {
            const std::size_t frames = std::max<std::size_t>(1,
                std::size_t(std::lround(m_sampleRate * delays[i])) +
                    std::size_t(channel * (i * 2 + 1)));
            m_diffusers[channel][i].prepare(frames);
        }
    }
    static constexpr double tankDelays[] = {0.089, 0.113, 0.157, 0.211};
    for (int channel = 0; channel < 2; ++channel) {
        for (int i = 0; i < 4; ++i) {
            const double offset = channel == 0 ? 0.0 : 0.0017 * double(i + 1);
            m_tank[channel][i].prepare(std::size_t(std::lround(
                m_sampleRate * (tankDelays[i] + offset))));
        }
    }
    for (std::uint32_t i = 0; i < kParameterCount; ++i)
        m_smoothed[i] = m_values[i].load(std::memory_order_relaxed);
    m_active = true;
    resetDsp();
    return true;
}

void GravityInstance::deactivate() {
    m_active = false;
    m_processing = false;
    resetDsp();
    for (auto& channel : m_capture) channel.clear();
    for (auto& channel : m_diffusers)
        for (Allpass& allpass : channel) allpass.line.clear();
    for (auto& channel : m_tank)
        for (TankLine& line : channel) line.line.clear();
}

std::span<const ParameterInfo> GravityInstance::parameters() const noexcept {
    return parameterTable();
}

std::int32_t GravityInstance::parameterIndexForId(std::string_view id) const noexcept {
    for (const ParameterInfo& info : parameterTable()) {
        if (info.id == id) return std::int32_t(info.index);
    }
    return -1;
}

double GravityInstance::parameterValue(std::uint32_t index) const noexcept {
    return index < kParameterCount
        ? m_values[index].load(std::memory_order_relaxed)
        : 0.0;
}

std::string GravityInstance::parameterText(std::uint32_t index,
                                           double plainValue) const {
    return gravity::parameterText(index, plainValue);
}

double GravityInstance::clampParameter(std::uint32_t index, double value) noexcept {
    if (index >= kParameterCount || !std::isfinite(value)) return 0.0;
    const ParameterInfo& info = parametersImpl()[index];
    const double clamped = std::clamp(value, info.minValue, info.maxValue);
    return info.isStepped ? std::round(clamped) : clamped;
}

void GravityInstance::setParameterFromHost(std::uint32_t index, double plainValue) {
    if (index >= kParameterCount) return;
    m_values[index].store(clampParameter(index, plainValue),
                          std::memory_order_relaxed);
}

bool GravityInstance::saveState(std::vector<std::uint8_t>& out) const {
    json document;
    document["version"] = kStateVersion;
    {
        const std::scoped_lock lock(m_presetMutex);
        document["preset"] = {{"kind", m_presetKind}, {"name", m_presetName}};
    }
    json values = json::object();
    for (const ParameterInfo& info : parameterTable())
        values[info.id] = parameterValue(info.index);
    document["params"] = std::move(values);
    const std::string text = document.dump();
    out.assign(text.begin(), text.end());
    return true;
}

bool GravityInstance::loadState(std::span<const std::uint8_t> state) {
    if (state.empty()) return false;
    const json document = json::parse(state.begin(), state.end(), nullptr, false);
    if (document.is_discarded() || !document.is_object()) return false;
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
    if (const auto preset = document.find("preset"); preset != document.end()) {
        if (preset->is_number_integer()) {
            setLastPreset(preset->get<int>());
        } else if (preset->is_object()) {
            setPresetReference(preset->value("kind", "custom"),
                               preset->value("name", "Custom"));
        } else {
            setLastPreset(0);
        }
    } else {
        setLastPreset(0);
    }
    m_frozen.store(false, std::memory_order_release);
    m_clearRequested.store(true, std::memory_order_release);
    return true;
}

void GravityInstance::setLastPreset(int index) {
    const int last = int(factoryPresets().size()) - 1;
    const int clamped = std::clamp(index, 0, std::max(0, last));
    m_lastPreset.store(clamped, std::memory_order_relaxed);
    setPresetReference("factory", std::string(factoryPresets()[std::size_t(clamped)].name));
}

void GravityInstance::setPresetReference(std::string kind, std::string name) {
    if (kind != "factory" && kind != "user" && kind != "custom") kind = "custom";
    if (name.empty()) name = "Custom";
    const bool factory = kind == "factory";
    const std::string factoryName = name;
    {
        const std::scoped_lock lock(m_presetMutex);
        m_presetKind = std::move(kind);
        m_presetName = std::move(name);
    }
    if (factory) {
        const auto presets = factoryPresets();
        for (int i = 0; i < int(presets.size()); ++i) {
            if (presets[std::size_t(i)].name == factoryName) {
                m_lastPreset.store(i, std::memory_order_relaxed);
                break;
            }
        }
    }
}

std::pair<std::string, std::string> GravityInstance::presetReference() const {
    const std::scoped_lock lock(m_presetMutex);
    return {m_presetKind, m_presetName};
}

void GravityInstance::Allpass::prepare(std::size_t frames) {
    line.assign(std::max<std::size_t>(1, frames), 0.0f);
    cursor = 0;
}

void GravityInstance::Allpass::clear() noexcept {
    std::fill(line.begin(), line.end(), 0.0f);
    cursor = 0;
}

float GravityInstance::Allpass::process(float input, float feedback) noexcept {
    if (line.empty()) return input;
    const float delayed = line[cursor];
    const float output = delayed - feedback * input;
    line[cursor] = input + feedback * output;
    if (++cursor == line.size()) cursor = 0;
    return output;
}

void GravityInstance::TankLine::prepare(std::size_t frames) {
    line.assign(std::max<std::size_t>(1, frames), 0.0f);
    cursor = 0;
}

void GravityInstance::TankLine::clear() noexcept {
    std::fill(line.begin(), line.end(), 0.0f);
    cursor = 0;
}

float GravityInstance::TankLine::read() const noexcept {
    return line.empty() ? 0.0f : line[cursor];
}

void GravityInstance::TankLine::write(float value) noexcept {
    if (line.empty()) return;
    line[cursor] = value;
    if (++cursor == line.size()) cursor = 0;
}

double GravityInstance::randomUnit() noexcept {
    // xorshift64*: deterministic across realtime and offline renders.
    m_rng ^= m_rng >> 12;
    m_rng ^= m_rng << 25;
    m_rng ^= m_rng >> 27;
    const std::uint64_t value = m_rng * 2685821657736338717ull;
    return double(value >> 11) * (1.0 / 9007199254740992.0);
}

float GravityInstance::readCapture(int channel, double position) const noexcept {
    const auto& buffer = m_capture[std::clamp(channel, 0, 1)];
    if (buffer.empty()) return 0.0f;
    const double size = double(buffer.size());
    position = std::fmod(position, size);
    if (position < 0.0) position += size;
    const std::size_t a = std::size_t(position);
    const std::size_t b = a + 1 == buffer.size() ? 0 : a + 1;
    const float fraction = float(position - double(a));
    return buffer[a] + (buffer[b] - buffer[a]) * fraction;
}

void GravityInstance::spawnGrain(double delayFrames, double grainFrames,
                                 int overlap, Algorithm algorithm,
                                 double gravity, double pitch) noexcept {
    Grain* slot = nullptr;
    for (Grain& grain : m_grains) {
        if (!grain.active) { slot = &grain; break; }
    }
    if (!slot) {
        slot = &m_grains.front();
        for (Grain& grain : m_grains)
            if (grain.age > slot->age) slot = &grain;
    }

    const int generation = std::max(1, int(double(m_processedSamples) /
                                            std::max(1.0, delayFrames)));
    const double mass = std::clamp(
        m_smoothed[std::uint32_t(Param::Mass)] * 2.0 - 1.0, -1.0, 1.0);
    const double motion = std::clamp(
        m_smoothed[std::uint32_t(Param::Motion)], 0.0, 1.0);
    const double reverse = std::clamp(
        m_smoothed[std::uint32_t(Param::Reverse)], 0.0, 1.0);
    const double spread = std::clamp(
        m_smoothed[std::uint32_t(Param::PitchSpread)], 0.0, 12.0);
    const PitchSnap snap = PitchSnap(std::clamp(
        int(std::lround(m_values[std::uint32_t(Param::PitchSnap)].load(
            std::memory_order_relaxed))), 0, 3));
    double semitones = snapPitch(pitch, snap);
    semitones += mass >= 0.0 ? -1.5 * mass : -0.5 * mass;
    const std::uint64_t serial = m_grainSerial.load(std::memory_order_relaxed);
    const float attractor = spread > 1.0e-9 ? ((serial & 1u) ? 1.0f : -1.0f) : 0.0f;
    semitones += double(attractor) * spread;
    const double trajectoryDepth = 0.25 + 0.75 * gravity;
    switch (algorithm) {
        case Algorithm::Orbit:
            semitones += std::sin(m_orbitPhase + randomBipolar()) *
                         0.25 * trajectoryDepth;
            break;
        case Algorithm::Fall: {
            static constexpr double trajectory[] = {0.0, -2.0, -4.0, -7.0, -12.0};
            semitones += trajectory[std::clamp(generation - 1, 0, 4)] * trajectoryDepth;
            break;
        }
        case Algorithm::Rise: {
            static constexpr double trajectory[] = {0.0, 3.0, 5.0, 7.0, 12.0};
            semitones += trajectory[std::clamp(generation - 1, 0, 4)] * trajectoryDepth;
            break;
        }
        case Algorithm::Void:
            grainFrames *= 1.8;
            break;
        case Algorithm::Collapse:
            grainFrames *= std::pow(0.78, std::min(generation, 12));
            semitones += randomBipolar() *
                         std::min(3.0, generation * 0.5) * trajectoryDepth;
            break;
        case Algorithm::ZeroG:
            semitones += randomBipolar() * 0.5 * trajectoryDepth;
            break;
    }

    grainFrames *= std::exp2(1.25 * mass);
    if (motion > 0.0) {
        semitones += randomBipolar() * 0.4 * motion;
        grainFrames *= std::max(0.3, 1.0 + randomBipolar() * 0.7 * motion);
    }

    grainFrames = std::clamp(grainFrames, m_sampleRate * 0.012,
                             m_sampleRate * 0.5);
    double jitter = 0.0;
    if (algorithm == Algorithm::ZeroG) jitter = randomBipolar() * delayFrames * 0.35;
    if (motion > 0.0) jitter += randomBipolar() * delayFrames * 0.25 * motion;
    slot->active = true;
    slot->read = double(m_writeCursor) - delayFrames + jitter;
    slot->step = std::pow(2.0, std::clamp(semitones, -24.0, 24.0) / 12.0);
    const double algorithmReverse = algorithm == Algorithm::Void ? 0.35 : 0.0;
    const double reverseProbability =
        1.0 - (1.0 - reverse) * (1.0 - algorithmReverse);
    if (reverseProbability > 0.0 && randomUnit() < reverseProbability)
        slot->step = -slot->step;
    slot->age = 0;
    slot->length = std::max<std::uint32_t>(2, std::uint32_t(std::lround(grainFrames)));

    switch (algorithm) {
        case Algorithm::Orbit:
            slot->pan = float(std::sin(m_orbitPhase + generation * 0.65));
            break;
        case Algorithm::Fall:
        case Algorithm::Rise:
            slot->pan = float(randomBipolar() * std::min(1.0, 0.2 + generation * 0.15));
            break;
        case Algorithm::Void:
        case Algorithm::ZeroG:
            slot->pan = float(randomBipolar());
            break;
        case Algorithm::Collapse:
            slot->pan = float(randomBipolar() * std::pow(0.8, generation));
            break;
    }
    if (attractor != 0.0f)
        slot->pan = std::clamp(slot->pan + attractor * 0.35f, -1.0f, 1.0f);
    if (motion > 0.0)
        slot->pan = std::clamp(slot->pan + float(randomBipolar() * motion), -1.0f, 1.0f);
    slot->gain = float(2.0 / std::max(2, overlap));
    slot->color = 1.0f;
    slot->filter = {};
    if (motion > 0.0) {
        slot->gain *= float(std::pow(10.0, randomBipolar() * 2.0 * motion / 20.0));
        slot->color = float(std::clamp(1.0 + randomBipolar() * 0.6 * motion,
                                       0.35, 1.6));
    }
    m_grainSerial.fetch_add(1, std::memory_order_relaxed);
}

void GravityInstance::publishPeak(std::atomic<float>& destination,
                                  float value) noexcept {
    value = std::abs(value);
    float previous = destination.load(std::memory_order_relaxed);
    while (previous < value &&
           !destination.compare_exchange_weak(previous, value,
                                              std::memory_order_relaxed)) {}
}

void GravityInstance::renderSlice(const PluginProcessContext& context,
                                  std::uint32_t offset,
                                  std::uint32_t frames) noexcept {
    if (frames == 0) return;
    const bool frozenNow = frozen();
    const double tempo = context.transport.tempo > 0.0
                           ? context.transport.tempo : 120.0;
    const double smoothStep = std::min(1.0, 1.0 / (m_sampleRate * 0.020));
    std::array<double, kParameterCount> target{};
    for (std::uint32_t i = 0; i < kParameterCount; ++i)
        target[i] = m_values[i].load(std::memory_order_relaxed);

    const auto discrete = [&](Param p) {
        return int(std::lround(target[std::uint32_t(p)]));
    };
    const Algorithm algorithm = Algorithm(std::clamp(discrete(Param::Algorithm), 0, 5));
    const bool synced = discrete(Param::TimingSync) != 0;
    const double delaySeconds = synced
        ? (60.0 / tempo) * divisionQuarters(discrete(Param::TimingDivision))
        : target[std::uint32_t(Param::TimingMs)] / 1000.0;
    const double delayFrames = std::clamp(delaySeconds * m_sampleRate,
                                          m_sampleRate * 0.020,
                                          m_sampleRate * 2.0);

    const int detectorSource = std::clamp(discrete(Param::DetectorSource), 0, 2);
    const bool sidechainAvailable = context.sidechainInputs &&
                                    context.sidechainInputChannels > 0;
    const std::uint64_t sidechainMask = context.sidechainInputChannels >= 64
        ? ~std::uint64_t{0}
        : ((std::uint64_t{1} << context.sidechainInputChannels) - 1u);
    const bool sidechainAudible = sidechainAvailable &&
        (context.sidechainSilenceMask & sidechainMask) != sidechainMask;
    const bool detectorUsesSidechain =
        detectorSource == int(DetectorSource::Sidechain) ||
        (detectorSource == int(DetectorSource::Auto) && sidechainAudible);
    const float detectorFastStep = float(1.0 - std::exp(-1.0 / (m_sampleRate * 0.002)));
    const float detectorSlowStep = float(1.0 - std::exp(-1.0 / (m_sampleRate * 0.040)));
    const float duckAttackStep = float(1.0 - std::exp(-1.0 / (m_sampleRate * 0.005)));
    const float duckReleaseStep = float(1.0 - std::exp(-1.0 / (m_sampleRate * 0.350)));
    const float pulseDecay = float(std::exp(-1.0 / (m_sampleRate * 0.080)));

    float inputPeak[2]{};
    float outputPeak[2]{};
    float energyPeak = 0.0f;
    std::uint32_t activeGrainPeak = 0;
    float minimumDuckGain = 1.0f;
    const std::size_t captureSize = m_capture[0].size();

    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        for (Param parameter : {Param::Gravity, Param::Pitch, Param::Feedback,
                                Param::Decay, Param::Size, Param::TimingMs,
                                Param::Mass, Param::Motion, Param::Density,
                                Param::Diffusion, Param::Damping, Param::Reverse,
                                Param::StereoWidth, Param::StereoInput,
                                Param::Ducking, Param::Transient, Param::Drive,
                                Param::PitchSpread, Param::FeedbackLowcut}) {
            const std::uint32_t index = std::uint32_t(parameter);
            m_smoothed[index] += (target[index] - m_smoothed[index]) * smoothStep;
        }

        const double gravity = std::clamp(m_smoothed[std::uint32_t(Param::Gravity)], 0.0, 1.0);
        const double size = std::clamp(m_smoothed[std::uint32_t(Param::Size)], 0.0, 1.0);
        const double pitch = m_smoothed[std::uint32_t(Param::Pitch)];
        const double feedback = std::clamp(m_smoothed[std::uint32_t(Param::Feedback)], 0.0, 1.0);
        const double decay = std::max(0.1, m_smoothed[std::uint32_t(Param::Decay)]);
        const double mass = std::clamp(m_smoothed[std::uint32_t(Param::Mass)] * 2.0 - 1.0,
                                       -1.0, 1.0);
        const double motion = std::clamp(m_smoothed[std::uint32_t(Param::Motion)], 0.0, 1.0);
        const double density = std::clamp(m_smoothed[std::uint32_t(Param::Density)], 0.0, 1.0);
        const double diffusionControl = std::clamp(
            m_smoothed[std::uint32_t(Param::Diffusion)], 0.0, 1.0);
        const double damping = std::clamp(m_smoothed[std::uint32_t(Param::Damping)], 0.0, 1.0);
        const double width = std::clamp(m_smoothed[std::uint32_t(Param::StereoWidth)], 0.0, 1.0);
        const double stereoInput = std::clamp(m_smoothed[std::uint32_t(Param::StereoInput)], 0.0, 1.0);
        const double ducking = std::clamp(m_smoothed[std::uint32_t(Param::Ducking)], 0.0, 1.0);
        const double transient = std::clamp(m_smoothed[std::uint32_t(Param::Transient)], 0.0, 1.0);
        const double drive = std::clamp(m_smoothed[std::uint32_t(Param::Drive)], 0.0, 1.0);
        const double lowcut = std::clamp(m_smoothed[std::uint32_t(Param::FeedbackLowcut)], 0.0, 1.0);
        double grainFrames = m_sampleRate * 0.020 * std::pow(25.0, size);
        const int legacyOverlap = 2 + int(std::lround(6.0 * gravity));
        const int overlap = std::clamp(
            int(std::lround(double(legacyOverlap) *
                            std::exp2(3.0 * (density - 0.5)))), 1, 24);

        if (m_spawnCountdown <= 0.0) {
            spawnGrain(delayFrames, grainFrames, overlap, algorithm, gravity, pitch);
            double hop = grainFrames * std::exp2(1.25 * mass) / std::max(2, overlap);
            if (algorithm == Algorithm::ZeroG)
                hop *= 1.0 + randomBipolar() * 0.35;
            if (motion > 0.0)
                hop *= std::max(0.25, 1.0 + randomBipolar() * 0.25 * motion);
            hop *= 1.0 + 2.0 * transient;
            m_spawnCountdown += std::max(1.0, hop);
        }
        m_spawnCountdown -= 1.0;

        const std::uint32_t at = offset + frame;
        float input[2]{};
        for (int channel = 0; channel < 2; ++channel) {
            if (context.inputs && context.inputChannels > 0) {
                const int source = std::min<int>(channel, context.inputChannels - 1);
                input[channel] = context.inputs[source][at];
            }
            inputPeak[channel] = std::max(inputPeak[channel], std::abs(input[channel]));
            if (captureSize > 0) {
                const float fresh = frozenNow ? 0.0f : input[channel];
                m_capture[channel][m_writeCursor] =
                    fresh + std::tanh(m_feedbackSample[channel]);
            }
        }

        float detectorInput = std::max(std::abs(input[0]), std::abs(input[1]));
        if (detectorUsesSidechain) {
            detectorInput = 0.0f;
            if (sidechainAvailable) {
                for (std::uint16_t channel = 0;
                     channel < context.sidechainInputChannels; ++channel) {
                    detectorInput = std::max(detectorInput,
                        std::abs(context.sidechainInputs[channel][at]));
                }
            }
        }
        if (frozenNow) detectorInput = 0.0f;
        m_detectorFast += detectorFastStep * (detectorInput - m_detectorFast);
        m_detectorSlow += detectorSlowStep * (detectorInput - m_detectorSlow);
        const float duckStep = detectorInput > m_duckEnvelope
            ? duckAttackStep : duckReleaseStep;
        m_duckEnvelope += duckStep * (detectorInput - m_duckEnvelope);
        m_transientPulseState *= pulseDecay;
        if (m_transientCooldown > 0) --m_transientCooldown;
        const float flux = std::max(0.0f, m_detectorFast - m_detectorSlow);
        const float threshold = float(0.12 - 0.10 * transient);
        if (!frozenNow && transient > 0.0 && m_transientCooldown == 0 &&
            flux > threshold) {
            const int burst = 1 + int(std::lround(3.0 * transient));
            for (int grain = 0; grain < burst; ++grain)
                spawnGrain(delayFrames, grainFrames, overlap, algorithm, gravity, pitch);
            m_transientCooldown = std::max<std::uint32_t>(
                1, std::uint32_t(std::lround(m_sampleRate * 0.020)));
            m_transientPulseState = 1.0f;
        }

        float wet[2]{};
        float grainEnergy = 0.0f;
        std::uint32_t activeGrains = 0;
        for (Grain& grain : m_grains) {
            if (!grain.active) continue;
            ++activeGrains;
            const double phase = double(grain.age) /
                                 double(std::max<std::uint32_t>(1, grain.length - 1));
            const float window = float(0.5 - 0.5 * std::cos(2.0 * kPi * phase));
            const float sourceLeft = readCapture(0, grain.read);
            const float sourceRight = readCapture(1, grain.read);
            const float mono = 0.5f * (sourceLeft + sourceRight);
            float grainLeft = mono;
            float grainRight = mono;
            if (stereoInput > 0.0) {
                grainLeft += float(stereoInput) * (sourceLeft - mono);
                grainRight += float(stereoInput) * (sourceRight - mono);
            }
            if (motion > 0.0) {
                const float coefficient = std::clamp(0.04f + 0.28f * grain.color,
                                                     0.04f, 0.50f);
                grain.filter[0] += coefficient * (grainLeft - grain.filter[0]);
                grain.filter[1] += coefficient * (grainRight - grain.filter[1]);
                grainLeft = grain.filter[0];
                grainRight = grain.filter[1];
            }
            const double angle = (double(grain.pan) + 1.0) * kPi * 0.25;
            const float envelope = window * grain.gain;
            const float left = grainLeft * envelope * float(std::cos(angle));
            const float right = grainRight * envelope * float(std::sin(angle));
            wet[0] += left;
            wet[1] += right;
            grainEnergy += 0.5f * (std::abs(left) + std::abs(right));
            grain.read += grain.step;
            if (++grain.age >= grain.length) grain.active = false;
        }
        activeGrainPeak = std::max(activeGrainPeak, activeGrains);

        double diffusion = 0.15;
        switch (algorithm) {
            case Algorithm::Orbit: diffusion = 0.15; break;
            case Algorithm::Fall: diffusion = 0.28; break;
            case Algorithm::Rise: diffusion = 0.24; break;
            case Algorithm::Void: diffusion = 0.88; break;
            case Algorithm::Collapse: diffusion = 0.18; break;
            case Algorithm::ZeroG: diffusion = 0.78; break;
        }
        diffusion = std::clamp(diffusion * 2.0 * diffusionControl +
                               0.15 * std::max(0.0, mass), 0.0, 1.0);
        diffusion *= gravity * (0.35 + 0.65 * size);
        for (int channel = 0; channel < 2; ++channel) {
            float spread = wet[channel];
            const float allpassFeedback = float(0.30 + 0.38 * diffusion);
            for (Allpass& allpass : m_diffusers[channel])
                spread = allpass.process(spread, allpassFeedback);
            wet[channel] = wet[channel] * float(1.0 - diffusion) +
                           spread * float(diffusion);
        }

        double dampingCoefficient = 0.02 + (1.0 - size) * 0.18;
        if (algorithm == Algorithm::Fall || algorithm == Algorithm::Void)
            dampingCoefficient *= 0.45;
        if (algorithm == Algorithm::Rise) dampingCoefficient *= 1.35;
        dampingCoefficient = std::clamp(
            dampingCoefficient * std::exp2(4.0 * (0.5 - damping)),
            0.001, 0.4);

        const double longAmount = std::clamp(
            std::max(0.0, (diffusionControl - 0.5) * 2.0) +
            0.25 * std::max(0.0, mass), 0.0, 1.0);
        float tankDelayed[2][4]{};
        float tankOutput[2]{};
        for (int channel = 0; channel < 2; ++channel) {
            for (int line = 0; line < 4; ++line)
                tankDelayed[channel][line] = m_tank[channel][line].read();
            tankOutput[channel] = 0.25f *
                (tankDelayed[channel][0] + tankDelayed[channel][1] +
                 tankDelayed[channel][2] + tankDelayed[channel][3]);
        }
        for (int channel = 0; channel < 2; ++channel) {
            const float* d = tankDelayed[channel];
            const float mixed[4]{
                0.5f * (d[0] + d[1] + d[2] + d[3]),
                0.5f * (d[0] - d[1] + d[2] - d[3]),
                0.5f * (d[0] + d[1] - d[2] - d[3]),
                0.5f * (d[0] - d[1] - d[2] + d[3])};
            for (int line = 0; line < 4; ++line) {
                m_tankDamping[channel][line] += float(dampingCoefficient) *
                    (mixed[line] - m_tankDamping[channel][line]);
                const double lineSeconds = double(m_tank[channel][line].line.size()) /
                                           m_sampleRate;
                const float tankFeedback = float(std::min(
                    0.96, std::pow(10.0, -3.0 * lineSeconds / decay)) * longAmount);
                const float injection = float(longAmount) *
                    (wet[channel] * 0.28f + wet[1 - channel] * 0.12f);
                m_tank[channel][line].write(
                    injection + m_tankDamping[channel][line] * tankFeedback);
            }
        }
        if (longAmount > 0.0) {
            const float tankMix = float(0.75 * longAmount);
            wet[0] = wet[0] * (1.0f - tankMix) + tankOutput[0] * tankMix;
            wet[1] = wet[1] * (1.0f - tankMix) + tankOutput[1] * tankMix;
        }

        if (drive > 0.0) {
            const float gain = float(1.0 + 4.0 * drive);
            const float normalise = 1.0f / std::tanh(gain);
            wet[0] = std::tanh(wet[0] * gain) * normalise;
            wet[1] = std::tanh(wet[1] * gain) * normalise;
        }

        const double feedbackGain = std::min(
            0.96, feedback * (0.5 + 0.5 * gravity) *
                      std::exp(-delaySeconds / decay));
        for (int channel = 0; channel < 2; ++channel) {
            m_dampingState[channel] += float(dampingCoefficient) *
                (wet[channel] - m_dampingState[channel]);
            float feedbackInput = m_dampingState[channel];
            if (lowcut > 0.0) {
                const double frequency = lowcutFrequency(lowcut);
                const float coefficient = float(std::exp(-2.0 * kPi * frequency /
                                                         m_sampleRate));
                const float highpassed = coefficient *
                    (m_lowcutOutput[channel] + feedbackInput - m_lowcutInput[channel]);
                m_lowcutInput[channel] = feedbackInput;
                m_lowcutOutput[channel] = highpassed;
                feedbackInput = highpassed;
            } else {
                m_lowcutInput[channel] = 0.0f;
                m_lowcutOutput[channel] = 0.0f;
            }
            float feedbackDrive = feedbackInput * float(feedbackGain);
            if (algorithm == Algorithm::Collapse) feedbackDrive *= 1.4f;
            m_feedbackSample[channel] = std::tanh(feedbackDrive);
            if (std::abs(m_feedbackSample[channel]) < 1.0e-20f)
                m_feedbackSample[channel] = 0.0f;
        }

        const float duckGain = ducking > 0.0
            ? float(std::pow(10.0, -18.0 * ducking * m_duckEnvelope / 20.0))
            : 1.0f;
        minimumDuckGain = std::min(minimumDuckGain, duckGain);
        wet[0] *= duckGain;
        wet[1] *= duckGain;
        if (std::abs(width - 0.5) > 1.0e-12) {
            const float mid = 0.5f * (wet[0] + wet[1]);
            const float side = 0.5f * (wet[0] - wet[1]) * float(2.0 * width);
            wet[0] = mid + side;
            wet[1] = mid - side;
        }

        const float dryGain = float(std::cos(gravity * kPi * 0.5));
        const float wetGain = float(std::sin(gravity * kPi * 0.5));
        const std::uint16_t channels = std::min<std::uint16_t>(context.outputChannels, 2);
        if (channels == 1) {
            const float wetMono = (wet[0] + wet[1]) * 0.70710678f;
            const float output = input[0] * dryGain + wetMono * wetGain;
            context.outputs[0][at] = output;
            outputPeak[0] = std::max(outputPeak[0], std::abs(output));
        } else {
            for (int channel = 0; channel < 2; ++channel) {
                const float output = input[channel] * dryGain + wet[channel] * wetGain;
                context.outputs[channel][at] = output;
                outputPeak[channel] = std::max(outputPeak[channel], std::abs(output));
            }
        }
        for (std::uint16_t channel = channels; channel < context.outputChannels; ++channel)
            context.outputs[channel][at] = channels > 0 ? context.outputs[0][at] : 0.0f;

        energyPeak = std::max(energyPeak, std::min(1.0f, grainEnergy));
        const double movementRate = std::exp2(-mass) * (1.0 + motion);
        m_orbitPhase += (0.05 + 0.45 * gravity) * movementRate *
                        2.0 * kPi / m_sampleRate;
        if (m_orbitPhase >= 2.0 * kPi) m_orbitPhase -= 2.0 * kPi;
        if (captureSize > 0 && ++m_writeCursor == captureSize) m_writeCursor = 0;
        ++m_processedSamples;
    }

    publishPeak(m_inputPeakLeft, inputPeak[0]);
    publishPeak(m_inputPeakRight, inputPeak[1]);
    publishPeak(m_outputPeakLeft, outputPeak[0]);
    publishPeak(m_outputPeakRight, outputPeak[1]);
    const float previousEnergy = m_fieldEnergy.load(std::memory_order_relaxed);
    m_fieldEnergy.store(previousEnergy * 0.88f + energyPeak * 0.12f,
                        std::memory_order_relaxed);
    m_telemetryOrbit.store(float(m_orbitPhase / (2.0 * kPi)),
                           std::memory_order_relaxed);
    m_telemetryDuckGain.store(minimumDuckGain, std::memory_order_relaxed);
    m_telemetryTransient.store(m_transientPulseState, std::memory_order_relaxed);
    m_activeGrains.store(activeGrainPeak, std::memory_order_relaxed);
}

void GravityInstance::applyEvent(const PluginEvent& event) noexcept {
    if (event.kind != PluginEvent::Kind::ParamValue ||
        event.paramIndex >= kParameterCount) return;
    m_values[event.paramIndex].store(
        clampParameter(event.paramIndex, event.value),
        std::memory_order_relaxed);
}

PluginProcessDisposition GravityInstance::process(
    const PluginProcessContext& context) noexcept {
    if (!context.outputs || context.outputChannels == 0)
        return frozen() ? PluginProcessDisposition::Continue
                        : PluginProcessDisposition::Tail;
    if (m_clearRequested.exchange(false, std::memory_order_acq_rel)) resetDsp();
    if (m_capture[0].empty()) {
        for (std::uint16_t channel = 0; channel < context.outputChannels; ++channel) {
            const float* input = context.inputs && context.inputChannels > 0
                ? context.inputs[std::min<std::uint16_t>(channel, context.inputChannels - 1)]
                : nullptr;
            for (std::uint32_t i = 0; i < context.frames; ++i)
                context.outputs[channel][i] = input ? input[i] : 0.0f;
        }
        return PluginProcessDisposition::Tail;
    }

    std::uint32_t cursor = 0;
    for (const PluginEvent& event : context.inputEvents) {
        const std::uint32_t at = std::min(event.frameOffset, context.frames);
        if (at > cursor) {
            renderSlice(context, cursor, at - cursor);
            cursor = at;
        }
        applyEvent(event);
    }
    if (cursor < context.frames)
        renderSlice(context, cursor, context.frames - cursor);
    return frozen() ? PluginProcessDisposition::Continue
                    : PluginProcessDisposition::Tail;
}

void GravityInstance::resetDsp() noexcept {
    for (auto& channel : m_capture) std::fill(channel.begin(), channel.end(), 0.0f);
    for (auto& channel : m_diffusers)
        for (Allpass& allpass : channel) allpass.clear();
    for (auto& channel : m_tank)
        for (TankLine& line : channel) line.clear();
    for (Grain& grain : m_grains) grain = Grain{};
    m_writeCursor = 0;
    m_tankDamping = {};
    m_feedbackSample = {};
    m_dampingState = {};
    m_lowcutInput = {};
    m_lowcutOutput = {};
    m_spawnCountdown = 0.0;
    m_orbitPhase = 0.0;
    m_processedSamples = 0;
    m_rng = 0x9e3779b97f4a7c15ull;
    m_detectorFast = 0.0f;
    m_detectorSlow = 0.0f;
    m_duckEnvelope = 0.0f;
    m_transientPulseState = 0.0f;
    m_transientCooldown = 0;
    m_grainSerial.store(0, std::memory_order_relaxed);
    m_fieldEnergy.store(0.0f, std::memory_order_relaxed);
    m_telemetryOrbit.store(0.0f, std::memory_order_relaxed);
    m_telemetryDuckGain.store(1.0f, std::memory_order_relaxed);
    m_telemetryTransient.store(0.0f, std::memory_order_relaxed);
    m_activeGrains.store(0, std::memory_order_relaxed);
}

void GravityInstance::reset() noexcept {
    m_frozen.store(false, std::memory_order_release);
    m_clearRequested.store(false, std::memory_order_release);
    resetDsp();
}

std::uint32_t GravityInstance::tailSamples() const noexcept {
    const double decay = parameterValue(std::uint32_t(Param::Decay));
    const double frames = (std::clamp(decay, 0.1, 20.0) + 2.5) * m_sampleRate;
    return std::uint32_t(std::min<double>(
        frames, double(std::numeric_limits<std::uint32_t>::max())));
}

Telemetry GravityInstance::consumeTelemetry() noexcept {
    Telemetry telemetry;
    telemetry.inputLeft = m_inputPeakLeft.exchange(0.0f, std::memory_order_relaxed);
    telemetry.inputRight = m_inputPeakRight.exchange(0.0f, std::memory_order_relaxed);
    telemetry.outputLeft = m_outputPeakLeft.exchange(0.0f, std::memory_order_relaxed);
    telemetry.outputRight = m_outputPeakRight.exchange(0.0f, std::memory_order_relaxed);
    telemetry.fieldEnergy = m_fieldEnergy.load(std::memory_order_relaxed);
    telemetry.orbitPhase = m_telemetryOrbit.load(std::memory_order_relaxed);
    telemetry.duckGain = m_telemetryDuckGain.load(std::memory_order_relaxed);
    telemetry.transientPulse = m_telemetryTransient.load(std::memory_order_relaxed);
    telemetry.grainSerial = m_grainSerial.load(std::memory_order_relaxed);
    telemetry.activeGrains = m_activeGrains.load(std::memory_order_relaxed);
    telemetry.frozen = frozen();
    return telemetry;
}

} // namespace daw::plugins::gravity
