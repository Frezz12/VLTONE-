#include "Internal/Vlt1073Instance.hpp"
#include <nlohmann/json.hpp>
#include <cstdio>

namespace daw::plugins::vlt1073 {
namespace {
static_assert(std::atomic<double>::is_always_lock_free);
double amplitude(double db) noexcept { return std::pow(10., db / 20); }
constexpr unsigned at(Param p) noexcept { return unsigned(p); }
}
std::span<const ParameterInfo> parameterTable() noexcept {
    static const std::array<ParameterInfo, kParameterCount> table{{
        {0,"inputMode","Input mode","",0,1,0,true,true},
        {1,"inputEnabled","Input enabled","",0,1,1,true,true},
        {2,"micGain","Mic gain","dB",20,80,20,true,true},
        {3,"lineGain","Line gain","dB",-10,20,0,true,true},
        {4,"micZ","Mic impedance","",0,1,1,true,true},
        {5,"pad","Mic pad","",0,1,0,true,true},
        {6,"highGain","High shelf gain","dB",-16,16,0},
        {7,"midFrequency","Presence frequency","",0,6,1,true,true},
        {8,"midGain","Presence gain","dB",-18,18,0},
        {9,"lowFrequency","Low shelf frequency","",0,4,1,true,true},
        {10,"lowGain","Low shelf gain","dB",-16,16,0},
        {11,"highPass","High pass","",0,4,0,true,true},
        {12,"eqEnabled","Equalizer","",0,1,1,true,true},
        {13,"phase","Polarity","",0,1,0,true,true},
        {14,"level","Level","dB",-90,10,0},
        {15,"output","Output","dB",-24,12,0}
    }};
    return table;
}
Vlt1073Instance::Vlt1073Instance() : m_filter(engine::dsp::halfBand65Taps()) {
    for (const auto& p : parameterTable()) m_values[p.index].store(p.defaultValue);
}
const PluginDescriptor& Vlt1073Instance::staticDescriptor() noexcept {
    static const auto d = [] { PluginDescriptor p; p.format = Format::Internal;
        p.uid = p.path = "daw.1073"; p.name = "VLT 1073"; p.vendor = "VLTONE"; p.version = "1.0";
        p.category = "Effect|EQ|Preamp"; p.stateSchemaVersion = 1; return p; }();
    return d;
}
bool Vlt1073Instance::setBusLayout(const PluginBusLayout& wanted, PluginBusLayout& accepted) {
    if (wanted.inputs.size() > 1 || wanted.outputs.size() > 1) return false;
    const auto n = wanted.inputs.empty() ? std::uint16_t(2) : wanted.inputs[0];
    if ((n != 1 && n != 2) || (!wanted.outputs.empty() && wanted.outputs[0] != n)) return false;
    m_layout = {{n},{n}}; accepted = m_layout; return true;
}
bool Vlt1073Instance::activate(const PluginProcessInfo& info) {
    if (!std::isfinite(info.sampleRate) || info.sampleRate < 8000 || info.sampleRate > 384000 || !info.maxBlockSize) return false;
    m_rate = info.sampleRate; m_smoothing = -std::expm1(-32 / (.012 * m_rate));
    m_audioSmoothing = -std::expm1(-1 / (.001 * m_rate));
    const auto g = [&](double hz) { return std::tan(std::numbers::pi * std::min(hz,m_rate * 1.5) / (4 * m_rate)); };
    m_couplingG = g(2); m_inputFluxG = g(85); m_outputFluxG = g(110); m_bandwidthG = g(38000);
    m_active = true; m_processing = false; reset(); return true;
}
std::int32_t Vlt1073Instance::parameterIndexForId(std::string_view id) const noexcept {
    for (const auto& p : parameterTable()) if (p.id == id) return std::int32_t(p.index);
    return -1;
}
double Vlt1073Instance::parameterValue(std::uint32_t i) const noexcept { return i < kParameterCount ? m_values[i].load(std::memory_order_relaxed) : 0; }
double Vlt1073Instance::clamp(unsigned i, double v) noexcept {
    const auto& p = parameterTable()[i];
    v = std::isfinite(v) ? std::clamp(v, p.minValue, p.maxValue) : p.defaultValue;
    if (i == at(Param::MicGain) || i == at(Param::LineGain)) return std::round(v / 5) * 5;
    return p.isStepped ? std::round(v) : v;
}
void Vlt1073Instance::setParameterFromHost(std::uint32_t i, double v) { if (i < kParameterCount) m_values[i].store(clamp(i,v), std::memory_order_relaxed); }
std::string Vlt1073Instance::parameterText(std::uint32_t i, double v) const {
    if (i >= kParameterCount) return {};
    v = clamp(i, v);
    switch (Param(i)) {
    case Param::InputMode: return v ? "MIC" : "LINE";
    case Param::MicZ: return v ? "HI" : "LO (300 ohm)";
    case Param::Pad: return v ? "-20 dB" : "0 dB";
    case Param::Phase: return v ? "Inverted" : "Normal";
    case Param::InputEnabled: case Param::EqEnabled: return v ? "ON" : "OFF";
    case Param::MidFrequency: v = kMidHz[unsigned(v)]; break;
    case Param::LowFrequency: v = kLowHz[unsigned(v)]; break;
    case Param::HighPass: v = kHighPassHz[unsigned(v)]; break;
    case Param::Level: if (v <= -90) return "-inf dB"; break;
    default: break;
    }
    char text[48];
    if (i == 7 || i == 9 || i == 11) {
        if (!v) return "OFF";
        std::snprintf(text, sizeof(text), "%.0f Hz", v);
    } else std::snprintf(text, sizeof(text), "%+.1f dB", v);
    return text;
}
bool Vlt1073Instance::saveState(std::vector<std::uint8_t>& out) const {
    nlohmann::json values = nlohmann::json::object();
    for (const auto& p : parameterTable()) values[p.id] = parameterValue(p.index);
    const auto json = nlohmann::json{{"version",1},{"params",values}}.dump(); out.assign(json.begin(),json.end()); return true;
}
bool Vlt1073Instance::loadState(std::span<const std::uint8_t> bytes) {
    if (bytes.empty() || bytes.size() > 65536) return false;
    const auto j = nlohmann::json::parse(bytes.begin(),bytes.end(),nullptr,false);
    if (!j.is_object() || !j.contains("version") || j["version"] != 1 || !j.contains("params") || !j["params"].is_object()) return false;
    std::array<double,kParameterCount> values{};
    for (const auto& p : parameterTable()) {
        values[p.index] = p.defaultValue;
        if (auto v = j["params"].find(p.id); v != j["params"].end()) {
            if (!v->is_number() || !std::isfinite(v->get<double>())) return false;
            values[p.index] = clamp(p.index,v->get<double>());
        }
    }
    for (unsigned i = 0; i < kParameterCount; ++i) setParameterFromHost(i,values[i]);
    return true;
}
void Vlt1073Instance::updateTargets() noexcept {
    for (unsigned i = 0; i < kParameterCount; ++i) m_target[i] = parameterValue(i);
    // Smooth frequencies in log space, switches as continuous conductances.
    m_target[7] = std::log(kMidHz[std::max(1u,unsigned(m_target[7]))]);
    m_target[9] = std::log(kLowHz[std::max(1u,unsigned(m_target[9]))]);
    m_target[11] = std::log(kHighPassHz[std::max(1u,unsigned(m_target[11]))]);
    if (parameterValue(7) == 0) m_target[8] = 0;
    if (parameterValue(9) == 0) m_target[10] = 0;
    m_target[14] = parameterValue(14) <= -90 ? 0 : amplitude(m_target[14]);
    m_target[15] = amplitude(m_target[15]);
}
void Vlt1073Instance::updateCircuit() noexcept {
    for (unsigned i = 0; i < kParameterCount; ++i) {
        m_smooth[i] += m_smoothing * (m_target[i] - m_smooth[i]);
        if (std::abs(m_smooth[i] - m_target[i]) < 1e-12) m_smooth[i] = m_target[i];
    }
    m_micMix = m_smooth[0]; m_inputOn = m_smooth[1];
    m_gain = amplitude(std::lerp(m_smooth[3],m_smooth[2],m_micMix));
    const double stageTarget = parameterValue(2) >= 55 && parameterValue(0) > .5 ? 1 : 0;
    m_highStage += m_smoothing * (stageTarget - m_highStage);
    m_firstGain = std::pow(m_gain, 1 - .42 * m_highStage); m_secondGain = m_gain / m_firstGain;
    // Fixed 150-ohm virtual microphone source, loaded by the 10468 primary.
    const double impedance = std::lerp(300., std::lerp(1200.,600.,m_highStage),m_smooth[4]);
    const double loading = (impedance / (impedance + 150)) / (1200. / 1350);
    m_inputScale = amplitude(-20 * m_smooth[5] * m_micMix) * std::lerp(1.,loading,m_micMix);
    m_level = m_smooth[14]; m_output = m_smooth[15]; m_phase = 1 - 2 * m_smooth[13]; m_eqMix = m_smooth[12];
    m_hpMix += m_smoothing * ((parameterValue(11) > 0 ? 1. : 0.) - m_hpMix);
    const auto g = [&](double f) { return std::tan(std::numbers::pi * std::min(f, m_rate * 1.5) / (4 * m_rate)); };
    const std::array<double,3> db{m_smooth[10],m_smooth[8],m_smooth[6]};
    const std::array<double,3> hz{std::exp(m_smooth[9]),std::exp(m_smooth[7]),12000};
    for (unsigned i = 0; i < 3; ++i) {
        const double a = amplitude(std::abs(db[i]));
        m_eq.boost[i] = db[i] > 0 ? a - 1 : 0; m_eq.cut[i] = db[i] < 0 ? a - 1 : 0;
        m_eq.g[i] = g(hz[i] * (i == 0 ? 1 / std::sqrt(a) : i == 2 ? std::sqrt(a) : 1));
    }
    m_hpG = g(std::exp(m_smooth[11]));
    // EQ and 5k fader remain connected loads when their signal is bypassed.
    m_load = 1 + .035 * (std::abs(m_smooth[6]) / 16 + std::abs(m_smooth[8]) / 18 + std::abs(m_smooth[10]) / 16) + .015 * (1 - std::min(1.,m_level));
    m_audioTarget = {m_firstGain,m_secondGain,m_micMix,m_highStage,m_inputScale,m_level,m_output,m_inputOn,m_eqMix,m_phase,m_hpMix,m_load,m_hpG};
}
void Vlt1073Instance::reset() noexcept {
    m_channels = {}; m_controlPhase = 0; m_fresh = true; updateTargets(); m_smooth = m_target;
    m_highStage = parameterValue(2) >= 55 && parameterValue(0) > .5 ? 1 : 0;
    m_hpMix = parameterValue(11) > 0 ? 1 : 0; updateCircuit();
    m_audio = m_audioTarget; m_audioEq = m_eq;
}
double Vlt1073Instance::tick4x(double x, Channel& c) noexcept {
    double v = x * kVoltsPerFullScale * m_audio[7] * m_audio[4];
    v = c.inputTransformer.tick(v,m_inputFluxG,std::lerp(.10,.18,m_audio[2]),.035);
    v -= c.inputCoupling.tick(v,m_couplingG);
    v = circuit::classA(v * m_audio[0], 31, .12);
    const double second = circuit::classA(v * m_audio[1],27,.18);
    v = std::lerp(v,second,m_audio[3]);
    const double eq = c.eq.tick(v,m_audioEq);
    const double filtered = c.highPass.tick(eq,m_audio[12]);
    v = std::lerp(v,std::lerp(eq,filtered,m_audio[10]),m_audio[8]);
    v = circuit::classA(v,40 / m_audio[11],.09);
    v *= m_audio[5];
    v = circuit::classA(v,25,.35);
    v = c.outputTransformer.tick(v,m_outputFluxG,.23,.05);
    v -= c.outputCoupling.tick(v,m_couplingG);
    v = c.bandwidth.tick(v,m_bandwidthG);
    return v / kVoltsPerFullScale;
}
void Vlt1073Instance::render(const PluginProcessContext& context, unsigned offset, unsigned frames) noexcept {
    if (!frames) return;
    // Hosts can restore parameters as frame-zero events after activate().
    // Establish the initial operating point only once those events have been
    // applied, so playback, freeze and export start from identical memories.
    if (m_fresh) { reset(); m_fresh = false; }
    updateTargets();
    const unsigned channels = std::min(unsigned(m_layout.outputs[0]),2u);
    for (unsigned f = offset; f < offset + frames; ++f) {
        if (m_controlPhase == 0) updateCircuit();
        m_controlPhase = (m_controlPhase + 1) & 31u;
        // Interpolate the control-rate solution at every sample, including
        // switches; no 32-sample staircase is applied to audio amplitudes.
        for (unsigned i = 0; i < m_audio.size(); ++i) {
            m_audio[i] += m_audioSmoothing * (m_audioTarget[i] - m_audio[i]);
            if (std::abs(m_audio[i] - m_audioTarget[i]) < 1e-12) m_audio[i] = m_audioTarget[i];
        }
        for (unsigned i = 0; i < 3; ++i) {
            m_audioEq.boost[i] += m_audioSmoothing * (m_eq.boost[i] - m_audioEq.boost[i]);
            m_audioEq.cut[i] += m_audioSmoothing * (m_eq.cut[i] - m_audioEq.cut[i]);
            m_audioEq.g[i] += m_audioSmoothing * (m_eq.g[i] - m_audioEq.g[i]);
        }
        std::array<double,2> input{},output{};
        for (unsigned ch = 0; ch < channels; ++ch) {
            const double x = context.inputs && ch < context.inputChannels && context.inputs[ch] ? context.inputs[ch][f] : 0;
            input[ch] = std::isfinite(x) ? std::clamp(x,-64.,64.) : 0;
        }
        for (unsigned p2 = 0; p2 < 2; ++p2) for (unsigned ch = 0; ch < channels; ++ch) {
            auto& c = m_channels[ch];
            const double up2 = c.up2.upsample(input[ch],m_filter,p2);
            double down4 = 0;
            for (unsigned p4 = 0; p4 < 2; ++p4) {
                const double up4 = c.up4.upsample(up2,m_filter,p4);
                const double wet = tick4x(up4,c);
                if (p4 == 0) down4 = c.down4.tick(wet,m_filter);
                else c.down4.push(wet);
            }
            if (p2 == 0) output[ch] = c.down2.tick(down4,m_filter);
            else c.down2.push(down4);
        }
        for (unsigned ch = 0; ch < context.outputChannels; ++ch) if (context.outputs && context.outputs[ch])
            context.outputs[ch][f] = ch < channels && std::isfinite(output[ch]) ? float(output[ch] * m_audio[6] * m_audio[9]) : 0;
    }
}
PluginProcessDisposition Vlt1073Instance::process(const PluginProcessContext& c) noexcept {
    if (!m_active || !m_processing) {
        for (unsigned ch = 0; ch < c.outputChannels; ++ch) if (c.outputs && c.outputs[ch]) std::fill_n(c.outputs[ch],c.frames,0.f);
        return PluginProcessDisposition::Continue;
    }
    unsigned offset = 0;
    for (const auto& event : c.inputEvents) if (event.kind == PluginEvent::Kind::ParamValue && event.paramIndex < kParameterCount) {
        const unsigned end = std::clamp(event.frameOffset,offset,c.frames);
        if (end > offset) render(c,offset,end-offset);
        setParameterFromHost(event.paramIndex,event.value); offset = end;
    }
    if (offset < c.frames) render(c,offset,c.frames-offset);
    return PluginProcessDisposition::Continue;
}
} // namespace daw::plugins::vlt1073
