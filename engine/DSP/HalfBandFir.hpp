#pragma once

#include <array>
#include <cmath>
#include <numbers>

namespace daw::engine::dsp {

/// Exact-zero even taps, unity DC gain. A nested 4x up/down round trip
/// contributes 48 original samples. Construct the taps on the control thread.
inline const std::array<double, 65>& halfBand65Taps() {
    static const auto taps = [] {
        std::array<double, 65> result{};
        double sum = 0;
        for (int i = 1; i < 65; i += 2) {
            const int x = i - 32;
            const double phase = 2 * std::numbers::pi * i / 64;
            result[unsigned(i)] = std::sin(std::numbers::pi * x / 2) /
                (std::numbers::pi * x) * (.42 - .5 * std::cos(phase) + .08 * std::cos(2 * phase));
            sum += result[unsigned(i)];
        }
        for (auto& tap : result) tap *= .5 / sum;
        result[32] = .5;
        return result;
    }();
    return taps;
}

struct HalfBandFir65 {
    // Mirrored ring avoids per-tap remainder operations. The multiply/add
    // order stays identical to the original CLA-2A implementation.
    std::array<double, 130> history{};
    unsigned cursor = 0;
    // Decimation still needs every input in the history, but only every
    // other convolution result. Advance discarded phases without computing it.
    void push(double input) noexcept {
        history[cursor] = input;
        history[cursor + 65] = input;
        if (++cursor == 65) cursor = 0;
    }
    double tick(double input, const std::array<double, 65>& taps) noexcept {
        const unsigned at = cursor;
        push(input);
        double result = .5 * history[at + 65 - 32];
        for (unsigned i = 1; i < 32; i += 2)
            result += taps[i] * (history[at + 65 - i] + history[at + 65 - (64 - i)]);
        return result;
    }
    // A dedicated upsampler alternates phase 0 (input * 2) and phase 1
    // (inserted zero), starting from a cleared history. On phase 0 every
    // noncentral tap multiplies an inserted zero, leaving just the delay.
    double upsample(double input, const std::array<double, 65>& taps, unsigned phase) noexcept {
        if (phase != 0) return tick(0, taps);
        const double result = .5 * history[cursor + 65 - 32];
        push(2 * input);
        return result;
    }
};
} // namespace daw::engine::dsp
