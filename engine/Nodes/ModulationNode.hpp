#pragma once
#include "Graph/Node.hpp"
#include "Common/RealtimeSnapshot.hpp"
#include "Modulation/Modulation.hpp"
#include <atomic>
#include <array>

namespace daw::engine {
class ModulationNode final : public Node {
public:
    struct Settings {
        modulation::Source source;
        modulation::Curve rate; // Hz or cycles per beat, according to source.sync.
    };
    explicit ModulationNode(std::string name) : m_name(std::move(name)) {}
    std::string_view name() const noexcept override { return m_name; }
    ChannelCount outputChannels() const noexcept override { return 1; }
    MidiNodeRole midiRole() const noexcept override { return MidiNodeRole::None; }
    OfflineNodePolicy offlineNodePolicy() const noexcept override { return OfflineNodePolicy::Ordered; }
    bool isSource() const noexcept override { return true; }
    void setSettings(std::shared_ptr<const Settings> settings) { m_settings.publish(std::move(settings)); }
    float value() const noexcept { return m_value.load(std::memory_order_relaxed); }
    float position() const noexcept { return m_position.load(std::memory_order_relaxed); }
    using ControlState = std::shared_ptr<const Settings>;
    ControlState controlState() const { return m_settings.controlCopy(); }
    void restoreControlState(ControlState state) { setSettings(std::move(state)); }
    void reset() override;
    void process(const ProcessContext& context) override;
private:
    std::string m_name;
    RealtimeSnapshot<Settings> m_settings;
    std::atomic<float> m_value{0};
    std::atomic<float> m_position{0};
    double m_envelope = 0.0, m_freePhase = 0.0, m_smoothed = 0.5;
    std::array<double, 2> m_low{}, m_high{};
    SamplePos m_next = 0;
    bool m_initialized = false, m_wasPlaying = false;
};
} // namespace daw::engine
