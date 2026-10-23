// SPDX-License-Identifier: MIT
#include <vlt/spectre/DisplaySmoothing.hpp>
#include <algorithm>
#include <cmath>

namespace vlt::spectre {
std::vector<float> smoothDisplaySpectrum(const float* frequencies, const float* levels,
                                         std::size_t count, double widthOctaves) {
    if (!frequencies || !levels || count == 0 || !std::isfinite(widthOctaves) ||
        widthOctaves < 0.0 || widthOctaves > 1.0) return {};
    std::vector<double> octaves(count);
    float previous = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (!std::isfinite(frequencies[i]) || frequencies[i] <= previous || !std::isfinite(levels[i])) return {};
        previous = frequencies[i];
        octaves[i] = std::log2(double(frequencies[i]));
    }
    if (widthOctaves == 0.0) return {levels, levels + count};

    // A rounded upper envelope gives a narrow measured peak visible shoulders.
    // At half the requested width a single peak falls by 12 dB. This is purely
    // presentation bandwidth: it never changes the FFT or the numeric readout.
    // Unlike restoring selected raw peaks, this rule has no prominence threshold
    // that can switch abruptly as a peak moves or changes level.
    const double curvature = 48.0 / (widthOctaves * widthOctaves);
    const auto bounds = std::minmax_element(levels, levels + count);
    const double radius = widthOctaves * std::sqrt(double(*bounds.second - *bounds.first) / 48.0);
    std::vector<float> envelope(count);
    std::size_t first = 0, last = 0;
    for (std::size_t i = 0; i < count; ++i) {
        while (first < i && octaves[i] - octaves[first] > radius) ++first;
        while (last + 1 < count && octaves[last + 1] - octaves[i] <= radius) ++last;
        double highest = levels[i];
        for (auto j = first; j <= last; ++j) {
            const double distance = octaves[j] - octaves[i];
            highest = std::max(highest, levels[j] - curvature * distance * distance);
        }
        envelope[i] = float(highest);
    }

    // Round the joins between neighbouring shoulders too. Averaging this envelope
    // (rather than the original narrow FFT spikes) keeps resonances visible.
    // A resolved isolated peak loses about 1/3 dB; nearby peaks may merge visually.
    const double sigma = widthOctaves / 12.0;
    const double roundingRadius = 3.0 * sigma;
    std::vector<float> rounded(count);
    first = last = 0;
    for (std::size_t i = 0; i < count; ++i) {
        while (first < i && octaves[i] - octaves[first] > roundingRadius) ++first;
        while (last + 1 < count && octaves[last + 1] - octaves[i] <= roundingRadius) ++last;
        double sum = 0, weightSum = 0;
        for (auto j = first; j <= last; ++j) {
            const double distance = (octaves[j] - octaves[i]) / sigma;
            const double weight = std::exp(-0.5 * distance * distance);
            sum += double(envelope[j]) * weight;
            weightSum += weight;
        }
        rounded[i] = float(sum / weightSum);
    }
    return rounded;
}
} // namespace vlt::spectre
