#pragma once

#include "Host/PluginInstance.hpp"

#include <array>
#include <atomic>
#include <complex>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace daw::plugins::equalizer {

enum class ProcessingMode : std::uint32_t { ZeroLatency = 0, AnalogPhase, LinearPhase };
enum class LinearResolution : std::uint32_t { Low = 0, Medium, High };
enum class FilterType : std::uint32_t {
    Bell = 0,
    LowShelf,
    HighShelf,
    LowCut,
    HighCut,
    Notch,
    BandPass,
    Tilt,
    AllPass,
};
enum class Slope : std::uint32_t {
    Db6 = 0, Db12, Db18, Db24, Db36, Db48, Db72, Db96,
};
enum class Placement : std::uint32_t { Stereo = 0, Left, Right, Mid, Side };
enum class DetectorMode : std::uint32_t { Band = 0, Free };

enum class GlobalParam : std::uint32_t {
    ProcessingMode = 0,
    LinearResolution,
    OutputGain,
    OutputBalance,
    PolarityInvert,
    GainScale,
    AutoGain,
    Count,
};

enum class BandParam : std::uint32_t {
    Enabled = 0,
    Type,
    Frequency,
    Gain,
    Q,
    Slope,
    Placement,
    DynamicEnabled,
    DynamicRange,
    DynamicAuto,
    DynamicThreshold,
    DynamicAttack,
    DynamicRelease,
    DynamicExternal,
    DetectorMode,
    DetectorLow,
    DetectorHigh,
    Count,
};

inline constexpr std::uint32_t kBandCount = 24;
inline constexpr std::uint32_t kGlobalParameterCount =
    std::uint32_t(GlobalParam::Count);
inline constexpr std::uint32_t kBandParameterCount =
    std::uint32_t(BandParam::Count);
inline constexpr std::uint32_t kParameterCount =
    kGlobalParameterCount + kBandCount * kBandParameterCount;
inline constexpr std::size_t kSpectrumBinCount = 128;

constexpr std::uint32_t globalParameter(GlobalParam parameter) noexcept {
    return std::uint32_t(parameter);
}

constexpr std::uint32_t bandParameter(std::uint32_t band,
                                      BandParam parameter) noexcept {
    return kGlobalParameterCount + band * kBandParameterCount +
           std::uint32_t(parameter);
}

struct BandState {
    bool enabled = false;
    FilterType type = FilterType::Bell;
    double frequency = 1000.0;
    double gainDb = 0.0;
    double q = 1.0;
    Slope slope = Slope::Db12;
    Placement placement = Placement::Stereo;
    bool dynamicEnabled = false;
    double dynamicRangeDb = 0.0;
    bool dynamicAuto = true;
    double thresholdDb = -24.0;
    double attackMs = 20.0;
    double releaseMs = 160.0;
    bool externalSidechain = false;
    DetectorMode detectorMode = DetectorMode::Band;
    double detectorLow = 20.0;
    double detectorHigh = 20000.0;
};

struct AnalyzerConfig {
    bool enabled = false;
    bool pre = true;
    bool post = true;
    bool sidechain = false;
    bool frozen = false;
    int speed = 1;
    double tiltDbPerOctave = 3.0;
};

struct Telemetry {
    std::array<float, kSpectrumBinCount> pre{};
    std::array<float, kSpectrumBinCount> post{};
    std::array<float, kSpectrumBinCount> sidechain{};
    std::array<float, kBandCount> dynamicGainDb{};
    float inputLeft = 0.0f;
    float inputRight = 0.0f;
    float outputLeft = 0.0f;
    float outputRight = 0.0f;
    bool sidechainPresent = false;
};

struct FactoryPreset {
    std::string_view name;
    std::array<double, kParameterCount> values{};
};

std::span<const ParameterInfo> parameterTable() noexcept;
std::string parameterText(std::uint32_t index, double value);
std::span<const FactoryPreset> factoryPresets() noexcept;
std::string parameterId(std::uint32_t index);

/// VLT's built-in 24-band parametric and dynamic equalizer.
class EqualizerInstance final : public PluginInstance {
public:
    EqualizerInstance();

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
    std::uint32_t latencySamples() const noexcept override;
    std::uint32_t tailSamples() const noexcept override { return 0; }

    BandState bandState(std::uint32_t band) const noexcept;
    double responseDb(double frequency) const noexcept;
    double bandResponseDb(std::uint32_t band, double frequency) const noexcept;

    void setAnalyzerConfig(const AnalyzerConfig& config) noexcept;
    AnalyzerConfig analyzerConfig() const noexcept;
    void setAuditionBand(int band) noexcept;
    Telemetry consumeTelemetry() noexcept;

    void captureComparison(char slot);
    std::array<double, kParameterCount> comparison(char slot) const;
    void copyComparison(char from, char to);
    void setActiveComparison(char slot) noexcept;
    char activeComparison() const noexcept;

    void setPresetReference(std::string kind, std::string name);
    std::pair<std::string, std::string> presetReference() const;

private:
    struct Coefficients {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    };
    struct Biquad {
        Coefficients c;
        double z1 = 0.0, z2 = 0.0;
        double process(double input) noexcept;
        void reset() noexcept { z1 = z2 = 0.0; }
    };
    struct FilterChain {
        std::array<Biquad, 8> sections{};
        std::uint32_t count = 0;
        double process(double input) noexcept;
        void reset() noexcept;
    };
    struct FirState {
        std::array<double, 33> history{};
        std::size_t cursor = 0;
        double push(double input, const std::array<double, 33>& coefficients) noexcept;
        void reset() noexcept { history = {}; cursor = 0; }
    };

    static double clampParameter(std::uint32_t index, double value) noexcept;
    static int slopeOrder(Slope slope) noexcept;
    static bool supportsDynamics(FilterType type) noexcept;
    static Coefficients design(FilterType type, double frequency, double q,
                               double gainDb, double sampleRate) noexcept;
    static void buildChain(FilterChain& chain, const BandState& band,
                           double dynamicGainDb, double gainScale,
                           double sampleRate) noexcept;
    static std::complex<double> coefficientResponse(const Coefficients& c,
                                                     double omega) noexcept;
    static double responseMagnitude(const BandState& band, double frequency,
                                    double dynamicGainDb, double gainScale,
                                    double sampleRate) noexcept;

    void applyEvent(const PluginEvent& event) noexcept;
    void renderSlice(const PluginProcessContext& context, std::uint32_t offset,
                     std::uint32_t frames) noexcept;
    void updateSmoothedParameters() noexcept;
    void updateFilterCoefficients(double sampleRate) noexcept;
    void updateDynamics(const PluginProcessContext& context, std::uint32_t frame,
                        double left, double right) noexcept;
    void processIirFrame(double& left, double& right, double sampleRate) noexcept;
    void processLinearFrame(double inputLeft, double inputRight,
                            double& outputLeft, double& outputRight) noexcept;
    void renderLinearTransform() noexcept;
    void fft(std::vector<std::complex<double>>& values, bool inverse) noexcept;
    void collectAnalyzer(double preLeft, double preRight, double postLeft,
                         double postRight, double sideLeft,
                         double sideRight) noexcept;
    void publishAnalyzerFrame() noexcept;
    void updateAutoGain() noexcept;
    void resetDsp() noexcept;
    std::size_t linearFftSize() const noexcept;

    PluginDescriptor m_descriptor;
    PluginListener* m_listener = nullptr;
    PluginBusLayout m_layout{{2, 2}, {2}};
    std::array<std::atomic<double>, kParameterCount> m_values;
    std::array<double, kParameterCount> m_targets{};
    std::array<double, kParameterCount> m_smoothed{};

    double m_sampleRate = 48000.0;
    std::uint32_t m_maxBlockSize = 512;
    bool m_active = false;
    bool m_processing = false;
    std::uint32_t m_controlCountdown = 0;
    std::uint32_t m_autoGainCountdown = 0;

    std::array<std::array<FilterChain, 2>, kBandCount> m_filters{};
    std::array<std::array<Biquad, 2>, kBandCount> m_detectors{};
    std::array<double, kBandCount> m_envelopes{};
    std::array<double, kBandCount> m_autoLevelsDb{};
    std::array<double, kBandCount> m_dynamicGainDb{};
    double m_autoGainDb = 0.0;

    std::array<double, 33> m_halfband{};
    std::array<std::array<FirState, 2>, 2> m_analogFir{};
    std::array<double, 2> m_analogPrevious{};

    std::size_t m_linearSize = 0;
    std::size_t m_linearHop = 0;
    std::size_t m_linearInputPosition = 0;
    std::size_t m_linearOutputPosition = 0;
    std::size_t m_linearHopCountdown = 0;
    std::uint64_t m_linearSamples = 0;
    std::array<std::vector<double>, 2> m_linearInput;
    std::array<std::vector<double>, 2> m_linearOutput;
    std::array<std::vector<std::complex<double>>, 2> m_linearSpectrum;
    std::vector<double> m_linearWindow;

    static constexpr std::size_t kAnalyzerSize = 2048;
    std::array<std::array<double, kAnalyzerSize>, 6> m_analyzerRing{};
    std::size_t m_analyzerPosition = 0;
    std::size_t m_analyzerCountdown = kAnalyzerSize / 2;
    std::array<std::atomic<float>, kSpectrumBinCount> m_preSpectrum{};
    std::array<std::atomic<float>, kSpectrumBinCount> m_postSpectrum{};
    std::array<std::atomic<float>, kSpectrumBinCount> m_sideSpectrum{};
    std::array<std::atomic<float>, kBandCount> m_dynamicTelemetry{};
    std::atomic<std::uint32_t> m_analyzerFlags{0};
    std::atomic<int> m_analyzerSpeed{1};
    std::atomic<double> m_analyzerTilt{3.0};
    std::atomic<int> m_auditionBand{-1};
    std::atomic<bool> m_sidechainPresent{false};
    std::atomic<float> m_inputPeakLeft{0.0f};
    std::atomic<float> m_inputPeakRight{0.0f};
    std::atomic<float> m_outputPeakLeft{0.0f};
    std::atomic<float> m_outputPeakRight{0.0f};
    std::vector<std::complex<double>> m_analyzerWork;

    mutable std::mutex m_stateMutex;
    std::array<double, kParameterCount> m_comparisonA{};
    std::array<double, kParameterCount> m_comparisonB{};
    char m_activeComparison = 'A';
    std::string m_presetKind{"factory"};
    std::string m_presetName{"Flat"};
};

} // namespace daw::plugins::equalizer
