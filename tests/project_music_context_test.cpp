#include "ai/ProjectMusicContext.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

static int failures = 0;

static bool check(bool condition, const char* what) {
    std::printf("%s  %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++failures;
    return condition;
}

static bool near(double a, double b, double tolerance = 1e-6) {
    return std::abs(a - b) <= tolerance;
}

static daw::NoteModel note(int pitch, double start, double length,
                           bool muted = false) {
    daw::NoteModel out;
    out.pitch = pitch;
    out.startBeats = start;
    out.lengthBeats = length;
    out.velocity = 100;
    out.muted = muted;
    return out;
}

static daw::InsertModel namedInsert(const std::string& name) {
    daw::InsertModel out;
    out.name = name;
    return out;
}

int main() {
    daw::ProjectModel project;
    project.tempo = 120.0;
    project.timeSigNumerator = 4;
    project.timeSigDenominator = 4;
    project.keyRoot = 7;
    project.scale = "natural_minor";
    project.masterInserts.push_back(namedInsert("Limiter"));

    daw::TrackModel harmony;
    harmony.id = "harmony-track";
    harmony.name = "Harmony";
    harmony.kind = daw::TrackKind::Instrument;
    harmony.instrument = namedInsert("Grand Piano");
    harmony.inserts.push_back(namedInsert("Reverb"));

    daw::ClipModel midi;
    midi.id = "harmony-clip";
    midi.name = "Two Chords";
    midi.kind = daw::ClipKind::Midi;
    midi.startSeconds = 2.0;    // bar 2 at 120 BPM
    midi.durationSeconds = 4.0; // bars 2 and 3
    midi.inserts.push_back(namedInsert("Chorus"));
    midi.notes = {
        note(60, 0.0, 4.0), note(64, 0.0, 4.0), note(67, 0.0, 4.0),
        note(55, 4.0, 4.0), note(59, 4.0, 4.0), note(62, 4.0, 4.0),
        note(12, 0.0, 8.0, true),
    };
    harmony.clips.push_back(std::move(midi));
    project.tracks.push_back(std::move(harmony));

    daw::TrackModel audio;
    audio.id = "audio-track";
    audio.name = "Reference Beat";
    audio.kind = daw::TrackKind::Audio;
    daw::ClipModel beat;
    beat.id = "beat-clip";
    beat.name = "Beat";
    beat.kind = daw::ClipKind::Audio;
    beat.startSeconds = 2.0;
    beat.durationSeconds = 2.0;
    beat.musicalAnalysis.algorithmVersion = 3;
    beat.musicalAnalysis.tempo.status =
        daw::MusicalAnalysisStatus::Available;
    beat.musicalAnalysis.tempo.bpm = 126.0;
    beat.musicalAnalysis.tempo.confidence = 0.91;
    beat.musicalAnalysis.tempo.stability = 0.84;
    beat.musicalAnalysis.tempo.alternatives = {63.0, 252.0};
    beat.musicalAnalysis.key.status =
        daw::MusicalAnalysisStatus::Ambiguous;
    beat.musicalAnalysis.key.root = 9;
    beat.musicalAnalysis.key.scale = "natural_minor";
    beat.musicalAnalysis.key.confidence = 0.78;
    beat.musicalAnalysis.key.alternateRoot = 0;
    beat.musicalAnalysis.key.alternateScale = "major";
    audio.clips.push_back(std::move(beat));
    project.tracks.push_back(std::move(audio));

    const daw::ai::ProjectMusicContext context =
        daw::ai::buildProjectMusicContext(project, 2.0, 4.0);

    check(near(context.fromBar, 2.0) && near(context.toBar, 4.0) &&
              near(context.beatsPerBar, 4.0),
          "the requested half-open bar range is retained");
    check(context.globalKey.root == 7 &&
              context.globalKey.scale == "natural_minor" &&
              context.globalKey.source == "project" &&
              near(context.globalKey.confidence, 1.0),
          "the project key carries provenance and confidence");
    check(context.detectedMidiKey.available() &&
              context.detectedMidiKey.source == "midi_detected",
          "the notes also provide a separately sourced detected key");
    check(context.masterInserts.size() == 1 &&
              context.masterInserts.front() == "Limiter",
          "master insert names are exposed");

    check(context.chords.size() == 2, "one chord is reported for each bar");
    if (context.chords.size() == 2) {
        check(near(context.chords[0].startBar, 2.0) &&
                  context.chords[0].root == 0 &&
                  context.chords[0].quality == "major" &&
                  context.chords[0].chordTonePitchClasses ==
                      std::vector<int>({0, 4, 7}),
              "bar 2 contains C major and its chord tones");
        check(near(context.chords[1].startBar, 3.0) &&
                  context.chords[1].root == 7 &&
                  context.chords[1].quality == "major" &&
                  context.chords[1].pitchClasses ==
                      std::vector<int>({2, 7, 11}),
              "bar 3 contains G major and the sounding pitch classes");
    }

    check(context.tracks.size() == 2, "all project tracks are represented");
    if (context.tracks.size() == 2) {
        const daw::ai::MusicTrackSummary& track = context.tracks[0];
        check(track.instrument == "Grand Piano" && track.inserts.size() == 1 &&
                  track.inserts.front() == "Reverb" &&
                  track.clips.size() == 1 &&
                  track.clips.front().inserts.front() == "Chorus",
              "instrument, channel insert, and clip insert names survive");
        check(track.activity.noteCount == 6 &&
                  near(track.activity.activeBeats, 8.0) &&
                  near(track.activity.activityRatio, 1.0) &&
                  near(track.activity.noteDensityPerBar, 3.0) &&
                  track.activity.lowestPitch == 55 &&
                  track.activity.highestPitch == 67 &&
                  track.activity.maxPolyphony == 3 &&
                  near(track.activity.averagePolyphony, 3.0) &&
                  track.activity.onsetProfile16.size() == 16 &&
                  near(track.activity.onsetProfile16.front(), 1.0),
              "MIDI activity, density, register, and polyphony are summarized");

        const daw::ai::MusicActivitySummary& audioActivity =
            context.tracks[1].activity;
        check(near(audioActivity.activeBeats, 4.0) &&
                  near(audioActivity.activityRatio, 0.5) &&
                  audioActivity.maxPolyphony == 1,
              "an audio clip contributes timeline activity");
    }

    check(context.audioClips.size() == 1 &&
              context.audioClips.front().algorithmVersion == 3 &&
              context.audioClips.front().tempoStatus == "available" &&
              near(context.audioClips.front().bpm, 126.0) &&
              context.audioClips.front().keyStatus == "ambiguous" &&
              context.audioClips.front().keyRoot == 9 &&
              near(context.audioClips.front().keyConfidence, 0.78),
          "stored audio tempo/key analysis is copied without decoding audio");

    const nlohmann::json json = context.toJson();
    check(json["range"]["fromBar"] == 2.0 &&
              json["globalKey"]["source"] == "project" &&
              json["tracks"][0]["activity"]["polyphony"]["max"] == 3 &&
              json["audioClips"][0]["tempo"]["bpm"] == 126.0 &&
              json["chords"][0]["chordToneNames"] ==
                  nlohmann::json::array({"C", "E", "G"}),
          "the compact JSON form is ready for an AI tool result");

    const daw::ai::ProjectMusicContext openEnded =
        daw::ai::buildProjectMusicContext(project, 2.0, 0.0);
    check(near(openEnded.toBar, 4.0),
          "an omitted end bar resolves to the project end");

    std::printf("\n%s\n", failures == 0 ? "ALL PASSED" : "FAILURES PRESENT");
    return failures;
}
