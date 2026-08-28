#include "ai/CompositionEngine.hpp"

#include <cmath>
#include <cstdio>
#include <set>
#include <sstream>
#include <string>

namespace ai = daw::ai;

static int failures = 0;

static bool check(bool condition, const char* what) {
    std::printf("%s  %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++failures;
    return condition;
}

static bool sameNotes(const std::vector<ai::CompositionNoteEvent>& a,
                      const std::vector<ai::CompositionNoteEvent>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].pitch != b[i].pitch ||
            a[i].startBeats != b[i].startBeats ||
            a[i].lengthBeats != b[i].lengthBeats ||
            a[i].velocity != b[i].velocity)
            return false;
    }
    return true;
}

static std::string fingerprint(
    const std::vector<ai::CompositionNoteEvent>& notes) {
    std::ostringstream out;
    for (const auto& note : notes)
        out << note.pitch << '@' << note.startBeats << '+' << note.lengthBeats
            << ':' << note.velocity << ';';
    return out.str();
}

static bool completeScore(const ai::CompositionCandidateScore& score) {
    const ai::CompositionScoreComponent* components[] = {
        &score.harmony, &score.rhythm, &score.registerFit, &score.repetition,
        &score.voiceLeading};
    for (const auto* component : components)
        if (component->value < 0.0 || component->value > 1.0 ||
            component->explanation.empty())
            return false;
    return score.total >= 0.0 && score.total <= 1.0;
}

int main() {
    ai::CompositionRequest melody;
    melody.role = ai::CompositionRole::Melody;
    melody.bars = 4;
    melody.seed = 0x12345678ULL;
    melody.variationCount = 5;
    melody.creativity = 0.63;
    melody.keyRoot = 9; // A
    melody.scale = "natural_minor";
    melody.pitchRange = ai::CompositionPitchRange{60, 84};
    melody.rhythmicDensity = 0.58;

    const auto first = ai::generateCompositionCandidates(melody);
    const auto again = ai::generateCompositionCandidates(melody);
    check(first.size() == 5, "the requested five melody variants are generated");
    check(first.size() == again.size(), "a repeated seeded run has the same size");

    bool deterministic = first.size() == again.size();
    bool valid = !first.empty();
    bool scored = !first.empty();
    std::set<std::string> ids;
    std::set<std::string> musicalVariants;
    for (std::size_t i = 0; i < first.size() && i < again.size(); ++i) {
        deterministic = deterministic && first[i].id == again[i].id &&
                        first[i].variationIndex == again[i].variationIndex &&
                        sameNotes(first[i].notes, again[i].notes) &&
                        first[i].score.total == again[i].score.total;
        valid = valid &&
                ai::validateCompositionCandidate(melody, first[i]).valid();
        scored = scored && completeScore(first[i].score);
        ids.insert(first[i].id);
        musicalVariants.insert(fingerprint(first[i].notes));
    }
    check(deterministic, "the same seed produces byte-identical candidates");
    check(valid, "every generated melody passes candidate validation");
    check(scored, "all five explainable score components and total are bounded");
    check(ids.size() == first.size(), "candidate IDs are stable and unique");
    check(musicalVariants.size() == first.size(),
          "variants differ in musical note data, not just their IDs");

    ai::CompositionRequest complementary = melody;
    complementary.bars = 1;
    complementary.variationCount = 3;
    complementary.avoidOnsetProfile16.assign(16, 0.0);
    complementary.avoidOnsetProfile16[0] = 1.0;
    const auto answers = ai::generateCompositionCandidates(complementary);
    bool avoidsBusyDownbeat = !answers.empty();
    for (const auto& candidate : answers)
        avoidsBusyDownbeat =
            avoidsBusyDownbeat && !candidate.notes.empty() &&
            candidate.notes.front().startBeats > 0.0 &&
            candidate.score.rhythm.explanation.find("Complementary-space") !=
                std::string::npos;
    check(avoidsBusyDownbeat,
          "melody variants answer a busy onset slot and score the free space");

    bool melodyInKeyAndRange = !first.empty();
    const std::set<int> aMinor = {9, 11, 0, 2, 4, 5, 7};
    for (const auto& note : first.front().notes)
        melodyInKeyAndRange =
            melodyInKeyAndRange && note.pitch >= 60 && note.pitch <= 84 &&
            aMinor.count(note.pitch % 12) != 0;
    check(melodyInKeyAndRange,
          "melody notes obey the requested A-minor scale and register");

    ai::CompositionRequest bass = melody;
    bass.role = ai::CompositionRole::Bass;
    bass.seed = 99;
    bass.variationCount = 3;
    bass.pitchRange.reset(); // exercise the role's default bass register
    const auto basses = ai::generateCompositionCandidates(bass);
    bool bassValidAndLow = basses.size() == 3;
    double bassPitchSum = 0.0;
    std::size_t bassNotes = 0;
    for (const auto& candidate : basses) {
        bassValidAndLow =
            bassValidAndLow &&
            ai::validateCompositionCandidate(bass, candidate).valid();
        for (const auto& note : candidate.notes) {
            bassValidAndLow =
                bassValidAndLow && note.pitch >= 28 && note.pitch <= 52;
            bassPitchSum += note.pitch;
            ++bassNotes;
        }
    }
    check(bassValidAndLow && bassNotes > 0 && bassPitchSum / bassNotes < 50.0,
          "bass generation is valid, monophonic and stays in its low register");

    ai::CompositionRequest chords = melody;
    chords.role = ai::CompositionRole::Chords;
    chords.variationCount = 1; // documented clamp
    chords.pitchRange.reset();
    const auto chordCandidates = ai::generateCompositionCandidates(chords);
    check(chordCandidates.size() == 3 &&
              ai::validateCompositionCandidate(chords, chordCandidates.front())
                  .valid(),
          "chords generate polyphonic candidates and clamp the count to three");

    ai::CompositionRequest drums = melody;
    drums.role = ai::CompositionRole::Drums;
    drums.variationCount = 99; // documented clamp
    drums.pitchRange.reset();
    const auto drumCandidates = ai::generateCompositionCandidates(drums);
    check(drumCandidates.size() == 5 &&
              ai::validateCompositionCandidate(drums, drumCandidates.front())
                  .valid(),
          "drums generate valid GM patterns and clamp the count to five");

    ai::CompositionCandidate broken;
    broken.notes = {{60, 0.0, 1.0, 100}, {62, 0.5, 0.0, 200},
                    {128, 20.0, 1.0, 100}};
    const auto brokenValidation =
        ai::validateCompositionCandidate(melody, broken);
    check(!brokenValidation.valid() && brokenValidation.errors.size() >= 4,
          "invalid pitch, time, duration, velocity and overlap are rejected");

    ai::CompositionRequest bad = melody;
    bad.bars = 0;
    bad.scale = "made_up";
    bad.keyRoot = 12;
    check(!ai::validateCompositionRequest(bad).valid() &&
              ai::generateCompositionCandidates(bad).empty(),
          "an invalid request is reported and produces no candidates");

    std::printf("\n%s\n", failures == 0 ? "ALL PASSED" : "FAILURES PRESENT");
    return failures;
}
