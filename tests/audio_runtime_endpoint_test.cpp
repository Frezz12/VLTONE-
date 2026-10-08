#include "AudioRuntimeEndpoint.hpp"
#include "platform/AudioFileDecoder.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

#include "process_test_utils.hpp"

namespace {
using namespace daw;
using namespace std::chrono_literals;
int failures = 0;
bool check(bool value, const char* message) {
    std::printf("%s %s\n", value ? "PASS" : "FAIL", message);
    std::fflush(stdout);
    failures += !value;
    return value;
}
template<class F> bool eventually(F&& condition) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    do {
        if (condition()) return true;
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}
AudioSessionSpec session() {
    AudioSessionSpec value;
    AudioGraphSpec::Channel channel;
    channel.id = "track"; channel.name = "Track";
    channel.input = {true, true, 0, 2, 3};
    value.graph.channels.push_back(channel);
    AudioPluginSpec plugin;
    plugin.id = "eq"; plugin.name = "Equalizer"; plugin.uid = "daw.equalizer";
    plugin.descriptor.uid = plugin.uid; plugin.descriptor.name = plugin.name;
    plugin.descriptor.format = plugin.requiredFormat = plugins::Format::Internal;
    AudioPluginChainSpec chain; chain.channelId = channel.id; chain.slots.push_back(plugin);
    value.pluginChains.push_back(std::move(chain));
    value.channels.push_back({channel.id, {}});
    return value;
}
using daw::test::killChild;
void pollingUnderControlLoad(const std::string& helper) {
    AudioRuntimeEndpoint endpoint(helper, 3s);
    if (!check(bool(endpoint.prepare(48000, 64)) && bool(endpoint.applySession(session())),
        "busy control fixture starts a production endpoint")) return;
    endpoint.transportCommand({.action = AudioTransportCommand::Action::Play});
    std::atomic<unsigned> commands{0};
    std::vector<std::jthread> readers;
    for (unsigned i = 0; i < 8; ++i) readers.emplace_back([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            (void)endpoint.pluginParameter({"track", "eq"}, "output.gain", AudioRuntimeEndpoint::Readout::Current);
            commands.fetch_add(1, std::memory_order_relaxed);
        }
    });
    bool observed = true;
    for (int block = 0; block < 4; ++block) {
        const auto before = endpoint.transportSnapshot().position;
        if (!endpoint.advanceForTest(64, 4)) { observed = false; break; }
        const auto deadline = std::chrono::steady_clock::now() + 500ms;
        while (endpoint.transportSnapshot().position <= before && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(5ms);
        observed &= endpoint.transportSnapshot().position > before;
    }
    for (auto& reader : readers) reader.request_stop();
    readers.clear();
    check(observed && commands.load() >= 8 && endpoint.metadata().connected,
        "continuous parameter commands cannot freeze transport polling");
}
void remote(const std::string& helper) {
    auto endpoint = std::make_shared<AudioRuntimeEndpoint>(helper, 3s);
    auto projection = session();
    if (!check(bool(endpoint->prepare(48000, 64)) && bool(endpoint->applySession(projection)),
               "production endpoint prepares its session in an actual child")) return;
    const auto child = endpoint->processId();
    check(child != 0 && endpoint->isRemote() && endpoint->metadata().connected,
          "UI endpoint owns a process connection without a native fallback");
    bool denied = false;
    try { (void)endpoint->nativeForWorkerOrTest(); } catch (const std::logic_error&) { denied = true; }
    check(denied, "production endpoint cannot return a native runtime pointer");
    const AudioPluginAddress eq{"track", "eq"};
    const auto oldIdentity = endpoint->pluginInstanceId(eq);
    check(oldIdentity && endpoint->hasPlugin(eq) && endpoint->hasChannel("track"),
          "acknowledged plugin metadata is available immediately after publication");

    endpoint->transportCommand({.action = AudioTransportCommand::Action::Play});
    check(endpoint->transportSnapshot().playing, "transport acknowledges Play before returning to the controller");
    const float input[] = {.25f, -.5f};
    check(bool(endpoint->advanceForTest(64, 16, input)), "child renders device blocks while the UI owns only values");
    check(eventually([&] { return endpoint->meterSnapshot("track").left > 0.01f; }),
          "batched meter values reach the UI cache");
    check(endpoint->inputBeatsAt(UINT64_MAX) > 0,
          "MIDI timing reads the current audio clock through shared memory");

    std::vector<PluginParameterReadout> readouts{{"output.gain", 0, 123, true}};
    AudioPluginAddress absent{"track", "absent"};
    endpoint->readPluginParameters(absent, readouts);
    check(!readouts.front().available, "an uncached or missing parameter clears stale caller availability");
    bool confirmedReadout = true;
    const AudioPluginAddress editor{"track", "eq", false, oldIdentity};
    for (int edit = 0; edit < 16; ++edit) {
        // Subscribe before the edit, including an address with a client instance token.
        (void)endpoint->pluginParameter(editor, "output.gain");
        const double requested = -double(edit + 1);
        confirmedReadout &= endpoint->setPluginParameter(editor, "output.gain", requested);
        const double confirmed = endpoint->pluginParameter(editor, "output.gain", AudioRuntimeEndpoint::Readout::Current);
        confirmedReadout &= confirmed == requested && endpoint->pluginParameter(editor, "output.gain") == confirmed;
    }
    check(confirmedReadout, "paint readout immediately retains each confirmed edit without another polling interval");
    check(endpoint->setPluginParameter(eq, "output.gain", -6), "parameter edit crosses the endpoint");
    check(bool(endpoint->advanceForTest(64, 3, input)) && bool(endpoint->captureRecoveryCheckpoint()),
          "completed processing can be checkpointed for engine recovery");
    check(eventually([&] { return std::abs(endpoint->pluginParameter(eq, "output.gain") + 6) < 1e-6; }),
          "parameter readouts eventually expose the acknowledged value");

    const auto transaction = endpoint->captureTransaction();
    auto changed = projection;
    changed.graph.channels.front().volume = .3f;
    check(bool(endpoint->applySession(changed)) && bool(endpoint->restoreTransaction(transaction)),
          "a numeric endpoint transaction restores an earlier projection");
    endpoint->releaseTransaction(transaction);
    auto invalid = projection;
    invalid.graph.channels.front().outputBusId = "track";
    check(!endpoint->applySession(invalid) && endpoint->pluginInstanceId(eq) == oldIdentity,
          "a rejected graph leaves the healthy plugin identity intact");

    endpoint->transportCommand({.action = AudioTransportCommand::Action::Pause});
    std::shared_ptr<AudioRuntimeEndpoint> secondary;
    check(bool(endpoint->createSecondary(secondary, session())) && secondary &&
          secondary->metadata().sessionId != 0 && secondary->processId() == child,
          "audition uses a numeric secondary session in the same audio process");
    if (secondary) {
        check(bool(endpoint->startAudition(secondary)) && bool(endpoint->advanceForTest(64, 4, input)),
              "primary callback drives the secondary session");
        endpoint->stopAudition();
        check(bool(secondary->closeSession()), "secondary closes after its renderer is detached");
    }

    check(killChild(child), "fixture terminates only its own audio child");
    check(eventually([&] { return !endpoint->metadata().connected; }),
          "a dead audio process invalidates cached playback without crashing the UI owner");
    check(!endpoint->transportSnapshot().playing, "a disconnected endpoint reports stopped playback");
    if (!check(bool(endpoint->restart()), "explicit recovery restarts the audio process")) return;
    check(endpoint->processId() != child && !endpoint->transportSnapshot().playing,
          "recovery creates a new process and does not start playback");
    const auto newIdentity = endpoint->pluginInstanceId(eq);
    check(newIdentity && newIdentity != oldIdentity,
          "plugin identities cannot alias the previous process generation");
    const bool staleRejected = !endpoint->setPluginParameter({"track", "eq", false, oldIdentity}, "output.gain", 0);
    check(staleRejected, "an editor handle from the dead generation cannot mutate a replacement plugin");
    check(eventually([&] { return std::abs(endpoint->pluginParameter(eq, "output.gain") + 6) < 1e-6; }),
          "whole-process recovery preserves the confirmed plugin state");
    const auto beforeFormat = endpoint->pluginInstanceId(eq);
    check(bool(endpoint->captureRecoveryCheckpoint()) &&
          bool(endpoint->replacePreparedSession(projection, 96000, 128, false)),
          "complete format replacement prepares a new session generation");
    check(endpoint->metadata().sampleRate == 96000 && endpoint->metadata().blockSize == 128 &&
          endpoint->pluginInstanceId(eq) != beforeFormat &&
          eventually([&] { return std::abs(endpoint->pluginParameter(eq, "output.gain") + 6) < 1e-6; }),
          "format change preserves confirmed plugin state with fresh instance identities");
}
void replacePluginState(const std::string& helper) {
    AudioRuntimeEndpoint endpoint(helper, 3s);
    auto projection = session();
    projection.pluginChains.front().slots.front().channelMode = PluginChannelMode::DualMono;
    if (!check(bool(endpoint.prepare(48000, 64)) && bool(endpoint.applySession(projection)),
        "state replacement fixture starts both mono sides")) return;
    const AudioPluginAddress left{"track", "eq"}, right{"track", "eq", true};
    check(endpoint.setPluginParameter(left, "output.gain", -6) &&
          endpoint.setPluginParameter(right, "output.gain", -3) &&
          bool(endpoint.advanceForTest(64, 4)), "mono sides have distinct completed states");
    const auto saved = endpoint.pluginStateSnapshot(left);
    const auto leftId = endpoint.pluginInstanceId(left), rightId = endpoint.pluginInstanceId(right);
    AudioPluginStateRestore restore;
    restore.state = {'b', 'a', 'd'};
    std::vector<InsertParameter> parameters;
    check(!endpoint.restorePluginState({"track", "eq", false, leftId}, restore, parameters) &&
          endpoint.pluginInstanceId(left) == leftId && endpoint.pluginInstanceId(right) == rightId,
          "a rejected native state leaves both healthy mono processors intact");
    restore.state = saved.state;
    check(endpoint.setPluginParameter(left, "output.gain", -9) && bool(endpoint.advanceForTest(64, 4)) &&
          bool(endpoint.restorePluginState({"track", "eq", false, leftId}, restore, parameters)),
          "native state is prepared on a fresh processor before publication");
    check(endpoint.pluginInstanceId(left) != leftId && endpoint.pluginInstanceId(right) == rightId &&
          std::abs(endpoint.pluginParameter(left, "output.gain", AudioRuntimeEndpoint::Readout::Current) + 6) < 1e-6,
          "state replacement retires only its selected mono side");
    check(!endpoint.restorePluginState({"track", "eq", false, leftId}, restore, parameters),
          "an obsolete state editor cannot replace the new instance");
    check(killChild(endpoint.processId()) && eventually([&] { return !endpoint.metadata().connected; }) &&
          bool(endpoint.restart()) &&
          std::abs(endpoint.pluginParameter(left, "output.gain", AudioRuntimeEndpoint::Readout::Current) + 6) < 1e-6 &&
          std::abs(endpoint.pluginParameter(right, "output.gain", AudioRuntimeEndpoint::Readout::Current) + 3) < 1e-6,
          "recovery retains the imported side and the untouched opposite state");
}
void interruptedCapture(const std::string& helper) {
    const auto directory = std::filesystem::temp_directory_path() /
        ("vlt-endpoint-capture-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if (!std::filesystem::create_directory(directory)) throw std::runtime_error("Cannot create test capture directory");
    struct Cleanup { std::filesystem::path directory; ~Cleanup() { std::error_code error; std::filesystem::remove_all(directory, error); } } cleanup{directory};
    AudioRuntimeEndpoint endpoint(helper, 3s);
    if (!check(bool(endpoint.prepare(48000, 64)) && bool(endpoint.applySession(session())), "capture fixture starts the audio process")) return;
    AudioCaptureSpec spec; spec.directory = directory.string(); spec.channelCount = 2;
    AudioCaptureStarted capture;
    const auto started = endpoint.startCapture(spec, capture);
    if (!check(bool(started) && capture.id, "capture acknowledges its exact file before publication")) return;
    check(endpoint.publishCaptures(std::span(&capture.id, 1)), "capture is published to the child callback");
    const float input[] = {.125f, -.375f};
    check(bool(endpoint.advanceForTest(64, 512, input)), "child records the hardware input fixture");
    check(eventually([&] { return std::filesystem::file_size(capture.path) > 100000; }), "writer puts audio data on disk before the crash");
    check(killChild(endpoint.processId()) && eventually([&] { return !endpoint.metadata().connected; }), "recording process terminates independently of its client");
    check(!endpoint.restart(), "recovery cannot discard an unfinalized interrupted capture");
    endpoint.transportCommand({.action = AudioTransportCommand::Action::Stop});
    check(!endpoint.setPluginParameter({"track", "eq"}, "output.gain", -3), "commands to a dead child fail without throwing into GUI callbacks");
    audio::RecordingSession closed;
    const auto unavailable = directory / "temporarily-unavailable.wav";
    std::filesystem::rename(capture.path, unavailable);
    check(!endpoint.stopCapture(capture.id, closed) && endpoint.hasActiveCaptures() && !endpoint.restart(),
          "failed file repair retains the capture token and blocks destructive recovery");
    std::filesystem::rename(unavailable, capture.path);
    const auto finalized = endpoint.stopCapture(capture.id, closed);
    check(bool(finalized) && closed.fileWriteSucceeded && closed.interrupted && closed.writtenFrames > 0 &&
          closed.writtenFrames <= 32768 && !endpoint.hasActiveCaptures(), "client repairs only the complete disk prefix after the writer exits");
    audio::platform::DecodedAudio wave;
    const auto decoded = audio::platform::decodeAudioFile(closed.filePath, wave);
    bool exact = bool(decoded) && wave.frames == closed.writtenFrames && wave.channels == 2;
    if (exact) for (std::size_t i = 0; i < wave.interleaved.size(); i += 2)
        exact &= wave.interleaved[i] == input[0] && wave.interleaved[i + 1] == input[1];
    check(exact, "repaired WAV decodes with the original input samples and channel order");
    auto wrong = closed; wrong.sampleRate = 96000;
    audio::RecordingSession rejected;
    check(!audio::AudioRecorder::recoverInterruptedFile(wrong, rejected), "repair rejects a file whose format differs from the acknowledged capture");
    { std::ofstream tail(closed.filePath, std::ios::binary | std::ios::app); tail.write("xyz", 3); }
    audio::RecordingSession prefix;
    check(bool(audio::AudioRecorder::recoverInterruptedFile(closed, prefix)) && prefix.writtenFrames == closed.writtenFrames,
          "repair ignores an incomplete trailing interleaved frame");
    auto gap = closed;
    gap.capturedFrames += 8; gap.droppedFrames = 8;
    check(bool(audio::AudioRecorder::recoverInterruptedFile(gap, prefix)) && prefix.droppedFrames == 8,
          "repair does not count an already acknowledged recording gap twice");
    check(bool(endpoint.restart()), "engine can restart after the interrupted take is safely finalized");
    AudioCaptureStarted next;
    check(bool(endpoint.startCapture(spec, next)) && endpoint.publishCaptures(std::span(&next.id, 1)) &&
          bool(endpoint.advanceForTest(64, 8, input)), "new recording starts after recovery");
    audio::RecordingSession completed;
    check(bool(endpoint.stopCapture(next.id, completed)) && completed.state == audio::RecordingSession::State::Stopped &&
          completed.fileWriteSucceeded && completed.writtenFrames == 512 && !endpoint.hasActiveCaptures(),
          "normal stop acknowledges a finalized WAV and retires its endpoint token");
    check(bool(endpoint.restart()), "a normally stopped take does not prevent a later engine restart");
}
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try {
        // Construction/destruction must be safe even if initialization never ran.
        { AudioRuntimeEndpoint unopened(argv[1]); unopened.closeDevice(); }
        remote(argv[1]);
        pollingUnderControlLoad(argv[1]);
        replacePluginState(argv[1]);
        interruptedCapture(argv[1]);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL endpoint exception: %s\n", error.what()); ++failures;
    }
    return failures ? 1 : 0;
}
