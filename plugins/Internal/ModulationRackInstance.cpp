#include "Internal/ModulationRackInstance.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <nlohmann/json.hpp>

namespace daw::plugins::modulation {
namespace eq = equalizer;
const PluginDescriptor& ModulationRackInstance::staticDescriptor() {
    static const auto descriptor = [] {
        PluginDescriptor d;
        d.format = Format::Internal;
        d.uid = d.path = "daw.modulation";
        d.name = "Modulation";
        d.vendor = "VLTONE";
        d.version = "1.0";
        d.stateSchemaVersion = 1;
        d.category = "Effect|Modulation";
        d.mainInputChannels = d.mainOutputChannels = 2;
        return d;
    }();
    return descriptor;
}
std::span<const ParameterInfo> ModulationRackInstance::parameterTable() {
    static const auto table = [] {
        std::array<ParameterInfo, parameterCount> t;
        constexpr const char* ids[]{"chorus", "doubler", "flanger", "phaser"};
        for (unsigned m = 0; m < 4; ++m) {
            const auto prefix = std::string(ids[m]) + ".";
            const auto name = descriptorFor(kinds[m]).name + " ";
            t[offsets[m]] = {offsets[m], prefix + "enabled", name + "Enabled", "", 0, 1, 1, true, true, false};
            for (auto p : modulation::parameterTable(kinds[m])) {
                p.index += offsets[m] + 1;
                p.id = prefix + p.id;
                p.name = name + p.name;
                t[p.index] = std::move(p);
            }
        }
        t[orderParameter] = {orderParameter, "order", "Module order", "", 0, 23, 0, true, true, false};
        t[eqEnabledParameter] = {eqEnabledParameter, "eq.enabled", "EQ Enabled", "", 0, 1, 1, true, true, false};
        constexpr double frequencies[]{30, 180, 700, 2500, 8000, 18000};
        for (unsigned band = 0; band < bandCount; ++band)
            for (unsigned field = 0; field < 4; ++field) {
                auto p = eq::parameterTable()[eqParameter(band, field)];
                p.index = eqOffset + band * 4 + field;
                p.id = "eq." + p.id;
                if (field == 0) p.defaultValue = band > 0 && band < 5 ? 1 : 0;
                if (field == 1) { p.minValue = 20; p.maxValue = 20000; p.defaultValue = frequencies[band]; }
                if (field == 2) { p.minValue = -18; p.maxValue = 18; p.defaultValue = 0; }
                t[p.index] = std::move(p);
            }
        return t;
    }();
    return table;
}
unsigned ModulationRackInstance::eqParameter(unsigned band, unsigned field) noexcept {
    constexpr eq::BandParam fields[]{eq::BandParam::Enabled, eq::BandParam::Frequency,
                                    eq::BandParam::Gain, eq::BandParam::Q};
    return eq::bandParameter(band, fields[field]);
}
ModulationRackInstance::Order ModulationRackInstance::decodeOrder(unsigned value) noexcept {
    Order order{0, 1, 2, 3};
    for (unsigned i = 0; i < std::min(value, 23u); ++i)
        std::next_permutation(order.begin(), order.end());
    return order;
}
unsigned ModulationRackInstance::encodeOrder(Order wanted) noexcept {
    Order order{0, 1, 2, 3};
    unsigned index = 0;
    do { if (order == wanted) return index; ++index; }
    while (std::next_permutation(order.begin(), order.end()));
    return 0;
}
ModulationRackInstance::ModulationRackInstance()
    : m_modules{std::make_unique<ChorusInstance>(), std::make_unique<DoublerInstance>(),
                std::make_unique<FlangerInstance>(), std::make_unique<PhaserInstance>()} {
    (void)staticDescriptor();
    for (const auto& p : parameters()) setParameterFromHost(p.index, p.defaultValue);
    m_eq.setParameterFromHost(eq::bandParameter(0, eq::BandParam::Type), double(eq::FilterType::LowCut));
    m_eq.setParameterFromHost(eq::bandParameter(5, eq::BandParam::Type), double(eq::FilterType::HighCut));
    syncParameters();
}
bool ModulationRackInstance::setBusLayout(const PluginBusLayout& wanted, PluginBusLayout& accepted) {
    if (m_active || wanted.inputs.size() != 1 || wanted.outputs != wanted.inputs ||
        (wanted.inputs[0] != 1 && wanted.inputs[0] != 2)) return false;
    for (auto& module : m_modules) if (!module->setBusLayout(wanted, accepted)) return false;
    if (!m_eq.setBusLayout(wanted, accepted)) return false;
    accepted = m_layout = wanted;
    return true;
}
bool ModulationRackInstance::activate(const PluginProcessInfo& info) {
    if (!std::isfinite(info.sampleRate) || info.sampleRate < 8000 || info.sampleRate > 384000) return false;
    m_rate = info.sampleRate;
    syncParameters();
    for (auto& module : m_modules) if (!module->activate(info)) { deactivate(); return false; }
    if (!m_eq.activate(info)) { deactivate(); return false; }
    m_active = true;
    reset();
    return true;
}
void ModulationRackInstance::deactivate() {
    for (auto& module : m_modules) module->deactivate();
    m_eq.deactivate();
    m_active = m_processing = false;
}
std::int32_t ModulationRackInstance::parameterIndexForId(std::string_view id) const noexcept {
    for (const auto& p : parameters()) if (p.id == id) return std::int32_t(p.index);
    return -1;
}
double ModulationRackInstance::parameterValue(std::uint32_t i) const noexcept {
    return i < parameterCount ? m_values[i].load(std::memory_order_relaxed) : 0;
}
void ModulationRackInstance::setParameterFromHost(std::uint32_t i, double v) {
    if (i >= parameterCount) return;
    const auto& p = parameters()[i];
    v = std::isfinite(v) ? std::clamp(v, p.minValue, p.maxValue) : p.defaultValue;
    if (p.isStepped) v = std::round(v);
    m_values[i].store(v, std::memory_order_relaxed);
    // EQ response drawing reads these atomics on the UI thread as well.
    if (i >= eqOffset) m_eq.setParameterFromHost(eqParameter((i - eqOffset) / 4, (i - eqOffset) % 4), v);
}
std::string ModulationRackInstance::parameterText(std::uint32_t i, double v) const {
    if (i >= parameterCount) return {};
    if (i >= eqOffset) return m_eq.parameterText(eqParameter((i - eqOffset) / 4, (i - eqOffset) % 4), v);
    if (i == orderParameter) {
        std::string text;
        for (auto m : decodeOrder(unsigned(std::clamp(dsp::clean(v), 0., 23.)))) {
            if (!text.empty()) text += " > ";
            text += descriptorFor(kinds[m]).name;
        }
        return text;
    }
    if (i == eqEnabledParameter) return v >= .5 ? "On" : "Off";
    for (unsigned m = 0; m < 4; ++m) {
        if (i == offsets[m]) return v >= .5 ? "On" : "Off";
        if (i > offsets[m] && i <= offsets[m] + m_modules[m]->parameters().size())
            return m_modules[m]->parameterText(i - offsets[m] - 1, v);
    }
    return {};
}
void ModulationRackInstance::syncParameters() noexcept {
    for (unsigned m = 0; m < 4; ++m)
        for (const auto& p : m_modules[m]->parameters())
            m_modules[m]->setParameterFromHost(p.index, parameterValue(offsets[m] + 1 + p.index));
}
bool ModulationRackInstance::saveState(std::vector<std::uint8_t>& out) const {
    nlohmann::json j{{"version", 1}, {"uid", descriptor().uid}};
    for (const auto& p : parameters()) j["params"][p.id] = parameterValue(p.index);
    const auto text = j.dump();
    out.assign(text.begin(), text.end());
    return true;
}
bool ModulationRackInstance::loadState(std::span<const std::uint8_t> data) {
    const auto j = nlohmann::json::parse(data.begin(), data.end(), nullptr, false);
    if (!j.is_object() || !j.contains("version") || j["version"] != 1 ||
        !j.contains("uid") || j["uid"] != descriptor().uid ||
        !j.contains("params") || !j["params"].is_object()) return false;
    std::array<double, parameterCount> values{};
    for (const auto& p : parameters()) {
        const auto it = j["params"].find(p.id);
        if (it != j["params"].end() && !it->is_number()) return false;
        values[p.index] = it == j["params"].end() ? p.defaultValue : it->get<double>();
    }
    for (const auto& p : parameters()) setParameterFromHost(p.index, values[p.index]);
    return true;
}
void ModulationRackInstance::reset() noexcept {
    syncParameters();
    for (unsigned m = 0; m < 4; ++m) {
        m_modules[m]->reset();
        m_enabled[m] = parameterValue(offsets[m]);
    }
    m_eq.reset();
    m_enabled[4] = parameterValue(eqEnabledParameter);
    m_audioOrder = unsigned(parameterValue(orderParameter));
    m_orderGain = 1;
}
std::uint32_t ModulationRackInstance::tailSamples() const noexcept {
    std::uint32_t tail = std::uint32_t(m_rate); // conservative bound for the low-cut filter
    for (const auto& module : m_modules) tail += module->tailSamples();
    return tail;
}
void ModulationRackInstance::render(const PluginProcessContext& ctx, unsigned begin, unsigned end) noexcept {
    syncParameters();
    for (unsigned at = begin; at < end;) {
        const unsigned n = std::min(quantum, end - at);
        for (unsigned ch = 0; ch < 2; ++ch)
            for (unsigned f = 0; f < n; ++f) {
                const auto src = ctx.inputChannels ? std::min<unsigned>(ch, ctx.inputChannels - 1) : 0;
                m_a[ch][f] = m_dry[ch][f] = ctx.inputs && ctx.inputChannels && ctx.inputs[src]
                    ? float(dsp::clean(ctx.inputs[src][at + f])) : 0;
            }
        if (m_active) {
            const auto wantedOrder = unsigned(parameterValue(orderParameter));
            if (m_orderGain == 0) m_audioOrder = wantedOrder;
            const auto order = decodeOrder(m_audioOrder);
            for (unsigned stage = 0; stage < 5; ++stage) {
                const auto m = stage < 4 ? order[stage] : 4;
                const double target = parameterValue(m < 4 ? offsets[m] : eqEnabledParameter);
                const float* inputs[]{m_a[0].data(), m_a[1].data()};
                float* outputs[]{m_b[0].data(), m_b[1].data()};
                auto child = ctx;
                child.inputs = inputs; child.outputs = outputs;
                child.inputChannels = child.outputChannels = m_layout.inputs[0];
                child.frames = n; child.inputEvents = {}; child.outputEvents = nullptr;
                child.sidechainInputs = nullptr; child.sidechainInputChannels = 0;
                child.inputSilenceMask = child.sidechainSilenceMask = 0;
                child.sampleTime += at;
                if (m < 4) m_modules[m]->process(child);
                else {
                    // Equalizer consumes audio-thread targets from host events;
                    // its public setters only publish the control-thread view.
                    std::array<PluginEvent, bandCount * 4> events{};
                    for (unsigned b = 0; b < bandCount; ++b)
                        for (unsigned f = 0; f < 4; ++f) {
                            auto& event = events[b * 4 + f];
                            event.kind = PluginEvent::Kind::ParamValue;
                            event.paramIndex = eqParameter(b, f);
                            event.value = parameterValue(eqOffset + b * 4 + f);
                        }
                    child.inputEvents = events;
                    m_eq.process(child);
                }
                for (unsigned f = 0; f < n; ++f) {
                    m_enabled[m] += std::clamp(target - m_enabled[m], -1 / (.005 * m_rate), 1 / (.005 * m_rate));
                    for (unsigned ch = 0; ch < 2; ++ch) {
                        const auto wetCh = std::min<unsigned>(ch, child.outputChannels - 1);
                        m_a[ch][f] += float(m_enabled[m] * (m_b[wetCh][f] - m_a[ch][f]));
                    }
                }
            }
            for (unsigned f = 0; f < n; ++f) {
                const double target = wantedOrder == m_audioOrder ? 1 : 0;
                m_orderGain += std::clamp(target - m_orderGain, -1 / (.005 * m_rate), 1 / (.005 * m_rate));
                for (unsigned ch = 0; ch < 2; ++ch)
                    m_a[ch][f] = m_dry[ch][f] + float(m_orderGain * (m_a[ch][f] - m_dry[ch][f]));
            }
        }
        for (unsigned ch = 0; ch < ctx.outputChannels; ++ch)
            if (ctx.outputs[ch]) for (unsigned f = 0; f < n; ++f)
                ctx.outputs[ch][at + f] = ch < 2 ? float(dsp::clean(m_a[ch][f])) : 0;
        at += n;
    }
}
PluginProcessDisposition ModulationRackInstance::process(const PluginProcessContext& ctx) noexcept {
    if (!ctx.outputs) return PluginProcessDisposition::Continue;
    unsigned cursor = 0;
    for (const auto& e : ctx.inputEvents) {
        const auto at = std::clamp(e.frameOffset, cursor, ctx.frames);
        if (at > cursor) render(ctx, cursor, at);
        cursor = at;
        if (e.kind == PluginEvent::Kind::ParamValue) setParameterFromHost(e.paramIndex, e.value);
    }
    if (cursor < ctx.frames) render(ctx, cursor, ctx.frames);
    return PluginProcessDisposition::Continue;
}
}
