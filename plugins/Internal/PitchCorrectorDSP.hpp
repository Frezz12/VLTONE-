#pragma once

#include <cstdint>
#include <memory>

namespace daw::plugins::pitch {

inline constexpr double kDefaultRetuneMs = 20;
inline constexpr double kDefaultTune = 68.3772233983162; // 100 * (1 - sqrt(20 / 200)).

// Keep the original normalized `tune` parameter in projects/automation. Only
// its presentation is in milliseconds; 0 ms still retains the anti-click ramp.
double retuneMilliseconds(double tune) noexcept;
double tuneFromMilliseconds(double milliseconds) noexcept;

struct Settings {
    double tune = kDefaultTune, humanize = 0, vibrato = 0;
    double a4Hz = 440, amount = 100, outputDb = 0;
    int key = 0, scale = 0, voice = 0;
    std::uint16_t noteMask = 0x0fff;
    bool formants = true;
};

struct Telemetry {
    double inputHz = 0, targetHz = 0, confidence = 0, correctionCents = 0;
    double inputLevel = 0, outputLevel = 0;
    bool voiced = false;
    std::uint64_t serial = 0;
};

// A monophonic vocal corrector. Stereo channels share detection and splice
// positions. prepare() owns every allocation; process() and reset() do not.
class PitchCorrectorDSP {
public:
    PitchCorrectorDSP();
    ~PitchCorrectorDSP();
    PitchCorrectorDSP(const PitchCorrectorDSP&) = delete;
    PitchCorrectorDSP& operator=(const PitchCorrectorDSP&) = delete;
    void prepare(double sampleRate, std::uint32_t maxBlock,
                 std::uint32_t channels, int quality);
    void reset() noexcept;
    void process(const float* const* input, float* const* output,
                 std::uint32_t frames, const Settings&) noexcept;
    std::uint32_t latencySamples() const noexcept;
    std::uint32_t tailSamples() const noexcept;
    Telemetry telemetry() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace daw::plugins::pitch
