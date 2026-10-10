#include "AudioRuntimeOwner.hpp"

#include <cmath>
#include <stdexcept>

#pragma push_macro("slots")
#undef slots

namespace daw {
namespace {
audio::Result unsupported(std::string message) {
    return audio::Result::fail(audio::EngineError::NotSupported, std::move(message));
}
template<class F> audio::Result guarded(F&& operation) {
    try { return operation(); }
    catch (const std::exception& error) {
        return audio::Result::fail(audio::EngineError::Unknown, error.what());
    }
}
}
AudioRuntimeOwner::AudioRuntimeOwner() : m_runtime(std::make_shared<AudioRuntime>()) {}
AudioRuntimeOwner::~AudioRuntimeOwner() = default;

audio::Result AudioRuntimeOwner::applySession(AudioSessionSpec session, bool reconfigure,
    std::span<const AudioPluginStateEdit> restores, std::span<const AudioPluginCheckpoint> checkpoints,
    const std::function<void()>& afterCommitBeforeRetire) {
    AudioSessionPublication publication;
    const auto result = m_runtime->applySession(std::move(session), reconfigure, restores, checkpoints, &publication, afterCommitBeforeRetire);
    if (result) m_imported = std::move(publication.imported);
    return result;
}
audio::Result AudioRuntimeOwner::replaceSession(AudioSessionSpec session,
    std::span<const AudioPluginStateEdit> restores, std::span<const AudioPluginCheckpoint> checkpoints,
    std::span<const AudioTransportCommand> transport, const std::function<void()>& beforeRetire) {
    return replacePreparedSession(std::move(session), m_runtime->sampleRate, m_runtime->bufferSize,
        m_runtime->preparation().offline, restores, checkpoints, transport, beforeRetire);
}
audio::Result AudioRuntimeOwner::replacePreparedSession(AudioSessionSpec value, double rate,
    std::uint32_t frames, bool offline, std::span<const AudioPluginStateEdit> restores,
    std::span<const AudioPluginCheckpoint> checkpoints, std::span<const AudioTransportCommand> transport,
    const std::function<void()>& beforeRetire) {
    if (!std::isfinite(rate) || rate < 1000 || rate > 768000 || !frames || frames > engine::kMaxBlockSize)
        return audio::Result::fail(audio::EngineError::InvalidArgument, "Invalid audio preparation format.");
    return guarded([&] {
        if (m_runtime->hasActiveCaptures() || m_runtime->transportSnapshot().recording)
            return unsupported("Cannot replace an active recording generation.");
        if (m_runtime->auditionDriven)
            return unsupported("Stop audition before replacing its runtime generation.");
        std::vector<AudioPluginStateEdit> retainedRestores(restores.begin(), restores.end());
        std::vector<AudioPluginCheckpoint> retainedCheckpoints(checkpoints.begin(), checkpoints.end());
        if (checkpoints.empty() && m_runtime->prepared &&
            (m_runtime->sampleRate != rate || m_runtime->bufferSize != frames || m_runtime->preparation().offline != offline)) {
            if (!m_runtime->pluginFaults().empty())
                return unsupported("Restore or remove failed plugins before changing the audio format.");
            if (const auto result = m_runtime->capturePluginCheckpoints(retainedCheckpoints); !result)
                return result;
            const auto compatible = [&](const AudioPluginAddress& address, const std::string& uid, plugins::Format format) {
                for (const auto& chain : value.pluginChains) if (chain.channelId == address.channelId)
                    for (const auto& slot : chain.slots) if (slot.id == address.slotId && slot.uid == uid &&
                        slot.requiredFormat == format && (!address.right || slot.channelMode == PluginChannelMode::DualMono)) return true;
                return false;
            };
            std::erase_if(retainedCheckpoints, [&](const auto& checkpoint) {
                return !compatible({checkpoint.channelId, checkpoint.slotId}, checkpoint.uid, checkpoint.format);
            });
            for (const auto& address : m_runtime->pluginAddresses()) {
                const auto* instance = m_runtime->pluginInstance(address);
                if (!instance || (instance->descriptor().uid != "daw.sampler" &&
                                  instance->descriptor().uid != "daw.slicer")) continue;
                const auto snapshot = m_runtime->pluginStateSnapshot(address);
                if (!snapshot.ownsSample || !compatible(address, snapshot.descriptor.uid, snapshot.descriptor.format) ||
                    std::any_of(retainedRestores.begin(), retainedRestores.end(), [&](const auto& edit) {
                        return edit.address.channelId == address.channelId && edit.address.slotId == address.slotId && edit.address.right == address.right;
                    })) continue;
                AudioPluginStateEdit edit;
                edit.address = address; edit.address.instance = 0;
                edit.state.state = snapshot.state; edit.state.source = snapshot.sample; edit.state.sourcePath = snapshot.samplePath;
                edit.parameters = snapshot.parameters;
                retainedRestores.push_back(std::move(edit));
            }
        }
        auto candidate = std::make_shared<AudioRuntime>();
        candidate->isRenderClone = m_runtime->isRenderClone;
        candidate->renderingPass = m_runtime->renderingPass;
        candidate->renderTapsPreFader = m_runtime->renderTapsPreFader;
        candidate->renderTapsAtSource = m_runtime->renderTapsAtSource;
        candidate->liveDeviceAllowed = m_runtime->liveDeviceAllowed;
        m_runtime->inheritAudioWorkers(*candidate);
        if (const auto result = candidate->prepare(rate, frames, offline); !result) return result;
        AudioSessionPublication publication;
        if (const auto result = candidate->applySession(std::move(value), false, retainedRestores, retainedCheckpoints, &publication); !result) return result;
        if (m_runtime->metronome) candidate->setMetronomeSample(m_runtime->metronome->sample());
        for (const auto& command : transport) candidate->transportCommand(command);
        // Prepare every plugin and its state before touching the running
        // device. Reuse the configured stream rather than reopening hardware
        // for every project; a failed candidate leaves the old session live.
        if (const auto result = candidate->takeDeviceFrom(*m_runtime); !result) return result;
        if (beforeRetire) try { beforeRetire(); }
        catch (const std::exception& error) {
            (void)m_runtime->takeDeviceFrom(*candidate);
            m_runtime->stopForFailedRollback();
            return audio::Result::fail(audio::EngineError::AudioThreadError,
                std::string("Could not retire the previous session editor: ") + error.what());
        }
        m_runtime = std::move(candidate);
        m_imported = std::move(publication.imported);
        return audio::Result::ok();
    });
}
audio::Result AudioRuntimeOwner::configureDevice(const audio::AudioDeviceConfig& requested, audio::AudioDeviceConfig& actual) {
    // DeviceManager can restore a different device/format even when it returns
    // an error. Keep every resulting stream stopped until the controller has
    // prepared that actual format and attached the corresponding callback.
    if (const auto detached = m_runtime->detachDeviceCallback(); !detached) return detached;
    const auto result = m_runtime->openDevice(requested);
    actual = m_runtime->deviceConfiguration();
    return result;
}
} // namespace daw
#pragma pop_macro("slots")
