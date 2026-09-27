#include "Internal/CompressorInstance.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>

namespace daw::plugins::compressor {
namespace {
static_assert(std::atomic<double>::is_always_lock_free && std::atomic<float>::is_always_lock_free);
double db(double amplitude) noexcept { return 20 * std::log10(std::max(amplitude, 1e-12)); }
double sample(const float* const* inputs, unsigned channels, unsigned channel, unsigned frame) noexcept {
    if (!inputs || channel >= channels || !inputs[channel]) return 0;
    const double value = inputs[channel][frame];
    return std::isfinite(value) ? std::clamp(value, -64.0, 64.0) : 0;
}
}
std::span<const ParameterInfo> parameterTable() noexcept {
    static const std::array<ParameterInfo, kParameterCount> table{{
        {0, "ratio", "Ratio", ":1", 1, 20, 3},
        {1, "threshold", "Threshold", "dB", -60, 0, -18},
        {2, "makeup", "Makeup", "dB", -12, 24, 0},
        {3, "attack", "Attack", "ms", .1, 100, 10},
        {4, "release", "Release", "ms", 10, 1500, 120},
        {5, "knee", "Knee", "dB", 0, 24, 6},
        {6, "mix", "Mix", "%", 0, 100, 100},
        {7, "mode", "Mode", "", 0, 1, 0, true, true},
        {8, "autoGain", "Auto Gain", "", 0, 1, 0, true, true},
    }};
    return table;
}
double reductionDb(double input, double threshold, double ratio, double knee) noexcept {
    const double above = input - threshold;
    const double slope = 1 - 1 / ratio;
    if (knee > 0 && above > -knee / 2 && above < knee / 2) {
        const double x = above + knee / 2;
        return slope * x * x / (2 * knee);
    }
    return above <= -knee / 2 ? 0 : slope * above;
}
double autoGainDb(double threshold, double ratio, double knee) noexcept {
    return std::clamp(reductionDb(-18, threshold, ratio, knee), 0.0, 12.0);
}
CompressorInstance::CompressorInstance() {
    for (const auto& p : parameterTable()) m_values[p.index].store(p.defaultValue);
    // Windowed-sinc half-band: 33 taps, exact zero even taps and 0.5 centre.
    // Each filter delays 16 samples at 2x; the pair delays 16 host samples.
    double sideSum = 0;
    for (int i = 0; i < 33; ++i) {
        const int x = i - 16;
        if (x % 2) {
            m_filter[unsigned(i)] = std::sin(std::numbers::pi * x / 2) /
                (std::numbers::pi * x) * (.54 - .46 * std::cos(2 * std::numbers::pi * i / 32));
            sideSum += m_filter[unsigned(i)];
        }
    }
    for (auto& tap : m_filter) tap *= .5 / sideSum;
    m_filter[16] = .5;
}
const PluginDescriptor& CompressorInstance::staticDescriptor() noexcept {
    static const PluginDescriptor descriptor = [] {
        PluginDescriptor d;
        d.format = Format::Internal; d.uid = d.path = "daw.compressor";
        d.name = "Compressor"; d.vendor = "VLTONE"; d.version = "1.0";
        d.category = "Effect|Dynamics|Compressor"; d.stateSchemaVersion = 1;
        return d;
    }();
    return descriptor;
}
bool CompressorInstance::setBusLayout(const PluginBusLayout& wanted, PluginBusLayout& accepted) {
    if (wanted.inputs.size() > 2 || wanted.outputs.size() > 1) return false;
    const auto main = wanted.inputs.empty() ? std::uint16_t(2) : wanted.inputs[0];
    const auto side = wanted.inputs.size() < 2 ? std::uint16_t(2) : wanted.inputs[1];
    if ((main != 1 && main != 2) || side > 2 ||
        (!wanted.outputs.empty() && wanted.outputs[0] != main)) return false;
    m_layout = {{main, side}, {main}}; accepted = m_layout; return true;
}
bool CompressorInstance::activate(const PluginProcessInfo& info) {
    if (!std::isfinite(info.sampleRate) || info.sampleRate < 8000 || info.sampleRate > 384000 || !info.maxBlockSize) return false;
    m_rate = info.sampleRate; m_sidechain = info.sidechainConnected;
    m_rmsCoefficient = std::exp(-1 / (.010 * m_rate));
    m_smoothCoefficient = std::exp(-1 / (.010 * m_rate));
    m_active = true; m_processing = false; reset(); return true;
}
std::int32_t CompressorInstance::parameterIndexForId(std::string_view id) const noexcept {
    for (const auto& p : parameterTable()) if (p.id == id) return std::int32_t(p.index);
    return -1;
}
double CompressorInstance::parameterValue(std::uint32_t index) const noexcept {
    return index < kParameterCount ? m_values[index].load(std::memory_order_relaxed) : 0;
}
double CompressorInstance::clamp(unsigned index, double value) noexcept {
    const auto& p = parameterTable()[index];
    value = std::isfinite(value) ? std::clamp(value, p.minValue, p.maxValue) : p.defaultValue;
    return p.isStepped ? std::round(value) : value;
}
void CompressorInstance::setParameterFromHost(std::uint32_t index, double value) {
    if (index < kParameterCount) m_values[index].store(clamp(index, value), std::memory_order_relaxed);
}
std::string CompressorInstance::parameterText(std::uint32_t index, double value) const {
    if (index >= kParameterCount) return {};
    value = clamp(index, value);
    if (index == unsigned(Param::Mode)) return value > .5 ? "Punch" : "Soft";
    if (index == unsigned(Param::AutoGain)) return value > .5 ? "On" : "Off";
    char text[64];
    std::snprintf(text, sizeof(text), index == 0 ? "%.1f:1" : "%.1f %s", value, parameterTable()[index].unit.c_str());
    return text;
}
bool CompressorInstance::saveState(std::vector<std::uint8_t>& out) const {
    nlohmann::json params = nlohmann::json::object();
    for (const auto& p : parameterTable()) params[p.id] = parameterValue(p.index);
    const auto text = nlohmann::json{{"version", 1}, {"params", params}}.dump();
    out.assign(text.begin(), text.end()); return true;
}
bool CompressorInstance::loadState(std::span<const std::uint8_t> state) {
    if (state.empty() || state.size() > 65536) return false;
    const auto doc = nlohmann::json::parse(state.begin(), state.end(), nullptr, false);
    if (!doc.is_object() || !doc.contains("version") || doc["version"] != 1 ||
        !doc.contains("params") || !doc["params"].is_object()) return false;
    std::array<double, kParameterCount> values{};
    for (const auto& p : parameterTable()) {
        values[p.index] = p.defaultValue;
        if (auto it = doc["params"].find(p.id); it != doc["params"].end()) {
            if (!it->is_number() || !std::isfinite(it->get<double>())) return false;
            values[p.index] = clamp(p.index, it->get<double>());
        }
    }
    for (unsigned i = 0; i < kParameterCount; ++i) setParameterFromHost(i, values[i]);
    return true;
}
double CompressorInstance::Fir::tick(double input, const std::array<double, 33>& taps) noexcept {
    history[cursor] = input;
    double result = 0;
    for (unsigned i = 0; i < 33; ++i) result += taps[i] * history[(cursor + 33 - i) % 33];
    cursor = (cursor + 1) % 33; return result;
}
void CompressorInstance::reset() noexcept {
    m_up = {}; m_down = {}; m_dry = {}; m_wet = {}; m_cursor = 0;
    m_power = m_softReduction = m_punchReduction = 0;
    m_mode = m_modeTarget = parameterValue(7); m_modeRemaining = 0; m_modeStep = 0;
    m_modeDelay.fill(m_mode);
    m_makeup = parameterValue(2); m_mix = parameterValue(6) / 100;
    m_auto = parameterValue(8) * autoGainDb(parameterValue(1), parameterValue(0), parameterValue(5));
    m_blockPeaks = {};
    for (auto& peak : m_peaks) peak.store(0, std::memory_order_relaxed);
}
void CompressorInstance::render(const PluginProcessContext& c, unsigned offset, unsigned frames) noexcept {
    if (!frames) return;
    std::array<double, kParameterCount> p;
    for (unsigned i = 0; i < kParameterCount; ++i) p[i] = parameterValue(i);
    const double attack = std::exp(-1 / (.001 * p[3] * m_rate));
    const double release = std::exp(-1 / (.001 * p[4] * m_rate));
    const double autoGain = p[8] * autoGainDb(p[1], p[0], p[5]);
    if (p[7] != m_modeTarget) {
        m_modeTarget = p[7]; m_modeRemaining = unsigned(std::max(1.0, std::round(.010 * m_rate)));
        m_modeStep = (m_modeTarget - m_mode) / m_modeRemaining;
    }
    constexpr double drive = 1.9952623149688795; // +6 dB; dividing by drive gives unity small-signal gain.
    for (unsigned frame = offset; frame < offset + frames; ++frame) {
        std::array<double, 2> input{sample(c.inputs, c.inputChannels, 0, frame),
                                  sample(c.inputs, c.inputChannels, 1, frame)};
        const double inPeak = std::max(std::abs(input[0]), std::abs(input[1]));
        // Connection state, not signal energy, chooses the detector. A routed
        // silent or temporarily missing sidechain must never fall back to main.
        const double detector = m_sidechain ? std::max(
            std::abs(sample(c.sidechainInputs, c.sidechainInputChannels, 0, frame)),
            std::abs(sample(c.sidechainInputs, c.sidechainInputChannels, 1, frame))) : inPeak;
        m_power = m_rmsCoefficient * m_power + (1 - m_rmsCoefficient) * detector * detector;
        if (m_power < 1e-24) m_power = 0;
        const double soft = reductionDb(db(std::sqrt(m_power)), p[1], p[0], p[5]);
        const double punch = reductionDb(db(detector), p[1], p[0], p[5]);
        const auto envelope = [&](double current, double target) {
            const double coefficient = target > current ? attack : release;
            const double value = coefficient * current + (1 - coefficient) * target;
            return value < 1e-12 ? 0 : value;
        };
        m_softReduction = envelope(m_softReduction, soft);
        m_punchReduction = envelope(m_punchReduction, punch);
        if (m_modeRemaining && !--m_modeRemaining) m_mode = m_modeTarget;
        else if (m_modeRemaining) m_mode += m_modeStep;
        m_auto = autoGain + m_smoothCoefficient * (m_auto - autoGain);
        m_makeup = p[2] + m_smoothCoefficient * (m_makeup - p[2]);
        m_mix = p[6] / 100 + m_smoothCoefficient * (m_mix - p[6] / 100);
        const double reduction = std::lerp(m_softReduction, m_punchReduction, m_mode);
        const double gain = std::pow(10.0, (m_makeup + m_auto - reduction) / 20);
        const double delayedMode = m_modeDelay[m_cursor]; m_modeDelay[m_cursor] = m_mode;
        double outPeak = 0;
        for (unsigned channel = 0; channel < 2; ++channel) {
            const double wet = input[channel] * gain;
            const double dryDelayed = m_dry[channel][m_cursor], wetDelayed = m_wet[channel][m_cursor];
            m_dry[channel][m_cursor] = input[channel]; m_wet[channel][m_cursor] = wet;
            const double even = m_up[channel].tick(2 * wet, m_filter);
            const double saturated = m_down[channel].tick(std::tanh(drive * even) / drive, m_filter);
            const double odd = m_up[channel].tick(0, m_filter);
            m_down[channel].tick(std::tanh(drive * odd) / drive, m_filter);
            const double processed = wetDelayed + .25 * delayedMode * (saturated - wetDelayed);
            const float output = float(std::lerp(dryDelayed, processed, m_mix));
            if (c.outputs && channel < c.outputChannels && c.outputs[channel]) c.outputs[channel][frame] = output;
            outPeak = std::max(outPeak, std::abs(double(output)));
        }
        m_cursor = (m_cursor + 1) % kLatency;
        m_blockPeaks[0] = std::max(m_blockPeaks[0], float(inPeak));
        m_blockPeaks[1] = std::max(m_blockPeaks[1], float(outPeak));
        m_blockPeaks[2] = std::max(m_blockPeaks[2], float(reduction));
    }
}
PluginProcessDisposition CompressorInstance::process(const PluginProcessContext& c) noexcept {
    if (!m_active) {
        if (c.outputs) for (unsigned ch = 0; ch < c.outputChannels; ++ch)
            if (c.outputs[ch]) std::fill_n(c.outputs[ch], c.frames, 0.f);
        return PluginProcessDisposition::Error;
    }
    m_blockPeaks = {};
    unsigned position = 0;
    for (const auto& event : c.inputEvents) {
        if (event.kind != PluginEvent::Kind::ParamValue || event.paramIndex >= kParameterCount) continue;
        const unsigned at = std::clamp(event.frameOffset, position, c.frames);
        render(c, position, at - position); setParameterFromHost(event.paramIndex, event.value); position = at;
    }
    render(c, position, c.frames - position);
    if (c.outputs) for (unsigned ch = 2; ch < c.outputChannels; ++ch)
        if (c.outputs[ch]) std::fill_n(c.outputs[ch], c.frames, 0.f);
    for (unsigned i = 0; i < 3; ++i) {
        float previous = m_peaks[i].load(std::memory_order_relaxed);
        // Single writer; a concurrent UI exchange may only lower this value.
        // One bounded CAS avoids any audio-thread retry loop.
        if (!m_peaks[i].compare_exchange_strong(previous, std::max(previous, m_blockPeaks[i]), std::memory_order_relaxed))
            m_peaks[i].store(m_blockPeaks[i], std::memory_order_relaxed);
    }
    return PluginProcessDisposition::Continue;
}
Telemetry CompressorInstance::consumeTelemetry() noexcept {
    return {m_peaks[0].exchange(0, std::memory_order_relaxed), m_peaks[1].exchange(0, std::memory_order_relaxed),
            m_peaks[2].exchange(0, std::memory_order_relaxed)};
}
} // namespace daw::plugins::compressor
