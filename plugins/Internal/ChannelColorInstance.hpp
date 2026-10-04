#pragma once

#include "Host/PluginInstance.hpp"
#include "DSP/HalfBandFir.hpp"
#include <array>
#include <atomic>

namespace daw::plugins::channel_color {
inline constexpr unsigned kLatency = 48;
std::span<const ParameterInfo> parameterTable() noexcept;

/// A channel-owned tape/triode color stage. Its host owns the power bypass.
/// Histories belong to a channel; component tolerances belong to its saved seed.
class ChannelColorInstance final : public PluginInstance {
public:
    ChannelColorInstance();
    static const PluginDescriptor& staticDescriptor() noexcept;
    static std::string_view uid() noexcept { return "daw.channel-color"; }
    const PluginDescriptor& descriptor() const noexcept override { return staticDescriptor(); }
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
    std::uint32_t latencySamples() const noexcept override { return kLatency; }
    std::uint32_t tailSamples() const noexcept override { return std::uint32_t(m_rate); }
    bool tailSamplesKnown() const noexcept override { return true; }
    void setProfileSeed(std::uint64_t); // control thread, under RenderGate
    std::uint64_t profileSeed() const noexcept { return m_seed; }

private:
    struct Coefficients { double b0=1, b1=0, b2=0, a1=0, a2=0; };
    struct Filter {
        double z1=0, z2=0;
        double tick(double, const Coefficients&) noexcept;
    };
    struct Channel {
        engine::dsp::HalfBandFir65 up2, up4, down4, down2;
        Filter pre, post, bump, loss;
        double field=0, magnetization=0, dcInput=0, dcOutput=0;
        std::array<double, kLatency> dry{};
    };
    void render(const PluginProcessContext&, unsigned, unsigned) noexcept;
    double magneticSlope(double, double, double) const noexcept;
    double magnetize(Channel&, double) const noexcept;
    double triode(double) const noexcept;
    void makeTriodeTable(double);
    static Coefficients interpolate(const std::array<Coefficients, 129>&, double) noexcept;

    PluginBusLayout m_layout{{2}, {2}};
    std::array<std::atomic<double>, 2> m_values{};
    std::array<Channel, 2> m_channels{};
    const std::array<double, 65>& m_taps;
    std::array<double, 1025> m_triode{}, m_triodeSlope{};
    std::array<double, 257> m_gainTable{};
    std::array<Coefficients, 129> m_pre{}, m_post{};
    Coefficients m_bump, m_loss;
    double m_rate=48000, m_smoothing=0, m_dcPole=0;
    double m_drive=0, m_tone=0, m_driveTarget=0, m_toneTarget=0;
    double m_a=.063, m_k=.08, m_c=.9, m_alpha=.0016, m_linear=1;
    std::uint64_t m_seed=0;
    unsigned m_dryCursor=0;
    bool m_profileReady=false, m_active=false, m_processing=false;
};
} // namespace daw::plugins::channel_color
