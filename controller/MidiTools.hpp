#pragma once

#include "model/Document.hpp"

#include <cstdint>
#include <string>
#include <vector>

// Note transforms — the arithmetic behind the piano roll's Tools and Edit
// menus, as pure functions over a vector of notes.
//
// Nothing here touches the document, the undo stack, the controller or Qt: a
// transform takes the notes it should act on (normally the selection, or the
// whole clip when nothing is selected) and returns the replacement. The caller
// splices the result back and records one undo entry via
// `EngineController::setClipNotes`. That split is what makes every one of these
// testable head-on, and what lets a parameter dialog run the same code on a
// throwaway copy to draw a live preview.
//
// Conventions shared by all of them:
//   • Times are beats measured from the clip's start, exactly like NoteModel.
//   • A transform that *keeps* a note keeps its id, so a selection survives it.
//     A transform that *creates* one leaves the id empty — `setClipNotes` mints
//     a fresh uuid, and nothing can accidentally end up with a duplicate.
//   • Anything random takes an explicit `seed`, so a preview and the apply that
//     follows it produce byte-identical results, and so tests are stable.
//   • Ranges are clamped by `setClipNotes`, not here; a transform may return a
//     pitch of -3 and rely on that.
namespace daw::miditools {

using Notes = std::vector<NoteModel>;

// ── Scales ──────────────────────────────────────────────────────────────────

enum class Scale {
    Chromatic,
    Major,
    NaturalMinor,
    HarmonicMinor,
    MelodicMinor,
    Dorian,
    Phrygian,
    Lydian,
    Mixolydian,
    Locrian,
    PentatonicMajor,
    PentatonicMinor,
    Blues,
    WholeTone,
};

/// Display name, e.g. "Natural Minor".
std::string scaleName(Scale scale);
/// The stable lower-case id a project file and the AI tools use, e.g.
/// "natural_minor". Separate from `scaleName` on purpose: a display name is
/// free to be reworded, a stored id is not.
std::string scaleId(Scale scale);
Scale scaleFromId(const std::string& id);
/// Every scale in menu order.
const std::vector<Scale>& allScales();
/// Semitone offsets of the scale's degrees from its root, ascending, 0-based.
const std::vector<int>& scaleDegrees(Scale scale);
/// True when `pitch` is a degree of `scale` rooted at pitch class `root` (0=C).
bool inScale(int pitch, int root, Scale scale);
/// Nearest pitch that is in the scale. Ties round down, so a run of chromatic
/// notes collapses predictably rather than alternating.
int snapPitchToScale(int pitch, int root, Scale scale);
/// Move `pitch` by `degrees` steps *along* the scale rather than in semitones.
int transposeInScale(int pitch, int degrees, int root, Scale scale);

// ── Grid vocabulary ─────────────────────────────────────────────────────────

/// How a base division is modified. Straight = as written, Triplet = two thirds
/// of it, Dotted = one and a half.
enum class GridFlavour { Straight, Triplet, Dotted };

/// Beats for a division named by its denominator (1 = whole note = 4 beats,
/// 4 = quarter = 1 beat, 16 = sixteenth = 0.25), with the flavour applied.
double gridBeatsFor(int denominator, GridFlavour flavour);

// ── Groove templates ────────────────────────────────────────────────────────

/// A repeating map of timing pushes and dynamic accents, sampled at `slots`
/// evenly spaced points across `lengthBeats`.
///
/// This is what "groove quantize" applies on top of the plain grid: the grid
/// says where the beat is, the groove says how far off it the player actually
/// sat and how hard they hit it.
struct Groove {
    std::string name;
    double lengthBeats = 4.0;
    /// Timing offset per slot, in beats. Positive drags behind the beat.
    std::vector<double> offsets;
    /// Velocity multiplier per slot. Empty or 1.0 leaves velocity alone.
    std::vector<double> velocities;

    bool empty() const { return offsets.empty() && velocities.empty(); }
};

/// The built-in groove presets (Straight, Swing 8ths, Swing 16ths, MPC 54,
/// Latin, Laid Back, Push).
const std::vector<Groove>& groovePresets();

// ── Quantize ────────────────────────────────────────────────────────────────

struct QuantizeParams {
    /// What gets pulled to the grid.
    enum class Target {
        Start,        ///< note starts; lengths ride along unchanged
        End,          ///< note ends; starts stay put, so lengths change
        Length,       ///< the length itself, start unchanged
        StartLength,  ///< start first, then the length
        StartEnd,     ///< both independently — can open gaps, which is the point
    };

    double gridBeats = 0.25;
    /// 0 = leave alone, 1 = sit exactly on the grid. Anything between moves the
    /// note that fraction of the way, which is how a part keeps its feel.
    double strength = 1.0;
    Target target = Target::Start;
    /// 0.5 = straight. Above that the odd `swingUnitBeats` slots are pushed late.
    double swing = 0.5;
    double swingUnitBeats = 0.5;
    /// Notes already this close to the grid are left untouched — the dead zone
    /// that stops quantise from flattening deliberate detail.
    double toleranceBeats = 0.0;
    /// Stop notes on the same pitch from swapping places or stacking.
    bool preserveOrder = true;
    /// Uniform ± jitter applied after the grid, to take the machine edge off.
    double randomizeBeats = 0.0;
    Groove groove;
    /// How much of the groove's timing and velocity is taken on, 0 … 1.
    double grooveTiming = 1.0;
    double grooveVelocity = 1.0;
    uint32_t seed = 1;
};

Notes quantize(Notes notes, const QuantizeParams& params);

// ── Arpeggiator ─────────────────────────────────────────────────────────────

struct ArpParams {
    enum class Direction { Up, Down, UpDown, DownUp, Random, Chord };
    enum class PlayMode {
        Once,   ///< one pass through the pitch pool and stop
        Loop,   ///< repeat for as long as the source chord lasts
        Fill,   ///< repeat to the end of the region handed in
    };

    /// One position in the pattern. Steps cycle independently of the pitch pool,
    /// so 3 steps against a 4-note chord give a phrase that takes 12 slots to
    /// repeat — the reason step sequencing is worth having at all.
    struct Step {
        int velocity = 100;
        bool skip = false;      ///< silence: the slot passes, the pitch advances
        bool tie = false;       ///< extend the previous note through this slot
        int transpose = 0;      ///< semitones, relative to the pooled pitch
    };

    Direction direction = Direction::Up;
    int octaves = 1;            ///< 1 … 5
    double rateBeats = 0.25;
    /// Note length as a fraction of the rate. 0.5 at 1/8 gives 1/16s.
    double gate = 0.9;
    std::vector<Step> steps;    ///< empty = one plain step
    /// −1 … 1: fades the phrase from loud to soft, or the other way.
    double velocityRamp = 0.0;
    double swing = 0.5;
    int humanizeVelocity = 0;
    double humanizeTiming = 0.0;
    PlayMode playMode = PlayMode::Loop;
    /// Keep the source chord and lay the arpeggio over it.
    bool merge = false;
    uint32_t seed = 1;
};

/// `regionEndBeats` bounds Fill mode; pass the clip's length.
Notes arpeggiate(const Notes& source, const ArpParams& params,
                 double regionEndBeats);

// ── Glue / legato ───────────────────────────────────────────────────────────

struct GlueParams {
    enum class Mode {
        MergeOverlapping,  ///< run of touching notes → one long note
        Legato,            ///< stretch each note to where the next one starts
    };

    Mode mode = Mode::MergeOverlapping;
    /// Merge only notes at the same pitch. Off, a run merges regardless, which
    /// is occasionally wanted and usually not.
    bool samePitchOnly = true;
    /// Largest silence still counted as "touching".
    double gapToleranceBeats = 0.0;
    /// Ceiling on a stretched note, so the last note of a phrase doesn't run to
    /// the end of the world. 0 = no limit.
    double legatoMaxBeats = 0.0;
};

Notes glue(Notes notes, const GlueParams& params);

/// Quick Legato: stretch every note in `notes` to where the next note starts.
///
/// "Next" is measured against `context` — the whole clip — not against `notes`,
/// so stretching a selection still stops at the unselected note in the way, and
/// a single note has something to reach towards. Pass the clip for both when
/// there is no selection. `maxBeats` caps a stretch; 0 = no limit.
Notes legato(Notes notes, const Notes& context, double maxBeats = 0.0);

// ── Articulate ──────────────────────────────────────────────────────────────

/// Length and velocity shaping — the two things that make a row of identical
/// notes sound played rather than typed in.
///
/// The length side measures against the distance to the next note rather than
/// against the note's own length, which is what makes one setting mean the same
/// thing to a phrase of quarters and to a phrase of sixteenths: `gate` 1.0 runs
/// each note into the next, 0.5 leaves half the gap open, and anything under
/// about 0.4 reads as staccato.
struct ArticulateParams {
    enum class LengthMode {
        Keep,   ///< lengths untouched; velocity shaping only
        Gate,   ///< a fraction of the distance to the next note
        Scale,  ///< a fraction of the note's own length
    };

    LengthMode lengthMode = LengthMode::Gate;
    double gate = 0.8;
    /// How much of the computed length is taken on. 0 leaves the note as it
    /// was, 1 applies the gate outright; in between is what tightens a part
    /// without flattening the phrasing it already has.
    double amount = 1.0;
    double minLengthBeats = 1.0 / 16.0;
    /// Ceiling, so gating against a distant next note cannot stretch one note
    /// across the whole bar. 0 = no limit.
    double maxLengthBeats = 0.0;
    /// Every `accentEvery`th note in time order is played harder. 0 turns the
    /// velocity side off entirely.
    int accentEvery = 0;
    int accentVelocity = 14;   ///< added to the accented notes
    int otherVelocity = -6;    ///< added to the rest, so the accent stands out
};

Notes articulate(Notes notes, const ArticulateParams& params);

// ── Strum ───────────────────────────────────────────────────────────────────

struct StrumParams {
    enum class Direction { Up, Down };
    enum class Shape { Linear, Accelerate, Decelerate };

    Direction direction = Direction::Up;
    /// Time from the first note of the chord to the last.
    double spanBeats = 0.125;
    Shape shape = Shape::Linear;
    /// −1 … 1: how much quieter (or louder) the last note is than the first.
    double velocityTaper = 0.0;
    /// Notes whose starts fall within this of each other count as one chord.
    double chordWindowBeats = 0.02;
    /// Move the ends with the starts, so lengths survive. Off, the ends stay
    /// where they were and the chord finishes together.
    bool adjustEnds = false;
};

Notes strum(Notes notes, const StrumParams& params);

// ── Randomize / humanize ────────────────────────────────────────────────────

struct RandomParams {
    bool velocity = false;
    int velocityAmount = 20;         ///< ± this much
    bool pitch = false;
    int pitchAmount = 2;             ///< ± semitones
    bool timing = false;
    double timingBeats = 0.02;
    bool duration = false;
    double durationPercent = 20.0;   ///< ± % of the note's own length
    /// Normal instead of uniform — most notes barely move, a few move a lot,
    /// which is what a human actually does.
    bool gaussian = false;
    /// Keep randomised pitches inside a scale instead of going chromatic.
    bool scaleAware = false;
    int scaleRoot = 0;
    Scale scale = Scale::Major;
    /// Round randomised starts back onto the grid.
    bool constrainToGrid = false;
    double gridBeats = 0.25;
    /// Don't let jitter push anything past `regionEndBeats`.
    bool preserveTotalDuration = false;
    double regionEndBeats = 0.0;
    uint32_t seed = 1;
};

Notes randomize(Notes notes, const RandomParams& params);
/// The usual "make it breathe" preset: small timing and velocity spread only.
Notes humanize(Notes notes, double timingBeats, int velocityAmount,
               uint32_t seed);

// ── Chords ──────────────────────────────────────────────────────────────────

struct ChordParams {
    enum class Type {
        Major, Minor, Diminished, Augmented,
        Major7, Minor7, Dominant7, Minor7b5, Diminished7,
        Sus2, Sus4, Add9, Major9, Minor9, Power,
    };

    Type type = Type::Major;
    /// Rotate the chord: 1 lifts the root an octave, 2 lifts root and third.
    int inversion = 0;
    /// Double the root an octave up.
    bool addOctave = false;
    /// Drop the root an octave below the rest — the "drop bass" voicing.
    bool bassOctave = false;
};

std::string chordName(ChordParams::Type type);
const std::vector<ChordParams::Type>& allChordTypes();
/// Turn each note into a chord rooted on it. The original note keeps its id;
/// the notes stacked on top come back with empty ids.
Notes buildChords(const Notes& notes, const ChordParams& params);

// ── Single-purpose edits ────────────────────────────────────────────────────

Notes transpose(Notes notes, int semitones);
Notes transposeInScale(Notes notes, int degrees, int root, Scale scale);
Notes snapToScale(Notes notes, int root, Scale scale);
/// Fold pitches into [low, high] by octaves, keeping the pitch class.
Notes limitPitch(Notes notes, int low, int high);
/// Mirror pitches about the midpoint of the selection — a melodic inversion.
Notes invertPitch(Notes notes);
/// Mirror the selection in time about its own span.
Notes reverseTime(Notes notes);

Notes setVelocity(Notes notes, int velocity);
Notes addVelocity(Notes notes, int delta);
Notes scaleVelocity(Notes notes, double factor);
/// Ramp velocity across the selection in time order, first note to last.
Notes rampVelocity(Notes notes, int from, int to);

Notes setLength(Notes notes, double beats);
Notes scaleLength(Notes notes, double factor);
Notes nudge(Notes notes, double beats);
/// Cyclic shift in time: what `nudge` does, except nothing leaves the phrase.
///
/// A note pushed past the end of the cycle reappears at its start, so the note
/// count and the length of the passage survive and only the rhythm turns.
/// Lengths are left alone — a long note that wraps keeps its tail instead of
/// being cut at the seam, which is what makes rotating back a real undo.
///
/// `spanBeats` is the length of the cycle, measured from the earliest note's
/// start; 0 derives it from the notes themselves. Passing it explicitly is
/// usually what you want, because a phrase whose last note is short spans less
/// than the bar it was written in and would otherwise rotate off the grid.
Notes rotate(Notes notes, double beats, double spanBeats = 0.0);
/// Cut every note crossing a grid line into separate notes on each side.
Notes splitAtGrid(Notes notes, double gridBeats);
/// Cut every note that spans `beats` there. Notes not crossing it are untouched.
Notes splitAt(Notes notes, double beats);

Notes setMuted(Notes notes, bool muted);
Notes toggleMuted(Notes notes);
Notes setColor(Notes notes, uint32_t color);

// ── Harmonic analysis ───────────────────────────────────────────────────────
//
// What is actually playing, read off the notes. The assistant needs this before
// it can add anything to existing material: a bass part follows the root of the
// chord above it, a counter-melody has to land on chord tones, and neither is
// possible from a raw note list. The piano roll can show the same reading.

/// One chord's worth of time.
struct HarmonySegment {
    double startBeats = 0.0;
    double lengthBeats = 0.0;
    /// Pitch class, 0 = C. -1 when nothing sounds in this segment.
    int root = -1;
    /// "major", "minor", "diminished", "augmented", "sus2", "sus4", "power",
    /// "major7", "minor7", "dominant7", "minor7b5", "single" or "unknown".
    std::string quality;
    /// The pitch classes present, low to high, deduplicated.
    std::vector<int> pitchClasses;
    /// The lowest note actually sounding, as a MIDI number; -1 when silent.
    int bassPitch = -1;
    /// How well the winning chord explained the segment, 0 … 1. Below about
    /// 0.6 the reading is a guess and a caller should say so.
    double confidence = 0.0;
};

/// Chop the notes into segments and name the chord in each.
///
/// `segmentBeats` is how long one segment is; 0 uses `beatsPerBar`, which is
/// the right default because harmony changes on bar lines far more often than
/// anywhere else. A note counts towards every segment it *sounds* through, not
/// only the one it starts in — otherwise a held pad would vanish after its
/// first bar. Weighted by how long each pitch sounds and by how low it is, so a
/// bass note counts for more than a passing sixteenth, which is what a listener
/// hears too.
///
/// Segments run from the first note to the last note's end. Empty in, empty
/// out.
std::vector<HarmonySegment> analyzeHarmony(const Notes& notes,
                                           double beatsPerBar,
                                           double segmentBeats = 0.0);

/// The key the notes are in.
struct KeyGuess {
    int root = -1;             ///< pitch class, 0 = C; -1 when there is nothing
    Scale scale = Scale::Major;
    double confidence = 0.0;   ///< 0 … 1, how far ahead of the runner-up
};

/// Weighted pitch-class histogram against major and minor profiles — the
/// Krumhansl-Schmuckler method, which is standard, cheap and good enough to
/// keep a generated part in key.
KeyGuess estimateKey(const Notes& notes);

/// "C", "F#" — a pitch class, without an octave.
std::string pitchClassName(int pitchClass);

// ── Helpers shared with the UI ──────────────────────────────────────────────

/// Earliest start and latest end across the notes. Returns {0,0} when empty.
void spanOf(const Notes& notes, double* startBeats, double* endBeats);
/// "C5", "D#3" — the naming the keyboard and note labels use. FL Studio's
/// convention: MIDI 60 (middle C) is C5.
std::string pitchName(int pitch);

} // namespace daw::miditools
