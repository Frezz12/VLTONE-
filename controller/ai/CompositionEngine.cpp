#include "ai/CompositionEngine.hpp"
#include "MidiTools.hpp"

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
    for (auto scale : miditools::allScales())
        if (miditools::scaleId(scale) == name)
            return miditools::scaleDegrees(scale);
    return {};
}

double pulseBeats(const CompositionRequest& request) {
    if (!request.meterNumerator) return 1.0;
    const double unit = 4.0 / request.meterDenominator;
    return request.meterDenominator == 8 && request.meterNumerator > 3 &&
                   request.meterNumerator % 3 == 0 ? unit * 3 : unit;
}

bool strongBeat(const CompositionRequest& request, double beat) {
    const double phase = beat / pulseBeats(request);
    return std::abs(phase - std::round(phase)) < 0.04;
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
    // Validated melodic requests contain a scale tone. Keep the scoring helper
    // defensive when it is called independently with an invalid request.
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
    const int slots = std::max(1, int(std::lround(request.beatsPerBar * 4.0)));
    const double characterDensity = request.character == CompositionCharacter::Spacious
        ? 0.60 : request.character == CompositionCharacter::Rhythmic ? 1.30 : 1.0;
    const int count = std::clamp(int(std::lround((1.0 + density * 6.0) *
        characterDensity * request.beatsPerBar / 4.0)) + variation % 3 - 1, 1, slots);
    std::array<std::vector<int>, 2> rhythms;
    for (int half = 0; half < 2; ++half) {
        rhythms[half] = spreadSlots(slots, std::max(1, count - half),
                                   variation + half, creativity, random);
        preferFreeSlots(rhythms[half], slots, request.avoidOnsetProfile16, variation);
    }
    const std::vector<int> pool = scalePitchPool(request);
    const CompositionPitchRange range = effectiveRange(request);
    const int centre = nearestIndex(pool, (range.lowest + range.highest) / 2);
    std::array<std::vector<int>, 2> motifs;
    motifs[0].resize(std::size_t(count + 2), 0);
    const int direction = random.chance(.5) ? 1 : -1;
    for (std::size_t i = 1; i < motifs[0].size(); ++i) {
        // A small rise and return gives the question an audible contour.
        // Mostly stepwise motion leaves room for intentional repeated notes.
        int step = direction * (i <= std::size_t((count + 1) / 2) ? 1 : -1);
        if (random.chance(.20)) step = 0;
        if (random.chance(creativity * .18)) step *= 2;
        if (i > 1 && step == 0 && motifs[0][i - 1] == motifs[0][i - 2]) step = direction;
        motifs[0][i] = motifs[0][i - 1] + step;
    }
    // The second bar answers the first rather than replaying its pitches.
    for (int pitch : motifs[0]) motifs[1].push_back(direction - pitch);
    std::vector<CompositionNoteEvent> notes;
    int previous = pool[centre], lastStep = 0;
    for (int bar = 0; bar < request.bars; ++bar) {
        const double barStart = bar * request.beatsPerBar;
        const int section = (bar / 2) % 4; // two-bar A, A', B, A''
        auto positions = rhythms[bar % 2];
        // Keep the motif's opening, but let the answer breathe and develop.
        if (section == 1 && bar % 2 && positions.size() > 2)
            positions.pop_back();
        if (section == 2 && positions.size() > 1 && creativity > 0.2) {
            positions.back() = std::min(slots - 1, positions.back() + 1);
            if (bar % 2 && positions.size() > 2) positions.erase(positions.begin() + 1);
        }
        const bool finalBar = bar == request.bars - 1;
        if (finalBar && request.ending == CompositionEnding::Resolved)
            while (positions.size() > 1 && positions.back() > slots * 3 / 4)
                positions.pop_back();
        for (std::size_t i = 0; i < positions.size(); ++i) {
            const bool finalNote = finalBar && i + 1 == positions.size();
            int offset = motifs[bar % 2][i];
            if (section == 1 && i > positions.size() / 2) offset = -offset;
            if (section == 2) offset = 2 - offset;
            if (section == 3 && !finalBar) ++offset;
            const int index = std::clamp(centre + variation % 3 - 1 + offset,
                                        0, int(pool.size()) - 1);
            const double rawStart = barStart + positions[i] * request.beatsPerBar / slots;
            int pitch = pool[std::size_t(index)];
            if (!notes.empty() && std::abs(lastStep) >= 5)
                pitch = pool[std::clamp(nearestIndex(pool, previous) +
                    (lastStep > 0 ? -1 : 1), 0, int(pool.size()) - 1)];
            if (strongBeat(request, rawStart)) {
                if (const auto* harmony = harmonyAt(request, rawStart)) {
                    auto tones = harmony->chordTonePitchClasses;
                    if (tones.empty() && harmony->root >= 0) tones.push_back(harmony->root);
                    const int fitted = nearestPitchFromClasses(tones, pitch, range);
                    if (fitted >= 0) pitch = fitted;
                }
            }
            if (finalNote && request.ending == CompositionEnding::Resolved) {
                const int fitted = nearestPitchForClass(
                    cadencePitchClass(request), pitch, range);
                if (fitted >= 0) pitch = fitted;
            }
            const int nextSlot = i + 1 < positions.size() ? positions[i + 1] : slots;
            const double gap = (nextSlot - positions[i]) * request.beatsPerBar / slots;
            const double gate = request.character == CompositionCharacter::Rhythmic ? 0.60 : 0.84;
            const int accent = strongBeat(request, rawStart) ? 10 : 0;
            const int velocity = 84 + accent + (section == 2 ? 6 : 0) +
                int(std::lround(random.integer(-12, 12) * (0.25 + request.expression)));
            addNote(notes, pitch, rawStart,
                    finalNote && request.ending == CompositionEnding::Resolved
                        ? totalBeats - rawStart : std::max(0.015625, gap * gate),
                    velocity, totalBeats);
            lastStep = pitch - previous;
            previous = pitch;
        }
    }
    // Timing is shaped after the phrase, then bounded against the next onset.
    for (std::size_t i = 0; i < notes.size(); ++i) {
        auto& note = notes[i];
        const double eighth = note.startBeats * 2.0;
        if (std::abs(eighth - std::round(eighth)) < kEpsilon &&
            int(std::llround(eighth)) % 2) {
            const double next = i + 1 < notes.size() ? notes[i + 1].startBeats : totalBeats;
            note.startBeats += std::min(request.swing - 0.5, std::max(0.0, next - note.startBeats - 0.0625));
        }
        note.startBeats = std::clamp(note.startBeats +
            (random.unit() - 0.5) * 0.025 * request.expression,
            0.0, totalBeats - 0.015625);
    }
    for (std::size_t i = 0; i < notes.size(); ++i) {
        if (i) notes[i].startBeats = std::max(notes[i].startBeats,
                                             notes[i - 1].startBeats + 1.0 / 960.0);
    }
    for (std::size_t i = 0; i < notes.size(); ++i) {
        const double end = i + 1 < notes.size() ? notes[i + 1].startBeats : totalBeats;
        notes[i].lengthBeats = std::min(notes[i].lengthBeats,
                                       std::max(1.0 / 960.0, end - notes[i].startBeats));
    }
    if (!notes.empty() && request.ending == CompositionEnding::Resolved)
        notes.back().lengthBeats = totalBeats - notes.back().startBeats;
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

std::vector<CompositionNoteEvent> renderChords(
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

    std::vector<double> onsets;
    for (int bar = 0; bar < request.bars; ++bar)
        for (int hit = 0; hit < hits; ++hit)
            onsets.push_back(bar * request.beatsPerBar + hit * request.beatsPerBar / hits);
    for (const auto& segment : request.harmony) onsets.push_back(segment.startBeats);
    std::sort(onsets.begin(), onsets.end());
    onsets.erase(std::unique(onsets.begin(), onsets.end(), [](double a, double b) {
        return std::abs(a - b) < 1.0e-6;
    }), onsets.end());
    for (std::size_t at = 0; at < onsets.size(); ++at) {
            const double start = onsets[at];
            const int bar = int(start / request.beatsPerBar);
            const int hit = int((start - bar * request.beatsPerBar) * hits / request.beatsPerBar);
            int degree = progression[std::size_t((bar + (variation >= 3)) % 4)];
            if (hit > 0 && random.chance(creativity * 0.35)) ++degree;
            std::vector<int> chord;
            const CompositionHarmonySegment* harmony = harmonyAt(request, start);
            if (!harmony && bar == request.bars - 1 && request.ending == CompositionEnding::Resolved)
                degree = 0;
            const int toneCount = harmony && !harmony->chordTonePitchClasses.empty()
                ? int(harmony->chordTonePitchClasses.size()) : density + creativity > 1.15 ? 4 : 3;
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
            // Compare compact inversions against the actual previous voices.
            // Every supplied chord tone is retained, including extensions.
            if (harmony && !harmony->chordTonePitchClasses.empty()) {
                auto classes = harmony->chordTonePitchClasses;
                std::sort(classes.begin(), classes.end());
                classes.erase(std::unique(classes.begin(), classes.end()), classes.end());
                double bestCost = 1.0e9;
                for (int bass = range.lowest; bass <= range.highest; ++bass) {
                    const auto first = std::find(classes.begin(), classes.end(), pitchClass(bass));
                    if (first == classes.end()) continue;
                    std::vector<int> voiced{bass};
                    for (std::size_t voice = 1; voice < classes.size(); ++voice) {
                        const int pc = classes[(std::size_t(first - classes.begin()) + voice) % classes.size()];
                        int pitch = voiced.back() + 1;
                        while (pitchClass(pitch) != pc) ++pitch;
                        voiced.push_back(pitch);
                    }
                    if (voiced.back() > range.highest) continue;
                    double cost = 0;
                    for (std::size_t voice = 0; voice < voiced.size(); ++voice) {
                        const int target = previous.empty()
                            ? (range.lowest + range.highest) / 2 + (int(voice) - 1) * 4 + (variation % 3 - 1) * 7
                            : previous[std::min(voice, previous.size() - 1)];
                        cost += std::abs(voiced[voice] - target);
                    }
                    if (cost < bestCost) { bestCost = cost; chord = std::move(voiced); }
                }
                if (bestCost == 1.0e9) return {}; // The register cannot hold this chord.
            }
            previous = chord;
            double end = at + 1 < onsets.size() ? onsets[at + 1] : totalBeats;
            if (harmony) end = std::min(end, harmony->startBeats + harmony->lengthBeats);
            const double length = (end - start) * 0.94;
            for (int pitch : chord)
                addNote(notes, pitch, start, length,
                        76 + random.integer(-5, 13), totalBeats);
    }
    return notes;
}

std::vector<CompositionNoteEvent> generateChords(
    const CompositionRequest& request, int variation, Random& random) {
    CompositionRequest harmonized = request;
    if (harmonized.harmony.empty()) {
        static constexpr int progressions[][4] = {
            {0, 5, 3, 4}, {0, 3, 1, 4}, {0, 2, 5, 3},
            {0, 5, 2, 6}, {0, 3, 6, 2}, {0, 6, 5, 6}, {0, 3, 4, 0}
        };
        const auto scale = effectiveScale(request);
        const int pattern = (variation + random.integer(0, 6)) % 7;
        const double total = request.bars * request.beatsPerBar;
        const double step = request.chordBars * request.beatsPerBar;
        for (int at = 0; at * step < total - kEpsilon; ++at) {
            int degree = progressions[pattern][at % 4];
            if ((at + 1) * step >= total - kEpsilon &&
                request.ending == CompositionEnding::Resolved) degree = 0;
            CompositionHarmonySegment segment;
            segment.startBeats = at * step;
            segment.lengthBeats = std::min(step, total - segment.startBeats);
            segment.root = pitchClass(effectiveRoot(request) + scale[degree % scale.size()]);
            for (int tone = 0; tone < (request.seventhChords ? 4 : 3); ++tone)
                segment.chordTonePitchClasses.push_back(pitchClass(effectiveRoot(request) +
                    scale[(degree + tone * 2) % scale.size()]));
            harmonized.harmony.push_back(std::move(segment));
        }
    }
    auto notes = renderChords(harmonized, variation, random);
    for (auto& note : notes) {
        const double phase = note.startBeats * 2;
        if (std::abs(phase - std::round(phase)) < kEpsilon && int(std::llround(phase)) % 2) {
            const double shift = std::min(request.swing - 0.5, note.lengthBeats * 0.4);
            note.startBeats += shift;
            note.lengthBeats -= shift;
        }
        note.velocity = std::clamp(note.velocity + int(std::lround(
            random.integer(-12, 12) * request.expression)), 1, 127);
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
    if (request.meterNumerator < 0 || request.meterNumerator > 32 ||
        (request.meterDenominator != 1 && request.meterDenominator != 2 &&
         request.meterDenominator != 4 && request.meterDenominator != 8 &&
         request.meterDenominator != 16 && request.meterDenominator != 32) ||
        (request.meterNumerator && std::abs(request.beatsPerBar -
            request.meterNumerator * 4.0 / request.meterDenominator) > kEpsilon))
        result.errors.push_back("meter must agree with beatsPerBar");
    if (!std::isfinite(request.swing) || request.swing < 0.5 || request.swing > 0.75 ||
        !std::isfinite(request.expression) || request.expression < 0 || request.expression > 1)
        result.errors.push_back("swing or expression is outside its range");
    if (request.chordBars != 0.5 && request.chordBars != 1 && request.chordBars != 2)
        result.errors.push_back("chordBars must be 0.5, 1 or 2");
    if (request.character < CompositionCharacter::Singing || request.character > CompositionCharacter::Spacious ||
        request.ending < CompositionEnding::Resolved || request.ending > CompositionEnding::Loop)
        result.errors.push_back("character or ending is not supported");
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
    if (result.valid() && request.role != CompositionRole::Drums) {
        const auto range = effectiveRange(request);
        const auto scale = effectiveScale(request);
        bool any = false;
        for (int pitch = range.lowest; pitch <= range.highest; ++pitch)
            any |= belongsToScale(pitch, effectiveRoot(request), scale);
        if (!any) result.errors.push_back("pitchRange contains no notes in the selected scale");
    }
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
        const auto range = effectiveRange(request);
        if (request.role != CompositionRole::Drums &&
            (note.pitch < range.lowest || note.pitch > range.highest))
            result.errors.push_back("note pitch is outside the requested register");
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
            if (!strongBeat(request, note.startBeats))
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
    case CompositionRole::Melody:
        targetPerBar = std::max(1.0, (1.0 + density * 6.0) * request.beatsPerBar / 4.0 *
            (request.character == CompositionCharacter::Spacious ? 0.60 :
             request.character == CompositionCharacter::Rhythmic ? 1.30 : 1.0));
        break;
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
    const bool melodic = request.role == CompositionRole::Melody || request.role == CompositionRole::Chords;
    const int attempts = melodic ? count * 6 : count;
    candidates.reserve(std::size_t(attempts));
    for (int variation = 0; variation < attempts; ++variation) {
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
    if (!melodic) return candidates;
    // Select by both musical fit and distance from already presented options.
    // Velocities and UUIDs do not make two otherwise identical phrases distinct.
    std::vector<CompositionCandidate> selected;
    while (!candidates.empty() && int(selected.size()) < count) {
        std::size_t best = 0;
        double bestValue = -1;
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            double similarity = 0;
            for (const auto& chosen : selected) {
                std::size_t same = 0;
                const auto& a = candidates[i].notes;
                const auto& b = chosen.notes;
                for (std::size_t n = 0; n < std::min(a.size(), b.size()); ++n)
                    if (a[n].pitch == b[n].pitch && std::abs(a[n].startBeats - b[n].startBeats) < 0.04)
                        ++same;
                similarity = std::max(similarity, double(same) / std::max(a.size(), b.size()));
            }
            const double value = candidates[i].score.total - similarity * 0.35;
            if (value > bestValue) { bestValue = value; best = i; }
        }
        selected.push_back(std::move(candidates[best]));
        candidates.erase(candidates.begin() + best);
    }
    return selected;
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
