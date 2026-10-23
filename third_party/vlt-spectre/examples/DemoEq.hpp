// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <array>
#include <cmath>

// A real peaking filter ONLY for the demonstration. The analyzer library owns no
// EQ: a host supplies its own before/after samples, including third-party plugins.
// Coefficient equations: https://www.w3.org/TR/audio-eq-cookbook/#peaking-eqn
class DemoEq {
public:
    void setParameters(double frequencyHz, double gainDb, double q) {
        targetFrequency = std::clamp(frequencyHz, 20.0, sampleRate * .45);
        targetGain = std::clamp(gainDb, -18.0, 18.0);
        targetQ = std::clamp(q, .2, 8.0);
    }
    void reset(double rate) {
        sampleRate = rate;
        targetFrequency = std::min(targetFrequency, sampleRate * .45);
        frequency = targetFrequency; gain = targetGain; quality = targetQ;
        state = {}; countdown = 0; updateCoefficients();
    }
    std::array<float, 2> process(float left, float right) {
        // Smooth parameters on a fixed sample clock; UI event timing cannot cause
        // a coefficient jump. Recalculating every 32 samples keeps this demo small.
        if (countdown-- == 0) {
            countdown = 31;
            const double amount = 1.0 - std::exp(-32.0 / (sampleRate * .03));
            frequency += amount * (targetFrequency - frequency);
            gain += amount * (targetGain - gain);
            quality += amount * (targetQ - quality);
            updateCoefficients();
        }
        std::array<float, 2> result{};
        const double input[]{left, right};
        for (std::size_t channel = 0; channel < 2; ++channel) {
            const double output = b0 * input[channel] + state[channel][0];
            state[channel][0] = b1 * input[channel] - a1 * output + state[channel][1];
            state[channel][1] = b2 * input[channel] - a2 * output;
            result[channel] = float(output);
        }
        return result;
    }
private:
    void updateCoefficients() {
        const double omega = 6.283185307179586 * frequency / sampleRate;
        const double alpha = std::sin(omega) / (2.0 * quality);
        const double amplitude = std::pow(10.0, gain / 40.0);
        const double a0 = 1.0 + alpha / amplitude;
        b0 = (1.0 + alpha * amplitude) / a0;
        b1 = a1 = -2.0 * std::cos(omega) / a0;
        b2 = (1.0 - alpha * amplitude) / a0;
        a2 = (1.0 - alpha / amplitude) / a0;
    }
    double sampleRate = 48000;
    double targetFrequency = 6000, targetGain = 6, targetQ = 1;
    double frequency = 6000, gain = 6, quality = 1;
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    std::array<std::array<double, 2>, 2> state{};
    unsigned countdown = 0;
};
