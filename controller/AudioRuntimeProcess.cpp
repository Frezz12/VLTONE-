#include "AudioRuntimeProcess.hpp"
#include "AudioRuntimeProcessProtocol.hpp"
#include "AudioRuntime.hpp"
#include "AudioRuntimeRpc.hpp"
#include "AudioRuntimePluginFields.hpp"
#include "AudioRuntimeDurableEdits.hpp"
#include "Internal/ChannelColorInstance.hpp"
#include "Platform/PathUtils.hpp"
#include "SharedProcess.hpp"
#include "PluginProcess.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <new>
#include <random>
#include <thread>

namespace audio {
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AudioDeviceConfig, outputDeviceUid, inputDeviceUid,
    inputEnabled, sampleRate, bufferSize, inputChannelSelectors, outputChannelSelectors)
}
namespace daw {
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AudioTransportSnapshot, state, position, duration,
    loopStart, loopEnd, positionSeconds, presentationSeconds, tempo, playing, recording, loopEnabled)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AudioMeterSnapshot, left, right, hold)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AudioDeviceSnapshot, running, hasStream, inputUsesDeviceTime,
    state, sampleRate, bufferSize, callbacks, renderCalls, lastCallbackNs, lastRenderStatus, xruns)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AudioRuntimeDiagnostics, load, latency, lastRenderError,
    failedBlocks, gatedBlocks, droppedProfileEvents, workers, realtimeWorkers, workgroupWorkers)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AudioPluginAddress, channelId, slotId, right, instance)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AudioPluginNotice, kind, address, chain, clipId, parameterId, value, touch)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AudioPluginServiceResult, changed, scanned, notices, error)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AudioProcessSnapshot, sessionId, auditionSessionId, generation, revision, transport,
    device, runtime, configuration, meters, plugins)

namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using Process = plugins::ipc::SharedProcess;
using audioipc::Mailbox;
namespace fs = std::filesystem;
// Let the inner watchdog reject and reap its own plugin before the outer
// supervisor concludes that the audio process itself is unresponsive.
constexpr auto kNestedPluginBudget = plugins::kPluginControlTimeout +
    plugins::ipc::kProcessStopWait + plugins::kPluginCloseTimeout;

struct Failure : std::runtime_error {
    audio::EngineError code;
    Failure(audio::EngineError c, const std::string& message) : std::runtime_error(message), code(c) {}
};
void require(bool valid, const char* message) {
    if (!valid) throw Failure(audio::EngineError::InvalidArgument, message);
}
void require(const audio::Result& result) {
    if (!result) throw Failure(result.error(), result.message());
}
std::vector<std::uint8_t> readFile(const fs::path& path) {
    require(!fs::is_symlink(fs::symlink_status(path)) && fs::is_regular_file(path), "Missing audio process resource.");
    const auto size = fs::file_size(path);
    require(size <= kMaxAudioSessionBytes, "Audio process resource exceeds size limit.");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::ifstream stream(path, std::ios::binary);
    require(bool(stream.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(size))), "Cannot read audio process resource.");
    return bytes;
}
void writeFile(const fs::path& path, std::span<const std::uint8_t> bytes) {
    require(!fs::is_symlink(fs::symlink_status(path)), "Audio process resource is a symbolic link.");
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    stream.close();
    require(bool(stream), "Cannot write audio process resource.");
}
std::vector<std::uint8_t> valueBytes(const Json& value, const fs::path& path) {
    require(value.contains("value") != value.contains("file"), "Invalid audio value payload.");
    if (value.contains("value")) {
        const auto& bytes = value.at("value").get_binary();
        return {bytes.begin(), bytes.end()};
    }
    require(value.at("file").get<bool>(), "Invalid audio value file marker.");
    return readFile(path);
}
struct Directory {
    fs::path path;
    Directory() {
        std::random_device random;
        const auto parent = fs::weakly_canonical(fs::temp_directory_path());
        for (unsigned retry = 0; retry < 16; ++retry) {
            const auto candidate = parent / (".vlt-audio-" + std::to_string(random()) + "-" + std::to_string(random()));
            if (fs::create_directory(candidate)) { path = candidate; return; }
        }
        throw Failure(audio::EngineError::FileWriteError, "Cannot create private audio process directory.");
    }
    ~Directory() {
        // Only our own successfully-created directory is ever removed, after
        // child exit or generation retirement has released every PCM mapping.
        std::error_code ignored;
        if (!path.empty()) fs::remove_all(path, ignored);
    }
};
bool isStart(AudioTransportCommand::Action action) {
    using A = AudioTransportCommand::Action;
    return action == A::Play || action == A::StartPlayback || action == A::Record;
}
void requireIsolatedHosting(const AudioSessionPacket& packet) {
    if (packet.session.hosting.mode == plugins::HostingMode::Isolated) return;
    for (const auto& chain : packet.session.pluginChains)
        for (const auto& slot : chain.slots)
            require(slot.descriptor.format == plugins::Format::Internal ||
                slot.descriptor.format == plugins::Format::Unknown,
                "External plugins in the audio process require isolated hosting.");
}
std::string journalKey(const AudioControlPacket& packet) {
    return std::visit([](const auto& value) -> std::string {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, AudioTransportCommand>) {
            using A = AudioTransportCommand::Action;
            if (isStart(value.action) || value.action == A::Pause) return {};
            if (value.action == A::Seek || value.action == A::SeekSeconds || value.action == A::Stop) return "transport:position";
            return "transport:" + std::to_string(int(value.action));
        } else if constexpr (std::is_same_v<T, AudioMidiCommand>) return {}; // held notes never survive a crash
        else if constexpr (std::is_same_v<T, AudioPreviewCommand>) return {}; // audition never restarts by itself
        else if constexpr (std::is_same_v<T, AudioFaderCommand>)
            return Json::array({"fader", value.channelId, value.clipId, int(value.target)}).dump();
        else if constexpr (std::is_same_v<T, AudioInputCommand>) return Json::array({"input", value.channelId}).dump();
        else return Json::array({"send", value.channelId, value.sendId}).dump();
    }, packet.command);
}
void checkpointTransport(AudioSessionPacket& packet,
    const std::map<std::string, AudioControlPacket>& journal, const AudioTransportSnapshot& snapshot) {
    using A = AudioTransportCommand::Action;
    auto& transport = packet.transport;
    for (const auto& [key, packet] : journal)
        if (const auto* command = std::get_if<AudioTransportCommand>(&packet.command)) transport.push_back(*command);
    std::erase_if(transport, [](const auto& value) { return value.action != A::TimeSignature; });
    if (transport.size() > 1) transport.erase(transport.begin(), transport.end() - 1);
    AudioTransportCommand tempo; tempo.action = A::Tempo; tempo.value = snapshot.tempo;
    AudioTransportCommand duration; duration.action = A::Duration; duration.position = snapshot.duration;
    AudioTransportCommand loop; loop.action = A::LoopRange; loop.position = snapshot.loopStart; loop.end = snapshot.loopEnd;
    AudioTransportCommand enabled; enabled.action = A::LoopEnabled; enabled.enabled = snapshot.loopEnabled;
    AudioTransportCommand position; position.action = A::Seek; position.position = snapshot.position;
    transport.insert(transport.end(), {tempo, duration, loop, enabled, position});
}
const AudioPluginSpec* findSpec(const AudioSessionSpec& session, const std::string& channelId, const std::string& slotId) {
    for (const auto& chain : session.pluginChains) if (chain.channelId == channelId)
        for (const auto& spec : chain.slots) if (spec.id == slotId) return &spec;
    return nullptr;
}
bool sameStateContract(const AudioPluginSpec* before, const AudioPluginSpec* after) {
    return before && after && before->uid == after->uid && before->requiredFormat == after->requiredFormat &&
        (!after->requireExactVersion || (before->descriptor.version == after->requiredVersion &&
        (after->requiredParameterFingerprint.empty() ||
         before->descriptor.parameterFingerprint == after->requiredParameterFingerprint)));
}
std::vector<AudioPluginStateEdit> retainRestores(const AudioSessionPacket& previous,
    const AudioSessionSpec& next, const std::vector<AudioPluginStateEdit>& edits) {
    auto retained = edits;
    for (auto& edit : retained) edit.address.instance = 0;
    for (const auto& edit : previous.restores) {
        const auto& address = edit.address;
        const auto* before = findSpec(previous.session, address.channelId, address.slotId);
        const auto* after = findSpec(next, address.channelId, address.slotId);
        if (!sameStateContract(before, after) || (address.right && after->channelMode != PluginChannelMode::DualMono) ||
            after->miniModule || after->uid == plugins::channel_color::ChannelColorInstance::uid()) continue;
        if (std::none_of(edits.begin(), edits.end(), [&](const auto& value) {
            return value.address.channelId == address.channelId && value.address.slotId == address.slotId &&
                   value.address.right == address.right;
        })) retained.push_back(edit);
    }
    return retained;
}
std::vector<AudioPluginCheckpoint> retainCheckpoints(const AudioSessionPacket& previous, const AudioSessionSpec& next,
    std::span<const AudioPluginStateEdit> edits = {}) {
    const auto slot = [](const AudioSessionSpec& session, const AudioPluginCheckpoint& checkpoint) -> const AudioPluginSpec* {
        for (const auto& chain : session.pluginChains) if (chain.channelId == checkpoint.channelId)
            for (const auto& spec : chain.slots) if (spec.id == checkpoint.slotId) return &spec;
        return nullptr;
    };
    auto retained = previous.checkpoints;
    std::erase_if(retained, [&](const auto& checkpoint) {
        const auto* before = slot(previous.session, checkpoint);
        const auto* after = slot(next, checkpoint);
        if (!before || !after || before->uid != after->uid || after->uid != checkpoint.uid ||
            after->requiredFormat != checkpoint.format ||
            (after->channelMode == PluginChannelMode::DualMono) != checkpoint.right.has_value()) return true;
        // These document-authored built-ins reconstruct their complete state
        // from the latest spec. An older opaque checkpoint would undo the edit.
        if (after->miniModule || after->uid == plugins::channel_color::ChannelColorInstance::uid()) return true;
        return after->requireExactVersion &&
            (before->descriptor.version != after->requiredVersion ||
             (!after->requiredParameterFingerprint.empty() &&
              before->descriptor.parameterFingerprint != after->requiredParameterFingerprint));
    });
    for (auto& checkpoint : retained) for (const auto& edit : edits) {
        if (edit.address.channelId != checkpoint.channelId || edit.address.slotId != checkpoint.slotId) continue;
        auto& side = edit.address.right ? *checkpoint.right : checkpoint.left;
        side = {};
        side.hasState = !edit.state.state.empty();
        side.projectState = side.hasState;
        side.state = edit.state.state;
        side.parameters = edit.parameters;
    }
    return retained;
}
void applyControl(AudioRuntime& runtime, const AudioControlPacket& packet) {
    std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, AudioTransportCommand>) runtime.transportCommand(value);
        else if constexpr (std::is_same_v<T, AudioPreviewCommand>) runtime.previewCommand(value);
        else if constexpr (std::is_same_v<T, AudioFaderCommand>)
            require(runtime.setFader(value.channelId, value.target, value.change, value.clipId), "Unknown audio fader.");
        else if constexpr (std::is_same_v<T, AudioInputCommand>)
            require(runtime.setInput(value.channelId, value.input), "Unknown audio input.");
        else if constexpr (std::is_same_v<T, AudioSendCommand>)
            require(runtime.setSend(value.channelId, value.sendId, value.level, value.enabled), "Unknown audio send.");
        else if constexpr (std::is_same_v<T, AudioMidiCommand>)
            require(runtime.sendLiveMidi(value.channelId, value.event), "Audio MIDI queue rejected the event.");
    }, packet.command);
}
} // namespace

struct AudioRuntimeProcess::Impl {
    std::string executable, lastError;
    std::chrono::milliseconds timeout;
    std::shared_ptr<Process> process;
    struct Session {
        std::unique_ptr<Directory> resources;
        std::unique_ptr<ProcessAudioResources> registry;
        ProcessAudioResources::Cache valueCache;
        AudioSessionPacket packet;
        std::shared_ptr<const AudioSessionPublication> publication;
        AudioTransportSnapshot lastControl;
        AudioDeviceSnapshot lastDevice;
        std::map<std::string, AudioControlPacket> journal;
        audio_rpc::DurableEdits durable;
        audio::AudioDeviceConfig configuration;
        bool deviceEnabled = false;
        std::uint64_t controlSequence = 0;
    };
    std::map<std::uint64_t, Session> sessions;
    struct Transaction {
        std::uint64_t sessionId = 0, controlSequence = 0;
        AudioSessionPacket packet;
        std::map<std::string, AudioControlPacket> journal;
        audio_rpc::DurableEdits durable;
    };
    std::map<std::uint64_t, Transaction> transactions;
    std::uint64_t sequence = 0, nextSessionId = 0, nextTransactionId = 0;
    Session& session(std::uint64_t id) {
        const auto found = sessions.find(id);
        require(found != sessions.end(), "Audio process session does not exist.");
        return found->second;
    }
    const Transaction& transaction(std::uint64_t token, std::uint64_t id) {
        const auto found = transactions.find(token);
        require(found != transactions.end() && found->second.sessionId == id &&
            found->second.packet.generation == session(id).packet.generation,
            "Audio transaction belongs to another session or retired generation.");
        return found->second;
    }
    void retireTransactions(std::uint64_t id) {
        std::erase_if(transactions, [id](const auto& item) { return item.second.sessionId == id; });
    }

    Impl(std::string exe, std::chrono::milliseconds deadline) : executable(std::move(exe)), timeout(deadline) {
        require(!executable.empty() && timeout.count() > 0, "Invalid audio process configuration.");
    }
    void stop() {
        if (process) {
            process->stop(); // kill/reap every writer before retiring shared clocks
            auto& box = *reinterpret_cast<Mailbox*>(process->data());
            for (auto& slot : box.inputClocks) slot.generation.store(0, std::memory_order_release);
        }
        process.reset();
        transactions.clear();
        for (auto& [id, session] : sessions) {
            session.registry.reset();
            session.valueCache = {};
            session.resources.reset();
            session.controlSequence = 0;
        }
        sequence = 0;
    }
    void launch() {
        if (process && process->running()) return;
        stop();
        auto candidate = std::make_shared<Process>();
        std::string error;
        if (!candidate->create(sizeof(Mailbox), error)) throw Failure(audio::EngineError::Unknown, error);
        new (candidate->data()) Mailbox;
        if (!candidate->launch(executable, error)) throw Failure(audio::EngineError::Unknown, error);
        process = std::move(candidate);
    }
    Json exchange(const Json& request) {
        if (!process || !process->running()) {
            stop();
            throw Failure(audio::EngineError::NotInitialized, "Audio process is not running.");
        }
        auto bytes = Json::to_cbor(request);
        require(bytes.size() <= audioipc::kCapacity, "Audio process request exceeds size limit.");
        auto& box = *reinterpret_cast<Mailbox*>(process->data());
        const auto id = ++sequence;
        std::memcpy(box.input, bytes.data(), bytes.size());
        box.requestSize = std::uint32_t(bytes.size());
        auto nestedDeadline = box.pluginControlDeadlineNs.load(std::memory_order_acquire);
        auto progress = box.preparationProgress.load(std::memory_order_acquire);
        box.request.store(id, std::memory_order_release);
        process->signal();
        auto deadline = Clock::now() + timeout;
        const bool preparing = request.value("op", "") == "replace" || request.value("op", "") == "apply";
        for (;;) {
            const auto completed = box.response.load(std::memory_order_acquire);
            if (completed == id) break;
            if (completed > id || !process->running()) {
                stop();
                throw Failure(audio::EngineError::AudioThreadError, "Audio process exited before acknowledging the command.");
            }
            const auto now = Clock::now();
            // Read the lease before progress: clearing it follows the completed
            // reply tick, so acquire cannot observe a cleared lease and miss
            // that reply's progress publication.
            const auto nested = box.pluginControlDeadlineNs.load(std::memory_order_acquire);
            const auto completedSteps = box.preparationProgress.load(std::memory_order_acquire);
            const bool advanced = preparing && completedSteps != progress;
            if (advanced) {
                progress = completedSteps;
                deadline = now + timeout;
            }
            if (preparing && nested > 0 && (nested != nestedDeadline || advanced)) {
                const auto remaining = std::chrono::nanoseconds(std::max<std::int64_t>(0,
                    nested - engine::presentationNowNs()));
                // The parent caps each new real RPC itself. The same hung
                // operation cannot extend its deadline on subsequent polls.
                deadline = std::max(deadline, now + std::min(remaining,
                    std::chrono::duration_cast<std::chrono::nanoseconds>(kNestedPluginBudget)));
            }
            nestedDeadline = nested;
            if (now >= deadline) {
                stop();
                throw Failure(audio::EngineError::Timeout, "Audio process control deadline expired; its process was stopped.");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Json reply;
        try {
            require(box.responseSize > 0 && box.responseSize <= audioipc::kCapacity, "Invalid audio process reply size.");
            reply = Json::from_cbor(box.output, box.output + box.responseSize);
            require(reply.at("request").get<std::uint64_t>() == id, "Audio process reply belongs to another request.");
        } catch (...) { stop(); throw; }
        if (!reply.at("ok").get<bool>())
            throw Failure(audio::EngineError(reply.at("code").get<int>()), reply.at("error").get<std::string>());
        return reply.at("data");
    }
    template<class F> audio::Result guarded(F&& action) {
        try { action(); lastError.clear(); return audio::Result::ok(); }
        catch (const Failure& failure) { lastError = failure.what(); return audio::Result::fail(failure.code, lastError); }
        catch (const std::exception& failure) { lastError = failure.what(); return audio::Result::fail(audio::EngineError::InvalidArgument, lastError); }
    }
    void remember(Session& session, const AudioControlPacket& packet) {
        auto& journal = session.journal;
        const auto key = journalKey(packet);
        if (key.empty()) return;
        auto next = packet;
        const auto prior = journal.find(key);
        if (auto* fader = std::get_if<AudioFaderCommand>(&next.command); fader && prior != journal.end()) {
            const auto& previous = std::get<AudioFaderCommand>(prior->second.command).change;
            if (!fader->change.gain) fader->change.gain = previous.gain;
            if (!fader->change.pan) fader->change.pan = previous.pan;
            if (!fader->change.silent) fader->change.silent = previous.silent;
            if (!fader->change.mono) fader->change.mono = previous.mono;
        }
        journal.insert_or_assign(key, std::move(next));
    }
};

AudioRuntimeProcess::AudioRuntimeProcess(std::string executable, std::chrono::milliseconds timeout)
    : m(std::make_unique<Impl>(std::move(executable), timeout)) {}
AudioRuntimeProcess::~AudioRuntimeProcess() { m->stop(); }
void AudioRuntimeProcess::close() { m->stop(); }
bool AudioRuntimeProcess::running() {
    if (m->process && m->process->running()) return true;
    m->stop();
    return false;
}
std::uint64_t AudioRuntimeProcess::processId() const { return m->process ? m->process->processId() : 0; }
std::uint64_t AudioRuntimeProcess::generation(std::uint64_t id) const {
    const auto found = m->sessions.find(id);
    return found == m->sessions.end() ? 0 : found->second.packet.generation;
}
const std::string& AudioRuntimeProcess::error() const { return m->lastError; }
AudioInputClockReader AudioRuntimeProcess::inputClock(std::uint64_t id) const {
    const auto expected = generation(id);
    if (!m->process || !expected) return {};
    auto& box = *reinterpret_cast<const Mailbox*>(m->process->data());
    for (const auto& slot : box.inputClocks) {
        if (slot.generation.load(std::memory_order_acquire) == expected &&
            slot.sessionId.load(std::memory_order_relaxed) == id)
            return {std::shared_ptr<const audioipc::InputClockSlot>(m->process, &slot), id, expected};
    }
    return {};
}
const AudioSessionPublication& AudioRuntimeProcess::lastPublication(std::uint64_t id) const {
    const auto& publication = m->session(id).publication;
    require(bool(publication), "Audio session has no publication acknowledgement.");
    return *publication;
}
std::shared_ptr<const AudioSessionPublication> AudioRuntimeProcess::lastPublicationOwner(std::uint64_t id) const {
    return m->session(id).publication;
}
void AudioRuntimeProcess::retainSessionState(AudioSessionPacket& next, std::uint64_t id) const {
    const auto& previous = m->session(id).packet;
    auto checkpoints = retainCheckpoints(previous, next.session, next.restores);
    auto restores = retainRestores(previous, next.session, next.restores);
    next.checkpoints = std::move(checkpoints);
    next.restores = std::move(restores);
}
const AudioTransportSnapshot& AudioRuntimeProcess::lastControlSnapshot(std::uint64_t id) const {
    return m->session(id).lastControl;
}
const audio::AudioDeviceConfig& AudioRuntimeProcess::deviceConfiguration(std::uint64_t id) const {
    return m->session(id).configuration;
}
const AudioDeviceSnapshot& AudioRuntimeProcess::lastDeviceSnapshot(std::uint64_t id) const {
    return m->session(id).lastDevice;
}

audio::Result AudioRuntimeProcess::replaceSession(AudioSessionPacket packet, bool openDevice,
    audio::AudioDeviceConfig configuration, std::uint64_t id, bool retainUnprojectedControls) {
    return m->guarded([&] {
        require(id == 0 || !openDevice, "Secondary audio sessions cannot own a device.");
        require(packet.generation > generation(id), "Audio session generation is stale.");
        audio_rpc::DurableEdits retained;
        if (retainUnprojectedControls && m->sessions.contains(id)) {
            retained = m->session(id).durable;
            retained.projected(packet);
        }
        auto directory = std::make_unique<Directory>();
        auto registry = std::make_unique<ProcessAudioResources>(directory->path);
        const auto bytes = encodeAudioSession(packet, *registry);
        writeFile(directory->path / "session.bin", bytes);
        m->launch();
        const auto clockBefore = std::uint64_t(engine::presentationNowNs());
        const auto answer = m->exchange({{"op", "replace"}, {"sessionId", id}, {"directory", platform::pathToUtf8(directory->path)},
            {"generation", packet.generation}, {"device", openDevice}, {"configuration", configuration}});
        const auto clockAfter = std::uint64_t(engine::presentationNowNs());
        try {
            const auto childClock = answer.at("clockNs").get<std::uint64_t>();
            require(childClock >= clockBefore && childClock <= clockAfter,
                "Audio input clock does not share the native timestamp domain.");
        } catch (...) {
            // The replacement already committed. An invalid clock reply must
            // not leave a live child behind the parent's old session baseline.
            m->stop();
            throw;
        }
        require(answer.at("generation").get<std::uint64_t>() == packet.generation, "Audio process committed another generation.");
        std::shared_ptr<const AudioSessionPublication> publication;
        try {
            auto [value] = audio_value::decode<AudioSessionPublication>(readFile(directory->path / "publication.bin"));
            publication = std::make_shared<const AudioSessionPublication>(std::move(value));
        } catch (...) { m->stop(); throw; }
        // The server acknowledges only after destroying the previous runtime.
        // Keep its directory until that point, including when a native load fails.
        m->retireTransactions(id);
        auto& session = m->sessions[id];
        // RPC mappings belong to the retired directory even when the latest
        // reply had no PCM. Caller-owned snapshots keep their mappings alive.
        session.valueCache = {};
        session.resources = std::move(directory);
        session.registry = std::move(registry);
        session.packet = std::move(packet);
        session.publication = std::move(publication);
        session.configuration = answer.at("configuration").get<audio::AudioDeviceConfig>();
        session.lastControl = answer.at("transport").get<AudioTransportSnapshot>();
        session.lastDevice = answer.at("device").get<AudioDeviceSnapshot>();
        session.deviceEnabled = openDevice;
        session.journal.clear();
        session.durable.clear();
        session.controlSequence = 0;
        m->nextSessionId = std::max(m->nextSessionId, id);
        if (retainUnprojectedControls) {
            if (const auto restored = retained.replay(*this, id, session.packet); !restored) {
                m->stop();
                require(restored);
            }
        }
    });
}
audio::Result AudioRuntimeProcess::createSession(AudioSessionPacket packet, std::uint64_t& id) {
    return m->guarded([&] {
        require(running() && m->sessions.contains(0), "A primary audio session is required.");
        require(m->nextSessionId < std::numeric_limits<std::uint64_t>::max(), "Audio session ID overflow.");
        const auto next = ++m->nextSessionId;
        require(replaceSession(std::move(packet), false, {}, next));
        id = next;
    });
}
audio::Result AudioRuntimeProcess::applySession(AudioSessionPacket packet, std::uint64_t expectedRevision,
    std::uint64_t id, bool reconfigurePlugins) {
    return m->guarded([&] {
        auto& session = m->session(id);
        require(session.resources && session.registry, "Audio session resources are unavailable.");
        require(packet.generation == session.packet.generation && expectedRevision == session.packet.revision &&
            packet.revision > expectedRevision, "Audio session update generation or revision is stale.");
        require(packet.checkpoints.empty() && packet.transport.empty(), "Incremental audio updates cannot import native state or transport commands.");
        require(packet.sampleRate == session.packet.sampleRate && packet.blockSize == session.packet.blockSize &&
            packet.offline == session.packet.offline, "Audio format changes require a new generation.");
        const auto bytes = encodeAudioSession(packet, *session.registry);
        writeFile(session.resources->path / "session-update.bin", bytes);
        const auto answer = m->exchange({{"op", "apply"}, {"sessionId", id}, {"generation", generation(id)},
            {"expectedRevision", expectedRevision}, {"revision", packet.revision}, {"reconfigure", reconfigurePlugins}});
        try {
            const auto data = answer.get<AudioProcessSnapshot>();
            require(data.sessionId == id && data.generation == packet.generation && data.revision == packet.revision,
                "Audio update acknowledgement belongs to another session revision.");
            auto [publication] = audio_value::decode<AudioSessionPublication>(readFile(session.resources->path / "publication.bin"));
            auto publicationOwner = std::make_shared<const AudioSessionPublication>(std::move(publication));
            packet.transport = session.packet.transport;
            checkpointTransport(packet, session.journal, data.transport);
            packet.checkpoints = retainCheckpoints(session.packet, packet.session, packet.restores);
            packet.restores = retainRestores(session.packet, packet.session, packet.restores);
            session.durable.projected(packet);
            session.packet = std::move(packet);
            session.publication = std::move(publicationOwner);
            // Reconciled graph values supersede old fader/input/send commands.
            // Native checkpoints update only through explicit captureCheckpoint,
            // so a failed plugin never prevents unrelated topology edits.
            session.journal.clear();
        } catch (...) {
            // An invalid success reply leaves publication ambiguous. Preserve
            // acknowledged parent values and stop the child before recovery.
            m->stop();
            throw;
        }
    });
}
audio::Result AudioRuntimeProcess::closeSession(std::uint64_t id) {
    return m->guarded([&] {
        require(id != 0, "Close the audio process to retire its primary session.");
        const auto& session = m->session(id);
        m->exchange({{"op", "close"}, {"sessionId", id}, {"generation", session.packet.generation}});
        // The child detached its output and drained all readers before ack.
        m->retireTransactions(id);
        m->sessions.erase(id);
    });
}
audio::Result AudioRuntimeProcess::startAudition(std::uint64_t id) {
    return m->guarded([&] {
        require(id != 0, "A primary session cannot audition itself.");
        const auto& session = m->session(id);
        m->exchange({{"op", "audition"}, {"sessionId", id}, {"generation", session.packet.generation},
            {"primaryGeneration", generation()}});
    });
}
audio::Result AudioRuntimeProcess::stopAudition() {
    return m->guarded([&] { m->exchange({{"op", "stopAudition"}, {"sessionId", 0}, {"generation", generation()}}); });
}
audio::Result AudioRuntimeProcess::send(const AudioControlPacket& packet, std::uint64_t id) {
    return m->guarded([&] {
        auto& session = m->session(id);
        require(packet.generation == session.packet.generation, "Audio control generation is stale.");
        require(packet.requestId > session.controlSequence, "Audio control request is stale.");
        const auto answer = m->exchange({{"op", "control"}, {"sessionId", id}, {"value", Json::binary(encodeAudioControl(packet))}});
        try {
            const auto transport = answer.get<AudioTransportSnapshot>();
            m->remember(session, packet);
            session.lastControl = transport;
            session.controlSequence = packet.requestId;
        } catch (...) { m->stop(); throw; }
    });
}
audio::Result AudioRuntimeProcess::poll(AudioProcessSnapshot& snapshot, std::uint64_t id) {
    return m->guarded([&] {
        const auto& session = m->session(id);
        auto value = m->exchange({{"op", "snapshot"}, {"sessionId", id}, {"generation", session.packet.generation}}).get<AudioProcessSnapshot>();
        require(value.generation == generation(id) && value.sessionId == id, "Audio process snapshot belongs to another session generation.");
        snapshot = std::move(value);
    });
}
audio::Result AudioRuntimeProcess::advanceForTest(std::uint32_t frames, std::uint32_t blocks,
    std::uint64_t id, std::span<const float> inputChannels) {
    return m->guarded([&] {
        require(inputChannels.size() <= engine::kMaxChannels &&
            std::all_of(inputChannels.begin(), inputChannels.end(), [](float value) { return std::isfinite(value); }),
            "Invalid headless audio input.");
        m->exchange({{"op", "advance"}, {"sessionId", id}, {"generation", generation(id)},
            {"frames", frames}, {"blocks", blocks},
            {"input", std::vector<float>(inputChannels.begin(), inputChannels.end())}});
    });
}
audio::Result AudioRuntimeProcess::configureDevice(const audio::AudioDeviceConfig& requested,
    audio::AudioDeviceConfig& actual) {
    return m->guarded([&] {
        const auto answer = m->exchange({{"op", "configure"}, {"sessionId", 0}, {"generation", generation()}, {"configuration", requested}});
        auto next = answer.at("configuration").get<audio::AudioDeviceConfig>();
        m->session(0).lastDevice = answer.at("device").get<AudioDeviceSnapshot>();
        m->session(0).lastControl = answer.at("transport").get<AudioTransportSnapshot>();
        m->session(0).configuration = next;
        m->session(0).deviceEnabled = true;
        actual = std::move(next);
    });
}
audio::Result AudioRuntimeProcess::captureCheckpoint(std::uint64_t id) {
    return m->guarded([&] {
        auto& session = m->session(id);
        require(bool(session.resources), "Audio process has no checkpoint directory.");
        const auto data = m->exchange({{"op", "checkpoint"}, {"sessionId", id}, {"generation", generation(id)}});
        auto checkpoints = decodeAudioCheckpoints(readFile(session.resources->path / "checkpoint.bin"));
        const auto snapshot = data.get<AudioProcessSnapshot>();
        require(snapshot.generation == generation(id) && snapshot.sessionId == id, "Audio checkpoint belongs to another session generation.");
        // Capture timing along with opaque state. Other acknowledged edits
        // (faders/routing) are still replayed from the bounded, coalesced journal.
        checkpointTransport(session.packet, session.journal, snapshot.transport);
        session.packet.checkpoints = std::move(checkpoints);
        std::erase_if(session.journal, [](const auto& item) { return std::holds_alternative<AudioTransportCommand>(item.second.command); });
    });
}
audio::Result AudioRuntimeProcess::startDevice() {
    return m->guarded([&] {
        m->session(0).lastDevice = m->exchange({{"op", "startDevice"}, {"sessionId", 0}, {"generation", generation()}}).get<AudioDeviceSnapshot>();
    });
}

audio::Result AudioRuntimeProcess::stopDevice() {
    return m->guarded([&] {
        m->exchange({{"op", "stopDevice"}, {"sessionId", 0}, {"generation", generation()}});
    });
}
audio::Result AudioRuntimeProcess::detachDeviceCallback() {
    return m->guarded([&] {
        m->exchange({{"op", "detachDeviceCallback"}, {"sessionId", 0}, {"generation", generation()}});
    });
}
audio::Result AudioRuntimeProcess::closeDevice() {
    return m->guarded([&] {
        m->exchange({{"op", "closeDevice"}, {"sessionId", 0}, {"generation", generation()}});
        m->session(0).deviceEnabled = false;
    });
}
audio::Result AudioRuntimeProcess::invoke(std::uint32_t operation, const ValueEncoder& encode,
    const ValueDecoder& decode, std::uint64_t id) {
    return m->guarded([&] {
        auto& session = m->session(id);
        require(session.resources && session.registry && encode && decode,
            "Audio session value resources are unavailable.");
        const auto root = session.resources->path;
        const auto bytes = encode(*session.registry);
        require(bytes.size() <= kMaxAudioSessionBytes, "Audio value request exceeds size limit.");
        auto durable = session.durable.prepare(audio_rpc::Method(operation), bytes, root, session.packet);
        Json request{{"op", "invoke"}, {"sessionId", id}, {"generation", session.packet.generation},
            {"method", operation}};
        if (bytes.size() <= audioipc::kCapacity - 1024) request["value"] = Json::binary(bytes);
        else { writeFile(root / "value-request.bin", bytes); request["file"] = true; }
        const auto answer = m->exchange(request);
        try {
            // Keep mappings still owned by an editor across unrelated readout
            // calls, but do not make this cache the last owner of old displays.
            std::erase_if(session.valueCache.samples,
                [](const auto& item) { return item.second.use_count() == 1; });
            auto retained = session.valueCache.samples;
            const auto output = valueBytes(answer, root / "value-reply.bin");
            const bool accepted = audio_rpc::DurableEdits::accepted(durable, output,
                root / "runtime-values", session.valueCache);
            decode(output, root / "runtime-values", session.valueCache);
            for (auto& [key, sample] : retained)
                if (sample.use_count() > 1) session.valueCache.samples.try_emplace(key, std::move(sample));
            if (accepted && durable.source) {
                const auto requests = audio_rpc::DurableEdits::sourceRequests(durable, session.packet);
                auto [status, reply] = audio_rpc::call<audio_rpc::Method::pluginStateSnapshots>(*this, id, {requests});
                require(status);
                audio_rpc::DurableEdits::acceptSource(durable, std::get<0>(reply), session.packet);
            } else if (accepted) session.durable.acceptHost(std::move(durable));
            // A source setter has already mutated the child. If its postimage
            // cannot be retained, the catch stops it before returning failure;
            // the parent packet remains the last fully acknowledged baseline.
        } catch (...) { m->stop(); throw; }
    });
}

audio::Result AudioRuntimeProcess::captureTransaction(std::uint64_t& token, std::uint64_t id) {
    return m->guarded([&] {
        const auto& session = m->session(id);
        require(session.resources && session.registry, "Audio session resources are unavailable.");
        require(m->transactions.size() < audioipc::kMaxTransactions &&
            m->nextTransactionId != std::numeric_limits<std::uint64_t>::max(), "Audio transaction capacity exceeded.");
        const auto next = ++m->nextTransactionId;
        Impl::Transaction saved;
        saved.sessionId = id; saved.controlSequence = session.controlSequence;
        saved.packet = session.packet; saved.journal = session.journal;
        saved.durable = session.durable;
        // Allocate the parent lease before asking the child to pin graph owners.
        // The generation registry already owns every referenced PCM file.
        m->transactions.emplace(next, std::move(saved));
        try {
            const auto answer = m->exchange({{"op", "captureTransaction"}, {"sessionId", id},
                {"generation", generation(id)}, {"token", next}, {"revision", session.packet.revision},
                {"controlSequence", session.controlSequence}});
            try {
                require(answer.at("token").get<std::uint64_t>() == next &&
                    answer.at("generation").get<std::uint64_t>() == session.packet.generation &&
                    answer.at("revision").get<std::uint64_t>() == session.packet.revision &&
                    answer.at("controlSequence").get<std::uint64_t>() == session.controlSequence,
                    "Audio transaction capture acknowledgement is stale.");
                auto& value = m->transactions.at(next);
                checkpointTransport(value.packet, value.journal, answer.at("transport").get<AudioTransportSnapshot>());
                std::erase_if(value.journal, [](const auto& item) {
                    return std::holds_alternative<AudioTransportCommand>(item.second.command);
                });
            } catch (...) { m->stop(); throw; }
            token = next;
        } catch (...) { m->transactions.erase(next); throw; }
    });
}
audio::Result AudioRuntimeProcess::restoreTransaction(std::uint64_t token, std::uint64_t id) {
    return m->guarded([&] {
        // Restore does not consume its lease. Prepare copies before the child
        // commits, so allocation failure cannot split graph and recovery state.
        auto saved = m->transaction(token, id);
        Json answer;
        try {
            answer = m->exchange({{"op", "restoreTransaction"}, {"sessionId", id},
                {"generation", saved.packet.generation}, {"token", token}});
        } catch (const Failure& failure) {
            // A native rollback error may have changed part of the graph. Stop
            // that child before any later command can use ambiguous state.
            if (failure.code == audio::EngineError::AudioThreadError) m->stop();
            throw;
        }
        try {
            require(answer.at("token").get<std::uint64_t>() == token &&
                answer.at("generation").get<std::uint64_t>() == saved.packet.generation &&
                answer.at("revision").get<std::uint64_t>() == saved.packet.revision &&
                answer.at("controlSequence").get<std::uint64_t>() == saved.controlSequence,
                "Audio transaction restore acknowledgement is stale.");
        } catch (...) { m->stop(); throw; }
        auto& session = m->session(id);
        session.packet = std::move(saved.packet); session.journal = std::move(saved.journal);
        session.durable = std::move(saved.durable);
        session.controlSequence = saved.controlSequence;
    });
}
audio::Result AudioRuntimeProcess::releaseTransaction(std::uint64_t token, std::uint64_t id) {
    return m->guarded([&] {
        const auto generation = m->transaction(token, id).packet.generation;
        m->exchange({{"op", "releaseTransaction"}, {"sessionId", id}, {"generation", generation}, {"token", token}});
        m->transactions.erase(token);
    });
}
audio::Result AudioRuntimeProcess::restart() {
    return m->guarded([&] {
        require(m->sessions.contains(0), "Audio process has no acknowledged session to restart.");
        for (const auto& [id, session] : m->sessions)
            require(session.packet.generation != std::numeric_limits<std::uint64_t>::max(), "Audio generation overflow.");
        m->stop();
        auto saved = std::move(m->sessions);
        m->sessions.clear();
        try {
            // Ordered map restores primary first. A failed member stops the
            // whole child and retains the complete parent checkpoint family.
            for (const auto& [id, session] : saved) {
                auto next = session.packet;
                ++next.generation;
                std::erase_if(next.transport, [](const auto& command) {
                    return isStart(command.action) || command.action == AudioTransportCommand::Action::Pause;
                });
                // A new child has no capture writers. Rebuild a stopped graph
                // so private take players and suspended Clip FX rejoin output.
                for (auto& channel : next.session.graph.channels) channel.capturing = false;
                const auto edits = session.journal;
                require(replaceSession(std::move(next), session.deviceEnabled, session.configuration, id));
                require(session.durable.replay(*this, id, m->session(id).packet));
                std::uint64_t request = 0;
                for (const auto& [key, value] : edits) {
                    auto command = value;
                    command.generation = generation(id); command.requestId = ++request;
                    require(send(command, id));
                }
            }
        } catch (...) {
            m->stop();
            m->sessions = std::move(saved);
            throw;
        }
    });
}

namespace {
struct ServerSession {
    std::size_t inputClockSlot = audioipc::kInputClockSlots;
    std::shared_ptr<AudioRuntime> runtime;
    AudioSessionPacket packet;
    fs::path directory;
    ProcessAudioResources::Cache resources;
    std::unique_ptr<ProcessAudioResources> valueResources;
    std::uint64_t lastControl = 0;
    bool hardware = false, formatMatches = true;
    AudioPluginServiceResult pluginEvents;
    std::size_t pendingNoticeBytes = 0;
    struct Transaction {
        AudioRuntime::TransactionId runtimeToken = 0;
        AudioSessionPacket packet;
        ProcessAudioResources::Cache resources;
        std::uint64_t lastControl = 0;
        Json acknowledgement(std::uint64_t token) const {
            return {{"token", token}, {"generation", packet.generation},
                {"revision", packet.revision}, {"controlSequence", lastControl}};
        }
    };
    std::map<std::uint64_t, Transaction> transactions;
    void service() {
        try {
            auto next = runtime->servicePlugins();
            pluginEvents.changed |= next.changed; pluginEvents.scanned |= next.scanned;
            if (!next.error.empty()) pluginEvents.error = std::move(next.error);
            for (auto& notice : next.notices) {
                const auto bytes = sizeof(notice) + notice.address.channelId.size() + notice.address.slotId.size() +
                    notice.clipId.size() + notice.parameterId.size();
                if (bytes > 512u * 1024u - pendingNoticeBytes) {
                    pluginEvents.error = "Plugin notification queue exceeded its bounded capacity; refresh plugin state.";
                    continue;
                }
                pendingNoticeBytes += bytes;
                pluginEvents.notices.push_back(std::move(notice));
            }
        } catch (const std::exception& error) { pluginEvents.error = error.what(); }
    }
    AudioProcessSnapshot snapshot(std::uint64_t id, std::uint64_t auditionId) {
        AudioProcessSnapshot value;
        value.sessionId = id; value.auditionSessionId = auditionId;
        value.generation = packet.generation; value.revision = packet.revision;
        value.transport = runtime->transportSnapshot(); value.device = runtime->deviceSnapshot();
        value.runtime = runtime->diagnostics(); value.configuration = runtime->deviceConfiguration();
        value.meters.emplace(AudioGraphSpec::masterChannelId, runtime->meterSnapshot(AudioGraphSpec::masterChannelId));
        for (const auto& channel : packet.session.graph.channels)
            value.meters.emplace(channel.id, runtime->meterSnapshot(channel.id));
        value.plugins = pluginEvents;
        return value;
    }
    void requireGeneration(const Json& request) {
        require(request.at("generation").get<std::uint64_t>() == packet.generation,
            "Audio process request belongs to another generation.");
    }
};
struct Server {
    explicit Server(Mailbox& box) : mailbox(box) {}
    Mailbox& mailbox;
    void preparationProgress() {
        mailbox.preparationProgress.fetch_add(1, std::memory_order_release);
    }
    plugins::ScopedPluginControlProgress observePluginControl() {
        return {this, [](void* context, std::chrono::nanoseconds remaining, bool completed) noexcept {
            auto& server = *static_cast<Server*>(context);
            if (remaining.count() > 0) {
                const auto bounded = std::min(remaining,
                    std::chrono::duration_cast<std::chrono::nanoseconds>(plugins::kPluginControlTimeout));
                const auto cleanup = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    plugins::ipc::kProcessStopWait + plugins::kPluginCloseTimeout);
                server.mailbox.pluginControlDeadlineNs.store(engine::presentationNowNs() +
                    bounded.count() + cleanup.count(), std::memory_order_release);
            } else {
                if (completed) server.preparationProgress();
                server.mailbox.pluginControlDeadlineNs.store(0, std::memory_order_release);
            }
        }};
    }
    // Primary device closes before a secondary is destroyed. Audition's graph
    // holds its own runtime owner until a block-boundary detach completes.
    std::map<std::uint64_t, ServerSession> sessions;
    std::uint64_t lastSessionId = 0, auditionId = 0, lastTransactionId = 0;
    std::optional<std::uint64_t> drainPluginEvents;
    ~Server() {
        if (sessions.contains(0)) { sessions.at(0).runtime->closeDevice(); stopAudition(); }
        for (auto& [id, state] : sessions) unbindClock(state);
    }
    void unbindClock(ServerSession& state) {
        if (state.inputClockSlot == audioipc::kInputClockSlots) return;
        mailbox.inputClocks[state.inputClockSlot].generation.store(0, std::memory_order_release);
        state.runtime->bindInputClock(nullptr); // wait for its last audio writer before any slot reuse
        state.inputClockSlot = audioipc::kInputClockSlots;
    }
    std::size_t freeClockSlot() const {
        for (std::size_t i = 0; i < audioipc::kInputClockSlots; ++i)
            if (!mailbox.inputClocks[i].generation.load(std::memory_order_acquire)) return i;
        throw Failure(audio::EngineError::InvalidArgument, "Audio input clock capacity exceeded.");
    }
    ServerSession& session(std::uint64_t id) {
        const auto found = sessions.find(id);
        require(found != sessions.end(), "Audio process session does not exist.");
        return found->second;
    }
    void stopAudition() {
        if (!auditionId) return;
        session(0).runtime->stopAudition();
        auditionId = 0;
    }
    void service() { for (auto& [id, session] : sessions) session.service(); }
    void acknowledgeEvents() {
        if (!drainPluginEvents) return;
        auto& state = session(*drainPluginEvents);
        state.pluginEvents = {}; state.pendingNoticeBytes = 0;
    }
    Json replace(const Json& request, std::uint64_t id) {
        const auto pluginControl = observePluginControl();
        const auto found = sessions.find(id);
        auto* previous = found == sessions.end() ? nullptr : &found->second;
        const auto generation = request.at("generation").get<std::uint64_t>();
        require(generation > (previous ? previous->packet.generation : 0), "Audio session generation is stale.");
        require(id == 0 || sessions.contains(0), "A primary audio session is required.");
        require(previous || sessions.size() < 32, "Audio session capacity exceeded.");
        const auto clockSlot = previous ? previous->inputClockSlot : freeClockSlot();
        require(previous || id == 0 || id > lastSessionId, "Retired audio session ID cannot be reused.");
        require(!auditionId || (id != 0 && id != auditionId), "Stop audition before replacing its runtime generation.");
        require(!previous || (!previous->runtime->hasActiveCaptures() && !previous->runtime->transportSnapshot().recording),
            "Cannot replace an active recording generation.");
        const bool hardware = request.at("device").get<bool>();
        require(id == 0 || !hardware, "Secondary audio sessions cannot own a device.");
        const auto root = platform::pathFromUtf8(request.at("directory").get<std::string>());
        require(root.is_absolute() && fs::is_directory(root) && !fs::is_symlink(fs::symlink_status(root)),
            "Invalid audio session resource directory.");
        ProcessAudioResources::Cache resources;
        auto packet = decodeAudioSession(readFile(root / "session.bin"), root, &resources);
        requireIsolatedHosting(packet);
        require(packet.generation == generation, "Audio session envelope generation differs from its payload.");
        require(!hardware || !packet.offline, "An offline runtime cannot open a live audio device.");
        preparationProgress();
        auto candidate = std::make_shared<AudioRuntime>();
        require(candidate->prepare(packet.sampleRate, packet.blockSize, packet.offline));
        preparationProgress();
        require(candidate->applySession(packet.session, false, packet.restores, packet.checkpoints, nullptr,
            [&](const AudioSessionPublication& publication) {
                writeFile(root / "publication.bin", audio_value::encode(publication));
            }, [&] { preparationProgress(); }));
        preparationProgress();
        candidate->flushSamplerPrecompute(true);
        preparationProgress();
        const auto requested = request.at("configuration").get<audio::AudioDeviceConfig>();
        const bool oldRunning = previous && previous->runtime->deviceSnapshot().running;
        const auto oldConfig = previous ? previous->runtime->deviceConfiguration() : audio::AudioDeviceConfig{};
        if (previous) previous->runtime->closeDevice();
        try {
            if (hardware) {
                require(candidate->openDevice(requested));
                const auto actual = candidate->deviceConfiguration();
                if (actual.sampleRate != packet.sampleRate || actual.bufferSize > packet.blockSize)
                    throw Failure(audio::EngineError::SampleRateMismatch,
                        "Device format differs from projected session; configure it before publishing a new session.");
                require(candidate->startDevice());
            }
            for (const auto& command : packet.transport) {
                require(id == 0 || command.action != AudioTransportCommand::Action::Record,
                    "Secondary sessions cannot start recording.");
                candidate->transportCommand(command);
            }
        } catch (...) {
            candidate->closeDevice();
            if (previous && previous->hardware) {
                const auto restored = previous->runtime->openDevice(oldConfig);
                if (!restored || (oldRunning && !previous->runtime->startDevice()))
                    throw Failure(audio::EngineError::DeviceError, "New audio device failed and the previous device could not be restored.");
            }
            throw;
        }
        const auto configuration = candidate->deviceConfiguration();
        if (previous) unbindClock(*previous);
ServerSession next;
        next.inputClockSlot = clockSlot;
        next.runtime = std::move(candidate); next.packet = std::move(packet);
        next.directory = root; next.resources = std::move(resources); next.hardware = hardware;
        // Assignment releases the previous graph AND decoded packet mappings
        // before acknowledgment allows removal of the old parent directory.
        sessions.insert_or_assign(id, std::move(next));
        // New map entries allocate before the clock identity is published.
        // The preceding generation's writer has already drained and detached.
        auto& clock = mailbox.inputClocks[clockSlot];
        clock.generation.store(0, std::memory_order_release);
        clock.sessionId.store(id, std::memory_order_relaxed);
        sessions.at(id).runtime->bindInputClock(&clock.clock, &clock.presentation);
        clock.generation.store(generation, std::memory_order_release);
        lastSessionId = std::max(lastSessionId, id);
        return {{"generation", generation}, {"configuration", configuration},
            {"transport", sessions.at(id).runtime->transportSnapshot()}, {"device", sessions.at(id).runtime->deviceSnapshot()},
            {"clockNs", std::uint64_t(engine::presentationNowNs())}};
    }
    Json dispatch(const Json& request) {
        const auto op = request.at("op").get<std::string>();
        const auto id = request.at("sessionId").get<std::uint64_t>();
        if (op == "replace") return replace(request, id);
        auto& state = session(id);
        auto& runtime = *state.runtime;
        if (op == "control") {
            const auto& bytes = request.at("value").get_binary();
            auto packet = decodeAudioControl(bytes);
            require(packet.generation == state.packet.generation, "Audio control generation is stale.");
            require(packet.requestId > state.lastControl, "Audio control request is stale.");
            if (const auto* command = std::get_if<AudioTransportCommand>(&packet.command)) {
                require(id == 0 || command->action != AudioTransportCommand::Action::Record,
                    "Secondary sessions cannot start recording.");
                if (isStart(command->action)) {
                    require(id != 0 || !auditionId, "Stop audition before starting the primary transport.");
                    require(state.formatMatches && (!state.hardware || runtime.deviceSnapshot().running),
                        "Publish a session matching the configured device before playback or recording.");
                }
            }
            applyControl(runtime, packet);
            state.lastControl = packet.requestId;
            return runtime.transportSnapshot();
        }
        state.requireGeneration(request);
        if (op == "invoke") {
            const auto method = audio_rpc::Method(request.at("method").get<std::uint32_t>());
            require(id == 0 || method != audio_rpc::Method::startCapture,
                "Secondary sessions cannot start recording.");
            const auto bytes = valueBytes(request, state.directory / "value-request.bin");
            // The next serialized request acknowledges that the preceding
            // reply was decoded. Its immutable mappings survive file removal.
            if (state.valueResources) state.valueResources->collectExpired();
            if (!state.valueResources) {
                const auto directory = state.directory / "runtime-values";
                require(fs::create_directory(directory) ||
                    (!fs::is_symlink(fs::symlink_status(directory)) && fs::is_directory(directory)),
                    "Cannot create audio readout resources.");
                state.valueResources = std::make_unique<ProcessAudioResources>(directory,
                    ProcessAudioResources::Cancelled{}, ProcessAudioResources::Retention::WeakSources);
            }
            const auto result = audio_rpc::dispatch(runtime, method, bytes,
                state.directory, state.resources, *state.valueResources);
            if (result.size() <= audioipc::kCapacity - 1024) return {{"value", Json::binary(result)}};
            writeFile(state.directory / "value-reply.bin", result);
            return {{"file", true}};
        }
        if (op == "captureTransaction") {
            const auto token = request.at("token").get<std::uint64_t>();
            require(token > lastTransactionId, "Audio transaction token is stale.");
            std::size_t count = 0;
            for (const auto& [sessionId, current] : sessions) count += current.transactions.size();
            require(count < audioipc::kMaxTransactions, "Audio transaction capacity exceeded.");
            require(request.at("revision").get<std::uint64_t>() == state.packet.revision &&
                request.at("controlSequence").get<std::uint64_t>() == state.lastControl,
                "Audio transaction capture revision or sequence is stale.");
            require(!runtime.hasActiveCaptures() && !runtime.transportSnapshot().recording,
                "Cannot capture an audio transaction while recording.");
            ServerSession::Transaction saved;
            saved.packet = state.packet; saved.resources = state.resources; saved.lastControl = state.lastControl;
            const auto transport = runtime.transportSnapshot();
            checkpointTransport(saved.packet, {}, transport);
            auto answer = saved.acknowledgement(token);
            answer["transport"] = transport;
            saved.runtimeToken = runtime.captureTransaction();
            const auto runtimeToken = saved.runtimeToken;
            try { state.transactions.emplace(token, std::move(saved)); }
            catch (...) { runtime.releaseTransaction(runtimeToken); throw; }
            lastTransactionId = token;
            return answer;
        }
        if (op == "restoreTransaction" || op == "releaseTransaction") {
            const auto token = request.at("token").get<std::uint64_t>();
            const auto found = state.transactions.find(token);
            require(found != state.transactions.end() && found->second.packet.generation == state.packet.generation,
                "Audio transaction belongs to another session or retired generation.");
            const auto& saved = found->second;
            if (op == "releaseTransaction") {
                runtime.releaseTransaction(saved.runtimeToken);
                state.transactions.erase(found);
                return Json::object();
            }
            require(!runtime.hasActiveCaptures() && !runtime.transportSnapshot().recording,
                "Cannot restore an audio transaction while recording.");
            auto packet = saved.packet;
            auto resources = saved.resources;
            auto answer = saved.acknowledgement(token);
            if (const auto result = runtime.restoreTransaction(saved.runtimeToken); !result)
                throw Failure(audio::EngineError::AudioThreadError, "Audio transaction rollback failed: " + result.message());
            // No enclosing render gate exists on this process control thread.
            // Drop abandoned graph owners without retaining a restore history.
            runtime.collectTransactionRetirements(saved.runtimeToken);
            state.packet = std::move(packet); state.resources = std::move(resources); state.lastControl = saved.lastControl;
            // Delivered touches/values cannot be replayed safely. A fresh
            // readout is authoritative after rollback; discard aborted events.
            state.pluginEvents = {}; state.pendingNoticeBytes = 0;
            state.pluginEvents.changed = state.pluginEvents.scanned = true;
            return answer;
        }
        if (op == "apply") {
            const auto pluginControl = observePluginControl();
            require(request.at("expectedRevision").get<std::uint64_t>() == state.packet.revision,
                "Audio session update revision is stale.");
            auto resources = state.resources;
            auto packet = decodeAudioSession(readFile(state.directory / "session-update.bin"), state.directory, &resources);
            requireIsolatedHosting(packet);
            require(packet.generation == state.packet.generation && packet.revision > state.packet.revision &&
                packet.revision == request.at("revision").get<std::uint64_t>(), "Audio update envelope differs from its payload.");
            require(packet.sampleRate == state.packet.sampleRate && packet.blockSize == state.packet.blockSize &&
                packet.offline == state.packet.offline, "Audio format changes require a new generation.");
            require(packet.checkpoints.empty() && packet.transport.empty(),
                "Incremental audio updates cannot import native state or transport commands.");
            preparationProgress();
            // Recorders and their hardware-input routes are owned separately
            // from graph nodes. Reconciliation keeps the device, capture IDs,
            // publication list and format; its gate drains the current block.
            // This also supports preparing monitoring before publishCaptures.
            AudioProcessSnapshot snapshot;
            snapshot.sessionId = id; snapshot.auditionSessionId = auditionId;
            snapshot.generation = packet.generation; snapshot.revision = packet.revision;
            snapshot.transport = runtime.transportSnapshot();
            // Prepare the bounded acknowledgment and transport journal before
            // publishing. Only no-throw moves remain after the runtime commit.
            Json answer = snapshot;
            packet.transport = state.packet.transport;
            checkpointTransport(packet, {}, snapshot.transport);
            auto checkpoints = retainCheckpoints(state.packet, packet.session, packet.restores);
            auto restores = retainRestores(state.packet, packet.session, packet.restores);
            require(runtime.applySession(packet.session, request.value("reconfigure", false), packet.restores, {}, nullptr,
                [&](const AudioSessionPublication& publication) {
                    writeFile(state.directory / "publication.bin", audio_value::encode(publication));
                }, [&] { preparationProgress(); }));
            packet.checkpoints = std::move(checkpoints);
            packet.restores = std::move(restores);
            state.packet = std::move(packet); state.resources = std::move(resources);
            return answer;
        }
        if (op == "audition") {
            auto& primary = session(0);
            require(id != 0 && !auditionId, "Audio audition is already active or its target is invalid.");
            require(request.at("primaryGeneration").get<std::uint64_t>() == primary.packet.generation,
                "Audio audition belongs to another primary generation.");
            require(primary.formatMatches && (!primary.hardware || primary.runtime->deviceSnapshot().running),
                "Primary device format is unavailable for audition.");
            require(primary.runtime->startAudition(state.runtime));
            auditionId = id;
            return Json::object();
        }
        if (op == "stopAudition") {
            require(id == 0, "Only the primary session owns audition output.");
            stopAudition();
            return Json::object();
        }
        if (op == "close") {
            require(id != 0, "Primary audio session must be closed with its process.");
            if (auditionId == id) stopAudition();
            unbindClock(state);
            sessions.erase(id);
            return Json::object();
        }
        if (op == "snapshot") { drainPluginEvents = id; return state.snapshot(id, auditionId); }
        if (op == "checkpoint") {
            std::vector<AudioPluginCheckpoint> checkpoints;
            require(runtime.capturePluginCheckpoints(checkpoints, AudioPluginCheckpointPurpose::Recovery));
            writeFile(state.directory / "checkpoint.bin", encodeAudioCheckpoints(checkpoints));
            state.packet.checkpoints = std::move(checkpoints);
            return state.snapshot(id, auditionId);
        }
        if (op == "configure") {
            require(id == 0 && !auditionId, "Stop audition before changing the primary audio device.");
            require(state.transactions.empty(), "Release audio transactions before changing the audio device.");
            require(!runtime.hasActiveCaptures() && !runtime.transportSnapshot().recording, "Cannot change audio devices while recording.");
            const auto requested = request.at("configuration").get<audio::AudioDeviceConfig>();
            const auto previous = runtime.deviceConfiguration();
            const bool wasRunning = runtime.deviceSnapshot().running;
            runtime.transportCommand({AudioTransportCommand::Action::Pause});
            runtime.closeDevice();
            const auto opened = runtime.openDevice(requested);
            if (!opened) {
                if (state.hardware) { require(runtime.openDevice(previous)); if (wasRunning) require(runtime.startDevice()); }
                require(opened);
            }
            state.hardware = true;
            const auto actual = runtime.deviceConfiguration();
            state.formatMatches = actual.sampleRate == state.packet.sampleRate && actual.bufferSize <= state.packet.blockSize;
            return {{"configuration", actual}, {"device", runtime.deviceSnapshot()}, {"transport", runtime.transportSnapshot()}};
        }
        if (op == "stopDevice" || op == "detachDeviceCallback" || op == "closeDevice") {
            require(id == 0, "Only the primary session owns a device.");
            require(!runtime.hasActiveCaptures() && !runtime.transportSnapshot().recording,
                "Stop recording before closing or detaching the audio device.");
            if (op == "stopDevice") require(runtime.stopDevice());
            else if (op == "detachDeviceCallback") require(runtime.detachDeviceCallback());
            else { runtime.closeDevice(); state.hardware = false; }
            return Json::object();
        }
        if (op == "startDevice") {
            require(id == 0 && state.hardware && runtime.deviceSnapshot().hasStream,
                "The primary audio device has not been configured.");
            const auto actual = runtime.deviceConfiguration();
            require(state.formatMatches && actual.sampleRate == state.packet.sampleRate &&
                actual.bufferSize <= state.packet.blockSize,
                "Publish a session matching the configured device before starting it.");
            require(runtime.startDevice());
            return runtime.deviceSnapshot();
        }
        if (op == "advance") {
            require(!state.hardware && (id == 0 || id != auditionId),
                "Headless block driving cannot race a device or audition renderer.");
            const auto frames = request.at("frames").get<std::uint32_t>(), blocks = request.at("blocks").get<std::uint32_t>();
            require(frames > 0 && frames <= state.packet.blockSize && blocks > 0 && blocks <= 4096,
                "Invalid headless audio block count.");
            const auto levels = request.value("input", std::vector<float>{});
            require(levels.size() <= engine::kMaxChannels &&
                std::all_of(levels.begin(), levels.end(), [](float value) { return std::isfinite(value); }),
                "Invalid headless audio input.");
            audio::AudioBuffer output(2, frames); output.clear();
            std::optional<audio::AudioBuffer> input;
            if (!levels.empty()) {
                input.emplace(audio::ChannelCount(levels.size()), frames);
                for (std::size_t ch = 0; ch < levels.size(); ++ch)
                    std::fill_n(input->getChannel(ch), frames, levels[ch]);
            }
            audio::AudioCallbackContext context;
            context.outputBuffer = &output; context.numFrames = frames; context.sampleRate = state.packet.sampleRate;
            context.inputBuffer = input ? &*input : nullptr;
            for (std::uint32_t i = 0; i < blocks; ++i) {
                runtime.processDeviceBlock(context);
                require(context.renderStatus == audio::AudioCallbackContext::RenderStatus::Complete,
                    "Audio runtime failed to render the headless block.");
            }
            return Json::object();
        }
        throw Failure(audio::EngineError::NotSupported, "Unknown audio process operation.");
    }
};
} // namespace

int AudioRuntimeProcess::main(int argc, char** argv) {
    Process process;
    std::string error;
    if (!process.attach(argc, argv, error) || process.size() != sizeof(Mailbox)) return 2;
    auto& box = *reinterpret_cast<Mailbox*>(process.data());
    if (box.magic != audioipc::kMagic || box.version != audioipc::kVersion || box.bytes != sizeof(Mailbox)) return 3;
    Server server(box);
    std::uint64_t lastRequest = 0;
    for (;;) {
        const auto request = box.request.load(std::memory_order_acquire);
        if (request > lastRequest) {
            Json reply{{"request", request}};
            try {
                require(request == lastRequest + 1, "Audio mailbox request sequence is stale or skipped.");
                require(box.requestSize > 0 && box.requestSize <= audioipc::kCapacity, "Invalid audio process request size.");
                auto value = Json::from_cbor(box.input, box.input + box.requestSize);
                reply["data"] = server.dispatch(value); reply["ok"] = true;
            } catch (const Failure& failure) {
                reply["ok"] = false; reply["code"] = int(failure.code); reply["error"] = failure.what();
            } catch (const std::exception& failure) {
                reply["ok"] = false; reply["code"] = int(audio::EngineError::InvalidArgument); reply["error"] = failure.what();
            }
            auto bytes = Json::to_cbor(reply);
            if (bytes.size() > audioipc::kCapacity) {
                bytes = Json::to_cbor(Json{{"request", request}, {"ok", false}, {"code", int(audio::EngineError::OutOfMemory)},
                    {"error", "Audio process reply exceeds size limit."}});
            } else if (server.drainPluginEvents && reply.at("ok").get<bool>()) {
                server.acknowledgeEvents();
            }
            server.drainPluginEvents.reset();
            std::memcpy(box.output, bytes.data(), bytes.size()); box.responseSize = std::uint32_t(bytes.size());
            lastRequest = request;
            box.response.store(request, std::memory_order_release);
        }
        server.service();
        process.wait(10);
    }
}
} // namespace daw
