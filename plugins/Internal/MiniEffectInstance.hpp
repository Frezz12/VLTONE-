#pragma once

#include "Host/PluginInstance.hpp"
#include <array>
#include <atomic>
#include <memory>

namespace daw::plugins::mini {
enum class EffectKind { Tremolo, Stereo, Air, Pitch, Filter, SoftClip };
struct EffectType { std::string_view id, name; EffectKind kind; };
std::span<const EffectType> effectTypes() noexcept;
std::span<const ParameterInfo> effectParameters(EffectKind) noexcept;
std::unique_ptr<PluginInstance> createMiniEffect(std::string_view);

// Internal Mini/Creator DSP nodes. The enclosing graph owns their two controls,
// modes, state and latency compensation; no standalone insert/editor is needed.
class MiniEffectInstance final : public PluginInstance {
public:
    explicit MiniEffectInstance(EffectKind);
    ~MiniEffectInstance() override;
    const PluginDescriptor& descriptor() const noexcept override;
    bool supportsOfflinePipelining() const noexcept override { return true; }
    void setListener(PluginListener*) noexcept override {}
    bool setBusLayout(const PluginBusLayout&, PluginBusLayout&) override;
    PluginBusLayout busLayout() const override { return m_layout; }
    bool activate(const PluginProcessInfo&) override;
    void deactivate() override { m_active = m_processing = false; }
    bool isActive() const noexcept override { return m_active; }
    bool isProcessing() const noexcept override { return m_processing; }
    void startProcessing() override { m_processing = true; }
    void stopProcessing() override { m_processing = false; }
    std::span<const ParameterInfo> parameters() const noexcept override { return effectParameters(m_kind); }
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
    std::uint32_t latencySamples() const noexcept override;
    std::uint32_t tailSamples() const noexcept override;
    bool tailSamplesKnown() const noexcept override { return true; }
private:
    struct DSP;
    std::unique_ptr<DSP> m_dsp;
    EffectKind m_kind;
    PluginBusLayout m_layout{{2}, {2}};
    std::array<std::atomic<double>, 3> m_values{};
    bool m_active = false, m_processing = false;
};
} // namespace daw::plugins::mini
