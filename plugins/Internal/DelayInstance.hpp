#pragma once
#include "Host/PluginInstance.hpp"
#include "Internal/ModulationDsp.hpp"
#include <array>
#include <atomic>

namespace daw::plugins::delay {
enum class Param : unsigned {
    TimeMode, TimeMs, Division, Bpm, Mode, Feedback, Mix, Output,
    Character, CharacterAmount, LowCut, HighCut, ModDepth, ModRate, Count
};
inline constexpr unsigned kParameterCount = unsigned(Param::Count);
inline constexpr unsigned kCharacterCount = 7;
inline constexpr unsigned kDivisionCount = 23;
std::span<const ParameterInfo> parameterTable() noexcept;
std::string_view divisionName(unsigned) noexcept;
std::string_view characterName(unsigned) noexcept;
struct Timing { double requestedMs = 375, milliseconds = 375, bpm = 120; };
Timing timing(std::span<const double>, const engine::TransportInfo&) noexcept;
struct Telemetry { float input = 0, wet = 0, output = 0; };

class DelayInstance final : public PluginInstance {
public:
    DelayInstance();
    static const PluginDescriptor& staticDescriptor() noexcept;
    static std::string_view uid() noexcept { return "daw.delay"; }
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
    std::uint32_t latencySamples() const noexcept override { return 0; }
    std::uint32_t tailSamples() const noexcept override;
    Telemetry consumeTelemetry() noexcept;
private:
    void render(const PluginProcessContext&, unsigned, unsigned) noexcept;
    static double clamp(unsigned, double) noexcept;
    PluginBusLayout m_layout{{2}, {2}};
    std::array<std::atomic<double>, kParameterCount> m_values{};
    std::array<double, kParameterCount> m_smooth{};
    std::array<modulation::dsp::Delay, 2> m_lines, m_colourLines;
    std::array<modulation::dsp::Tone, 2> m_loopTone{}, m_phoneTone{}, m_radioTone{}, m_tapeTone{};
    std::array<double, kCharacterCount> m_weights{}, m_weightFrom{}, m_weightTo{};
    std::array<double, 2> m_held{};
    std::array<float, 3> m_blockPeaks{};
    std::array<std::atomic<float>, 3> m_peaks{};
    std::atomic<double> m_tailMs{8000};
    double m_rate = 48000, m_smoothStep = 0, m_fadeStep = 0;
    double m_delayFrom = 0, m_delayTo = 0, m_delayFade = 1, m_wanted = 0;
    double m_route = 0, m_routeFrom = 0, m_routeTo = 0, m_routeFade = 1;
    double m_characterFade = 1, m_modPhase = 0, m_fmPhase = 0, m_tapePhase = 0;
    double m_holdPhase = 1, m_radioEnvelope = 0, m_radioAttack = 0, m_radioRelease = 0;
    unsigned m_character = 0;
    bool m_active = false, m_processing = false;
};
} // namespace daw::plugins::delay
