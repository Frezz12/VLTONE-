#include "MidiTools.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <numeric>
#include <random>

namespace daw::miditools {

namespace {

constexpr double kEps = 1e-9;
/// Shortest note any transform will produce. Matches the controller's own
/// minimum, so nothing here can create a note `setClipNotes` would then resize.
constexpr double kMinLength = 1.0 / 32.0;

/// Deterministic generator. Every randomised transform seeds one of these from
/// its params, so a preview and the apply behind it agree exactly.
struct Rng {
    explicit Rng(uint32_t seed) : engine(seed ? seed : 1u) {}

    double uniform(double spread) {
        std::uniform_real_distribution<double> d(-spread, spread);
        return d(engine);
    }
    double normal(double spread) {
        // Three sigma inside the requested spread, then clipped: a normal
        // distribution has no bound, and a stray six-sigma note would read as a
        // mistake rather than as a performance.
        std::normal_distribution<double> d(0.0, spread / 3.0);
        return std::clamp(d(engine), -spread, spread);
    }
    double spread(double amount, bool gaussian) {
        return gaussian ? normal(amount) : uniform(amount);
    }
    size_t index(size_t count) {
        std::uniform_int_distribution<size_t> d(0, count ? count - 1 : 0);
        return d(engine);
    }

    std::mt19937 engine;
};

/// Indices into `notes`, ordered by start then pitch — the reading order a
/// musician would use, and what every "first note to last note" transform needs.
std::vector<size_t> timeOrder(const Notes& notes) {
    std::vector<size_t> order(notes.size());
    std::iota(order.begin(), order.end(), size_t(0));
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        if (std::abs(notes[a].startBeats - notes[b].startBeats) > kEps)
            return notes[a].startBeats < notes[b].startBeats;
        return notes[a].pitch < notes[b].pitch;
    });
    return order;
}

double noteEnd(const NoteModel& n) { return n.startBeats + n.lengthBeats; }

} // namespace

// ── Scales ──────────────────────────────────────────────────────────────────

const std::vector<int>& scaleDegrees(Scale scale) {
    static const std::map<Scale, std::vector<int>> table = {
        {Scale::Chromatic,       {0,1,2,3,4,5,6,7,8,9,10,11}},
        {Scale::Major,           {0,2,4,5,7,9,11}},
        {Scale::NaturalMinor,    {0,2,3,5,7,8,10}},
        {Scale::HarmonicMinor,   {0,2,3,5,7,8,11}},
        {Scale::MelodicMinor,    {0,2,3,5,7,9,11}},
        {Scale::Dorian,          {0,2,3,5,7,9,10}},
        {Scale::Phrygian,        {0,1,3,5,7,8,10}},
        {Scale::Lydian,          {0,2,4,6,7,9,11}},
        {Scale::Mixolydian,      {0,2,4,5,7,9,10}},
        {Scale::Locrian,         {0,1,3,5,6,8,10}},
        {Scale::PentatonicMajor, {0,2,4,7,9}},
        {Scale::PentatonicMinor, {0,3,5,7,10}},
        {Scale::Blues,           {0,3,5,6,7,10}},
        {Scale::WholeTone,       {0,2,4,6,8,10}},
    };
    static const std::vector<int> chromatic = {0,1,2,3,4,5,6,7,8,9,10,11};
    auto it = table.find(scale);
    return it == table.end() ? chromatic : it->second;
}

std::string scaleName(Scale scale) {
    switch (scale) {
        case Scale::Chromatic:       return "Chromatic";
        case Scale::Major:           return "Major";
        case Scale::NaturalMinor:    return "Natural Minor";
        case Scale::HarmonicMinor:   return "Harmonic Minor";
        case Scale::MelodicMinor:    return "Melodic Minor";
        case Scale::Dorian:          return "Dorian";
        case Scale::Phrygian:        return "Phrygian";
        case Scale::Lydian:          return "Lydian";
        case Scale::Mixolydian:      return "Mixolydian";
        case Scale::Locrian:         return "Locrian";
        case Scale::PentatonicMajor: return "Pentatonic Major";
        case Scale::PentatonicMinor: return "Pentatonic Minor";
        case Scale::Blues:           return "Blues";
        case Scale::WholeTone:       return "Whole Tone";
    }
    return "Scale";
}

std::string scaleId(Scale scale) {
    switch (scale) {
        case Scale::Chromatic:       return "chromatic";
        case Scale::Major:           return "major";
        case Scale::NaturalMinor:    return "natural_minor";
        case Scale::HarmonicMinor:   return "harmonic_minor";
        case Scale::MelodicMinor:    return "melodic_minor";
        case Scale::Dorian:          return "dorian";
        case Scale::Phrygian:        return "phrygian";
        case Scale::Lydian:          return "lydian";
        case Scale::Mixolydian:      return "mixolydian";
        case Scale::Locrian:         return "locrian";
        case Scale::PentatonicMajor: return "pentatonic_major";
        case Scale::PentatonicMinor: return "pentatonic_minor";
        case Scale::Blues:           return "blues";
        case Scale::WholeTone:       return "whole_tone";
    }
    return "major";
}

Scale scaleFromId(const std::string& id) {
    for (Scale scale : allScales())
        if (scaleId(scale) == id) return scale;
    // "minor" is what everyone types, and it is not the id of anything.
    if (id == "minor") return Scale::NaturalMinor;
    return Scale::Major;
}

const std::vector<Scale>& allScales() {
    static const std::vector<Scale> scales = {
        Scale::Chromatic, Scale::Major, Scale::NaturalMinor,
        Scale::HarmonicMinor, Scale::MelodicMinor, Scale::Dorian,
        Scale::Phrygian, Scale::Lydian, Scale::Mixolydian, Scale::Locrian,
        Scale::PentatonicMajor, Scale::PentatonicMinor, Scale::Blues,
        Scale::WholeTone,
    };
    return scales;
}

bool inScale(int pitch, int root, Scale scale) {
    const int pc = ((pitch - root) % 12 + 12) % 12;
    const auto& degrees = scaleDegrees(scale);
    return std::find(degrees.begin(), degrees.end(), pc) != degrees.end();
}

int snapPitchToScale(int pitch, int root, Scale scale) {
    if (inScale(pitch, root, scale)) return pitch;
    // Walk outwards, downwards first: a chromatic run then collapses in one
    // direction instead of alternating either side of the scale.
    for (int distance = 1; distance <= 6; ++distance) {
        if (inScale(pitch - distance, root, scale)) return pitch - distance;
        if (inScale(pitch + distance, root, scale)) return pitch + distance;
    }
    return pitch;
}

int transposeInScale(int pitch, int degrees, int root, Scale scale) {
    const auto& table = scaleDegrees(scale);
    if (table.empty() || degrees == 0) return pitch;

    const int snapped = snapPitchToScale(pitch, root, scale);
    const int pc = ((snapped - root) % 12 + 12) % 12;
    int index = 0;
    for (size_t i = 0; i < table.size(); ++i) {
        if (table[i] == pc) { index = int(i); break; }
    }
    const int size = int(table.size());
    // `snapped - pc` lands on the scale's root in this octave, which is what the
    // degree offsets below are measured from.
    const int octaveBase = snapped - pc;
    const long total = long(index) + degrees;
    const long octaves = long(std::floor(double(total) / double(size)));
    const int newIndex = int(total - octaves * size);
    return octaveBase + int(octaves) * 12 + table[newIndex];
}

// ── Grid ────────────────────────────────────────────────────────────────────

double gridBeatsFor(int denominator, GridFlavour flavour) {
    if (denominator <= 0) return 0.0;
    const double beats = 4.0 / double(denominator);
    switch (flavour) {
        case GridFlavour::Triplet: return beats * 2.0 / 3.0;
        case GridFlavour::Dotted:  return beats * 1.5;
        case GridFlavour::Straight: break;
    }
    return beats;
}

// ── Grooves ─────────────────────────────────────────────────────────────────

const std::vector<Groove>& groovePresets() {
    static const std::vector<Groove> presets = [] {
        std::vector<Groove> list;
        list.push_back({"Straight", 4.0, {}, {}});
        // A swung eighth sits a third of the way into the second half of the
        // beat; two slots across one beat is exactly the pair of eighths.
        list.push_back({"Swing 8ths", 1.0, {0.0, 1.0 / 6.0}, {1.0, 0.9}});
        list.push_back({"Swing 16ths", 0.5, {0.0, 1.0 / 12.0}, {1.0, 0.92}});
        list.push_back({"MPC 54%", 1.0, {0.0, 0.05}, {1.0, 0.85}});
        list.push_back({"Latin", 4.0,
                        {0.0, 0.02, -0.01, 0.04, 0.0, 0.03, -0.02, 0.05},
                        {1.0, 0.8, 0.95, 0.75, 1.0, 0.82, 0.9, 0.78}});
        list.push_back({"Laid Back", 1.0, {0.03}, {}});
        list.push_back({"Push", 1.0, {-0.03}, {}});
        return list;
    }();
    return presets;
}

namespace {

/// Sample a groove at `beats`: which slot of the repeating pattern is this, and
/// what does it ask for?
void grooveAt(const Groove& groove, double beats, double* offset,
              double* velocityScale) {
    *offset = 0.0;
    *velocityScale = 1.0;
    if (groove.empty() || groove.lengthBeats <= 0.0) return;

    const size_t slots = std::max(groove.offsets.size(), groove.velocities.size());
    if (slots == 0) return;
    double phase = std::fmod(beats, groove.lengthBeats);
    if (phase < 0.0) phase += groove.lengthBeats;
    const double slotWidth = groove.lengthBeats / double(slots);
    // Round rather than truncate: a note a hair *before* its slot belongs to
    // that slot, not to the one before it.
    size_t slot = size_t(std::llround(phase / slotWidth)) % slots;

    if (slot < groove.offsets.size()) *offset = groove.offsets[slot];
    if (slot < groove.velocities.size()) *velocityScale = groove.velocities[slot];
}

/// Where the grid wants this time, swing and groove included.
double gridTarget(double beats, const QuantizeParams& p) {
    if (p.gridBeats <= 0.0) return beats;
    double target = std::round(beats / p.gridBeats) * p.gridBeats;

    if (p.swingUnitBeats > 0.0 && std::abs(p.swing - 0.5) > kEps) {
        const long slot = std::llround(target / p.swingUnitBeats);
        if (slot % 2 != 0) target += (p.swing - 0.5) * p.swingUnitBeats;
    }
    if (!p.groove.empty()) {
        double offset = 0.0, velocity = 1.0;
        grooveAt(p.groove, target, &offset, &velocity);
        target += offset * p.grooveTiming;
    }
    return target;
}

/// Pull one time towards the grid, honouring strength, the dead zone and the
/// post-quantise jitter.
double quantizeTime(double beats, const QuantizeParams& p, Rng& rng) {
    if (p.gridBeats <= 0.0) return beats;
    const double target = gridTarget(beats, p);
    if (p.toleranceBeats > 0.0 && std::abs(target - beats) <= p.toleranceBeats) {
        return beats;
    }
    double out = beats + (target - beats) * std::clamp(p.strength, 0.0, 1.0);
    if (p.randomizeBeats > 0.0) out += rng.uniform(p.randomizeBeats);
    return std::max(0.0, out);
}

double quantizeLength(double length, const QuantizeParams& p) {
    if (p.gridBeats <= 0.0) return length;
    // A note shorter than half the grid would round to nothing, so the grid is
    // the floor: quantising lengths should never silence anything.
    const double target = std::max(p.gridBeats, std::round(length / p.gridBeats) * p.gridBeats);
    return std::max(kMinLength,
                    length + (target - length) * std::clamp(p.strength, 0.0, 1.0));
}

} // namespace

// ── Quantize ────────────────────────────────────────────────────────────────

Notes quantize(Notes notes, const QuantizeParams& params) {
    Rng rng(params.seed);

    // Keep the pre-quantise starts: `preserveOrder` below has to know which note
    // came first *originally*, not after everything moved.
    std::vector<double> originalStarts;
    originalStarts.reserve(notes.size());
    for (const auto& n : notes) originalStarts.push_back(n.startBeats);

    for (auto& note : notes) {
        const double start = note.startBeats;
        const double end = noteEnd(note);

        switch (params.target) {
            case QuantizeParams::Target::Start:
                note.startBeats = quantizeTime(start, params, rng);
                break;
            case QuantizeParams::Target::End: {
                const double newEnd = quantizeTime(end, params, rng);
                note.lengthBeats = std::max(kMinLength, newEnd - start);
                break;
            }
            case QuantizeParams::Target::Length:
                note.lengthBeats = quantizeLength(note.lengthBeats, params);
                break;
            case QuantizeParams::Target::StartLength:
                note.startBeats = quantizeTime(start, params, rng);
                note.lengthBeats = quantizeLength(note.lengthBeats, params);
                break;
            case QuantizeParams::Target::StartEnd: {
                const double newStart = quantizeTime(start, params, rng);
                const double newEnd = quantizeTime(end, params, rng);
                note.startBeats = newStart;
                note.lengthBeats = std::max(kMinLength, newEnd - newStart);
                break;
            }
        }

        if (!params.groove.empty() && params.grooveVelocity > 0.0) {
            double offset = 0.0, scale = 1.0;
            grooveAt(params.groove, note.startBeats, &offset, &scale);
            const double blended = 1.0 + (scale - 1.0) * params.grooveVelocity;
            note.velocity = int(std::lround(double(note.velocity) * blended));
            note.velocity = std::clamp(note.velocity, 1, 127);
        }
    }

    if (params.preserveOrder) {
        // Per pitch, in original order, no note may start before the one that
        // used to precede it. Without this a swung note can leapfrog its
        // neighbour and a repeated pitch turns into a stutter.
        std::map<int, std::vector<size_t>> byPitch;
        std::vector<size_t> order(notes.size());
        std::iota(order.begin(), order.end(), size_t(0));
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return originalStarts[a] < originalStarts[b];
        });
        for (size_t index : order) byPitch[notes[index].pitch].push_back(index);

        for (auto& [pitch, indices] : byPitch) {
            double floorBeats = -1.0;
            for (size_t index : indices) {
                notes[index].startBeats = std::max(notes[index].startBeats, floorBeats);
                floorBeats = notes[index].startBeats;
            }
        }
    }

    return notes;
}

// ── Arpeggiator ─────────────────────────────────────────────────────────────

Notes arpeggiate(const Notes& source, const ArpParams& params,
                 double regionEndBeats) {
    Notes out;
    if (source.empty() || params.rateBeats <= 0.0) return out;
    if (params.merge) out = source;

    double spanStart = 0.0, spanEnd = 0.0;
    spanOf(source, &spanStart, &spanEnd);

    // ── The pitch pool: the chord, stacked up the requested octaves ──
    std::vector<int> chord;
    for (const auto& n : source) chord.push_back(n.pitch);
    std::sort(chord.begin(), chord.end());
    chord.erase(std::unique(chord.begin(), chord.end()), chord.end());
    if (chord.empty()) return out;

    std::vector<int> pool;
    const int octaves = std::clamp(params.octaves, 1, 5);
    for (int octave = 0; octave < octaves; ++octave) {
        for (int pitch : chord) pool.push_back(pitch + 12 * octave);
    }

    std::vector<int> sequence = pool;
    switch (params.direction) {
        case ArpParams::Direction::Up:
            break;
        case ArpParams::Direction::Down:
            std::reverse(sequence.begin(), sequence.end());
            break;
        case ArpParams::Direction::UpDown:
            // The turning notes are not repeated, so a loop reads as one line
            // rather than as a bounce that stutters at each end.
            for (size_t i = pool.size() - 1; i-- > 1;) sequence.push_back(pool[i]);
            break;
        case ArpParams::Direction::DownUp:
            std::reverse(sequence.begin(), sequence.end());
            for (size_t i = 1; i + 1 < pool.size(); ++i) sequence.push_back(pool[i]);
            break;
        case ArpParams::Direction::Random:
        case ArpParams::Direction::Chord:
            break;
    }
    if (sequence.empty()) sequence = pool;

    std::vector<ArpParams::Step> steps = params.steps;
    if (steps.empty()) steps.push_back(ArpParams::Step{});

    // ── How many slots ──
    double end = spanEnd;
    if (params.playMode == ArpParams::PlayMode::Fill) {
        end = std::max(spanEnd, regionEndBeats);
    }
    size_t slotCount = 0;
    if (params.playMode == ArpParams::PlayMode::Once) {
        slotCount = sequence.size();
    } else {
        const double length = std::max(0.0, end - spanStart);
        slotCount = size_t(std::max(1.0, std::ceil(length / params.rateBeats - kEps)));
    }
    if (slotCount == 0) return out;

    Rng rng(params.seed);
    const double gate = std::clamp(params.gate, 0.05, 4.0);
    // Index of the last note this call emitted, so a tie can lengthen it.
    size_t lastEmitted = out.size();
    bool haveLast = false;

    for (size_t slot = 0; slot < slotCount; ++slot) {
        const ArpParams::Step& step = steps[slot % steps.size()];
        double time = spanStart + double(slot) * params.rateBeats;
        if (std::abs(params.swing - 0.5) > kEps && slot % 2 != 0) {
            time += (params.swing - 0.5) * params.rateBeats;
        }
        if (params.humanizeTiming > 0.0) time += rng.uniform(params.humanizeTiming);
        time = std::max(0.0, time);

        if (step.tie && haveLast) {
            // Fold this slot into the note before it instead of restriking: the
            // held note now runs to the end of *this* slot's gate.
            const double heldEnd =
                spanStart + double(slot) * params.rateBeats + params.rateBeats * gate;
            out[lastEmitted].lengthBeats =
                std::max(kMinLength, heldEnd - out[lastEmitted].startBeats);
            continue;
        }
        if (step.skip) {
            haveLast = false;
            continue;
        }

        const double progress =
            slotCount > 1 ? double(slot) / double(slotCount - 1) : 0.0;
        const double rampScale = 1.0 + params.velocityRamp * (2.0 * progress - 1.0);
        int velocity = int(std::lround(double(step.velocity) * rampScale));
        if (params.humanizeVelocity > 0) {
            velocity += int(std::lround(rng.uniform(double(params.humanizeVelocity))));
        }
        velocity = std::clamp(velocity, 1, 127);

        const double length = std::max(kMinLength, params.rateBeats * gate);

        auto emit = [&](int pitch) {
            NoteModel note;
            note.id.clear();   // created, not kept: setClipNotes mints the uuid
            note.pitch = pitch + step.transpose;
            note.startBeats = time;
            note.lengthBeats = length;
            note.velocity = velocity;
            out.push_back(note);
        };

        if (params.direction == ArpParams::Direction::Chord) {
            for (int pitch : pool) emit(pitch);
            lastEmitted = out.size() - 1;
        } else if (params.direction == ArpParams::Direction::Random) {
            emit(sequence[rng.index(sequence.size())]);
            lastEmitted = out.size() - 1;
        } else {
            emit(sequence[slot % sequence.size()]);
            lastEmitted = out.size() - 1;
        }
        haveLast = true;
    }

    return out;
}

// ── Glue / legato ───────────────────────────────────────────────────────────

Notes legato(Notes notes, const Notes& context, double maxBeats) {
    if (notes.empty()) return notes;

    // Every distinct start in the phrase; a note runs until the next one. The
    // starts come from the context rather than from `notes`, so a selection
    // stretches up to the unselected note in its way instead of over it.
    std::vector<double> starts;
    starts.reserve(context.size());
    for (const auto& n : context) starts.push_back(n.startBeats);
    std::sort(starts.begin(), starts.end());

    for (auto& note : notes) {
        auto it = std::upper_bound(starts.begin(), starts.end(),
                                   note.startBeats + kEps);
        if (it == starts.end()) continue;   // the last note keeps its length
        double length = *it - note.startBeats;
        if (maxBeats > 0.0) length = std::min(length, maxBeats);
        note.lengthBeats = std::max(kMinLength, length);
    }
    return notes;
}

Notes glue(Notes notes, const GlueParams& params) {
    if (params.mode == GlueParams::Mode::Legato) {
        return legato(notes, notes, params.legatoMaxBeats);
    }
    if (notes.size() < 2) return notes;

    // ── Merge overlapping ──
    std::vector<size_t> order(notes.size());
    std::iota(order.begin(), order.end(), size_t(0));
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        if (params.samePitchOnly && notes[a].pitch != notes[b].pitch)
            return notes[a].pitch < notes[b].pitch;
        return notes[a].startBeats < notes[b].startBeats;
    });

    Notes merged;
    for (size_t index : order) {
        const NoteModel& note = notes[index];
        const bool joinable =
            !merged.empty() &&
            (!params.samePitchOnly || merged.back().pitch == note.pitch) &&
            note.startBeats <= noteEnd(merged.back()) + params.gapToleranceBeats + kEps;
        if (joinable) {
            // The head note wins on id, pitch and velocity — the run becomes one
            // sustained note, and its attack is the one you actually hear.
            const double end = std::max(noteEnd(merged.back()), noteEnd(note));
            merged.back().lengthBeats =
                std::max(kMinLength, end - merged.back().startBeats);
            continue;
        }
        merged.push_back(note);
    }
    return merged;
}

// ── Articulate ──────────────────────────────────────────────────────────────

Notes articulate(Notes notes, const ArticulateParams& params) {
    if (notes.empty()) return notes;

    const std::vector<size_t> order = timeOrder(notes);

    // ── Length side ──
    if (params.lengthMode != ArticulateParams::LengthMode::Keep) {
        for (size_t i = 0; i < order.size(); ++i) {
            auto& note = notes[order[i]];
            double target = note.lengthBeats;

            if (params.lengthMode == ArticulateParams::LengthMode::Gate) {
                // Distance to the next note's start, or to the note's own end if
                // it is the last.
                const double next = (i + 1 < order.size())
                                        ? notes[order[i + 1]].startBeats
                                        : noteEnd(note);
                const double gap = std::max(0.0, next - note.startBeats);
                target = gap * params.gate;
            } else {
                target = note.lengthBeats * params.gate;
            }

            if (params.maxLengthBeats > 0.0) {
                target = std::min(target, params.maxLengthBeats);
            }
            target = std::max(params.minLengthBeats, target);

            // Blend toward the target, so amount < 1 keeps the existing phrasing.
            const double blended =
                note.lengthBeats + (target - note.lengthBeats) * params.amount;
            note.lengthBeats = std::max(kMinLength, blended);
        }
    }

    // ── Velocity side ──
    if (params.accentEvery > 0) {
        for (size_t i = 0; i < order.size(); ++i) {
            auto& note = notes[order[i]];
            const bool accent = (i % size_t(params.accentEvery) == 0);
            const int delta = accent ? params.accentVelocity : params.otherVelocity;
            note.velocity = std::clamp(note.velocity + delta, 1, 127);
        }
    }

    return notes;
}

// ── Strum ───────────────────────────────────────────────────────────────────

Notes strum(Notes notes, const StrumParams& params) {
    if (notes.size() < 2 || params.spanBeats <= 0.0) return notes;

    std::vector<size_t> order = timeOrder(notes);

    size_t groupStart = 0;
    while (groupStart < order.size()) {
        // Everything starting within the window of the group's first note is
        // one chord; anything later is a separate event and strums on its own.
        const double anchor = notes[order[groupStart]].startBeats;
        size_t groupEnd = groupStart + 1;
        while (groupEnd < order.size() &&
               notes[order[groupEnd]].startBeats - anchor <=
                   params.chordWindowBeats + kEps) {
            ++groupEnd;
        }

        const size_t count = groupEnd - groupStart;
        if (count > 1) {
            std::vector<size_t> chord(order.begin() + long(groupStart),
                                      order.begin() + long(groupEnd));
            std::stable_sort(chord.begin(), chord.end(), [&](size_t a, size_t b) {
                return params.direction == StrumParams::Direction::Up
                           ? notes[a].pitch < notes[b].pitch
                           : notes[a].pitch > notes[b].pitch;
            });

            for (size_t i = 0; i < chord.size(); ++i) {
                const double fraction = double(i) / double(chord.size() - 1);
                double shaped = fraction;
                switch (params.shape) {
                    case StrumParams::Shape::Accelerate:
                        // Wide gaps first, closing up — the hand speeds through.
                        shaped = 1.0 - (1.0 - fraction) * (1.0 - fraction);
                        break;
                    case StrumParams::Shape::Decelerate:
                        shaped = fraction * fraction;
                        break;
                    case StrumParams::Shape::Linear:
                        break;
                }

                NoteModel& note = notes[chord[i]];
                const double originalEnd = noteEnd(note);
                note.startBeats = anchor + shaped * params.spanBeats;
                // `adjustEnds` keeps each length, so the chord finishes as
                // raggedly as it started. Otherwise the ends stay where they
                // were and the chord lands together, which is the guitar-like
                // result and the reason it is the default.
                if (!params.adjustEnds) {
                    note.lengthBeats = std::max(kMinLength, originalEnd - note.startBeats);
                }

                if (std::abs(params.velocityTaper) > kEps) {
                    const double scale = 1.0 + params.velocityTaper * fraction;
                    note.velocity = std::clamp(
                        int(std::lround(double(note.velocity) * scale)), 1, 127);
                }
            }
        }
        groupStart = groupEnd;
    }
    return notes;
}

// ── Randomize ───────────────────────────────────────────────────────────────

Notes randomize(Notes notes, const RandomParams& params) {
    Rng rng(params.seed);

    for (auto& note : notes) {
        if (params.velocity && params.velocityAmount > 0) {
            const double delta =
                rng.spread(double(params.velocityAmount), params.gaussian);
            note.velocity =
                std::clamp(note.velocity + int(std::lround(delta)), 1, 127);
        }
        if (params.pitch && params.pitchAmount > 0) {
            const double delta =
                rng.spread(double(params.pitchAmount), params.gaussian);
            int pitch = note.pitch + int(std::lround(delta));
            if (params.scaleAware) {
                pitch = snapPitchToScale(pitch, params.scaleRoot, params.scale);
            }
            note.pitch = std::clamp(pitch, 0, 127);
        }
        if (params.timing && params.timingBeats > 0.0) {
            double start =
                note.startBeats + rng.spread(params.timingBeats, params.gaussian);
            if (params.constrainToGrid && params.gridBeats > 0.0) {
                start = std::round(start / params.gridBeats) * params.gridBeats;
            }
            note.startBeats = std::max(0.0, start);
        }
        if (params.duration && params.durationPercent > 0.0) {
            const double factor =
                1.0 + rng.spread(params.durationPercent / 100.0, params.gaussian);
            note.lengthBeats = std::max(kMinLength, note.lengthBeats * factor);
        }
        if (params.preserveTotalDuration && params.regionEndBeats > 0.0) {
            // Jitter must not push anything past the region: a note that ends
            // after the clip does simply stops existing to the listener.
            if (noteEnd(note) > params.regionEndBeats) {
                note.startBeats =
                    std::max(0.0, std::min(note.startBeats,
                                           params.regionEndBeats - kMinLength));
                note.lengthBeats = std::max(kMinLength,
                                            params.regionEndBeats - note.startBeats);
            }
        }
    }
    return notes;
}

Notes humanize(Notes notes, double timingBeats, int velocityAmount,
               uint32_t seed) {
    RandomParams params;
    params.timing = timingBeats > 0.0;
    params.timingBeats = timingBeats;
    params.velocity = velocityAmount > 0;
    params.velocityAmount = velocityAmount;
    params.gaussian = true;   // most notes near the beat, a few well off it
    params.seed = seed;
    return randomize(std::move(notes), params);
}

// ── Chords ──────────────────────────────────────────────────────────────────

namespace {

const std::vector<int>& chordIntervals(ChordParams::Type type) {
    static const std::map<ChordParams::Type, std::vector<int>> table = {
        {ChordParams::Type::Major,       {0, 4, 7}},
        {ChordParams::Type::Minor,       {0, 3, 7}},
        {ChordParams::Type::Diminished,  {0, 3, 6}},
        {ChordParams::Type::Augmented,   {0, 4, 8}},
        {ChordParams::Type::Major7,      {0, 4, 7, 11}},
        {ChordParams::Type::Minor7,      {0, 3, 7, 10}},
        {ChordParams::Type::Dominant7,   {0, 4, 7, 10}},
        {ChordParams::Type::Minor7b5,    {0, 3, 6, 10}},
        {ChordParams::Type::Diminished7, {0, 3, 6, 9}},
        {ChordParams::Type::Sus2,        {0, 2, 7}},
        {ChordParams::Type::Sus4,        {0, 5, 7}},
        {ChordParams::Type::Add9,        {0, 4, 7, 14}},
        {ChordParams::Type::Major9,      {0, 4, 7, 11, 14}},
        {ChordParams::Type::Minor9,      {0, 3, 7, 10, 14}},
        {ChordParams::Type::Power,       {0, 7}},
    };
    static const std::vector<int> fallback = {0, 4, 7};
    auto it = table.find(type);
    return it == table.end() ? fallback : it->second;
}

} // namespace

std::string chordName(ChordParams::Type type) {
    switch (type) {
        case ChordParams::Type::Major:       return "Major";
        case ChordParams::Type::Minor:       return "Minor";
        case ChordParams::Type::Diminished:  return "Diminished";
        case ChordParams::Type::Augmented:   return "Augmented";
        case ChordParams::Type::Major7:      return "Major 7";
        case ChordParams::Type::Minor7:      return "Minor 7";
        case ChordParams::Type::Dominant7:   return "Dominant 7";
        case ChordParams::Type::Minor7b5:    return "Minor 7♭5";
        case ChordParams::Type::Diminished7: return "Diminished 7";
        case ChordParams::Type::Sus2:        return "Sus2";
        case ChordParams::Type::Sus4:        return "Sus4";
        case ChordParams::Type::Add9:        return "Add9";
        case ChordParams::Type::Major9:      return "Major 9";
        case ChordParams::Type::Minor9:      return "Minor 9";
        case ChordParams::Type::Power:       return "Power";
    }
    return "Chord";
}

const std::vector<ChordParams::Type>& allChordTypes() {
    static const std::vector<ChordParams::Type> types = {
        ChordParams::Type::Major, ChordParams::Type::Minor,
        ChordParams::Type::Diminished, ChordParams::Type::Augmented,
        ChordParams::Type::Major7, ChordParams::Type::Minor7,
        ChordParams::Type::Dominant7, ChordParams::Type::Minor7b5,
        ChordParams::Type::Diminished7, ChordParams::Type::Sus2,
        ChordParams::Type::Sus4, ChordParams::Type::Add9,
        ChordParams::Type::Major9, ChordParams::Type::Minor9,
        ChordParams::Type::Power,
    };
    return types;
}

Notes buildChords(const Notes& notes, const ChordParams& params) {
    Notes out;
    std::vector<int> intervals = chordIntervals(params.type);

    // An inversion lifts the lowest voices an octave, one per step.
    const int inversion = std::clamp(params.inversion, 0, int(intervals.size()) - 1);
    for (int i = 0; i < inversion; ++i) intervals[size_t(i)] += 12;
    if (params.addOctave) intervals.push_back(12);
    if (params.bassOctave) intervals.insert(intervals.begin(), -12);

    for (const auto& note : notes) {
        bool first = true;
        for (int interval : intervals) {
            NoteModel voice = note;
            voice.pitch = note.pitch + interval;
            // The root keeps the original note's identity so the selection and
            // the undo entry still line up with what the user clicked.
            if (!first) voice.id.clear();
            first = false;
            out.push_back(voice);
        }
    }
    return out;
}

// ── Single-purpose edits ────────────────────────────────────────────────────

Notes transpose(Notes notes, int semitones) {
    for (auto& note : notes) note.pitch += semitones;
    return notes;
}

Notes transposeInScale(Notes notes, int degrees, int root, Scale scale) {
    for (auto& note : notes) {
        note.pitch = transposeInScale(note.pitch, degrees, root, scale);
    }
    return notes;
}

Notes snapToScale(Notes notes, int root, Scale scale) {
    for (auto& note : notes) note.pitch = snapPitchToScale(note.pitch, root, scale);
    return notes;
}

Notes limitPitch(Notes notes, int low, int high) {
    if (low > high) std::swap(low, high);
    for (auto& note : notes) {
        if (high - low >= 11) {
            // Fold by octaves so the pitch class survives; only a range too
            // narrow to hold an octave has to clamp and change the note.
            while (note.pitch < low) note.pitch += 12;
            while (note.pitch > high) note.pitch -= 12;
        }
        note.pitch = std::clamp(note.pitch, low, high);
    }
    return notes;
}

Notes invertPitch(Notes notes) {
    if (notes.empty()) return notes;
    int low = notes.front().pitch, high = notes.front().pitch;
    for (const auto& note : notes) {
        low = std::min(low, note.pitch);
        high = std::max(high, note.pitch);
    }
    const int sum = low + high;   // mirror about the middle of what is selected
    for (auto& note : notes) note.pitch = sum - note.pitch;
    return notes;
}

Notes reverseTime(Notes notes) {
    if (notes.empty()) return notes;
    double start = 0.0, end = 0.0;
    spanOf(notes, &start, &end);
    for (auto& note : notes) {
        // A note's *end* becomes its distance from the start of the span.
        note.startBeats = start + end - noteEnd(note);
    }
    return notes;
}

Notes setVelocity(Notes notes, int velocity) {
    for (auto& note : notes) note.velocity = std::clamp(velocity, 1, 127);
    return notes;
}

Notes addVelocity(Notes notes, int delta) {
    for (auto& note : notes) {
        note.velocity = std::clamp(note.velocity + delta, 1, 127);
    }
    return notes;
}

Notes scaleVelocity(Notes notes, double factor) {
    for (auto& note : notes) {
        note.velocity = std::clamp(
            int(std::lround(double(note.velocity) * factor)), 1, 127);
    }
    return notes;
}

Notes rampVelocity(Notes notes, int from, int to) {
    if (notes.empty()) return notes;
    const std::vector<size_t> order = timeOrder(notes);
    for (size_t i = 0; i < order.size(); ++i) {
        const double progress =
            order.size() > 1 ? double(i) / double(order.size() - 1) : 0.0;
        notes[order[i]].velocity = std::clamp(
            int(std::lround(double(from) + (double(to) - double(from)) * progress)),
            1, 127);
    }
    return notes;
}

Notes setLength(Notes notes, double beats) {
    for (auto& note : notes) note.lengthBeats = std::max(kMinLength, beats);
    return notes;
}

Notes scaleLength(Notes notes, double factor) {
    for (auto& note : notes) {
        note.lengthBeats = std::max(kMinLength, note.lengthBeats * factor);
    }
    return notes;
}

Notes nudge(Notes notes, double beats) {
    for (auto& note : notes) {
        note.startBeats = std::max(0.0, note.startBeats + beats);
    }
    return notes;
}

Notes rotate(Notes notes, double beats, double spanBeats) {
    if (notes.empty() || std::abs(beats) < kEps) return notes;
    double start = 0.0, end = 0.0;
    spanOf(notes, &start, &end);
    const double cycle = spanBeats > 0.0 ? spanBeats : (end - start);
    if (cycle <= kEps) return notes;   // one chord: there is nothing to rotate

    for (auto& note : notes) {
        double offset = std::fmod(note.startBeats - start + beats, cycle);
        // fmod keeps the sign of its dividend, so a leftward rotation lands
        // negative and has to be brought back into the cycle.
        if (offset < 0.0) offset += cycle;
        note.startBeats = start + offset;
    }
    return notes;
}

Notes splitAt(Notes notes, double beats) {
    Notes out;
    for (const auto& note : notes) {
        if (beats <= note.startBeats + kEps || beats >= noteEnd(note) - kEps) {
            out.push_back(note);
            continue;
        }
        NoteModel left = note;
        left.lengthBeats = beats - note.startBeats;
        NoteModel right = note;
        right.id.clear();          // a new note, and it must not share the id
        right.startBeats = beats;
        right.lengthBeats = noteEnd(note) - beats;
        out.push_back(left);
        out.push_back(right);
    }
    return out;
}

Notes splitAtGrid(Notes notes, double gridBeats) {
    if (gridBeats <= 0.0) return notes;
    Notes out;
    for (const auto& note : notes) {
        const double end = noteEnd(note);
        double cut = std::floor(note.startBeats / gridBeats + 1.0) * gridBeats;
        double pieceStart = note.startBeats;
        bool cutAny = false;
        for (; cut < end - kEps; cut += gridBeats) {
            if (cut <= pieceStart + kEps) continue;
            NoteModel piece = note;
            if (cutAny) piece.id.clear();
            piece.startBeats = pieceStart;
            piece.lengthBeats = cut - pieceStart;
            out.push_back(piece);
            pieceStart = cut;
            cutAny = true;
        }
        NoteModel tail = note;
        if (cutAny) tail.id.clear();
        tail.startBeats = pieceStart;
        tail.lengthBeats = std::max(kMinLength, end - pieceStart);
        out.push_back(tail);
    }
    return out;
}

Notes setMuted(Notes notes, bool muted) {
    for (auto& note : notes) note.muted = muted;
    return notes;
}

Notes toggleMuted(Notes notes) {
    for (auto& note : notes) note.muted = !note.muted;
    return notes;
}

Notes setColor(Notes notes, uint32_t color) {
    for (auto& note : notes) note.color = color;
    return notes;
}

// ── Helpers ─────────────────────────────────────────────────────────────────

void spanOf(const Notes& notes, double* startBeats, double* endBeats) {
    if (notes.empty()) {
        if (startBeats) *startBeats = 0.0;
        if (endBeats) *endBeats = 0.0;
        return;
    }
    double start = notes.front().startBeats;
    double end = noteEnd(notes.front());
    for (const auto& note : notes) {
        start = std::min(start, note.startBeats);
        end = std::max(end, noteEnd(note));
    }
    if (startBeats) *startBeats = start;
    if (endBeats) *endBeats = end;
}

// ── Harmonic analysis ───────────────────────────────────────────────────────

namespace {

/// The chords worth recognising, as intervals above the root. Ordered so that
/// the richer reading of the same notes is tried first: a set that is both a
/// minor triad and a minor seventh should be named the seventh.
struct ChordShape {
    const char* name;
    std::array<int, 5> intervals;
    int count;
};

const std::array<ChordShape, 11> kChordShapes{{
    {"major7", {0, 4, 7, 11, 0}, 4},
    {"minor7", {0, 3, 7, 10, 0}, 4},
    {"dominant7", {0, 4, 7, 10, 0}, 4},
    {"minor7b5", {0, 3, 6, 10, 0}, 4},
    {"major", {0, 4, 7, 0, 0}, 3},
    {"minor", {0, 3, 7, 0, 0}, 3},
    {"diminished", {0, 3, 6, 0, 0}, 3},
    {"augmented", {0, 4, 8, 0, 0}, 3},
    {"sus4", {0, 5, 7, 0, 0}, 3},
    {"sus2", {0, 2, 7, 0, 0}, 3},
    {"power", {0, 7, 0, 0, 0}, 2},
}};

/// How much of a segment's weight each pitch class carries, and how low the
/// lowest sounding note is.
struct SegmentWeights {
    std::array<double, 12> weight{};
    double total = 0.0;
    int bassPitch = -1;
};

SegmentWeights weighSegment(const Notes& notes, double from, double to) {
    SegmentWeights out;
    for (const NoteModel& note : notes) {
        if (note.muted) continue;
        const double start = std::max(note.startBeats, from);
        const double end =
            std::min(note.startBeats + std::max(0.0, note.lengthBeats), to);
        const double sounding = end - start;
        if (sounding <= 1e-9) continue;

        // Long notes and low notes decide what a chord is; a passing sixteenth
        // at the top of a run does not. Velocity counts a little, so a ghost
        // note cannot rename the chord.
        const double lowness = 1.0 + std::max(0, 72 - note.pitch) / 48.0;
        const double weight = sounding * lowness * (0.5 + note.velocity / 254.0);
        out.weight[std::size_t(((note.pitch % 12) + 12) % 12)] += weight;
        out.total += weight;
        if (out.bassPitch < 0 || note.pitch < out.bassPitch)
            out.bassPitch = note.pitch;
    }
    return out;
}

} // namespace

std::vector<HarmonySegment> analyzeHarmony(const Notes& notes,
                                           double beatsPerBar,
                                           double segmentBeats) {
    std::vector<HarmonySegment> out;
    if (notes.empty()) return out;
    if (!(beatsPerBar > 0.0)) beatsPerBar = 4.0;
    double step = segmentBeats > 0.0 ? segmentBeats : beatsPerBar;
    if (!(step > 0.0)) step = 4.0;

    double first = 0.0, last = 0.0;
    spanOf(notes, &first, &last);
    if (!(last > first)) return out;
    // Segment boundaries sit on the grid the notes were written against, not on
    // the first note: a part that begins with a pickup would otherwise put every
    // bar line in the wrong place.
    const double origin = std::floor(first / step) * step;
    const int count = std::min(4096, int(std::ceil((last - origin) / step - 1e-9)));

    for (int i = 0; i < count; ++i) {
        const double from = origin + i * step;
        const SegmentWeights weights = weighSegment(notes, from, from + step);

        HarmonySegment segment;
        segment.startBeats = from;
        segment.lengthBeats = step;
        segment.bassPitch = weights.bassPitch;
        if (weights.total <= 0.0) {
            segment.quality = "silent";
            out.push_back(std::move(segment));
            continue;
        }

        // Anything under a fortieth of the segment's weight is passing colour,
        // not a chord tone; including it turns every triad into an "unknown".
        const double floorWeight = weights.total * 0.025;
        for (int pc = 0; pc < 12; ++pc)
            if (weights.weight[std::size_t(pc)] > floorWeight)
                segment.pitchClasses.push_back(pc);

        double bestScore = -1.0;
        for (int root = 0; root < 12; ++root) {
            for (const ChordShape& shape : kChordShapes) {
                double inside = 0.0;
                std::array<bool, 12> member{};
                for (int k = 0; k < shape.count; ++k)
                    member[std::size_t((root + shape.intervals[std::size_t(k)]) % 12)] = true;
                for (int pc = 0; pc < 12; ++pc)
                    if (member[std::size_t(pc)]) inside += weights.weight[std::size_t(pc)];

                // Weight explained by the chord, less a penalty for chord tones
                // that are not actually there — otherwise a single note matches
                // every chord containing it.
                double present = 0.0;
                for (int k = 0; k < shape.count; ++k) {
                    const std::size_t pc =
                        std::size_t((root + shape.intervals[std::size_t(k)]) % 12);
                    if (weights.weight[pc] > floorWeight) present += 1.0;
                }
                const double coverage = inside / weights.total;
                const double completeness = present / double(shape.count);
                double score = coverage * (0.45 + 0.55 * completeness);
                // The lowest note is the root far more often than not, and it is
                // what a listener hears as the root even when it is not.
                if (weights.bassPitch >= 0 &&
                    ((weights.bassPitch % 12) + 12) % 12 == root)
                    score += 0.08;
                if (score > bestScore) {
                    bestScore = score;
                    segment.root = root;
                    segment.quality = shape.name;
                    segment.confidence = std::clamp(score, 0.0, 1.0);
                }
            }
        }
        if (segment.pitchClasses.size() == 1) {
            segment.root = segment.pitchClasses.front();
            segment.quality = "single";
            segment.confidence = 1.0;
        } else if (segment.confidence < 0.45) {
            segment.quality = "unknown";
        }
        out.push_back(std::move(segment));
    }
    return out;
}

KeyGuess estimateKey(const Notes& notes) {
    KeyGuess guess;
    std::array<double, 12> weight{};
    double total = 0.0;
    for (const NoteModel& note : notes) {
        if (note.muted) continue;
        const double w = std::max(0.05, note.lengthBeats) *
                         (0.5 + note.velocity / 254.0);
        weight[std::size_t(((note.pitch % 12) + 12) % 12)] += w;
        total += w;
    }
    if (total <= 0.0) return guess;

    // Krumhansl-Schmuckler probe-tone profiles.
    static const std::array<double, 12> kMajor{
        6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88};
    static const std::array<double, 12> kMinor{
        6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17};

    const auto correlate = [&weight, total](const std::array<double, 12>& profile,
                                            int root) {
        double meanX = 0.0, meanY = 0.0;
        for (int i = 0; i < 12; ++i) {
            meanX += weight[std::size_t((root + i) % 12)] / total;
            meanY += profile[std::size_t(i)];
        }
        meanX /= 12.0;
        meanY /= 12.0;
        double num = 0.0, denX = 0.0, denY = 0.0;
        for (int i = 0; i < 12; ++i) {
            const double x = weight[std::size_t((root + i) % 12)] / total - meanX;
            const double y = profile[std::size_t(i)] - meanY;
            num += x * y;
            denX += x * x;
            denY += y * y;
        }
        const double den = std::sqrt(denX * denY);
        return den > 1e-12 ? num / den : 0.0;
    };

    double best = -2.0, second = -2.0;
    for (int root = 0; root < 12; ++root) {
        for (int minor = 0; minor < 2; ++minor) {
            const double score = correlate(minor ? kMinor : kMajor, root);
            if (score > best) {
                second = best;
                best = score;
                guess.root = root;
                guess.scale = minor ? Scale::NaturalMinor : Scale::Major;
            } else if (score > second) {
                second = score;
            }
        }
    }
    guess.confidence = std::clamp(best - std::max(0.0, second), 0.0, 1.0);
    return guess;
}

std::string pitchClassName(int pitchClass) {
    static const char* names[12] = {"C", "C#", "D", "D#", "E", "F",
                                    "F#", "G", "G#", "A", "A#", "B"};
    return names[std::size_t(((pitchClass % 12) + 12) % 12)];
}

std::string pitchName(int pitch) {
    static const char* names[12] = {"C", "C#", "D", "D#", "E", "F",
                                    "F#", "G", "G#", "A", "A#", "B"};
    const int pc = ((pitch % 12) + 12) % 12;
    // FL Studio's octave numbering: middle C (MIDI 60) is C5, so the lowest
    // MIDI note is C0. Every label in the app goes through here, which is what
    // keeps the piano roll, the sampler's root note and the note tools agreeing.
    return std::string(names[pc]) + std::to_string(pitch / 12);
}

} // namespace daw::miditools
