// Crash recovery, layer 1: the document-only serializer that the journal is
// built on.
//
// The point of `saveDocument` is what it does NOT do — `save` copies every
// referenced audio file into the package, which is far too expensive to run
// every few seconds. These checks pin down both halves of that split: the
// journal writes references and copies nothing, and the packaged format comes
// out byte for byte the way it always did.
#include "EngineController.hpp"
#include "ProjectSerializer.hpp"
#include "recovery/CloudRecordingRecovery.hpp"
#include "recovery/RecoveryJournal.hpp"
#include "plugins/ScanProcess.hpp"
#include "crash/CrashHandler.hpp"
#include "Recording/RecordingEngine.hpp"
#include "Core/AudioBuffer.hpp"
#include "platform/PathUtils.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <utility>
#include <nlohmann/json.hpp>
#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace audio;

static int failures = 0;
static bool check(bool cond, const char* what) {
    std::printf("%s  %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) ++failures;
    return cond;
}

static void writeTone(const std::string& path, SampleRate rate,
                      BufferSize frames) {
    AudioBuffer tone(2, frames);
    for (BufferSize f = 0; f < frames; ++f) {
        const float s = 0.5f * std::sin(2.0f * 3.14159265f * 440.0f * f / rate);
        tone.getChannel(0)[f] = s;
        tone.getChannel(1)[f] = s;
    }
    AudioRecorder rec;
    rec.initialize(rate, 2);
    rec.writeWAVFile(path, tone, rate);
}

static std::string readFile(const fs::path& path) {
    std::ifstream is(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << is.rdbuf();
    return buffer.str();
}

#if !defined(_WIN32)
// Wait for a spawned child to exit, and reap it. isProcessAlive() cannot answer
// this: a child that has exited but not been waited on is a zombie, and
// kill(pid, 0) succeeds on a zombie — it would report the guard as still
// running forever.
static bool reapedWithin(std::int64_t pid, int millis) {
    for (int waited = 0; waited < millis; waited += 50) {
        int status = 0;
        const pid_t done = ::waitpid(static_cast<pid_t>(pid), &status, WNOHANG);
        if (done == static_cast<pid_t>(pid) || done < 0) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}
#endif

static std::size_t countFiles(const fs::path& dir) {
    std::size_t n = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec);
         it != fs::recursive_directory_iterator(); ++it) {
        if (it->is_regular_file()) ++n;
    }
    return n;
}

int main() {
    const fs::path dir = fs::temp_directory_path() / "daw-recovery-test";
    fs::remove_all(dir);
    fs::create_directories(dir);

    // Source audio lives OUTSIDE any package, which is the situation the
    // journal has to handle: the user imported a sample from their library and
    // has not saved the project yet.
    const fs::path sourceDir = dir / "library";
    fs::create_directories(sourceDir);
    const std::string tonePath = (sourceDir / "tone.wav").string();
    writeTone(tonePath, 48000, 24000);

    // ── A project with something of every kind in it ──
    daw::EngineController ctrl;
    check(ctrl.initialize(48000, 512, /*openDevice=*/false).isOk(),
          "controller initialises offline");

    const std::string audioTrack = ctrl.addTrack(daw::TrackKind::Audio, "Guitar");
    const std::string midiTrack = ctrl.addTrack(daw::TrackKind::Midi, "Piano");
    const std::string clip = ctrl.importAudio(tonePath, audioTrack, 1.0);
    check(!clip.empty(), "imports an audio clip from outside any package");

    const std::string midiClip = ctrl.addMidiClip(midiTrack, 0.0, 4.0);
    std::vector<daw::NoteModel> notes;
    for (int i = 0; i < 4; ++i) {
        daw::NoteModel n;
        n.pitch = 60 + i * 4;
        n.startBeats = i;
        n.lengthBeats = 1.0;
        n.velocity = 90 + i;
        notes.push_back(n);
    }
    ctrl.setClipNotes(midiTrack, midiClip, notes, "notes");
    ctrl.setTempo(132.0);
    ctrl.setProjectKey(9, "minor");
    ctrl.setAiInstructions("keep the low end tight");
    ctrl.setTrackVolume(midiTrack, 0.6f);
    ctrl.setTrackPan(midiTrack, -0.4f);

    const daw::ProjectModel& model = ctrl.project();

    // ── saveDocument(Absolute): the journal ──
    const fs::path journal = dir / "recovery" / "project.json";
    check(daw::ProjectSerializer::saveDocument(
              model, journal.string(), daw::MediaPaths::Absolute).isOk(),
          "saveDocument writes a journal");
    check(fs::exists(journal), "journal file exists");
    check(fs::exists(journal.parent_path()),
          "saveDocument created the directory it was given");

    // The whole reason this function exists.
    check(!fs::exists(journal.parent_path() / "media"),
          "journal creates no media directory");
    check(countFiles(journal.parent_path()) == 1,
          "journal is exactly one file — nothing was copied");
    check(countFiles(sourceDir) == 1, "the source library is left untouched");

    const std::string journalText = readFile(journal);
    std::string serializedTonePath = tonePath;
    for (std::size_t i = 0; i < serializedTonePath.size(); ++i) {
        if (serializedTonePath[i] == '\\') {
            serializedTonePath.insert(i, 1, '\\');
            ++i;
        }
    }
    check(journalText.find(serializedTonePath) != std::string::npos,
          "journal references the original file by absolute path");

    // ── loadDocument reads it back whole ──
    daw::ProjectModel restored;
    check(daw::ProjectSerializer::loadDocument(
              restored, journal.string(), /*mediaDir=*/"").isOk(),
          "loadDocument reads the journal");
    check(restored.tracks.size() == 2, "both tracks come back");
    check(std::fabs(restored.tempo - 132.0) < 1e-9, "tempo survives");
    check(restored.keyRoot == 9 && restored.scale == "minor", "key survives");
    check(restored.aiInstructions == "keep the low end tight",
          "AI instructions survive");
    if (restored.tracks.size() == 2) {
        const auto& guitar = restored.tracks[0];
        const auto& piano = restored.tracks[1];
        check(guitar.clips.size() == 1 && guitar.clips[0].filePath == tonePath,
              "the audio clip still points at the user's own file");
        // Imported at 1 s under the default 120 BPM, i.e. on beat 2. The
        // tempo change to 132 moved it in seconds precisely so that it did not
        // move in bars, and that is the position the journal holds.
        check(std::fabs(guitar.clips[0].startSeconds - 120.0 / 132.0) < 1e-9,
              "clip position survives");
        check(piano.clips.size() == 1 && piano.clips[0].notes.size() == 4,
              "the MIDI notes survive");
        if (piano.clips.size() == 1 && piano.clips[0].notes.size() == 4) {
            const auto& n = piano.clips[0].notes[3];
            check(n.pitch == 72 && std::fabs(n.startBeats - 3.0) < 1e-9 &&
                      n.velocity == 93,
                  "note pitch, position and velocity survive");
        }
        check(std::fabs(piano.volume - 0.6f) < 1e-6f &&
                  std::fabs(piano.pan + 0.4f) < 1e-6f,
              "the mix survives");
    }

    // A non-empty mediaDir must not disturb absolute paths — this is what lets
    // one reader serve both media modes, so it is worth pinning down.
    daw::ProjectModel viaMediaDir;
    daw::ProjectSerializer::loadDocument(viaMediaDir, journal.string(),
                                         (dir / "somewhere" / "media").string());
    check(viaMediaDir.tracks.size() == 2 &&
              viaMediaDir.tracks[0].clips.size() == 1 &&
              viaMediaDir.tracks[0].clips[0].filePath == tonePath,
          "absolute paths ignore mediaDir");

    // ── Recovery serialization matches the packaged VLT document ──
    const fs::path pkg = dir / "song.vlt";
    check(ctrl.saveProject(pkg.string()).isOk(), "saves a real package");
    check(fs::exists(pkg / "Content" / "tone.wav"),
          "package still copies media, as it always did");

    const fs::path basenameDoc = dir / "basenames.json";
    // saveProject rewrote nothing in the live model, so serializing it with
    // Basenames must reproduce the package's own Project.vlt manifest.
    daw::ProjectModel packaged;
    daw::ProjectSerializer::load(packaged, pkg.string());
    // Reading the package resolves paths into Content/; writing those back as
    // basenames yields the same names the package holds.
    daw::ProjectSerializer::saveDocument(packaged, basenameDoc.string(),
                                         daw::MediaPaths::Basenames);
    check(readFile(basenameDoc) == readFile(pkg / "Project.vlt"),
          "Basenames output is byte-identical to the package's Project.vlt");

    // The application contract is UTF-8 std::string at its public boundary and
    // native filesystem paths underneath. Exercise both directions here: the
    // audio source, project package and manifest all contain Cyrillic and
    // spaces. On Windows this specifically catches accidental ACP conversions.
    {
        const fs::path unicodeLibrary =
            dir / fs::path(u8"Тест аудио") / fs::path(u8"Библиотека");
        const fs::path unicodeTone = unicodeLibrary / fs::path(u8"звук.wav");
        const fs::path unicodePackage =
            dir / fs::path(u8"Тест сборки") /
            fs::path(u8"Проект с пробелами.vlt");
        fs::create_directories(unicodeLibrary);
        const std::string unicodeToneUtf8 =
            daw::platform::pathToUtf8(unicodeTone);
        const std::string unicodePackageUtf8 =
            daw::platform::pathToUtf8(unicodePackage);
        writeTone(unicodeToneUtf8, 48000, 4800);

        daw::EngineController unicodeController;
        unicodeController.initialize(48000, 512, /*openDevice=*/false);
        const std::string unicodeTrack =
            unicodeController.addTrack(daw::TrackKind::Audio, "Unicode");
        check(!unicodeController
                   .importAudio(unicodeToneUtf8, unicodeTrack, 0.0)
                   .empty(),
              "imports audio through a Unicode path");
        check(unicodeController.saveProject(unicodePackageUtf8).isOk(),
              "saves a project through a Unicode path");
        daw::ProjectModel unicodeReloaded;
        check(daw::ProjectSerializer::load(unicodeReloaded, unicodePackageUtf8)
                  .isOk() &&
                  unicodeReloaded.tracks.size() == 1 &&
                  unicodeReloaded.tracks.front().clips.size() == 1,
              "reloads a project through a Unicode path");
        check(fs::is_regular_file(unicodePackage / "Project.vlt") &&
                  fs::is_regular_file(unicodePackage / "Content" /
                                      unicodeTone.filename()),
              "Unicode package contains its manifest and media");
    }

    // ── Atomic write leaves no debris ──
    bool debris = false;
    for (const auto& entry : fs::directory_iterator(journal.parent_path())) {
        if (entry.path().filename().string().find(".tmp-") != std::string::npos)
            debris = true;
    }
    check(!debris, "no temporary file is left behind");

    // Overwriting in place keeps the previous file readable at every instant;
    // the observable part of that is simply that the new content replaced it.
    ctrl.setTempo(90.0);
    daw::ProjectSerializer::saveDocument(ctrl.project(), journal.string(),
                                         daw::MediaPaths::Absolute);
    daw::ProjectModel rewritten;
    daw::ProjectSerializer::loadDocument(rewritten, journal.string(), "");
    check(std::fabs(rewritten.tempo - 90.0) < 1e-9,
          "rewriting the journal replaces it");

    // ── Failure modes report rather than throw ──
    daw::ProjectModel ignored;
    check(!daw::ProjectSerializer::loadDocument(
              ignored, (dir / "nope.json").string(), "").isOk(),
          "a missing journal fails cleanly");

    const fs::path garbage = dir / "garbage.json";
    { std::ofstream(garbage) << "{ this is not json"; }
    check(!daw::ProjectSerializer::loadDocument(ignored, garbage.string(), "").isOk(),
          "malformed JSON fails cleanly");

    const fs::path foreign = dir / "foreign.json";
    { std::ofstream(foreign) << R"({"format":"something-else"})"; }
    check(!daw::ProjectSerializer::loadDocument(ignored, foreign.string(), "").isOk(),
          "a foreign file is rejected");

    // ── The journal: a live session writing itself to disk ──
    const fs::path root = dir / "recovery-root";
    {
        daw::recovery::RecoveryJournal journal;
        check(journal.start(root.string(), "test-1.0",
                            std::chrono::milliseconds(200)),
              "journal opens a session");
        check(journal.running(), "journal reports itself running");
        const fs::path session(journal.sessionDir());
        check(fs::exists(session / "session.json"),
              "session.json is written at once, before any edit");

        daw::recovery::SessionInfo info;
        check(daw::recovery::readSession(session.string(), info),
              "session.json parses");
        check(info.pid == daw::recovery::currentProcessId(),
              "the session names this process");
        check(info.appVersion == "test-1.0", "the session carries the version");

        journal.setProjectPath((dir / "song.vlt").string(), "song");
        journal.requestWrite(ctrl.project());
        journal.flush();
        check(journal.writeCount() == 1, "flush forces the pending write out");
        check(fs::exists(session / "project.json"), "the journal file appears");

        daw::ProjectModel fromJournal;
        check(daw::ProjectSerializer::loadDocument(
                  fromJournal, (session / "project.json").string(), "").isOk() &&
                  fromJournal.tracks.size() == 2,
              "the journal holds a loadable document");

        // The debounce is the whole reason this is affordable: a burst of edits
        // must not become a burst of writes.
        const std::uint64_t before = journal.writeCount();
        for (int i = 0; i < 200; ++i) {
            ctrl.setTrackPan(midiTrack, -0.4f + 0.001f * i);
            journal.requestWrite(ctrl.project());
        }
        journal.flush();
        const std::uint64_t burst = journal.writeCount() - before;
        check(burst >= 1 && burst < 20,
              "200 edits coalesce into a handful of writes");

        // …and the last state still wins, which is what makes coalescing safe.
        daw::ProjectModel latest;
        daw::ProjectSerializer::loadDocument(
            latest, (session / "project.json").string(), "");
        check(latest.tracks.size() == 2 &&
                  std::fabs(latest.tracks[1].pan - (-0.4f + 0.001f * 199)) < 1e-5f,
              "the newest edit is what landed on disk");

        // The heartbeat the watchdog reads must actually advance.
        daw::recovery::SessionInfo first;
        daw::recovery::readSession(session.string(), first);
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        daw::recovery::SessionInfo second;
        daw::recovery::readSession(session.string(), second);
        check(second.heartbeat > first.heartbeat, "the heartbeat advances");
        check(second.projectPath == (dir / "song.vlt").string(),
              "the session records where the project lives");

        journal.stop();
        check(!fs::exists(session), "a clean stop deletes the session directory");
    }

    // ── Finalized cloud recordings survive until their durable commit ──
    //
    // This sidecar is intentionally separate from the document journal. A WAV
    // may have closed successfully while its upload/recording.commit was still
    // in flight, so recovering only the last ProjectModel would lose the take.
    {
        const fs::path session = root / "cloud-recording-session";
        fs::create_directories(session);
        const fs::path firstWav = sourceDir / "first-cloud-take.wav";
        const fs::path secondWav = sourceDir / "second-take.wav";
        writeTone(daw::platform::pathToUtf8(firstWav), 48000, 24000);
        writeTone(daw::platform::pathToUtf8(secondWav), 44100, 44100);

        daw::recovery::CloudRecordingRecoveryManifest manifest;
        manifest.projectId = "11111111-1111-4111-8111-111111111111";
        manifest.sessionId = "22222222-2222-4222-8222-222222222222";
        manifest.createdAtUnixMs = daw::recovery::nowUnixMs();

        daw::recovery::CloudRecordingRecoveryRun firstRun;
        firstRun.runId = "88888888-8888-4888-8888-888888888888";
        firstRun.opId = "99999999-9999-4999-8999-999999999999";
        firstRun.transactionId = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
        firstRun.createdAtUnixMs = manifest.createdAtUnixMs;
        firstRun.recoveryOnly = true;
        firstRun.lostLease = true;

        daw::recovery::CloudRecordingCapture firstCapture;
        firstCapture.captureId = "55555555-5555-4555-8555-555555555555";
        firstCapture.trackId = "33333333-3333-4333-8333-333333333333";
        firstCapture.leaseId = "44444444-4444-4444-8444-444444444444";
        firstCapture.uploadId = "66666666-6666-4666-8666-666666666666";
        firstCapture.assetId = "77777777-7777-4777-8777-777777777777";
        firstCapture.localWavPath =
            daw::platform::pathToUtf8(firstWav);
        firstCapture.startSeconds = 12.0;
        firstCapture.durationSeconds = 0.5;
        firstCapture.sampleRate = 48000.0;
        firstCapture.channels = 2;
        firstCapture.frames = 24000;
        firstCapture.passes.push_back({12.0, 12.5, 0.0});
        firstCapture.semantics.mode =
            daw::recovery::CloudRecordingMode::Overwrite;
        firstRun.captures.push_back(firstCapture);
        manifest.runs.push_back(firstRun);

        daw::recovery::CloudRecordingRecoveryRun secondRun;
        secondRun.runId = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
        secondRun.opId = "cccccccc-cccc-4ccc-8ccc-cccccccccccc";
        secondRun.transactionId = "dddddddd-dddd-4ddd-8ddd-dddddddddddd";
        secondRun.createdAtUnixMs = manifest.createdAtUnixMs + 1;
        secondRun.recoveryOnly = true;
        secondRun.lostLease = true;

        daw::recovery::CloudRecordingCapture secondCapture;
        // A second stop on the same track is appended to the sidecar rather
        // than replacing the first recovery-only take.
        secondCapture.trackId = firstCapture.trackId;
        secondCapture.captureId = "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee";
        secondCapture.leaseId = "ffffffff-ffff-4fff-8fff-ffffffffffff";
        secondCapture.uploadId = "12121212-1212-4212-8212-121212121212";
        secondCapture.assetId = "13131313-1313-4313-8313-131313131313";
        secondCapture.localWavPath =
            daw::platform::pathToUtf8(secondWav);
        secondCapture.startSeconds = 8.0;
        secondCapture.durationSeconds = 1.0;
        secondCapture.sampleRate = 44100.0;
        secondCapture.channels = 2;
        secondCapture.frames = 44100;
        // File offsets are monotonic even though a transport loop rewound the
        // second pass to an earlier point on the timeline.
        secondCapture.passes.push_back({8.0, 8.25, 0.0});
        secondCapture.passes.push_back({4.0, 4.75, 0.25});
        secondCapture.semantics.mode =
            daw::recovery::CloudRecordingMode::Layers;
        secondCapture.semantics.loopEnabled = true;
        secondCapture.semantics.loopStartSeconds = 4.0;
        secondCapture.semantics.loopEndSeconds = 8.0;
        secondCapture.semantics.loopCreatesTakes = true;
        secondCapture.semantics.trimTakesToRegion = false;
        secondCapture.semantics.autoExpandAfterRecord = true;
        secondCapture.semantics.compCrossfadeMs = 7.0;
        secondRun.captures.push_back(secondCapture);
        manifest.runs.push_back(secondRun);

        const fs::path recoveryFile =
            session / daw::recovery::kCloudRecordingRecoveryFile;
        {
            daw::recovery::CloudRecordingRecoveryStore store(
                daw::platform::pathToUtf8(session));
            check(store.write(manifest).ok(),
                  "cloud recording recovery publishes an atomic manifest");
            check(store.exists() &&
                      fs::path(daw::platform::pathFromUtf8(store.manifestPath())) ==
                          recoveryFile,
                  "cloud recording recovery lives in the current session");
        }

        // Destruction models the important half of a crash: unlike stop(), it
        // performs no cleanup and the next process can open the same sidecar.
        check(fs::is_regular_file(recoveryFile),
              "cloud recording recovery survives store destruction");
        daw::recovery::CloudRecordingRecoveryStore reopened(
            daw::platform::pathToUtf8(session));
        daw::recovery::CloudRecordingRecoveryManifest loaded;
        check(reopened.read(loaded).ok() && loaded == manifest &&
                  loaded.runs.size() == 2 &&
                  loaded.runs[0].captures[0].trackId ==
                      loaded.runs[1].captures[0].trackId &&
                  loaded.runs[1].captures[0].semantics.loopEnabled &&
                  !loaded.runs[1].captures[0]
                       .semantics.trimTakesToRegion,
              "cloud recovery round-trips runs, repeated tracks and frozen semantics");

        const std::string raw = readFile(recoveryFile);
        const nlohmann::json encoded = nlohmann::json::parse(raw);
        check(encoded.size() == 6 &&
                  encoded.value("format", "") ==
                      daw::recovery::kCloudRecordingRecoveryFormat &&
                  encoded.value("version", 0) ==
                      daw::recovery::kCloudRecordingRecoveryVersion &&
                  encoded.contains("createdAt") &&
                  encoded.at("runs").size() == 2 &&
                  encoded.at("runs")[0].contains("opId") &&
                  encoded.at("runs")[0].contains("transactionId") &&
                  encoded.at("runs")[0].at("captures")[0]
                      .value("uploadPhase", "") == "captured" &&
                  !encoded.contains("token") &&
                  !encoded.contains("url") &&
                  !encoded.contains("nickname"),
              "cloud recovery v2 has stable resumability ids and no identity secrets");
        check(raw.find(".tmp-") == std::string::npos,
              "cloud recovery publishes no temporary path in its JSON");

        using CaptureStatus =
            daw::recovery::CloudRecordingCaptureStatus;
        const auto classify =
            daw::recovery::classifyCloudRecordingCaptureStatus;
        check(classify(true, 4800, 4800, 0, true, 4800) ==
                  CaptureStatus::Ready &&
                  classify(true, 0, 0, 0, false, 0) ==
                      CaptureStatus::ZeroFrames &&
                  classify(true, 4800, 4800, 0, false, 4800) ==
                      CaptureStatus::Unreadable,
              "cloud recovery exposes only complete readable audio as ready");
        check(classify(false, 4800, 4800, 0, true, 4800) ==
                  CaptureStatus::WriteFailed &&
                  classify(true, 4800, 4700, 0, true, 4700) ==
                      CaptureStatus::WriteFailed &&
                  classify(true, 4800, 4800, 64, true, 4800) ==
                      CaptureStatus::WriteFailed &&
                  classify(false, 0, 0, 0, false, 0) ==
                      CaptureStatus::WriteFailed,
              "writer failure, short writes and drops cannot become ready or zero-frame captures");

        // Existing closed files remain durable even when they cannot yet be
        // planned into a clip. This includes a zero-byte recorder artifact, a
        // non-WAV payload bearing the reserved local .wav name and a readable
        // prefix from a failed writer.
        const fs::path zeroWav = sourceDir / "zero-frame.wav";
        const fs::path unreadableWav = sourceDir / "unreadable.wav";
        const fs::path failedPrefixWav =
            sourceDir / "failed-writer-prefix.wav";
        { std::ofstream(zeroWav, std::ios::binary); }
        { std::ofstream(unreadableWav, std::ios::binary) << "not a wav"; }
        writeTone(daw::platform::pathToUtf8(failedPrefixWav), 48000, 4800);
        auto rawManifest = manifest;
        daw::recovery::CloudRecordingRecoveryRun rawRun;
        rawRun.runId = "14141414-1414-4414-8414-141414141414";
        rawRun.opId = "15151515-1515-4515-8515-151515151515";
        rawRun.transactionId = "16161616-1616-4616-8616-161616161616";
        rawRun.createdAtUnixMs = manifest.createdAtUnixMs + 2;
        rawRun.recoveryOnly = true;
        rawRun.lostLease = true;
        daw::recovery::CloudRecordingCapture zeroCapture;
        zeroCapture.captureId = "17171717-1717-4717-8717-171717171717";
        zeroCapture.trackId = "18181818-1818-4818-8818-181818181818";
        zeroCapture.leaseId = "19191919-1919-4919-8919-191919191919";
        zeroCapture.uploadId = "20202020-2020-4020-8020-202020202020";
        zeroCapture.assetId = "21212121-2121-4121-8121-212121212121";
        zeroCapture.status =
            daw::recovery::CloudRecordingCaptureStatus::ZeroFrames;
        zeroCapture.localWavPath =
            daw::platform::pathToUtf8(zeroWav);
        zeroCapture.startSeconds = 20.0;
        zeroCapture.sampleRate = 48000.0;
        zeroCapture.channels = 2;
        rawRun.captures.push_back(zeroCapture);

        daw::recovery::CloudRecordingCapture unreadableCapture;
        unreadableCapture.captureId =
            "22222222-3333-4222-8222-333333333333";
        unreadableCapture.trackId =
            "23232323-2323-4323-8323-232323232323";
        unreadableCapture.leaseId =
            "24242424-2424-4424-8424-242424242424";
        unreadableCapture.uploadId =
            "25252525-2525-4525-8525-252525252525";
        unreadableCapture.assetId =
            "26262626-2626-4626-8626-262626262626";
        unreadableCapture.status =
            daw::recovery::CloudRecordingCaptureStatus::Unreadable;
        unreadableCapture.localWavPath =
            daw::platform::pathToUtf8(unreadableWav);
        unreadableCapture.startSeconds = 21.0;
        unreadableCapture.frames = 128;
        rawRun.captures.push_back(unreadableCapture);

        daw::recovery::CloudRecordingCapture failedPrefixCapture;
        failedPrefixCapture.captureId =
            "27272727-2727-4727-8727-272727272727";
        failedPrefixCapture.trackId =
            "28282828-2828-4828-8828-282828282828";
        failedPrefixCapture.leaseId =
            "29292929-2929-4929-8929-292929292929";
        failedPrefixCapture.uploadId =
            "30303030-3030-4030-8030-303030303030";
        failedPrefixCapture.assetId =
            "31313131-3131-4131-8131-313131313131";
        failedPrefixCapture.status =
            daw::recovery::CloudRecordingCaptureStatus::WriteFailed;
        failedPrefixCapture.localWavPath =
            daw::platform::pathToUtf8(failedPrefixWav);
        failedPrefixCapture.startSeconds = 22.0;
        failedPrefixCapture.durationSeconds = 0.1;
        failedPrefixCapture.sampleRate = 48000.0;
        failedPrefixCapture.channels = 2;
        failedPrefixCapture.frames = 4800;
        failedPrefixCapture.passes.push_back({22.0, 22.1, 0.0});
        rawRun.captures.push_back(failedPrefixCapture);
        rawManifest.runs.push_back(rawRun);
        check(reopened.write(rawManifest).ok(),
              "v2 persists zero-frame, unreadable and failed-writer WAV artifacts");
        daw::recovery::CloudRecordingRecoveryManifest rawLoaded;
        check(reopened.read(rawLoaded).ok() && rawLoaded == rawManifest &&
                  rawLoaded.runs.back().captures[0].status ==
                      daw::recovery::CloudRecordingCaptureStatus::ZeroFrames &&
                  rawLoaded.runs.back().captures[1].status ==
                      daw::recovery::CloudRecordingCaptureStatus::Unreadable &&
                  rawLoaded.runs.back().captures[2].status ==
                      daw::recovery::CloudRecordingCaptureStatus::WriteFailed &&
                  rawLoaded.runs.back().captures[2].passes.size() == 1 &&
                  rawLoaded.runs.back().captures[2].frames == 4800,
              "raw recovery status and readable failed prefix survive a strict v2 round-trip");

        // A strict legacy reader migrates v1 in memory. Generated retry ids
        // are deterministic, and the next write upgrades the file to v2.
        const nlohmann::json legacyCapture = {
            {"trackId", firstCapture.trackId},
            {"leaseId", firstCapture.leaseId},
            {"localWavPath", firstCapture.localWavPath},
            {"startSeconds", firstCapture.startSeconds},
            {"durationSeconds", firstCapture.durationSeconds},
            {"sampleRate", firstCapture.sampleRate},
            {"channels", firstCapture.channels},
            {"frames", firstCapture.frames},
            {"passes", {{{"startSeconds", 12.0},
                          {"endSeconds", 12.5},
                          {"captureOffsetSeconds", 0.0}}}},
            {"mode", "overwrite"}};
        const nlohmann::json legacy = {
            {"format", daw::recovery::kCloudRecordingRecoveryFormat},
            {"version", daw::recovery::kCloudRecordingRecoveryLegacyVersion},
            {"projectId", manifest.projectId},
            {"sessionId", manifest.sessionId},
            {"createdAt", manifest.createdAtUnixMs},
            {"recoveryOnly", true},
            {"lostLease", true},
            {"captures", {legacyCapture}}};
        { std::ofstream(recoveryFile) << legacy.dump(); }
        daw::recovery::CloudRecordingRecoveryManifest migratedOnce;
        daw::recovery::CloudRecordingRecoveryManifest migratedTwice;
        check(reopened.read(migratedOnce).ok() &&
                  reopened.read(migratedTwice).ok() &&
                  migratedOnce == migratedTwice &&
                  migratedOnce.version ==
                      daw::recovery::kCloudRecordingRecoveryVersion &&
                  migratedOnce.runs.size() == 1 &&
                  !migratedOnce.runs[0].captures[0].semantics.complete,
              "strict v1 read migrates deterministically without guessing semantics");
        check(reopened.write(migratedOnce).ok() &&
                  nlohmann::json::parse(readFile(recoveryFile))
                          .value("version", 0) ==
                      daw::recovery::kCloudRecordingRecoveryVersion &&
                  nlohmann::json::parse(readFile(recoveryFile))
                      .contains("runs"),
              "writing a migrated legacy manifest upgrades it to v2");
        check(reopened.write(manifest).ok(),
              "the current v2 generation is restored after migration testing");

        // Validation happens before publication: an invalid replacement must
        // leave the previous valid generation readable.
        auto invalid = manifest;
        invalid.projectId = "AAAAAAAA-AAAA-4AAA-8AAA-AAAAAAAAAAAA";
        check(invalid.projectId != manifest.projectId &&
                  reopened.write(invalid).code ==
                      daw::recovery::CloudRecordingRecoveryCode::Invalid,
              "uppercase non-canonical cloud UUID is rejected");
        daw::recovery::CloudRecordingRecoveryManifest stillValid;
        check(reopened.read(stillValid).ok() && stillValid == manifest,
              "a rejected cloud recovery write preserves the prior generation");

        invalid = manifest;
        invalid.runs.front().captures.front().localWavPath = "relative.wav";
        check(reopened.write(invalid).code ==
                  daw::recovery::CloudRecordingRecoveryCode::Invalid,
              "a relative recovery WAV path is rejected");
        invalid = manifest;
        invalid.runs.front().captures.front().localWavPath =
            daw::platform::pathToUtf8(sourceDir / "missing.wav");
        check(reopened.write(invalid).code ==
                  daw::recovery::CloudRecordingRecoveryCode::Invalid,
              "a missing recovery WAV is rejected");
        invalid = manifest;
        invalid.runs.front().captures.front().durationSeconds = 1.0;
        check(reopened.write(invalid).code ==
                  daw::recovery::CloudRecordingRecoveryCode::Invalid,
              "inconsistent cloud recording frames and duration are rejected");
        invalid = manifest;
        invalid.runs.front().captures.push_back(
            invalid.runs.front().captures.front());
        check(reopened.write(invalid).code ==
                  daw::recovery::CloudRecordingRecoveryCode::Invalid,
              "duplicate recovery leases and WAV paths are rejected");

        // Readers accept only the closed schema. This is also the privacy
        // boundary: a future caller cannot smuggle a bearer token, URL or
        // nickname into a recovery manifest as an unrecognized convenience.
        nlohmann::json forged = encoded;
        forged["token"] = "must-not-survive";
        { std::ofstream(recoveryFile) << forged.dump(); }
        daw::recovery::CloudRecordingRecoveryManifest untouched = manifest;
        untouched.projectId = "77777777-7777-4777-8777-777777777777";
        check(reopened.read(untouched).code ==
                  daw::recovery::CloudRecordingRecoveryCode::Invalid &&
                  untouched.projectId ==
                      "77777777-7777-4777-8777-777777777777",
              "strict read rejects unknown sensitive fields without partial output");

        forged = encoded;
        forged["runs"][0]["captures"][0]["uploadUrl"] =
            "https://example.invalid";
        { std::ofstream(recoveryFile) << forged.dump(); }
        check(reopened.read(untouched).code ==
                  daw::recovery::CloudRecordingRecoveryCode::Invalid,
              "strict read rejects unknown per-capture fields");
        forged = encoded;
        forged["runs"][0]["captures"][0]["semantics"]["inputDevice"] =
            "must-not-survive";
        { std::ofstream(recoveryFile) << forged.dump(); }
        check(reopened.read(untouched).code ==
                  daw::recovery::CloudRecordingRecoveryCode::Invalid,
              "strict read rejects local input data inside frozen semantics");
        forged = encoded;
        forged["runs"][0]["captures"][0]["status"] = "unknown";
        { std::ofstream(recoveryFile) << forged.dump(); }
        check(reopened.read(untouched).code ==
                  daw::recovery::CloudRecordingRecoveryCode::Invalid,
              "strict read rejects unknown capture states");
        forged = encoded;
        forged["version"] = 3;
        { std::ofstream(recoveryFile) << forged.dump(); }
        check(reopened.read(untouched).code ==
                  daw::recovery::CloudRecordingRecoveryCode::Invalid,
              "strict read rejects unsupported manifest versions");
        { std::ofstream(recoveryFile) << "{ broken json"; }
        check(reopened.read(untouched).code ==
                  daw::recovery::CloudRecordingRecoveryCode::Invalid,
              "strict read rejects a torn cloud recovery manifest");

        check(reopened.write(manifest).ok(),
              "a valid cloud recovery generation replaces corrupt input");
        bool cloudDebris = false;
        for (const auto& entry : fs::directory_iterator(session)) {
            const std::string name = entry.path().filename().string();
            cloudDebris = cloudDebris || name.find(".tmp-") != std::string::npos;
        }
        check(!cloudDebris,
              "successful cloud recovery publication leaves no temp debris");

        fs::remove(secondWav);
        check(reopened.read(untouched).code ==
                  daw::recovery::CloudRecordingRecoveryCode::Invalid,
              "strict read refuses a manifest whose WAV disappeared");
        writeTone(daw::platform::pathToUtf8(secondWav), 44100, 44100);
        check(reopened.read(loaded).ok(),
              "cloud recovery becomes readable when its WAV is restored");

        check(reopened.removeRunAfterCommit(
                  "88888888-8888-4888-8888-88888888888X").code ==
                      daw::recovery::CloudRecordingRunCleanupCode::Invalid &&
                  reopened.read(loaded).ok() && loaded == manifest &&
                  fs::is_regular_file(firstWav) &&
                  fs::is_regular_file(secondWav),
              "invalid exact-run cleanup fails closed before any publication");

        // Exact-run cleanup first publishes a replacement that retains the
        // other run. Only then may it unlink the selected run's WAV.
        const auto firstCleanup =
            reopened.removeRunAfterCommit(firstRun.runId);
        daw::recovery::CloudRecordingRecoveryManifest afterFirstCleanup;
        check(firstCleanup.code ==
                      daw::recovery::CloudRecordingRunCleanupCode::Removed &&
                  reopened.read(afterFirstCleanup).ok() &&
                  afterFirstCleanup.runs.size() == 1 &&
                  afterFirstCleanup.runs.front() == secondRun &&
                  !fs::exists(firstWav) && fs::is_regular_file(secondWav),
              "exact-run cleanup preserves other runs and removes only its WAV");
        check(reopened.removeRunAfterCommit(firstRun.runId).code ==
                      daw::recovery::CloudRecordingRunCleanupCode::AlreadyAbsent &&
                  reopened.read(afterFirstCleanup).ok() &&
                  afterFirstCleanup.runs.size() == 1 &&
                  fs::is_regular_file(secondWav),
              "exact-run cleanup is idempotent without touching remaining audio");

        const fs::path cleanupIntentFile =
            session / daw::recovery::kCloudRecordingRunCleanupFile;
        const nlohmann::json reusedPathIntent = {
            {"format", daw::recovery::kCloudRecordingRunCleanupFormat},
            {"version", daw::recovery::kCloudRecordingRunCleanupVersion},
            {"projectId", manifest.projectId},
            {"sessionId", manifest.sessionId},
            {"runId", firstRun.runId},
            {"wavPaths", {secondCapture.localWavPath}}};
        { std::ofstream(cleanupIntentFile) << reusedPathIntent.dump(); }
        check(reopened.removeRunAfterCommit(firstRun.runId).code ==
                      daw::recovery::CloudRecordingRunCleanupCode::Invalid &&
                  fs::is_regular_file(secondWav) && reopened.exists(),
              "cleanup intent cannot delete a WAV referenced by a remaining run");
        fs::remove(cleanupIntentFile);

        // Model a crash after the replacement manifest became visible but
        // before its selected WAV or durable intent was removed. The next
        // process can finish from the intent even though the run is no longer
        // present in the manifest.
        writeTone(daw::platform::pathToUtf8(firstWav), 48000, 24000);
        check(reopened.write(manifest).ok(),
              "cloud cleanup crash fixture restores both runs");
        const nlohmann::json cleanupIntent = {
            {"format", daw::recovery::kCloudRecordingRunCleanupFormat},
            {"version", daw::recovery::kCloudRecordingRunCleanupVersion},
            {"projectId", manifest.projectId},
            {"sessionId", manifest.sessionId},
            {"runId", firstRun.runId},
            {"wavPaths", {firstCapture.localWavPath}}};
        { std::ofstream(cleanupIntentFile) << cleanupIntent.dump(); }
        auto replacement = manifest;
        replacement.runs.erase(replacement.runs.begin());
        check(reopened.write(replacement).ok() && fs::exists(firstWav),
              "published cleanup generation never deletes WAV before its boundary");
        const auto resumedCleanup =
            reopened.removeRunAfterCommit(firstRun.runId);
        check(resumedCleanup.code ==
                      daw::recovery::CloudRecordingRunCleanupCode::Removed &&
                  !fs::exists(firstWav) && !fs::exists(cleanupIntentFile) &&
                  reopened.read(afterFirstCleanup).ok() &&
                  afterFirstCleanup.runs.size() == 1 &&
                  afterFirstCleanup.runs.front() == secondRun &&
                  fs::is_regular_file(secondWav),
              "post-publication crash resumes WAV cleanup without losing another run");

        const nlohmann::json lastRunIntent = {
            {"format", daw::recovery::kCloudRecordingRunCleanupFormat},
            {"version", daw::recovery::kCloudRecordingRunCleanupVersion},
            {"projectId", manifest.projectId},
            {"sessionId", manifest.sessionId},
            {"runId", secondRun.runId},
            {"wavPaths", {secondCapture.localWavPath}}};
        { std::ofstream(cleanupIntentFile) << lastRunIntent.dump(); }
        check(reopened.removeAfterCommit().ok() &&
                  !fs::exists(recoveryFile) && reopened.exists() &&
                  fs::is_regular_file(secondWav),
              "intent-only last-run crash remains discoverable before WAV cleanup");
        const auto lastCleanup =
            reopened.removeRunAfterCommit(secondRun.runId);
        check(lastCleanup.code ==
                      daw::recovery::CloudRecordingRunCleanupCode::Removed &&
                  !reopened.exists() && !fs::exists(secondWav) &&
                  !fs::exists(cleanupIntentFile),
              "last exact-run cleanup removes its WAV and the sidecar");
        check(reopened.removeRunAfterCommit(secondRun.runId).code ==
                  daw::recovery::CloudRecordingRunCleanupCode::AlreadyAbsent,
              "last-run cleanup remains idempotent after sidecar removal");

        // Keep the broad legacy API covered independently. It intentionally
        // removes only the sidecar and remains available to old callers.
        writeTone(daw::platform::pathToUtf8(firstWav), 48000, 24000);
        writeTone(daw::platform::pathToUtf8(secondWav), 44100, 44100);
        check(reopened.write(manifest).ok() &&
                  reopened.removeAfterCommit().ok() && !reopened.exists(),
              "only explicit post-commit cleanup removes cloud recovery");
        check(reopened.removeAfterCommit().ok(),
              "post-commit cloud recovery cleanup is idempotent");
    }

    // ── Plugin chunks are part of the same journal generation ──
    {
        daw::recovery::RecoveryJournal journal;
        check(journal.start(root.string(), "test-state-1.0",
                            std::chrono::milliseconds(10)),
              "a plugin-state journal opens");
        const fs::path session(journal.sessionDir());

        daw::recovery::RecoverySnapshot first;
        first.project = ctrl.project();
        daw::InsertModel slot;
        slot.id = "state-slot";
        slot.name = "State fixture";
        slot.format = daw::PluginFormat::Internal;
        slot.uid = "daw.test.state";
        slot.stateFile = "state-slot-old.bin";
        first.project.masterInserts.push_back(slot);
        first.pluginStates.push_back(
            {slot.stateFile, std::vector<std::uint8_t>{1, 2, 3, 4}});
        journal.requestWrite(std::move(first));
        journal.flush();
        check(readFile(session / "State" / "state-slot-old.bin") ==
                  std::string("\x01\x02\x03\x04", 4),
              "the journal writes the opaque plugin chunk");

        daw::recovery::RecoverySnapshot second;
        second.project = ctrl.project();
        slot.stateFile = "state-slot-new.bin";
        second.project.masterInserts.push_back(slot);
        second.pluginStates.push_back(
            {slot.stateFile, std::vector<std::uint8_t>{9, 8, 7}});
        journal.requestWrite(std::move(second));
        journal.flush();
        check(fs::is_regular_file(session / "State" / "state-slot-new.bin") &&
                  !fs::exists(session / "State" / "state-slot-old.bin"),
              "publishing a new generation removes only its obsolete chunk");

        daw::ProjectModel stateDocument;
        check(daw::ProjectSerializer::loadDocument(
                  stateDocument, (session / "project.json").string(), "").isOk() &&
                  stateDocument.masterInserts.size() == 1 &&
                  stateDocument.masterInserts.front().stateFile ==
                      "state-slot-new.bin",
              "the published journal references the matching state generation");
        journal.stop();
    }

    // ── A session that is never stopped is what a crash leaves behind ──
    std::string abandoned;
    {
        daw::recovery::RecoveryJournal journal;
        journal.start(root.string(), "test-1.0", std::chrono::milliseconds(50));
        abandoned = journal.sessionDir();
        journal.requestWrite(ctrl.project());
        journal.flush();
        // No stop() — the destructor must NOT tidy up, or a crash unwind would
        // destroy the very evidence recovery needs.
    }
    check(fs::exists(fs::path(abandoned) / "project.json"),
          "an unstopped session survives its journal object");

    // ── Finding leftovers ──
    {
        // Rewrite the abandoned session with a pid that cannot exist, so it
        // looks like what it is meant to represent: a process that is gone.
        daw::recovery::SessionInfo info;
        daw::recovery::readSession(abandoned, info);
        info.pid = 0x7FFFFFFF;
        daw::recovery::writeSession(info);
        check(!daw::recovery::isProcessAlive(info.pid),
              "an impossible pid reads as dead");
        check(daw::recovery::isProcessAlive(daw::recovery::currentProcessId()),
              "this process reads as alive");

        auto stale = daw::recovery::staleSessions(root.string());
        check(stale.size() == 1, "the leftover session is found");
        if (stale.size() == 1) {
            check(stale[0].directory == abandoned, "…and it is the right one");
            check(stale[0].projectPath.empty(),
                  "a session that never saved carries no project path");
            check(stale[0].appVersion == "test-1.0",
                  "the leftover still names the build that wrote it");
        }

        // A session belonging to a process that is still running is somebody
        // else's live DAW and must be left strictly alone.
        daw::recovery::RecoveryJournal live;
        live.start(root.string(), "test-1.0");
        live.requestWrite(ctrl.project());
        live.flush();
        auto stillOne = daw::recovery::staleSessions(root.string());
        check(stillOne.size() == 1, "a live session is not offered for recovery");
        check(daw::recovery::staleSessions(root.string(), live.sessionDir()).size() == 1,
              "excluding one's own session changes nothing here");
        live.stop();

        // A session that died before writing anything holds nothing to offer,
        // and is swept up rather than shown as an empty prompt.
        const fs::path empty = root / "1-1";
        fs::create_directories(empty);
        daw::recovery::SessionInfo emptyInfo;
        emptyInfo.directory = empty.string();
        emptyInfo.pid = 0x7FFFFFFF;
        daw::recovery::writeSession(emptyInfo);
        check(daw::recovery::staleSessions(root.string()).size() == 1,
              "a session with no journal is not offered");
        check(!fs::exists(empty), "…and is cleaned up");

        daw::recovery::discardSession(abandoned);
        check(daw::recovery::staleSessions(root.string()).empty(),
              "discarding a session removes it");
    }

    // ── The watchdog, against the real binary ──
    //
    // What it uniquely does is notice deaths and freezes from outside. Both are
    // driven here with a sacrificial process rather than mocked, because the
    // mechanism being tested — a pipe closing when a process dies — has no
    // meaning without a real process to kill.
    {
        const fs::path guardRoot = dir / "guard-root";
        const fs::path session = guardRoot / "1234-1";
        fs::create_directories(session);

        daw::recovery::SessionInfo info;
        info.directory = session.string();
        info.pid = daw::recovery::currentProcessId();
        info.heartbeat = 1;
        info.heartbeatUnixMs = daw::recovery::nowUnixMs();
        info.stats.trackCount = 3;
        daw::recovery::writeSession(info);
        { std::ofstream(session / "project.json") << "{}"; }

        int parentEnd = -1;
        const std::int64_t guardPid = daw::ScanProcess::spawnDetached(
            DAW_GUARD_PATH, {"--session", session.string(), "--pid",
                             std::to_string(info.pid)}, &parentEnd);
        check(guardPid > 0, "the guard starts");
#if defined(_WIN32)
        check(parentEnd < 0, "the guard uses native parent-process monitoring");
#else
        check(parentEnd >= 0, "the guard holds a parent-death pipe");
#endif

        // It logs what the program was doing — the "statistics" half of the job.
        for (int i = 0; i < 30 && !fs::exists(session / "health.jsonl"); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        check(fs::exists(session / "health.jsonl"),
              "the guard records a health log");
        if (fs::exists(session / "health.jsonl")) {
            const std::string line = readFile(session / "health.jsonl");
            check(line.find("\"trackCount\":3") != std::string::npos,
                  "the health log carries the program's statistics");
        }

        // Closing the pipe is exactly what dying does. The guard must notice
        // and leave a verdict behind.
#if !defined(_WIN32)
        ::close(parentEnd);
        parentEnd = -1;
        bool sawCrash = false;
        for (int i = 0; i < 50 && !sawCrash; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            daw::recovery::SessionInfo after;
            if (daw::recovery::readSession(session.string(), after))
                sawCrash = after.outcome == daw::recovery::Outcome::Crashed;
        }
        check(sawCrash, "the guard records a crash when the parent's pipe closes");
        check(reapedWithin(guardPid, 5000),
              "the guard exits once it has judged the session");
#endif
    }

    // ── The crash handler ──
    //
    // Provoked for real, in a forked child, because the thing under test is a
    // signal handler: nothing about it is exercised by calling it directly.
#if !defined(_WIN32)
    {
        const fs::path marker = dir / "crash.txt";
        // fork() copies the unflushed stdout buffer, and a child that dies with
        // it still full prints every earlier line a second time. Flush first,
        // so the report reads as what actually happened.
        std::fflush(nullptr);
        const pid_t child = ::fork();
        if (child == 0) {
            daw::crash::install(marker.string());
            daw::crash::setPluginInFlight("Serum");
            volatile int* boom = nullptr;
            *boom = 1;
            ::_exit(0);          // unreachable
        }
        int status = 0;
        ::waitpid(child, &status, 0);
        check(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV,
              "the handler re-raises, so the process still dies of its signal");

        const std::string described = daw::crash::readMarker(marker.string());
        check(described == "crashed in Serum (SIGSEGV)",
              "the marker names the signal and the plugin that was on the stack");
        const std::string raw = readFile(marker);
        check(raw.find("stack:") != std::string::npos &&
                  raw.find("recovery_test") != std::string::npos,
              "the marker carries a backtrace of the faulting process");
        check(raw.find("version=2") != std::string::npos &&
                  raw.find("pid=") != std::string::npos &&
                  raw.find("signal_code=") != std::string::npos &&
                  raw.find("address=0x") != std::string::npos,
              "the marker carries full machine-readable signal context");

        // With no plugin loaded there is nothing to blame, and the report must
        // not invent one.
        const fs::path bare = dir / "crash-bare.txt";
        std::fflush(nullptr);
        const pid_t second = ::fork();
        if (second == 0) {
            daw::crash::install(bare.string());
            ::abort();
        }
        ::waitpid(second, &status, 0);
        check(daw::crash::readMarker(bare.string()) == "crashed (SIGABRT)",
              "a crash outside any plugin is reported as just the signal");

        // macOS frameworks and third-party plugins commonly terminate through
        // a trap rather than a segmentation fault. This was the signal in the
        // field report that originally escaped the handler.
        const fs::path trap = dir / "crash-trap.txt";
        std::fflush(nullptr);
        const pid_t third = ::fork();
        if (third == 0) {
            daw::crash::install(trap.string());
            ::raise(SIGTRAP);
            ::_exit(0);
        }
        ::waitpid(third, &status, 0);
        check(WIFSIGNALED(status) && WTERMSIG(status) == SIGTRAP &&
                  daw::crash::readMarker(trap.string()) == "crashed (SIGTRAP)",
              "SIGTRAP is captured and still re-raised to the operating system");

        check(daw::crash::readMarker((dir / "nothing.txt").string()).empty(),
              "no marker means no claim about how anything ended");
    }
#endif

    // A freeze is the one failure no in-process handler can ever catch: a hung
    // program runs no signal handler and no timer. Only the heartbeat going
    // quiet, seen from outside, reveals it.
    {
        const fs::path hangRoot = dir / "guard-hang";
        const fs::path session = hangRoot / "1234-3";
        fs::create_directories(session);
        daw::recovery::SessionInfo info;
        info.directory = session.string();
        info.pid = daw::recovery::currentProcessId();
        info.heartbeat = 7;
        info.heartbeatUnixMs = daw::recovery::nowUnixMs();
        daw::recovery::writeSession(info);

        int parentEnd = -1;
        const std::int64_t guardPid = daw::ScanProcess::spawnDetached(
            DAW_GUARD_PATH, {"--session", session.string(), "--pid",
                             std::to_string(info.pid), "--hang-seconds", "2"},
            &parentEnd);
#if !defined(_WIN32)
        // The heartbeat is deliberately never touched again: the "program" is
        // alive but frozen.
        bool sawHang = false;
        for (int i = 0; i < 60 && !sawHang; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            daw::recovery::SessionInfo after;
            if (daw::recovery::readSession(session.string(), after))
                sawHang = after.outcome == daw::recovery::Outcome::Hung;
        }
        check(sawHang, "a frozen heartbeat on a live process is reported as a hang");

        // …and a program that comes back must clear the verdict, or the next
        // launch would cry crash over a stall the user never noticed.
        daw::recovery::SessionInfo revived;
        daw::recovery::readSession(session.string(), revived);
        revived.heartbeat += 1;
        revived.heartbeatUnixMs = daw::recovery::nowUnixMs();
        daw::recovery::writeSession(revived);
        bool recovered = false;
        for (int i = 0; i < 40 && !recovered; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            daw::recovery::SessionInfo after;
            if (daw::recovery::readSession(session.string(), after))
                recovered = after.outcome == daw::recovery::Outcome::Running;
        }
        check(recovered, "a heartbeat that resumes clears the hang verdict");

        fs::remove_all(session);
        ::close(parentEnd);
        reapedWithin(guardPid, 5000);
#else
        (void)guardPid; (void)parentEnd;
#endif
    }

    // A clean shutdown deletes the session directory, and the guard must take
    // that as "nothing happened" and leave rather than reporting a crash.
    {
        const fs::path cleanRoot = dir / "guard-clean";
        const fs::path session = cleanRoot / "1234-2";
        fs::create_directories(session);
        daw::recovery::SessionInfo info;
        info.directory = session.string();
        info.pid = daw::recovery::currentProcessId();
        daw::recovery::writeSession(info);

        int parentEnd = -1;
        const std::int64_t guardPid = daw::ScanProcess::spawnDetached(
            DAW_GUARD_PATH, {"--session", session.string(), "--pid",
                             std::to_string(info.pid)}, &parentEnd);
#if !defined(_WIN32)
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        fs::remove_all(session);          // what a clean shutdown looks like
        ::close(parentEnd);
        check(reapedWithin(guardPid, 5000),
              "the guard exits when the session ends cleanly");
        check(!fs::exists(session), "…and writes no verdict over the deletion");
#else
        (void)guardPid; (void)parentEnd;
#endif
    }

    if (failures) {
        std::printf("\n%d FAILURES PRESENT\n", failures);
        return 1;
    }
    std::printf("\nall recovery checks passed\n");
    return 0;
}
