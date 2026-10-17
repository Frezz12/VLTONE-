#pragma once
#include "Host/PluginInstance.hpp"
#include "Graph/Node.hpp"
#include "Common/RealtimeSnapshot.hpp"
#include "Modulation/Modulation.hpp"
#include <atomic>
#include <memory>

namespace daw::plugins {
// Resolves the parameter's base and all contributors before sending one value
// to the adapter. No generated value enters the document or the gesture queue.
class PluginModulation {
public:
    struct Route {
        std::uint32_t parameter = 0, input = 0; // input counts Modulation edges only
        double depth = 0.2;
        bool bipolar = true, enabled = true;
        daw::modulation::Curve automation;
        std::string id;
    };
    struct State {
        std::atomic<double> manual{0}, base{0}, effective{0};
        std::atomic<bool> mapped{false};
        double delta = 0, from = 0;
        std::uint64_t generation = 0;
        std::uint32_t remaining = 0;
    };
    struct Target { std::uint32_t parameter = 0; std::shared_ptr<State> state; std::vector<Route> routes; };
    struct Configuration { std::uint64_t generation = 0; std::vector<Target> targets; };
    void configure(std::vector<Route> routes, const PluginInstance& instance);
    void reset() noexcept;
    void prepare(std::size_t capacity) { m_original.reserve(capacity); }
    bool active() const noexcept { return m_active.load(std::memory_order_relaxed); }
    void edit(std::uint32_t index, double value) noexcept;
    double base(std::uint32_t index, double fallback) const noexcept;
    double manual(std::uint32_t index, double fallback) const noexcept;
    double effective(std::uint32_t index, double fallback) const noexcept;
    bool mapped(std::uint32_t index) const noexcept;
    bool apply(const engine::ProcessContext&, const PluginInstance&, std::vector<PluginEvent>&);
    std::shared_ptr<const Configuration> snapshot() const { return m_configuration.controlCopy(); }
    void restore(std::shared_ptr<const Configuration> configuration);
private:
    std::vector<std::shared_ptr<State>> m_states;
    engine::RealtimeSnapshot<Configuration> m_configuration;
    std::vector<PluginEvent> m_original;
    std::atomic<bool> m_active{false};
    std::uint64_t m_generation = 0;
};
} // namespace daw::plugins
