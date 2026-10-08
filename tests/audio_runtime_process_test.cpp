#include "AudioRuntimeProcess.hpp"
#include "AudioRuntimeProcessProtocol.hpp"
#include "AudioRuntimeCalls.hpp"
#include "SharedProcess.hpp"
#include "PluginProcess.hpp"
#include "Platform/PathUtils.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

namespace {
using namespace daw;
int failures = 0;
bool check(bool value, const char* message) {
    std::printf("%s %s\n", value ? "PASS" : "FAIL", message);
    failures += !value;
    return value;
}
AudioSessionPacket session(std::uint64_t generation = 1, float amplitude = .5f) {
    AudioSessionPacket packet; packet.generation = generation; packet.revision = 19;
    packet.blockSize = 64;
    AudioGraphSpec::Channel channel; channel.id = "source";
    packet.session.graph.channels.push_back(channel);
    auto audio = std::make_shared<engine::SampleBuffer>(2, 16384, 48000);
    for (unsigned ch = 0; ch < 2; ++ch) std::fill_n(audio->writableChannel(ch), audio->frames(), amplitude);
    engine::ClipPlacement placement;
    placement.audio = audio; placement.clipId = "clip"; placement.lengthSamples = audio->frames();
    auto clips = std::make_shared<engine::ClipPlayerNode::ClipList>(); clips->push_back(placement);
    AudioSessionSpec::Channel content; content.id = "source";
    content.content.clips = AudioContentSpec::Clips{clips, {}};
    packet.session.channels.push_back(std::move(content));
    AudioTransportCommand duration; duration.action = AudioTransportCommand::Action::Duration; duration.position = 16384;
    packet.transport.push_back(duration);
    return packet;
}
bool terminateOwnedChild(std::uint64_t pid) {
#ifdef _WIN32
    const auto process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, DWORD(pid));
    if (!process) return false;
    const bool killed = TerminateProcess(process, 99) != 0;
    if (killed) WaitForSingleObject(process, 5000);
    CloseHandle(process); return killed;
#else
    return kill(pid_t(pid), SIGKILL) == 0;
#endif
}
void processContract(const std::string& executable) {
    AudioRuntimeProcess process(executable);
    const auto started = process.replaceSession(session());
    if (!check(bool(started), "separate audio process accepts an immutable PCM session")) {
        std::printf("  %s\n", started.message().c_str()); return;
    }
    const auto pid = process.processId();
    check(pid != 0 && process.running(), "owned audio worker is running");
    AudioProcessSnapshot snapshot;
    check(bool(process.poll(snapshot)) && snapshot.generation == 1 && snapshot.revision == 19 &&
          !snapshot.device.running && !snapshot.transport.playing,
          "poll returns owned transport/device/diagnostic values without opening hardware");
    check(!process.startDevice() && process.running() && bool(process.poll(snapshot)) &&
          !snapshot.device.hasStream && !snapshot.transport.playing,
          "starting an unconfigured device fails without changing the acknowledged headless session");
    AudioControlPacket command; command.generation = 1; command.requestId = 1;
    command.command = AudioTransportCommand{AudioTransportCommand::Action::StartPlayback};
    check(bool(process.send(command)), "transport command reaches child renderer");
    check(!process.send(command), "duplicate control sequence is rejected");
    command.generation = 0; ++command.requestId;
    check(!process.send(command), "stale generation cannot change live transport");
    check(bool(process.advanceForTest(64, 16)), "child renders the real device callback headlessly");
    check(bool(process.poll(snapshot)) && snapshot.transport.position == 1024 &&
          snapshot.meters.at("source").left > .49f && snapshot.meters.at("master").right > .49f,
          "decoded PCM renders through child graph and advances exact sample positions");
    const auto ownedSnapshot = snapshot;
    check(bool(process.captureCheckpoint()), "native checkpoint and transport position are acknowledged atomically");
    AudioFaderCommand fader; fader.channelId = "source"; fader.change.gain = .25f;
    command.generation = 1; command.requestId = 2; command.command = fader;
    check(bool(process.send(command)), "fader gain is applied in the audio process");
    fader.change.gain.reset(); fader.change.pan = 0.f;
    ++command.requestId; command.command = fader;
    check(bool(process.send(command)), "partial fader edit preserves prior gain for crash recovery");

    auto invalid = session(2);
    invalid.session.graph.channels.front().outputBusId = "bus";
    AudioGraphSpec::Channel bus; bus.id = "bus"; bus.outputBusId = "source";
    invalid.session.graph.channels.push_back(bus);
    check(!process.replaceSession(std::move(invalid)), "cyclic candidate graph is rejected before publication");
    check(bool(process.poll(snapshot)) && snapshot.generation == 1 && snapshot.transport.playing,
          "failed generation leaves previous runtime and PCM mappings alive");
    check(terminateOwnedChild(pid), "fault fixture terminates only its owned audio child");
    check(!process.poll(snapshot), "dead audio worker returns an explicit error without local fallback");
    if (!check(bool(process.restart()), "worker restarts from owned checkpoint and acknowledged control edits")) {
        std::printf("  %s\n", process.error().c_str()); return;
    }
    check(process.processId() != pid && bool(process.poll(snapshot)) && snapshot.generation == 2 &&
          snapshot.transport.position == 1024 && !snapshot.transport.playing && !snapshot.transport.recording,
          "restart publishes a new process/generation and leaves capture and playback stopped");
    check(ownedSnapshot.transport.position == 1024 && ownedSnapshot.meters.at("source").left > .49f,
          "previously returned snapshots remain valid after child destruction");
    command.generation = process.generation(); command.requestId = 100;
    command.command = AudioTransportCommand{AudioTransportCommand::Action::StartPlayback};
    check(bool(process.send(command)) && bool(process.advanceForTest(64, 16)) && bool(process.poll(snapshot)) &&
          std::abs(snapshot.meters.at("source").left - .125f) < .003f,
          "restored child uses the coalesced fader gain after restart");
    process.close();
    check(!process.running() && process.processId() == 0, "close reaps the audio process before releasing resource files");
}
void deadline(const std::string& fixture) {
    AudioRuntimeProcess process(fixture, std::chrono::milliseconds(100));
    const auto start = std::chrono::steady_clock::now();
    const auto result = process.replaceSession(session());
    const auto elapsed = std::chrono::steady_clock::now() - start;
    check(!result && result.error() == audio::EngineError::Timeout && !process.running() &&
          elapsed < std::chrono::seconds(5), "unresponsive control worker is killed within a bounded deadline");
}
void pausedRestart(const std::string& executable) {
    AudioRuntimeProcess process(executable);
    auto packet = session();
    packet.transport.push_back({AudioTransportCommand::Action::Pause});
    AudioProcessSnapshot snapshot;
    check(bool(process.replaceSession(std::move(packet))) && bool(process.poll(snapshot)) &&
          snapshot.transport.state == engine::TransportState::Paused && bool(process.restart()) &&
          bool(process.poll(snapshot)) && snapshot.transport.state == engine::TransportState::Stopped,
          "restart converts an initially paused session to stopped without an intermediate checkpoint");
}
void auditionSessions(const std::string& executable) {
    AudioRuntimeProcess process(executable);
    std::uint64_t secondary = 0;
    if (!check(bool(process.replaceSession(session())) && bool(process.createSession(session(1, .2f), secondary)),
        "one child owns primary and a numeric secondary session")) return;
    const auto pid = process.processId();
    AudioProcessSnapshot primary, draft;
    check(secondary != 0 && bool(process.poll(draft, secondary)) && draft.sessionId == secondary &&
          !draft.device.running && !draft.device.hasStream && process.processId() == pid,
          "secondary snapshot is session scoped and creates no device or extra audio process");
    AudioControlPacket command; command.generation = 1; command.requestId = 1;
    command.command = AudioTransportCommand{AudioTransportCommand::Action::StartPlayback};
    check(bool(process.send(command)) && bool(process.advanceForTest(64, 16)) && bool(process.startAudition(secondary)),
          "audition takes over the primary output inside the child");
    check(bool(process.advanceForTest(64, 16)) && bool(process.poll(primary)) && bool(process.poll(draft, secondary)) &&
          primary.auditionSessionId == secondary && primary.transport.position == 1024 && !primary.transport.playing &&
          draft.transport.position == 1024 && draft.transport.playing &&
          std::abs(primary.meters.at("master").left - .2f) < .003f,
          "primary callback renders secondary PCM while primary transport remains paused");
    check(!process.advanceForTest(64, 1, secondary), "secondary cannot be driven twice while audition is active");
    check(!process.startAudition(secondary) && !process.replaceSession(session(2), false, {}, secondary),
          "duplicate audition and active-generation replacement are rejected without detaching output");
    AudioFaderCommand fader; fader.channelId = "source"; fader.change.gain = .5f;
    command.command = fader;
    check(bool(process.send(command, secondary)), "secondary control sequence is independent of primary sequence");
    fader.change.gain = .25f; command.command = fader; command.requestId = 2;
    check(bool(process.send(command)) && bool(process.advanceForTest(64, 16)) && bool(process.poll(primary)) &&
          std::abs(primary.meters.at("master").left - .1f) < .003f,
          "editing the inactive primary does not change secondary audio");
    command.command = AudioTransportCommand{AudioTransportCommand::Action::Record};
    check(!process.send(command, secondary), "secondary session refuses recording commands");
    check(bool(process.captureCheckpoint(secondary)), "secondary checkpoint uses its own resource directory");
    const auto ownedDraft = draft;
    check(bool(process.closeSession(secondary)) && bool(process.poll(primary)) && primary.auditionSessionId == 0 &&
          !process.poll(draft, secondary) && !process.startAudition(secondary) && !process.closeSession(secondary),
          "deleting active audition drains readers and rejects the retired session ID");
    check(ownedDraft.sessionId == secondary && ownedDraft.transport.position == 1024,
          "secondary value snapshots survive session destruction");
    command.requestId = 3; command.command = AudioTransportCommand{AudioTransportCommand::Action::StartPlayback};
    check(bool(process.send(command)) && bool(process.advanceForTest(64, 16)) && bool(process.poll(primary)) &&
          std::abs(primary.meters.at("master").left - .125f) < .003f,
          "detaching audition restores primary with edits made while it was inactive");

    auto mismatch = session(1, .3f); mismatch.sampleRate = 44100;
    std::uint64_t wrongFormat = 0, next = 0;
    check(bool(process.createSession(std::move(mismatch), wrongFormat)) && !process.startAudition(wrongFormat) &&
          bool(process.poll(primary)) && primary.transport.playing && primary.auditionSessionId == 0,
          "incompatible audition format leaves the primary playing unchanged");
    check(bool(process.closeSession(wrongFormat)), "unused secondary can be removed independently");
    auto invalid = session();
    invalid.session.graph.channels.front().outputBusId = "bus";
    AudioGraphSpec::Channel bus; bus.id = "bus"; bus.outputBusId = "source";
    invalid.session.graph.channels.push_back(bus);
    std::uint64_t untouched = 777;
    check(!process.createSession(std::move(invalid), untouched) && untouched == 777 && process.running(),
          "failed secondary preparation neither publishes an ID nor terminates the shared audio child");
    check(bool(process.createSession(session(1, .3f), next)) && next > secondary &&
          bool(process.replaceSession(session(2, .3f), false, {}, next)) && process.generation(next) == 2,
          "new secondary ID is monotonic and supports independent generation replacement");
    command.generation = 2; command.requestId = 1;
    fader.change.gain = .25f; command.command = fader;
    check(bool(process.send(command, next)) && bool(process.startAudition(next)) && bool(process.advanceForTest(64, 16)) &&
          bool(process.captureCheckpoint(next)) && bool(process.captureCheckpoint()),
          "primary and secondary checkpoints coexist with separately journaled edits");
    check(terminateOwnedChild(pid) && !process.poll(primary) && bool(process.restart()),
          "crashed child restores its full acknowledged session family");
    check(bool(process.poll(primary)) && bool(process.poll(draft, next)) && primary.generation == 2 && draft.generation == 3 &&
          !primary.transport.playing && !draft.transport.playing && !primary.transport.recording &&
          primary.auditionSessionId == 0 && !process.poll(draft, secondary),
          "restart keeps session IDs, retires deleted drafts, and never resumes playback or audition");
    check(bool(process.startAudition(next)) && bool(process.advanceForTest(64, 16)) && bool(process.poll(primary)) &&
          std::abs(primary.meters.at("master").left - .075f) < .003f,
          "restarted secondary restores its PCM, position, and its own fader journal");
    check(bool(process.stopAudition()) && bool(process.closeSession(next)), "explicit stop and secondary retirement succeed after restart");
}
void incrementalSessions(const std::string& executable) {
    AudioRuntimeProcess process(executable);
    auto primaryValues = session(), secondaryValues = session(1, .2f);
    std::uint64_t secondary = 0;
    if (!check(bool(process.replaceSession(primaryValues)) && bool(process.createSession(secondaryValues, secondary)) &&
          bool(process.startAudition(secondary)), "incremental fixture starts real child audition")) return;
    primaryValues.transport.clear(); secondaryValues.transport.clear();
    primaryValues.revision = secondaryValues.revision = 20;
    primaryValues.session.graph.masterVolume = secondaryValues.session.graph.masterVolume = .5f;
    check(bool(process.applySession(primaryValues, 19)) && bool(process.applySession(secondaryValues, 19, secondary)),
          "primary and active secondary reconcile independently without changing their generations");
    AudioProcessSnapshot primary, draft;
    check(bool(process.advanceForTest(64, 16)) && bool(process.poll(primary)) && bool(process.poll(draft, secondary)) &&
          primary.revision == 20 && draft.revision == 20 && primary.generation == 1 && draft.generation == 1 &&
          primary.auditionSessionId == secondary && std::abs(primary.meters.at("master").left - .1f) < .003f,
          "same-generation secondary edits become audible without interrupting audition selection");
    auto stale = secondaryValues; stale.revision = 21;
    check(!process.applySession(stale, 19, secondary), "stale document revision cannot overwrite a newer runtime graph");
    stale.generation = 2;
    check(!process.applySession(stale, 20, secondary), "incremental updates cannot target another runtime generation");
    auto cyclic = secondaryValues; cyclic.revision = 21;
    cyclic.session.graph.channels.front().outputBusId = "bus";
    AudioGraphSpec::Channel bus; bus.id = "bus"; bus.outputBusId = "source";
    cyclic.session.graph.channels.push_back(bus); cyclic.session.graph.masterVolume = .01f;
    check(!process.applySession(cyclic, 20, secondary) && bool(process.advanceForTest(64, 16)) &&
          bool(process.poll(primary)) && bool(process.poll(draft, secondary)) && draft.revision == 20 &&
          std::abs(primary.meters.at("master").left - .1f) < .003f,
          "failed secondary graph rolls back its changed controls and leaves audition playing");
    auto replacement = session(1, .4f); replacement.transport.clear(); replacement.revision = 21;
    replacement.session.graph.masterVolume = .5f;
    check(bool(process.applySession(replacement, 20, secondary)) && bool(process.advanceForTest(64, 16)) &&
          bool(process.poll(primary)) && std::abs(primary.meters.at("master").left - .2f) < .003f,
          "new immutable PCM publishes through the persistent session resource registry");
    const auto pid = process.processId();
    check(terminateOwnedChild(pid) && !process.poll(primary) && bool(process.restart()) &&
          bool(process.poll(draft, secondary)) && draft.revision == 21 && !draft.transport.playing &&
          bool(process.startAudition(secondary)) && bool(process.advanceForTest(64, 16)) && bool(process.poll(primary)) &&
          std::abs(primary.meters.at("master").left - .2f) < .003f,
          "restart uses the latest committed session revision, PCM, and retained checkpoint journal");
    check(bool(process.closeSession(secondary)), "edited active secondary retires with all generation resources");
    AudioControlPacket play; play.generation = process.generation(); play.requestId = 100;
    play.command = AudioTransportCommand{AudioTransportCommand::Action::StartPlayback};
    check(bool(process.send(play)) && bool(process.advanceForTest(64, 16)) && bool(process.poll(primary)) &&
          std::abs(primary.meters.at("master").left - .25f) < .003f,
          "primary transaction made during audition survives both output restoration and process restart");
}
void transactionRecovery(const std::string& executable) {
    AudioRuntimeProcess process(executable);
    AudioControlPacket command; command.generation = 1; command.requestId = 1;
    command.command = AudioTransportCommand{AudioTransportCommand::Action::StartPlayback};
    if (!check(bool(process.replaceSession(session())) && bool(process.send(command)) &&
        bool(process.advanceForTest(64, 16)), "transaction fixture starts the real child renderer")) return;
    AudioFaderCommand fader; fader.channelId = "source"; fader.change.gain = .5f;
    command.command = fader; command.requestId = 2;
    std::uint64_t token = 0;
    if (!check(bool(process.send(command)) && bool(process.captureTransaction(token)) && token,
        "numeric lease captures graph controls and the acknowledged recovery journal")) return;
    const auto pid = process.processId();
    auto next = session(1, .8f); next.transport.clear(); next.revision = 20;
    next.session.graph.masterVolume = .25f;
    fader.change.gain = .75f; command.command = fader; command.requestId = 3;
    AudioProcessSnapshot snapshot;
    check(bool(process.applySession(next, 19)) && bool(process.send(command)) &&
          bool(process.advanceForTest(64, 16)) && bool(process.poll(snapshot)) &&
          snapshot.revision == 20 && std::abs(snapshot.meters.at("master").left - .15f) < .003f,
          "intermediate publication changes PCM, graph values and command history");
    const auto livePosition = snapshot.transport.position;
    check(bool(process.restoreTransaction(token)) && bool(process.poll(snapshot)) &&
          process.processId() == pid && snapshot.revision == 19 && snapshot.transport.playing &&
          snapshot.transport.position == livePosition && snapshot.plugins.changed && snapshot.plugins.scanned &&
          snapshot.plugins.notices.empty(),
          "rollback restores revision without rewinding live transport and requests fresh plugin readout");
    check(bool(process.advanceForTest(64, 16)) && bool(process.poll(snapshot)) &&
          std::abs(snapshot.meters.at("master").left - .25f) < .003f,
          "rollback restores the original mapped PCM and live fader controls");
    // Reusing the first post-capture sequence proves both sides restored the
    // sequence, rather than merely reverting the document's revision number.
    fader.change.gain = .1f; command.command = fader;
    check(bool(process.send(command)) && bool(process.restoreTransaction(token)) &&
          bool(process.restoreTransaction(token)) && bool(process.releaseTransaction(token)) &&
          !process.restoreTransaction(token) && !process.releaseTransaction(token),
          "restore is repeatable, restores command sequence, and only release consumes the lease");
    check(terminateOwnedChild(pid) && !process.poll(snapshot) && bool(process.restart()) &&
          bool(process.poll(snapshot)) && snapshot.generation == 2 && snapshot.revision == 19 &&
          snapshot.transport.position == 1024 && !snapshot.transport.playing && !snapshot.transport.recording,
          "crash after rollback restarts the captured revision and position with transport stopped");
    command.generation = 2; command.requestId = 100;
    command.command = AudioTransportCommand{AudioTransportCommand::Action::StartPlayback};
    check(bool(process.send(command)) && bool(process.advanceForTest(64, 16)) && bool(process.poll(snapshot)) &&
          std::abs(snapshot.meters.at("master").left - .25f) < .003f,
          "restart replays only the restored journal and PCM, never the abandoned intermediate edits");
}
void transactionLifetimes(const std::string& executable) {
    AudioRuntimeProcess process(executable);
    std::uint64_t secondary = 0, primaryToken = 0, secondaryToken = 0;
    if (!check(bool(process.replaceSession(session())) && bool(process.createSession(session(1, .2f), secondary)) &&
        bool(process.captureTransaction(primaryToken)) && bool(process.captureTransaction(secondaryToken, secondary)),
        "primary and secondary transactions share a process with independent numeric leases")) return;
    check(!process.restoreTransaction(primaryToken, secondary) && !process.releaseTransaction(primaryToken, secondary) &&
          !process.restoreTransaction(secondaryToken) && !process.restoreTransaction(0),
          "session mismatch and unknown transaction tokens cannot mutate either graph");
    audio::AudioDeviceConfig actual;
    check(!process.configureDevice({}, actual) && process.running(),
          "an outstanding primary lease prevents incompatible device reconfiguration");
    auto invalid = session(2);
    invalid.session.graph.channels.front().outputBusId = "bus";
    AudioGraphSpec::Channel bus; bus.id = "bus"; bus.outputBusId = "source";
    invalid.session.graph.channels.push_back(bus);
    check(!process.replaceSession(std::move(invalid)) && bool(process.restoreTransaction(primaryToken)),
          "a rejected generation candidate preserves valid leases of the published graph");
    check(bool(process.replaceSession(session(2))) && !process.restoreTransaction(primaryToken) &&
          !process.releaseTransaction(primaryToken) && bool(process.restoreTransaction(secondaryToken, secondary)),
          "successful replacement retires only leases belonging to the replaced generation");
    check(bool(process.startAudition(secondary)), "secondary transaction fixture begins audition");
    auto update = session(1, .7f); update.revision = 20; update.transport.clear();
    AudioProcessSnapshot snapshot, draft;
    check(bool(process.applySession(std::move(update), 19, secondary)) &&
          bool(process.advanceForTest(64, 16)) && bool(process.restoreTransaction(secondaryToken, secondary)) &&
          bool(process.advanceForTest(64, 16)) && bool(process.poll(snapshot)) && bool(process.poll(draft, secondary)) &&
          snapshot.auditionSessionId == secondary && draft.revision == 19 && draft.transport.playing &&
          std::abs(snapshot.meters.at("master").left - .2f) < .003f,
          "secondary rollback preserves active audition while restoring its own PCM and metadata");
    check(bool(process.closeSession(secondary)) && !process.restoreTransaction(secondaryToken, secondary) &&
          !process.releaseTransaction(secondaryToken, secondary),
          "closing active secondary drains output and retires all of its transaction tokens");
    std::uint64_t closingToken = 0;
    check(bool(process.captureTransaction(closingToken)) && closingToken > secondaryToken,
          "transaction IDs are monotonic across independent session and generation retirement");
    process.close();
    check(!process.restoreTransaction(closingToken) && !process.releaseTransaction(closingToken) && !process.running(),
          "closing the audio process invalidates every outstanding lease");
    check(bool(process.restart()), "acknowledged session can restart after retiring its process leases");
    std::vector<std::uint64_t> tokens;
    for (std::size_t i = 0; i < audioipc::kMaxTransactions; ++i) {
        std::uint64_t token = 0;
        if (!process.captureTransaction(token)) break;
        tokens.push_back(token);
    }
    std::uint64_t untouched = 777;
    check(tokens.size() == audioipc::kMaxTransactions && tokens.front() > closingToken &&
          !process.captureTransaction(untouched) && untouched == 777,
          "transaction ownership is bounded and overflow does not publish a token");
    if (!tokens.empty()) {
        check(bool(process.releaseTransaction(tokens.back())) && bool(process.captureTransaction(untouched)) &&
              untouched > tokens.back(), "releasing a lease returns capacity without reusing its identity");
        tokens.pop_back(); tokens.push_back(untouched);
    }
    const auto pid = process.processId();
    check(terminateOwnedChild(pid) && !process.running() && !process.restoreTransaction(untouched) && bool(process.restart()),
          "observing child death retires outstanding native leases before restarting its generation");
}
void recordingSessionUpdates(const std::string& executable) {
    using audio_rpc::Method;
    struct Directory {
        std::filesystem::path path = std::filesystem::temp_directory_path() /
            ("daw-recording-update-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        Directory() { std::filesystem::create_directory(path); }
        ~Directory() { std::error_code error; std::filesystem::remove_all(path, error); }
    } directory;
    try {
        AudioRuntimeProcess process(executable);
        auto packet = session(); packet.session.channels.clear();
        packet.session.graph.channels.front().input = {true, false, 1, 1, 3};
        if (!check(bool(process.replaceSession(packet)), "recording update fixture starts a real child")) return;
        const auto request = [&]<Method Id>(typename audio_rpc::Binding<Id>::Inputs arguments) {
            auto [status, result] = audio_rpc::call<Id>(process, 0, std::move(arguments));
            if (!status) throw std::runtime_error(status.message());
            return result;
        };
        const AudioCaptureSpec spec{directory.path.string(), 1, 1, true, 0};
        const auto [startedResult, startedOutputs] = request.template operator()<Method::startCapture>({spec, AudioCaptureStarted{}});
        const auto started = std::get<0>(startedOutputs);
        if (!check(bool(startedResult.result()) && started.id,
            "capture writer is prepared before its monitoring graph or publication")) return;
        packet.transport.clear(); packet.revision = 20;
        packet.session.graph.channels.front().capturing = true;
        packet.session.graph.channels.front().input.enabled = true;
        check(bool(process.applySession(packet, 19)), "same-generation projection prepares monitoring with an unpublished active capture");
        const auto initial = std::get<0>(request.template operator()<Method::captureStatus>({started.id}));
        check(initial.available && initial.recording && initial.recordedFrames == 0 &&
              std::get<0>(request.template operator()<Method::publishCaptures>({std::vector<AudioCaptureId>{started.id}})),
              "graph projection preserves the capture identity and its pending publication");
        AudioControlPacket command; command.generation = 1; command.requestId = 1;
        command.command = AudioTransportCommand{AudioTransportCommand::Action::Record};
        const std::array<float, 2> input{.25f, -.5f};
        AudioProcessSnapshot snapshot;
        check(bool(process.send(command)) && bool(process.advanceForTest(64, 8, 0, input)) && bool(process.poll(snapshot)) &&
              snapshot.transport.recording && std::abs(snapshot.meters.at("master").left - .5f) < .003f,
              "real callback records and monitors the selected hardware input");
        packet.revision = 21;
        packet.session.graph.channels.front().input.channel = 0;
        packet.session.graph.channels.front().volume = .5f;
        check(bool(process.applySession(packet, 20)) && bool(process.advanceForTest(64, 8, 0, input)) &&
              bool(process.poll(snapshot)) && snapshot.revision == 21 && snapshot.transport.recording &&
              std::abs(snapshot.meters.at("master").left - .125f) < .003f,
              "recording continues while a safe graph edit changes only monitoring route and gain");
        auto cyclic = packet; cyclic.revision = 22;
        cyclic.session.graph.channels.front().outputBusId = "bus";
        cyclic.session.graph.channels.front().volume = .01f;
        AudioGraphSpec::Channel bus; bus.id = "bus"; bus.outputBusId = "source";
        cyclic.session.graph.channels.push_back(bus);
        check(!process.applySession(std::move(cyclic), 21) && bool(process.advanceForTest(64, 8, 0, input)) &&
              bool(process.poll(snapshot)) && snapshot.revision == 21 && snapshot.transport.recording &&
              std::abs(snapshot.meters.at("master").left - .125f) < .003f,
              "failed graph reconciliation rolls back monitoring without replacing or stopping capture");
        const auto peaks = std::get<0>(request.template operator()<Method::capturePeaks>({started.id, 0}));
        check(peaks.status.recording && peaks.status.recordedFrames == 1536 && !peaks.values.empty() &&
              std::all_of(peaks.values.begin(), peaks.values.end(), [](float value) { return std::abs(value - .5f) < .0001f; }),
              "capture retains its hardware route and exact continuous frame count across successful and failed edits");
        auto wrongFormat = packet; wrongFormat.revision = 22; wrongFormat.sampleRate = 44100;
        audio::AudioDeviceConfig actual;
        std::uint64_t token = 777;
        check(!process.applySession(wrongFormat, 21) && !process.replaceSession(session(2)) &&
              !process.configureDevice({}, actual) && !process.captureTransaction(token) && token == 777,
              "active capture still rejects format, generation, device and transaction replacement");
        const auto [closedResult, closedOutputs] = request.template operator()<Method::stopCapture>({started.id, audio::RecordingSession{}});
        const auto& closed = std::get<0>(closedOutputs);
        check(bool(closedResult.result()) && closed.fileWriteSucceeded && closed.writtenFrames == 1536 &&
              closed.channelCount == 1 && std::filesystem::is_regular_file(closed.filePath),
              "the preserved capture finalizes one durable mono file with all recorded frames");
        command.requestId = 2; command.command = AudioTransportCommand{AudioTransportCommand::Action::Stop};
        packet.revision = 22; packet.session.graph.channels.front().capturing = false;
        check(bool(process.send(command)) && bool(process.applySession(packet, 21)) &&
              !std::get<0>(request.template operator()<Method::captureStatus>({started.id})).available,
              "recording finish publishes the post-capture graph without resurrecting the retired capture ID");
    } catch (const std::exception& error) {
        std::printf("  %s\n", error.what());
        check(false, "recording session update fixture completed");
    }
}
void recordingRestart(const std::string& executable) {
    AudioRuntimeProcess process(executable);
    auto packet = session();
    auto& content = *packet.session.channels.front().content.clips;
    content.individual.emplace("private-take", content.shared);
    content.shared = std::make_shared<const engine::ClipPlayerNode::ClipList>();
    packet.session.graph.channels.front().clipFx.push_back({"private-take", "Take", .5f});
    AudioPluginSpec slot; slot.id = "eq"; slot.uid = slot.descriptor.uid = "daw.equalizer";
    slot.name = slot.descriptor.name = "Take EQ";
    slot.requiredFormat = slot.descriptor.format = plugins::Format::Internal;
    packet.session.pluginChains.push_back({"source", AudioPluginChainSpec::Kind::ClipFx, "private-take", {slot}});
    AudioControlPacket command; command.generation = 1; command.requestId = 1;
    command.command = AudioTransportCommand{AudioTransportCommand::Action::StartPlayback};
    AudioProcessSnapshot snapshot;
    if (!check(bool(process.replaceSession(packet)) && bool(process.send(command)) &&
        bool(process.advanceForTest(64, 16)) && bool(process.poll(snapshot)) &&
        std::abs(snapshot.meters.at("master").left - .25f) < .003f,
        "recording recovery fixture renders a private take through its native Clip FX")) return;
    packet.transport.clear(); packet.revision = 20;
    packet.session.graph.channels.front().capturing = true;
    command.requestId = 2; command.command = AudioTransportCommand{AudioTransportCommand::Action::Record};
    const auto suppressed = process.applySession(packet, 19);
    const auto [suspended, ignored] = audio_rpc::call<audio_rpc::Method::suspendRecordingClipFx>(process, 0,
        {std::vector<std::string>{"source"}});
    check(bool(suppressed) && bool(suspended) && bool(process.send(command)) &&
          bool(process.advanceForTest(64, 16)) && bool(process.poll(snapshot)) &&
          snapshot.transport.recording && snapshot.meters.at("master").left < .001f,
          "recording projection suppresses the previous private take and suspends its Clip FX");
    check(terminateOwnedChild(process.processId()) && !process.poll(snapshot) && bool(process.restart()) &&
          bool(process.poll(snapshot)) && !snapshot.transport.playing && !snapshot.transport.recording,
          "crash recovery discards capture mode and leaves the new child stopped");
    command.generation = process.generation(); command.requestId = 100;
    command.command = AudioTransportCommand{AudioTransportCommand::Action::StartPlayback};
    check(bool(process.send(command)) && bool(process.advanceForTest(64, 16)) && bool(process.poll(snapshot)) &&
          std::abs(snapshot.meters.at("master").left - .25f) < .003f,
          "fresh stopped projection reconnects private takes and prepares their formerly suspended Clip FX");
}
#if defined(DAW_FAULT_CLAP_PATH)
void nestedPreparationDeadline(const std::string& executable) {
    const auto slot = [](std::string id, std::string uid) {
        AudioPluginSpec value; value.id = std::move(id); value.uid = std::move(uid); value.name = value.uid;
        value.requiredFormat = value.descriptor.format = plugins::Format::Clap;
        value.descriptor.uid = value.uid; value.descriptor.name = value.name; value.descriptor.path = DAW_FAULT_CLAP_PATH;
        return value;
    };
    auto packet = session(); packet.blockSize = 1024;
    packet.session.hosting = {plugins::HostingMode::Isolated, DAW_PLUGIN_HOST_PATH};
    AudioPluginChainSpec chain; chain.channelId = "source";
    chain.slots.push_back(slot("healthy", "com.daw.test.fault"));
    packet.session.pluginChains.push_back(chain);
    AudioRuntimeProcess process(executable);
    if (!check(bool(process.replaceSession(packet)), "nested deadline fixture starts with a healthy published plugin")) return;
    const auto pid = process.processId();
    const auto identity = process.lastPublication().plugins.front().instance;
    const auto bounded = plugins::kPluginControlTimeout + plugins::ipc::kProcessStopWait + std::chrono::seconds(3);

    auto replacement = packet; replacement.generation = 2;
    replacement.session.pluginChains.front().slots.push_back(slot("stuck-state", "com.daw.test.fault.state_hang"));
    AudioPluginStateEdit restore; restore.address = {"source", "stuck-state"}; restore.state.state.resize(32768);
    replacement.restores.push_back(std::move(restore));
    const auto stateStarted = std::chrono::steady_clock::now();
    const auto rejected = process.replaceSession(std::move(replacement));
    const auto stateElapsed = std::chrono::steady_clock::now() - stateStarted;
    AudioProcessSnapshot snapshot;
    if (!check(!rejected && process.running() && process.processId() == pid &&
        bool(process.poll(snapshot)) && snapshot.generation == 1 &&
        process.lastPublication().plugins.front().instance == identity &&
        stateElapsed >= plugins::kPluginControlTimeout && stateElapsed < bounded,
        "a hung plugin state import expires inside its host and leaves the previous audio process and session alive")) {
        std::printf("  %s\n", rejected.message().c_str()); return;
    }

    auto unavailable = slot("stuck-activation", "com.daw.test.fault.activate");
    unavailable.loadPolicy = AudioPluginLoadPolicy::PreserveUnavailable;
    packet.session.pluginChains.front().slots.push_back(std::move(unavailable));
    packet.transport.clear(); packet.revision = 20;
    const auto activationStarted = std::chrono::steady_clock::now();
    const auto updated = process.applySession(packet, 19);
    const auto activationElapsed = std::chrono::steady_clock::now() - activationStarted;
    const auto [status, reply] = audio_rpc::call<audio_rpc::Method::pluginRuntimeStatus>(process, 0,
        {std::string("source"), std::string("stuck-activation")});
    check(bool(updated) && process.running() && process.processId() == pid && bool(process.poll(snapshot)) &&
        snapshot.revision == 20 && process.lastPublication().plugins.size() == 1 &&
        process.lastPublication().plugins.front().instance == identity && bool(status) &&
        std::get<0>(reply).state == AudioPluginRuntimeState::Missing &&
        activationElapsed >= plugins::kPluginControlTimeout && activationElapsed < bounded,
        "a hung optional plugin becomes a placeholder without killing its healthy sibling or audio process");
    if (!updated) std::printf("  %s\n", updated.message().c_str());
}

void preparationDeadline(const std::string& executable) {
    auto packet = session(); packet.blockSize = 1024;
    packet.session.hosting = {plugins::HostingMode::Isolated, DAW_PLUGIN_HOST_PATH};
    AudioPluginChainSpec chain; chain.channelId = "source";
    packet.session.pluginChains.push_back(std::move(chain));
    const auto append = [&](unsigned offset) {
        packet.restores.clear();
        for (unsigned i = 0; i < 6; ++i) {
            AudioPluginSpec slot;
            slot.id = "slow-" + std::to_string(offset + i);
            slot.uid = "com.daw.test.fault.state_slow"; slot.name = "Slow state fixture";
            slot.requiredFormat = slot.descriptor.format = plugins::Format::Clap;
            slot.descriptor.uid = slot.uid; slot.descriptor.name = slot.name;
            slot.descriptor.path = DAW_FAULT_CLAP_PATH;
            AudioPluginStateEdit restore;
            restore.address = {"source", slot.id};
            restore.state.state.resize(32768); // Fixture loads this valid zero-gain state in 300 ms.
            packet.restores.push_back(std::move(restore));
            packet.session.pluginChains.front().slots.push_back(std::move(slot));
        }
    };
    append(0);
    const auto timeout = std::chrono::milliseconds(1500);
    AudioRuntimeProcess process(executable, timeout);
    const auto started = std::chrono::steady_clock::now();
    const auto loaded = process.replaceSession(packet);
    if (!check(bool(loaded) && process.running() && std::chrono::steady_clock::now() - started > timeout,
        "completed plugin preparation steps keep a healthy multi-plugin project alive beyond one control timeout")) {
        std::printf("  %s\n", loaded.message().c_str()); return;
    }
    append(6); packet.transport.clear(); packet.revision = 20;
    const auto updated = process.applySession(packet, 19);
    AudioProcessSnapshot snapshot;
    check(bool(updated) && bool(process.poll(snapshot)) && snapshot.revision == 20 &&
        process.lastPublication().plugins.size() == 12,
        "incremental batch loading uses the same progress-aware deadline and publishes every plugin");
    if (!updated) std::printf("  %s\n", updated.message().c_str());
}

void recoveryCheckpointEdits(const std::string& executable) {
    auto packet = session(); packet.blockSize = 1024;
    packet.session.hosting = {plugins::HostingMode::Isolated, DAW_PLUGIN_HOST_PATH};
    AudioPluginSpec slot;
    slot.id = "fault"; slot.uid = "com.daw.test.fault"; slot.name = "Fault fixture";
    slot.requiredFormat = slot.descriptor.format = plugins::Format::Clap;
    slot.descriptor.uid = slot.uid; slot.descriptor.name = slot.name; slot.descriptor.path = DAW_FAULT_CLAP_PATH;
    slot.channelMode = PluginChannelMode::DualMono; slot.preferredChannels = 1;
    slot.parameters = {{"0", 0}, {"1", .7}};
    slot.rightParameters = {{"0", 0}, {"1", .3}};
    AudioPluginChainSpec chain; chain.channelId = "source"; chain.slots.push_back(slot);
    packet.session.pluginChains.push_back(chain);
    AudioRuntimeProcess process(executable);
    if (!check(bool(process.replaceSession(packet)), "recovery checkpoint fixture creates isolated dual mono sides")) return;
    const AudioPluginAddress right{"source", "fault", true};
    const auto setRight = [&](const char* parameter, double value) {
        const auto [status, reply] = audio_rpc::call<audio_rpc::Method::setPluginParameter>(process, 0,
            {right, std::string(parameter), value});
        return bool(status) && std::get<0>(reply);
    };
    const auto play = [&] {
        AudioControlPacket command; command.generation = process.generation(); command.requestId = 1;
        command.command = AudioTransportCommand{AudioTransportCommand::Action::StartPlayback};
        return bool(process.send(command));
    };
    if (!check(play() && bool(process.advanceForTest(1024, 2)), "initial native values complete processing")) return;
    const auto [exactStatus, exact] = audio_rpc::call<audio_rpc::Method::capturePluginCheckpoints>(process, 0,
        {std::vector<AudioPluginCheckpoint>{}, AudioPluginCheckpointPurpose::Exact});
    const auto& exactCheckpoints = std::get<0>(std::get<1>(exact));
    if (!check(bool(exactStatus) && bool(std::get<0>(exact).result()) && exactCheckpoints.size() == 1 &&
        exactCheckpoints.front().left.hasState && exactCheckpoints.front().right && exactCheckpoints.front().right->hasState,
        "explicit exact capture establishes an opaque checkpoint for both native sides")) return;
    if (!check(setRight("1", .6) && bool(process.advanceForTest(1024)),
        "safe right-side edit is confirmed after the opaque checkpoint")) return;
    if (!check(setRight("0", 1) && setRight("1", .9) && bool(process.captureCheckpoint()),
        "recovery capture succeeds while a poisonous host edit is still queued")) return;
    check(!process.advanceForTest(1024), "queued fault really crashes its isolated side");
    const auto restoredAudio = [&] {
        AudioProcessSnapshot snapshot;
        if (!process.restart() || !process.poll(snapshot) || snapshot.transport.playing || !play() ||
            !process.advanceForTest(1024, 2) || !process.poll(snapshot)) return false;
        const auto meter = snapshot.meters.at("master");
        return std::abs(meter.left - .35f) < .003f && std::abs(meter.right - .3f) < .003f;
    };
    if (!check(restoredAudio(),
        "restart excludes unprocessed poison and preserves confirmed edits plus the healthy dual mono side")) return;
    check(setRight("0", 1) && !process.advanceForTest(1024), "recovered side can fail independently again");
    check(bool(process.captureCheckpoint()), "recovery capture accepts an already failed isolated side");
    check(restoredAudio(), "checkpoint after failure retains the same confirmed audible values");
}

void failedPluginEdit(const std::string& executable) {
    auto packet = session(); packet.blockSize = 1024;
    packet.session.hosting = {plugins::HostingMode::Isolated, DAW_PLUGIN_HOST_PATH};
    AudioPluginSpec slot;
    slot.id = "fault"; slot.uid = "com.daw.test.fault"; slot.name = "Fault fixture";
    slot.requiredFormat = slot.descriptor.format = plugins::Format::Clap;
    slot.descriptor.uid = slot.uid; slot.descriptor.name = slot.name; slot.descriptor.path = DAW_FAULT_CLAP_PATH;
    slot.parameters = {{"0", 0}, {"1", .7}};
    AudioPluginChainSpec chain; chain.channelId = "source"; chain.slots.push_back(slot);
    packet.session.pluginChains.push_back(chain);
    packet.session.channels.front().content.plugins = AudioContentSpec::PluginCurves{{"fault", "0", .125, {{0, .125}}}};
    AudioRuntimeProcess process(executable);
    if (!check(bool(process.replaceSession(packet)) && bool(process.captureCheckpoint()),
        "recovery journal captures a last-good external plugin checkpoint")) {
        std::printf("  %s\n", process.error().c_str()); return;
    }
    AudioControlPacket play; play.generation = 1; play.requestId = 1;
    play.command = AudioTransportCommand{AudioTransportCommand::Action::StartPlayback};
    check(bool(process.send(play)) && !process.advanceForTest(1024), "automation crashes only the isolated plugin process");
    packet.transport.clear(); packet.revision = 20; packet.session.graph.masterVolume = .8f;
    AudioProcessSnapshot snapshot;
    check(bool(process.applySession(packet, 19)) && bool(process.poll(snapshot)) && snapshot.revision == 20 && process.running(),
          "unrelated graph edits remain available when an external plugin has already failed");
    check(bool(process.restart()) && bool(process.poll(snapshot)) && snapshot.generation == 2 && !snapshot.transport.playing,
          "failed matching slot retains its last-good checkpoint for explicit process recovery");
    packet.generation = 2; packet.revision = 21; packet.session.pluginChains.clear();
    packet.session.channels.front().content.plugins.reset();
    check(bool(process.applySession(packet, 20)) && bool(process.restart()) && bool(process.poll(snapshot)) &&
          snapshot.generation == 3 && snapshot.revision == 21,
          "removing a checkpointed plugin cannot resurrect its stale checkpoint on restart");
}
#endif
} // namespace

int main(int argc, char** argv) {
    // Launching this test itself through SharedProcess is the unresponsive
    // worker fixture; it owns no DSP and only waits until its parent kills it.
    if (argc > 1 && std::string_view(argv[1]).starts_with("--vlt-mapping=")) {
        daw::plugins::ipc::SharedProcess process; std::string error;
        if (!process.attach(argc, argv, error)) return 3;
        for (;;) process.wait(1000);
    }
    if (argc != 2) return 2;
    processContract(argv[1]);
    pausedRestart(argv[1]);
    auditionSessions(argv[1]);
    incrementalSessions(argv[1]);
    transactionRecovery(argv[1]);
    transactionLifetimes(argv[1]);
    recordingSessionUpdates(argv[1]);
    recordingRestart(argv[1]);
#if defined(DAW_FAULT_CLAP_PATH)
    nestedPreparationDeadline(argv[1]);
    preparationDeadline(argv[1]);
    recoveryCheckpointEdits(argv[1]);
    failedPluginEdit(argv[1]);
#endif
    deadline(daw::platform::pathToUtf8(std::filesystem::absolute(daw::platform::pathFromUtf8(argv[0]))));
    return failures ? 1 : 0;
}
