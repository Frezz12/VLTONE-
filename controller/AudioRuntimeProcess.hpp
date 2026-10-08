#pragma once

#include "AudioSessionCodec.hpp"
#include "AudioInputClock.hpp"
#include "AudioRuntimePluginService.hpp"
#include "Core/Result.hpp"

#include <chrono>
#include <functional>
#include <memory>

namespace daw {

struct AudioProcessSnapshot {
    std::uint64_t sessionId = 0, auditionSessionId = 0;
    std::uint64_t generation = 0, revision = 0;
    AudioTransportSnapshot transport;
    AudioDeviceSnapshot device;
    AudioRuntimeDiagnostics runtime;
    audio::AudioDeviceConfig configuration;
    std::unordered_map<std::string, AudioMeterSnapshot> meters;
    /// Notifications collected since the preceding successful poll. The
    /// child keeps them across checkpoint requests and failed replies.
    AudioPluginServiceResult plugins;
};

/// Control-thread client. It owns session values and OS resources, never a DSP,
/// audio device or native plugin. The child renders directly to its device;
/// UI polling is unrelated to audio block delivery.
class AudioRuntimeProcess final {
public:
    explicit AudioRuntimeProcess(std::string executable,
        std::chrono::milliseconds timeout = std::chrono::seconds(10));
    ~AudioRuntimeProcess();
    AudioRuntimeProcess(const AudioRuntimeProcess&) = delete;
    AudioRuntimeProcess& operator=(const AudioRuntimeProcess&) = delete;
    audio::Result replaceSession(AudioSessionPacket packet, bool openDevice = false,
        audio::AudioDeviceConfig configuration = {}, std::uint64_t sessionId = 0,
        bool retainUnprojectedControls = false);
    /// Reconcile values within a generation. The packet carries no transport
    /// commands or imported checkpoints. Optional restores target only newly
    /// prepared plugin sides and finish before publication. Compatible last-good
    /// state and PCM identities persist across these updates.
    audio::Result applySession(AudioSessionPacket packet, std::uint64_t expectedRevision,
        std::uint64_t sessionId = 0, bool reconfigurePlugins = false);
    const AudioSessionPublication& lastPublication(std::uint64_t sessionId = 0) const;
    std::shared_ptr<const AudioSessionPublication> lastPublicationOwner(std::uint64_t sessionId = 0) const;
    /// Broker-thread helpers for a complete new-format projection. Only state
    /// with a compatible plugin identity survives; caller's values win.
    void retainSessionState(AudioSessionPacket& next, std::uint64_t sessionId = 0) const;
    const AudioTransportSnapshot& lastControlSnapshot(std::uint64_t sessionId = 0) const;
    const audio::AudioDeviceConfig& deviceConfiguration(std::uint64_t sessionId = 0) const;
    const AudioDeviceSnapshot& lastDeviceSnapshot(std::uint64_t sessionId = 0) const;
    /// Secondary sessions run in the same child and cannot open hardware.
    /// Their numeric IDs are never reused during this client's lifetime.
    audio::Result createSession(AudioSessionPacket packet, std::uint64_t& sessionId);
    audio::Result closeSession(std::uint64_t sessionId);
    audio::Result startAudition(std::uint64_t sessionId);
    audio::Result stopAudition();
    audio::Result send(const AudioControlPacket& command, std::uint64_t sessionId = 0);
    audio::Result poll(AudioProcessSnapshot& snapshot, std::uint64_t sessionId = 0);
    /// Broker-thread acquisition after a session acknowledgment. Retain this
    /// reader on MIDI/UI threads; reads need no broker lock or IPC request.
    AudioInputClockReader inputClock(std::uint64_t sessionId = 0) const;
    audio::Result captureCheckpoint(std::uint64_t sessionId = 0);
    /// Typed adapters encode/decode locally; only bounded values and resource
    /// IDs cross the mailbox. Callbacks never run on the audio thread.
    /// A bounded whitelist retains acknowledged host controls and complete
    /// built-in source postimages. Other native state enters a confirmed
    /// checkpoint; graph/content publication uses applySession values.
    using ValueEncoder = std::function<std::vector<std::uint8_t>(ProcessAudioResources&)>;
    using ValueDecoder = std::function<void(std::span<const std::uint8_t>,
        const std::filesystem::path&, ProcessAudioResources::Cache&)>;
    audio::Result invoke(std::uint32_t operation, const ValueEncoder& encode,
        const ValueDecoder& decode, std::uint64_t sessionId = 0);
    /// Control-thread leases retain runtime graph owners and the matching
    /// recovery journal within one generation. Restore is repeatable and does
    /// not rewind live transport; release, replacement or process exit retires
    /// the token. Device reconfiguration requires releasing outstanding leases.
    audio::Result captureTransaction(std::uint64_t& token, std::uint64_t sessionId = 0);
    audio::Result restoreTransaction(std::uint64_t token, std::uint64_t sessionId = 0);
    audio::Result releaseTransaction(std::uint64_t token, std::uint64_t sessionId = 0);
    /// Opens a stopped device. A changed actual sample rate requires a new
    /// projected session; its old sample-domain placements are never replayed
    /// at a different rate. Caller publishes the matching session explicitly.
    audio::Result configureDevice(const audio::AudioDeviceConfig& requested,
        audio::AudioDeviceConfig& actual);
    /// Start the already configured primary device when its actual format
    /// matches the current projection. Transport remains under caller control.
    audio::Result startDevice();
    audio::Result stopDevice();
    audio::Result detachDeviceCallback();
    audio::Result closeDevice();
    /// Restart from the last acknowledged checkpoint and control edits.
    /// Playback and capture remain stopped until the user starts them again.
    audio::Result restart();
    void close();
    bool running();
    std::uint64_t processId() const;
    std::uint64_t generation(std::uint64_t sessionId = 0) const;
    const std::string& error() const;
    /// Headless process fixture only; refuses a runtime that opened hardware.
    /// It drives the same callback with optional constant input per channel.
    audio::Result advanceForTest(std::uint32_t frames, std::uint32_t blocks = 1,
        std::uint64_t sessionId = 0, std::span<const float> inputChannels = {});
    static int main(int argc, char** argv);
private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace daw
