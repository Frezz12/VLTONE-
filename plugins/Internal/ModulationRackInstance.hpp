#pragma once
#include "Internal/ModulationInstance.hpp"
#include "Internal/EqualizerInstance.hpp"
#include <memory>

namespace daw::plugins::modulation {
// Fixed parameter identities follow the module, never its position in the chain.
class ModulationRackInstance final : public PluginInstance {
public:
    static constexpr unsigned moduleCount = 4, bandCount = 6;
    static constexpr std::array<Kind, 4> kinds{Kind::Chorus, Kind::Doubler, Kind::Flanger, Kind::Phaser};
    static constexpr std::array<unsigned, 4> offsets{0, 5, 9, 14};
    static constexpr unsigned orderParameter = 19, eqEnabledParameter = 20, eqOffset = 21;
    static constexpr unsigned parameterCount = eqOffset + bandCount * 4;
    using Order = std::array<unsigned, 4>;
    ModulationRackInstance();
    static const PluginDescriptor& staticDescriptor();
    static std::span<const ParameterInfo> parameterTable();
    static Order decodeOrder(unsigned) noexcept;
    static unsigned encodeOrder(Order) noexcept;
    static unsigned eqParameter(unsigned band, unsigned field) noexcept;
    const PluginDescriptor& descriptor() const noexcept override { return staticDescriptor(); }
    bool supportsOfflinePipelining() const noexcept override { return true; }
    void setListener(PluginListener*) noexcept override {}
    bool setBusLayout(const PluginBusLayout&, PluginBusLayout&) override;
    PluginBusLayout busLayout() const override { return m_layout; }
    bool activate(const PluginProcessInfo&) override;
    void deactivate() override;
    bool isActive() const noexcept override { return m_active; }
    bool isProcessing() const noexcept override { return m_processing; }
    void startProcessing() override { m_processing = true; }
    void stopProcessing() override { m_processing = false; }
    std::span<const ParameterInfo> parameters() const noexcept override { return parameterTable(); }
    std::int32_t parameterIndexForId(std::string_view) const noexcept override;
    double parameterValue(std::uint32_t) const noexcept override;
    std::string parameterText(std::uint32_t, double) const override;
    void setParameterFromHost(std::uint32_t, double) override;
    bool saveState(std::vector<std::uint8_t>&) const override;
    bool loadState(std::span<const std::uint8_t>) override;
    bool hasEditor() const noexcept override { return false; }
    bool openEditor(void*, PluginEditorHost*) override { return false; }
    void closeEditor() override {}
    bool isEditorOpen() const noexcept override { return false; }
    bool editorSize(std::uint32_t&, std::uint32_t&) const override { return false; }
    bool editorCanResize() const override { return false; }
    bool setEditorSize(std::uint32_t&, std::uint32_t&) override { return false; }
    PluginProcessDisposition process(const PluginProcessContext&) noexcept override;
    void reset() noexcept override;
    std::uint32_t latencySamples() const noexcept override { return 0; }
    std::uint32_t tailSamples() const noexcept override;
    const equalizer::EqualizerInstance& equalizer() const noexcept { return m_eq; }
private:
    void syncParameters() noexcept;
    void render(const PluginProcessContext&, unsigned begin, unsigned end) noexcept;
    std::array<std::unique_ptr<ModulationInstance>, 4> m_modules;
    equalizer::EqualizerInstance m_eq;
    std::array<std::atomic<double>, parameterCount> m_values{};
    PluginBusLayout m_layout{{2}, {2}};
    bool m_active = false, m_processing = false;
    double m_rate = 48000, m_orderGain = 1;
    unsigned m_audioOrder = 0;
    std::array<double, 5> m_enabled{};
    // Bounded scratch storage: no allocation even for oversized host blocks.
    static constexpr unsigned quantum = 64;
    using Buffer = std::array<std::array<float, quantum>, 2>;
    Buffer m_a{}, m_b{}, m_dry{};
};
}
