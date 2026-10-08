// The automation editor's curve maths, checked head-on.
//
// Every generator and shaping command is a pure `vector<AutomationPoint>`
// transform in AutomationTools.hpp, so this test needs no audio device, no Qt
// and no document: build a curve, run the transform, read the result. The one
// document-level check at the end covers the undoable primitive the editor
// lands all of them through.
#include "AutomationTools.hpp"
#include "EngineController.hpp"
#include "Internal/EqualizerInstance.hpp"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>
#include <unordered_set>

namespace at = daw::autotools;
using daw::AutomationPoint;
using daw::AutomationSegment;

static int failures = 0;
static bool check(bool cond, const char* what) {
    std::printf("%s  %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) ++failures;
    return cond;
}

static bool near(double a, double b, double tolerance = 1e-6) {
    return std::abs(a - b) <= tolerance;
}

static bool sameCurve(const std::vector<AutomationPoint>& a,
                      const std::vector<AutomationPoint>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!near(a[i].beats, b[i].beats) ||
            !near(a[i].value, b[i].value) || a[i].shape != b[i].shape ||
            !near(a[i].curve, b[i].curve))
            return false;
    }
    return true;
}

static bool uniquePointIds(const std::vector<AutomationPoint>& points) {
    std::unordered_set<std::string> ids;
    for (const AutomationPoint& point : points) {
        if (point.id.empty() || !ids.insert(point.id).second) return false;
    }
    return true;
}

/// The clip with the given id on the given track, or null.
static const daw::ClipModel* findClip(const daw::EngineController& c,
                                      const std::string& trackId,
                                      const std::string& clipId) {
    const auto* t = c.project().findTrack(trackId);
    if (!t) return nullptr;
    for (const auto& clip : t->clips) {
        if (clip.id == clipId) return &clip;
    }
    return nullptr;
}

static AutomationPoint pt(double beats, double value,
                          AutomationSegment shape = AutomationSegment::Linear,
                          double curve = 0.0) {
    AutomationPoint p;
    p.beats = beats;
    p.value = value;
    p.shape = shape;
    p.curve = curve;
    return p;
}

/// Sorted, one point per instant, every value on the scale — the shape every
/// transform promises its caller, so it is worth asserting on all of them
/// rather than on each one separately.
static bool wellFormed(const at::Points& points) {
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (points[i].value < 0.0 || points[i].value > 1.0) return false;
        if (points[i].beats < 0.0) return false;
        if (i > 0 && points[i].beats <= points[i - 1].beats) return false;
    }
    return true;
}

/// The peak-to-trough swing of a curve, sampled densely enough to catch the
/// shape between breakpoints rather than only at them.
static void swing(const at::Points& points, double fromBeats, double toBeats,
                  double& lo, double& hi) {
    lo = 1.0;
    hi = 0.0;
    for (int i = 0; i <= 400; ++i) {
        const double b = fromBeats + (toBeats - fromBeats) * i / 400.0;
        const double v = daw::automationValueAt(points, b, 0.0);
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
}

int main() {
    // ── LFO ─────────────────────────────────────────────────────────────────
    {
        at::LfoSpec spec;
        spec.wave = at::LfoWave::Sine;
        spec.lengthBeats = 8.0;
        spec.rateBeats = 2.0;
        spec.depth = 1.0;
        spec.center = 0.5;
        at::Points sine = at::lfo(spec);
        check(wellFormed(sine), "a sine LFO comes out sorted, unique and in range");
        check(near(sine.front().beats, 0.0) && near(sine.back().beats, 8.0, 1e-6),
              "it spans exactly the length asked for");
        double lo = 0.0, hi = 0.0;
        swing(sine, 0.0, 8.0, lo, hi);
        check(near(lo, 0.0, 0.02) && near(hi, 1.0, 0.02),
              "at full depth about 0.5 it reaches both rails");
        check(near(daw::automationValueAt(sine, 0.5, 0.0), 1.0, 0.02),
              "a quarter of the way through a 2-beat cycle it is at the crest");
        check(near(daw::automationValueAt(sine, 1.5, 0.0), 0.0, 0.02),
              "three quarters through, at the trough");

        spec.depth = 0.5;
        at::Points shallow = at::lfo(spec);
        swing(shallow, 0.0, 8.0, lo, hi);
        check(near(lo, 0.25, 0.02) && near(hi, 0.75, 0.02),
              "half depth swings half as far, still centred");

        spec.depth = 1.0;
        spec.center = 0.25;
        at::Points low = at::lfo(spec);
        swing(low, 0.0, 8.0, lo, hi);
        check(near(hi, 0.75, 0.02) && near(lo, 0.0, 1e-9),
              "a centre near the floor clips against it rather than wrapping");

        spec.center = 0.5;
        spec.phase = 0.25;
        at::Points shifted = at::lfo(spec);
        check(near(daw::automationValueAt(shifted, 0.0, 0.0), 1.0, 0.02),
              "a quarter turn of phase starts the sine at its crest");
    }
    {
        at::LfoSpec spec;
        spec.wave = at::LfoWave::Square;
        spec.lengthBeats = 4.0;
        spec.rateBeats = 1.0;
        at::Points square = at::lfo(spec);
        check(wellFormed(square) && square.size() == 8,
              "a square wave is two breakpoints a cycle and not one more");
        check(square.front().shape == AutomationSegment::Hold,
              "its segments hold — a switch that ramps passes through settings "
              "it does not have");
        check(near(daw::automationValueAt(square, 0.4, 0.0), 1.0) &&
                  near(daw::automationValueAt(square, 0.6, 0.0), 0.0),
              "it is flat either side of the edge, not sloping");
    }
    {
        at::LfoSpec spec;
        spec.wave = at::LfoWave::Ramp;
        spec.lengthBeats = 4.0;
        spec.rateBeats = 2.0;
        at::Points ramp = at::lfo(spec);
        check(wellFormed(ramp), "a ramp comes out well formed despite its jumps");
        check(near(daw::automationValueAt(ramp, 0.0, 0.0), 0.0, 1e-6) &&
                  near(daw::automationValueAt(ramp, 1.0, 0.0), 0.5, 0.01),
              "it climbs linearly through its cycle");
        // The vertical drop back is two points a hair apart. Just before the
        // boundary the wave is still at the top; just after, at the bottom.
        check(daw::automationValueAt(ramp, 1.999, 0.0) > 0.9 &&
                  daw::automationValueAt(ramp, 2.001, 0.0) < 0.1,
              "the return is a jump, not a slope back through the cycle");
    }
    {
        at::LfoSpec spec;
        spec.wave = at::LfoWave::SampleHold;
        spec.lengthBeats = 8.0;
        spec.rateBeats = 1.0;
        spec.seed = 7;
        at::Points a = at::lfo(spec);
        at::Points b = at::lfo(spec);
        check(a == b, "sample & hold repeats exactly for one seed");
        spec.seed = 8;
        check(!(at::lfo(spec) == a), "and differs for another");
        check(a.size() == 8 && a.front().shape == AutomationSegment::Hold,
              "one held level per cycle");
        bool varied = false;
        for (std::size_t i = 1; i < a.size(); ++i) {
            if (std::abs(a[i].value - a[0].value) > 0.05) varied = true;
        }
        check(varied, "the levels actually differ from each other");
    }

    // ── Point gestures ─────────────────────────────────────────────────────
    {
        const at::Points curve{pt(0.0, 0.8), pt(4.0, 0.8), pt(8.0, 0.6)};
        const at::Points lowered = at::dragPoint(curve, 1, 6.0, 0.2);
        check(lowered.size() == curve.size() && near(lowered[1].beats, 6.0) &&
                  near(lowered[1].value, 0.2),
              "lowering a point moves only that point");

        const at::Points vertical =
            at::dragPoint(curve, 1, 4.0, 0.2);
        check(vertical.size() == curve.size() &&
                  near(vertical[1].beats, 4.0) && near(vertical[1].value, 0.2),
              "a vertical drag does not create a second point");

        const at::Points raised = at::dragPoint(curve, 1, 6.0, 0.9);
        check(raised.size() == curve.size() && near(raised[1].beats, 6.0) &&
                  near(raised[1].value, 0.9),
              "raising a point also moves one handle");
    }

    // ── Transforms ──────────────────────────────────────────────────────────
    {
        at::Points curve{pt(0.0, 0.0), pt(2.0, 1.0), pt(4.0, 0.25)};
        at::Points flipped = at::invert(curve);
        check(near(flipped[0].value, 1.0) && near(flipped[1].value, 0.0) &&
                  near(flipped[2].value, 0.75),
              "invert mirrors every value about the middle");
        check(at::invert(flipped) == curve, "and twice is the original again");
    }
    {
        at::Points curve{pt(0.0, 0.0), pt(1.0, 1.0)};
        at::Points half = at::scaleValues(curve, 0.5);
        check(near(half[0].value, 0.25) && near(half[1].value, 0.75),
              "scaling shrinks a curve about the middle, not towards the floor");
        at::Points lifted = at::scaleValues(curve, 1.0, 0.2);
        check(near(lifted[0].value, 0.2) && near(lifted[1].value, 1.0),
              "offset lifts it, and the top clamps rather than wrapping");
    }
    {
        at::Points curve{pt(0.1, 0.0), pt(0.4, 0.5), pt(2.2, 1.0)};
        at::Points snapped = at::quantizeTime(curve, 1.0);
        check(snapped.size() == 2 && near(snapped[0].beats, 0.0) &&
                  near(snapped[1].beats, 2.0),
              "quantising time snaps to the grid and fuses the collision");
        check(near(snapped[0].value, 0.0),
              "the earlier of two fused points is the one that survives");
    }
    {
        at::Points curve{pt(0.0, 0.1), pt(1.0, 0.4), pt(2.0, 0.9)};
        at::Points laddered = at::quantizeValues(curve, 3);
        check(near(laddered[0].value, 0.0) && near(laddered[1].value, 0.5) &&
                  near(laddered[2].value, 1.0),
              "quantising values snaps them to a ladder of levels");
    }
    {
        // A spike between two flat runs: smoothing should pull the spike down
        // without moving the ends.
        at::Points curve{pt(0.0, 0.0), pt(1.0, 0.0), pt(2.0, 1.0), pt(3.0, 0.0),
                         pt(4.0, 0.0)};
        at::Points eased = at::smooth(curve, 0.5);
        check(near(eased.front().value, 0.0) && near(eased.back().value, 0.0),
              "smoothing pins the ends");
        check(eased[2].value < 0.6 && eased[2].value > 0.0,
              "and takes the top off the spike");
        check(eased.size() == curve.size(), "without adding or dropping points");
        at::Points harder = at::smooth(curve, 0.5, 4);
        check(harder[2].value < eased[2].value, "more passes smooth further");
        at::Points none = at::smooth(curve, 0.0);
        check(none == curve, "an amount of zero is a no-op");
    }
    {
        // Five points on one straight line, plus a real corner.
        at::Points curve{pt(0.0, 0.0), pt(1.0, 0.25), pt(2.0, 0.5),
                         pt(3.0, 0.75), pt(4.0, 1.0), pt(5.0, 0.0)};
        at::Points thinned = at::thin(curve, 0.01);
        check(thinned.size() == 3 && near(thinned[1].beats, 4.0),
              "thinning eats the straights and keeps the corner");
        at::Points shaped = curve;
        shaped[1].shape = AutomationSegment::Hold;
        check(at::thin(shaped, 0.01).size() == 4,
              "a point somebody shaped by hand is never thinned away");
    }
    {
        at::Points curve{pt(0.0, 0.0), pt(1.0, 1.0), pt(2.0, 0.0)};
        at::Points held = at::setShape(curve, AutomationSegment::Hold, 0.0);
        check(held[0].shape == AutomationSegment::Hold &&
                  held[1].shape == AutomationSegment::Hold,
              "setShape reaches every segment in the range");
        check(near(daw::automationValueAt(held, 0.5, 0.0), 0.0),
              "and the curve really steps");
    }
    {
        at::Points curve{pt(0.0, 0.0, AutomationSegment::Hold), pt(1.0, 0.25),
                         pt(4.0, 1.0)};
        at::Points back = at::reverse(curve);
        check(near(back[0].value, 1.0) && near(back[1].value, 0.25) &&
                  near(back[2].value, 0.0),
              "reversing plays the shape backwards in place");
        check(near(back[0].beats, 0.0) && near(back[2].beats, 4.0),
              "over exactly the same stretch of time");
        check(back[1].shape == AutomationSegment::Hold,
              "a segment's shape travels with the segment, not with the point");
    }
    {
        at::Points curve{pt(0.0, 0.0), pt(1.0, 1.0), pt(8.0, 0.5)};
        at::Points moved = at::shiftTime(curve, 2.0, at::Range{0.0, 1.5});
        check(moved.size() == 3 && near(moved[0].beats, 2.0) &&
                  near(moved[1].beats, 3.0) && near(moved[2].beats, 8.0),
              "shifting moves only what the range covers");
        at::Points off = at::shiftTime(curve, -0.5, at::Range{0.0, 1.5});
        check(off.size() == 2 && near(off[0].beats, 0.5),
              "and a point pushed before zero is dropped, not piled on it");
    }
    {
        at::Points curve{pt(0.0, 0.0), pt(1.0, 1.0)};
        at::Points filled = at::repeat(curve, at::Range{0.0, 2.0}, 8.0);
        check(near(daw::automationValueAt(filled, 3.0, 0.0), 1.0) &&
                  near(daw::automationValueAt(filled, 5.0, 0.0), 1.0),
              "repeat tiles the range forward");
        check(wellFormed(filled), "and the result is still well formed");
    }
    {
        at::Points curve{pt(0.0, 0.0), pt(1.0, 0.5), pt(2.0, 1.0), pt(3.0, 0.25)};
        at::Points into = at::splice(curve, at::Range{0.5, 2.5},
                                     at::Points{pt(1.0, 0.0), pt(2.0, 0.0)});
        check(into.size() == 4 && near(into[1].value, 0.0) && near(into[2].value, 0.0),
              "a splice replaces the range and leaves the rest alone");
        check(near(into[0].value, 0.0) && near(into[3].value, 0.25),
              "including the points either side of it");
    }

    // ── Through the document ────────────────────────────────────────────────
    //
    // The editor never writes points itself: it runs a transform on a copy and
    // hands the result to the controller. This is that hand-off, and the undo
    // behaviour the whole editor depends on — a gesture is live and free, and
    // becomes exactly one entry when it is let go.
    {
        daw::EngineController controller{daw::EngineController::TestRuntime{}};
        controller.initialize(48000, 512, /*openDevice=*/false);
        const std::string trackId = controller.addTrack(daw::TrackKind::Audio, "Keys");
        daw::AutomationTarget target;
        target.kind = daw::AutomationTargetKind::TrackVolume;
        target.channelId = trackId;
        const std::string laneId = controller.addAutomationLane(trackId, target);
        const std::string clipId = controller.addAutomationClip(laneId, target, 0.0, 8.0);

        const daw::ClipModel* clip = findClip(controller, laneId, clipId);
        check(clip != nullptr, "an automation clip lands on the lane");
        check(near(controller.automationResetValue(target),
                   daw::normalizedFromGain(1.0)),
              "a volume automation point resets to unity");
        target.kind = daw::AutomationTargetKind::TrackPan;
        check(near(controller.automationResetValue(target), 0.5),
              "a pan automation point resets to centre");
        target.kind = daw::AutomationTargetKind::TrackMute;
        check(near(controller.automationResetValue(target), 0.0),
              "a mute automation point resets to off");
        target.kind = daw::AutomationTargetKind::TrackVolume;

        at::LfoSpec spec;
        spec.wave = at::LfoWave::Triangle;
        spec.lengthBeats = 8.0;
        spec.rateBeats = 2.0;
        const at::Points before = clip->automation.points;
        const bool activeBefore = clip->automation.active;
        const at::Points drawn = at::lfo(spec);

        const std::size_t undoDepth = controller.undoDepth();
        controller.setAutomationPoints(laneId, clipId, drawn);
        check(controller.undoDepth() == undoDepth,
              "a live edit costs nothing on the undo stack");
        check(uniquePointIds(findClip(controller, laneId, clipId)->automation.points),
              "new automation points receive unique collaboration ids");
        controller.setAutomationPoints(laneId, clipId, at::invert(drawn));
        controller.commitAutomationEdit(laneId, clipId, before, "LFO",
                                        activeBefore);
        check(controller.undoDepth() == undoDepth + 1,
              "however many live steps it took, letting go is one entry");

        clip = findClip(controller, laneId, clipId);
        check(clip && sameCurve(clip->automation.points, at::invert(drawn)),
              "and the curve the transform produced is what the clip holds");

        controller.undo();
        clip = findClip(controller, laneId, clipId);
        check(clip && clip->automation.points == before &&
                  !clip->automation.active,
              "one undo puts the whole gesture back and makes it passive");

        const std::string midiTrack =
            controller.addTrack(daw::TrackKind::Midi, "Controller IDs");
        const std::string midiClip =
            controller.addMidiClip(midiTrack, 0.0, 2.0);
        const std::string controllerLane =
            controller.addControllerLane(midiTrack, midiClip, "Modulation", 1);
        controller.setLanePoints(midiTrack, midiClip, controllerLane,
                                 {pt(0.0, 0.2), pt(1.0, 0.8)});
        const daw::ClipModel* midi = findClip(controller, midiTrack, midiClip);
        const auto lane = midi
            ? std::find_if(midi->lanes.begin(), midi->lanes.end(),
                           [&](const auto& item) {
                               return item.id == controllerLane;
                           })
            : std::vector<daw::ControllerLane>::const_iterator{};
        check(midi && lane != midi->lanes.end() && uniquePointIds(lane->points),
              "controller-lane points receive unique collaboration ids");
        controller.shutdown();
    }

    // ── Realtime compilation and history ───────────────────────────────────
    {
        daw::EngineController controller{daw::EngineController::TestRuntime{}};
        controller.initialize(48000, 512, /*openDevice=*/false);
        controller.setTempo(120.0);
        const std::string track =
            controller.addTrack(daw::TrackKind::Audio, "Step target");
        daw::AutomationTarget target;
        target.kind = daw::AutomationTargetKind::TrackPan;
        target.channelId = track;
        const std::string lane = controller.addAutomationLane(track, target);
        const std::string clip =
            controller.addAutomationClip(lane, target, 0.0, 2.0);
        controller.setAutomationPoints(
            lane, clip,
            {pt(0.0, 0.0, AutomationSegment::Hold), pt(1.0, 1.0)});
        controller.seekSeconds(0.25);  // beat 0.5, still before the step
        const auto held = controller.automationValueAtPlayhead(target);
        check(held && near(*held, -1.0),
              "a Hold segment stays stepped in the compiled realtime curve");
        controller.shutdown();
    }
    {
        daw::EngineController controller{daw::EngineController::TestRuntime{}};
        controller.initialize(48000, 512, /*openDevice=*/false);
        const std::string track =
            controller.addTrack(daw::TrackKind::Audio, "Undo target");
        daw::AutomationTarget target;
        target.kind = daw::AutomationTargetKind::TrackVolume;
        target.channelId = track;
        const auto [lane, clip] = controller.ensureAutomation(target);
        check(!lane.empty() && !clip.empty(),
              "automation creation produces a lane and clip");
        controller.undo();
        check(!controller.project().findTrack(lane) &&
                  !controller.project().findTrack(track)->automationExpanded,
              "creation undo removes the lane and restores disclosure state");
        controller.redo();
        check(controller.project().findTrack(lane) &&
                  findClip(controller, lane, clip),
              "creation redo restores the same lane and clip identities");
        controller.undo();
        check(!controller.project().findTrack(lane),
              "the restored automation can be undone again");
        controller.shutdown();
    }
    {
        daw::EngineController controller{daw::EngineController::TestRuntime{}};
        controller.initialize(48000, 512, /*openDevice=*/false);
        const std::string first =
            controller.addTrack(daw::TrackKind::Audio, "First");
        const std::string second =
            controller.addTrack(daw::TrackKind::Audio, "Second");
        controller.setTrackVolumeLive(first, 0.25f);
        daw::AutomationTarget volume;
        volume.kind = daw::AutomationTargetKind::TrackVolume;
        volume.channelId = first;
        const std::string lane = controller.addAutomationLane(first, volume);
        const std::string clip =
            controller.addAutomationClip(lane, volume, 0.0, 2.0);
        const daw::ClipModel before = *findClip(controller, lane, clip);

        daw::AutomationTarget pan;
        pan.kind = daw::AutomationTargetKind::TrackPan;
        pan.channelId = second;
        controller.setAutomationTarget(lane, clip, pan);
        const daw::ClipModel after = *findClip(controller, lane, clip);
        controller.setTrackVolumeLive(first, 0.8f);
        controller.undo();
        const daw::ClipModel* restored = findClip(controller, lane, clip);
        check(restored && restored->name == before.name &&
                  restored->automation.target == before.automation.target &&
                  near(restored->automation.defaultValue,
                       before.automation.defaultValue) &&
                  restored->automation.points == before.automation.points,
              "retarget undo restores the exact passive curve and name");
        controller.redo();
        restored = findClip(controller, lane, clip);
        check(restored && restored->name == after.name &&
                  restored->automation.target == after.automation.target &&
                  near(restored->automation.defaultValue,
                       after.automation.defaultValue) &&
                  restored->automation.points == after.automation.points,
              "retarget redo restores the exact replacement state");
        controller.shutdown();
    }
    {
        daw::EngineController controller{daw::EngineController::TestRuntime{}};
        controller.initialize(48000, 512, /*openDevice=*/false);
        controller.setTempo(120.0);
        const std::string track =
            controller.addTrack(daw::TrackKind::Audio, "Plugin target");
        const std::string slot = controller.addInsert(
            track,
            daw::plugins::equalizer::EqualizerInstance::staticDescriptor());
        const auto parameters = controller.insertParameters(track, slot);
        const auto parameter = std::find_if(
            parameters.begin(), parameters.end(),
            [](const auto& info) { return info.isAutomatable; });
        bool merged = false;
        if (!slot.empty() && parameter != parameters.end()) {
            daw::AutomationTarget target;
            target.kind = daw::AutomationTargetKind::PluginParameter;
            target.channelId = track;
            target.slotId = slot;
            target.parameterId = parameter->id;
            const std::string lane = controller.addAutomationLane(track, target);
            const std::string clip =
                controller.addAutomationClip(lane, target, 0.0, 2.0);
            controller.setAutomationPoints(lane, clip,
                                           {pt(0.0, 0.2), pt(4.0, 0.8)});
            const std::string right = controller.splitClip(lane, clip, 1.0);
            const auto graph = controller.routingGraph();
            const auto* nodes = controller.trackNodes(track);
            if (!right.empty() && graph && nodes && !nodes->inserts.empty()) {
                for (const auto& entry : graph->nodes) {
                    if (entry.id != nodes->inserts.front()) continue;
                    const auto* plugin =
                        dynamic_cast<const daw::plugins::PluginNode*>(entry.node);
                    const auto curves = plugin ? plugin->automation() : nullptr;
                    merged = curves && curves->size() == 1;
                }
            }
            check(controller.setInsertChannelMode(track, slot, daw::PluginChannelMode::DualMono),
                  "automated effect switches to two independent instances");
            const auto dualGraph = controller.routingGraph();
            const auto dualCurvesMatch = [&](bool populated, double firstValue) {
                unsigned count = 0;
                for (const auto& entry : dualGraph->nodes) {
                    const auto* plugin = dynamic_cast<const daw::plugins::PluginNode*>(entry.node);
                    if (!plugin || plugin->instance()->descriptor().uid !=
                        daw::plugins::equalizer::EqualizerInstance::staticDescriptor().uid) continue;
                    ++count;
                    const auto curves = plugin->automation();
                    if (!curves) return false;
                    if (!populated) { if (!curves->empty()) return false; continue; }
                    if (curves->size() != 1 || curves->front().points.empty()) return false;
                    const auto index = plugin->instance()->parameterIndexForId(parameter->id);
                    if (index < 0 || curves->front().parameterIndex != std::uint32_t(index)) return false;
                    const auto& info = plugin->instance()->parameters()[std::size_t(index)];
                    if (!near(curves->front().points.front().second,
                              info.minValue + (info.maxValue - info.minValue) * firstValue)) return false;
                }
                return count == 2;
            };
            check(dualCurvesMatch(true, .2),
                  "both dual-mono instances resolve and merge the same normalized parameter curves");
            controller.setAutomationPoints(lane, clip, {pt(0, .6), pt(2, .7)});
            check(controller.routingGraph() == dualGraph && dualCurvesMatch(true, .6),
                  "incremental automation edits update both instances without rebuilding topology");
            controller.removeAutomationLane(lane);
            check(dualCurvesMatch(false, 0),
                  "removing automation clears both native instances, including the right channel");
        }
        check(merged,
              "split clips targeting one plugin parameter compile as one curve");
        controller.shutdown();
    }

    // A passive lane follows the last touched parameter, until the user draws.
    {
        daw::EngineController c{daw::EngineController::TestRuntime{}};
        c.initialize(48000, 512, false);
        const std::string track = c.addTrack(daw::TrackKind::Audio, "Follow");
        const std::string slot = c.addInsert(track,
            daw::plugins::equalizer::EqualizerInstance::staticDescriptor());
        std::vector<std::string> parameters;
        for (const auto& info : c.insertParameters(track, slot))
            if (info.isAutomatable && !info.isStepped) parameters.push_back(info.id);
        if (check(parameters.size() >= 3, "follow fixture exposes three parameters")) {
            daw::AutomationTarget volume;
            volume.channelId = track;
            const auto graph = c.routingGraph();
            const auto preview = c.ensureAutomation(volume, true);
            check(c.routingGraph() == graph,
                  "adding an automation preview does not rebuild the audio graph");
            c.renameTrack(track, "Renamed owner");
            c.setAutomationExpanded(track, false);
            c.touchInsertParameter(track, slot, parameters[0]);
            c.touchInsertParameter(track, slot, parameters[1]);
            c.followTouchedAutomation();
            check(c.automationLanesOf(track).size() == 1 &&
                  findClip(c, preview.first, preview.second)->automation.target == volume &&
                  !c.project().findTrack(track)->automationExpanded,
                  "hidden automation remembers touches without opening or adding lanes");
            const auto last = c.lastTouchedAutomation(track);
            check(last && last->parameterId == parameters[1],
                  "the latest touch is remembered separately for its channel");
            const auto reused = c.ensureAutomation(*last, true);
            check(reused == preview && c.automationLanesOf(track).size() == 1 &&
                  !findClip(c, reused.first, reused.second)->automation.active &&
                  c.project().findTrack(reused.first)->name == c.automationTargetName(*last),
                  "the first plugin touch reuses the volume preview even after its owner is renamed");
            c.touchInsertParameter(track, slot, parameters[0]);
            c.touchInsertParameter(track, slot, parameters[2]);
            c.followTouchedAutomation();
            const auto latest = *c.lastTouchedAutomation(track);
            check(c.findAutomation(latest) == preview && c.automationLanesOf(track).size() == 1,
                  "multiple knob touches in a UI turn replace one preview");
            const auto depth = c.undoDepth();
            for (int i = 0; i < 200; ++i)
                c.setInsertParameter(track, slot, parameters[2],
                    c.automationToPlain(latest, 0.25));
            c.followTouchedAutomation();
            check(c.undoDepth() == depth && c.routingGraph() == graph &&
                  near(findClip(c, preview.first, preview.second)->automation.defaultValue, 0.25),
                  "a knob burst updates the passive value without history or graph churn");
            auto before = findClip(c, preview.first, preview.second)->automation.points;
            auto drawn = before;
            drawn.front().value = 0.8;
            c.setAutomationPoints(preview.first, preview.second, drawn);
            c.commitAutomationEdit(preview.first, preview.second, before, "Draw", false);
            c.touchInsertParameter(track, slot, parameters[0]);
            check(c.followTouchedAutomation() == daw::EngineController::AutomationFollowChange::Structure &&
                  c.automationLanesOf(track).size() == 2 &&
                  findClip(c, preview.first, preview.second)->automation.points == drawn &&
                  findClip(c, preview.first, preview.second)->automation.active,
                  "drawing protects the original curve and the next knob gets a new lane");
            const auto second = c.findAutomation(*c.lastTouchedAutomation(track));
            c.undo();
            check(!c.project().findTrack(second.first) &&
                  findClip(c, preview.first, preview.second)->automation.points == drawn,
                  "one undo removes only the new preview");
            c.redo();
            check(c.findAutomation(*c.lastTouchedAutomation(track)) == second,
                  "redo restores the same preview identities");
            c.touchInsertParameter(track, slot, parameters[1]);
            c.followTouchedAutomation();
            const auto retarget = *c.lastTouchedAutomation(track);
            check(c.findAutomation(retarget) == second,
                  "the next untouched preview is reused below the protected curve");
            c.undo();
            check(findClip(c, second.first, second.second)->automation.target.parameterId == parameters[0],
                  "retarget undo restores the old parameter");
            c.redo();
            check(c.findAutomation(retarget) == second &&
                  c.project().findTrack(second.first)->name == c.automationTargetName(retarget),
                  "retarget redo restores the parameter and lane name together");
            c.touchInsertParameter(track, slot, parameters[2]);
            c.followTouchedAutomation();
            check(c.automationLanesOf(track).size() == 2 && c.findAutomation(latest) == preview,
                  "touching an already drawn parameter never duplicates its lane");
            auto custom = findClip(c, second.first, second.second)->automation.points;
            custom.front().value = 0.7;
            c.setAutomationPoints(second.first, second.second, custom, false);
            c.touchInsertParameter(track, slot, parameters[0]);
            c.followTouchedAutomation();
            check(c.automationLanesOf(track).size() == 3 &&
                  findClip(c, second.first, second.second)->automation.points == custom,
                  "a disabled custom curve is never mistaken for an untouched preview");

            // Exercise the actual notification ring, including beginEdit with
            // no value change, as emitted by a native VST/CLAP editor.
            bool gesture = false;
            const auto* nodes = c.trackNodes(track);
            for (const auto& entry : c.routingGraph()->nodes) {
                if (!nodes || nodes->inserts.empty() || entry.id != nodes->inserts.front()) continue;
                auto* node = dynamic_cast<daw::plugins::PluginNode*>(entry.node);
                if (!node) continue;
                const auto index = node->instance()->parameterIndexForId(parameters[1]);
                node->onParameterGesture(std::uint32_t(index), true);
                c.pumpPluginEvents();
                gesture = c.lastTouchedAutomation(track)->parameterId == parameters[1];
            }
            check(gesture, "native beginEdit selects automation without changing the knob value");

            const std::string master = c.ensureMasterTrack();
            daw::AutomationTarget masterVolume;
            masterVolume.channelId = daw::EngineController::kMasterChannelId;
            const auto masterPreview = c.ensureAutomation(masterVolume, true);
            const std::string masterSlot = c.addInsert(masterVolume.channelId,
                daw::plugins::equalizer::EqualizerInstance::staticDescriptor());
            c.touchInsertParameter(masterVolume.channelId, masterSlot, parameters[0]);
            c.followTouchedAutomation();
            const auto masterTarget = c.lastTouchedAutomation(masterVolume.channelId);
            check(masterTarget && c.findAutomation(*masterTarget) == masterPreview &&
                  c.project().findTrack(masterPreview.first)->parentId == master &&
                  masterTarget->channelId == "master",
                  "Master reuses its preview with the real master channel target");

            const std::string instrument = c.addTrack(daw::TrackKind::Instrument, "Instrument touch");
            const auto sampler = c.pluginManager().find(daw::plugins::Format::Internal, "daw.sampler");
            if (check(sampler && c.setTrackInstrumentPlugin(instrument, *sampler),
                      "an instrument loads for automation follow")) {
                const std::string instrumentSlot = c.project().findTrack(instrument)->instrument.id;
                daw::AutomationTarget instrumentVolume;
                instrumentVolume.channelId = instrument;
                const auto instrumentPreview = c.ensureAutomation(instrumentVolume, true);
                for (const auto& info : c.insertParameters(instrument, instrumentSlot)) {
                    if (!info.isAutomatable) continue;
                    c.touchInsertParameter(instrument, instrumentSlot, info.id);
                    break;
                }
                c.followTouchedAutomation();
                const auto instrumentTarget = c.lastTouchedAutomation(instrument);
                check(instrumentTarget && instrumentTarget->slotId.empty() &&
                      c.findAutomation(*instrumentTarget) == instrumentPreview &&
                      masterTarget && c.findAutomation(*masterTarget) == masterPreview &&
                      c.lastTouchedAutomation(track)->parameterId == parameters[1],
                      "instrument touches use the empty slot convention and leave other tracks alone");
                if (instrumentTarget) {
                    auto explicitTarget = *instrumentTarget;
                    explicitTarget.slotId = instrumentSlot;
                    c.setAutomationTarget(instrumentPreview.first, instrumentPreview.second, explicitTarget);
                    c.setAutomationPoints(instrumentPreview.first, instrumentPreview.second,
                                          {pt(0, .35), pt(4, .75)});
                    // Both the serialized slot ID and the legacy empty-slot
                    // convention must resolve to the same native instrument.
                    const auto curvesMatch = [&] {
                        const auto graph = c.routingGraph();
                        const auto* nodes = c.trackNodes(instrument);
                        if (!graph || !nodes) return false;
                        for (const auto& entry : graph->nodes) {
                            if (entry.id != nodes->instrument) continue;
                            const auto* plugin = dynamic_cast<const daw::plugins::PluginNode*>(entry.node);
                            const auto curves = plugin ? plugin->automation() : nullptr;
                            const int index = plugin ? plugin->instance()->parameterIndexForId(explicitTarget.parameterId) : -1;
                            return index >= 0 && curves && curves->size() == 1 &&
                                   curves->front().parameterIndex == std::uint32_t(index) &&
                                   !curves->front().points.empty() &&
                                   near(curves->front().points.front().second,
                                        c.automationToPlain(explicitTarget, .35));
                        }
                        return false;
                    };
                    check(curvesMatch(), "runtime resolves an instrument's explicit stable slot ID");
                    c.setAutomationTarget(instrumentPreview.first, instrumentPreview.second, *instrumentTarget);
                    c.setAutomationPoints(instrumentPreview.first, instrumentPreview.second,
                                          {pt(0, .35), pt(4, .75)});
                    check(curvesMatch(), "legacy instrument automation still resolves after retargeting");
                }
            }
            c.newProject(false);
            check(!c.lastTouchedAutomation(track) &&
                  c.followTouchedAutomation() == daw::EngineController::AutomationFollowChange::None,
                  "a new project drops remembered and pending touches");
        }
        c.shutdown();
    }

    std::printf("\n%s\n", failures == 0 ? "ALL PASSED" : "FAILURES PRESENT");
    return failures;
}
