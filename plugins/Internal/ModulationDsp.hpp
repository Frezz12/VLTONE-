#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

namespace daw::plugins::modulation::dsp {
inline constexpr double pi = std::numbers::pi_v<double>;
inline double pole(double seconds, double sampleRate) noexcept {
    return std::exp(-1.0 / (seconds * sampleRate));
}
inline double clean(double x) noexcept {
    return std::isfinite(x) ? x : 0.0;
}
inline double flush(double x) noexcept {
    return std::abs(x) < 1.e-24 ? 0.0 : x;
}

// The write head denotes the current sample. Four-point Lagrange reads use
// floor(read)-1 ... floor(read)+2; callers keep at least three samples of delay.
class Delay {
  public:
    void prepare(double rate, double seconds = 0.05) {
        m_data.assign(std::size_t(std::ceil(rate * seconds)) + 8, 0.0);
        m_cursor = 0;
    }
    void reset() noexcept {
        std::fill(m_data.begin(), m_data.end(), 0.0);
        m_cursor = 0;
    }
    void write(double x) noexcept { m_data[m_cursor] = x; }
    void advance() noexcept {
        if (++m_cursor == m_data.size())
            m_cursor = 0;
    }
    double read(double delay) const noexcept {
        double pos = double(m_cursor) - std::clamp(delay, 3.0, double(m_data.size() - 4));
        if (pos < 0)
            pos += double(m_data.size());
        const auto i = std::int64_t(pos);
        const double t = pos - double(i);
        const auto at = [&](std::int64_t n) {
            if (n < 0)
                n += std::int64_t(m_data.size());
            if (n >= std::int64_t(m_data.size()))
                n -= std::int64_t(m_data.size());
            return m_data[std::size_t(n)];
        };
        return -t * (t - 1) * (t - 2) / 6 * at(i - 1) + (t + 1) * (t - 1) * (t - 2) / 2 * at(i) -
               (t + 1) * t * (t - 2) / 2 * at(i + 1) + (t + 1) * t * (t - 1) / 6 * at(i + 2);
    }

  private:
    std::vector<double> m_data;
    std::size_t m_cursor = 0;
};

// Two cascaded one-poles are non-resonant even while cutoff is moving.
struct Tone {
    double low = 0, high1 = 0, high2 = 0;
    double hp = 0, lp = 0;
    void tune(double lowHz, double highHz, double rate) noexcept {
        hp = lowHz > 0 ? std::exp(-2 * pi * lowHz / rate) : 1.0;
        lp = std::exp(-2 * pi * std::min(highHz, rate * 0.45) / rate);
    }
    double process(double x) noexcept {
        low = flush(hp * low + (1 - hp) * x);
        x -= low;
        high1 = flush(lp * high1 + (1 - lp) * x);
        high2 = flush(lp * high2 + (1 - lp) * high1);
        return high2;
    }
    void reset() noexcept { low = high1 = high2 = 0; }
};

struct Wander {
    std::uint32_t seed = 1;
    double from = 0, to = 0, phase = 0, step = 0;
    double random() noexcept {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return double(seed) / 4294967295.0;
    }
    void reset(std::uint32_t initial, double rate) noexcept {
        seed = initial;
        phase = 0;
        from = random() * 2 - 1;
        to = random() * 2 - 1;
        step = 1.0 / (rate * (.7 + 1.8 * random()));
    }
    double next(double rate) noexcept {
        phase += step;
        if (phase >= 1) {
            phase -= 1;
            from = to;
            to = random() * 2 - 1;
            step = 1.0 / (rate * (.7 + 1.8 * random()));
        }
        // Quintic interpolation: zero velocity and acceleration at each knot.
        const double s = phase * phase * phase * (phase * (phase * 6 - 15) + 10);
        return from + (to - from) * s;
    }
};

struct Allpass {
    double z = 0;
    double process(double x, double a) noexcept {
        const double y = a * x + z;
        z = flush(x - a * y);
        return y;
    }
};
} // namespace daw::plugins::modulation::dsp
