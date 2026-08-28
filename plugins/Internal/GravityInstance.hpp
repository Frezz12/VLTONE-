#pragma once

#include "Host/PluginInstance.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace daw::plugins::gravity {

enum class Param : std::uint32_t {
    Gravity,
    Pitch,
    Feedback,
    Decay,
    Size,
    Algorithm,
    TimingSync,
    TimingDivision,
    TimingMs,
    Mass,
    Motion,
    Density,
    Diffusion,
    Damping,
    Reverse,
    StereoWidth,
    StereoInput,
    Ducking,
    Transient,
    Drive,
    PitchSpread,
    PitchSnap,
    FeedbackLowcut,
    DetectorSource,
    Count,
};

enum class Algorithm : int { Orbit = 0, Fall, Rise, Void, Collapse, ZeroG };
enum class Division : int {
    Sixteenth = 0,
    EighthTriplet,
    Eighth,
    EighthDotted,
    Quarter,
    ThirtySecond,
    SixteenthTriplet,
    QuarterTriplet,
    QuarterDotted,
    Half,
    Whole,
};
enum class PitchSnap : int { Off = 0, Chromatic, Perfect, Octave };
enum class DetectorSource : int { Main = 0, Sidechain, Auto };

inline constexpr std::uint32_t kParameterCount = std::uint32_t(Param::Count);

struct FactoryPreset {
    std::string_view name;
    std::array<double, kParameterCount> values;
};

std::span<const ParameterInfo> parameterTable() noexcept;
std::string parameterText(std::uint32_t index, double value);
std::span<const FactoryPreset> factoryPresets() noexcept;

struct Telemetry {
    float inputLeft = 0.0f;
    float inputRight = 0.0f;
    float outputLeft = 0.0f;
    float outputRight = 0.0f;
    float fieldEnergy = 0.0f;
    float orbitPhase = 0.0f;
    float duckGain = 1.0f;
    float transientPulse = 0.0f;
    std::uint64_t grainSerial = 0;
    std::uint32_t activeGrains = 0;
    bool frozen = false;
};

/// VLT's built-in spatial granular pitch-delay.
///
/// All storage is prepared in activate(). process() only touches fixed arrays,
/// pre-sized vectors and atomics, so it remains allocation-free on the audio
/// thread. The Qt editor is host-drawn, like the built-in Sampler.
class GravityInstance final : public PluginInstance {
public:
    GravityInstance();

    static const PluginDescriptor& staticDescriptor() noexcept;
    static std::string_view uid() noexcept;

    const PluginDescriptor& descriptor() const noexcept override { return m_descriptor; }
    void setListener(PluginListener* listener) noexcept override { m_listener = listener; }

    bool setBusLayout(const PluginBusLayout& wanted, PluginBusLayout& accepted) override;
    PluginBusLayout busLayout() const override { return m_layout; }
    bool activate(const PluginProcessInfo& info) override;
    void deactivate() override;
    bool isActive() const noexcept override { return m_active; }
    void startProcessing() override { m_processing = true; }
    void stopProcessing() override { m_processing = false; }

    std::span<const ParameterInfo> parameters() const noexcept override;
    std::int32_t parameterIndexForId(std::string_view id) const noexcept override;
    double parameterValue(std::uint32_t index) const noexcept override;
    std::string parameterText(std::uint32_t index, double plainValue) const override;
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

    PluginProcessDisposition process(const PluginProcessContext& context) noexcept override;
    void reset() noexcept override;
    std::uint32_t latencySamples() const noexcept override { return 0; }
    std::uint32_t tailSamples() const noexcept override;

    void setFrozen(bool frozen) noexcept { m_frozen.store(frozen, std::memory_order_release); }
    bool frozen() const noexcept { return m_frozen.load(std::memory_order_acquire); }
    void clearTail() noexcept { m_clearRequested.store(true, std::memory_order_release); }
    Telemetry consumeTelemetry() noexcept;

    void setLastPreset(int index);
    int lastPreset() const noexcept { return m_lastPreset.load(std::memory_order_relaxed); }
    void setPresetReference(std::string kind, std::string name);
    std::pair<std::string, std::string> presetReference() const;

private:
    struct Grain {
        bool active = false;
        double read = 0.0;
        double step = 1.0;
        std::uint32_t age = 0;
        std::uint32_t length = 0;
        float pan = 0.0f;
        float gain = 1.0f;
        float color = 1.0f;
        std::array<float, 2> filter{};
    };

    struct Allpass {
        std::vector<float> line;
        std::size_t cursor = 0;

        void prepare(std::size_t frames);
        void clear() noexcept;
        float process(float input, float feedback) noexcept;
    };

    struct TankLine {
        std::vector<float> line;
        std::size_t cursor = 0;

        void prepare(std::size_t frames);
        void clear() noexcept;
        float read() const noexcept;
        void write(float value) noexcept;
    };

    static double clampParameter(std::uint32_t index, double value) noexcept;
    void applyEvent(const PluginEvent& event) noexcept;
    void renderSlice(const PluginProcessContext& context, std::uint32_t offset,
                     std::uint32_t frames) noexcept;
    void resetDsp() noexcept;
    void spawnGrain(double delayFrames, double grainFrames, int overlap,
                    Algorithm algorithm, double gravity, double pitch) noexcept;
    float readCapture(int channel, double position) const noexcept;
    double randomUnit() noexcept;
    double randomBipolar() noexcept { return randomUnit() * 2.0 - 1.0; }
    void publishPeak(std::atomic<float>& destination, float value) noexcept;

    PluginDescriptor m_descriptor;
    PluginListener* m_listener = nullptr;
    PluginBusLayout m_layout{{2, 2}, {2}};
    std::array<std::atomic<double>, kParameterCount> m_values;
    std::array<double, kParameterCount> m_smoothed{};

    double m_sampleRate = 48000.0;
    std::uint32_t m_maxBlockSize = 512;
    bool m_active = false;
    bool m_processing = false;

    std::array<std::vector<float>, 2> m_capture;
    std::size_t m_writeCursor = 0;
    std::array<Grain, 64> m_grains{};
    std::array<std::array<Allpass, 4>, 2> m_diffusers;
    std::array<std::array<TankLine, 4>, 2> m_tank;
    std::array<std::array<float, 4>, 2> m_tankDamping{};
    std::array<float, 2> m_feedbackSample{};
    std::array<float, 2> m_dampingState{};
    std::array<float, 2> m_lowcutInput{};
    std::array<float, 2> m_lowcutOutput{};
    double m_spawnCountdown = 0.0;
    double m_orbitPhase = 0.0;
    std::uint64_t m_processedSamples = 0;
    std::uint64_t m_rng = 0x9e3779b97f4a7c15ull;
    float m_detectorFast = 0.0f;
    float m_detectorSlow = 0.0f;
    float m_duckEnvelope = 0.0f;
    float m_transientPulseState = 0.0f;
    std::uint32_t m_transientCooldown = 0;

    std::atomic<bool> m_frozen{false};
    std::atomic<bool> m_clearRequested{false};
    std::atomic<int> m_lastPreset{0};
    std::atomic<std::uint64_t> m_grainSerial{0};
    std::atomic<float> m_inputPeakLeft{0.0f};
    std::atomic<float> m_inputPeakRight{0.0f};
    std::atomic<float> m_outputPeakLeft{0.0f};
    std::atomic<float> m_outputPeakRight{0.0f};
    std::atomic<float> m_fieldEnergy{0.0f};
    std::atomic<float> m_telemetryOrbit{0.0f};
    std::atomic<float> m_telemetryDuckGain{1.0f};
    std::atomic<float> m_telemetryTransient{0.0f};
    std::atomic<std::uint32_t> m_activeGrains{0};

    mutable std::mutex m_presetMutex;
    std::string m_presetKind{"factory"};
    std::string m_presetName{"WAVEFARERS"};
};

} // namespace daw::plugins::gravity
