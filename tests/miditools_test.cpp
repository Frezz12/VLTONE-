// The piano roll's note maths, checked head-on.
//
// Every Tools and Edit command is a pure `vector<NoteModel>` transform in
// MidiTools.hpp, which is exactly why this test exists: no audio device, no Qt,
// no document — build a handful of notes, run the transform, read the result.
// The one document-level check at the end covers `setClipNotes`, the undoable
// primitive all of those commands land through.
#include "EngineController.hpp"
#include "MidiPreviewIndex.hpp"
#include "MidiTools.hpp"
#include "ProjectSerializer.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
namespace mt = daw::miditools;

static int failures = 0;
static bool check(bool cond, const char* what) {
    std::printf("%s  %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) ++failures;
    return cond;
}

static bool near(double a, double b, double tolerance = 1e-6) {
    return std::abs(a - b) <= tolerance;
}

static daw::NoteModel makeNote(const std::string& id, int pitch, double start,
                               double length, int velocity = 100) {
    daw::NoteModel n;
    n.id = id;
    n.pitch = pitch;
    n.startBeats = start;
    n.lengthBeats = length;
    n.velocity = velocity;
    return n;
}

/// The note in `notes` with the given id, or null.
static const daw::NoteModel* byId(const mt::Notes& notes, const std::string& id) {
    for (const auto& n : notes) {
        if (n.id == id) return &n;
    }
    return nullptr;
}

static size_t countWithEmptyId(const mt::Notes& notes) {
    size_t count = 0;
    for (const auto& n : notes) {
        if (n.id.empty()) ++count;
    }
    return count;
}

int main() {
    // ── Arrangement MIDI-preview culling ──
    // A dense clip may contain far more notes than can occupy the viewport.
    // Scrolling its compact overview must inspect a bounded tail, while a long
    // note beginning off-screen still has to be reported as visible.
    {
        mt::Notes notes;
        notes.reserve(100000);
        for (int i = 0; i < 100000; ++i) {
            notes.push_back(makeNote(std::to_string(i), 36 + (i % 60),
                                     double(i) * 0.25, 0.1));
        }
        notes.front().lengthBeats = 30000.0; // spans the late viewport

        daw::MidiPreviewIndex preview;
        preview.rebuild(notes);
        std::size_t inspected = 0;
        std::size_t visible = 0;
        bool foundLongNote = false;
        preview.forEachVisible(
            notes, 24000.0, 24001.0,
            [&](const daw::NoteModel& note, std::size_t) {
                ++visible;
                foundLongNote |= note.id == "0";
            },
            &inspected);
        check(foundLongNote && visible == 5,
              "the preview finds visible notes and an off-screen long note");
        check(inspected <= daw::MidiPreviewIndex::kNotesPerBlock * 2,
              "a late preview query does not scan the dense MIDI clip");
        check(preview.lowestPitch() == 36 && preview.highestPitch() == 95,
              "the preview caches the clip's fitted pitch range");

        std::swap(notes[1], notes.back());
        preview.rebuild(notes);
        bool foundMoved = false;
        preview.forEachVisible(notes, 0.2, 0.4,
                               [&](const daw::NoteModel& note, std::size_t) {
                                   foundMoved |= note.id == "1";
                               });
        check(foundMoved, "the preview index accepts unsorted note storage");
    }

    // ── Scales ──
    {
        check(mt::inScale(60, 0, mt::Scale::Major) &&
                  !mt::inScale(61, 0, mt::Scale::Major),
              "C is in C major and C# is not");
        check(mt::snapPitchToScale(61, 0, mt::Scale::Major) == 60,
              "an off-scale pitch snaps down to the nearest degree");
        check(mt::inScale(61, 1, mt::Scale::Major),
              "the same pitch is in scale once the root moves to C#");
        check(mt::transposeInScale(60, 1, 0, mt::Scale::Major) == 62 &&
                  mt::transposeInScale(64, 1, 0, mt::Scale::Major) == 65,
              "one scale degree up is a tone from C but a semitone from E");
        check(mt::transposeInScale(60, 7, 0, mt::Scale::Major) == 72 &&
                  mt::transposeInScale(60, -7, 0, mt::Scale::Major) == 48,
              "seven degrees is an octave either way");
        check(mt::gridBeatsFor(16, mt::GridFlavour::Straight) == 0.25 &&
                  near(mt::gridBeatsFor(8, mt::GridFlavour::Triplet), 1.0 / 3.0) &&
                  near(mt::gridBeatsFor(8, mt::GridFlavour::Dotted), 0.75),
              "grid divisions, triplets and dotted values");
    }

    // ── Quantize ──
    {
        mt::Notes notes = {
            makeNote("a", 60, 0.03, 0.5),
            makeNote("b", 62, 0.47, 0.5),
            makeNote("c", 64, 1.10, 0.3),
        };

        mt::QuantizeParams p;
        p.gridBeats = 0.25;
        mt::Notes hard = mt::quantize(notes, p);
        check(near(byId(hard, "a")->startBeats, 0.0) &&
                  near(byId(hard, "b")->startBeats, 0.5) &&
                  near(byId(hard, "c")->startBeats, 1.0),
              "full-strength quantize puts every start on the grid");
        check(near(byId(hard, "c")->lengthBeats, 0.3),
              "quantizing starts leaves lengths alone");

        p.strength = 0.5;
        mt::Notes half = mt::quantize(notes, p);
        check(near(byId(half, "a")->startBeats, 0.015) &&
                  near(byId(half, "c")->startBeats, 1.05),
              "half strength moves a note half of the way to the grid");

        p.strength = 1.0;
        p.toleranceBeats = 0.05;
        mt::Notes tolerant = mt::quantize(notes, p);
        check(near(byId(tolerant, "a")->startBeats, 0.03) &&
                  near(byId(tolerant, "c")->startBeats, 1.0),
              "the dead zone spares a note already close to the grid");

        p.toleranceBeats = 0.0;
        p.target = mt::QuantizeParams::Target::End;
        mt::Notes ends = mt::quantize(notes, p);
        check(near(byId(ends, "a")->startBeats, 0.03) &&
                  near(byId(ends, "a")->startBeats + byId(ends, "a")->lengthBeats, 0.5),
              "quantizing ends holds the start and rewrites the length");

        p.target = mt::QuantizeParams::Target::Length;
        mt::Notes lengths = mt::quantize(notes, p);
        check(near(byId(lengths, "c")->startBeats, 1.10) &&
                  near(byId(lengths, "c")->lengthBeats, 0.25),
              "quantizing the length holds the start");

        // Swing: at 75% the off-eighth is pushed a quarter of an eighth late.
        mt::QuantizeParams swung;
        swung.gridBeats = 0.5;
        swung.swing = 0.75;
        swung.swingUnitBeats = 0.5;
        mt::Notes swingNotes = {makeNote("x", 60, 0.0, 0.5),
                                makeNote("y", 60, 0.52, 0.5)};
        mt::Notes swingOut = mt::quantize(swingNotes, swung);
        check(near(byId(swingOut, "x")->startBeats, 0.0) &&
                  near(byId(swingOut, "y")->startBeats, 0.625),
              "swing delays the off-beat and leaves the down-beat alone");

        // Order preservation: two notes at the same pitch that would swap.
        mt::Notes crowd = {makeNote("first", 60, 0.24, 0.1),
                           makeNote("second", 60, 0.26, 0.1)};
        mt::QuantizeParams order;
        order.gridBeats = 0.5;
        order.preserveOrder = true;
        mt::Notes kept = mt::quantize(crowd, order);
        check(byId(kept, "first")->startBeats <= byId(kept, "second")->startBeats,
              "preserve order stops same-pitch notes from swapping places");

        // A groove is deterministic, and its velocity map is taken on.
        mt::QuantizeParams grooved;
        grooved.gridBeats = 0.5;
        grooved.groove = mt::groovePresets()[1];   // Swing 8ths
        mt::Notes a = mt::quantize(swingNotes, grooved);
        mt::Notes b = mt::quantize(swingNotes, grooved);
        check(near(byId(a, "y")->startBeats, byId(b, "y")->startBeats) &&
                  byId(a, "y")->startBeats > 0.5,
              "a groove drags the off-beat behind the grid, repeatably");
        check(byId(a, "y")->velocity < byId(a, "x")->velocity,
              "the groove's velocity map accents the down-beat");
    }

    // ── Arpeggiator ──
    {
        const mt::Notes chord = {makeNote("r", 60, 0.0, 4.0),
                                 makeNote("t", 64, 0.0, 4.0),
                                 makeNote("f", 67, 0.0, 4.0)};
        mt::ArpParams p;
        p.rateBeats = 0.5;
        p.playMode = mt::ArpParams::PlayMode::Loop;
        mt::Notes up = mt::arpeggiate(chord, p, 8.0);
        check(up.size() == 8, "a 4-beat chord at 1/8 gives eight slots");
        check(up[0].pitch == 60 && up[1].pitch == 64 && up[2].pitch == 67 &&
                  up[3].pitch == 60,
              "Up walks the chord and wraps");
        check(near(up[1].startBeats, 0.5) && near(up[1].lengthBeats, 0.5 * p.gate),
              "slots land on the rate and the gate sets the length");
        check(countWithEmptyId(up) == up.size(),
              "arpeggiated notes carry no id, so setClipNotes mints fresh ones");

        p.direction = mt::ArpParams::Direction::Down;
        mt::Notes down = mt::arpeggiate(chord, p, 8.0);
        check(down[0].pitch == 67 && down[1].pitch == 64 && down[2].pitch == 60,
              "Down walks the other way");

        p.direction = mt::ArpParams::Direction::UpDown;
        mt::Notes updown = mt::arpeggiate(chord, p, 8.0);
        check(updown[0].pitch == 60 && updown[1].pitch == 64 &&
                  updown[2].pitch == 67 && updown[3].pitch == 64 &&
                  updown[4].pitch == 60,
              "Up/Down turns around without repeating the top note");

        p.direction = mt::ArpParams::Direction::Up;
        p.octaves = 2;
        mt::Notes octaves = mt::arpeggiate(chord, p, 8.0);
        check(octaves[3].pitch == 72 && octaves[5].pitch == 79,
              "the octave range stacks the chord above itself");

        p.octaves = 1;
        p.playMode = mt::ArpParams::PlayMode::Once;
        check(mt::arpeggiate(chord, p, 8.0).size() == 3,
              "Once plays the pool through exactly once");

        p.playMode = mt::ArpParams::PlayMode::Fill;
        check(mt::arpeggiate(chord, p, 8.0).size() == 16,
              "Fill runs to the end of the region, not of the chord");

        p.playMode = mt::ArpParams::PlayMode::Loop;
        p.merge = true;
        check(mt::arpeggiate(chord, p, 8.0).size() == 8 + chord.size(),
              "Merge keeps the source chord underneath");

        p.merge = false;
        p.steps = {{100, false, false, 0}, {100, true, false, 0}};   // 2nd skipped
        check(mt::arpeggiate(chord, p, 8.0).size() == 4,
              "a skipped step drops its slot");

        p.steps = {{100, false, false, 0}, {100, false, true, 0}};   // 2nd tied
        mt::Notes tied = mt::arpeggiate(chord, p, 8.0);
        check(tied.size() == 4 && near(tied[0].lengthBeats, 0.5 + 0.5 * p.gate),
              "a tied step lengthens the note before it instead of restriking");

        p.steps.clear();
        p.direction = mt::ArpParams::Direction::Chord;
        check(mt::arpeggiate(chord, p, 8.0).size() == 8 * 3,
              "Chord mode restrikes the whole chord on every slot");

        p.direction = mt::ArpParams::Direction::Random;
        p.seed = 7;
        const mt::Notes r1 = mt::arpeggiate(chord, p, 8.0);
        const mt::Notes r2 = mt::arpeggiate(chord, p, 8.0);
        bool identical = r1.size() == r2.size();
        for (size_t i = 0; identical && i < r1.size(); ++i) {
            identical = r1[i].pitch == r2[i].pitch;
        }
        check(identical, "the same seed gives the same random arpeggio");
    }

    // ── Glue ──
    {
        mt::Notes run = {makeNote("a", 60, 0.0, 1.0), makeNote("b", 60, 1.0, 1.0),
                         makeNote("c", 60, 2.5, 1.0), makeNote("d", 64, 0.0, 1.0)};
        mt::GlueParams p;
        mt::Notes merged = mt::glue(run, p);
        check(merged.size() == 3, "touching same-pitch notes become one");
        check(byId(merged, "a") && near(byId(merged, "a")->lengthBeats, 2.0),
              "the merged note keeps the head's id and runs to the tail's end");
        check(byId(merged, "c") != nullptr && byId(merged, "d") != nullptr,
              "a note past the gap, and one at another pitch, are left alone");

        p.gapToleranceBeats = 0.5;
        check(mt::glue(run, p).size() == 2,
              "a gap tolerance pulls the straggler into the run");

        p.gapToleranceBeats = 0.0;
        p.samePitchOnly = false;
        check(mt::glue(run, p).size() < 3,
              "ignoring pitch merges across pitches too");

        mt::Notes detached = {makeNote("a", 60, 0.0, 0.25),
                              makeNote("b", 62, 1.0, 0.25),
                              makeNote("c", 64, 3.0, 0.25)};
        mt::GlueParams legato;
        legato.mode = mt::GlueParams::Mode::Legato;
        mt::Notes joined = mt::glue(detached, legato);
        check(near(byId(joined, "a")->lengthBeats, 1.0) &&
                  near(byId(joined, "b")->lengthBeats, 2.0) &&
                  near(byId(joined, "c")->lengthBeats, 0.25),
              "legato stretches each note to the next and leaves the last one");

        legato.legatoMaxBeats = 1.5;
        check(near(mt::glue(detached, legato)[1].lengthBeats, 1.5),
              "the legato ceiling caps a long stretch");

        // Quick Legato measures "the next note" against the whole clip, which is
        // what makes it work on a partial selection — and on a lone note, where
        // the old size<2 guard made the command a no-op.
        mt::Notes one = {makeNote("a", 60, 0.0, 0.25)};
        check(near(mt::legato(one, detached)[0].lengthBeats, 1.0),
              "one selected note stretches to the next note in the clip");

        mt::Notes ends = {makeNote("a", 60, 0.0, 0.25),
                          makeNote("c", 64, 3.0, 0.25)};
        mt::Notes stretched = mt::legato(ends, detached);
        check(near(byId(stretched, "a")->lengthBeats, 1.0),
              "a selection stops at the unselected note in its way");
        check(near(byId(stretched, "c")->lengthBeats, 0.25),
              "the clip's last note keeps its length even when selected");
    }

    // ── Articulate ──
    {
        // Four notes on the beat, each far shorter than the gap to the next.
        mt::Notes run = {makeNote("a", 60, 0.0, 0.25), makeNote("b", 62, 1.0, 0.25),
                         makeNote("c", 64, 2.0, 0.25), makeNote("d", 65, 3.0, 0.5)};

        mt::ArticulateParams p;
        p.gate = 1.0;
        mt::Notes tight = mt::articulate(run, p);
        check(near(byId(tight, "a")->lengthBeats, 1.0) &&
                  near(byId(tight, "b")->lengthBeats, 1.0),
              "a full gate runs each note into the next");
        // The last note has no "next" to measure against, so it gates against
        // its own end rather than collapsing to the minimum.
        check(near(byId(tight, "d")->lengthBeats, 0.5),
              "and the last note keeps the length it had");

        p.gate = 0.5;
        mt::Notes staccato = mt::articulate(run, p);
        check(near(byId(staccato, "a")->lengthBeats, 0.5),
              "half a gate leaves half the gap silent");

        // Amount is the whole point of the parameter: it must interpolate, not
        // switch. Halfway from 0.25 to 0.5 is 0.375.
        p.amount = 0.5;
        check(near(byId(mt::articulate(run, p), "a")->lengthBeats, 0.375),
              "amount blends toward the gate instead of jumping to it");

        p.amount = 1.0;
        p.maxLengthBeats = 0.3;
        check(near(byId(mt::articulate(run, p), "a")->lengthBeats, 0.3),
              "the ceiling caps what the gate would otherwise reach");

        mt::ArticulateParams keep;
        keep.lengthMode = mt::ArticulateParams::LengthMode::Keep;
        keep.accentEvery = 2;
        keep.accentVelocity = 20;
        keep.otherVelocity = -10;
        mt::Notes accented = mt::articulate(run, keep);
        check(near(byId(accented, "a")->lengthBeats, 0.25),
              "Keep leaves every length exactly as it was");
        check(byId(accented, "a")->velocity == 120 &&
                  byId(accented, "b")->velocity == 90 &&
                  byId(accented, "c")->velocity == 120,
              "every other note is accented, in time order");

        mt::ArticulateParams scale;
        scale.lengthMode = mt::ArticulateParams::LengthMode::Scale;
        scale.gate = 2.0;
        check(near(byId(mt::articulate(run, scale), "a")->lengthBeats, 0.5),
              "Scale works off the note's own length, not the gap");
    }

    // ── Rotate ──
    {
        // A bar of four quarters. The last note is short, so the notes' own span
        // is 3.5 beats — which is exactly why the caller passes the real cycle.
        mt::Notes bar = {makeNote("a", 60, 0.0, 1.0), makeNote("b", 62, 1.0, 1.0),
                         makeNote("c", 64, 2.0, 1.0), makeNote("d", 65, 3.0, 0.5)};

        mt::Notes right = mt::rotate(bar, 1.0, 4.0);
        check(near(byId(right, "a")->startBeats, 1.0) &&
                  near(byId(right, "b")->startBeats, 2.0) &&
                  near(byId(right, "c")->startBeats, 3.0) &&
                  near(byId(right, "d")->startBeats, 0.0),
              "rotating right moves everything on, and wraps the last note to the start");
        check(near(byId(right, "d")->lengthBeats, 0.5),
              "a wrapped note keeps its length");
        check(right.size() == bar.size() && countWithEmptyId(right) == 0,
              "nothing is created or destroyed, so the selection survives");

        mt::Notes left = mt::rotate(bar, -1.0, 4.0);
        check(near(byId(left, "a")->startBeats, 3.0) &&
                  near(byId(left, "b")->startBeats, 0.0),
              "rotating left wraps the first note to the end");

        // The pair has to compose back to where it started, or "rotate back" is
        // not an undo of "rotate".
        mt::Notes there_and_back = mt::rotate(mt::rotate(bar, 1.0, 4.0), -1.0, 4.0);
        bool restored = true;
        for (const auto& note : bar) {
            const auto* back = byId(there_and_back, note.id);
            if (!back || !near(back->startBeats, note.startBeats)) restored = false;
        }
        check(restored, "rotating one way and back leaves the bar untouched");

        check(near(byId(mt::rotate(bar, 4.0, 4.0), "a")->startBeats, 0.0),
              "a full turn is the identity");
        check(near(byId(mt::rotate(bar, 9.0, 4.0), "a")->startBeats, 1.0),
              "and more than a full turn wraps rather than running away");

        // Without an explicit cycle it falls back to the notes' own span, which
        // is the documented behaviour rather than an accident.
        check(near(byId(mt::rotate(bar, 3.5), "a")->startBeats, 0.0),
              "the derived cycle is the span from the first start to the last end");

        // Simultaneous notes have no rhythm to turn, and their derived cycle is
        // just their length — which is exactly why a caller that means "a bar"
        // has to say so. Rotating them by the cycle is still the identity.
        mt::Notes chord = {makeNote("x", 60, 1.0, 1.0), makeNote("y", 64, 1.0, 1.0)};
        check(near(mt::rotate(chord, 1.0)[0].startBeats, 1.0) &&
                  near(mt::rotate(chord, 1.0)[1].startBeats, 1.0),
              "a chord turned by its own cycle stays where it is");
        check(near(mt::rotate(chord, 4.0, 4.0)[0].startBeats, 1.0),
              "and a chord given a real bar to turn in wraps within that bar");
    }

    // ── Strum ──
    {
        mt::Notes chord = {makeNote("low", 60, 1.0, 2.0),
                           makeNote("mid", 64, 1.0, 2.0),
                           makeNote("high", 67, 1.0, 2.0),
                           makeNote("later", 72, 2.5, 1.0)};
        mt::StrumParams p;
        p.spanBeats = 0.2;
        mt::Notes up = mt::strum(chord, p);
        check(near(byId(up, "low")->startBeats, 1.0) &&
                  near(byId(up, "mid")->startBeats, 1.1) &&
                  near(byId(up, "high")->startBeats, 1.2),
              "Up spreads the chord from the bottom note over the span");
        check(near(byId(up, "later")->startBeats, 2.5),
              "a note outside the chord window is not part of the strum");
        check(near(byId(up, "high")->lengthBeats, 1.8),
              "by default the ends stay put, so the chord finishes together");

        p.adjustEnds = true;
        check(near(byId(mt::strum(chord, p), "high")->lengthBeats, 2.0),
              "adjusting the ends preserves each note's length instead");

        p.adjustEnds = false;
        p.direction = mt::StrumParams::Direction::Down;
        mt::Notes down = mt::strum(chord, p);
        check(near(byId(down, "high")->startBeats, 1.0) &&
                  near(byId(down, "low")->startBeats, 1.2),
              "Down starts from the top note");

        p.direction = mt::StrumParams::Direction::Up;
        p.velocityTaper = -0.5;
        mt::Notes tapered = mt::strum(chord, p);
        check(byId(tapered, "high")->velocity < byId(tapered, "low")->velocity,
              "a negative taper fades the strum out across the chord");
    }

    // ── Randomize / humanize ──
    {
        const mt::Notes source = {makeNote("a", 60, 0.0, 1.0),
                                  makeNote("b", 60, 1.0, 1.0),
                                  makeNote("c", 60, 2.0, 1.0),
                                  makeNote("d", 60, 3.0, 1.0)};
        mt::RandomParams p;
        p.velocity = true;
        p.velocityAmount = 20;
        p.seed = 42;
        const mt::Notes r1 = mt::randomize(source, p);
        const mt::Notes r2 = mt::randomize(source, p);
        bool same = true, moved = false;
        for (size_t i = 0; i < r1.size(); ++i) {
            same = same && r1[i].velocity == r2[i].velocity;
            moved = moved || r1[i].velocity != source[i].velocity;
        }
        check(same && moved, "randomised velocity moves, and repeats on the seed");

        bool inRange = true;
        for (const auto& n : r1) {
            inRange = inRange && std::abs(n.velocity - 100) <= 20;
        }
        check(inRange, "velocity stays inside the requested spread");

        p.velocity = false;
        p.pitch = true;
        p.pitchAmount = 3;
        p.scaleAware = true;
        p.scale = mt::Scale::Major;
        p.scaleRoot = 0;
        bool allInScale = true;
        for (const auto& n : mt::randomize(source, p)) {
            allInScale = allInScale && mt::inScale(n.pitch, 0, mt::Scale::Major);
        }
        check(allInScale, "scale-aware randomisation never leaves the scale");

        p.pitch = false;
        p.timing = true;
        p.timingBeats = 0.4;
        p.preserveTotalDuration = true;
        p.regionEndBeats = 4.0;
        bool inside = true;
        for (const auto& n : mt::randomize(source, p)) {
            inside = inside && n.startBeats + n.lengthBeats <= 4.0 + 1e-9;
        }
        check(inside, "preserving the total duration keeps notes inside the clip");

        p.constrainToGrid = true;
        p.gridBeats = 0.5;
        bool onGrid = true;
        for (const auto& n : mt::randomize(source, p)) {
            onGrid = onGrid && near(std::fmod(n.startBeats + 1e-9, 0.5), 0.0, 1e-6);
        }
        check(onGrid, "constrain-to-grid rounds the jitter back onto the grid");

        const mt::Notes human = mt::humanize(source, 0.05, 10, 3);
        bool touched = false;
        for (size_t i = 0; i < human.size(); ++i) {
            touched = touched || !near(human[i].startBeats, source[i].startBeats);
        }
        check(touched, "humanize nudges the timing");
    }

    // ── Chords ──
    {
        const mt::Notes single = {makeNote("root", 60, 0.0, 1.0)};
        mt::ChordParams p;
        mt::Notes triad = mt::buildChords(single, p);
        check(triad.size() == 3 && triad[0].pitch == 60 && triad[1].pitch == 64 &&
                  triad[2].pitch == 67,
              "a major triad is built on the note");
        check(triad[0].id == "root" && countWithEmptyId(triad) == 2,
              "the root keeps its identity and the added voices are new notes");

        p.type = mt::ChordParams::Type::Minor7;
        check(mt::buildChords(single, p).size() == 4,
              "a seventh chord has four voices");

        p.type = mt::ChordParams::Type::Major;
        p.inversion = 1;
        mt::Notes inverted = mt::buildChords(single, p);
        check(inverted[0].pitch == 72 && inverted[1].pitch == 64,
              "the first inversion lifts the root an octave");

        p.inversion = 0;
        p.bassOctave = true;
        check(mt::buildChords(single, p).front().pitch == 48,
              "the bass octave adds a root below the chord");
    }

    // ── Single-purpose edits ──
    {
        const mt::Notes source = {makeNote("a", 60, 0.0, 1.0, 80),
                                  makeNote("b", 67, 1.0, 0.5, 100),
                                  makeNote("c", 64, 2.0, 2.0, 120)};

        check(mt::transpose(source, 12)[0].pitch == 72, "transpose by semitones");
        check(mt::transposeInScale(source, 1, 0, mt::Scale::Major)[0].pitch == 62,
              "transpose by scale degrees");

        const mt::Notes inverted = mt::invertPitch(source);
        check(inverted[0].pitch == 67 && inverted[1].pitch == 60 &&
                  inverted[2].pitch == 63,
              "inversion mirrors the pitches about the middle of the selection");

        const mt::Notes reversed = mt::reverseTime(source);
        check(near(byId(reversed, "c")->startBeats, 0.0) &&
                  near(byId(reversed, "a")->startBeats, 3.0),
              "reversing time turns the phrase around inside its own span");

        check(mt::setVelocity(source, 64)[0].velocity == 64 &&
                  mt::addVelocity(source, 20)[0].velocity == 100 &&
                  mt::addVelocity(source, 50)[2].velocity == 127,
              "velocity set and offset, clamped at the top");
        check(mt::scaleVelocity(source, 0.5)[2].velocity == 60,
              "velocity scaling is proportional");

        const mt::Notes ramp = mt::rampVelocity(source, 20, 120);
        check(byId(ramp, "a")->velocity == 20 && byId(ramp, "b")->velocity == 70 &&
                  byId(ramp, "c")->velocity == 120,
              "a velocity ramp runs across the selection in time order");

        check(near(mt::setLength(source, 0.25)[1].lengthBeats, 0.25) &&
                  near(mt::scaleLength(source, 2.0)[1].lengthBeats, 1.0),
              "length set and scale");
        check(near(mt::nudge(source, 0.5)[0].startBeats, 0.5) &&
                  near(mt::nudge(source, -5.0)[0].startBeats, 0.0),
              "nudge shifts in time and never goes negative");

        check(mt::limitPitch(source, 60, 72)[2].pitch == 64 &&
                  mt::limitPitch(source, 48, 59)[1].pitch == 55,
              "limiting folds by octaves and keeps the pitch class");

        // Only "c" (2.0 → 4.0) crosses a bar line; a note that merely *ends* on
        // one is left whole, which is why this is 4 notes and not 5.
        const mt::Notes split = mt::splitAtGrid(source, 1.0);
        check(split.size() == 4 && countWithEmptyId(split) == 1,
              "splitting at the grid cuts the crossing note and ids only the new piece");
        const mt::Notes cut = mt::splitAt(source, 2.5);
        check(cut.size() == 4 && near(cut[3].startBeats, 2.5) &&
                  near(cut[2].lengthBeats, 0.5),
              "splitting at a point only touches the note that spans it");

        check(mt::setMuted(source, true)[0].muted &&
                  !mt::toggleMuted(mt::setMuted(source, true))[0].muted,
              "mute and toggle");
        check(mt::setColor(source, 0xFF8800u)[1].color == 0xFF8800u, "note colour");

        double start = 0.0, end = 0.0;
        mt::spanOf(source, &start, &end);
        check(near(start, 0.0) && near(end, 4.0), "the span covers every note");
        check(mt::pitchName(60) == "C5" && mt::pitchName(61) == "C#5",
              "pitch naming matches the keyboard (FL numbering: 60 is C5)");
    }

    // ── setClipNotes: the undoable landing point ──
    {
        daw::EngineController controller;
        controller.initialize(48000.0, 512, false);
        const std::string track = controller.addTrack(daw::TrackKind::Midi, "Keys");
        const std::string clip = controller.addMidiClip(track, 0.0, 4.0);
        check(!clip.empty(), "a MIDI clip to edit");

        controller.addNote(track, clip, 60, 0.03, 1.0);
        controller.addNote(track, clip, 64, 1.05, 1.0);
        const auto* before = controller.project().findTrack(track);
        const std::string firstId = before->clips.front().notes.front().id;

        mt::QuantizeParams p;
        p.gridBeats = 1.0;
        controller.setClipNotes(track, clip,
                                mt::quantize(before->clips.front().notes, p),
                                "Quantize");

        auto notesNow = [&] {
            return controller.project().findTrack(track)->clips.front().notes;
        };
        check(notesNow().size() == 2 && near(notesNow()[0].startBeats, 0.0) &&
                  near(notesNow()[1].startBeats, 1.0),
              "the quantised vector lands in the document");
        check(notesNow().front().id == firstId,
              "a note that survived the transform kept its id");
        check(controller.undoLabel() == "Quantize",
              "the whole transform is one labelled undo entry");

        controller.undo();
        check(near(notesNow()[0].startBeats, 0.03) &&
                  near(notesNow()[1].startBeats, 1.05),
              "undo restores every note at once");
        controller.redo();
        check(near(notesNow()[0].startBeats, 0.0) &&
                  notesNow().front().id == firstId,
              "redo replays the captured vector, ids included");

        // Notes arriving with no id (arpeggios, chords, split halves) must come
        // out with distinct uuids, or the roll's selection breaks on them.
        mt::Notes fresh = mt::buildChords({notesNow().front()}, mt::ChordParams{});
        controller.setClipNotes(track, clip, fresh, "Chord");
        const auto after = notesNow();
        bool allIded = true;
        for (size_t i = 0; i < after.size(); ++i) {
            allIded = allIded && !after[i].id.empty();
            for (size_t j = i + 1; j < after.size(); ++j) {
                allIded = allIded && after[i].id != after[j].id;
            }
        }
        check(after.size() == 3 && allIded,
              "setClipNotes mints a unique uuid for every created note");

        // Out-of-range values are the transform's right to produce; clamping is
        // the controller's job on the way in.
        mt::Notes wild = after;
        wild[0].pitch = 300;
        wild[1].velocity = 999;
        wild[2].startBeats = -4.0;
        controller.setClipNotes(track, clip, wild, "Wild");
        const auto clamped = notesNow();
        check(clamped[0].pitch == 127 && clamped[1].velocity == 127 &&
                  near(clamped[2].startBeats, 0.0),
              "setClipNotes clamps pitch, velocity and time into range");

        // ── Controller lanes ──
        const std::string laneId =
            controller.addControllerLane(track, clip, "Mod Wheel", 1);
        check(!laneId.empty(), "a controller lane is created");
        auto lanes = [&] {
            return controller.project().findTrack(track)->clips.front().lanes;
        };
        check(lanes().size() == 1 && lanes().front().cc == 1,
              "the lane lands on the clip with its CC number");

        // Points arrive unsorted and with a duplicate time, exactly as a drag
        // that dumped one point on top of another would leave them.
        std::vector<daw::AutomationPoint> drawn = {
            {2.0, 0.9}, {0.5, 0.2}, {2.0, 0.4}, {1.0, 5.0}, {-1.0, 0.5}};
        controller.setLanePoints(track, clip, laneId, drawn);
        const auto points = lanes().front().points;
        check(points.size() == 4, "points at the same beat collapse to one");
        bool sorted = true, bounded = true;
        for (size_t i = 0; i < points.size(); ++i) {
            bounded = bounded && points[i].value >= 0.0 && points[i].value <= 1.0 &&
                      points[i].beats >= 0.0;
            if (i) sorted = sorted && points[i - 1].beats <= points[i].beats;
        }
        check(sorted && bounded, "the curve is sorted and clamped on the way in");

        controller.commitLaneEdit(track, clip, laneId, {}, "Draw Curve");
        check(controller.undoLabel() == "Draw Curve",
              "a finished curve gesture is one labelled undo entry");
        controller.undo();
        check(lanes().front().points.empty(),
              "undo restores the curve as it was before the gesture");
        controller.redo();
        check(lanes().front().points.size() == 4, "redo puts the curve back");

        // A no-op gesture must not litter the undo stack.
        controller.commitLaneEdit(track, clip, laneId, lanes().front().points,
                                  "Draw Curve");
        check(controller.undoLabel() == "Draw Curve",
              "committing an unchanged curve records nothing");

        // ── Mute and colour survive a save/reload ──
        mt::Notes decorated = mt::setColor(mt::setMuted(clamped, true), 0x30D158u);
        decorated.front().pan = -0.75f;
        controller.setClipNotes(track, clip, decorated, "Mute + colour");
        const fs::path dir = fs::temp_directory_path() / "daw_miditools_test";
        fs::remove_all(dir);
        fs::create_directories(dir);
        const std::string path = (dir / "project.dawproject").string();
        check(bool(daw::ProjectSerializer::save(controller.project(), path)),
              "project saves");

        daw::ProjectModel reloaded;
        check(bool(daw::ProjectSerializer::load(reloaded, path)), "project reloads");
        const auto& loadedClip = reloaded.findTrack(track)->clips.front();
        const auto& loadedNotes = loadedClip.notes;
        check(loadedNotes.size() == 3 && loadedNotes.front().muted &&
                  loadedNotes.front().color == 0x30D158u,
              "a note's muted flag and colour round-trip through the file");
        check(std::abs(loadedNotes.front().pan + 0.75f) < 1e-6,
              "per-note pan round-trips too");
        check(loadedClip.lanes.size() == 1 &&
                  loadedClip.lanes.front().name == "Mod Wheel" &&
                  loadedClip.lanes.front().points.size() == 4,
              "a controller lane and its curve round-trip through the file");
        fs::remove_all(dir);

        controller.shutdown();
    }

    // ── Harmonic analysis ──
    //
    // What the assistant reads before it writes a bass part. A four-bar
    // Am–F–C–G, voiced the way somebody would actually play it, has to come
    // back as those four roots — everything the bass playbook says rests on it.
    {
        mt::Notes progression;
        const struct { int root; int third; int fifth; } chords[4] = {
            {57, 60, 64},   // A minor
            {53, 57, 60},   // F major
            {48, 52, 55},   // C major
            {55, 59, 62},   // G major
        };
        for (int bar = 0; bar < 4; ++bar) {
            const double at = bar * 4.0;
            progression.push_back(makeNote("", chords[bar].root, at, 4.0));
            progression.push_back(makeNote("", chords[bar].third, at, 4.0));
            progression.push_back(makeNote("", chords[bar].fifth, at, 4.0));
        }

        const std::vector<mt::HarmonySegment> segments =
            mt::analyzeHarmony(progression, 4.0);
        check(segments.size() == 4, "one harmony segment per bar");
        if (segments.size() == 4) {
            check(segments[0].root == 9 && segments[0].quality == "minor",
                  "bar 1 reads as A minor");
            check(segments[1].root == 5 && segments[1].quality == "major",
                  "bar 2 reads as F major");
            check(segments[2].root == 0 && segments[2].quality == "major",
                  "bar 3 reads as C major");
            check(segments[3].root == 7 && segments[3].quality == "major",
                  "bar 4 reads as G major");
            check(segments[0].bassPitch == 57 && segments[2].bassPitch == 48,
                  "the lowest sounding note is reported per segment");
            check(near(segments[1].startBeats, 4.0) &&
                      near(segments[1].lengthBeats, 4.0),
                  "segments are one bar long and start on the bar line");
        }

        // A pad held across a bar line belongs to both bars, not only the one
        // it starts in.
        mt::Notes held;
        held.push_back(makeNote("", 60, 0.0, 8.0));
        held.push_back(makeNote("", 64, 0.0, 8.0));
        held.push_back(makeNote("", 67, 0.0, 8.0));
        const auto sustained = mt::analyzeHarmony(held, 4.0);
        check(sustained.size() == 2 && sustained[1].root == 0 &&
                  sustained[1].quality == "major",
              "a note sounding through a segment counts in it");

        // Sevenths beat the triad hiding inside them.
        mt::Notes seventh;
        seventh.push_back(makeNote("", 43, 0.0, 4.0));
        seventh.push_back(makeNote("", 47, 0.0, 4.0));
        seventh.push_back(makeNote("", 50, 0.0, 4.0));
        seventh.push_back(makeNote("", 53, 0.0, 4.0));
        const auto dominant = mt::analyzeHarmony(seventh, 4.0);
        check(dominant.size() == 1 && dominant[0].root == 7 &&
                  dominant[0].quality == "dominant7",
              "G B D F reads as a dominant seventh, not a G triad");

        const auto silence = mt::analyzeHarmony({}, 4.0);
        check(silence.empty(), "no notes, no segments");

        const mt::KeyGuess key = mt::estimateKey(progression);
        check(key.root == 0 && key.scale == mt::Scale::Major,
              "Am-F-C-G estimates as C major");
        check(mt::pitchClassName(9) == "A" && mt::pitchClassName(6) == "F#",
              "pitch classes are named without an octave");
    }

    std::printf("\n%s\n", failures == 0 ? "ALL PASSED" : "FAILURES PRESENT");
    return failures;
}
