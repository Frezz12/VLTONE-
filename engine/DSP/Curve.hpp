#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

/// Automation curve shaping, in the engine so that everything which has to
/// agree about the shape of a segment can reach it.
///
/// Three parties read a curve and they must not disagree by so much as a pixel:
/// the controller, which compiles curves into plugin automation; `GainNode`,
/// which plays a track's volume and pan straight from one on the audio thread;
/// and the arrangement, which draws it. A shape defined in any one of them
/// would be re-derived — slightly differently — in the other two.
///
/// The enum is deliberately *not* `daw::AutomationSegment`: this layer must not
/// depend on the document model, and the pairing is static_asserted where the
/// two meet, the same arrangement `PluginFormat` and `plugins::Format` already
/// have.
namespace daw::engine::curve {

enum class Shape : std::uint8_t { Linear = 0, Hold = 1, SCurve = 2 };

/// One breakpoint. `beats` is whatever time base the caller is using — the
/// controller rebases clip-relative beats onto the timeline before it gets
/// here, and this code never needs to know which it was handed.
struct Point {
    double beats = 0.0;
    double value = 0.0;
    Shape shape = Shape::Linear;
    /// −1 convex … 0 straight … +1 concave, for the segment that starts here.
    double curve = 0.0;
};

/// Reshape a linear 0…1 position within a segment.
///
/// Realtime-safe: no allocation, no branching on anything but the shape, and
/// `std::pow` only where the segment is actually bent.
inline double shapeT(double t, Shape shape, double curve) noexcept {
    t = std::clamp(t, 0.0, 1.0);
    switch (shape) {
        case Shape::Hold:
            // The value belongs to the point it started from until the very
            // end of the segment. A switch that ramps passes through settings
            // it does not have.
            return 0.0;
        case Shape::SCurve: {
            // Eased at both ends. `curve` slides the easing towards one end so
            // an S can be lopsided without needing a fourth shape.
            const double eased = t * t * (3.0 - 2.0 * t);
            const double bias = std::clamp(curve, -1.0, 1.0);
            if (std::abs(bias) < 1e-9) return eased;
            const double exponent = std::pow(2.0, -bias * 1.5);
            return std::pow(eased, exponent);
        }
        case Shape::Linear:
            break;
    }
    const double bend = std::clamp(curve, -1.0, 1.0);
    if (std::abs(bend) < 1e-9) return t;
    // Same family the clip fades use: an exponent either side of 1, so 0 is
    // exactly straight and the two directions are mirror images.
    return std::pow(t, std::pow(2.0, -bend * 2.0));
}

/// The curve's value at `beats`.
///
/// Before the first point the curve holds `fallback` — *not* the first point's
/// value. That is the rule `PluginNode` already plays by, and it is what makes
/// a clip's `defaultValue` mean anything.
///
/// Realtime-safe. `hint` is a cursor into `points` that the caller keeps
/// between blocks; pass 0 (or leave it) for a one-off lookup.
inline double valueAt(const std::vector<Point>& points, double beats,
                      double fallback, std::size_t* hint = nullptr) noexcept {
    if (points.empty()) return fallback;
    if (beats < points.front().beats) return fallback;
    if (beats >= points.back().beats) return points.back().value;

    // Forward-only from the cursor, which is why an ordinary block costs one
    // comparison rather than a search. A backward seek is the caller's to
    // notice; it resets the hint.
    std::size_t i = hint ? std::min(*hint, points.size() - 1) : 0;
    while (i + 1 < points.size() && points[i + 1].beats <= beats) ++i;
    if (hint) *hint = i;

    const Point& from = points[i];
    if (i + 1 >= points.size()) return from.value;
    const Point& to = points[i + 1];
    const double span = to.beats - from.beats;
    if (!(span > 0.0)) return to.value;

    const double t = shapeT((beats - from.beats) / span, from.shape, from.curve);
    return from.value + (to.value - from.value) * t;
}

/// Break one segment into straight pieces that follow its shape.
///
/// For consumers that can only interpolate linearly — the plugin automation
/// path is one, and giving it curve shapes would mean putting `std::pow` on the
/// audio thread for every plugin on every block. Densifying once, here, when
/// the graph is compiled costs nothing at playback and is exact to within the
/// step.
inline int densifySteps(Shape shape, double curve) noexcept {
    if (shape == Shape::Hold) return 1;          // a step needs no pieces
    if (std::abs(curve) < 1e-9 && shape == Shape::Linear) return 1;
    return 16;
}

} // namespace daw::engine::curve
