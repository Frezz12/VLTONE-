#include "ai/ProjectMusicContext.hpp"

#include "EngineController.hpp"
#include "MidiTools.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <unordered_map>
#include <utility>

namespace daw::ai {
namespace {

using json = nlohmann::json;

struct Interval {
    double from = 0.0;
    double to = 0.0;
};

class ActivityBuilder {
public:
    void add(double from, double to) {
        if (to > from) m_intervals.push_back({from, to});
    }

    void addNote(double from, double to, int pitch) {
        if (!(to > from)) return;
        add(from, to);
        ++m_noteCount;
        const double length = to - from;
        m_pitchWeight += length;
        m_weightedPitch += double(pitch) * length;
        m_lowestPitch = m_lowestPitch < 0 ? pitch : std::min(m_lowestPitch, pitch);
        m_highestPitch = std::max(m_highestPitch, pitch);
        m_onsets.push_back(from);
    }

    MusicActivitySummary finish(double rangeFrom, double rangeBeats,
                                double beatsPerBar) const {
        MusicActivitySummary out;
        out.noteCount = m_noteCount;
        out.lowestPitch = m_lowestPitch;
        out.highestPitch = m_highestPitch;
        if (m_pitchWeight > 0.0)
            out.averagePitch = m_weightedPitch / m_pitchWeight;
        if (!m_onsets.empty() && rangeBeats > 0.0 && beatsPerBar > 0.0) {
            constexpr int slots = 16;
            out.onsetProfile16.assign(slots, 0.0);
            std::set<std::pair<int, int>> occupied;
            for (double onset : m_onsets) {
                const double relative = onset - rangeFrom;
                if (relative < 0.0 || relative >= rangeBeats) continue;
                const int bar = int(std::floor(relative / beatsPerBar));
                const double phase =
                    (relative - bar * beatsPerBar) / beatsPerBar;
                const int slot = std::clamp(int(std::floor(phase * slots)),
                                            0, slots - 1);
                occupied.emplace(bar, slot);
            }
            const double bars = std::max(1.0, rangeBeats / beatsPerBar);
            for (const auto& [bar, slot] : occupied) {
                (void)bar;
                out.onsetProfile16[std::size_t(slot)] += 1.0 / bars;
            }
            for (double& value : out.onsetProfile16)
                value = std::clamp(value, 0.0, 1.0);
        }
        if (m_intervals.empty()) return out;

        std::vector<Interval> ordered = m_intervals;
        std::sort(ordered.begin(), ordered.end(), [](const Interval& a,
                                                     const Interval& b) {
            return a.from < b.from || (a.from == b.from && a.to < b.to);
        });
        double mergedFrom = ordered.front().from;
        double mergedTo = ordered.front().to;
        double voiceBeats = 0.0;
        std::vector<std::pair<double, int>> events;
        events.reserve(ordered.size() * 2);
        for (const Interval& interval : ordered) {
            voiceBeats += interval.to - interval.from;
            events.emplace_back(interval.from, 1);
            events.emplace_back(interval.to, -1);
            if (interval.from > mergedTo) {
                out.activeBeats += mergedTo - mergedFrom;
                mergedFrom = interval.from;
                mergedTo = interval.to;
            } else {
                mergedTo = std::max(mergedTo, interval.to);
            }
        }
        out.activeBeats += mergedTo - mergedFrom;
        if (rangeBeats > 0.0)
            out.activityRatio = std::clamp(out.activeBeats / rangeBeats, 0.0, 1.0);
        if (out.activeBeats > 0.0 && beatsPerBar > 0.0) {
            out.noteDensityPerBar =
                double(out.noteCount) / (out.activeBeats / beatsPerBar);
            out.averagePolyphony = voiceBeats / out.activeBeats;
        }

        // End events precede starts at an equal position, so adjacent notes do
        // not briefly count as overlapping.
        std::sort(events.begin(), events.end(), [](const auto& a, const auto& b) {
            return a.first < b.first || (a.first == b.first && a.second < b.second);
        });
        int voices = 0;
        for (const auto& [at, delta] : events) {
            (void)at;
            voices += delta;
            out.maxPolyphony = std::max(out.maxPolyphony, voices);
        }
        return out;
    }

private:
    std::vector<Interval> m_intervals;
    std::size_t m_noteCount = 0;
    int m_lowestPitch = -1;
    int m_highestPitch = -1;
    double m_pitchWeight = 0.0;
    double m_weightedPitch = 0.0;
    std::vector<double> m_onsets;
};

double projectBeatsPerBar(const ProjectModel& project) {
    const int numerator = std::max(1, project.timeSigNumerator);
    const int denominator = std::max(1, project.timeSigDenominator);
    return double(numerator) * 4.0 / double(denominator);
}

double clipLengthBeats(const ProjectModel& project, const ClipModel& clip) {
    if (clip.durationSeconds > 0.0)
        return secondsToBeats(clip.durationSeconds, project.tempo);
    if (clip.kind != ClipKind::Midi) return 0.0;
    double end = 0.0;
    for (const NoteModel& note : clip.notes)
        end = std::max(end, note.startBeats + std::max(0.0, note.lengthBeats));
    return end;
}

double projectEndBeats(const ProjectModel& project) {
    double end = 0.0;
    for (const TrackModel& track : project.tracks) {
        for (const ClipModel& clip : track.clips) {
            const double start = secondsToBeats(clip.startSeconds, project.tempo);
            end = std::max(end, start + clipLengthBeats(project, clip));
        }
    }
    return end;
}

std::string slotName(const InsertModel& slot) {
    return !slot.name.empty() ? slot.name : slot.uid;
}

std::vector<std::string> insertNames(const std::vector<InsertModel>& inserts) {
    std::vector<std::string> names;
    names.reserve(inserts.size());
    for (const InsertModel& insert : inserts) {
        const std::string name = slotName(insert);
        if (!name.empty()) names.push_back(name);
    }
    return names;
}

std::string analysisStatus(MusicalAnalysisStatus status) {
    switch (status) {
    case MusicalAnalysisStatus::Ambiguous: return "ambiguous";
    case MusicalAnalysisStatus::Available: return "available";
    case MusicalAnalysisStatus::Unavailable: return "unavailable";
    }
    return "unavailable";
}

int pitchClass(int pitch) { return ((pitch % 12) + 12) % 12; }

std::vector<int> chordTones(int root, const std::string& quality,
                            const std::vector<int>& sounding) {
    if (root < 0) return {};
    const std::vector<int>* intervals = nullptr;
    static const std::vector<int> major{0, 4, 7};
    static const std::vector<int> minor{0, 3, 7};
    static const std::vector<int> diminished{0, 3, 6};
    static const std::vector<int> augmented{0, 4, 8};
    static const std::vector<int> sus2{0, 2, 7};
    static const std::vector<int> sus4{0, 5, 7};
    static const std::vector<int> power{0, 7};
    static const std::vector<int> major7{0, 4, 7, 11};
    static const std::vector<int> minor7{0, 3, 7, 10};
    static const std::vector<int> dominant7{0, 4, 7, 10};
    static const std::vector<int> minor7b5{0, 3, 6, 10};
    static const std::vector<int> single{0};
    if (quality == "major") intervals = &major;
    else if (quality == "minor") intervals = &minor;
    else if (quality == "diminished") intervals = &diminished;
    else if (quality == "augmented") intervals = &augmented;
    else if (quality == "sus2") intervals = &sus2;
    else if (quality == "sus4") intervals = &sus4;
    else if (quality == "power") intervals = &power;
    else if (quality == "major7") intervals = &major7;
    else if (quality == "minor7") intervals = &minor7;
    else if (quality == "dominant7") intervals = &dominant7;
    else if (quality == "minor7b5") intervals = &minor7b5;
    else if (quality == "single") intervals = &single;
    if (!intervals) return sounding;

    std::vector<int> out;
    out.reserve(intervals->size());
    for (int interval : *intervals) out.push_back(pitchClass(root + interval));
    return out;
}

std::vector<std::string> pitchClassNames(const std::vector<int>& classes) {
    std::vector<std::string> names;
    names.reserve(classes.size());
    for (int pc : classes) names.push_back(miditools::pitchClassName(pc));
    return names;
}

AudioClipAnalysisSummary audioAnalysis(const TrackModel& track,
                                       const ClipModel& clip) {
    const ClipMusicalAnalysisModel& stored = clip.musicalAnalysis;
    AudioClipAnalysisSummary out;
    out.trackId = track.id;
    out.clipId = clip.id;
    out.algorithmVersion = stored.algorithmVersion;
    out.tempoStatus = analysisStatus(stored.tempo.status);
    out.bpm = stored.tempo.bpm;
    out.tempoConfidence = stored.tempo.confidence;
    out.tempoStability = stored.tempo.stability;
    out.tempoAlternatives = stored.tempo.alternatives;
    out.variableTempo = stored.tempo.variable;
    out.keyStatus = analysisStatus(stored.key.status);
    out.keyRoot = stored.key.root;
    if (out.keyRoot >= 0)
        out.keyRootName = miditools::pitchClassName(out.keyRoot);
    out.keyScale = stored.key.scale;
    out.keyConfidence = stored.key.confidence;
    out.alternateKeyRoot = stored.key.alternateRoot;
    if (out.alternateKeyRoot >= 0)
        out.alternateKeyRootName =
            miditools::pitchClassName(out.alternateKeyRoot);
    out.alternateKeyScale = stored.key.alternateScale;
    out.tuningCents = stored.key.tuningCents;
    return out;
}

json keyJson(const MusicKeySummary& key) {
    return json{{"root", key.root},
                {"rootName", key.rootName},
                {"scale", key.scale},
                {"source", key.source},
                {"confidence", key.confidence}};
}

json activityJson(const MusicActivitySummary& activity) {
    json registerJson = nullptr;
    if (activity.lowestPitch >= 0) {
        registerJson = json{{"lowestPitch", activity.lowestPitch},
                            {"lowestNote",
                             miditools::pitchName(activity.lowestPitch)},
                            {"highestPitch", activity.highestPitch},
                            {"highestNote",
                             miditools::pitchName(activity.highestPitch)},
                            {"averagePitch", activity.averagePitch}};
    }
    return json{{"noteCount", activity.noteCount},
                {"activeBeats", activity.activeBeats},
                {"activityRatio", activity.activityRatio},
                {"noteDensityPerBar", activity.noteDensityPerBar},
                {"onsetProfile16", activity.onsetProfile16},
                {"register", std::move(registerJson)},
                {"polyphony",
                 {{"max", activity.maxPolyphony},
                  {"average", activity.averagePolyphony}}}};
}

} // namespace

ProjectMusicContext buildProjectMusicContext(const ProjectModel& project,
                                             double fromBar, double toBar,
                                             double harmonySegmentBeats) {
    ProjectMusicContext out;
    out.tempo = project.tempo > 0.0 && std::isfinite(project.tempo)
                    ? project.tempo
                    : 1.0;
    out.beatsPerBar = projectBeatsPerBar(project);
    out.fromBar = std::isfinite(fromBar) ? std::max(1.0, fromBar) : 1.0;
    if (!std::isfinite(toBar) || toBar <= out.fromBar) {
        const double endBar = projectEndBeats(project) / out.beatsPerBar + 1.0;
        out.toBar = std::max(out.fromBar + 1.0, endBar);
    } else {
        out.toBar = toBar;
    }
    const double rangeFrom = (out.fromBar - 1.0) * out.beatsPerBar;
    const double rangeTo = (out.toBar - 1.0) * out.beatsPerBar;
    const double rangeBeats = rangeTo - rangeFrom;

    if (project.keyRoot >= 0 && !project.scale.empty()) {
        out.globalKey.root = pitchClass(project.keyRoot);
        out.globalKey.rootName =
            miditools::pitchClassName(out.globalKey.root);
        out.globalKey.scale = project.scale;
        out.globalKey.source = "project";
        out.globalKey.confidence = 1.0;
    }
    out.masterInserts = insertNames(project.masterInserts);

    struct PatternGate {
        double from = 0.0;
        double to = std::numeric_limits<double>::max();
        bool muted = false;
    };
    std::unordered_map<std::string, PatternGate> patternGates;
    for (const TrackModel& track : project.tracks) {
        if (track.kind != TrackKind::Pattern) continue;
        for (const ClipModel& clip : track.clips) {
            if (clip.kind != ClipKind::Pattern) continue;
            const double from = secondsToBeats(clip.startSeconds, project.tempo);
            patternGates.try_emplace(
                clip.id,
                PatternGate{from, from + clipLengthBeats(project, clip), clip.muted});
        }
    }

    std::vector<NoteModel> gatheredNotes;
    out.tracks.reserve(project.tracks.size());
    for (const TrackModel& track : project.tracks) {
        MusicTrackSummary trackOut;
        trackOut.id = track.id;
        trackOut.name = track.name;
        trackOut.kind = toString(track.kind);
        trackOut.muted = track.muted;
        trackOut.instrument = slotName(track.instrument);
        if (track.samplerFx.isOwnedBy(track.instrument))
            trackOut.inserts = insertNames(track.samplerFx.inserts);
        const std::vector<std::string> channelInserts = insertNames(track.inserts);
        trackOut.inserts.insert(trackOut.inserts.end(), channelInserts.begin(),
                                channelInserts.end());

        ActivityBuilder trackActivity;
        for (const ClipModel& clip : track.clips) {
            const double clipFrom =
                secondsToBeats(clip.startSeconds, project.tempo);
            const double length = clipLengthBeats(project, clip);
            const double clipTo = clipFrom + length;
            if (!(clipTo > rangeFrom && clipFrom < rangeTo)) continue;

            MusicClipSummary clipOut;
            clipOut.id = clip.id;
            clipOut.name = clip.name;
            clipOut.kind = toString(clip.kind);
            clipOut.startBar = clipFrom / out.beatsPerBar + 1.0;
            clipOut.endBar = clipTo / out.beatsPerBar + 1.0;
            clipOut.muted = clip.muted;
            clipOut.inserts = insertNames(clip.inserts);
            ActivityBuilder clipActivity;

            if (clip.kind == ClipKind::Audio) {
                out.audioClips.push_back(audioAnalysis(track, clip));
                if (!clip.muted) {
                    const double audibleFrom = std::max(clipFrom, rangeFrom);
                    const double audibleTo = std::min(clipTo, rangeTo);
                    clipActivity.add(audibleFrom, audibleTo);
                    trackActivity.add(audibleFrom, audibleTo);
                }
            } else if (clip.kind == ClipKind::Midi && !clip.muted) {
                double gateFrom = clipFrom;
                double gateTo = length > 0.0
                                    ? clipTo
                                    : std::numeric_limits<double>::max();
                if (!clip.patternClipId.empty()) {
                    const auto gate = patternGates.find(clip.patternClipId);
                    if (gate != patternGates.end()) {
                        if (gate->second.muted) {
                            gateTo = gateFrom;
                        } else {
                            gateFrom = std::max(gateFrom, gate->second.from);
                            gateTo = std::min(gateTo, gate->second.to);
                        }
                    }
                }
                for (const NoteModel& note : clip.notes) {
                    if (note.muted || !(note.lengthBeats > 0.0)) continue;
                    if (length > 0.0 && note.startBeats >= length) continue;
                    const double sourceLength =
                        length > 0.0
                            ? std::min(note.lengthBeats, length - note.startBeats)
                            : note.lengthBeats;
                    const double noteFrom = clipFrom + note.startBeats;
                    const double noteTo = noteFrom + sourceLength;
                    const double audibleFrom =
                        std::max({noteFrom, gateFrom, rangeFrom});
                    const double audibleTo = std::min({noteTo, gateTo, rangeTo});
                    if (!(audibleTo > audibleFrom)) continue;

                    clipActivity.addNote(audibleFrom, audibleTo, note.pitch);
                    trackActivity.addNote(audibleFrom, audibleTo, note.pitch);
                    NoteModel moved = note;
                    moved.startBeats = audibleFrom;
                    moved.lengthBeats = audibleTo - audibleFrom;
                    gatheredNotes.push_back(std::move(moved));
                }
            }
            clipOut.activity =
                clipActivity.finish(rangeFrom, rangeBeats, out.beatsPerBar);
            trackOut.clips.push_back(std::move(clipOut));
        }
        trackOut.activity =
            trackActivity.finish(rangeFrom, rangeBeats, out.beatsPerBar);
        out.tracks.push_back(std::move(trackOut));
    }

    if (!gatheredNotes.empty()) {
        const miditools::KeyGuess key = miditools::estimateKey(gatheredNotes);
        if (key.root >= 0) {
            out.detectedMidiKey.root = key.root;
            out.detectedMidiKey.rootName = miditools::pitchClassName(key.root);
            out.detectedMidiKey.scale = miditools::scaleId(key.scale);
            out.detectedMidiKey.source = "midi_detected";
            out.detectedMidiKey.confidence = key.confidence;
        }

        const std::vector<miditools::HarmonySegment> harmony =
            miditools::analyzeHarmony(gatheredNotes, out.beatsPerBar,
                                      harmonySegmentBeats);
        out.chords.reserve(harmony.size());
        for (const miditools::HarmonySegment& segment : harmony) {
            const double from = std::max(segment.startBeats, rangeFrom);
            const double to =
                std::min(segment.startBeats + segment.lengthBeats, rangeTo);
            if (!(to > from)) continue;
            MusicChordSummary chord;
            chord.startBar = from / out.beatsPerBar + 1.0;
            chord.lengthBeats = to - from;
            chord.root = segment.root;
            if (chord.root >= 0)
                chord.rootName = miditools::pitchClassName(chord.root);
            chord.quality = segment.quality;
            chord.pitchClasses = segment.pitchClasses;
            chord.pitchClassNames = pitchClassNames(chord.pitchClasses);
            chord.chordTonePitchClasses =
                chordTones(chord.root, chord.quality, chord.pitchClasses);
            chord.chordToneNames = pitchClassNames(chord.chordTonePitchClasses);
            chord.bassPitch = segment.bassPitch;
            chord.confidence = segment.confidence;
            out.chords.push_back(std::move(chord));
        }
    }
    return out;
}

ProjectMusicContext buildProjectMusicContext(const EngineController& controller,
                                             double fromBar, double toBar,
                                             double harmonySegmentBeats) {
    return buildProjectMusicContext(controller.project(), fromBar, toBar,
                                    harmonySegmentBeats);
}

nlohmann::json ProjectMusicContext::toJson() const {
    json chordJson = json::array();
    for (const MusicChordSummary& chord : chords) {
        json item{{"startBar", chord.startBar},
                  {"lengthBeats", chord.lengthBeats},
                  {"root", chord.root},
                  {"rootName", chord.rootName},
                  {"quality", chord.quality},
                  {"pitchClasses", chord.pitchClasses},
                  {"pitchClassNames", chord.pitchClassNames},
                  {"chordTonePitchClasses", chord.chordTonePitchClasses},
                  {"chordToneNames", chord.chordToneNames},
                  {"confidence", chord.confidence}};
        if (chord.bassPitch >= 0) {
            item["bassPitch"] = chord.bassPitch;
            item["bassNote"] = miditools::pitchName(chord.bassPitch);
        }
        chordJson.push_back(std::move(item));
    }

    json trackJson = json::array();
    for (const MusicTrackSummary& track : tracks) {
        json clipJson = json::array();
        for (const MusicClipSummary& clip : track.clips) {
            clipJson.push_back(json{{"id", clip.id},
                                    {"name", clip.name},
                                    {"kind", clip.kind},
                                    {"startBar", clip.startBar},
                                    {"endBar", clip.endBar},
                                    {"muted", clip.muted},
                                    {"inserts", clip.inserts},
                                    {"activity", activityJson(clip.activity)}});
        }
        trackJson.push_back(json{{"id", track.id},
                                 {"name", track.name},
                                 {"kind", track.kind},
                                 {"muted", track.muted},
                                 {"instrument", track.instrument},
                                 {"inserts", track.inserts},
                                 {"activity", activityJson(track.activity)},
                                 {"clips", std::move(clipJson)}});
    }

    json audioJson = json::array();
    for (const AudioClipAnalysisSummary& clip : audioClips) {
        json key{{"status", clip.keyStatus},
                 {"root", clip.keyRoot},
                 {"rootName", clip.keyRootName},
                 {"scale", clip.keyScale},
                 {"confidence", clip.keyConfidence},
                 {"alternateRoot", clip.alternateKeyRoot},
                 {"alternateRootName", clip.alternateKeyRootName},
                 {"alternateScale", clip.alternateKeyScale},
                 {"tuningCents", clip.tuningCents}};
        audioJson.push_back(
            json{{"trackId", clip.trackId},
                 {"clipId", clip.clipId},
                 {"algorithmVersion", clip.algorithmVersion},
                 {"tempo",
                  {{"status", clip.tempoStatus},
                   {"bpm", clip.bpm},
                   {"confidence", clip.tempoConfidence},
                   {"stability", clip.tempoStability},
                   {"alternatives", clip.tempoAlternatives},
                   {"variable", clip.variableTempo}}},
                 {"key", std::move(key)}});
    }

    return json{{"range",
                 {{"fromBar", fromBar},
                  {"toBar", toBar},
                  {"beatsPerBar", beatsPerBar}}},
                {"tempo", tempo},
                {"globalKey", keyJson(globalKey)},
                {"detectedMidiKey",
                 detectedMidiKey.available() ? keyJson(detectedMidiKey)
                                             : json(nullptr)},
                {"masterInserts", masterInserts},
                {"chords", std::move(chordJson)},
                {"tracks", std::move(trackJson)},
                {"audioClips", std::move(audioJson)}};
}

} // namespace daw::ai
