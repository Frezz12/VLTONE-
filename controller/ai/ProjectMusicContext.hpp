#pragma once

#include "model/Document.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace daw {
class EngineController;
}

namespace daw::ai {

/// A key with enough provenance for an agent to decide how much to trust it.
struct MusicKeySummary {
    int root = -1;
    std::string rootName;
    std::string scale;
    std::string source;
    double confidence = 0.0;

    bool available() const noexcept { return root >= 0 && !scale.empty(); }
};

/// Activity measured only inside the requested bar range.
struct MusicActivitySummary {
    std::size_t noteCount = 0;
    double activeBeats = 0.0;
    double activityRatio = 0.0;
    double noteDensityPerBar = 0.0;
    /// Sixteenth-note phase profile across the requested bars. Each value is
    /// the fraction of bars with a MIDI onset in that slot (0 free, 1 busy).
    std::vector<double> onsetProfile16;
    int lowestPitch = -1;
    int highestPitch = -1;
    double averagePitch = -1.0;
    int maxPolyphony = 0;
    double averagePolyphony = 0.0;
};

struct MusicClipSummary {
    std::string id;
    std::string name;
    std::string kind;
    double startBar = 1.0;
    double endBar = 1.0;
    bool muted = false;
    std::vector<std::string> inserts;
    MusicActivitySummary activity;
};

struct MusicTrackSummary {
    std::string id;
    std::string name;
    std::string kind;
    bool muted = false;
    std::string instrument;
    std::vector<std::string> inserts;
    std::vector<MusicClipSummary> clips;
    MusicActivitySummary activity;
};

struct MusicChordSummary {
    double startBar = 1.0;
    double lengthBeats = 0.0;
    int root = -1;
    std::string rootName;
    std::string quality;
    std::vector<int> pitchClasses;
    std::vector<std::string> pitchClassNames;
    std::vector<int> chordTonePitchClasses;
    std::vector<std::string> chordToneNames;
    int bassPitch = -1;
    double confidence = 0.0;
};

/// Stored analysis for one audio clip; no audio is decoded while building the
/// context. Status is "unavailable", "ambiguous", or "available".
struct AudioClipAnalysisSummary {
    std::string trackId;
    std::string clipId;
    int algorithmVersion = 0;

    std::string tempoStatus;
    double bpm = 0.0;
    double tempoConfidence = 0.0;
    double tempoStability = 0.0;
    std::vector<double> tempoAlternatives;
    bool variableTempo = false;

    std::string keyStatus;
    int keyRoot = -1;
    std::string keyRootName;
    std::string keyScale;
    double keyConfidence = 0.0;
    int alternateKeyRoot = -1;
    std::string alternateKeyRootName;
    std::string alternateKeyScale;
    double tuningCents = 0.0;
};

/// Read-only musical facts for a half-open, one-based bar range
/// [fromBar, toBar). A non-positive/invalid toBar means "to project end".
struct ProjectMusicContext {
    double fromBar = 1.0;
    double toBar = 2.0;
    double beatsPerBar = 4.0;
    double tempo = 120.0;
    MusicKeySummary globalKey;
    MusicKeySummary detectedMidiKey;
    std::vector<std::string> masterInserts;
    std::vector<MusicChordSummary> chords;
    std::vector<MusicTrackSummary> tracks;
    std::vector<AudioClipAnalysisSummary> audioClips;

    /// Compact camelCase shape ready to return from an AI tool.
    nlohmann::json toJson() const;
};

ProjectMusicContext buildProjectMusicContext(
    const ProjectModel& project, double fromBar = 1.0, double toBar = 0.0,
    double harmonySegmentBeats = 0.0);

ProjectMusicContext buildProjectMusicContext(
    const EngineController& controller, double fromBar = 1.0,
    double toBar = 0.0, double harmonySegmentBeats = 0.0);

} // namespace daw::ai
