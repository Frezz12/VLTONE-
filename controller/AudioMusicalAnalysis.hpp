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
    DetectionStatus status = DetectionStatus::Unavailable;
    double bpm = 0.0;
    double confidence = 0.0;
    double stability = 0.0;
    std::vector<double> alternatives;
    bool variable = false;
    std::string reason;

    bool highConfidence() const noexcept {
        return status == DetectionStatus::Available && confidence >= 0.82 &&
               stability >= 0.85 && !variable;
    }
};

struct KeyEstimate {
    DetectionStatus status = DetectionStatus::Unavailable;
    int root = -1;                 // pitch class, C = 0
    std::string scale;             // "major", "natural_minor", or empty
    double confidence = 0.0;
    int alternateRoot = -1;
    std::string alternateScale;
    double tuningCents = 0.0;
    std::string reason;

    bool highConfidence() const noexcept {
        return status == DetectionStatus::Available && confidence >= 0.75;
    }
};

struct MusicalAnalysisResult {
    static constexpr int kAlgorithmVersion = 1;
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
    /// Optional weak prior, used only when a token agrees with audio evidence.
    std::string fileNameHint;
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

ClipMusicalAnalysisModel toClipAnalysisModel(
    const MusicalAnalysisResult& result,
    const MusicalAnalysisRequest& request);

} // namespace daw::analysis
