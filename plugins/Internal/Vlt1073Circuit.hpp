#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace daw::plugins::vlt1073::circuit {
// Reduced, shared-feedback RC/RLC network. See docs/audio/vlt1073.md for
// the reduction, component provenance and the limits of the circuit claim.
// All memories are capacitor voltages / inductor currents. The trapezoidal
// companions expose a direct term and a history term, allowing the feedback
// node to be solved algebraically (no iterative audio-thread solver).
struct Rc {
    double memory = 0;
    double direct(double g) const noexcept { return g / (1 + g); }
    double history(double g) const noexcept { return memory / (1 + g); }
    double tick(double x, double g) noexcept {
        const double y = direct(g) * x + history(g);
        memory = 2 * y - memory;
        if (std::abs(memory) < 1e-24) memory = 0;
        return y;
    }
};
struct Rlc {
    double current = 0, voltage = 0;
    double direct(double g, double damping) const noexcept { return damping * g / (1 + g * (g + damping)); }
    double history(double g, double damping) const noexcept { return damping * (current - g * voltage) / (1 + g * (g + damping)); }
    double tick(double x, double g, double damping) noexcept {
        const double i = (current + g * (x - voltage)) / (1 + g * (g + damping));
        const double v = voltage + g * i;
        current = 2 * i - current; voltage = 2 * v - voltage;
        if (std::abs(current) < 1e-24) current = 0;
        if (std::abs(voltage) < 1e-24) voltage = 0;
        return damping * i;
    }
};
struct EqSettings {
    // LF, presence, HF; conductances referred to the summing node.
    std::array<double, 3> boost{}, cut{}, g{};
    double damping = .8;
};
struct Eq {
    std::array<Rc, 2> low{}, high{};
    std::array<Rlc, 2> mid{};
    double tick(double x, const EqSettings& s) noexcept {
        const std::array<double, 3> forward{low[0].tick(x, s.g[0]), mid[0].tick(x, s.g[1], s.damping), x - high[0].tick(x, s.g[2])};
        const std::array<double, 3> direct{low[1].direct(s.g[0]), mid[1].direct(s.g[1], s.damping), 1 - high[1].direct(s.g[2])};
        const std::array<double, 3> history{low[1].history(s.g[0]), mid[1].history(s.g[1], s.damping), -high[1].history(s.g[2])};
        double source = x, conductance = 1;
        for (unsigned i = 0; i < 3; ++i) {
            source += s.boost[i] * forward[i] - s.cut[i] * history[i];
            conductance += s.cut[i] * direct[i];
        }
        const double y = source / conductance;
        low[1].tick(y, s.g[0]); mid[1].tick(y, s.g[1], s.damping); high[1].tick(y, s.g[2]);
        return y;
    }
};
// B182/C: series C, shunt L, series C. Source resistance and termination
// give a third-order Butterworth reduction of the terminated ladder.
struct HighPass {
    Rc first;
    Rlc resonator;
    double tick(double x, double g) noexcept {
        const double firstOut = x - first.tick(x, g);
        const double i = (resonator.current + g * (firstOut - resonator.voltage)) / (1 + g * (g + 1));
        const double v = resonator.voltage + g * i;
        const double y = firstOut - i - v;
        resonator.current = 2 * i - resonator.current;
        resonator.voltage = 2 * v - resonator.voltage;
        if (std::abs(resonator.current) < 1e-24) resonator.current = 0;
        if (std::abs(resonator.voltage) < 1e-24) resonator.voltage = 0;
        return y;
    }
};
inline double sigmoid(double x) noexcept { return x / std::sqrt(1 + x * x); }
// Third-order emitter-degeneration expansion plus a C2 collector-rail knee.
// Bias expresses single-ended asymmetry. The reduced headroom and Early-effect
// coefficients are assumptions, not measured BC184C / 2N3055 device curves.
inline double classA(double volts, double headroom, double bias) noexcept {
    const double x = volts + bias;
    const double magnitude = std::abs(x);
    const double t = std::clamp((magnitude / headroom - .8) / .4, 0., 1.);
    const double knee = .8 + .4 * (t - t*t*t + .5*t*t*t*t);
    const double limited = std::copysign(magnitude < headroom * .8 ? magnitude : headroom * knee, x);
    constexpr double early = .001;
    const double h2 = headroom * headroom;
    // Factor the difference of cubes so zero input cancels exactly; separate
    // biased terms otherwise introduce round-off DC into a silent channel.
    return (limited-bias) * (1-early*(limited*limited+limited*bias+bias*bias)/h2)
        / (1-3*early*bias*bias/h2);
}
struct Transformer {
    Rc flux;
    double magnetization = 0;
    double tick(double volts, double g, double strength, double coercivity) noexcept {
        const double field = flux.tick(volts, g);
        // A bounded play operator supplies remanence and a minor hysteresis
        // loop. The reversible branch has unit slope at the origin.
        magnetization = std::clamp(magnetization, field - coercivity, field + coercivity);
        const double fieldEffective = field + .12 * magnetization;
        const double compressed = 14 * sigmoid(fieldEffective / 14);
        return volts + strength * (compressed - fieldEffective);
    }
};
} // namespace daw::plugins::vlt1073::circuit
