#pragma once

#include "Graph/Node.hpp"

#include <atomic>
#include <string>
#include <utility>

namespace daw::engine {

/// Passes audio through untouched and publishes final-sum peaks for the UI.
/// Peak reduction is fused into the last aggregation pass, so metering adds no
/// second traversal of the completed output block.
class MeterNode : public Node {
public:
    explicit MeterNode(std::string name = "Meter") : m_name(std::move(name)) {}
    std::string_view name() const noexcept override { return m_name; }
    MidiNodeRole midiRole() const noexcept override { return MidiNodeRole::None; }

    float peakLeft() const noexcept { return m_peakL.load(std::memory_order_relaxed); }
    float peakRight() const noexcept { return m_peakR.load(std::memory_order_relaxed); }

    void process(const ProcessContext& context) noexcept override;

private:
    std::string m_name;
    std::atomic<float> m_peakL{0.0f};
    std::atomic<float> m_peakR{0.0f};
};

} // namespace daw::engine
