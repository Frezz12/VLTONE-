#pragma once

#include "Core/Result.hpp"
#include "model/Document.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace daw::analysis {

enum class DetectionStatus : std::uint8_t {
    Unavailable = 0,
    Ambiguous = 1,
    Available = 2,
};

struct TempoEstimate {
    int algorithmVersion = 2;
    DetectionStatus status = DetectionStatus::Unavailable;
    double bpm = 0.0;
    double confidence = 0.0;
    double stability = 0.0;
    std::vector<double> alternatives;
    bool variable = false;
    std::string reason;
    std::string backend = "dsp";
    bool calibrated = false;
    /// Uncalibrated evidence, retained for reproducible offline calibration.
    double evidence = 0.0;

    bool highConfidence() const noexcept;
};

struct KeyEstimate {
    int algorithmVersion = 2;
    DetectionStatus status = DetectionStatus::Unavailable;
    int root = -1;                 // pitch class, C = 0
    std::string scale;             // "major", "natural_minor", or empty
    double confidence = 0.0;
    int alternateRoot = -1;
    std::string alternateScale;
    double tuningCents = 0.0;
    std::string reason;
    std::string backend = "hpcp";
    bool calibrated = false;
    bool variable = false;
    double evidence = 0.0;
    /// Canonical C-based order: 12 major classes followed by 12 minor classes.
    std::vector<double> profileScores;
    std::vector<double> neuralScores;

    bool highConfidence() const noexcept;
};

struct MusicalAnalysisResult {
    static constexpr int kAlgorithmVersion = 2;
    int algorithmVersion = kAlgorithmVersion;
    TempoEstimate tempo;
    KeyEstimate key;
    double analyzedSeconds = 0.0;
};

struct MusicalAnalysisRequest {
    bool detectTempo = true;
    bool detectKey = true;
    /// Source-file range. A non-positive duration means through EOF.
    double offsetSeconds = 0.0;
    double durationSeconds = 0.0;
    /// Playback transformations that change the musical answer without
    /// changing the source samples we inspect.
    double stretchTime = 1.0;
    double pitchShiftSemitones = 0.0;
    /// Kept for source compatibility. Version 2 never uses filename hints.
    std::string fileNameHint;
    /// Deterministic DSP-only execution for diagnostics and fallback testing.
    bool useNeuralModels = true;
};

/// Return false from progress to cancel. Progress is 0...1 and the phase is a
/// stable English identifier suitable for a translated UI label.
using AnalysisProgress =
    std::function<bool(double progress, std::string_view phase)>;

audio::Result analyzeAudioFile(const std::string& path,
                               const MusicalAnalysisRequest& request,
                               MusicalAnalysisResult& out,
                               const AnalysisProgress& progress = {});

/// Pure in-memory entry point used by deterministic tests and by future
/// render-before-analysis paths. Samples are interleaved.
audio::Result analyzeAudioSamples(const float* interleaved,
                                  std::size_t frames, int channels,
                                  double sampleRate,
                                  const MusicalAnalysisRequest& request,
                                  MusicalAnalysisResult& out,
                                  const AnalysisProgress& progress = {});

std::string pitchClassName(int root);
std::string keyDisplayName(const KeyEstimate& key);
std::string camelotName(int root, const std::string& scale);

/// Shared confidence policy for stored clip results and every application path.
bool highConfidence(const ClipTempoAnalysisModel& tempo) noexcept;
bool highConfidence(const ClipKeyAnalysisModel& key) noexcept;

/// The one rounding policy for every detected-tempo display and application.
/// Returns 0 for non-finite, non-positive or unrepresentable input.
int roundedBpm(double bpm) noexcept;
/// Rounded, unique candidates that can be applied to the project (1...999).
std::vector<int> applicableTempos(const TempoEstimate& tempo);

ClipMusicalAnalysisModel toClipAnalysisModel(
    const MusicalAnalysisResult& result,
    const MusicalAnalysisRequest& request);

} // namespace daw::analysis
