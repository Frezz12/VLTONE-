#pragma once
#include <array>
#include <algorithm>
#include <cstddef>

namespace daw::engine::dsp {
// Compile-time oscillator recurrence: no lazy allocation, trigonometry or
// static-initialization lock on the audio thread. The interpolated window has
// < 1.5e-7 absolute error and preserves Hann's complementary half-periods.
inline constexpr auto kHannWindow = [] {
    std::array<double, 4097> table{};
    constexpr double cosine = 0.99999882345170190993;
    constexpr double sine = 0.00153398018628476561;
    double real = 1.0, imaginary = 0.0;
    for (std::size_t i = 0; i < 2048; ++i) {
        table[i] = (1.0 - real) * 0.5;
        table[i + 2048] = 1.0 - table[i];
        const double next = real * cosine - imaginary * sine;
        imaginary = imaginary * cosine + real * sine;
        real = next;
    }
    table[0] = table[4096] = 0.0;
    table[2048] = 1.0;
    return table;
}();

inline double hannWindow(double phase) noexcept {
    if (phase <= 0.0 || phase >= 1.0) return 0.0;
    const double position = phase * 4096.0;
    const auto index = static_cast<std::size_t>(position);
    return kHannWindow[index] + (kHannWindow[index + 1] - kHannWindow[index]) *
                                   (position - double(index));
}
} // namespace daw::engine::dsp
