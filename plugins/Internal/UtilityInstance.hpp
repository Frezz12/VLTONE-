#pragma once

#include "Host/PluginInstance.hpp"
#include <array>
#include <atomic>

namespace daw::plugins::utility {
enum class Kind { Gain, Stereo, Reverse };
inline constexpr unsigned kParameterCapacity = 5;
const PluginDescriptor& descriptorFor(Kind) noexcept;
std::span<const ParameterInfo> parameterTable(Kind) noexcept;
bool isUtilityUid(std::string_view) noexcept;
double reverseMilliseconds(double mode, double milliseconds, double division, double tempo) noexcept;

// Three independent insert identities sharing only the host/state plumbing.
class UtilityInstance final : public PluginInstance {
public:
    explicit UtilityInstance(Kind);
    const PluginDescriptor& descriptor() const noexcept override { return descriptorFor(m_kind); }
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
    std::span<const ParameterInfo> parameters() const noexcept override { return parameterTable(m_kind); }
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

private:
    struct Ramp {
        double value = 0, target = 0, step = 0;
        unsigned remaining = 0;
        void reset(double v) noexcept { value = target = v; step = 0; remaining = 0; }
        void set(double v, unsigned frames) noexcept;
        double next() noexcept;
    };
    struct ReverseLane {
        std::array<std::vector<float>, 2> samples;
        unsigned length = 0, position = 0, bank = 0;
        bool ready = false;
        void reset(unsigned frames) noexcept;
        std::array<double, 2> tick(double l, double r, unsigned fade) noexcept;
    };
    double clamp(unsigned, double) const noexcept;
    std::array<double, 4> matrix(bool mono) const noexcept;
    void render(const PluginProcessContext&, unsigned begin, unsigned end) noexcept;
    Kind m_kind;
    PluginBusLayout m_layout{{2}, {2}};
    std::array<std::atomic<double>, kParameterCapacity> m_values{};
    std::array<Ramp, 4> m_matrix;
    Ramp m_gain, m_mix;
    std::array<ReverseLane, 2> m_reverse;
    std::atomic<unsigned> m_longestSlice{0};
    unsigned m_lane = 0, m_crossfade = 0, m_rampFrames = 480, m_edgeFrames = 144;
    bool m_pending = false, m_active = false, m_processing = false;
    double m_rate = 48000;
};
} // namespace daw::plugins::utility
