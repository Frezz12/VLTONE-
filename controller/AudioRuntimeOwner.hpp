#pragma once

#include "AudioRuntime.hpp"
#include <stdexcept>

namespace daw {

/// Owns the local runtime across transactional project/device-format changes.
/// Ordinary operations call AudioRuntime directly; no transport, RPC mirrors or
/// background polling sit between the controller and the audio state.
class AudioRuntimeOwner final {
public:
    AudioRuntimeOwner();
    ~AudioRuntimeOwner();
    AudioRuntimeOwner(const AudioRuntimeOwner&) = delete;
    AudioRuntimeOwner& operator=(const AudioRuntimeOwner&) = delete;
    AudioRuntime* operator->() const noexcept { return m_runtime.get(); }
    AudioRuntime& get() const noexcept { return *m_runtime; }
    std::shared_ptr<AudioRuntime> sharedRuntime() const { return m_runtime; }

    audio::Result applySession(AudioSessionSpec session, bool reconfigurePlugins = false,
        std::span<const AudioPluginStateEdit> restores = {},
        std::span<const AudioPluginCheckpoint> checkpoints = {},
        const std::function<void()>& afterCommitBeforeRetire = {});
    audio::Result replaceSession(AudioSessionSpec session,
        std::span<const AudioPluginStateEdit> restores = {},
        std::span<const AudioPluginCheckpoint> checkpoints = {},
        std::span<const AudioTransportCommand> transport = {},
        const std::function<void()>& beforeRetire = {});
    audio::Result replacePreparedSession(AudioSessionSpec session, double sampleRate,
        std::uint32_t blockSize, bool offline,
        std::span<const AudioPluginStateEdit> restores = {},
        std::span<const AudioPluginCheckpoint> checkpoints = {},
        std::span<const AudioTransportCommand> transport = {},
        const std::function<void()>& beforeRetire = {});
    const std::vector<AudioPluginStateSnapshot>& lastImportedPluginStates() const noexcept { return m_imported; }
    audio::Result configureDevice(const audio::AudioDeviceConfig& requested, audio::AudioDeviceConfig& actual);

private:
    std::shared_ptr<AudioRuntime> m_runtime;
    std::vector<AudioPluginStateSnapshot> m_imported;
};
} // namespace daw
