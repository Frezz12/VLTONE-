#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace daw::ai {

/// The musical job a candidate should perform in the arrangement.
enum class CompositionRole { Melody, Bass, Chords, Drums };

/// Inclusive MIDI pitch bounds. Ignored for drums, whose pitches are GM keys.
struct CompositionPitchRange {
    int lowest = 0;
    int highest = 127;
};

/// Project-relative harmony copied into the request as beats from the start of
/// the generated part. Empty means the engine may choose its own progression.
struct CompositionHarmonySegment {
    double startBeats = 0.0;
    double lengthBeats = 4.0;
    int root = -1;
    std::vector<int> chordTonePitchClasses;
};

/// A compact, model-independent brief for deterministic MIDI generation.
struct CompositionRequest {
    CompositionRole role = CompositionRole::Melody;
    int bars = 4;
    std::uint64_t seed = 0;
    /// Always clamped to 3..5 by generateCompositionCandidates().
    int variationCount = 3;
    double creativity = 0.5;       ///< 0 = stable/repetitive, 1 = adventurous
    std::optional<int> keyRoot;    ///< pitch class 0..11; absent = C
    std::optional<std::string> scale; ///< absent = major
    std::optional<CompositionPitchRange> pitchRange;
    double rhythmicDensity = 0.5;  ///< 0 = sparse, 1 = busy
    double beatsPerBar = 4.0;
    std::vector<CompositionHarmonySegment> harmony;
    /// Existing arrangement onsets by sixteenth-note phase. Melody generation
    /// prefers lower values so a new line answers rather than doubles it.
    std::vector<double> avoidOnsetProfile16;
};

/// Same beat-relative shape as NoteModel, without document ownership/UUIDs.
struct CompositionNoteEvent {
    int pitch = 60;
    double startBeats = 0.0;
    double lengthBeats = 1.0;
    int velocity = 100;
};

struct CompositionScoreComponent {
    double value = 0.0;            ///< normalised 0..1
    std::string explanation;
};

/// Explainable quality signals. Total is a fixed weighted sum of the five.
struct CompositionCandidateScore {
    CompositionScoreComponent harmony;
    CompositionScoreComponent rhythm;
    CompositionScoreComponent registerFit;
    CompositionScoreComponent repetition;
    CompositionScoreComponent voiceLeading;
    double total = 0.0;
};

struct CompositionCandidate {
    std::string id;                ///< stable for request, seed and note data
    int variationIndex = 0;
    std::vector<CompositionNoteEvent> notes;
    CompositionCandidateScore score;
};

struct CompositionValidation {
    std::vector<std::string> errors;
    bool valid() const noexcept { return errors.empty(); }
};

/// Validate the trust-boundary fields before generation. Variation count is not
/// an error because it has the documented 3..5 clamp.
CompositionValidation validateCompositionRequest(
    const CompositionRequest& request);

/// Validate non-empty notes, MIDI/time bounds and monophonic melody/bass.
CompositionValidation validateCompositionCandidate(
    const CompositionRequest& request,
    const CompositionCandidate& candidate);

CompositionCandidateScore scoreCompositionCandidate(
    const CompositionRequest& request,
    const std::vector<CompositionNoteEvent>& notes);

/// Generate 3..5 deterministic, independently scored candidates.
/// An invalid request returns an empty vector.
std::vector<CompositionCandidate> generateCompositionCandidates(
    const CompositionRequest& request);

struct StoredCompositionCandidate {
    CompositionRequest request;
    CompositionCandidate candidate;
};

/// Per-panel ephemeral candidates. Applying by ID means the model cannot alter
/// the validated note list between preview/scoring and insertion.
class CompositionCandidateStore {
public:
    void replace(const CompositionRequest& request,
                 const std::vector<CompositionCandidate>& candidates);
    std::optional<StoredCompositionCandidate> find(
        const std::string& candidateId) const;
    std::uint64_t nextSeed(std::uint64_t base);
    void clear();

private:
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, StoredCompositionCandidate> m_candidates;
    std::uint64_t m_generationSerial = 0;
};

const char* compositionRoleName(CompositionRole role) noexcept;

} // namespace daw::ai
