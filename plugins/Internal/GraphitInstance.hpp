#pragma once

#include "Host/PluginInstance.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace daw::plugins::graphit {

enum class Param : std::uint32_t { Amount = 0, Mode, Priority, Count };
enum class Mode : std::uint32_t { A = 0, B, P, C, X };

inline constexpr std::uint32_t kParameterCount = std::uint32_t(Param::Count);
inline constexpr std::size_t kHistorySize = 48;

struct Telemetry {
    float inputLeft = 0.0f;
    float inputRight = 0.0f;
    float outputLeft = 0.0f;
    float outputRight = 0.0f;
    float gainReductionDb = 0.0f;
};

std::span<const ParameterInfo> parameterTable() noexcept;
std::string parameterText(std::uint32_t index, double value);

/// VLT's built-in one-knob colour, saturation and dynamics effect.
class GraphitInstance final : public PluginInstance {
public:
    GraphitInstance();

    static const PluginDescriptor& staticDescriptor() noexcept;
    static std::string_view uid() noexcept;

    const PluginDescriptor& descriptor() const noexcept override { return m_descriptor; }
    void setListener(PluginListener* listener) noexcept override { m_listener = listener; }

    bool setBusLayout(const PluginBusLayout& wanted,
                      PluginBusLayout& accepted) override;
    PluginBusLayout busLayout() const override { return m_layout; }
    bool activate(const PluginProcessInfo& info) override;
    void deactivate() override;
    bool isActive() const noexcept override { return m_active; }
    void startProcessing() override { m_processing = true; }
    void stopProcessing() override { m_processing = false; }

    std::span<const ParameterInfo> parameters() const noexcept override;
    std::int32_t parameterIndexForId(std::string_view id) const noexcept override;
    double parameterValue(std::uint32_t index) const noexcept override;
    std::string parameterText(std::uint32_t index,
                              double plainValue) const override;
    void setParameterFromHost(std::uint32_t index, double plainValue) override;

    bool saveState(std::vector<std::uint8_t>& out) const override;
    bool loadState(std::span<const std::uint8_t> state) override;

    bool hasEditor() const noexcept override { return false; }
    bool openEditor(void*, PluginEditorHost*) override { return false; }
    void closeEditor() override {}
    bool isEditorOpen() const noexcept override { return false; }
    bool editorSize(std::uint32_t&, std::uint32_t&) const override { return false; }
    bool editorCanResize() const override { return false; }
    bool setEditorSize(std::uint32_t&, std::uint32_t&) override { return false; }

    PluginProcessDisposition process(
        const PluginProcessContext& context) noexcept override;
    void reset() noexcept override;
    std::uint32_t latencySamples() const noexcept override { return 0; }
    std::uint32_t tailSamples() const noexcept override { return 0; }

    Telemetry consumeTelemetry() noexcept;

private:
    enum class FilterType : std::uint8_t { Bell, LowShelf, HighShelf, HighCut };
    enum class Curve : std::uint8_t { Tanh, Atan, Cubic, Hard };

    struct EqBand {
        FilterType type = FilterType::Bell;
        double frequency = 1000.0;
        double gainDb = 0.0;
        double q = 0.707;
        bool enabled = false;
    };

    struct Profile {
        std::array<EqBand, 4> bands{};
        Curve curve = Curve::Tanh;
        double driveDb = 0.0;
        double saturationMix = 0.0;
        double thresholdDb = 0.0;
        double ratio = 1.0;
        double attackMs = 20.0;
        double releaseMs = 150.0;
        double kneeDb = 6.0;
        double compressionMix = 0.0;
        double makeupDb = 0.0;
        double trimDb = 0.0;
    };

    struct Coefficients {
        double b0 = 1.0;
        double b1 = 0.0;
        double b2 = 0.0;
        double a1 = 0.0;
        double a2 = 0.0;
    };

    struct Biquad {
        Coefficients coefficients;
        double z1 = 0.0;
        double z2 = 0.0;

        double process(double input) noexcept;
        void reset() noexcept { z1 = z2 = 0.0; }
    };

    enum class FadeState : std::uint8_t { Steady, Out, In };

    static const Profile& profile(int mode) noexcept;
    static double clampParameter(std::uint32_t index, double value) noexcept;
    static Coefficients design(FilterType type, double frequency, double gainDb,
                               double q, double sampleRate) noexcept;
    static double transfer(Curve curve, double value) noexcept;
    static double antiderivative(Curve curve, double value) noexcept;

    void applyEvent(const PluginEvent& event) noexcept;
    void renderSlice(const PluginProcessContext& context, std::uint32_t offset,
                     std::uint32_t frames) noexcept;
    void updateFilters(const Profile& selected, double depth,
                       double priority) noexcept;
    double saturate(Curve curve, double value, std::uint16_t channel) noexcept;
    void requestMode(int mode) noexcept;
    void resetPath() noexcept;
    void publishPeak(std::atomic<float>& destination, float value) noexcept;

    PluginDescriptor m_descriptor;
    PluginListener* m_listener = nullptr;
    PluginBusLayout m_layout{{2}, {2}};
    std::array<std::atomic<double>, kParameterCount> m_values;

    double m_sampleRate = 48000.0;
    std::uint32_t m_maxBlockSize = 512;
    bool m_active = false;
    bool m_processing = false;
    double m_amountSmoothed = 0.35;
    double m_prioritySmoothed = 0.0;
    int m_activeMode = 0;
    int m_pendingMode = 0;
    FadeState m_fadeState = FadeState::Steady;
    double m_modeFade = 1.0;
    std::uint32_t m_controlCountdown = 0;

    std::array<std::array<Biquad, 4>, 2> m_filters{};
    std::array<std::array<Biquad, 2>, 2> m_priorityFilters{};
    std::array<double, 2> m_previousDriven{};
    double m_gainReductionDb = 0.0;

    std::atomic<float> m_inputPeakLeft{0.0f};
    std::atomic<float> m_inputPeakRight{0.0f};
    std::atomic<float> m_outputPeakLeft{0.0f};
    std::atomic<float> m_outputPeakRight{0.0f};
    std::atomic<float> m_gainReductionTelemetry{0.0f};
};

} // namespace daw::plugins::graphit
