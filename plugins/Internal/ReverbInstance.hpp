#pragma once

#include "Host/PluginInstance.hpp"
#include "Internal/ModulationDsp.hpp"
#include <array>
#include <atomic>

namespace daw::plugins::reverb {
enum class Param : unsigned {
    Mode, Size, Brightness, PreDelay, Decay, Width, Mix, Bloom, Motion, Duck,
    Freeze, Diffusion, EarlyLate, LowCut, HighCut, ShimmerAmount, Count
};
inline constexpr unsigned kParameterCount = unsigned(Param::Count);
inline constexpr unsigned kModeCount = 6;
using Values = std::array<double, kParameterCount>;
std::span<const ParameterInfo> parameterTable() noexcept;
std::string_view modeName(unsigned mode) noexcept;
struct Preset { std::string_view name; Values values; };
std::span<const Preset> factoryPresets() noexcept;
struct Telemetry { float input = 0, early = 0, tail = 0, output = 0, duckGain = 1; };
struct State {
    Values values{}, a{}, b{};
    char comparison = 'A';
    std::string presetKind{"factory"}, presetName{"Soft Hall"};
};
struct Reflection { double seconds = 0, gain = 0, pan = 0; };
std::array<Reflection, 12> reflections(const Values&) noexcept;
double bloomSeconds(const Values&) noexcept;

class ReverbInstance final : public PluginInstance {
public:
    ReverbInstance();
    static const PluginDescriptor& staticDescriptor() noexcept;
    static std::string_view uid() noexcept { return "daw.reverb"; }
    const PluginDescriptor& descriptor() const noexcept override { return staticDescriptor(); }
    void setListener(PluginListener*) noexcept override {}
    bool needsMainThreadPump() const noexcept override { return false; }
    bool supportsOfflinePipelining() const noexcept override { return true; }
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
    std::uint32_t latencySamples() const noexcept override { return 0; }
    std::uint32_t tailSamples() const noexcept override;
    bool tailSamplesKnown() const noexcept override { return true; }
    Telemetry telemetry() const noexcept;
    Values values() const noexcept;
    State state() const;
    void setComparisonState(const State&);
    void captureComparison(char);
    Values comparison(char) const;
    void copyComparison();
    void setActiveComparison(char side) noexcept { if (side == 'A' || side == 'B') m_comparison = side; }
    char activeComparison() const noexcept { return m_comparison; }
    void setPresetReference(std::string kind, std::string name);
    std::pair<std::string, std::string> presetReference() const { return {m_presetKind, m_presetName}; }
private:
    static double clamp(unsigned, double) noexcept;
    void tune(bool initial = false) noexcept;
    void render(const PluginProcessContext&, unsigned offset, unsigned count) noexcept;
    PluginBusLayout m_layout{{2, 2}, {2}};
    std::array<std::atomic<double>, kParameterCount> m_values{};
    Values m_smooth{};
    std::array<modulation::dsp::Delay, 16> m_lines;
    std::array<modulation::dsp::Delay, 2> m_pre, m_early, m_bloom, m_shimmer;
    std::array<std::array<modulation::dsp::Delay, 4>, 2> m_diffusers;
    std::array<modulation::dsp::Tone, 2> m_outputTone{}, m_shimmerTone{};
    std::array<double, 16> m_from{}, m_to{}, m_feedback{}, m_low{}, m_phases{};
    std::array<Reflection, 12> m_earlyFrom{}, m_earlyTo{};
    double m_rate = 48000, m_smoothStep = 0, m_fadeStep = 0, m_fade = 1;
    double m_damping = 0, m_inputGain = .1, m_diffusion = .65, m_freeze = 0;
    double m_duckEnvelope = 0, m_duckAttack = 0, m_duckRelease = 0, m_meterStep = 0;
    double m_inputMeter = 0, m_earlyMeter = 0, m_tailMeter = 0, m_outputMeter = 0;
    double m_shimmerPhase = 0, m_shimmerMix = 0, m_bloomTime = 0;
    std::uint64_t m_clock = 0;
    unsigned m_profileFrom = 0, m_profileTo = 0;
    std::array<std::atomic<float>, 5> m_telemetry{};
    std::atomic<std::uint32_t> m_tailSamples{480000};
    Values m_a{}, m_b{};
    char m_comparison = 'A';
    std::string m_presetKind{"factory"}, m_presetName{"Soft Hall"};
    bool m_active = false, m_processing = false, m_sidechain = false, m_freezeStructure = false;
};
} // namespace daw::plugins::reverb
