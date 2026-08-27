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
