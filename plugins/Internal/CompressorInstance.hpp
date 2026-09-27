#pragma once

#include "Host/PluginInstance.hpp"
#include <array>
#include <atomic>

namespace daw::plugins::compressor {

enum class Param : std::uint32_t { Ratio, Threshold, Makeup, Attack, Release, Knee, Mix, Mode, AutoGain, Count };
inline constexpr unsigned kParameterCount = unsigned(Param::Count);
inline constexpr unsigned kLatency = 16;
std::span<const ParameterInfo> parameterTable() noexcept;
// Positive attenuation in dB. The editor samples this exact DSP curve.
double reductionDb(double inputDb, double threshold, double ratio, double knee) noexcept;
double autoGainDb(double threshold, double ratio, double knee) noexcept;

struct Telemetry { float input = 0, output = 0, reduction = 0; };

class CompressorInstance final : public PluginInstance {
public:
    CompressorInstance();
    static const PluginDescriptor& staticDescriptor() noexcept;
    static std::string_view uid() noexcept { return "daw.compressor"; }
    const PluginDescriptor& descriptor() const noexcept override { return staticDescriptor(); }
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
    std::uint32_t tailSamples() const noexcept override { return 32; }
    Telemetry consumeTelemetry() noexcept;

private:
    struct Fir {
        std::array<double, 33> history{};
        unsigned cursor = 0;
        double tick(double, const std::array<double, 33>&) noexcept;
    };
    void render(const PluginProcessContext&, unsigned offset, unsigned frames) noexcept;
    static double clamp(unsigned, double) noexcept;
    PluginBusLayout m_layout{{2, 2}, {2}};
    std::array<std::atomic<double>, kParameterCount> m_values{};
    std::array<double, 33> m_filter{};
    std::array<Fir, 2> m_up{}, m_down{};
    std::array<std::array<double, kLatency>, 2> m_dry{}, m_wet{};
    std::array<double, kLatency> m_modeDelay{};
    unsigned m_cursor = 0;
    double m_rate = 48000, m_rmsCoefficient = 0, m_smoothCoefficient = 0;
    double m_power = 0, m_softReduction = 0, m_punchReduction = 0;
    double m_mode = 0, m_modeTarget = 0, m_modeStep = 0;
    unsigned m_modeRemaining = 0;
    double m_makeup = 0, m_auto = 0, m_mix = 1;
    std::array<float, 3> m_blockPeaks{};
    std::array<std::atomic<float>, 3> m_peaks{};
    bool m_active = false, m_processing = false, m_sidechain = false;
};
} // namespace daw::plugins::compressor
