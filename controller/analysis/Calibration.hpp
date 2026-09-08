#pragma once
#include "AudioMusicalAnalysis.hpp"
#include <string_view>

namespace daw::analysis::detail {
/// Frozen calibration artifacts are separate for each task and fallback backend.
void calibrate(TempoEstimate& estimate);
void calibrate(KeyEstimate& estimate);
double keyModelWeight();
} // namespace daw::analysis::detail
