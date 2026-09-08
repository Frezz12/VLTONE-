#pragma once
#include "AudioMusicalAnalysis.hpp"
#include <array>
#include <filesystem>
#include <span>
#include <vector>

namespace daw::analysis::detail {
struct BeatActivations {
    double rate = 50.0;
    std::vector<double> beats;
    std::vector<double> downbeats;
};

std::filesystem::path analysisAssetDirectory();
BeatActivations neuralBeats(std::span<const float> mono, const AnalysisProgress& progress);
/// Empty on unsupported short input or unavailable model. C-based major/minor.
std::vector<double> neuralKey(std::span<const float> mono, const AnalysisProgress& progress);
/// Same frontend as the original Beat This! implementation, exposed for parity tests.
std::vector<float> beatLogMel(std::span<const float> mono, const AnalysisProgress& progress = {});
} // namespace daw::analysis::detail
