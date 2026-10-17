#pragma once
#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace daw::modulation {
enum class Kind { Lfo, Follower };
enum class Wave { Sine, Triangle, Up, Down, Square };
enum class Tap { BeforeEffects, AfterEffects, AfterFader };
struct Source {
    std::string id, name;
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
inline bool valid(const Source& s) noexcept {
    const auto between = [](double v, double lo, double hi) { return std::isfinite(v) && v >= lo && v <= hi; };
    return !s.id.empty() && s.id.size() <= 128 && s.name.size() <= 256 &&
        int(s.kind) >= 0 && int(s.kind) <= 1 && int(s.wave) >= 0 && int(s.wave) <= 4 &&
        int(s.tap) >= 0 && int(s.tap) <= 2 && between(s.rate, 0.01, 20.0) &&
        between(s.beats, 0.03125, 128.0) && between(s.phase, 0.0, 1.0) &&
        between(s.smoothMs, 0.0, 1000.0) && between(s.gainDb, -48.0, 48.0) &&
        between(s.attackMs, 0.1, 2000.0) && between(s.releaseMs, 1.0, 10000.0) &&
        between(s.lowHz, 10.0, 20000.0) && between(s.highHz, 20.0, 24000.0) && s.lowHz < s.highHz;
}
} // namespace daw::modulation
