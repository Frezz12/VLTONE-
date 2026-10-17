#include "Internal/ReverbInstance.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace daw::plugins::reverb {
namespace dsp = modulation::dsp;
namespace {
static_assert(std::atomic<double>::is_always_lock_free && std::atomic<float>::is_always_lock_free);
constexpr std::array<std::string_view, 6> names{"Soft Hall", "Room", "Chamber", "Plate", "Ambience", "Shimmer"};
// Geometry differs independently of RT60. Prime lengths avoid shared modes.
constexpr std::array<double, 16> lengths{1493,1601,1747,1867,1999,2131,2281,2423,2591,2741,2917,3089,3259,3469,3671,3877};
constexpr std::array<double, 6> scales{1., .42, .70, .82, .28, 1.10};
constexpr std::array<double, 6> diffusionScale{1., .72, .9, 1.12, .65, 1.05};
constexpr std::array<double, 6> dampingScale{1., .85, .92, 1.2, 1.05, .9};
constexpr std::array<double, 12> taps{.007,.011,.017,.023,.031,.041,.053,.067,.079,.097,.113,.137};
constexpr std::array<double, 12> pans{-.8,.65,-.25,.9,-.95,.15,.5,-.6,.8,-.3,.35,-.75};
constexpr unsigned idx(Param p) { return unsigned(p); }
double sample(const float* const* data, unsigned channels, unsigned channel, unsigned frame) noexcept {
    if (!data || !channels || !data[std::min(channel, channels - 1)]) return 0;
    const double v = data[std::min(channel, channels - 1)][frame];
    return std::isfinite(v) ? std::clamp(v, -16., 16.) : 0.;
}
double delayed(const dsp::Delay& line, double delay, double now) noexcept {
    // The shared delay interpolator requires >= 3 samples; retain a true zero pre-delay.
    return delay < 3 ? std::lerp(now, line.readLinear(3), delay / 3) : line.readLinear(delay);
}
void hadamard(std::array<double, 16>& v) noexcept {
    for (unsigned stride = 1; stride < 16; stride *= 2)
        for (unsigned base = 0; base < 16; base += 2 * stride)
            for (unsigned i = 0; i < stride; ++i) {
                const double a = v[base + i], b = v[base + i + stride];
                v[base + i] = a + b; v[base + i + stride] = a - b;
            }
    for (auto& x : v) x *= .25;
}
Values defaults() {
    Values out{};
    for (const auto& p : parameterTable()) out[p.index] = p.defaultValue;
    return out;
}
}
std::span<const ParameterInfo> parameterTable() noexcept {
    static const std::array<ParameterInfo, kParameterCount> table{{
        {0,"mode","Space","",0,5,0,true,true},
        {1,"size","Size","%",0,100,65},
        {2,"brightness","Brightness","%",0,100,45},
        {3,"preDelay","Pre-delay","ms",0,250,30},
        {4,"decay","Decay","s",.2,30,4.2},
        {5,"width","Width","%",0,150,120},
        {6,"mix","Mix","%",0,100,25},
        {7,"bloom","Bloom","%",0,100,35},
        {8,"motion","Motion","%",0,100,15},
        {9,"duck","Duck","%",0,100,0},
        {10,"freeze","Freeze","",0,1,0,true,true},
        {11,"diffusion","Diffusion","%",0,100,80},
        {12,"earlyLate","Early / Late","%",0,100,65},
        {13,"lowCut","Low Cut","Hz",20,2000,120},
        {14,"highCut","High Cut","Hz",2000,20000,10800},
        {15,"shimmerAmount","Shimmer Amount","%",0,100,35}
    }};
    return table;
}
std::string_view modeName(unsigned mode) noexcept { return names[std::min(mode, 5u)]; }
std::span<const Preset> factoryPresets() noexcept {
    static const std::array<Preset, 6> presets = [] {
        std::array<Preset, 6> p{};
        for (unsigned i = 0; i < p.size(); ++i) { p[i] = {names[i], defaults()}; p[i].values[0] = i; }
        p[1].values[1] = 35; p[1].values[3] = 8; p[1].values[4] = .75; p[1].values[7] = 10; p[1].values[12] = 45;
        p[2].values[1] = 48; p[2].values[3] = 18; p[2].values[4] = 1.8; p[2].values[7] = 20;
        p[3].values[2] = 58; p[3].values[3] = 12; p[3].values[4] = 2.4; p[3].values[11] = 95; p[3].values[12] = 85;
        p[4].values[1] = 28; p[4].values[3] = 0; p[4].values[4] = .4; p[4].values[7] = 0; p[4].values[12] = 25;
        p[5].values[1] = 78; p[5].values[4] = 8; p[5].values[7] = 60; p[5].values[8] = 25;
        return p;
    }();
    return presets;
}
std::array<Reflection, 12> reflections(const Values& p) noexcept {
    std::array<Reflection, 12> out{};
    const unsigned mode = unsigned(std::clamp(p[0], 0., 5.));
    const double scale = scales[mode] * (.4 + p[1] * .014);
    for (unsigned i = 0; i < out.size(); ++i) {
        // Plate's closely packed reflections and Room's asymmetric clusters.
        const double cluster = mode == 3 ? .65 : mode == 1 && i % 3 == 0 ? 1.13 : 1.;
        out[i] = {taps[i] * scale * cluster, std::exp(-.20 * i) * .24, pans[(i + mode) % pans.size()]};
    }
    return out;
}
double bloomSeconds(const Values& p) noexcept { return .45 * std::pow(p[idx(Param::Bloom)] / 100., 1.4); }
ReverbInstance::ReverbInstance() {
    m_a = m_b = defaults();
    for (const auto& p : parameterTable()) m_values[p.index].store(p.defaultValue);
}
const PluginDescriptor& ReverbInstance::staticDescriptor() noexcept {
    static const PluginDescriptor d = [] {
        PluginDescriptor p; p.format = Format::Internal; p.uid = p.path = "daw.reverb";
        p.name = "Reverb"; p.vendor = "VLTONE"; p.version = "1.0";
        p.category = "Effect|Reverb"; p.stateSchemaVersion = 1; return p;
    }();
    return d;
}
bool ReverbInstance::setBusLayout(const PluginBusLayout& wanted, PluginBusLayout& accepted) {
    if (wanted.inputs.size() > 2 || wanted.outputs.size() > 1) return false;
    const auto main = wanted.inputs.empty() ? std::uint16_t(2) : wanted.inputs[0];
    const auto side = wanted.inputs.size() < 2 ? std::uint16_t(2) : wanted.inputs[1];
    if ((main != 1 && main != 2) || side > 2 || (!wanted.outputs.empty() && wanted.outputs[0] != main)) return false;
    m_layout = {{main, side}, {main}}; accepted = m_layout; return true;
}
bool ReverbInstance::activate(const PluginProcessInfo& info) {
    if (!std::isfinite(info.sampleRate) || info.sampleRate < 8000 || info.sampleRate > 384000 || !info.maxBlockSize) return false;
    m_rate = info.sampleRate; m_sidechain = info.sidechainConnected;
    m_smoothStep = 1 - dsp::pole(.020, m_rate); m_fadeStep = 1 / (.100 * m_rate);
    m_duckAttack = dsp::pole(.010, m_rate); m_duckRelease = dsp::pole(.300, m_rate);
    m_meterStep = 1 - dsp::pole(.030, m_rate);
    for (auto& l : m_lines) l.prepare(m_rate, .25);
    for (auto& l : m_pre) l.prepare(m_rate, .26);
    for (auto& l : m_early) l.prepare(m_rate, .32);
    for (auto& l : m_bloom) l.prepare(m_rate, .46);
    for (auto& l : m_shimmer) l.prepare(m_rate, .07);
    for (auto& channel : m_diffusers) for (auto& l : channel) l.prepare(m_rate, .05);
    for (auto& tone : m_shimmerTone) tone.tune(120, std::min(6000., m_rate * .2), m_rate);
    m_active = true; m_processing = false; reset(); return true;
}
double ReverbInstance::clamp(unsigned i, double value) noexcept {
    const auto& p = parameterTable()[i];
    value = std::isfinite(value) ? std::clamp(value, p.minValue, p.maxValue) : p.defaultValue;
    return p.isStepped ? std::round(value) : value;
}
std::int32_t ReverbInstance::parameterIndexForId(std::string_view id) const noexcept {
    for (const auto& p : parameterTable()) if (p.id == id) return std::int32_t(p.index);
    return -1;
}
double ReverbInstance::parameterValue(std::uint32_t i) const noexcept {
    return i < kParameterCount ? m_values[i].load(std::memory_order_relaxed) : 0;
}
Values ReverbInstance::values() const noexcept {
    Values out{}; for (unsigned i = 0; i < out.size(); ++i) out[i] = parameterValue(i); return out;
}
void ReverbInstance::setParameterFromHost(std::uint32_t i, double v) {
    if (i < kParameterCount) m_values[i].store(clamp(i, v), std::memory_order_relaxed);
}
std::string ReverbInstance::parameterText(std::uint32_t i, double v) const {
    if (i >= kParameterCount) return {};
    v = clamp(i, v);
    if (i == idx(Param::Mode)) return std::string(modeName(unsigned(v)));
    if (i == idx(Param::Freeze)) return v > .5 ? "On" : "Off";
    char s[64];
    if (i == idx(Param::EarlyLate)) std::snprintf(s, sizeof(s), "%.0f / %.0f", 100 - v, v);
    else if (parameterTable()[i].unit == "Hz" && v >= 1000) std::snprintf(s, sizeof(s), "%.1f kHz", v / 1000);
    else std::snprintf(s, sizeof(s), i == idx(Param::Decay) ? "%.2f %s" : "%.0f %s", v, parameterTable()[i].unit.c_str());
    return s;
}
void ReverbInstance::captureComparison(char side) { (side == 'B' ? m_b : m_a) = values(); }
State ReverbInstance::state() const {
    const auto current = values();
    return {current, m_comparison == 'A' ? current : m_a, m_comparison == 'B' ? current : m_b,
            m_comparison, m_presetKind, m_presetName};
}
void ReverbInstance::setComparisonState(const State& state) {
    m_a = state.a; m_b = state.b; setActiveComparison(state.comparison);
    setPresetReference(state.presetKind, state.presetName);
}
Values ReverbInstance::comparison(char side) const { return side == 'B' ? m_b : m_a; }
void ReverbInstance::copyComparison() { captureComparison(m_comparison); (m_comparison == 'A' ? m_b : m_a) = values(); }
void ReverbInstance::setPresetReference(std::string kind, std::string name) { m_presetKind = std::move(kind); m_presetName = std::move(name); }
bool ReverbInstance::saveState(std::vector<std::uint8_t>& out) const {
    const auto params = [](const Values& v) {
        auto j = nlohmann::json::object();
        for (const auto& p : parameterTable()) j[p.id] = v[p.index];
        return j;
    };
    const auto current = values();
    const auto doc = nlohmann::json{{"version",1},{"params",params(current)},
        {"a",params(m_comparison == 'A' ? current : m_a)}, {"b",params(m_comparison == 'B' ? current : m_b)},
        {"comparison",std::string(1,m_comparison)}, {"presetKind",m_presetKind},{"presetName",m_presetName}}.dump();
    out.assign(doc.begin(), doc.end()); return true;
}
bool ReverbInstance::loadState(std::span<const std::uint8_t> state) {
    if (state.empty() || state.size() > 65536) return false;
    const auto doc = nlohmann::json::parse(state.begin(), state.end(), nullptr, false);
    if (!doc.is_object() || doc.value("version",nlohmann::json{}) != 1 || !doc.contains("params")) return false;
    const auto read = [](const nlohmann::json& j, Values& out) {
        if (!j.is_object()) return false;
        out = defaults();
        for (const auto& p : parameterTable()) if (const auto it = j.find(p.id); it != j.end()) {
            if (!it->is_number() || !std::isfinite(it->get<double>())) return false;
            out[p.index] = clamp(p.index, it->get<double>());
        }
        return true;
    };
    Values current{}, a{}, b{};
    if (!read(doc["params"], current)) return false;
    a = b = current;
    if ((doc.contains("a") && !read(doc["a"], a)) || (doc.contains("b") && !read(doc["b"], b))) return false;
    for (const char* key : {"comparison","presetKind","presetName"})
        if (doc.contains(key) && (!doc[key].is_string() || doc[key].get_ref<const std::string&>().size() > 256)) return false;
    const auto side = doc.value("comparison", std::string("A"));
    if (side != "A" && side != "B") return false;
    for (unsigned i = 0; i < current.size(); ++i) setParameterFromHost(i, current[i]);
    m_a = a; m_b = b; m_comparison = side[0];
    m_presetKind = doc.value("presetKind",std::string("custom"));
    m_presetName = doc.value("presetName",std::string("Custom"));
    return true;
}
void ReverbInstance::reset() noexcept {
    for (auto* bank : {&m_pre,&m_early,&m_bloom,&m_shimmer}) for (auto& l : *bank) l.reset();
    for (auto& l : m_lines) l.reset();
    for (auto& ch : m_diffusers) for (auto& l : ch) l.reset();
    for (auto* bank : {&m_outputTone,&m_shimmerTone}) for (auto& t : *bank) t.reset();
    m_smooth = values(); m_low.fill(0); m_clock = 0;
    for (unsigned i = 0; i < 16; ++i) m_phases[i] = double(i) * .38196601125;
    m_freeze = m_smooth[idx(Param::Freeze)]; m_freezeStructure = false; m_fade = 1;
    m_shimmerPhase = m_shimmerMix = m_duckEnvelope = 0;
    m_inputMeter = m_earlyMeter = m_tailMeter = m_outputMeter = 0;
    for (auto& v : m_telemetry) v.store(0,std::memory_order_relaxed);
    m_telemetry[4].store(1,std::memory_order_relaxed);
    tune(true);
}
void ReverbInstance::tune(bool initial) noexcept {
    const bool frozen = parameterValue(idx(Param::Freeze)) > .5;
    const unsigned mode = unsigned(parameterValue(idx(Param::Mode)));
    const double size = (.4 + m_smooth[1] * .014) * scales[mode];
    // Retarget only after a tap crossfade completes. The latest requested
    // geometry wins; no restart can jump a partially audible delay tap.
    if (initial || m_fade >= 1) {
        bool changed = initial || frozen != m_freezeStructure;
        std::array<double,16> wanted{};
        for (unsigned i = 0; i < 16; ++i) {
            wanted[i] = frozen ? std::round(initial ? lengths[i] * size * m_rate / 48000. : m_to[i])
                              : lengths[i] * size * m_rate / 48000.;
            changed |= std::abs(wanted[i] - m_to[i]) > .01;
        }
        changed |= !frozen && mode != m_profileTo;
        if (changed) {
            m_from = initial ? wanted : m_to; m_to = wanted;
            m_earlyFrom = initial ? reflections(m_smooth) : m_earlyTo;
            if (!frozen || initial) {
                auto p = m_smooth; p[0] = mode; m_earlyTo = reflections(p);
                m_profileFrom = initial ? mode : m_profileTo; m_profileTo = mode;
            }
            m_fade = initial ? 1 : 0; m_freezeStructure = frozen;
        }
    }
    // Hold the acoustic structure during Freeze; output controls remain live.
    if (!frozen || initial) {
        const double decay = m_smooth[idx(Param::Decay)];
        double feedbackPower = 0;
        for (unsigned i = 0; i < 16; ++i) {
            m_feedback[i] = std::exp(-6.90775527898 * std::lerp(m_from[i],m_to[i],m_fade) / (m_rate * decay));
            feedbackPower += m_feedback[i] * m_feedback[i];
        }
        m_inputGain = 1.8 * std::sqrt(std::max(.00001,1 - feedbackPower / 16));
        const double dampingProfile = std::lerp(dampingScale[m_profileFrom],dampingScale[m_profileTo],m_fade);
        const double diffusionProfile = std::lerp(diffusionScale[m_profileFrom],diffusionScale[m_profileTo],m_fade);
        const double hz = std::min(m_rate * .42, 1000 * std::pow(16.,m_smooth[2] / 100) * dampingProfile);
        m_damping += (initial ? 1 : .18) * (std::exp(-2*dsp::pi*hz/m_rate) - m_damping);
        m_diffusion = std::clamp((.2 + .5*m_smooth[11]/100)*diffusionProfile,.1,.75);
        m_bloomTime = bloomSeconds(m_smooth);
    }
    for (auto& t : m_outputTone) t.tune(m_smooth[13] <= 20 ? 0 : m_smooth[13],m_smooth[14],m_rate);
    const double seconds = m_smooth[3]*.001 + .8 + 1.7*m_smooth[4] + m_bloomTime;
    m_tailSamples.store(frozen ? std::numeric_limits<std::uint32_t>::max() : std::uint32_t(seconds*m_rate),std::memory_order_relaxed);
}
void ReverbInstance::render(const PluginProcessContext& c, unsigned offset, unsigned count) noexcept {
    const auto target = values();
    const bool mono = c.outputChannels == 1;
    for (unsigned f = offset; f < offset + count; ++f) {
        for (unsigned i = 1; i < kParameterCount; ++i) if (i != idx(Param::Freeze)) {
            m_smooth[i] += m_smoothStep*(target[i]-m_smooth[i]);
            if (std::abs(target[i]-m_smooth[i]) < 1.e-10) m_smooth[i] = target[i];
        }
        m_smooth[0] = target[0];
        if ((m_clock++ & 31u) == 0) tune();
        m_fade = std::min(1.,m_fade + m_fadeStep);
        m_freeze += std::clamp(target[10]-m_freeze,-m_fadeStep,m_fadeStep);
        const double gate = 1 - m_freeze;
        const double shimmerTarget = target[0] == 5 ? m_smooth[15]*.0018*gate : 0;
        m_shimmerMix += m_fadeStep*10*(shimmerTarget-m_shimmerMix);
        const double dry[2]{sample(c.inputs,c.inputChannels,0,f),sample(c.inputs,c.inputChannels,1,f)};
        double lateInput[2]{}, early[2]{}, shimmer[2]{};
        for (unsigned ch = 0; ch < 2; ++ch) {
            m_pre[ch].write(dry[ch]);
            const double pre = delayed(m_pre[ch],m_smooth[3]*m_rate*.001,dry[ch]);
            m_pre[ch].advance(); m_early[ch].write(pre);
            for (unsigned i = 0; i < taps.size(); ++i) {
                const auto& a = m_earlyFrom[i]; const auto& b = m_earlyTo[i];
                const double reflected = std::lerp(m_early[ch].readLinear(a.seconds*m_rate),m_early[ch].readLinear(b.seconds*m_rate),m_fade);
                const double pan = mono ? 0 : std::lerp(a.pan,b.pan,m_fade);
                early[ch] += reflected*std::lerp(a.gain,b.gain,m_fade)*(1 + (ch ? pan : -pan))*.7;
            }
            m_early[ch].advance();
            double diffused = pre;
            for (unsigned stage = 0; stage < 4; ++stage) {
                auto& line = m_diffusers[ch][stage];
                const double d = line.readLinear(m_rate*(.0047 + stage*.0061 + ch*.00073));
                const double y = d - m_diffusion*diffused;
                line.write(dsp::flush(diffused + m_diffusion*y)); line.advance(); diffused = y;
            }
            m_bloom[ch].write(diffused);
            double spread = 0;
            for (double tap : {.08,.29,.57,1.}) spread += delayed(m_bloom[ch],m_bloomTime*m_rate*tap,diffused)*.25;
            m_bloom[ch].advance();
            lateInput[ch] = std::lerp(diffused,spread,m_smooth[7]/100)*gate;
            const double second = m_shimmerPhase >= .5 ? m_shimmerPhase-.5 : m_shimmerPhase+.5;
            const double window = .5-.5*std::cos(2*dsp::pi*m_shimmerPhase);
            shimmer[ch] = std::lerp(m_shimmer[ch].readLinear((.003+.06*(1-second))*m_rate),
                                    m_shimmer[ch].readLinear((.003+.06*(1-m_shimmerPhase))*m_rate),window);
        }
        std::array<double,16> network{};
        double late[2]{};
        for (unsigned i = 0; i < 16; ++i) {
            const double movement = m_rate*.00028*(m_smooth[8]/100)*gate*std::sin(2*dsp::pi*m_phases[i]);
            m_phases[i] += (.11 + i*.013)*gate/m_rate;
            if (m_phases[i] >= 1) m_phases[i] -= std::floor(m_phases[i]);
            const double tap = std::lerp(m_lines[i].readLinear(m_from[i]+movement),m_lines[i].readLinear(m_to[i]+movement),m_fade);
            const double damping = m_damping*gate;
            m_low[i] = dsp::flush((1-damping)*tap + damping*m_low[i]);
            network[i] = m_low[i];
            late[0] += network[i]*((i & 2) ? -.25 : .25);
            late[1] += network[i]*((i & 4) ? -.25 : .25);
        }
        hadamard(network);
        for (unsigned i = 0; i < 16; ++i) {
            const double l = (i & 1) ? -.25 : .25, r = (i & 8) ? -.25 : .25;
            const double feedback = std::lerp(m_feedback[i],1.,m_freeze);
            const double returned = std::lerp(network[i],l*shimmer[0]+r*shimmer[1],m_shimmerMix);
            const double v = returned*feedback + m_inputGain*(l*lateInput[0]+r*lateInput[1]);
            m_lines[i].write(std::isfinite(v) ? dsp::flush(std::clamp(v,-64.,64.)) : 0);
            m_lines[i].advance();
        }
        for (unsigned ch = 0; ch < 2; ++ch) {
            m_shimmer[ch].write(m_shimmerTone[ch].process(late[ch])); m_shimmer[ch].advance();
        }
        m_shimmerPhase += 1/(.06*m_rate); if (m_shimmerPhase >= 1) m_shimmerPhase -= 1;
        const double detector = m_sidechain ? std::max(std::abs(sample(c.sidechainInputs,c.sidechainInputChannels,0,f)),std::abs(sample(c.sidechainInputs,c.sidechainInputChannels,1,f)))
                                           : std::max(std::abs(dry[0]),std::abs(dry[1]));
        const double pole = detector > m_duckEnvelope ? m_duckAttack : m_duckRelease;
        m_duckEnvelope = pole*m_duckEnvelope + (1-pole)*detector;
        const double duck = 1/(1 + m_smooth[9]*.24*m_duckEnvelope);
        double wet[2]{};
        for (unsigned ch = 0; ch < 2; ++ch)
            wet[ch] = m_outputTone[ch].process(std::lerp(early[ch]*gate,late[ch],m_smooth[12]/100));
        const double mid = .5*(wet[0]+wet[1]), side = mono ? 0 : .5*(wet[0]-wet[1])*m_smooth[5]/100;
        wet[0] = (mid+side)*duck; wet[1] = (mid-side)*duck;
        double outputPeak = 0;
        for (unsigned ch = 0; ch < c.outputChannels; ++ch) {
            const double out = std::lerp(dry[std::min(ch,1u)],wet[std::min(ch,1u)],m_smooth[6]/100);
            if (c.outputs && c.outputs[ch]) c.outputs[ch][f] = float(out);
            outputPeak = std::max(outputPeak,std::abs(out));
        }
        const auto meter = [this](double& state, double x) { state += m_meterStep*(x*x-state); };
        meter(m_inputMeter,std::max(std::abs(dry[0]),std::abs(dry[1])));
        meter(m_earlyMeter,std::max(std::abs(early[0]),std::abs(early[1]))*gate);
        meter(m_tailMeter,std::max(std::abs(late[0]),std::abs(late[1])));
        meter(m_outputMeter,outputPeak);
        m_telemetry[4].store(float(duck),std::memory_order_relaxed);
    }
}
PluginProcessDisposition ReverbInstance::process(const PluginProcessContext& c) noexcept {
    if (!m_active || !c.frames) return PluginProcessDisposition::Continue;
    unsigned at = 0;
    for (const auto& event : c.inputEvents) {
        const unsigned end = std::clamp(event.frameOffset,at,c.frames);
        render(c,at,end-at); at = end;
        if (event.kind == PluginEvent::Kind::ParamValue && event.paramIndex < kParameterCount)
            setParameterFromHost(event.paramIndex,event.value);
    }
    render(c,at,c.frames-at);
    const double levels[]{m_inputMeter,m_earlyMeter,m_tailMeter,m_outputMeter};
    for (unsigned i = 0; i < 4; ++i) m_telemetry[i].store(float(std::sqrt(std::max(0.,levels[i]))),std::memory_order_relaxed);
    return PluginProcessDisposition::Continue;
}
std::uint32_t ReverbInstance::tailSamples() const noexcept {
    return parameterValue(idx(Param::Freeze)) > .5 ? std::numeric_limits<std::uint32_t>::max() : m_tailSamples.load(std::memory_order_relaxed);
}
Telemetry ReverbInstance::telemetry() const noexcept {
    return {m_telemetry[0].load(std::memory_order_relaxed),m_telemetry[1].load(std::memory_order_relaxed),
        m_telemetry[2].load(std::memory_order_relaxed),m_telemetry[3].load(std::memory_order_relaxed),m_telemetry[4].load(std::memory_order_relaxed)};
}
} // namespace daw::plugins::reverb
