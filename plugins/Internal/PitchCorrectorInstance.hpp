#pragma once

#include "Host/PluginInstance.hpp"
#include "Internal/PitchCorrectorDSP.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace daw::plugins::pitch {

enum class Param : std::uint32_t {
    Tune, Humanize, Vibrato, A4Hz, Key, Scale, Voice, Formants,
    Amount, OutputDb, NoteMask, Quality, Count
};
enum class Quality : int { RealTime = 0, HD = 1 };
enum class Scale : int {
    Chromatic, Major, NaturalMinor, HarmonicMinor, MelodicMinor,
    MajorPentatonic, MinorPentatonic, Custom
};
enum class Voice : int { Auto, Low, Mid, High };
inline constexpr std::uint32_t kParameterCount = std::uint32_t(Param::Count);

struct FactoryPreset {
    std::string_view name;
    double tune, humanize, vibrato;
};

std::span<const ParameterInfo> parameterTable() noexcept;
std::span<const FactoryPreset> factoryPresets() noexcept;
std::string parameterText(std::uint32_t index, double value);

/// Native monophonic vocal correction. The host owns bypass, wet/dry and PDC.
class PitchCorrectorInstance final : public PluginInstance {
public:
    PitchCorrectorInstance();
    ~PitchCorrectorInstance() override;
    static const PluginDescriptor& staticDescriptor() noexcept;
    static std::string_view uid() noexcept;
    const PluginDescriptor& descriptor() const noexcept override { return m_descriptor; }
    bool supportsOfflinePipelining() const noexcept override { return true; }
    void setListener(PluginListener* listener) noexcept override { m_listener = listener; }
    bool setBusLayout(const PluginBusLayout& wanted, PluginBusLayout& accepted) override;
    PluginBusLayout busLayout() const override { return m_layout; }
    bool activate(const PluginProcessInfo& info) override;
    void deactivate() override;
    bool isActive() const noexcept override { return m_active; }
    bool isProcessing() const noexcept override { return m_processing; }
    void startProcessing() override { m_processing = m_active; }
    void stopProcessing() override { m_processing = false; }

    std::span<const ParameterInfo> parameters() const noexcept override;
    std::int32_t parameterIndexForId(std::string_view id) const noexcept override;
    double parameterValue(std::uint32_t index) const noexcept override;
    std::string parameterText(std::uint32_t index, double value) const override;
    void setParameterFromHost(std::uint32_t index, double value) override;
    bool saveState(std::vector<std::uint8_t>& out) const override;
    bool loadState(std::span<const std::uint8_t> state) override;

    bool hasEditor() const noexcept override { return false; }
    bool openEditor(void*, PluginEditorHost*) override { return false; }
    void closeEditor() override {}
    bool isEditorOpen() const noexcept override { return false; }
    bool editorSize(std::uint32_t&, std::uint32_t&) const override { return false; }
    bool editorCanResize() const override { return false; }
    bool setEditorSize(std::uint32_t&, std::uint32_t&) override { return false; }

    PluginProcessDisposition process(const PluginProcessContext& context) noexcept override;
    void reset() noexcept override;
    std::uint32_t latencySamples() const noexcept override { return m_latency.load(std::memory_order_relaxed); }
    std::uint32_t tailSamples() const noexcept override { return m_tail.load(std::memory_order_relaxed); }

    Telemetry telemetrySnapshot() const noexcept;
    int activeQuality() const noexcept { return m_activeQuality.load(std::memory_order_relaxed); }
    bool qualityChangePending() const noexcept;
    /// Control thread, with rendering parked and no live audio activity. Merely
    /// arms the next prepare; reported latency remains unchanged until activate.
    bool applyPendingQuality() noexcept;

private:
    static double clampParameter(std::uint32_t index, double value) noexcept;
    Settings settings() const noexcept;
    void renderSlice(const PluginProcessContext&, std::uint32_t offset, std::uint32_t frames) noexcept;
    void publishTelemetry() noexcept;

    PluginDescriptor m_descriptor;
    PluginListener* m_listener = nullptr;
    PluginBusLayout m_layout{{2}, {2}};
    std::array<std::atomic<double>, kParameterCount> m_values;
    PitchCorrectorDSP m_dsp;
    std::array<std::vector<float>, 2> m_discard;
    std::vector<float> m_silence;
    std::uint32_t m_maxBlock = 512;
    std::uint32_t m_channels = 2;
    bool m_active = false, m_processing = false, m_preparedOnce = false;
    bool m_adoptQualityOnPrepare = false;
    std::atomic<int> m_activeQuality{0};
    std::atomic<std::uint32_t> m_latency{480};
    std::atomic<std::uint32_t> m_tail{480};
    std::array<std::atomic<double>, 6> m_telemetry{};
    std::atomic<bool> m_voiced{false};
    std::atomic<std::uint64_t> m_telemetrySerial{0};
};

} // namespace daw::plugins::pitch
