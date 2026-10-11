#include "Internal/UtilityInstance.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace daw::plugins::utility {
namespace {
constexpr std::array<std::string_view, 5> divisions{"1/16", "1/8", "1/4", "1/2", "1/1"};
constexpr std::array<std::string_view, 5> modes{"Stereo", "Mono / Mid", "Side", "Left", "Right"};
double input(const PluginProcessContext& c, unsigned ch, unsigned frame) noexcept {
    if (!c.inputs || !c.inputChannels) return 0;
    ch = std::min(ch, unsigned(c.inputChannels - 1));
    const double v = c.inputs[ch] ? c.inputs[ch][frame] : 0;
    return std::isfinite(v) ? v : 0;
}
}
std::span<const ParameterInfo> parameterTable(Kind kind) noexcept {
    static const std::array<ParameterInfo, 2> gain{{
        {0, "gain", "Gain", "dB", -60, 24, 0},
        {1, "mute", "Mute", "", 0, 1, 0, true, true}
    }};
    static const std::array<ParameterInfo, 5> stereo{{
        {0, "mode", "Mode", "", 0, 4, 0, true, true},
        {1, "width", "Width", "%", 0, 200, 100},
        {2, "swap", "Swap L/R", "", 0, 1, 0, true, true},
        {3, "invertLeft", "Invert L", "", 0, 1, 0, true, true},
        {4, "invertRight", "Invert R", "", 0, 1, 0, true, true}
    }};
    static const std::array<ParameterInfo, 4> reverse{{
        {0, "timeMode", "Time source", "", 0, 1, 0, true, true},
        {1, "timeMs", "Length", "ms", 50, 4000, 500},
        {2, "division", "Division", "", 0, 4, 2, true, true},
        {3, "mix", "Dry / Wet", "%", 0, 100, 100}
    }};
    if (kind == Kind::Gain) return gain;
    if (kind == Kind::Stereo) return stereo;
    return reverse;
}
const PluginDescriptor& descriptorFor(Kind kind) noexcept {
    static const std::array<PluginDescriptor, 3> descriptors = [] {
        std::array<PluginDescriptor, 3> result;
        constexpr std::array ids{"daw.gain", "daw.stereo-utility", "daw.reverse"};
        constexpr std::array names{"Gain", "Stereo Utility", "Reverse"};
        for (unsigned i = 0; i < result.size(); ++i) {
            auto& p = result[i]; p.format = Format::Internal; p.uid = p.path = ids[i];
            p.name = names[i]; p.vendor = "VLTONE"; p.version = "1.0";
            p.category = "Effect|Utility"; p.stateSchemaVersion = 1;
        }
        return result;
    }();
    return descriptors[unsigned(kind)];
}
bool isUtilityUid(std::string_view uid) noexcept {
    return uid == "daw.gain" || uid == "daw.stereo-utility" || uid == "daw.reverse";
}
double reverseMilliseconds(double mode, double milliseconds, double division, double tempo) noexcept {
    if (!std::isfinite(tempo) || tempo <= 0) tempo = 120;
    if (!std::isfinite(milliseconds)) milliseconds = 500;
    if (!std::isfinite(division)) division = 2;
    const auto d = unsigned(std::clamp(std::round(division), 0., 4.));
    return std::clamp(mode >= .5 ? milliseconds : 60000. / tempo * std::exp2(double(d) - 2), 50., 4000.);
}
UtilityInstance::UtilityInstance(Kind kind) : m_kind(kind) {
    static_assert(std::atomic<double>::is_always_lock_free);
    for (const auto& p : parameters()) m_values[p.index].store(p.defaultValue);
    reset();
}
bool UtilityInstance::setBusLayout(const PluginBusLayout& wanted, PluginBusLayout& accepted) {
    if (wanted.inputs.size() > 1 || wanted.outputs.size() > 1) return false;
    const auto ch = wanted.inputs.empty() ? std::uint16_t(2) : wanted.inputs[0];
    if ((ch != 1 && ch != 2) || (!wanted.outputs.empty() && wanted.outputs[0] != ch)) return false;
    m_layout = {{ch}, {ch}}; accepted = m_layout; return true;
}
bool UtilityInstance::activate(const PluginProcessInfo& info) {
    if (!std::isfinite(info.sampleRate) || info.sampleRate < 8000 || info.sampleRate > 384000 || !info.maxBlockSize) return false;
    m_rate = info.sampleRate;
    m_rampFrames = std::max(1u, unsigned(std::lround(.010 * m_rate)));
    m_edgeFrames = std::max(1u, unsigned(std::lround(.003 * m_rate)));
    if (m_kind == Kind::Reverse)
        for (auto& lane : m_reverse)
            for (auto& channel : lane.samples) channel.resize(2 * std::size_t(std::ceil(4 * m_rate)));
    m_active = true; m_processing = false; reset(); return true;
}
double UtilityInstance::clamp(unsigned i, double value) const noexcept {
    const auto& p = parameters()[i];
    value = std::isfinite(value) ? std::clamp(value, p.minValue, p.maxValue) : p.defaultValue;
    return p.isStepped ? std::round(value) : value;
}
std::int32_t UtilityInstance::parameterIndexForId(std::string_view id) const noexcept {
    for (const auto& p : parameters()) if (p.id == id) return std::int32_t(p.index);
    return -1;
}
double UtilityInstance::parameterValue(std::uint32_t i) const noexcept {
    return i < parameters().size() ? m_values[i].load(std::memory_order_relaxed) : 0;
}
void UtilityInstance::setParameterFromHost(std::uint32_t i, double value) {
    if (i < parameters().size()) m_values[i].store(clamp(i, value), std::memory_order_relaxed);
}
std::string UtilityInstance::parameterText(std::uint32_t i, double value) const {
    if (i >= parameters().size()) return {};
    value = clamp(i, value);
    if (m_kind == Kind::Stereo && i == 0) return std::string(modes[unsigned(value)]);
    if (m_kind == Kind::Reverse && i == 0) return value == 0 ? "Host sync" : "Milliseconds";
    if (m_kind == Kind::Reverse && i == 2) return std::string(divisions[unsigned(value)]);
    const auto& p = parameters()[i];
    if (p.isStepped && p.maxValue == 1) return value >= .5 ? "On" : "Off";
    char text[64]; std::snprintf(text, sizeof(text), "%.1f %s", value, p.unit.c_str()); return text;
}
bool UtilityInstance::saveState(std::vector<std::uint8_t>& out) const {
    nlohmann::json params = nlohmann::json::object();
    for (const auto& p : parameters()) params[p.id] = parameterValue(p.index);
    const auto state = nlohmann::json{{"version", 1}, {"uid", descriptor().uid}, {"params", params}}.dump();
    out.assign(state.begin(), state.end()); return true;
}
bool UtilityInstance::loadState(std::span<const std::uint8_t> state) {
    if (state.empty() || state.size() > 65536) return false;
    const auto doc = nlohmann::json::parse(state.begin(), state.end(), nullptr, false);
    if (!doc.is_object() || doc.value("version", nlohmann::json{}) != 1 ||
        doc.value("uid", nlohmann::json{}) != descriptor().uid ||
        !doc.contains("params") || !doc["params"].is_object()) return false;
    std::array<double, kParameterCapacity> values{};
    for (const auto& p : parameters()) {
        values[p.index] = p.defaultValue;
        if (const auto it = doc["params"].find(p.id); it != doc["params"].end()) {
            if (!it->is_number() || !std::isfinite(it->get<double>())) return false;
            values[p.index] = clamp(p.index, it->get<double>());
        }
    }
    for (const auto& p : parameters()) setParameterFromHost(p.index, values[p.index]);
    return true;
}
void UtilityInstance::Ramp::set(double v, unsigned frames) noexcept {
    if (v == target) return;
    target = v; remaining = frames; step = (target - value) / frames;
}
double UtilityInstance::Ramp::next() noexcept {
    if (remaining) { value += step; if (!--remaining) value = target; }
    return value;
}
std::array<double, 4> UtilityInstance::matrix(bool mono) const noexcept {
    const unsigned mode = unsigned(parameterValue(0));
    const double w = parameterValue(1) / 100;
    std::array<double, 4> a;
    switch (mode) {
        case 1: a = {.5, .5, .5, .5}; break;
        case 2: a = {.5, -.5, -.5, .5}; break;
        case 3: a = {1, 0, 1, 0}; break;
        case 4: a = {0, 1, 0, 1}; break;
        default: a = {(1+w)*.5, (1-w)*.5, (1-w)*.5, (1+w)*.5}; break;
    }
    // A mono bus has one physical channel: R controls cannot manufacture side.
    if (mono) {
        const double sign = parameterValue(3) >= .5 ? -1 : 1;
        return mode == 2 ? std::array<double, 4>{} : std::array<double, 4>{sign, 0, sign, 0};
    }
    const double l = parameterValue(3) >= .5 ? -1 : 1;
    const double r = parameterValue(4) >= .5 ? -1 : 1;
    a[0] *= l; a[2] *= l; a[1] *= r; a[3] *= r;
    if (parameterValue(2) >= .5) { std::swap(a[0], a[1]); std::swap(a[2], a[3]); }
    return a;
}
void UtilityInstance::ReverseLane::reset(unsigned frames) noexcept {
    length = frames; position = bank = 0; ready = false;
    // Validity, not clearing megabytes on the audio thread, discards old audio.
}
std::array<double, 2> UtilityInstance::ReverseLane::tick(double l, double r, unsigned fade) noexcept {
    std::array<double, 2> out{};
    const unsigned write = bank * length + position;
    const unsigned read = (1 - bank) * length + length - 1 - position;
    const double envelope = std::min({1., double(position) / fade, double(length - 1 - position) / fade});
    const std::array in{l, r};
    for (unsigned ch = 0; ch < 2; ++ch) {
        if (ready) out[ch] = samples[ch][read] * envelope;
        samples[ch][write] = float(in[ch]);
    }
    if (++position == length) { position = 0; bank = 1 - bank; ready = true; }
    return out;
}
void UtilityInstance::reset() noexcept {
    m_gain.reset(parameterValue(1) >= .5 ? 0 : std::pow(10., parameterValue(0) / 20));
    m_mix.reset(m_kind == Kind::Reverse ? parameterValue(3) / 100 : 1);
    if (m_kind == Kind::Stereo) {
        const auto a = matrix(m_layout.inputs[0] == 1);
        for (unsigned i = 0; i < 4; ++i) m_matrix[i].reset(a[i]);
    }
    for (auto& lane : m_reverse) lane.reset(0);
    m_lane = m_crossfade = 0; m_pending = false;
    m_longestSlice.store(0, std::memory_order_relaxed);
}
void UtilityInstance::render(const PluginProcessContext& c, unsigned begin, unsigned end) noexcept {
    if (begin == end) return;
    unsigned length = 0;
    if (m_kind == Kind::Gain) {
        m_gain.set(parameterValue(1) >= .5 ? 0 : std::pow(10., parameterValue(0) / 20), m_rampFrames);
    } else if (m_kind == Kind::Stereo) {
        const auto a = matrix(c.inputChannels == 1);
        for (unsigned i = 0; i < 4; ++i) m_matrix[i].set(a[i], m_rampFrames);
    } else {
        length = unsigned(std::lround(reverseMilliseconds(parameterValue(0), parameterValue(1), parameterValue(2), c.transport.tempo) * m_rate / 1000));
        m_longestSlice.store(std::max(length, m_longestSlice.load(std::memory_order_relaxed)), std::memory_order_relaxed);
        if (!m_reverse[m_lane].length) m_reverse[m_lane].reset(length);
        m_mix.set(parameterValue(3) / 100, m_rampFrames);
    }
    for (unsigned frame = begin; frame < end; ++frame) {
        const double l = input(c, 0, frame), r = input(c, 1, frame);
        std::array<double, 2> out;
        if (m_kind == Kind::Gain) {
            const double gain = m_gain.next(); out = {l * gain, r * gain};
        } else if (m_kind == Kind::Stereo) {
            std::array<double, 4> a;
            for (unsigned i = 0; i < 4; ++i) a[i] = m_matrix[i].next();
            out = {a[0]*l + a[1]*r, a[2]*l + a[3]*r};
        } else {
            auto& current = m_reverse[m_lane];
            auto& next = m_reverse[1 - m_lane];
            if (!m_crossfade) {
                if (length == current.length) m_pending = false;
                else if (!m_pending || next.length != length) { next.reset(length); m_pending = true; }
            }
            out = current.tick(l, r, m_edgeFrames);
            if (m_pending) {
                if (next.ready && !m_crossfade) m_crossfade = 1;
                const auto wet = next.tick(l, r, m_edgeFrames);
                if (m_crossfade) {
                    const double blend = double(m_crossfade) / m_rampFrames;
                    for (unsigned ch = 0; ch < 2; ++ch) out[ch] = std::lerp(out[ch], wet[ch], blend);
                    if (++m_crossfade > m_rampFrames) { m_lane = 1 - m_lane; m_crossfade = 0; m_pending = false; }
                }
            }
            const double mix = m_mix.next();
            out = {std::lerp(l, out[0], mix), std::lerp(r, out[1], mix)};
        }
        if (c.outputs)
            for (unsigned ch = 0; ch < std::min(2u, unsigned(c.outputChannels)); ++ch)
                if (c.outputs[ch]) c.outputs[ch][frame] = float(out[ch]);
    }
}
PluginProcessDisposition UtilityInstance::process(const PluginProcessContext& c) noexcept {
    if (!m_active) {
        if (c.outputs) for (unsigned ch = 0; ch < c.outputChannels; ++ch)
            if (c.outputs[ch]) std::fill_n(c.outputs[ch], c.frames, 0.f);
        return PluginProcessDisposition::Error;
    }
    unsigned position = 0;
    for (const auto& e : c.inputEvents) {
        if (e.kind != PluginEvent::Kind::ParamValue || e.paramIndex >= parameters().size()) continue;
        const unsigned at = std::clamp(e.frameOffset, position, c.frames);
        render(c, position, at); setParameterFromHost(e.paramIndex, e.value); position = at;
    }
    render(c, position, c.frames);
    if (c.outputs) for (unsigned ch = 2; ch < c.outputChannels; ++ch)
        if (c.outputs[ch]) std::fill_n(c.outputs[ch], c.frames, 0.f);
    return PluginProcessDisposition::Continue;
}
std::uint32_t UtilityInstance::tailSamples() const noexcept {
    if (m_kind != Kind::Reverse) return 0;
    // Retain the largest slice of this transport run, including pending changes.
    // Before the first block the host tempo is unknown, so report a safe bound.
    const unsigned observed = m_longestSlice.load(std::memory_order_relaxed);
    const unsigned configured = unsigned(std::ceil((parameterValue(0) >= .5 ? parameterValue(1) : 4000) * m_rate / 1000));
    return 2 * (observed ? observed : configured) + m_rampFrames;
}
} // namespace daw::plugins::utility
