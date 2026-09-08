#pragma once
#include "AudioMusicalAnalysis.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <exception>
#include <numbers>
#include <numeric>
#include <span>
#include <vector>

namespace daw::analysis::detail {
inline constexpr double kAnalysisRate = 22050.0;
inline constexpr double kEpsilon = 1e-12;
struct Cancelled final : std::exception {};
inline void checkpoint(const AnalysisProgress& progress, double amount, std::string_view phase) {
    if (progress && !progress(std::clamp(amount, 0.0, 1.0), phase)) throw Cancelled{};
}
inline AnalysisProgress subProgress(const AnalysisProgress& p, double from, double to) {
    return [p, from, to](double amount, std::string_view phase) {
        return !p || p(from + std::clamp(amount, 0.0, 1.0) * (to - from), phase);
    };
}
inline void fft(std::vector<std::complex<double>>& values) {
    const std::size_t n = values.size();
    if (n < 2) return;
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(values[i], values[j]);
    }
    for (std::size_t length = 2; length <= n; length <<= 1) {
        const double angle = -2.0 * std::numbers::pi / double(length);
        const std::complex<double> step(std::cos(angle), std::sin(angle));
        for (std::size_t base = 0; base < n; base += length) {
            std::complex<double> phase(1.0, 0.0);
            const std::size_t half = length >> 1;
            for (std::size_t j = 0; j < half; ++j) {
                const auto even = values[base + j];
                const auto odd = values[base + j + half] * phase;
                values[base + j] = even + odd;
                values[base + j + half] = even - odd;
                phase *= step;
            }
        }
    }
}

inline std::vector<double> hann(std::size_t size) {
    std::vector<double> window(size);
    if (size < 2) return window;
    for (std::size_t i = 0; i < size; ++i) {
        window[i] = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * double(i) /
                                        double(size - 1));
    }
    return window;
}

inline double median(std::vector<double> values) {
    if (values.empty()) return 0.0;
    const std::size_t mid = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + std::ptrdiff_t(mid),
                     values.end());
    double result = values[mid];
    if ((values.size() & 1u) == 0) {
        const auto lower = std::max_element(values.begin(),
                                            values.begin() + std::ptrdiff_t(mid));
        result = (result + *lower) * 0.5;
    }
    return result;
}

inline void robustNormalize(std::vector<double>& values) {
    if (values.empty()) return;
    const double center = median(values);
    std::vector<double> deviations(values.size());
    std::transform(values.begin(), values.end(), deviations.begin(),
                   [center](double value) { return std::abs(value - center); });
    const double scale = std::max(1e-8, median(std::move(deviations)) * 1.4826);
    for (double& value : values)
        value = std::clamp((value - center) / scale, 0.0, 8.0);
}


inline double peakOffset(double left, double centre, double right) {
    const double curvature = left - 2.0 * centre + right;
    return curvature < -1e-12 ? std::clamp(0.5 * (left - right) / curvature, -0.5, 0.5) : 0.0;
}
inline double sampleAt(std::span<const double> values, double position) {
    if (position < 0 || position >= double(values.size())) return 0.0;
    const auto i = std::size_t(position);
    const double f = position - double(i);
    return values[i] * (1.0 - f) + (i + 1 < values.size() ? values[i + 1] : values[i]) * f;
}
TempoEstimate detectTempo(std::span<const float>, const MusicalAnalysisRequest&, const AnalysisProgress&);
KeyEstimate detectKey(std::span<const float>, const MusicalAnalysisRequest&, const AnalysisProgress&);
} // namespace daw::analysis::detail
