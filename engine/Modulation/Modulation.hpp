#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include "DSP/Curve.hpp"
#include <string>
#include <utility>
#include <vector>

namespace daw::modulation {
enum class Kind { Lfo = 0, Follower = 1, Shaper = 2, Steps = 3, Random = 4 };
enum class Wave { Sine, Triangle, Up, Down, Square };
enum class Tap { BeforeEffects, AfterEffects, AfterFader };
enum class Direction { Forward, Reverse, PingPong };
struct ShapePoint {
    double x = 0, y = 0, bend = 0;
    engine::curve::Shape shape = engine::curve::Shape::Linear;
    friend bool operator==(const ShapePoint&, const ShapePoint&) = default;
};
inline bool hasRate(Kind kind) noexcept { return kind != Kind::Follower; }
inline const char* kindName(Kind kind) noexcept {
    switch (kind) {
    case Kind::Lfo: return "LFO";
    case Kind::Follower: return "Envelope Follower";
    case Kind::Shaper: return "Shaper";
    case Kind::Steps: return "Steps";
    case Kind::Random: return "Random";
    }
    return "LFO";
}
struct Source {
    std::string id, name;
    std::string ownerSlotId; // Presentation owner; routes may still target other devices.
    Kind kind = Kind::Lfo;
    bool enabled = true;
    Wave wave = Wave::Sine;
    bool sync = true;
    double rate = 2.0, beats = 1.0, phase = 0.0, smoothMs = 2.0;
    std::string audioChannelId; // Empty means the owning Rack's pre-FX input.
    Tap tap = Tap::AfterEffects;
    double gainDb = 0.0, attackMs = 10.0, releaseMs = 150.0;
    bool filter = false;
    double lowHz = 20.0, highHz = 20000.0;
    std::vector<ShapePoint> points{{0, 0}, {1, 1}};
    std::vector<double> steps{1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0};
    Direction direction = Direction::Forward;
    std::uint32_t seed = 1;
    unsigned randomLoop = 0;
    bool randomSmooth = false;
    friend bool operator==(const Source&, const Source&) = default;
};
struct Route {
    std::string id, sourceId, slotId, pluginUid, pluginFormat, parameterId;
    double depth = 0.2;
    bool bipolar = true, enabled = true;
    friend bool operator==(const Route&, const Route&) = default;
};
struct Rack {
    bool enabled = true;
    std::vector<Source> sources;
    std::vector<Route> routes;
    friend bool operator==(const Rack&, const Rack&) = default;
};
// Curves use timeline beats and plain values, prepared off the audio thread.
struct Curve {
    double fallback = 0.0;
    bool automated = false;
    std::vector<std::pair<double, double>> points;
    std::vector<double> areas;
    double origin = 0.0;
    // Prefix areas make phase evaluation O(log points), with no allocation in process().
    void prepare() {
        std::stable_sort(points.begin(), points.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        std::size_t count = 0;
        for (const auto& p : points) {
            if (count && points[count - 1].first == p.first) points[count - 1] = p;
            else points[count++] = p;
        }
        points.resize(count); areas.resize(count); origin = 0;
        if (!count) return;
        areas[0] = points[0].first * fallback;
        for (std::size_t i = 1; i < count; ++i)
            areas[i] = areas[i - 1] + (points[i - 1].second + points[i].second) * .5 * (points[i].first - points[i - 1].first);
        origin = primitive(0);
    }
    double at(double beat) const noexcept {
        const auto right = std::upper_bound(points.begin(), points.end(), beat,
            [](double b, const auto& p) { return b < p.first; });
        if (right == points.begin()) return fallback;
        const auto& left = *(right - 1);
        if (right == points.end() || right->first <= left.first) return left.second;
        return std::lerp(left.second, right->second, (beat - left.first) / (right->first - left.first));
    }
    double integral(double beat) const noexcept { return primitive(beat) - origin; }
private:
    double primitive(double beat) const noexcept {
        const auto right = std::upper_bound(points.begin(), points.end(), beat,
            [](double b, const auto& p) { return b < p.first; });
        if (right == points.begin()) return fallback * beat;
        const auto i = std::size_t(right - points.begin() - 1);
        const auto& left = points[i];
        const double distance = beat - left.first;
        const double slope = right == points.end() ? 0 : (right->second - left.second) / (right->first - left.first);
        return areas[i] + left.second * distance + .5 * slope * distance * distance;
    }
};
inline double waveValue(Wave wave, double phase) noexcept {
    phase -= std::floor(phase);
    switch (wave) {
    case Wave::Sine: return 0.5 + 0.5 * std::sin(6.2831853071795864769 * phase);
    case Wave::Triangle: return 1.0 - std::abs(2.0 * phase - 1.0);
    case Wave::Up: return phase;
    case Wave::Down: return 1.0 - phase;
    case Wave::Square: return phase < 0.5 ? 1.0 : 0.0;
    }
    return 0.5;
}
inline double wrap(double value, double period = 1) noexcept {
    return value - std::floor(value / period) * period;
}
inline unsigned stepIndex(const Source& s, double clock) noexcept {
    const auto count = unsigned(s.steps.size());
    if (count <= 1) return 0;
    const auto cycle = s.direction == Direction::PingPong ? 2 * count - 2 : count;
    auto index = unsigned(wrap(std::floor(clock), cycle));
    if (s.direction == Direction::Reverse) index = count - 1 - index;
    else if (index >= count) index = cycle - index;
    return index;
}
inline double randomValue(std::uint32_t seed, double index, unsigned loop = 0) noexcept {
    // Counter-based hashing makes seeking, negative preroll and block partitioning deterministic.
    const auto signedIndex = std::int64_t(loop ? wrap(index, loop) : std::clamp(index, -9e15, 9e15));
    auto bits = std::uint64_t(signedIndex) + (std::uint64_t(seed) << 32) + 0x9e3779b97f4a7c15ULL;
    bits = (bits ^ (bits >> 30)) * 0xbf58476d1ce4e5b9ULL;
    bits = (bits ^ (bits >> 27)) * 0x94d049bb133111ebULL;
    bits ^= bits >> 31;
    return double(bits >> 11) * (1.0 / 9007199254740992.0);
}
inline double sourceValue(const Source& s, double clock) noexcept {
    if (!std::isfinite(clock)) return 0;
    switch (s.kind) {
    case Kind::Lfo: return waveValue(s.wave, clock);
    case Kind::Shaper: {
        if (s.points.size() < 2) return 0;
        const double phase = wrap(clock);
        const auto right = std::upper_bound(s.points.begin(), s.points.end(), phase,
            [](double x, const auto& point) { return x < point.x; });
        if (right == s.points.begin()) return right->y;
        if (right == s.points.end()) return s.points.back().y;
        const auto& left = *(right - 1);
        const double t = (phase - left.x) / (right->x - left.x);
        return std::lerp(left.y, right->y, engine::curve::shapeT(t, left.shape, left.bend));
    }
    case Kind::Steps: return s.steps.empty() ? 0 : s.steps[stepIndex(s, clock)];
    case Kind::Random: {
        const double index = std::floor(clock), a = randomValue(s.seed, index, s.randomLoop);
        if (!s.randomSmooth) return a;
        const double t = wrap(clock);
        return std::lerp(a, randomValue(s.seed, index + 1, s.randomLoop), t * t * (3 - 2 * t));
    }
    case Kind::Follower: return 0;
    }
    return 0;
}
inline double sourcePosition(const Source& s, double clock) noexcept {
    if (!std::isfinite(clock)) return 0;
    if (s.kind == Kind::Steps && !s.steps.empty())
        return (stepIndex(s, clock) + .5) / s.steps.size();
    return wrap(clock);
}
inline void shapePreset(Source& s, unsigned preset) {
    using S = engine::curve::Shape;
    switch (preset) {
    case 1: s.points = {{0, 1}, {1, 0}}; break;
    case 2: s.points = {{0, 0, 0, S::SCurve}, {1, 1}}; break;
    case 3: s.points = {{0, 1, 0, S::Hold}, {.5, 0, 0, S::Hold}, {1, 0}}; break;
    case 4: s.points = {{0, 1, .65}, {.7, 0}, {1, 0}}; break;
    default: s.points = {{0, 0}, {1, 1}}; break;
    }
}
inline void euclideanSteps(Source& s, unsigned pulses, unsigned rotation) {
    const auto count = unsigned(s.steps.size());
    if (!count) return;
    pulses = std::min(pulses, count);
    for (unsigned i = 0; i < count; ++i)
        s.steps[(i + rotation) % count] = (i * pulses) % count < pulses ? 1 : 0;
}
inline bool valid(const Source& s) noexcept {
    const auto between = [](double v, double lo, double hi) { return std::isfinite(v) && v >= lo && v <= hi; };
    if (s.points.size() < 2 || s.points.size() > 64 || s.points.front().x != 0 || s.points.back().x != 1 ||
        s.steps.empty() || s.steps.size() > 32 || int(s.direction) < 0 || int(s.direction) > 2 ||
        (s.randomLoop != 0 && s.randomLoop != 2 && s.randomLoop != 4 && s.randomLoop != 8 && s.randomLoop != 16 && s.randomLoop != 32)) return false;
    double previous = -1;
    for (const auto& p : s.points) {
        if (!between(p.x, 0, 1) || p.x <= previous || !between(p.y, 0, 1) || !between(p.bend, -1, 1) ||
            int(p.shape) < 0 || int(p.shape) > 2) return false;
        previous = p.x;
    }
    for (const auto v : s.steps) if (!between(v, 0, 1)) return false;
    return !s.id.empty() && s.id.size() <= 128 && s.name.size() <= 256 && s.ownerSlotId.size() <= 128 &&
        int(s.kind) >= 0 && int(s.kind) <= 4 && int(s.wave) >= 0 && int(s.wave) <= 4 &&
        int(s.tap) >= 0 && int(s.tap) <= 2 && between(s.rate, 0.01, 20.0) &&
        between(s.beats, 0.03125, 128.0) && between(s.phase, 0.0, 1.0) &&
        between(s.smoothMs, 0.0, 1000.0) && between(s.gainDb, -48.0, 48.0) &&
        between(s.attackMs, 0.1, 2000.0) && between(s.releaseMs, 1.0, 10000.0) &&
        between(s.lowHz, 10.0, 20000.0) && between(s.highHz, 20.0, 24000.0) && s.lowHz < s.highHz;
}
} // namespace daw::modulation
