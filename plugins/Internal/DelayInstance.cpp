#include "Internal/DelayInstance.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace daw::plugins::delay {
namespace dsp = modulation::dsp;
namespace {
static_assert(std::atomic<double>::is_always_lock_free && std::atomic<float>::is_always_lock_free);
constexpr std::array<std::string_view, kDivisionCount> divisions{
    "1/64", "1/64 D", "1/64 T", "1/32", "1/32 D", "1/32 T",
    "1/16", "1/16 D", "1/16 T", "1/8", "1/8 D", "1/8 T",
    "1/4", "1/4 D", "1/4 T", "1/2", "1/2 D", "1/2 T",
    "1/1", "1/1 D", "1/1 T", "1 bar", "2 bars"};
constexpr std::array<std::string_view, kCharacterCount> characters{
    "Clean", "Telephone", "Radio", "FM", "Tape", "Drive", "Lo-Fi"};
double input(const PluginProcessContext& c, unsigned ch, unsigned frame) noexcept {
    if (!c.inputs || !c.inputChannels) return 0;
    ch = std::min(ch, unsigned(c.inputChannels - 1));
    if (!c.inputs[ch]) return 0;
    const double v = c.inputs[ch][frame];
    return std::isfinite(v) ? std::clamp(v, -64.0, 64.0) : 0;
}
double hpCoefficient(double hz, double rate) noexcept {
    return hz <= 20 ? 1 : std::exp(-2 * dsp::pi * std::min(hz, .45 * rate) / rate);
}
double lpCoefficient(double hz, double rate) noexcept {
    return hz >= 20000 ? 0 : std::exp(-2 * dsp::pi * std::min(hz, .45 * rate) / rate);
}
}
std::span<const ParameterInfo> parameterTable() noexcept {
    static const std::array<ParameterInfo, kParameterCount> table{{
        {0, "timeMode", "Time source", "", 0, 2, 0, true, true},
        {1, "timeMs", "Time", "ms", 1, 8000, 375},
        {2, "division", "Division", "", 0, 22, 10, true, true},
        {3, "bpm", "Local tempo", "BPM", 30, 300, 120},
        {4, "mode", "Routing", "", 0, 1, 0, true, true},
        {5, "feedback", "Feedback", "%", 0, 95, 35},
        {6, "mix", "Dry / Wet", "%", 0, 100, 25},
        {7, "output", "Output", "dB", -18, 18, 0},
        {8, "character", "Character", "", 0, 6, 0, true, true},
        {9, "characterAmount", "Amount", "%", 0, 100, 50},
        {10, "lowCut", "Low Cut", "Hz", 20, 20000, 20},
        {11, "highCut", "High Cut", "Hz", 20, 20000, 20000},
        {12, "modDepth", "Depth", "%", 0, 100, 0},
        {13, "modRate", "Rate", "Hz", .1, 10, .3}
    }};
    return table;
}
std::string_view divisionName(unsigned i) noexcept { return divisions[std::min(i, kDivisionCount - 1)]; }
std::string_view characterName(unsigned i) noexcept { return characters[std::min(i, kCharacterCount - 1)]; }
Timing timing(std::span<const double> p, const engine::TransportInfo& transport) noexcept {
    Timing result;
    if (p.size() < kParameterCount) return result;
    result.bpm = p[0] == 1 ? p[3] : transport.tempo;
    if (!std::isfinite(result.bpm) || result.bpm <= 0) result.bpm = 120;
    const unsigned d = unsigned(std::clamp(p[2], 0., 22.));
    double quarters;
    if (d < 21) quarters = std::exp2(double(d / 3) - 4) * (d % 3 == 1 ? 1.5 : d % 3 == 2 ? 2. / 3 : 1);
    else quarters = std::max(1, transport.timeSigNumerator) * 4. / std::max(1, transport.timeSigDenominator) * (d == 22 ? 2 : 1);
    result.requestedMs = p[0] == 2 ? p[1] : 60000. / result.bpm * quarters;
    result.milliseconds = std::clamp(result.requestedMs, 1., 8000.);
    return result;
}
DelayInstance::DelayInstance() {
    for (const auto& p : parameterTable()) m_values[p.index].store(p.defaultValue);
}
const PluginDescriptor& DelayInstance::staticDescriptor() noexcept {
    static const PluginDescriptor d = [] {
        PluginDescriptor p; p.format = Format::Internal; p.uid = p.path = "daw.delay";
        p.name = "Flowers Delay"; p.vendor = "VLTONE"; p.version = "1.0";
        p.category = "Effect|Delay"; p.stateSchemaVersion = 1; return p;
    }();
    return d;
}
bool DelayInstance::setBusLayout(const PluginBusLayout& wanted, PluginBusLayout& accepted) {
    if (wanted.inputs.size() > 1 || wanted.outputs.size() > 1) return false;
    const auto channels = wanted.inputs.empty() ? std::uint16_t(2) : wanted.inputs[0];
    if ((channels != 1 && channels != 2) || (!wanted.outputs.empty() && wanted.outputs[0] != channels)) return false;
    m_layout = {{channels}, {channels}}; accepted = m_layout; return true;
}
bool DelayInstance::activate(const PluginProcessInfo& info) {
    if (!std::isfinite(info.sampleRate) || info.sampleRate < 8000 || info.sampleRate > 384000 || !info.maxBlockSize) return false;
    m_rate = info.sampleRate; m_smoothStep = 1 - dsp::pole(.010, m_rate); m_fadeStep = 1 / (.030 * m_rate);
    m_radioAttack = dsp::pole(.005, m_rate); m_radioRelease = dsp::pole(.100, m_rate);
    for (auto& line : m_lines) line.prepare(m_rate, 8.006);
    for (auto& line : m_colourLines) line.prepare(m_rate, .012);
    for (auto& tone : m_phoneTone) tone.tune(300, 3400, m_rate);
    for (auto& tone : m_radioTone) tone.tune(180, 5000, m_rate);
    for (auto& tone : m_tapeTone) tone.tune(0, 6000, m_rate);
    m_active = true; m_processing = false; reset(); return true;
}
double DelayInstance::clamp(unsigned i, double v) noexcept {
    const auto& p = parameterTable()[i];
    v = std::isfinite(v) ? std::clamp(v, p.minValue, p.maxValue) : p.defaultValue;
    return p.isStepped ? std::round(v) : v;
}
std::int32_t DelayInstance::parameterIndexForId(std::string_view id) const noexcept {
    for (const auto& p : parameterTable()) if (p.id == id) return std::int32_t(p.index);
    return -1;
}
double DelayInstance::parameterValue(std::uint32_t i) const noexcept { return i < kParameterCount ? m_values[i].load(std::memory_order_relaxed) : 0; }
void DelayInstance::setParameterFromHost(std::uint32_t i, double v) { if (i < kParameterCount) m_values[i].store(clamp(i, v), std::memory_order_relaxed); }
std::string DelayInstance::parameterText(std::uint32_t i, double v) const {
    if (i >= kParameterCount) return {};
    v = clamp(i, v);
    if (i == 0) return v == 0 ? "Host" : v == 1 ? "BPM" : "ms";
    if (i == 2) return std::string(divisionName(unsigned(v)));
    if (i == 4) return v == 0 ? "Stereo" : "Ping Pong";
    if (i == 8) return std::string(characterName(unsigned(v)));
    char text[64]; std::snprintf(text, sizeof(text), "%.1f %s", v, parameterTable()[i].unit.c_str()); return text;
}
bool DelayInstance::saveState(std::vector<std::uint8_t>& out) const {
    nlohmann::json params = nlohmann::json::object();
    for (const auto& p : parameterTable()) params[p.id] = parameterValue(p.index);
    const auto state = nlohmann::json{{"version", 1}, {"params", params}}.dump(); out.assign(state.begin(), state.end()); return true;
}
bool DelayInstance::loadState(std::span<const std::uint8_t> state) {
    if (state.empty() || state.size() > 65536) return false;
    const auto doc = nlohmann::json::parse(state.begin(), state.end(), nullptr, false);
    if (!doc.is_object() || doc.value("version", nlohmann::json{}) != 1 || !doc.contains("params") || !doc["params"].is_object()) return false;
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
void DelayInstance::reset() noexcept {
    for (unsigned i = 0; i < kParameterCount; ++i) m_smooth[i] = parameterValue(i);
    for (auto& line : m_lines) line.reset();
    for (auto& line : m_colourLines) line.reset();
    for (auto* tones : {&m_loopTone, &m_phoneTone, &m_radioTone, &m_tapeTone}) for (auto& tone : *tones) tone.reset();
    for (auto& tone : m_loopTone) { tone.hp = hpCoefficient(m_smooth[10], m_rate); tone.lp = lpCoefficient(m_smooth[11], m_rate); }
    m_delayFrom = m_delayTo = m_wanted = 0; m_delayFade = 1;
    m_route = m_routeFrom = m_routeTo = m_smooth[4]; m_routeFade = 1;
    m_character = unsigned(m_smooth[8]); m_weights = {}; m_weights[m_character] = 1;
    m_weightFrom = m_weightTo = m_weights; m_characterFade = 1;
    m_modPhase = m_fmPhase = m_tapePhase = m_radioEnvelope = 0; m_holdPhase = 1; m_held = {};
    m_blockPeaks = {}; for (auto& p : m_peaks) p.store(0, std::memory_order_relaxed);
    m_tailMs.store(8000, std::memory_order_relaxed);
}
void DelayInstance::render(const PluginProcessContext& c, unsigned offset, unsigned count) noexcept {
    if (!count) return;
    std::array<double, kParameterCount> p;
    for (unsigned i = 0; i < kParameterCount; ++i) p[i] = parameterValue(i);
    m_wanted = timing(p, c.transport).milliseconds * m_rate / 1000;
    if (m_delayTo == 0) m_delayFrom = m_delayTo = m_wanted;
    const bool mono = c.outputChannels == 1;
    const double route = mono ? 0 : p[4];
    if (mono) { m_route = m_routeFrom = m_routeTo = 0; m_routeFade = 1; }
    if (route != m_routeTo) { m_routeFrom = m_route; m_routeTo = route; m_routeFade = 0; }
    if (unsigned(p[8]) != m_character) {
        m_character = unsigned(p[8]); m_weightFrom = m_weights; m_weightTo = {};
        m_weightTo[m_character] = 1; m_characterFade = 0;
    }
    const double hp = hpCoefficient(p[10], m_rate), lp = lpCoefficient(p[11], m_rate);
    const double gainTarget = std::pow(10., p[7] / 20.);
    for (unsigned frame = offset; frame < offset + count; ++frame) {
        for (unsigned i : {5u, 6u, 9u, 12u, 13u}) m_smooth[i] += m_smoothStep * (p[i] - m_smooth[i]);
        // Store gain in dB so reset and live automation share the same scale.
        m_smooth[7] += m_smoothStep * (p[7] - m_smooth[7]);
        const double gain = std::abs(m_smooth[7] - p[7]) < 1.e-8 ? gainTarget : std::pow(10., m_smooth[7] / 20.);
        if (m_delayFade >= 1 && std::abs(m_wanted - m_delayTo) > 1.e-6) {
            m_delayFrom = m_delayTo; m_delayTo = m_wanted; m_delayFade = 0;
        }
        m_delayFade = std::min(1., m_delayFade + m_fadeStep);
        m_routeFade = std::min(1., m_routeFade + m_fadeStep); m_route = std::lerp(m_routeFrom, m_routeTo, m_routeFade);
        m_characterFade = std::min(1., m_characterFade + m_fadeStep);
        for (unsigned i = 0; i < kCharacterCount; ++i) m_weights[i] = std::lerp(m_weightFrom[i], m_weightTo[i], m_characterFade);
        const double dry[2]{input(c, 0, frame), input(c, 1, frame)};
        std::array<double, 2> wet{}, radio{};
        const double depth = std::min(.005 * m_rate * m_smooth[12] / 100, .4 * std::min(m_delayFrom, m_delayTo));
        for (unsigned ch = 0; ch < 2; ++ch) {
            const double modulation = depth * std::sin(2 * dsp::pi * (m_modPhase + (ch && !mono ? .25 : 0)));
            const double delayed = std::lerp(m_lines[ch].readLinear(m_delayFrom + modulation), m_lines[ch].readLinear(m_delayTo + modulation), m_delayFade);
            auto& tone = m_loopTone[ch]; tone.hp += m_smoothStep * (hp - tone.hp); tone.lp += m_smoothStep * (lp - tone.lp);
            wet[ch] = tone.process(delayed);
            radio[ch] = m_radioTone[ch].process(wet[ch]);
            m_colourLines[ch].write(wet[ch]);
        }
        const double radioLevel = std::max(std::abs(radio[0]), std::abs(radio[1]));
        const double pole = radioLevel > m_radioEnvelope ? m_radioAttack : m_radioRelease;
        m_radioEnvelope = pole * m_radioEnvelope + (1 - pole) * radioLevel;
        const double radioGain = m_radioEnvelope > .0630957 ? std::pow(.0630957 / m_radioEnvelope, .75) : 1;
        m_holdPhase += std::min(1., 12000 / m_rate);
        const bool hold = m_holdPhase >= 1; if (hold) m_holdPhase -= std::floor(m_holdPhase);
        const double fmRead = m_rate * (.001 + .0005 * std::sin(2 * dsp::pi * m_fmPhase));
        const double tapeRead = m_rate * (.003 + .0003 * std::sin(2 * dsp::pi * m_tapePhase));
        for (unsigned ch = 0; ch < 2; ++ch) {
            const double injected = std::lerp(dry[ch], ch ? 0. : .5 * (dry[0] + dry[1]), m_route);
            const double returned = std::lerp(wet[ch], wet[1 - ch], m_route);
            m_lines[ch].write(dsp::flush(injected + returned * m_smooth[5] / 100)); m_lines[ch].advance();
            if (hold) m_held[ch] = std::round(std::clamp(wet[ch], -1., 1.) * 511.) / 511.;
            const std::array<double, kCharacterCount> colours{
                wet[ch], std::tanh(1.6 * m_phoneTone[ch].process(wet[ch])) / 1.6,
                std::tanh(2.5 * radio[ch] * radioGain) / 1.5,
                m_colourLines[ch].readLinear(fmRead),
                std::tanh(2 * m_tapeTone[ch].process(m_colourLines[ch].readLinear(tapeRead))) / 1.6,
                std::tanh(7.94 * wet[ch]) / 2.5, m_held[ch]};
            double coloured = 0; for (unsigned i = 0; i < kCharacterCount; ++i) coloured += m_weights[i] * colours[i];
            const double effected = std::lerp(wet[ch], coloured, m_smooth[9] / 100);
            const double result = std::lerp(dry[ch], effected, m_smooth[6] / 100) * gain;
            if (c.outputs && ch < c.outputChannels && c.outputs[ch]) c.outputs[ch][frame] = float(dsp::clean(result));
            m_colourLines[ch].advance();
            m_blockPeaks[0] = std::max(m_blockPeaks[0], float(std::abs(dry[ch])));
            m_blockPeaks[1] = std::max(m_blockPeaks[1], float(std::abs(effected)));
            m_blockPeaks[2] = std::max(m_blockPeaks[2], float(std::abs(result)));
        }
        m_modPhase += m_smooth[13] / m_rate; m_modPhase -= std::floor(m_modPhase);
        m_fmPhase += 110 / m_rate; m_fmPhase -= std::floor(m_fmPhase);
        m_tapePhase += .47 / m_rate; m_tapePhase -= std::floor(m_tapePhase);
    }
    m_tailMs.store(std::max({m_delayFrom, m_delayTo, m_wanted}) / m_rate * 1000, std::memory_order_relaxed);
}
PluginProcessDisposition DelayInstance::process(const PluginProcessContext& c) noexcept {
    if (!m_active) {
        if (c.outputs) for (unsigned ch = 0; ch < c.outputChannels; ++ch) if (c.outputs[ch]) std::fill_n(c.outputs[ch], c.frames, 0.f);
        return PluginProcessDisposition::Error;
    }
    m_blockPeaks = {}; unsigned position = 0;
    for (const auto& e : c.inputEvents) {
        if (e.kind != PluginEvent::Kind::ParamValue || e.paramIndex >= kParameterCount) continue;
        const unsigned at = std::clamp(e.frameOffset, position, c.frames);
        render(c, position, at - position); setParameterFromHost(e.paramIndex, e.value); position = at;
    }
    render(c, position, c.frames - position);
    if (c.outputs) for (unsigned ch = 2; ch < c.outputChannels; ++ch) if (c.outputs[ch]) std::fill_n(c.outputs[ch], c.frames, 0.f);
    for (unsigned i = 0; i < 3; ++i) {
        float previous = m_peaks[i].load(std::memory_order_relaxed);
        if (!m_peaks[i].compare_exchange_strong(previous, std::max(previous, m_blockPeaks[i]), std::memory_order_relaxed)) m_peaks[i].store(m_blockPeaks[i], std::memory_order_relaxed);
    }
    return PluginProcessDisposition::Continue;
}
std::uint32_t DelayInstance::tailSamples() const noexcept {
    const double feedback = parameterValue(unsigned(Param::Feedback)) / 100;
    const double repeats = feedback > 0 ? 1 + std::ceil(std::log(1.e-6) / std::log(feedback)) : 1;
    const double ms = std::max(m_tailMs.load(std::memory_order_relaxed), parameterValue(unsigned(Param::TimeMs)));
    return std::uint32_t(std::min(2147483646., m_rate * (ms / 1000 * repeats + .5)));
}
Telemetry DelayInstance::consumeTelemetry() noexcept {
    return {m_peaks[0].exchange(0, std::memory_order_relaxed), m_peaks[1].exchange(0, std::memory_order_relaxed), m_peaks[2].exchange(0, std::memory_order_relaxed)};
}
} // namespace daw::plugins::delay
