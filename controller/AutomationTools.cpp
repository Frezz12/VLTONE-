#include "AutomationTools.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace daw::autotools {
namespace {

constexpr double kEps = 1e-9;

AutomationPoint at(double beats, double value,
                   AutomationSegment shape = AutomationSegment::Linear,
                   double curve = 0.0) {
    AutomationPoint p;
    p.beats = beats;
    p.value = value;
    p.shape = shape;
    p.curve = curve;
    return p;
}

Points finish(Points points) {
    normalizeAutomation(points);
    return points;
}

/// A cheap, stable integer hash — the whole random source for sample-and-hold.
/// A counter rather than a stream, so asking for cycle 40 costs the same as
/// asking for cycle 1 and a preview always matches the apply that follows it.
double randomAt(std::uint32_t seed, std::uint32_t index) {
    std::uint32_t h = seed * 0x9E3779B9u + index * 0x85EBCA6Bu;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return static_cast<double>(h) / 4294967295.0;   // 0…1
}

/// One cycle of a wave at `phase` (0…1), as −1 … 1. Sample-and-hold is absent:
/// it has no closed form and is generated cycle by cycle instead.
double waveAt(LfoWave wave, double phase) {
    phase -= std::floor(phase);
    switch (wave) {
        case LfoWave::Sine:
            return std::sin(2.0 * std::numbers::pi * phase);
        case LfoWave::Triangle:
            if (phase < 0.25) return 4.0 * phase;
            if (phase < 0.75) return 2.0 - 4.0 * phase;
            return 4.0 * phase - 4.0;
        case LfoWave::Saw:
            return 1.0 - 2.0 * phase;
        case LfoWave::Ramp:
            return 2.0 * phase - 1.0;
        case LfoWave::Square:
            return phase < 0.5 ? 1.0 : -1.0;
        case LfoWave::SampleHold:
            break;
    }
    return 0.0;
}

}   // namespace

std::string lfoWaveName(LfoWave wave) {
    switch (wave) {
        case LfoWave::Sine: return "Sine";
        case LfoWave::Triangle: return "Triangle";
        case LfoWave::Saw: return "Saw";
        case LfoWave::Ramp: return "Ramp";
        case LfoWave::Square: return "Square";
        case LfoWave::SampleHold: return "Sample & Hold";
    }
    return "Sine";
}

std::string lfoWaveId(LfoWave wave) {
    switch (wave) {
        case LfoWave::Sine: return "sine";
        case LfoWave::Triangle: return "triangle";
        case LfoWave::Saw: return "saw";
        case LfoWave::Ramp: return "ramp";
        case LfoWave::Square: return "square";
        case LfoWave::SampleHold: return "sample_hold";
    }
    return "sine";
}

LfoWave lfoWaveFromId(const std::string& id) {
    for (LfoWave wave : allLfoWaves()) {
        if (lfoWaveId(wave) == id) return wave;
    }
    return LfoWave::Sine;
}

const std::vector<LfoWave>& allLfoWaves() {
    static const std::vector<LfoWave> waves{
        LfoWave::Sine,   LfoWave::Triangle, LfoWave::Saw,
        LfoWave::Ramp,   LfoWave::Square,   LfoWave::SampleHold,
    };
    return waves;
}

Points lfo(const LfoSpec& spec) {
    Points out;
    const double start = std::max(0.0, spec.startBeats);
    const double length = spec.lengthBeats;
    if (!(length > 0.0)) return out;
    const double end = start + length;
    const double rate = std::max(1e-4, spec.rateBeats);
    const double amp = 0.5 * std::clamp(spec.depth, 0.0, 2.0);
    const double phase0 = spec.phase - std::floor(spec.phase);

    auto level = [&](double w) { return std::clamp(spec.center + amp * w, 0.0, 1.0); };
    auto phaseAt = [&](double beats) { return phase0 + (beats - start) / rate; };
    // The gap that carries a vertical jump. Small enough to be inaudible as a
    // ramp, wide enough that `normalizeAutomation` will not fuse the two points
    // back into one and turn the jump into a slope through the whole cycle.
    const double jump = std::min(1e-4, rate * 1e-3);

    switch (spec.wave) {
        case LfoWave::Sine:
        case LfoWave::Triangle: {
            const int steps = std::clamp(spec.stepsPerCycle, 2, 256);
            const double dt = rate / steps;
            const int count = static_cast<int>(std::floor(length / dt));
            for (int k = 0; k <= count; ++k) {
                const double t = start + k * dt;
                if (t > end + kEps) break;
                out.push_back(at(t, level(waveAt(spec.wave, phaseAt(t)))));
            }
            if (out.empty() || out.back().beats < end - kEps) {
                out.push_back(at(end, level(waveAt(spec.wave, phaseAt(end)))));
            }
            break;
        }
        case LfoWave::Square: {
            out.push_back(at(start, level(waveAt(spec.wave, phase0)),
                             AutomationSegment::Hold));
            // Half-cycle boundaries, indexed so an offset phase lands its first
            // edge in the right place rather than at the start.
            const double u0 = 2.0 * phase0;
            for (long long n = static_cast<long long>(std::floor(u0)) + 1;; ++n) {
                const double t = start + (n * 0.5 - phase0) * rate;
                if (t > end - kEps) break;
                const bool high = (n % 2 + 2) % 2 == 0;
                out.push_back(at(t, level(high ? 1.0 : -1.0), AutomationSegment::Hold));
            }
            break;
        }
        case LfoWave::Saw:
        case LfoWave::Ramp: {
            out.push_back(at(start, level(waveAt(spec.wave, phase0))));
            for (long long n = static_cast<long long>(std::floor(phase0)) + 1;; ++n) {
                const double t = start + (n - phase0) * rate;
                if (t > end - kEps) break;
                // The cycle ends where it ends, then jumps: two points a hair
                // apart, which is the only honest way to draw a discontinuity
                // on a curve made of breakpoints.
                const double before = spec.wave == LfoWave::Saw ? -1.0 : 1.0;
                const double after = spec.wave == LfoWave::Saw ? 1.0 : -1.0;
                if (t - jump > start + kEps) out.push_back(at(t - jump, level(before)));
                out.push_back(at(t, level(after)));
            }
            out.push_back(at(end, level(waveAt(spec.wave, phaseAt(end)))));
            break;
        }
        case LfoWave::SampleHold: {
            std::uint32_t index = 0;
            out.push_back(at(start, level(2.0 * randomAt(spec.seed, index++) - 1.0),
                             AutomationSegment::Hold));
            for (long long n = static_cast<long long>(std::floor(phase0)) + 1;; ++n) {
                const double t = start + (n - phase0) * rate;
                if (t > end - kEps) break;
                out.push_back(at(t, level(2.0 * randomAt(spec.seed, index++) - 1.0),
                                 AutomationSegment::Hold));
            }
            break;
        }
    }
    return finish(std::move(out));
}

Points ramp(double fromBeats, double toBeats, double fromValue, double toValue,
            AutomationSegment shape, double curve) {
    if (toBeats < fromBeats) {
        std::swap(fromBeats, toBeats);
        std::swap(fromValue, toValue);
    }
    Points out;
    out.push_back(at(fromBeats, fromValue, shape, curve));
    out.push_back(at(toBeats, toValue));
    return finish(std::move(out));
}

Points splice(Points points, const Range& range, const Points& replacement) {
    Points out;
    out.reserve(points.size() + replacement.size());
    for (const auto& p : points) {
        if (!range.contains(p.beats)) out.push_back(p);
    }
    for (const auto& p : replacement) out.push_back(p);
    return finish(std::move(out));
}

Points dragPoint(const Points& points, std::size_t index, double beats,
                 double value, double guardBeats) {
    if (index >= points.size()) return finish(points);

    const AutomationPoint origin = points[index];
    AutomationPoint moved = origin;
    moved.beats = std::max(0.0, beats);
    moved.value = std::clamp(value, 0.0, 1.0);

    Points result = points;
    result.erase(result.begin() + std::ptrdiff_t(index));
    // A dragged point owns the instant where it lands. Without this, stable
    // de-duplication could keep a neighbour and make the handle appear stuck.
    std::erase_if(result, [&](const AutomationPoint& point) {
        return std::abs(point.beats - moved.beats) < kEps;
    });

    if (moved.value < origin.value - kEps) {
        AutomationPoint anchor = origin;
        if (moved.beats <= origin.beats + kEps) {
            // A vertical or leftward pull has no horizontal distance in which
            // to reveal the ramp. Put the guard immediately before the landing
            // point at the value the handle had when it was picked up. This is
            // the point the user otherwise has to recreate by hand before
            // drawing a fade down.
            anchor.beats = std::max(0.0, moved.beats - std::max(guardBeats, kEps));
            anchor.value = origin.value;
            anchor.shape = AutomationSegment::Linear;
            anchor.curve = 0.0;
        }
        if (anchor.beats < moved.beats - kEps) result.push_back(anchor);
    }

    result.push_back(moved);
    return finish(std::move(result));
}

Points invert(Points points, const Range& range) {
    for (auto& p : points) {
        if (range.contains(p.beats)) p.value = 1.0 - p.value;
    }
    return finish(std::move(points));
}

Points scaleValues(Points points, double scale, double offset, double pivot,
                   const Range& range) {
    for (auto& p : points) {
        if (!range.contains(p.beats)) continue;
        p.value = pivot + (p.value - pivot) * scale + offset;
    }
    return finish(std::move(points));
}

Points quantizeTime(Points points, double gridBeats, const Range& range) {
    if (!(gridBeats > 0.0)) return finish(std::move(points));
    for (auto& p : points) {
        if (!range.contains(p.beats)) continue;
        p.beats = std::round(p.beats / gridBeats) * gridBeats;
    }
    return finish(std::move(points));
}

Points quantizeValues(Points points, int steps, const Range& range) {
    if (steps < 2) return finish(std::move(points));
    const double rungs = steps - 1;
    for (auto& p : points) {
        if (!range.contains(p.beats)) continue;
        p.value = std::round(p.value * rungs) / rungs;
    }
    return finish(std::move(points));
}

Points smooth(Points points, double amount, int passes, const Range& range) {
    amount = std::clamp(amount, 0.0, 1.0);
    if (points.size() < 3 || amount <= 0.0 || passes <= 0) {
        return finish(std::move(points));
    }
    normalizeAutomation(points);
    for (int pass = 0; pass < passes; ++pass) {
        // Read from a snapshot so the pass is a single simultaneous step: an
        // in-place sweep would smear each point into the next and drag the
        // whole curve towards wherever the sweep started.
        const Points before = points;
        for (std::size_t i = 1; i + 1 < points.size(); ++i) {
            if (!range.contains(before[i].beats)) continue;
            const double average = 0.5 * (before[i - 1].value + before[i + 1].value);
            points[i].value = before[i].value + (average - before[i].value) * amount;
        }
    }
    return finish(std::move(points));
}

Points thin(Points points, double tolerance, const Range& range) {
    normalizeAutomation(points);
    if (points.size() < 3 || !(tolerance > 0.0)) return points;

    // Ramer–Douglas–Peucker, iterative so a long curve cannot blow the stack.
    // The distance measured is vertical rather than perpendicular: beats and
    // normalised values are not the same quantity, and a tolerance in "value"
    // is the one a user can reason about.
    std::vector<char> keep(points.size(), 0);
    keep.front() = 1;
    keep.back() = 1;
    std::vector<std::pair<std::size_t, std::size_t>> work{{0, points.size() - 1}};
    while (!work.empty()) {
        const auto [lo, hi] = work.back();
        work.pop_back();
        if (hi <= lo + 1) continue;
        double worst = 0.0;
        std::size_t worstAt = lo;
        for (std::size_t i = lo + 1; i < hi; ++i) {
            const double span = points[hi].beats - points[lo].beats;
            const double t = span > kEps ? (points[i].beats - points[lo].beats) / span : 0.0;
            const double line = points[lo].value + (points[hi].value - points[lo].value) * t;
            const double d = std::abs(points[i].value - line);
            if (d > worst) {
                worst = d;
                worstAt = i;
            }
        }
        if (worst > tolerance) {
            keep[worstAt] = 1;
            work.push_back({lo, worstAt});
            work.push_back({worstAt, hi});
        }
    }
    Points out;
    out.reserve(points.size());
    for (std::size_t i = 0; i < points.size(); ++i) {
        // A point outside the range was never up for removal, and a shape other
        // than a plain straight line is a decision someone made by hand.
        const bool shaped = points[i].shape != AutomationSegment::Linear ||
                            std::abs(points[i].curve) > kEps;
        if (keep[i] || shaped || !range.contains(points[i].beats)) out.push_back(points[i]);
    }
    return out;
}

Points setShape(Points points, AutomationSegment shape, double curve,
                const Range& range) {
    normalizeAutomation(points);
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        if (!range.contains(points[i].beats)) continue;
        points[i].shape = shape;
        points[i].curve = std::clamp(curve, -1.0, 1.0);
    }
    return points;
}

Points reverse(Points points, const Range& range) {
    normalizeAutomation(points);
    Points inside;
    Points out;
    for (const auto& p : points) {
        (range.contains(p.beats) ? inside : out).push_back(p);
    }
    if (inside.size() < 2) return finish(std::move(points));

    const double lo = inside.front().beats;
    const double hi = inside.back().beats;
    for (std::size_t i = 0; i < inside.size(); ++i) {
        AutomationPoint p = inside[inside.size() - 1 - i];
        p.beats = lo + (hi - p.beats);
        // A shape describes the segment *after* its point, so reversing hands
        // each point the shape of what used to run into it.
        const std::size_t source = inside.size() - 1 - i;
        p.shape = source > 0 ? inside[source - 1].shape : AutomationSegment::Linear;
        p.curve = source > 0 ? inside[source - 1].curve : 0.0;
        out.push_back(p);
    }
    return finish(std::move(out));
}

Points shiftTime(Points points, double deltaBeats, const Range& range) {
    Points out;
    out.reserve(points.size());
    for (auto& p : points) {
        if (!range.contains(p.beats)) {
            out.push_back(p);
            continue;
        }
        p.beats += deltaBeats;
        if (p.beats >= -kEps) out.push_back(p);
    }
    return finish(std::move(out));
}

Points repeat(Points points, const Range& range, double untilBeats) {
    normalizeAutomation(points);
    const double span = range.toBeats - range.fromBeats;
    if (!(span > kEps) || untilBeats <= range.toBeats) return points;

    Points source;
    for (const auto& p : points) {
        if (range.contains(p.beats)) source.push_back(p);
    }
    if (source.empty()) return points;

    Points out = points;
    for (double offset = span; range.fromBeats + offset < untilBeats; offset += span) {
        for (const auto& p : source) {
            AutomationPoint copy = p;
            copy.beats = p.beats + offset;
            if (copy.beats > untilBeats + kEps) continue;
            out.push_back(copy);
        }
    }
    return finish(std::move(out));
}

}   // namespace daw::autotools
