#pragma once
#include "Host/PluginInstance.hpp"
#include "DSP/HalfBandFir.hpp"
#include "Internal/Vlt1073Circuit.hpp"
#include <array>
#include <atomic>

namespace daw::plugins::vlt1073 {
enum class Param : unsigned { InputMode, InputEnabled, MicGain, LineGain, MicZ, Pad, HighGain, MidFrequency, MidGain, LowFrequency, LowGain, HighPass, EqEnabled, Phase, Level, Output };
inline constexpr unsigned kParameterCount = 16, kLatency = 48;
inline constexpr std::array<double, 7> kMidHz{0, 360, 700, 1600, 3200, 4800, 7200};
inline constexpr std::array<double, 5> kLowHz{0, 35, 60, 110, 220}, kHighPassHz{0, 50, 80, 160, 300};
inline constexpr double kVoltsPerFullScale = 13.798512197; // -18 dBFS peak sine -> +4 dBu RMS.
std::span<const ParameterInfo> parameterTable() noexcept;

class Vlt1073Instance final : public PluginInstance {
public:
    Vlt1073Instance();
    static const PluginDescriptor& staticDescriptor() noexcept;
    static std::string_view uid() noexcept { return "daw.1073"; }
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
    std::uint32_t tailSamples() const noexcept override { return std::uint32_t(m_rate * 2); }
    bool tailSamplesKnown() const noexcept override { return true; }
private:
    using Fir = engine::dsp::HalfBandFir65;
    struct Channel {
        Fir up2, up4, down4, down2;
        circuit::Transformer inputTransformer, outputTransformer;
        circuit::Rc inputCoupling, outputCoupling, bandwidth;
        circuit::Eq eq;
        circuit::HighPass highPass;
    };
    static double clamp(unsigned, double) noexcept;
    void updateTargets() noexcept;
    void updateCircuit() noexcept;
    void render(const PluginProcessContext&, unsigned offset, unsigned frames) noexcept;
    double tick4x(double, Channel&) noexcept;
    PluginBusLayout m_layout{{2}, {2}};
    std::array<std::atomic<double>, kParameterCount> m_values{};
    std::array<double, kParameterCount> m_target{}, m_smooth{};
    std::array<double, 65> m_filter{};
    std::array<Channel, 2> m_channels{};
    circuit::EqSettings m_eq, m_audioEq;
    std::array<double, 13> m_audio{}, m_audioTarget{};
    double m_audioSmoothing = 0;
    double m_rate = 48000, m_smoothing = 0;
    double m_gain = 1, m_firstGain = 1, m_secondGain = 1, m_micMix = 0, m_highStage = 0;
    double m_inputScale = 1, m_level = 1, m_output = 1, m_inputOn = 1, m_eqMix = 1, m_phase = 1, m_hpMix = 0;
    double m_hpG = 0, m_couplingG = 0, m_inputFluxG = 0, m_outputFluxG = 0, m_bandwidthG = 0, m_load = 1;
    unsigned m_controlPhase = 0;
    bool m_active = false, m_processing = false, m_fresh = true;
};
} // namespace daw::plugins::vlt1073
