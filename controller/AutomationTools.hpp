#pragma once

#include "model/Document.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Curve transforms — the arithmetic behind the automation editor's generators
// and shaping commands, as pure functions over a vector of breakpoints.
//
// The same split MidiTools.hpp uses, and for the same reasons: nothing here
// touches the document, the undo stack, the controller or Qt. A transform takes
// the points it should act on and returns the replacement; the caller splices
// the result back through `EngineController::setAutomationPoints` (live) and
// `commitAutomationEdit` (one undo entry). That is what lets a dialog run the
// same code on a throwaway copy to draw a live preview, and what makes every
// one of these testable head-on.
//
// Conventions shared by all of them:
//   • Times are beats from the clip's start, exactly like AutomationPoint.
//   • Values are normalised 0…1, exactly like AutomationPoint. Turning that
//     into decibels or a plugin's own units is the controller's job
//     (`automationToPlain`), and deliberately not visible here.
//   • Every function returns points already in `normalizeAutomation` shape:
//     sorted, one value per instant, clamped. A caller may hand in anything.
//   • Anything random takes an explicit `seed`, so a preview and the apply that
//     follows produce byte-identical results and tests are stable.
namespace daw::autotools {

using Points = std::vector<AutomationPoint>;

/// A stretch of time a transform acts on — normally the editor's selection,
/// and by default the whole curve. Both ends are inclusive; a range that starts
/// after it ends selects nothing.
struct Range {
    double fromBeats = 0.0;
    double toBeats = 1e18;

    bool contains(double beats) const noexcept {
        return beats >= fromBeats - 1e-9 && beats <= toBeats + 1e-9;
    }
    bool empty() const noexcept { return toBeats < fromBeats; }
};

/// Everything, the default for every transform that takes a Range.
inline Range everything() { return Range{}; }

// ── Generators ──────────────────────────────────────────────────────────────

enum class LfoWave {
    Sine,
    Triangle,
    Saw,        // falls: starts high, ramps down, jumps back up
    Ramp,       // rises: the mirror of Saw
    Square,
    SampleHold, // one random level per cycle, held
};

std::string lfoWaveName(LfoWave wave);
std::string lfoWaveId(LfoWave wave);
LfoWave lfoWaveFromId(const std::string& id);
const std::vector<LfoWave>& allLfoWaves();

struct LfoSpec {
    LfoWave wave = LfoWave::Sine;
    double startBeats = 0.0;
    double lengthBeats = 4.0;
    /// Beats per cycle. A whole note at 4/4 is 4.0, a sixteenth is 0.25.
    double rateBeats = 1.0;
    /// How far the wave swings, as a fraction of the full 0…1 range. 1.0 with a
    /// centre of 0.5 reaches both rails.
    double depth = 1.0;
    /// The value the wave swings around.
    double center = 0.5;
    /// Where in the cycle it starts, 0…1 of a cycle.
    double phase = 0.0;
    /// Breakpoints per cycle for the shapes that curve. Square, Saw, Ramp and
    /// SampleHold ignore it: they are exact with two points per cycle, and
    /// spending more would only make them harder to edit afterwards.
    int stepsPerCycle = 16;
    std::uint32_t seed = 0;   // SampleHold only
};

/// A fresh curve. Not merged with anything — the caller decides whether this
/// replaces the clip's points or is spliced into a range.
Points lfo(const LfoSpec& spec);

/// Two points and the segment between them.
Points ramp(double fromBeats, double toBeats, double fromValue, double toValue,
            AutomationSegment shape = AutomationSegment::Linear, double curve = 0.0);

/// Replace everything inside `range` with `replacement`, whose beats are
/// absolute. The points outside are left exactly as they were — the splice
/// every generator lands through when the editor has a selection.
Points splice(Points points, const Range& range, const Points& replacement);

/// Move one breakpoint as a pointer gesture would. Lowering a point preserves
/// the curve immediately before it with a second, anchored breakpoint, so a
/// level change becomes a short ramp instead of pulling the whole preceding
/// segment down. `guardBeats` is the width of that ramp when the pointer has not
/// moved to the right of the point it picked up.
Points dragPoint(const Points& points, std::size_t index, double beats,
                 double value, double guardBeats);

// ── Transforms ──────────────────────────────────────────────────────────────

/// Mirror the values about 0.5 — a rise becomes a fall. Bends are left alone
/// and mirror themselves: a segment's value is `from + (to − from) · shape(t)`,
/// so flipping both ends flips the whole path through it.
Points invert(Points points, const Range& range = everything());

/// `value * scale + offset`, clamped. Scaling about `pivot` rather than about
/// zero, so shrinking a curve keeps it where it sits instead of dragging it to
/// the floor.
Points scaleValues(Points points, double scale, double offset = 0.0,
                   double pivot = 0.5, const Range& range = everything());

/// Move every point to the nearest multiple of `gridBeats`. Points that land on
/// the same slot collapse into one — the earlier of them survives, which is the
/// rule `normalizeAutomation` applies to every curve anyway.
Points quantizeTime(Points points, double gridBeats, const Range& range = everything());

/// Snap values onto a ladder of `steps` evenly spaced levels — 2 gives a
/// switch, 12 a semitone ladder on a pitch parameter.
Points quantizeValues(Points points, int steps, const Range& range = everything());

/// Ease the corners off: each point moves a fraction `amount` (0…1) of the way
/// towards the average of its neighbours, `passes` times. Ends are pinned, so
/// smoothing never drags the start or finish of the curve.
Points smooth(Points points, double amount, int passes = 1,
              const Range& range = everything());

/// Drop points the curve does not need, keeping it within `tolerance` (in
/// normalised value units) of the original. Ramer–Douglas–Peucker, which keeps
/// the corners and eats the straights.
Points thin(Points points, double tolerance, const Range& range = everything());

/// Set the segment shape (and bend) of every point in the range. The last point
/// of the curve is left alone: its shape is never read.
Points setShape(Points points, AutomationSegment shape, double curve,
                const Range& range = everything());

/// Reverse the curve in time within the range — the shape plays backwards, in
/// place. Segment shapes travel with the segment they describe.
Points reverse(Points points, const Range& range = everything());

/// Slide the range in time by `deltaBeats`, dropping anything pushed before
/// zero. Points outside the range stay; a collision resolves the same way
/// `quantizeTime`'s does.
Points shiftTime(Points points, double deltaBeats, const Range& range = everything());

/// Repeat the contents of `range` until `untilBeats`, so a bar of LFO fills a
/// chorus. The source range must have some length.
Points repeat(Points points, const Range& range, double untilBeats);

}   // namespace daw::autotools
