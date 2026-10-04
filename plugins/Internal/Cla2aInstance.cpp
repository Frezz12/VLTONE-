#include "Internal/Cla2aInstance.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>

namespace daw::plugins::cla2a {
namespace {
static_assert(std::atomic<double>::is_always_lock_free && std::atomic<float>::is_always_lock_free);
constexpr double kReferenceRms = 0.12589254117941673 / std::numbers::sqrt2; // -18 dBFS sine, 0 VU.
constexpr double kLampKnee = .25;

double gainAmplitude(double value) noexcept {
    // Audio-taper pot: mute at 0, unity at 40, +40 dB small-signal gain at 100.
    return value == 0 ? 0 : 100 * std::pow(value / 100, 5.025883189464120);
}
double peakDrive(double value) noexcept {
    // L/(L+1) is not used as a hard threshold: a lamp's broad turn-on region
    // supplies the knee. Calibrate 50 to 6 dB GR on the reference sine.
    const double conductance = std::pow(10., 6. / 20) - 1;
    const double light = std::pow(conductance, 1.5);
    const double drive = .5 * (light + std::sqrt(light * light + 4 * kLampKnee * light));
    const double position = value / 50;
    return drive / kReferenceRms * std::pow(position, 6);
}
double clean(double value) noexcept { return std::abs(value) < 1e-24 ? 0 : value; }
double inputSample(const PluginProcessContext& c, unsigned ch, unsigned frame) noexcept {
    if (!c.inputs || ch >= c.inputChannels || !c.inputs[ch]) return 0;
    const double x = c.inputs[ch][frame];
    return std::isfinite(x) ? std::clamp(x, -64., 64.) : 0;
}
double triode(double x, double drive, double bias, double zero, double normalization) noexcept {
    if (x == 0) return 0;
    return (std::tanh(drive * x + bias) - zero) * normalization;
}
}

std::span<const ParameterInfo> parameterTable() noexcept {
    static const std::array<ParameterInfo, kParameterCount> table{{
        {0, "gain", "Gain", "", 0, 100, 40},
        {1, "peakReduction", "Peak Reduction", "", 0, 100, 0},
        {2, "mode", "Mode", "", 0, 1, 0, true, true},
    }};
    return table;
}
Cla2aInstance::Cla2aInstance() : m_filter(engine::dsp::halfBand65Taps()) {
    for (const auto& p : parameterTable()) m_values[p.index].store(p.defaultValue);
}
const PluginDescriptor& Cla2aInstance::staticDescriptor() noexcept {
    static const PluginDescriptor descriptor = [] {
        PluginDescriptor d;
        d.format = Format::Internal; d.uid = d.path = "daw.cla2a";
        d.name = "VLT 2A"; d.vendor = "VLTONE"; d.version = "1.0";
        d.category = "Effect|Dynamics|Compressor"; d.stateSchemaVersion = 1;
        return d;
    }();
    return descriptor;
}
bool Cla2aInstance::setBusLayout(const PluginBusLayout& wanted, PluginBusLayout& accepted) {
    if (wanted.inputs.size() > 1 || wanted.outputs.size() > 1) return false;
    const auto main = wanted.inputs.empty() ? std::uint16_t(2) : wanted.inputs[0];
    if ((main != 1 && main != 2) || (!wanted.outputs.empty() && wanted.outputs[0] != main)) return false;
    m_layout = {{main}, {main}}; accepted = m_layout; return true;
}
bool Cla2aInstance::activate(const PluginProcessInfo& info) {
    if (!std::isfinite(info.sampleRate) || info.sampleRate < 8000 || info.sampleRate > 384000 || !info.maxBlockSize) return false;
    m_rate = info.sampleRate;
    const double rate = 4 * m_rate;
    const auto time = [rate](double seconds) { return std::exp(-1 / (seconds * rate)); };
    const auto cutoff = [rate](double hz) { return -std::expm1(-2 * std::numbers::pi * std::min(hz, rate * .45) / rate); };
    m_smoothing = m_attack = time(.010);
    m_detector = time(.001);
    m_fastRelease = time(.045);
    m_charge = time(.300); m_discharge = time(2.0);
    m_coupling = time(1 / (2 * std::numbers::pi * 4));
    m_inputFlux = cutoff(120); m_outputFlux = cutoff(160); m_highCut = cutoff(40000);
    for (unsigned i = 0; i < m_slowRelease.size(); ++i)
        m_slowRelease[i] = time(.100 + 1.800 * i / (m_slowRelease.size() - 1));
    m_active = true; m_processing = false; reset(); return true;
}
std::int32_t Cla2aInstance::parameterIndexForId(std::string_view id) const noexcept {
    for (const auto& p : parameterTable()) if (p.id == id) return std::int32_t(p.index);
    return -1;
}
double Cla2aInstance::parameterValue(std::uint32_t index) const noexcept {
    return index < kParameterCount ? m_values[index].load(std::memory_order_relaxed) : 0;
}
double Cla2aInstance::clamp(unsigned index, double value) noexcept {
    const auto& p = parameterTable()[index];
    value = std::isfinite(value) ? std::clamp(value, p.minValue, p.maxValue) : p.defaultValue;
    return p.isStepped ? std::round(value) : value;
}
void Cla2aInstance::setParameterFromHost(std::uint32_t index, double value) {
    if (index < kParameterCount) m_values[index].store(clamp(index, value), std::memory_order_relaxed);
}
std::string Cla2aInstance::parameterText(std::uint32_t index, double value) const {
    if (index >= kParameterCount) return {};
    value = clamp(index, value);
    if (index == unsigned(Param::Mode)) return value > .5 ? "Limit" : "Compress";
    char text[48]; std::snprintf(text, sizeof(text), "%.1f", value); return text;
}
bool Cla2aInstance::saveState(std::vector<std::uint8_t>& out) const {
    nlohmann::json params = nlohmann::json::object();
    for (const auto& p : parameterTable()) params[p.id] = parameterValue(p.index);
    const auto text = nlohmann::json{{"version", 1}, {"params", params}}.dump();
    out.assign(text.begin(), text.end()); return true;
}
bool Cla2aInstance::loadState(std::span<const std::uint8_t> state) {
    if (state.empty() || state.size() > 65536) return false;
    const auto doc = nlohmann::json::parse(state.begin(), state.end(), nullptr, false);
    if (!doc.is_object() || !doc.contains("version") || doc["version"] != 1 ||
        !doc.contains("params") || !doc["params"].is_object()) return false;
    std::array<double, kParameterCount> values{};
    for (const auto& p : parameterTable()) {
        values[p.index] = p.defaultValue;
        if (const auto it = doc["params"].find(p.id); it != doc["params"].end()) {
            if (!it->is_number() || !std::isfinite(it->get<double>())) return false;
            values[p.index] = clamp(p.index, it->get<double>());
        }
    }
    for (unsigned i = 0; i < kParameterCount; ++i) setParameterFromHost(i, values[i]);
    return true;
}
double Cla2aInstance::Coupling::tick(double input, double pole) noexcept {
    output = clean(input - previous + pole * output); previous = input; return output;
}
void Cla2aInstance::reset() noexcept {
    m_channels = {}; m_power = m_fastCell = m_slowCell = m_exposure = m_reduction = 0;
    m_gain = m_gainTarget = gainAmplitude(parameterValue(0));
    m_peakDrive = m_peakTarget = peakDrive(parameterValue(1));
    m_mode = m_modeTarget = parameterValue(2);
    m_blockPeaks = {};
    for (auto& peak : m_peaks) peak.store(0, std::memory_order_relaxed);
}
std::array<double, 2> Cla2aInstance::tick4x(std::array<double, 2> x, unsigned channels) noexcept {
    m_gain = m_gainTarget + m_smoothing * (m_gain - m_gainTarget);
    m_peakDrive = m_peakTarget + m_smoothing * (m_peakDrive - m_peakTarget);
    m_mode = m_modeTarget + m_smoothing * (m_mode - m_modeTarget);
    double detector = 0;
    for (unsigned ch = 0; ch < channels; ++ch) {
        auto& s = m_channels[ch];
        s.inputFlux = clean(s.inputFlux + m_inputFlux * (x[ch] - s.inputFlux));
        x[ch] += std::tanh(.5 * s.inputFlux) * 2 - s.inputFlux;
        detector = std::max(detector, x[ch] * x[ch]);
    }
    m_power = clean(detector + m_detector * (m_power - detector));
    const double drive = std::sqrt(std::max(0., m_power)) * m_peakDrive;
    const double light = drive * drive / (drive + kLampKnee);
    // Conductance law gives a continuously changing ratio, tending to 3:1
    // in Compress. Limit adds the steeper photocell/driver contribution.
    const double target = std::min(99., std::pow(light, 2. / 3) + m_mode * light);
    const double exposure = target / (1 + target);
    const double memoryPole = exposure > m_exposure ? m_charge : m_discharge;
    m_exposure = clean(exposure + memoryPole * (m_exposure - exposure));
    const double table = std::clamp(m_exposure, 0., 1.) * (m_slowRelease.size() - 1);
    const unsigned at = std::min(unsigned(table), unsigned(m_slowRelease.size() - 2));
    const double slowPole = std::lerp(m_slowRelease[at], m_slowRelease[at + 1], table - at);
    m_fastCell = clean(target + (target > m_fastCell ? m_attack : m_fastRelease) * (m_fastCell - target));
    m_slowCell = clean(target + (target > m_slowCell ? m_attack : slowPole) * (m_slowCell - target));
    const double conductance = std::lerp(m_fastCell, m_slowCell, .08 + .28 * m_exposure);
    const double attenuation = 1 / (1 + conductance);
    m_reduction = 20 / std::numbers::ln10 * std::log1p(conductance);
    constexpr double zero1 = .029991003238820143, zero2 = .014998875101240779;
    constexpr double norm1 = 1 / (.4 * (1 - zero1 * zero1));
    constexpr double norm2 = 1 / (.38 * (1 - zero2 * zero2));
    for (unsigned ch = 0; ch < channels; ++ch) {
        auto& s = m_channels[ch];
        double y = triode(x[ch] * attenuation * m_gain, .4, .03, zero1, norm1);
        y = s.interstage.tick(y, m_coupling);
        y = triode(y, .38, .015, zero2, norm2);
        s.outputFlux = clean(s.outputFlux + m_outputFlux * (y - s.outputFlux));
        y += std::tanh(.32 * s.outputFlux) / .32 - s.outputFlux;
        y = s.outputCoupling.tick(y, m_coupling);
        s.highCut = clean(s.highCut + m_highCut * (y - s.highCut));
        x[ch] = s.highCut;
    }
    return x;
}
void Cla2aInstance::render(const PluginProcessContext& c, unsigned offset, unsigned frames) noexcept {
    if (!frames) return;
    m_gainTarget = gainAmplitude(parameterValue(0)); m_peakTarget = peakDrive(parameterValue(1)); m_modeTarget = parameterValue(2);
    const unsigned channels = std::min(unsigned(m_layout.outputs[0]), 2u);
    for (unsigned frame = offset; frame < offset + frames; ++frame) {
        std::array<double, 2> input{}, at2{}, at4{}, output{};
        for (unsigned ch = 0; ch < channels; ++ch) {
            input[ch] = inputSample(c, ch, frame);
            m_blockPeaks[0] = std::max(m_blockPeaks[0], float(std::abs(input[ch])));
        }
        for (unsigned phase2 = 0; phase2 < 2; ++phase2) {
            for (unsigned ch = 0; ch < channels; ++ch)
                at2[ch] = m_channels[ch].up2.tick(phase2 == 0 ? 2 * input[ch] : 0, m_filter);
            std::array<double, 2> down4{};
            for (unsigned phase4 = 0; phase4 < 2; ++phase4) {
                for (unsigned ch = 0; ch < channels; ++ch)
                    at4[ch] = m_channels[ch].up4.tick(phase4 == 0 ? 2 * at2[ch] : 0, m_filter);
                const auto wet = tick4x(at4, channels);
                for (unsigned ch = 0; ch < channels; ++ch) {
                    const double down = m_channels[ch].down4.tick(wet[ch], m_filter);
                    if (phase4 == 0) down4[ch] = down;
                }
                m_blockPeaks[2] = std::max(m_blockPeaks[2], float(m_reduction));
            }
            for (unsigned ch = 0; ch < channels; ++ch) {
                const double down = m_channels[ch].down2.tick(down4[ch], m_filter);
                if (phase2 == 0) output[ch] = down;
            }
        }
        for (unsigned ch = 0; ch < channels; ++ch) {
            if (c.outputs && ch < c.outputChannels && c.outputs[ch]) c.outputs[ch][frame] = float(output[ch]);
            m_blockPeaks[1] = std::max(m_blockPeaks[1], float(std::abs(output[ch])));
        }
    }
}
PluginProcessDisposition Cla2aInstance::process(const PluginProcessContext& c) noexcept {
    const auto silence = [&](unsigned first) {
        if (c.outputs) for (unsigned ch = first; ch < c.outputChannels; ++ch)
            if (c.outputs[ch]) std::fill_n(c.outputs[ch], c.frames, 0.f);
    };
    if (!m_active) { silence(0); return PluginProcessDisposition::Error; }
    m_blockPeaks = {};
    unsigned position = 0;
    for (const auto& event : c.inputEvents) {
        if (event.kind != PluginEvent::Kind::ParamValue || event.paramIndex >= kParameterCount) continue;
        const unsigned at = std::clamp(event.frameOffset, position, c.frames);
        render(c, position, at - position); setParameterFromHost(event.paramIndex, event.value); position = at;
    }
    render(c, position, c.frames - position);
    silence(unsigned(m_layout.outputs[0]));
    for (unsigned i = 0; i < m_peaks.size(); ++i) {
        float previous = m_peaks[i].load(std::memory_order_relaxed);
        if (!m_peaks[i].compare_exchange_strong(previous, std::max(previous, m_blockPeaks[i]), std::memory_order_relaxed))
            m_peaks[i].store(m_blockPeaks[i], std::memory_order_relaxed);
    }
    return PluginProcessDisposition::Continue;
}
std::uint32_t Cla2aInstance::tailSamples() const noexcept { return std::uint32_t(std::ceil(m_rate)); }
Telemetry Cla2aInstance::consumeTelemetry() noexcept {
    return {m_peaks[0].exchange(0, std::memory_order_relaxed), m_peaks[1].exchange(0, std::memory_order_relaxed),
            m_peaks[2].exchange(0, std::memory_order_relaxed)};
}
} // namespace daw::plugins::cla2a
