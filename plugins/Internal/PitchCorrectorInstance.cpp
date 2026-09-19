#include "Internal/PitchCorrectorInstance.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace daw::plugins::pitch {
namespace {
static_assert(std::atomic<double>::is_always_lock_free && std::atomic<std::uint64_t>::is_always_lock_free,
              "Pitch parameters and meters must remain lock-free on the audio thread");
constexpr std::string_view kUid = "daw.pitch-corrector";
constexpr int kStateVersion = 1;
constexpr const char* kNotes[]{"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
constexpr const char* kScales[]{"Chromatic", "Major", "Natural Minor", "Harmonic Minor", "Melodic Minor", "Major Pentatonic", "Minor Pentatonic", "Custom"};
constexpr const char* kVoices[]{"Auto", "Low", "Mid", "High"};

const std::array<ParameterInfo, kParameterCount>& parametersImpl() {
    static const std::array<ParameterInfo, kParameterCount> table{{
        {0, "tune", "Retune Speed", "", 0, 100, kDefaultTune, true, false, false},
        {1, "humanize", "Humanize", "%", 0, 100, 0, true, false, false},
        {2, "vibrato", "Vibrato", "%", 0, 100, 0, true, false, false},
        {3, "a4_hz", "A4", "Hz", 400, 480, 440, true, false, false},
        {4, "key", "Key", "", 0, 11, 0, true, true, false},
        {5, "scale", "Scale", "", 0, 7, 0, true, true, false},
        {6, "voice", "Voice", "", 0, 3, 0, true, true, false},
        {7, "formants", "Preserve Formants", "", 0, 1, 1, true, true, false},
        {8, "amount", "Correction Amount", "%", 0, 100, 100, true, false, false},
        {9, "output_db", "Output", "dB", -24, 12, 0, true, false, false},
        {10, "note_mask", "Allowed Notes", "", 1, 4095, 4095, false, true, false},
        {11, "quality", "Quality", "", 0, 1, 0, false, true, false},
    }};
    return table;
}
} // namespace

std::span<const ParameterInfo> parameterTable() noexcept { return parametersImpl(); }
std::span<const FactoryPreset> factoryPresets() noexcept {
    static constexpr std::array<FactoryPreset, 4> presets{{
        {"Natural", 40, 60, 70}, {"Pop", 70, 30, 30},
        {"Tight", 90, 10, 10}, {"Hard", 100, 0, 0},
    }};
    return presets;
}

std::string parameterText(std::uint32_t index, double value) {
    if (index >= kParameterCount) return {};
    const auto& info = parametersImpl()[index];
    value = std::isfinite(value) ? std::clamp(value, info.minValue, info.maxValue) : info.defaultValue;
    char text[64]{};
    switch (Param(index)) {
    case Param::Tune: std::snprintf(text, sizeof(text), "%.1f ms", retuneMilliseconds(value)); break;
    case Param::Key: return kNotes[int(std::lround(value))];
    case Param::Scale: return kScales[int(std::lround(value))];
    case Param::Voice: return kVoices[int(std::lround(value))];
    case Param::Quality: return value >= 0.5 ? "HD" : "Real-Time";
    case Param::Formants: return value >= 0.5 ? "On" : "Off";
    case Param::A4Hz: std::snprintf(text, sizeof(text), "%.1f Hz", value); break;
    case Param::OutputDb: std::snprintf(text, sizeof(text), "%+.1f dB", value); break;
    case Param::NoteMask: {
        std::string result;
        const auto mask = std::uint16_t(std::lround(value));
        for (int note = 0; note < 12; ++note) if (mask & (1 << note)) {
            if (!result.empty()) result += ' ';
            result += kNotes[note];
        }
        return result;
    }
    default: std::snprintf(text, sizeof(text), "%.0f%%", value); break;
    }
    return text;
}

PitchCorrectorInstance::PitchCorrectorInstance() : m_descriptor(staticDescriptor()) {
    for (const auto& info : parameterTable()) m_values[info.index].store(info.defaultValue, std::memory_order_relaxed);
}
PitchCorrectorInstance::~PitchCorrectorInstance() = default;

const PluginDescriptor& PitchCorrectorInstance::staticDescriptor() noexcept {
    static const PluginDescriptor descriptor = [] {
        PluginDescriptor d;
        d.format = Format::Internal; d.uid = std::string(kUid); d.path = d.uid;
        d.name = "VLT Pitch"; d.vendor = "VLTONE"; d.version = "1.0";
        d.category = "Effect|Pitch|Vocal"; d.stateSchemaVersion = kStateVersion;
        d.mainInputChannels = d.mainOutputChannels = 2;
        return d;
    }();
    return descriptor;
}
std::string_view PitchCorrectorInstance::uid() noexcept { return kUid; }

bool PitchCorrectorInstance::setBusLayout(const PluginBusLayout& wanted, PluginBusLayout& accepted) {
    if (wanted.inputs.size() > 1 || wanted.outputs.size() > 1) return false;
    const auto channels = wanted.inputs.empty() ? std::uint16_t(2) : wanted.inputs.front();
    if ((channels != 1 && channels != 2) || (!wanted.outputs.empty() && wanted.outputs.front() != channels)) return false;
    m_layout.inputs = {channels}; m_layout.outputs = {channels}; accepted = m_layout;
    return true;
}

bool PitchCorrectorInstance::activate(const PluginProcessInfo& info) {
    if (!std::isfinite(info.sampleRate) || info.sampleRate < 12000 || info.sampleRate > 192000 || !info.maxBlockSize) return false;
    m_active = m_processing = false;
    m_maxBlock = info.maxBlockSize;
    m_channels = m_layout.inputs.front();
    const int quality = info.offline || !m_preparedOnce || m_adoptQualityOnPrepare
        ? int(parameterValue(std::uint32_t(Param::Quality))) : activeQuality();
    m_dsp.prepare(info.sampleRate, m_maxBlock, m_channels, quality);
    m_silence.assign(m_maxBlock, 0.f);
    for (auto& buffer : m_discard) buffer.assign(m_maxBlock, 0.f);
    m_activeQuality.store(quality, std::memory_order_relaxed);
    m_latency.store(m_dsp.latencySamples(), std::memory_order_relaxed);
    m_tail.store(m_dsp.tailSamples(), std::memory_order_relaxed);
    m_preparedOnce = true; m_adoptQualityOnPrepare = false; m_active = true;
    reset();
    return true;
}
void PitchCorrectorInstance::deactivate() { m_active = m_processing = false; }

std::span<const ParameterInfo> PitchCorrectorInstance::parameters() const noexcept { return parameterTable(); }
std::int32_t PitchCorrectorInstance::parameterIndexForId(std::string_view id) const noexcept {
    for (const auto& info : parameterTable()) if (info.id == id) return std::int32_t(info.index);
    return -1;
}
double PitchCorrectorInstance::parameterValue(std::uint32_t index) const noexcept {
    return index < kParameterCount ? m_values[index].load(std::memory_order_relaxed) : 0;
}
std::string PitchCorrectorInstance::parameterText(std::uint32_t index, double value) const { return pitch::parameterText(index, value); }

double PitchCorrectorInstance::clampParameter(std::uint32_t index, double value) noexcept {
    if (index >= kParameterCount) return 0;
    const auto& info = parametersImpl()[index];
    if (!std::isfinite(value)) return info.defaultValue;
    value = std::clamp(value, info.minValue, info.maxValue);
    if (info.isStepped) value = std::round(value);
    if (Param(index) == Param::A4Hz) value = std::round(value * 10) / 10;
    return value;
}
void PitchCorrectorInstance::setParameterFromHost(std::uint32_t index, double value) {
    if (index >= kParameterCount) return;
    value = clampParameter(index, value);
    const double previous = m_values[index].exchange(value, std::memory_order_relaxed);
    if (Param(index) == Param::Quality && previous != value) PluginMainThreadWork::request();
}

bool PitchCorrectorInstance::qualityChangePending() const noexcept {
    return m_preparedOnce && int(parameterValue(std::uint32_t(Param::Quality))) != activeQuality();
}
bool PitchCorrectorInstance::applyPendingQuality() noexcept {
    m_adoptQualityOnPrepare = qualityChangePending();
    return m_adoptQualityOnPrepare;
}

bool PitchCorrectorInstance::saveState(std::vector<std::uint8_t>& out) const {
    nlohmann::json params = nlohmann::json::object();
    for (const auto& info : parameterTable()) params[info.id] = parameterValue(info.index);
    const auto text = nlohmann::json{{"version", kStateVersion}, {"params", std::move(params)}}.dump();
    out.assign(text.begin(), text.end());
    return true;
}
bool PitchCorrectorInstance::loadState(std::span<const std::uint8_t> state) {
    if (state.empty() || state.size() > 65536) return false;
    const auto document = nlohmann::json::parse(state.begin(), state.end(), nullptr, false);
    if (!document.is_object()) return false;
    const auto version = document.find("version"), params = document.find("params");
    if (version == document.end() || !version->is_number_integer() || *version != kStateVersion ||
        params == document.end() || !params->is_object()) return false;
    std::array<double, kParameterCount> values{};
    for (const auto& info : parameterTable()) {
        values[info.index] = info.defaultValue;
        if (const auto value = params->find(info.id); value != params->end()) {
            if (!value->is_number()) return false;
            const double plain = value->get<double>();
            if (!std::isfinite(plain)) return false;
            values[info.index] = clampParameter(info.index, plain);
        }
    }
    // Validate the entire state first; a malformed preset must not partially land.
    for (std::uint32_t index = 0; index < kParameterCount; ++index) setParameterFromHost(index, values[index]);
    return true;
}

Settings PitchCorrectorInstance::settings() const noexcept {
    Settings result;
    result.tune = parameterValue(0); result.humanize = parameterValue(1); result.vibrato = parameterValue(2);
    result.a4Hz = parameterValue(3); result.key = int(parameterValue(4)); result.scale = int(parameterValue(5));
    result.voice = int(parameterValue(6)); result.formants = parameterValue(7) >= 0.5;
    result.amount = parameterValue(8); result.outputDb = parameterValue(9); result.noteMask = std::uint16_t(parameterValue(10));
    return result;
}

void PitchCorrectorInstance::renderSlice(const PluginProcessContext& context, std::uint32_t offset, std::uint32_t frames) noexcept {
    const auto current = settings();
    while (frames) {
        const auto count = std::min(frames, m_maxBlock);
        std::array<const float*, 2> input{};
        std::array<float*, 2> output{};
        for (std::uint32_t channel = 0; channel < m_channels; ++channel) {
            input[channel] = context.inputs && channel < context.inputChannels && context.inputs[channel]
                ? context.inputs[channel] + offset : m_silence.data();
            output[channel] = context.outputs && channel < context.outputChannels && context.outputs[channel]
                ? context.outputs[channel] + offset : m_discard[channel].data();
        }
        m_dsp.process(input.data(), output.data(), count, current);
        offset += count; frames -= count;
    }
}

PluginProcessDisposition PitchCorrectorInstance::process(const PluginProcessContext& context) noexcept {
    if (!m_active) {
        if (context.outputs) for (std::uint16_t channel = 0; channel < context.outputChannels; ++channel)
            if (context.outputs[channel]) std::fill_n(context.outputs[channel], context.frames, 0.f);
        return PluginProcessDisposition::Error;
    }
    std::uint32_t position = 0;
    for (const auto& event : context.inputEvents) {
        if (event.kind != PluginEvent::Kind::ParamValue || event.paramIndex >= kParameterCount) continue;
        const auto at = std::clamp(event.frameOffset, position, context.frames);
        renderSlice(context, position, at - position);
        setParameterFromHost(event.paramIndex, event.value);
        position = at;
    }
    renderSlice(context, position, context.frames - position);
    if (context.outputs) for (std::uint16_t channel = std::uint16_t(m_channels); channel < context.outputChannels; ++channel)
        if (context.outputs[channel]) std::fill_n(context.outputs[channel], context.frames, 0.f);
    publishTelemetry();
    return PluginProcessDisposition::Continue;
}

void PitchCorrectorInstance::reset() noexcept {
    m_dsp.reset();
    for (auto& value : m_telemetry) value.store(0, std::memory_order_relaxed);
    m_voiced.store(false, std::memory_order_relaxed);
    m_telemetrySerial.store(0, std::memory_order_relaxed);
}
void PitchCorrectorInstance::publishTelemetry() noexcept {
    const auto current = m_dsp.telemetry();
    const double values[]{current.inputHz, current.targetHz, current.confidence, current.correctionCents, current.inputLevel, current.outputLevel};
    for (std::size_t i = 0; i < m_telemetry.size(); ++i) m_telemetry[i].store(values[i], std::memory_order_relaxed);
    m_voiced.store(current.voiced, std::memory_order_relaxed);
    m_telemetrySerial.store(current.serial, std::memory_order_relaxed);
}
Telemetry PitchCorrectorInstance::telemetrySnapshot() const noexcept {
    Telemetry result;
    result.inputHz = m_telemetry[0].load(std::memory_order_relaxed);
    result.targetHz = m_telemetry[1].load(std::memory_order_relaxed);
    result.confidence = m_telemetry[2].load(std::memory_order_relaxed);
    result.correctionCents = m_telemetry[3].load(std::memory_order_relaxed);
    result.inputLevel = m_telemetry[4].load(std::memory_order_relaxed);
    result.outputLevel = m_telemetry[5].load(std::memory_order_relaxed);
    result.voiced = m_voiced.load(std::memory_order_relaxed);
    result.serial = m_telemetrySerial.load(std::memory_order_relaxed);
    return result;
}
} // namespace daw::plugins::pitch
