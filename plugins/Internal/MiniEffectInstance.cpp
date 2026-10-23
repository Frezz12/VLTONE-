#include "MiniEffectInstance.hpp"
#include "signalsmith-stretch.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <nlohmann/json.hpp>

namespace daw::plugins::mini {
namespace {
constexpr double pi = std::numbers::pi;
double clean(double x) noexcept { return std::isfinite(x) ? std::clamp(x, -1e6, 1e6) : 0.; }
double flush(double x) noexcept { return std::abs(x) < 1e-24 ? 0. : x; }
double read(const PluginProcessContext& c, unsigned ch, unsigned i) noexcept {
    return c.inputs && ch < c.inputChannels && c.inputs[ch] ? clean(c.inputs[ch][i]) : 0.;
}
// Topology-preserving integrators stay stable while the cutoff moves. Each
// channel owns its state; stereo shares coefficients, never audio history.
struct Filter {
    double z1 = 0, z2 = 0;
    double tick(double x, double g, double damping, bool high, bool first = false) noexcept {
        if (first) {
            const double v = (x - z1) * g / (1 + g), low = z1 + v;
            z1 = flush(low + v);
            return high ? x - low : low;
        }
        const double v1 = (z1 + g * (x - z2)) / (1 + g * (g + damping));
        const double v2 = z2 + g * v1;
        z1 = flush(2 * v1 - z1); z2 = flush(2 * v2 - z2);
        return high ? x - damping * v1 - v2 : v2;
    }
};
double clip(double x, double threshold) noexcept {
    // Independent C1 quadratic knee: unity below threshold, finite ceiling,
    // no look-ahead or oversampling delay. Not a verified Image-Line clone.
    const double a = std::abs(x), room = 1 - threshold;
    if (a <= threshold) return x;
    if (room < 1e-9 || a >= threshold + 2 * room) return std::copysign(1., x);
    const double over = a - threshold;
    return std::copysign(threshold + over - over * over / (4 * room), x);
}
}

std::span<const EffectType> effectTypes() noexcept {
    static constexpr EffectType types[]{
        {"tremolo", "Tremolo", EffectKind::Tremolo},
        {"stereo", "Stereo", EffectKind::Stereo},
        {"air", "Air", EffectKind::Air},
        {"pitch-formant", "Pitch / Formant", EffectKind::Pitch},
        {"filter", "Filter", EffectKind::Filter},
        {"soft-clip", "Soft Clip", EffectKind::SoftClip}};
    return types;
}
std::span<const ParameterInfo> effectParameters(EffectKind kind) noexcept {
    static const ParameterInfo tremolo[]{
        {0, "rate", "Rate", "Hz", .05, 20, 4, true, false, false, false, true},
        {1, "depth", "Depth", "%", 0, 100, 50},
        {2, "shape", "Shape", "", 0, 2, 0, false, true}};
    static const ParameterInfo stereo[]{
        {0, "width", "Width", "%", 0, 200, 125},
        {1, "safe", "Safe", "%", 0, 100, 100}};
    static const ParameterInfo air[]{
        {0, "presence", "Presence", "%", 0, 100, 0},
        {1, "air", "Air", "%", 0, 100, 0}};
    static const ParameterInfo pitch[]{
        {0, "pitch", "Pitch", "st", -12, 12, 0},
        {1, "formant", "Formant", "st", -12, 12, 0},
        {2, "quality", "Quality", "", 0, 1, 0, false, true}};
    static const ParameterInfo filter[]{
        {0, "lowcut", "Low Cut", "Hz", 20, 20000, 20, true, false, false, false, true},
        {1, "highcut", "High Cut", "Hz", 20, 20000, 20000, true, false, false, false, true},
        {2, "slope", "Slope", "", 0, 3, 2, false, true}};
    static const ParameterInfo soft[]{
        {0, "threshold", "Threshold", "dB", -48, 0, -6},
        {1, "post", "Post", "dB", -24, 12, 0}};
    switch (kind) {
        case EffectKind::Tremolo: return tremolo;
        case EffectKind::Stereo: return stereo;
        case EffectKind::Air: return air;
        case EffectKind::Pitch: return pitch;
        case EffectKind::Filter: return filter;
        case EffectKind::SoftClip: return soft;
    }
    return {};
}
std::unique_ptr<PluginInstance> createMiniEffect(std::string_view id) {
    for (const auto& type : effectTypes())
        if (type.id == id) return std::make_unique<MiniEffectInstance>(type.kind);
    return {};
}

struct MiniEffectInstance::DSP {
    double rate = 48000, pole = 0, phase = 0;
    std::array<double, 2> current{}, enabled{1, 1};
    std::array<std::array<Filter, 8>, 2> filters{};
    double midEnergy = 0, sideEnergy = 0, safeWidth = 1;
    std::array<double, 2> envelope{};
    double energyPole = 0, attackPole = 0, releasePole = 0;
    std::array<double, 2> airG{};
    std::array<double, 4> damping{};
    unsigned channels = 2, mode = 0, clock = 0, delayAt = 0, latency = 0;
    double wet = 0;
    double previous1 = 0, previous2 = 0, tonePower = 0, curvePower = 0, toneCurve = 0, richness = 0;
    std::unique_ptr<signalsmith::stretch::SignalsmithStretch<float>> stretch;
    std::array<std::vector<float>, 2> delay;
    std::array<std::array<float, 32>, 2> pitchIn{}, pitchOut{};

    double cutoff(double hz) const noexcept { return std::tan(pi * std::clamp(hz / rate, .00001, .45)); }
    double target(EffectKind kind, double value) const noexcept {
        if (kind == EffectKind::Filter) return cutoff(value);
        if (kind == EffectKind::SoftClip) return std::pow(10., value / 20);
        return value;
    }
    void render(EffectKind kind, const PluginProcessContext& c, unsigned begin,
                unsigned end, std::array<double, 2> values) noexcept {
        std::array<double, 2> targets{target(kind, values[0]), target(kind, values[1])};
        if (kind == EffectKind::Pitch) {
            for (unsigned at = begin; at < end;) {
                const unsigned count = std::min(end - at, 32 - clock);
                if (!clock) {
                    stretch->setTransposeSemitones(float(current[0]));
                    // Pitch and formant are independent; pitch alone preserves
                    // the spectral envelope instead of producing chipmunk vowels.
                    // A lone partial has no vowel envelope. Compensating that
                    // spectrum suppresses the shifted fundamental, so smoothly
                    // fade formant correction in as the spectrum becomes richer.
                    stretch->setFormantSemitones(float(std::lerp(current[0], current[1], richness)), true);
                }
                const float* in[]{pitchIn[0].data(), pitchIn[1].data()};
                float* out[]{pitchOut[0].data(), pitchOut[1].data()};
                for (unsigned i = 0; i < count; ++i) {
                    const double left = read(c, 0, at + i), right = read(c, 1, at + i);
                    pitchIn[0][i] = float(channels == 2 ? .5 * (left + right) : left);
                    pitchIn[1][i] = float(.5 * (left - right));
                }
                for (unsigned i = 0; i < count; ++i) {
                    const double x = std::abs(pitchIn[0][i]) >= std::abs(pitchIn[1][i]) ? pitchIn[0][i] : pitchIn[1][i];
                    const double curve = x + previous2 - 2 * previous1;
                    tonePower = flush(previous1 * previous1 + energyPole * (tonePower - previous1 * previous1));
                    curvePower = flush(curve * curve + energyPole * (curvePower - curve * curve));
                    toneCurve = flush(previous1 * curve + energyPole * (toneCurve - previous1 * curve));
                    const double coherence = toneCurve * toneCurve / (tonePower * curvePower + 1e-30);
                    const double targetRichness = std::clamp((1 - coherence - .005) / .045, 0., 1.);
                    richness = targetRichness + pole * (richness - targetRichness);
                    previous2 = previous1; previous1 = x;
                }
                stretch->process(in, int(count), out, int(count));
                const double wantedWet = std::abs(current[0]) + std::abs(current[1]) > 1e-5 ? 1 : 0;
                for (unsigned i = 0; i < count; ++i) {
                    wet = wantedWet + pole * (wet - wantedWet);
                    if (std::abs(wet - wantedWet) < 1e-8) wet = wantedWet;
                    for (unsigned ch = 0; ch < channels; ++ch) {
                        const double dry = delay[ch][delayAt];
                        delay[ch][delayAt] = float(read(c, ch, at + i));
                        const double shifted = channels == 2 ? pitchOut[0][i] + (ch ? -1. : 1.) * pitchOut[1][i] : pitchOut[0][i];
                        if (c.outputs[ch]) c.outputs[ch][at + i] = float(clean(std::lerp(dry, shifted, wet)));
                    }
                    if (++delayAt == latency) delayAt = 0;
                }
                const double decay = std::pow(pole, count);
                for (unsigned p = 0; p < 2; ++p) current[p] = targets[p] + decay * (current[p] - targets[p]);
                clock = (clock + count) % 32;
                at += count;
            }
            return;
        }
        for (unsigned i = begin; i < end; ++i) {
            for (unsigned p = 0; p < 2; ++p) current[p] = targets[p] + pole * (current[p] - targets[p]);
            std::array<double, 2> x{read(c, 0, i), read(c, 1, i)}, y = x;
            if (kind == EffectKind::Tremolo) {
                const double wave = mode == 1 ? 1 - 4 * std::abs(phase - .5)
                    : mode == 2 ? std::tanh(3 * std::cos(2 * pi * phase)) / std::tanh(3.)
                    : std::cos(2 * pi * phase);
                const double gain = 1 - current[1] * .005 * (1 - wave);
                for (unsigned ch = 0; ch < channels; ++ch) y[ch] = x[ch] * gain;
                phase += current[0] / rate; phase -= std::floor(phase);
            } else if (kind == EffectKind::Stereo && channels == 2) {
                const double mid = .5 * (x[0] + x[1]), side = .5 * (x[0] - x[1]);
                midEnergy = flush(mid * mid + energyPole * (midEnergy - mid * mid));
                sideEnergy = flush(side * side + energyPole * (sideEnergy - side * side));
                // Limit added width when side energy overtakes the centre.
                // Never try to repair existing anti-phase audio by muting it.
                const double limit = std::clamp(std::sqrt((midEnergy + 1e-20) / (sideEnergy + 1e-20)), 1., 2.);
                const double follow = limit < safeWidth ? attackPole : releasePole;
                safeWidth = limit + follow * (safeWidth - limit);
                const double width = current[0] * .01;
                const double w = std::lerp(width, std::min(width, safeWidth), current[1] * .01);
                y = {mid + side * w, mid - side * w};
            } else if (kind == EffectKind::Air) {
                std::array<std::array<double, 2>, 2> bands{};
                for (unsigned ch = 0; ch < channels; ++ch)
                    for (unsigned band = 0; band < 2; ++band)
                        bands[band][ch] = filters[ch][band].tick(x[ch], airG[band], band ? 1.2952877 : 1.6452966, true);
                for (unsigned band = 0; band < 2; ++band) {
                    const double level = std::max(bands[band][0] * bands[band][0], bands[band][1] * bands[band][1]);
                    // A shared RMS detector avoids rectifier ripple and image
                    // movement when only one channel has a bright transient.
                    envelope[band] = flush(level + energyPole * (envelope[band] - level));
                    const double k = current[band] * .01;
                    const double dynamic = std::pow(1 + std::sqrt(2 * envelope[band]) / (band ? .06909 : .04026), band ? -2. : -.90134);
                    const double amount = (band ? 4.88043 : 2.19024) * std::pow(2 * k, band ? 1. : 1.1183) * dynamic;
                    const double body = (band ? .18966 : .132) * k / (.25 + k * k);
                    for (unsigned ch = 0; ch < channels; ++ch)
                        y[ch] += body * x[ch] + amount * bands[band][ch];
                }
            } else if (kind == EffectKind::Filter) {
                const unsigned order = 1u << mode, stages = std::max(1u, order / 2);
                for (unsigned band = 0; band < 2; ++band) {
                    const double on = band == 0 ? (values[0] > 20 ? 1 : 0) : (values[1] < 20000 ? 1 : 0);
                    enabled[band] = on + pole * (enabled[band] - on);
                    if (std::abs(enabled[band] - on) < 1e-8) enabled[band] = on;
                    for (unsigned ch = 0; ch < channels; ++ch) {
                        double filtered = y[ch];
                        for (unsigned stage = 0; stage < stages; ++stage) {
                            filtered = filters[ch][4 * band + stage].tick(filtered, current[band], damping[stage], band == 0, order == 1);
                        }
                        y[ch] = std::lerp(y[ch], filtered, enabled[band]);
                    }
                }
            } else if (kind == EffectKind::SoftClip) {
                for (unsigned ch = 0; ch < channels; ++ch) y[ch] = clip(x[ch], current[0]) * current[1];
            }
            for (unsigned ch = 0; ch < channels; ++ch)
                if (c.outputs[ch]) c.outputs[ch][i] = float(clean(y[ch]));
        }
    }
};

MiniEffectInstance::MiniEffectInstance(EffectKind kind) : m_dsp(std::make_unique<DSP>()), m_kind(kind) {
    static_assert(std::atomic<double>::is_always_lock_free);
    for (const auto& p : parameters()) m_values[p.index].store(p.defaultValue);
}
MiniEffectInstance::~MiniEffectInstance() = default;
const PluginDescriptor& MiniEffectInstance::descriptor() const noexcept {
    static const auto descriptors = [] {
        std::array<PluginDescriptor, 6> result;
        for (const auto& type : effectTypes()) {
            auto& d = result[unsigned(type.kind)];
            d.format = Format::Internal; d.uid = d.path = "daw.mini." + std::string(type.id);
            d.name = type.name; d.vendor = "VLTONE"; d.version = "1.0";
            d.category = "Effect"; d.stateSchemaVersion = 1;
        }
        return result;
    }();
    return descriptors[unsigned(m_kind)];
}
bool MiniEffectInstance::setBusLayout(const PluginBusLayout& wanted, PluginBusLayout& accepted) {
    if (wanted.inputs.size() != 1 || wanted.outputs.size() != 1 ||
        wanted.inputs[0] != wanted.outputs[0] || wanted.inputs[0] < 1 || wanted.inputs[0] > 2) return false;
    accepted = m_layout = wanted; return true;
}
bool MiniEffectInstance::activate(const PluginProcessInfo& info) {
    m_active = false;
    if (!std::isfinite(info.sampleRate) || info.sampleRate < 8000 || info.sampleRate > 384000 || !info.maxBlockSize) return false;
    auto& d = *m_dsp;
    d.rate = info.sampleRate; d.channels = m_layout.outputs[0];
    d.mode = unsigned(parameterValue(2));
    d.pole = std::exp(-1 / ((m_kind == EffectKind::Pitch && d.mode ? .025 : .010) * d.rate));
    d.energyPole = std::exp(-1 / (.050 * d.rate));
    d.attackPole = std::exp(-1 / (.005 * d.rate));
    d.releasePole = std::exp(-1 / (.150 * d.rate));
    // Independent parallel-band model fitted to local Fresh Air 1.0.9 sweeps.
    // This is an approximation, not the vendor's undisclosed algorithm.
    d.airG = {d.cutoff(3019.62), d.cutoff(8799.92)};
    if (m_kind == EffectKind::Filter)
        for (unsigned stage = 0; stage < 4; ++stage)
            d.damping[stage] = 2 * std::cos(pi * (2 * stage + 1) / (2 * (1u << d.mode)));
    if (m_kind == EffectKind::Pitch) {
        d.stretch = std::make_unique<signalsmith::stretch::SignalsmithStretch<float>>(0);
        const auto window = std::bit_ceil(std::max(256u, unsigned(std::ceil(d.rate * (d.mode ? .080 : .020)))));
        const int hop = int(window / 8);
        d.stretch->configure(int(d.channels), hop * 8, hop, true);
        d.stretch->setFormantBase(float(200 / d.rate));
        d.latency = unsigned(d.stretch->inputLatency() + d.stretch->outputLatency());
        for (auto& channel : d.delay) channel.assign(d.latency, 0.f);
    }
    m_active = true; m_processing = false; reset(); return true;
}
std::int32_t MiniEffectInstance::parameterIndexForId(std::string_view id) const noexcept {
    for (const auto& p : parameters()) if (p.id == id) return std::int32_t(p.index);
    return -1;
}
double MiniEffectInstance::parameterValue(std::uint32_t i) const noexcept {
    return i < parameters().size() ? m_values[i].load(std::memory_order_relaxed) : 0;
}
void MiniEffectInstance::setParameterFromHost(std::uint32_t i, double v) {
    if (i >= parameters().size()) return;
    const auto& p = parameters()[i];
    v = std::isfinite(v) ? std::clamp(v, p.minValue, p.maxValue) : p.defaultValue;
    m_values[i].store(p.isStepped ? std::round(v) : v, std::memory_order_relaxed);
}
std::string MiniEffectInstance::parameterText(std::uint32_t i, double value) const {
    if (i >= parameters().size()) return {};
    if (i == 2) {
        const unsigned v = unsigned(std::clamp(std::round(clean(value)), 0., parameters()[i].maxValue));
        if (m_kind == EffectKind::Pitch) return v ? "Pro" : "Live";
        if (m_kind == EffectKind::Tremolo) return std::array{"Sine", "Triangle", "Pulse"}[v];
        return std::array{"6 dB/oct", "12 dB/oct", "24 dB/oct", "48 dB/oct"}[v];
    }
    char text[64]; std::snprintf(text, sizeof(text), "%.2f %s", value, parameters()[i].unit.c_str()); return text;
}
bool MiniEffectInstance::saveState(std::vector<std::uint8_t>& out) const {
    nlohmann::json values = nlohmann::json::array();
    for (const auto& p : parameters()) values.push_back(parameterValue(p.index));
    const auto data = nlohmann::json{{"version", 1}, {"uid", descriptor().uid}, {"values", values}}.dump();
    out.assign(data.begin(), data.end()); return true;
}
bool MiniEffectInstance::loadState(std::span<const std::uint8_t> bytes) {
    if (bytes.empty() || bytes.size() > 65536) return false;
    const auto data = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
    if (!data.is_object() || data.value("version", nlohmann::json{}) != 1 ||
        data.value("uid", nlohmann::json{}) != descriptor().uid || !data.contains("values") ||
        !data["values"].is_array() || data["values"].size() != parameters().size()) return false;
    for (const auto& v : data["values"]) if (!v.is_number() || !std::isfinite(v.get<double>())) return false;
    for (const auto& p : parameters()) setParameterFromHost(p.index, data["values"][p.index].get<double>());
    return true;
}
PluginProcessDisposition MiniEffectInstance::process(const PluginProcessContext& c) noexcept {
    if (!m_active || !c.outputs || c.outputChannels != m_dsp->channels) return PluginProcessDisposition::Error;
    unsigned at = 0;
    const auto render = [&](unsigned end) {
        m_dsp->render(m_kind, c, at, end, {parameterValue(0), parameterValue(1)}); at = end;
    };
    for (const auto& e : c.inputEvents) {
        if (e.kind != PluginEvent::Kind::ParamValue || e.paramIndex >= 2 || e.frameOffset >= c.frames) continue;
        render(std::clamp(e.frameOffset, at, c.frames));
        setParameterFromHost(e.paramIndex, e.value);
    }
    render(c.frames);
    return PluginProcessDisposition::Continue;
}
void MiniEffectInstance::reset() noexcept {
    auto& d = *m_dsp;
    d.phase = 0; d.clock = d.delayAt = 0; d.filters = {};
    d.midEnergy = d.sideEnergy = 0; d.safeWidth = 1; d.envelope = {};
    d.previous1 = d.previous2 = d.tonePower = d.curvePower = d.toneCurve = d.richness = 0;
    for (unsigned p = 0; p < 2; ++p) d.current[p] = d.target(m_kind, parameterValue(p));
    d.enabled = {parameterValue(0) > 20 ? 1. : 0., parameterValue(1) < 20000 ? 1. : 0.};
    d.wet = std::abs(d.current[0]) + std::abs(d.current[1]) > 1e-5 ? 1 : 0;
    if (d.stretch) {
        d.stretch->reset();
        for (auto& channel : d.delay) std::fill(channel.begin(), channel.end(), 0.f);
    }
}
std::uint32_t MiniEffectInstance::latencySamples() const noexcept { return m_dsp->latency; }
std::uint32_t MiniEffectInstance::tailSamples() const noexcept {
    return m_kind == EffectKind::Pitch ? 2 * m_dsp->latency
        : m_kind == EffectKind::Filter || m_kind == EffectKind::Air ? unsigned(m_dsp->rate * 2) : 0;
}
} // namespace daw::plugins::mini
