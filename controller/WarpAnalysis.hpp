#pragma once
#include "Audio/SampleBuffer.hpp"
#include <functional>
#include <vector>

namespace daw::analysis {
inline constexpr int kWarpAnalysisVersion = 2;
struct WarpTransient {
    double sourceSeconds = 0;
    double strength = 0;
    // Local spectral/energy evidence, not a calibrated probability.
    double confidence = 0;
};
std::vector<WarpTransient> detectWarpTransients(const engine::SampleBuffer& audio,
    double begin, double end, const std::function<bool()>& keepGoing = {});
}
