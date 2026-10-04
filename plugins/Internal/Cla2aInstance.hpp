#pragma once

#include "Host/PluginInstance.hpp"
#include "DSP/HalfBandFir.hpp"
#include <array>
#include <atomic>

namespace daw::plugins::cla2a {

enum class Param : std::uint32_t { Gain, PeakReduction, Mode, Count };
inline constexpr unsigned kParameterCount = unsigned(Param::Count);
inline constexpr unsigned kLatency = 48;
std::span<const ParameterInfo> parameterTable() noexcept;

struct Telemetry { float input = 0, output = 0, reduction = 0; };

/// Independently calibrated LA-2A model, not a capture of a particular unit.
/// T4 illumination and recovery are shared in stereo; amplifier/transformer
/// histories are per-channel. Nothing in process() allocates or locks.
class Cla2aInstance final : public PluginInstance {
public:
    Cla2aInstance();
    static const PluginDescriptor& staticDescriptor() noexcept;
    static std::string_view uid() noexcept { return "daw.cla2a"; }
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
    std::uint32_t tailSamples() const noexcept override;
    bool tailSamplesKnown() const noexcept override { return true; }
    Telemetry consumeTelemetry() noexcept;

private:
    using Fir = engine::dsp::HalfBandFir65;
    struct Coupling {
        double previous = 0, output = 0;
        double tick(double, double pole) noexcept;
    };
    struct Channel {
        Fir up2, up4, down4, down2;
        Coupling interstage, outputCoupling;
        double inputFlux = 0, outputFlux = 0, highCut = 0;
    };
    void render(const PluginProcessContext&, unsigned offset, unsigned frames) noexcept;
    std::array<double, 2> tick4x(std::array<double, 2>, unsigned channels) noexcept;
    static double clamp(unsigned, double) noexcept;
    PluginBusLayout m_layout{{2}, {2}};
    std::array<std::atomic<double>, kParameterCount> m_values{};
    std::array<double, 65> m_filter{};
    std::array<Channel, 2> m_channels{};
    double m_rate = 48000;
    double m_smoothing = 0, m_attack = 0, m_fastRelease = 0, m_detector = 0;
    double m_charge = 0, m_discharge = 0, m_coupling = 0;
    double m_inputFlux = 0, m_outputFlux = 0, m_highCut = 0;
    // The slow release coefficient is interpolated from a fixed table, so
    // program-dependent recovery needs no per-sample exp() or allocation.
    std::array<double, 129> m_slowRelease{};
    double m_gain = 1, m_peakDrive = 0, m_mode = 0;
    double m_gainTarget = 1, m_peakTarget = 0, m_modeTarget = 0;
    double m_power = 0, m_fastCell = 0, m_slowCell = 0, m_exposure = 0;
    double m_reduction = 0;
    std::array<float, 3> m_blockPeaks{};
    std::array<std::atomic<float>, 3> m_peaks{};
    bool m_active = false, m_processing = false;
};
} // namespace daw::plugins::cla2a
