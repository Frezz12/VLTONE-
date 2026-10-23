// SPDX-License-Identifier: MIT
#pragma once
#include <cstddef>
#include <vector>

namespace vlt::spectre {

/// A rounded VISUAL envelope, independent of Qt and of the measurement data.
/// Frequencies must be positive/increasing; levels must be finite. Invalid input
/// returns an empty vector. Width in octaves: 0 = exact bypass, 1/6 = rounded.
///
/// Broadens narrow peaks using parabolic shoulders, then rounds their joins. An
/// isolated, sufficiently sampled peak loses about 1/3 dB. Close peaks can merge;
/// use SpectrumFrame, not this envelope, for amplitude/frequency measurements.
/// No hard peak-detection threshold: slowly changing inputs cannot cause a jump.
/// Allocates output/scratch vectors; call on a UI/worker thread, never audio.
std::vector<float> smoothDisplaySpectrum(const float* frequencies, const float* levels,
                                         std::size_t count, double widthOctaves);

} // namespace vlt::spectre
