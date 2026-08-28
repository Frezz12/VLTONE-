#include "ai/CompositionEngine.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <set>
#include <sstream>
#include <tuple>
#include <utility>

namespace daw::ai {
namespace {

constexpr double kEpsilon = 1.0e-9;

double clamp01(double value) {
    return std::max(0.0, std::min(1.0, value));
}

int pitchClass(int pitch) {
    return ((pitch % 12) + 12) % 12;
}

std::string normaliseScale(std::string name) {
    for (char& c : name) {
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        if (c == '-' || c == ' ') c = '_';
    }
    if (name == "minor" || name == "aeolian") return "natural_minor";
    if (name == "ionian") return "major";
    if (name == "major_pentatonic") return "pentatonic_major";
    if (name == "minor_pentatonic") return "pentatonic_minor";
    return name;
}

std::vector<int> scaleIntervals(const std::string& requested) {
    const std::string name = normaliseScale(requested);
    if (name == "major") return {0, 2, 4, 5, 7, 9, 11};
    if (name == "natural_minor") return {0, 2, 3, 5, 7, 8, 10};
    if (name == "harmonic_minor") return {0, 2, 3, 5, 7, 8, 11};
    if (name == "dorian") return {0, 2, 3, 5, 7, 9, 10};
    if (name == "mixolydian") return {0, 2, 4, 5, 7, 9, 10};
    if (name == "pentatonic_major") return {0, 2, 4, 7, 9};
    if (name == "pentatonic_minor") return {0, 3, 5, 7, 10};
    if (name == "chromatic")
        return {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    return {};
}

int effectiveRoot(const CompositionRequest& request) {
    return request.keyRoot.value_or(0);
}

std::vector<int> effectiveScale(const CompositionRequest& request) {
    return scaleIntervals(request.scale.value_or("major"));
}

CompositionPitchRange defaultRange(CompositionRole role) {
    switch (role) {
    case CompositionRole::Melody: return {60, 84};
    case CompositionRole::Bass: return {28, 52};
    case CompositionRole::Chords: return {48, 79};
    case CompositionRole::Drums: return {35, 51};
    }
    return {0, 127};
}

CompositionPitchRange effectiveRange(const CompositionRequest& request) {
    return request.pitchRange.value_or(defaultRange(request.role));
}

const CompositionHarmonySegment* harmonyAt(const CompositionRequest& request,
                                            double beat) {
    for (const CompositionHarmonySegment& segment : request.harmony)
        if (beat + kEpsilon >= segment.startBeats &&
            beat < segment.startBeats + segment.lengthBeats - kEpsilon)
            return &segment;
    return nullptr;
}

bool belongsToScale(int pitch, int root, const std::vector<int>& intervals) {
    const int relative = pitchClass(pitch - root);
    return std::find(intervals.begin(), intervals.end(), relative) !=
           intervals.end();
}

std::vector<int> scalePitchPool(const CompositionRequest& request) {
    const CompositionPitchRange range = effectiveRange(request);
    const std::vector<int> intervals = effectiveScale(request);
    std::vector<int> result;
    for (int pitch = range.lowest; pitch <= range.highest; ++pitch)
        if (belongsToScale(pitch, effectiveRoot(request), intervals))
            result.push_back(pitch);
    // A one-semitone custom range may contain no scale tone. It is still a
    // usable register, so generation stays non-empty and harmony scoring makes
    // the compromise visible.
    if (result.empty()) result.push_back((range.lowest + range.highest) / 2);
    return result;
}

int nearestIndex(const std::vector<int>& pitches, int target) {
    int best = 0;
    for (int i = 1; i < int(pitches.size()); ++i)
        if (std::abs(pitches[i] - target) < std::abs(pitches[best] - target))
            best = i;
    return best;
}

int nearestPitchForClass(int wantedPitchClass, int target,
                         CompositionPitchRange range) {
    int best = -1;
    for (int pitch = range.lowest; pitch <= range.highest; ++pitch) {
        if (pitchClass(pitch) != pitchClass(wantedPitchClass)) continue;
        if (best < 0 || std::abs(pitch - target) < std::abs(best - target))
            best = pitch;
    }
    return best;
}

int nearestPitchFromClasses(const std::vector<int>& pitchClasses, int target,
                            CompositionPitchRange range) {
    int best = -1;
    for (int pitchClassValue : pitchClasses) {
        const int candidate =
            nearestPitchForClass(pitchClassValue, target, range);
        if (candidate >= 0 &&
            (best < 0 || std::abs(candidate - target) < std::abs(best - target)))
            best = candidate;
    }
    return best;
}

/// SplitMix64 gives byte-for-byte repeatability without relying on a standard
/// library distribution whose mapping is implementation-defined.
class Random {
public:
    explicit Random(std::uint64_t seed) : m_state(seed) {}

    std::uint64_t next() {
        std::uint64_t z = (m_state += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }

    double unit() { return double(next() >> 11) * (1.0 / 9007199254740992.0); }

    int integer(int lowest, int highest) {
        if (highest <= lowest) return lowest;
        return lowest + int(next() % std::uint64_t(highest - lowest + 1));
    }

    bool chance(double probability) { return unit() < clamp01(probability); }

private:
    std::uint64_t m_state;
};

std::vector<int> spreadSlots(int slots, int count, int variation,
                             double creativity, Random& random) {
    count = std::max(1, std::min(count, slots));
    std::vector<int> positions;
    positions.reserve(std::size_t(count));
    for (int i = 0; i < count; ++i) {
        int position = (i * slots) / count;
        if (i > 0 && random.chance(creativity * 0.55))
            position += random.integer(-1, 1);
        if (i > 0 && (variation % 2) && (i % 3 == 1)) ++position;
        positions.push_back(std::max(0, std::min(slots - 1, position)));
    }
    positions.front() = 0;
    std::sort(positions.begin(), positions.end());
    positions.erase(std::unique(positions.begin(), positions.end()),
                    positions.end());
    for (int slot = 0; int(positions.size()) < count && slot < slots; ++slot) {
        if (!std::binary_search(positions.begin(), positions.end(), slot)) {
            positions.push_back(slot);
            std::sort(positions.begin(), positions.end());
        }
    }
    return positions;
}

void preferFreeSlots(std::vector<int>& positions, int slots,
                     const std::vector<double>& profile, int variation) {
    if (profile.size() != 16 || slots <= 1) return;
    std::vector<int> chosen;
    chosen.reserve(positions.size());
    for (int original : positions) {
        int best = original;
        double bestCost = 2.0;
        for (int delta : {0, variation % 2 ? 1 : -1,
                          variation % 2 ? -1 : 1, 2, -2}) {
            const int candidate = original + delta;
            if (candidate < 0 || candidate >= slots ||
                std::find(chosen.begin(), chosen.end(), candidate) !=
                    chosen.end())
                continue;
            const int phase = std::clamp(
                int(std::floor(double(candidate) / slots * 16.0)), 0, 15);
            const double cost = profile[std::size_t(phase)] +
                                std::abs(delta) * 0.04;
            if (cost < bestCost) {
                bestCost = cost;
                best = candidate;
            }
        }
        chosen.push_back(best);
    }
    std::sort(chosen.begin(), chosen.end());
    positions = std::move(chosen);
}

void addNote(std::vector<CompositionNoteEvent>& notes, int pitch, double start,
             double length, int velocity, double totalBeats) {
    if (start < -kEpsilon || start >= totalBeats - kEpsilon) return;
    const double fitted = std::min(length, totalBeats - start);
    if (fitted <= kEpsilon) return;
    notes.push_back({pitch, std::max(0.0, start), fitted,
                     std::max(1, std::min(127, velocity))});
}

int cadencePitchClass(const CompositionRequest& request) {
    const double totalBeats = request.bars * request.beatsPerBar;
    if (const CompositionHarmonySegment* harmony =
            harmonyAt(request, std::max(0.0, totalBeats - 1.0e-6));
        harmony && harmony->root >= 0)
        return harmony->root;
    return effectiveRoot(request);
}

int developedMelodyOffset(const std::vector<int>& motif, std::size_t note,
                          int bar, bool finalBar) {
    const int original = motif[note];
    if (finalBar || bar % 4 == 0 || bar % 4 == 3) return original;
    if (bar % 4 == 2) return original + 1; // sequence the statement upward

    // Keep the opening recognisable, then answer it in contrary motion.
    const std::size_t pivot = std::max<std::size_t>(1, motif.size() / 2);
    if (note < pivot) return original;
    const int anchor = motif[pivot - 1];
    return anchor - (original - anchor);
}

int developedBassDegree(std::size_t note, std::size_t noteCount, int bar,
                        int variation, bool finalBar) {
    if (note == 0) return 0;
    int degree = ((int(note) + variation) % 3 == 1) ? 4 : 2;
    if (finalBar || bar % 4 == 0 || bar % 4 == 3) return degree;
    if (bar % 4 == 2) return degree == 4 ? 2 : 4;
    if (note >= std::max<std::size_t>(1, noteCount / 2))
        degree = (degree + 1) % 7; // an answering approach in the second half
    return degree;
}

std::vector<CompositionNoteEvent> generateMelody(
    const CompositionRequest& request, int variation, Random& random) {
    const double density = clamp01(request.rhythmicDensity);
    const double creativity = clamp01(request.creativity);
    const double totalBeats = request.bars * request.beatsPerBar;
    const int slots = std::max(1, int(std::lround(request.beatsPerBar * 2.0)));
    int count = int(std::lround(1.0 + density * 6.0));
    if (creativity > 0.2) count += variation % 3 - 1;
    std::vector<int> positions =
        spreadSlots(slots, count, variation, creativity, random);
    preferFreeSlots(positions, slots, request.avoidOnsetProfile16, variation);
    const std::vector<int> pool = scalePitchPool(request);
    const CompositionPitchRange range = effectiveRange(request);
    const int centre = nearestIndex(pool, (range.lowest + range.highest) / 2);

    std::vector<int> motif(positions.size(), 0);
    for (std::size_t i = 1; i < motif.size(); ++i) {
        int step = random.integer(-1, 1);
        if (random.chance(creativity * 0.32)) step = random.integer(-3, 3);
        motif[i] = motif[i - 1] + step;
    }

    std::vector<CompositionNoteEvent> notes;
    for (int bar = 0; bar < request.bars; ++bar) {
        const double barStart = bar * request.beatsPerBar;
        for (std::size_t i = 0; i < positions.size(); ++i) {
            const bool finalNote = bar == request.bars - 1 &&
                                   i + 1 == positions.size();
            const int offset = developedMelodyOffset(
                motif, i, bar, bar == request.bars - 1);
            const int index = std::max(
                0, std::min(int(pool.size()) - 1,
                            centre + (variation % 3 - 1) + offset));
            const double start =
                barStart + positions[i] * request.beatsPerBar / slots;
            int pitch = pool[std::size_t(index)];
            const double nearestBeat = std::round(start);
            if (std::abs(start - nearestBeat) < kEpsilon) {
                if (const CompositionHarmonySegment* harmony =
                        harmonyAt(request, start)) {
                    std::vector<int> chordTones =
                        harmony->chordTonePitchClasses;
                    if (chordTones.empty() && harmony->root >= 0)
                        chordTones.push_back(harmony->root);
                    const int fitted = nearestPitchFromClasses(
                        chordTones, pitch, range);
                    if (fitted >= 0) pitch = fitted;
                }
            }
            if (finalNote) {
                const int fitted = nearestPitchForClass(
                    cadencePitchClass(request), pitch, range);
                if (fitted >= 0) pitch = fitted;
            }
            const int nextSlot = i + 1 < positions.size() ? positions[i + 1] : slots;
            const double gap =
                (nextSlot - positions[i]) * request.beatsPerBar / slots;
            const double articulation = density > 0.75 ? 0.62 : 0.86;
            const double length = finalNote
                                      ? totalBeats - start
                                      : std::max(0.0625, gap * articulation);
            addNote(notes, pitch, start,
                    length, 88 + random.integer(-8, 15), totalBeats);
        }
    }
    return notes;
}

std::vector<CompositionNoteEvent> generateBass(
    const CompositionRequest& request, int variation, Random& random) {
    const double density = clamp01(request.rhythmicDensity);
    const double creativity = clamp01(request.creativity);
    const double totalBeats = request.bars * request.beatsPerBar;
    const int slots = std::max(1, int(std::lround(request.beatsPerBar)));
    int count = int(std::lround(1.0 + density * 3.0));
    if ((variation % 2) && creativity > 0.35) ++count;
    const std::vector<int> positions =
        spreadSlots(slots, count, variation, creativity * 0.5, random);
    const std::vector<int> intervals = effectiveScale(request);
    const CompositionPitchRange range = effectiveRange(request);
    const int centre = range.lowest + (range.highest - range.lowest) / 3;
    static constexpr std::array<int, 4> progression = {0, 3, 4, 0};

    std::vector<CompositionNoteEvent> notes;
    for (int bar = 0; bar < request.bars; ++bar) {
        int degree = progression[std::size_t((bar + variation / 2) % 4)];
        for (std::size_t i = 0; i < positions.size(); ++i) {
            const bool finalNote = bar == request.bars - 1 &&
                                   i + 1 == positions.size();
            int toneDegree = degree + developedBassDegree(
                                          i, positions.size(), bar, variation,
                                          bar == request.bars - 1);
            const int interval = intervals[std::size_t(
                ((toneDegree % int(intervals.size())) + int(intervals.size())) %
                int(intervals.size()))];
            int pitch = nearestPitchForClass(effectiveRoot(request) + interval,
                                             centre, range);
            if (pitch < 0) pitch = scalePitchPool(request).front();
            const double start = bar * request.beatsPerBar +
                                 positions[i] * request.beatsPerBar / slots;
            if (const CompositionHarmonySegment* harmony =
                    harmonyAt(request, start)) {
                std::vector<int> allowed = harmony->chordTonePitchClasses;
                if (i == 0 && harmony->root >= 0)
                    allowed = {harmony->root};
                else if (allowed.empty() && harmony->root >= 0)
                    allowed.push_back(harmony->root);
                const int fitted =
                    nearestPitchFromClasses(allowed, pitch, range);
                if (fitted >= 0) pitch = fitted;
            }
            if (finalNote) {
                const int fitted = nearestPitchForClass(
                    cadencePitchClass(request), pitch, range);
                if (fitted >= 0) pitch = fitted;
            }
            const int nextSlot = i + 1 < positions.size() ? positions[i + 1] : slots;
            const double gap =
                (nextSlot - positions[i]) * request.beatsPerBar / slots;
            const double length = finalNote
                                      ? totalBeats - start
                                      : std::max(0.0625, gap * 0.92);
            addNote(notes, pitch, start, length,
                    96 + random.integer(-7, 12), totalBeats);
        }
    }
    return notes;
}

std::vector<CompositionNoteEvent> generateChords(
    const CompositionRequest& request, int variation, Random& random) {
    const double density = clamp01(request.rhythmicDensity);
    const double creativity = clamp01(request.creativity);
    const double totalBeats = request.bars * request.beatsPerBar;
    const int hits = std::max(1, std::min(3, int(std::lround(1.0 + density * 2.0))));
    const std::vector<int> intervals = effectiveScale(request);
    const CompositionPitchRange range = effectiveRange(request);
    static constexpr std::array<int, 4> progression = {0, 3, 4, 0};
    std::vector<int> previous;
    std::vector<CompositionNoteEvent> notes;

    for (int bar = 0; bar < request.bars; ++bar) {
        for (int hit = 0; hit < hits; ++hit) {
            int degree = progression[std::size_t((bar + (variation >= 3)) % 4)];
            if (hit > 0 && random.chance(creativity * 0.35)) ++degree;
            const int toneCount = density + creativity > 1.15 ? 4 : 3;
            std::vector<int> chord;
            const double start = bar * request.beatsPerBar +
                                 hit * request.beatsPerBar / hits;
            const CompositionHarmonySegment* harmony = harmonyAt(request, start);
            for (int tone = 0; tone < toneCount; ++tone) {
                int target = (range.lowest + range.highest) / 2 + (tone - 1) * 4;
                if (tone < int(previous.size())) target = previous[std::size_t(tone)];
                if ((variation % 3) == 1 && tone == 0) target += 12;
                if ((variation % 3) == 2 && tone == toneCount - 1) target -= 12;

                int pitch = -1;
                if (harmony && !harmony->chordTonePitchClasses.empty()) {
                    const int pitchClassValue =
                        harmony->chordTonePitchClasses[std::size_t(
                            tone % int(harmony->chordTonePitchClasses.size()))];
                    pitch = nearestPitchForClass(pitchClassValue, target, range);
                } else {
                    const int toneDegree = degree + tone * 2;
                    const int interval = intervals[std::size_t(
                        toneDegree % int(intervals.size()))];
                    pitch = nearestPitchForClass(
                        effectiveRoot(request) + interval, target, range);
                }
                if (pitch >= 0 &&
                    std::find(chord.begin(), chord.end(), pitch) == chord.end())
                    chord.push_back(pitch);
            }
            if (chord.empty()) chord.push_back(scalePitchPool(request).front());
            std::sort(chord.begin(), chord.end());
            previous = chord;
            const double length = request.beatsPerBar / hits * 0.94;
            for (int pitch : chord)
                addNote(notes, pitch, start, length,
                        76 + random.integer(-5, 13), totalBeats);
        }
    }
    return notes;
}

bool hasDrumAt(const std::vector<CompositionNoteEvent>& notes, int pitch,
               double start) {
    return std::any_of(notes.begin(), notes.end(), [&](const auto& note) {
        return note.pitch == pitch && std::abs(note.startBeats - start) < kEpsilon;
    });
}

std::vector<CompositionNoteEvent> generateDrums(
    const CompositionRequest& request, int variation, Random& random) {
    const double density = clamp01(request.rhythmicDensity);
    const double creativity = clamp01(request.creativity);
    const double totalBeats = request.bars * request.beatsPerBar;
    const double hatStep = density > 0.72 ? 0.25 : density > 0.24 ? 0.5 : 1.0;
    std::vector<CompositionNoteEvent> notes;
    for (int bar = 0; bar < request.bars; ++bar) {
        const double base = bar * request.beatsPerBar;
        for (double beat = 0.0; beat < request.beatsPerBar - kEpsilon;
             beat += hatStep) {
            if (variation == 2 && std::fmod(beat / hatStep, 4.0) == 3.0 &&
                random.chance(0.55))
                continue;
            const bool open = variation % 3 == 1 &&
                              beat + hatStep >= request.beatsPerBar;
            addNote(notes, open ? 46 : 42, base + beat,
                    std::min(0.2, hatStep * 0.7),
                    70 + random.integer(-7, 20), totalBeats);
        }

        std::vector<double> kicks = {0.0};
        if (request.beatsPerBar >= 3.0) kicks.push_back(2.0);
        if ((variation % 2) && density > 0.35)
            kicks.push_back(std::min(request.beatsPerBar - 0.25, 1.5));
        for (double beat : kicks)
            addNote(notes, 36, base + beat, 0.2,
                    105 + random.integer(-5, 15), totalBeats);

        for (double beat = 1.0; beat < request.beatsPerBar; beat += 2.0)
            addNote(notes, 38, base + beat, 0.2,
                    101 + random.integer(-5, 14), totalBeats);

        if (variation >= 3 && creativity > 0.25) {
            const double fillStart = base + std::max(0.0, request.beatsPerBar - 0.75);
            for (int i = 0; i < 3; ++i) {
                const double at = fillStart + i * 0.25;
                const int pitch = 45 + i * 2;
                if (!hasDrumAt(notes, pitch, at))
                    addNote(notes, pitch, at, 0.18, 86 + i * 7, totalBeats);
            }
        }
    }
    return notes;
}

void sortNotes(std::vector<CompositionNoteEvent>& notes) {
    std::sort(notes.begin(), notes.end(), [](const auto& a, const auto& b) {
        if (a.startBeats != b.startBeats) return a.startBeats < b.startBeats;
        if (a.pitch != b.pitch) return a.pitch < b.pitch;
        return a.lengthBeats < b.lengthBeats;
    });
}

std::string decimal(double value, int precision = 2) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(precision) << value;
    return out.str();
}

void hashValue(std::uint64_t& hash, std::uint64_t value) {
    for (int byte = 0; byte < 8; ++byte) {
        hash ^= (value >> (byte * 8)) & 0xffULL;
        hash *= 1099511628211ULL;
    }
}

std::string candidateId(const CompositionRequest& request, int variation,
                        const std::vector<CompositionNoteEvent>& notes) {
    std::uint64_t hash = 1469598103934665603ULL;
    hashValue(hash, std::uint64_t(request.role));
    hashValue(hash, request.seed);
    hashValue(hash, std::uint64_t(variation));
    for (const auto& note : notes) {
        hashValue(hash, std::uint64_t(note.pitch));
        hashValue(hash, std::uint64_t(std::llround(note.startBeats * 960.0)));
        hashValue(hash, std::uint64_t(std::llround(note.lengthBeats * 960.0)));
        hashValue(hash, std::uint64_t(note.velocity));
    }
    std::ostringstream out;
    out << compositionRoleName(request.role) << '-' << std::hex
        << std::setfill('0') << std::setw(16) << hash;
    return out.str();
}

} // namespace

const char* compositionRoleName(CompositionRole role) noexcept {
    switch (role) {
    case CompositionRole::Melody: return "melody";
    case CompositionRole::Bass: return "bass";
    case CompositionRole::Chords: return "chords";
    case CompositionRole::Drums: return "drums";
    }
    return "unknown";
}

CompositionValidation validateCompositionRequest(
    const CompositionRequest& request) {
    CompositionValidation result;
    if (request.bars < 1 || request.bars > 64)
        result.errors.push_back("bars must be between 1 and 64");
    if (!std::isfinite(request.beatsPerBar) || request.beatsPerBar < 0.25 ||
        request.beatsPerBar > 16.0)
        result.errors.push_back("beatsPerBar must be between 0.25 and 16");
    if (!std::isfinite(request.creativity) || request.creativity < 0.0 ||
        request.creativity > 1.0)
        result.errors.push_back("creativity must be between 0 and 1");
    if (!std::isfinite(request.rhythmicDensity) ||
        request.rhythmicDensity < 0.0 || request.rhythmicDensity > 1.0)
        result.errors.push_back("rhythmicDensity must be between 0 and 1");
    if (request.keyRoot && (*request.keyRoot < 0 || *request.keyRoot > 11))
        result.errors.push_back("keyRoot must be a pitch class from 0 to 11");
    if (request.scale && scaleIntervals(*request.scale).empty())
        result.errors.push_back("scale is not supported");
    if (request.pitchRange &&
        (request.pitchRange->lowest < 0 || request.pitchRange->highest > 127 ||
         request.pitchRange->lowest > request.pitchRange->highest))
        result.errors.push_back("pitchRange must be ordered inside MIDI 0..127");
    const double totalBeats = request.bars * request.beatsPerBar;
    for (const CompositionHarmonySegment& segment : request.harmony) {
        if (!std::isfinite(segment.startBeats) || segment.startBeats < 0.0 ||
            segment.startBeats >= totalBeats)
            result.errors.push_back("harmony start is outside the requested bars");
        if (!std::isfinite(segment.lengthBeats) || segment.lengthBeats <= 0.0)
            result.errors.push_back("harmony length must be positive");
        if (segment.root < -1 || segment.root > 11)
            result.errors.push_back("harmony root must be a pitch class");
        for (int pitchClassValue : segment.chordTonePitchClasses)
            if (pitchClassValue < 0 || pitchClassValue > 11)
                result.errors.push_back("chord tones must be pitch classes");
    }
    if (!request.avoidOnsetProfile16.empty() &&
        request.avoidOnsetProfile16.size() != 16)
        result.errors.push_back("avoidOnsetProfile16 must contain 16 values");
    for (double occupancy : request.avoidOnsetProfile16)
        if (!std::isfinite(occupancy) || occupancy < 0.0 || occupancy > 1.0)
            result.errors.push_back("onset occupancy must be between 0 and 1");
    switch (request.role) {
    case CompositionRole::Melody:
    case CompositionRole::Bass:
    case CompositionRole::Chords:
    case CompositionRole::Drums: break;
    default: result.errors.push_back("role is not supported"); break;
    }
    return result;
}

CompositionValidation validateCompositionCandidate(
    const CompositionRequest& request,
    const CompositionCandidate& candidate) {
    CompositionValidation result = validateCompositionRequest(request);
    if (candidate.notes.empty()) {
        result.errors.push_back("candidate contains no notes");
        return result;
    }
    const double totalBeats = request.bars * request.beatsPerBar;
    for (const auto& note : candidate.notes) {
        if (note.pitch < 0 || note.pitch > 127)
            result.errors.push_back("note pitch is outside MIDI 0..127");
        if (!std::isfinite(note.startBeats) || note.startBeats < 0.0 ||
            note.startBeats >= totalBeats)
            result.errors.push_back("note start is outside the requested bars");
        if (!std::isfinite(note.lengthBeats) || note.lengthBeats <= 0.0)
            result.errors.push_back("note length must be positive");
        else if (std::isfinite(note.startBeats) &&
                 note.startBeats + note.lengthBeats > totalBeats + kEpsilon)
            result.errors.push_back("note end is outside the requested bars");
        if (note.velocity < 1 || note.velocity > 127)
            result.errors.push_back("note velocity is outside MIDI 1..127");
    }

    if (request.role == CompositionRole::Melody ||
        request.role == CompositionRole::Bass) {
        std::vector<CompositionNoteEvent> sorted = candidate.notes;
        sortNotes(sorted);
        double previousEnd = -1.0;
        for (const auto& note : sorted) {
            if (note.startBeats < previousEnd - kEpsilon) {
                result.errors.push_back("monophonic candidate contains overlaps");
                break;
            }
            previousEnd = std::max(previousEnd,
                                   note.startBeats + note.lengthBeats);
        }
    }
    std::sort(result.errors.begin(), result.errors.end());
    result.errors.erase(std::unique(result.errors.begin(), result.errors.end()),
                        result.errors.end());
    return result;
}

CompositionCandidateScore scoreCompositionCandidate(
    const CompositionRequest& request,
    const std::vector<CompositionNoteEvent>& notes) {
    CompositionCandidateScore score;
    if (notes.empty()) return score;

    const double density = clamp01(request.rhythmicDensity);
    const double creativity = clamp01(request.creativity);
    const std::vector<int> intervals = effectiveScale(request);
    const int root = effectiveRoot(request);

    if (request.role == CompositionRole::Drums) {
        score.harmony = {1.0, "Percussion is unpitched; harmony is neutral."};
    } else {
        const std::size_t inScale = std::count_if(
            notes.begin(), notes.end(), [&](const auto& note) {
                return belongsToScale(note.pitch, root, intervals);
            });
        const double scaleFit = double(inScale) / notes.size();
        std::size_t strongNotes = 0, strongChordTones = 0;
        for (const CompositionNoteEvent& note : notes) {
            if (std::abs(note.startBeats - std::round(note.startBeats)) >=
                kEpsilon)
                continue;
            const CompositionHarmonySegment* harmony =
                harmonyAt(request, note.startBeats);
            if (!harmony) continue;
            ++strongNotes;
            const int noteClass = pitchClass(note.pitch);
            if (std::find(harmony->chordTonePitchClasses.begin(),
                          harmony->chordTonePitchClasses.end(), noteClass) !=
                    harmony->chordTonePitchClasses.end() ||
                (harmony->chordTonePitchClasses.empty() &&
                 noteClass == harmony->root))
                ++strongChordTones;
        }
        const double chordFit = strongNotes
                                    ? double(strongChordTones) / strongNotes
                                    : scaleFit;
        score.harmony.value = request.harmony.empty()
                                  ? scaleFit
                                  : scaleFit * 0.4 + chordFit * 0.6;
        score.harmony.explanation = std::to_string(inScale) + "/" +
                                    std::to_string(notes.size()) +
                                    " notes are in scale; " +
                                    std::to_string(strongChordTones) + "/" +
                                    std::to_string(strongNotes) +
                                    " harmony-covered strong notes are chord tones.";
    }

    std::vector<double> onsets;
    onsets.reserve(notes.size());
    for (const auto& note : notes) onsets.push_back(note.startBeats);
    std::sort(onsets.begin(), onsets.end());
    onsets.erase(std::unique(onsets.begin(), onsets.end(), [](double a, double b) {
                     return std::abs(a - b) < kEpsilon;
                 }),
                 onsets.end());
    const double actualPerBar = double(onsets.size()) / std::max(1, request.bars);
    double targetPerBar = 1.0;
    switch (request.role) {
    case CompositionRole::Melody: targetPerBar = 1.0 + density * 6.0; break;
    case CompositionRole::Bass: targetPerBar = 1.0 + density * 3.0; break;
    case CompositionRole::Chords: targetPerBar = 1.0 + density * 2.0; break;
    case CompositionRole::Drums: targetPerBar = 3.0 + density * 13.0; break;
    }
    score.rhythm.value =
        clamp01(1.0 - std::abs(actualPerBar - targetPerBar) /
                          std::max(1.0, targetPerBar));
    score.rhythm.explanation = decimal(actualPerBar) +
                               " onsets/bar versus target " +
                               decimal(targetPerBar) + ".";
    if (request.role == CompositionRole::Melody &&
        request.avoidOnsetProfile16.size() == 16 && !onsets.empty()) {
        double occupied = 0.0;
        for (double onset : onsets) {
            const double phase = std::fmod(onset, request.beatsPerBar) /
                                 request.beatsPerBar;
            const int slot = std::clamp(
                int(std::floor(phase * 16.0 + kEpsilon)), 0, 15);
            occupied += request.avoidOnsetProfile16[std::size_t(slot)];
        }
        const double complement =
            1.0 - occupied / double(onsets.size());
        score.rhythm.value =
            clamp01(score.rhythm.value * 0.6 + complement * 0.4);
        score.rhythm.explanation += " Complementary-space fit " +
                                    decimal(complement) + ".";
    }

    if (request.role == CompositionRole::Drums) {
        score.registerFit = {1.0, "GM drum keys use their fixed register."};
    } else {
        const CompositionPitchRange range = effectiveRange(request);
        const std::size_t inside = std::count_if(
            notes.begin(), notes.end(), [&](const auto& note) {
                return note.pitch >= range.lowest && note.pitch <= range.highest;
            });
        score.registerFit.value = double(inside) / notes.size();
        score.registerFit.explanation = std::to_string(inside) + "/" +
                                        std::to_string(notes.size()) +
                                        " notes fit the requested register.";
    }

    if (request.bars <= 1) {
        score.repetition = {1.0, "One bar has no repetition opportunity."};
    } else {
        std::set<std::tuple<int, int, int>> signatures;
        for (const auto& note : notes) {
            const int bar = std::max(
                0, int(std::floor(note.startBeats / request.beatsPerBar +
                                  kEpsilon)));
            const double insideBar = note.startBeats - bar * request.beatsPerBar;
            const int tick = int(std::lround(insideBar * 16.0));
            const int tone = request.role == CompositionRole::Drums
                                 ? note.pitch
                                 : pitchClass(note.pitch);
            signatures.emplace(bar, tick, tone);
        }
        int eligible = 0;
        int repeated = 0;
        for (const auto& signature : signatures) {
            const int bar = std::get<0>(signature);
            if (bar == 0) continue;
            ++eligible;
            if (signatures.count({bar - 1, std::get<1>(signature),
                                  std::get<2>(signature)}))
                ++repeated;
        }
        const double actual = eligible ? double(repeated) / eligible : 0.0;
        const double target = 0.75 - creativity * 0.50;
        score.repetition.value = clamp01(1.0 - std::abs(actual - target));
        score.repetition.explanation = decimal(actual * 100.0, 0) +
                                       "% repeats versus target " +
                                       decimal(target * 100.0, 0) + "%.";
    }

    if (request.role == CompositionRole::Drums) {
        score.voiceLeading = {1.0, "Percussion has no pitched voice leading."};
    } else {
        std::vector<CompositionNoteEvent> sorted = notes;
        sortNotes(sorted);
        std::vector<std::pair<double, double>> centres;
        for (std::size_t at = 0; at < sorted.size();) {
            std::size_t end = at + 1;
            double sum = sorted[at].pitch;
            while (end < sorted.size() &&
                   std::abs(sorted[end].startBeats - sorted[at].startBeats) <
                       kEpsilon) {
                sum += sorted[end].pitch;
                ++end;
            }
            centres.push_back({sorted[at].startBeats, sum / double(end - at)});
            at = end;
        }
        double movement = 0.0;
        for (std::size_t i = 1; i < centres.size(); ++i)
            movement += std::abs(centres[i].second - centres[i - 1].second);
        const double average = centres.size() > 1
                                   ? movement / double(centres.size() - 1)
                                   : 0.0;
        score.voiceLeading.value = 1.0 / (1.0 + average / 7.0);
        score.voiceLeading.explanation = decimal(average) +
                                         " semitones average onset movement.";
    }

    score.total = clamp01(score.harmony.value * 0.30 +
                          score.rhythm.value * 0.20 +
                          score.registerFit.value * 0.15 +
                          score.repetition.value * 0.15 +
                          score.voiceLeading.value * 0.20);
    return score;
}

std::vector<CompositionCandidate> generateCompositionCandidates(
    const CompositionRequest& request) {
    if (!validateCompositionRequest(request).valid()) return {};
    const int count = std::max(3, std::min(5, request.variationCount));
    std::vector<CompositionCandidate> candidates;
    candidates.reserve(std::size_t(count));
    for (int variation = 0; variation < count; ++variation) {
        Random random(request.seed +
                      std::uint64_t(variation + 1) * 0x9e3779b97f4a7c15ULL);
        CompositionCandidate candidate;
        candidate.variationIndex = variation;
        switch (request.role) {
        case CompositionRole::Melody:
            candidate.notes = generateMelody(request, variation, random);
            break;
        case CompositionRole::Bass:
            candidate.notes = generateBass(request, variation, random);
            break;
        case CompositionRole::Chords:
            candidate.notes = generateChords(request, variation, random);
            break;
        case CompositionRole::Drums:
            candidate.notes = generateDrums(request, variation, random);
            break;
        }
        sortNotes(candidate.notes);
        candidate.id = candidateId(request, variation, candidate.notes);
        candidate.score = scoreCompositionCandidate(request, candidate.notes);
        if (validateCompositionCandidate(request, candidate).valid())
            candidates.push_back(std::move(candidate));
    }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const auto& a, const auto& b) {
                         return a.score.total > b.score.total;
                     });
    return candidates;
}

void CompositionCandidateStore::replace(
    const CompositionRequest& request,
    const std::vector<CompositionCandidate>& candidates) {
    std::scoped_lock lock(m_mutex);
    m_candidates.clear();
    for (const CompositionCandidate& candidate : candidates)
        m_candidates.emplace(
            candidate.id, StoredCompositionCandidate{request, candidate});
}

std::optional<StoredCompositionCandidate> CompositionCandidateStore::find(
    const std::string& candidateId) const {
    std::scoped_lock lock(m_mutex);
    const auto found = m_candidates.find(candidateId);
    return found == m_candidates.end()
               ? std::nullopt
               : std::optional<StoredCompositionCandidate>(found->second);
}

std::uint64_t CompositionCandidateStore::nextSeed(std::uint64_t base) {
    std::scoped_lock lock(m_mutex);
    return base + (++m_generationSerial * 0x9e3779b97f4a7c15ULL);
}

void CompositionCandidateStore::clear() {
    std::scoped_lock lock(m_mutex);
    m_candidates.clear();
    m_generationSerial = 0;
}

} // namespace daw::ai
