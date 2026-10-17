#include "Host/PluginModulation.hpp"
#include <algorithm>
#include <cmath>

namespace daw::plugins {
void PluginModulation::configure(std::vector<Route> routes, const PluginInstance& instance) {
    const auto parameters = instance.parameters();
    m_states.resize(parameters.size());
    for (const auto& route : routes) {
        if (route.parameter >= parameters.size() || parameters[route.parameter].isReadOnly) continue;
        if (!m_states[route.parameter]) {
            auto state = std::make_shared<State>();
            const auto value = instance.parameterValue(route.parameter);
            state->manual.store(value); state->base.store(value); state->effective.store(value);
            m_states[route.parameter] = std::move(state);
        }
    }
    auto config = std::make_shared<Configuration>();
    config->generation = ++m_generation;
    for (std::uint32_t i = 0; i < m_states.size(); ++i) if (m_states[i]) {
        Target target{i, m_states[i], {}};
        for (const auto& route : routes) if (route.parameter == i) target.routes.push_back(route);
        target.state->mapped.store(!target.routes.empty());
        config->targets.push_back(std::move(target));
    }
    m_active.store(!config->targets.empty());
    m_configuration.publish(std::move(config));
}
void PluginModulation::reset() noexcept {
    auto config = m_configuration.read();
    if (!config) return;
    for (const auto& target : config->targets) {
        auto& state = *target.state;
        state.delta = state.from = 0; state.remaining = 0; state.generation = 0;
        state.base.store(state.manual.load()); state.effective.store(state.manual.load());
    }
    m_active.store(!config->targets.empty());
}
void PluginModulation::restore(std::shared_ptr<const Configuration> config) {
    for (auto& state : m_states) if (state) state->mapped.store(false);
    if (config) for (const auto& target : config->targets) {
        if (m_states.size() <= target.parameter) m_states.resize(target.parameter + 1);
        m_states[target.parameter] = target.state;
        target.state->mapped.store(!target.routes.empty());
    }
    // Retire states first introduced by a failed transaction.
    for (std::size_t i = 0; i < m_states.size(); ++i)
        if (!config || std::none_of(config->targets.begin(), config->targets.end(),
                [&](const Target& t) { return t.parameter == i; })) m_states[i].reset();
    m_active.store(config && !config->targets.empty());
    m_configuration.publish(std::move(config));
}
void PluginModulation::edit(std::uint32_t i, double value) noexcept {
    if (i < m_states.size() && m_states[i] && std::isfinite(value)) {
        m_states[i]->manual.store(value, std::memory_order_relaxed);
        m_states[i]->base.store(value, std::memory_order_relaxed);
    }
}
double PluginModulation::base(std::uint32_t i, double fallback) const noexcept {
    return i < m_states.size() && m_states[i] ? m_states[i]->base.load(std::memory_order_relaxed) : fallback;
}
double PluginModulation::manual(std::uint32_t i, double fallback) const noexcept {
    return i < m_states.size() && m_states[i] ? m_states[i]->manual.load(std::memory_order_relaxed) : fallback;
}
double PluginModulation::effective(std::uint32_t i, double fallback) const noexcept {
    return i < m_states.size() && m_states[i] ? m_states[i]->effective.load(std::memory_order_relaxed) : fallback;
}
bool PluginModulation::mapped(std::uint32_t i) const noexcept {
    return i < m_states.size() && m_states[i] && m_states[i]->mapped.load(std::memory_order_relaxed);
}
bool PluginModulation::apply(const engine::ProcessContext& c, const PluginInstance& plugin,
                              std::vector<PluginEvent>& events) {
    auto config = m_configuration.read();
    if (!config || config->targets.empty()) return true;
    if (events.size() > m_original.capacity()) return false;
    m_original.assign(events.begin(), events.end());
    std::erase_if(events, [&](const PluginEvent& e) {
        return e.kind == PluginEvent::Kind::ParamValue && std::any_of(config->targets.begin(), config->targets.end(),
            [&](const Target& t) { return t.parameter == e.paramIndex; });
    });
    bool ok = true, active = false;
    const auto ramp = std::max(1u, std::uint32_t(c.sampleRate * 0.01));
    for (const auto& target : config->targets) {
        auto& state = *target.state;
        if (state.generation != config->generation) {
            state.from = state.delta; state.remaining = ramp; state.generation = config->generation;
        }
        double base = state.manual.load(std::memory_order_relaxed);
        double previous = state.effective.load(std::memory_order_relaxed);
        std::size_t cursor = 0;
        for (engine::FrameCount frame = 0; frame < c.frames; ++frame) {
            while (cursor < m_original.size() && m_original[cursor].frameOffset <= frame) {
                const auto& e = m_original[cursor++];
                if (e.kind == PluginEvent::Kind::ParamValue && e.paramIndex == target.parameter) base = e.value;
            }
            double delta = 0.0;
            for (const auto& route : target.routes) {
                if (!route.enabled) continue;
                active = true;
                std::uint32_t ordinal = 0;
                for (std::size_t input = 0; input < c.inputs.size(); ++input) {
                    if (input >= c.inputRoles.size() || c.inputRoles[input] != engine::InputRole::Modulation) continue;
                    if (ordinal++ != route.input) continue;
                    if (!c.inputs[input].numChannels()) break;
                    double signal = c.inputs[input].data(0)[frame];
                    if (!std::isfinite(signal)) break;
                    signal = std::clamp(signal, 0.0, 1.0);
                    if (route.bipolar) signal = signal * 2.0 - 1.0;
                    const double depth = !route.automation.automated ? route.depth : route.automation.at(c.playing || c.offline ? c.ppqAtOffset(frame) : c.transport.ppqPosition);
                    delta += signal * std::clamp(depth, -1.0, 1.0);
                    break;
                }
            }
            if (state.remaining) {
                delta = std::lerp(state.from, delta, 1.0 - double(--state.remaining) / ramp);
            }
            state.delta = delta;
            const double value = delta == 0.0 ? base : plugin.parameterFromNormalized(target.parameter,
                std::clamp(plugin.parameterToNormalized(target.parameter, base) + delta, 0.0, 1.0));
            if (frame == 0 || value != previous) {
                if (events.size() < events.capacity()) {
                    PluginEvent e; e.kind = PluginEvent::Kind::ParamValue;
                    e.paramIndex = target.parameter; e.frameOffset = frame; e.value = value;
                    events.push_back(e);
                } else ok = false;
            }
            previous = value;
        }
        state.base.store(base, std::memory_order_relaxed);
        state.effective.store(previous, std::memory_order_relaxed);
        active |= state.remaining != 0 || state.delta != 0.0;
    }
    m_active.store(active, std::memory_order_relaxed);
    for (std::uint32_t i = 0; i < events.size(); ++i) events[i].sortOrder = i;
    std::sort(events.begin(), events.end(), [](const auto& a, const auto& b) {
        return a.frameOffset < b.frameOffset || (a.frameOffset == b.frameOffset && a.sortOrder < b.sortOrder);
    });
    return ok;
}
} // namespace daw::plugins
