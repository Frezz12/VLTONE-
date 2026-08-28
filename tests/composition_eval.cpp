#include "ai/CompositionEngine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace ai = daw::ai;

namespace {

struct Scenario {
    std::string name;
    ai::CompositionRequest request;
    double minimumMeanScore = 0.65;
    double minimumHarmony = 0.75;
    double minimumMeanRhythm = 0.50;
};

struct Evaluation {
    Scenario scenario;
    std::vector<ai::CompositionCandidate> candidates;
    bool deterministic = false;
};

void hashValue(std::uint64_t& hash, std::uint64_t value) {
    for (int byte = 0; byte < 8; ++byte) {
        hash ^= (value >> (byte * 8)) & 0xffULL;
        hash *= 1099511628211ULL;
    }
}

std::string noteFingerprint(
    const std::vector<ai::CompositionNoteEvent>& notes) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto& note : notes) {
        hashValue(hash, std::uint64_t(note.pitch));
        hashValue(hash, std::uint64_t(std::llround(note.startBeats * 960.0)));
        hashValue(hash, std::uint64_t(std::llround(note.lengthBeats * 960.0)));
        hashValue(hash, std::uint64_t(note.velocity));
    }
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << hash;
    return out.str();
}

std::vector<ai::CompositionHarmonySegment> fourBarHarmony(
    std::initializer_list<std::pair<int, std::vector<int>>> chords) {
    std::vector<ai::CompositionHarmonySegment> result;
    double start = 0.0;
    for (const auto& [root, tones] : chords) {
        result.push_back({start, 4.0, root, tones});
        start += 4.0;
    }
    return result;
}

std::vector<Scenario> scenarios() {
    ai::CompositionRequest melody;
    melody.role = ai::CompositionRole::Melody;
    melody.bars = 4;
    melody.seed = 0x6d656c6f6479ULL;
    melody.variationCount = 3;
    melody.creativity = 0.62;
    melody.keyRoot = 4; // E natural minor
    melody.scale = "natural_minor";
    melody.pitchRange = ai::CompositionPitchRange{60, 84};
    melody.rhythmicDensity = 0.58;
    melody.harmony = fourBarHarmony(
        {{4, {4, 7, 11}}, {0, {0, 4, 7}}, {7, {7, 11, 2}}, {2, {2, 6, 9}}});
    melody.avoidOnsetProfile16 = {
        1.0, 0.1, 0.2, 0.0, 0.9, 0.1, 0.3, 0.0,
        0.8, 0.0, 0.2, 0.0, 0.9, 0.1, 0.3, 0.0};

    ai::CompositionRequest bass;
    bass.role = ai::CompositionRole::Bass;
    bass.bars = 4;
    bass.seed = 0x62617373ULL;
    bass.variationCount = 3;
    bass.creativity = 0.42;
    bass.keyRoot = 9; // A natural minor
    bass.scale = "natural_minor";
    bass.pitchRange = ai::CompositionPitchRange{28, 52};
    bass.rhythmicDensity = 0.52;
    bass.harmony = fourBarHarmony(
        {{9, {9, 0, 4}}, {5, {5, 9, 0}}, {0, {0, 4, 7}}, {7, {7, 11, 2}}});

    ai::CompositionRequest chords;
    chords.role = ai::CompositionRole::Chords;
    chords.bars = 4;
    chords.seed = 0x63686f726473ULL;
    chords.variationCount = 3;
    chords.creativity = 0.35;
    chords.keyRoot = 2; // D dorian
    chords.scale = "dorian";
    chords.pitchRange = ai::CompositionPitchRange{48, 76};
    chords.rhythmicDensity = 0.45;
    chords.harmony = fourBarHarmony(
        {{2, {2, 5, 9}}, {7, {7, 11, 2}}, {0, {0, 4, 7}}, {9, {9, 0, 4}}});

    ai::CompositionRequest drums;
    drums.role = ai::CompositionRole::Drums;
    drums.bars = 4;
    drums.seed = 0x6472756d73ULL;
    drums.variationCount = 3;
    drums.creativity = 0.55;
    drums.rhythmicDensity = 0.68;

    return {{"melody_busy_arrangement", melody, 0.80, 0.95, 0.72},
            {"bass_minor_progression", bass, 0.72, 0.95, 0.60},
            {"chords_dorian_progression", chords, 0.88, 0.95, 0.85},
            {"drums_four_on_floor", drums, 0.80, 1.00, 0.60}};
}

bool phraseRole(ai::CompositionRole role) {
    return role == ai::CompositionRole::Melody ||
           role == ai::CompositionRole::Bass;
}

int pitchClass(int pitch) {
    return ((pitch % 12) + 12) % 12;
}

int cadenceTarget(const ai::CompositionRequest& request) {
    const double end = request.bars * request.beatsPerBar;
    for (const auto& harmony : request.harmony)
        if (harmony.root >= 0 && harmony.startBeats < end &&
            harmony.startBeats + harmony.lengthBeats >= end - 1.0e-6)
            return harmony.root;
    return request.keyRoot.value_or(0);
}

bool hasCadentialEnding(const ai::CompositionRequest& request,
                        const ai::CompositionCandidate& candidate) {
    if (!phraseRole(request.role) || candidate.notes.empty()) return false;
    const auto last = std::max_element(
        candidate.notes.begin(), candidate.notes.end(),
        [](const auto& a, const auto& b) { return a.startBeats < b.startBeats; });
    const double phraseEnd = request.bars * request.beatsPerBar;
    return pitchClass(last->pitch) == pitchClass(cadenceTarget(request)) &&
           std::abs(last->startBeats + last->lengthBeats - phraseEnd) < 1.0e-6;
}

bool hasMotifDevelopment(const ai::CompositionRequest& request,
                         const ai::CompositionCandidate& candidate) {
    if (!phraseRole(request.role) || request.bars < 3) return false;
    std::vector<std::vector<const ai::CompositionNoteEvent*>> bars(
        std::size_t(request.bars));
    for (const auto& note : candidate.notes) {
        const int bar = std::clamp(
            int(std::floor(note.startBeats / request.beatsPerBar)), 0,
            request.bars - 1);
        bars[std::size_t(bar)].push_back(&note);
    }

    std::vector<std::vector<long long>> rhythms;
    std::vector<std::vector<int>> contours;
    std::vector<int> startingPitches;
    for (auto& notes : bars) {
        std::sort(notes.begin(), notes.end(), [](const auto* a, const auto* b) {
            return a->startBeats < b->startBeats;
        });
        if (notes.size() < 2) return false;
        std::vector<long long> rhythm;
        std::vector<int> contour;
        const int firstPitch = notes.front()->pitch;
        startingPitches.push_back(firstPitch);
        const int bar = int(std::floor(notes.front()->startBeats /
                                       request.beatsPerBar));
        for (const auto* note : notes) {
            rhythm.push_back(std::llround(
                (note->startBeats - bar * request.beatsPerBar) * 960.0));
            contour.push_back(note->pitch - firstPitch);
        }
        rhythms.push_back(std::move(rhythm));
        contours.push_back(std::move(contour));
    }

    const bool recognisableRhythm = std::all_of(
        rhythms.begin() + 1, rhythms.end(),
        [&](const auto& rhythm) { return rhythm == rhythms.front(); });
    bool developedMiddle = false;
    for (std::size_t bar = 1; bar + 1 < contours.size(); ++bar)
        developedMiddle = developedMiddle ||
                          contours[bar] != contours.front() ||
                          startingPitches[bar] != startingPitches.front();
    return recognisableRhythm && developedMiddle;
}

std::string candidateRow(const Scenario& scenario, std::size_t rank,
                         const ai::CompositionCandidate& candidate) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << scenario.name << '\t' << rank + 1 << '\t' << candidate.id << '\t'
        << noteFingerprint(candidate.notes) << '\t' << candidate.notes.size()
        << '\t' << std::fixed << std::setprecision(3) << candidate.score.total
        << '\t' << candidate.score.harmony.value << '\t'
        << candidate.score.rhythm.value << '\t'
        << candidate.score.registerFit.value << '\t'
        << candidate.score.repetition.value << '\t'
        << candidate.score.voiceLeading.value << '\t';
    if (phraseRole(scenario.request.role))
        out << (hasCadentialEnding(scenario.request, candidate) ? 1 : 0)
            << '\t'
            << (hasMotifDevelopment(scenario.request, candidate) ? 1 : 0);
    else
        out << "na\tna";
    out << '\n';
    return out.str();
}

std::vector<Evaluation> evaluate() {
    std::vector<Evaluation> result;
    for (Scenario scenario : scenarios()) {
        auto first = ai::generateCompositionCandidates(scenario.request);
        const auto second = ai::generateCompositionCandidates(scenario.request);
        bool deterministic = first.size() == second.size();
        for (std::size_t i = 0; deterministic && i < first.size(); ++i)
            deterministic = candidateRow(scenario, i, first[i]) ==
                            candidateRow(scenario, i, second[i]);
        result.push_back(
            {std::move(scenario), std::move(first), deterministic});
    }
    return result;
}

std::string report(const std::vector<Evaluation>& evaluations) {
    std::ostringstream out;
    out << "composition-eval-v2\n"
        << "scenario\trank\tcandidate_id\tnote_hash\tnotes\ttotal\tharmony"
           "\trhythm\tregister\trepetition\tvoice_leading\tcadence"
           "\tmotif_development\n";
    for (const auto& evaluation : evaluations)
        for (std::size_t i = 0; i < evaluation.candidates.size(); ++i)
            out << candidateRow(evaluation.scenario, i,
                                evaluation.candidates[i]);
    return out.str();
}

std::vector<std::string> gateFailures(
    const std::vector<Evaluation>& evaluations) {
    std::vector<std::string> failures;
    for (const auto& evaluation : evaluations) {
        const auto& scenario = evaluation.scenario;
        const auto& candidates = evaluation.candidates;
        if (candidates.size() != 3)
            failures.push_back(scenario.name + ": expected 3 candidates");
        if (!evaluation.deterministic)
            failures.push_back(scenario.name + ": output is not deterministic");

        std::set<std::string> fingerprints;
        double scoreSum = 0.0;
        double lowestHarmony = 1.0;
        double rhythmSum = 0.0;
        for (const auto& candidate : candidates) {
            if (!ai::validateCompositionCandidate(scenario.request, candidate)
                     .valid())
                failures.push_back(scenario.name + ": invalid candidate");
            fingerprints.insert(noteFingerprint(candidate.notes));
            scoreSum += candidate.score.total;
            lowestHarmony =
                std::min(lowestHarmony, candidate.score.harmony.value);
            rhythmSum += candidate.score.rhythm.value;
            if (candidate.score.registerFit.value < 0.999)
                failures.push_back(scenario.name + ": register gate failed");
            if (phraseRole(scenario.request.role) &&
                !hasCadentialEnding(scenario.request, candidate))
                failures.push_back(scenario.name + ": cadence gate failed");
            if (phraseRole(scenario.request.role) &&
                !hasMotifDevelopment(scenario.request, candidate))
                failures.push_back(scenario.name +
                                   ": motif-development gate failed");
        }
        if (fingerprints.size() != candidates.size())
            failures.push_back(scenario.name + ": candidates are not diverse");
        const double count = std::max<std::size_t>(1, candidates.size());
        if (scoreSum / count < scenario.minimumMeanScore)
            failures.push_back(scenario.name + ": mean-score gate failed");
        if (lowestHarmony + 1.0e-9 < scenario.minimumHarmony)
            failures.push_back(scenario.name + ": harmony gate failed");
        if (rhythmSum / count + 1.0e-9 < scenario.minimumMeanRhythm)
            failures.push_back(scenario.name + ": mean-rhythm gate failed");
    }
    return failures;
}

std::string readText(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream out;
    out << input.rdbuf();
    std::string value = out.str();
    value.erase(std::remove(value.begin(), value.end(), '\r'), value.end());
    return value;
}

bool writeRatingsTemplate(const std::string& path,
                          const std::vector<Evaluation>& evaluations) {
    std::ofstream output(path, std::ios::binary);
    if (!output) return false;
    output << "scenario\trole\tcandidate_id\tmusicality_1_5\tcontext_fit_1_5"
              "\trhythm_1_5\twould_use_0_1\tnotes\n";
    for (const auto& evaluation : evaluations)
        for (const auto& candidate : evaluation.candidates)
            output << evaluation.scenario.name << '\t'
                   << ai::compositionRoleName(evaluation.scenario.request.role)
                   << '\t' << candidate.id << "\t\t\t\t\t\n";
    return output.good();
}

} // namespace

int main(int argc, char** argv) {
    const auto evaluations = evaluate();
    const std::string actual = report(evaluations);
    std::cout << actual;

    std::vector<std::string> failures = gateFailures(evaluations);
    const std::string expected = readText(DAW_COMPOSITION_EVAL_GOLDEN);
    if (expected != actual)
        failures.push_back("golden regression snapshot differs");

    if (argc == 3 && std::string(argv[1]) == "--ratings-template" &&
        !writeRatingsTemplate(argv[2], evaluations))
        failures.push_back("could not write ratings template");
    else if (argc != 1 && argc != 3)
        failures.push_back("usage: composition_eval [--ratings-template PATH]");

    for (const std::string& failure : failures)
        std::cerr << "FAIL " << failure << '\n';
    if (failures.empty()) std::cout << "PASS all quality gates and snapshot\n";
    return failures.empty() ? 0 : 1;
}
