#include "EngineController.hpp"
#include "platform/Log.hpp"
#include "model/MiniModules.hpp"

#include <algorithm>
#include <utility>

namespace daw {
namespace {
std::string recoveryKey(const std::string& channel, const std::string& slot) {
    return channel + "/" + slot;
}
}

bool EngineController::applyChannelSnapshot(const std::string& channelId,
    const ChannelSnapshot& state, bool includeRouting) {
    auto* modules = miniModulesFor(m_project, channelId);
    auto* track = m_project.findTrack(channelId);
    const bool master = channelId == kMasterChannelId;
    if (!modules || (!master && !track) || state.miniModules.size() > plugins::mini::kMaxModules) return false;
    const auto previous = m_project;
    modules->clear();
    for (const auto& slot : state.miniModules) modules->push_back(slot.model);
    if (master) {
        m_project.masterVolume = std::clamp(state.volume, 0.f, 2.f);
        m_project.masterPan = std::clamp(state.pan, -1.f, 1.f);
    } else {
        track->freeze = {};
        track->volume = std::clamp(state.volume, 0.f, 2.f);
        track->pan = std::clamp(state.pan, -1.f, 1.f);
        if (includeRouting) {
            track->muted = state.muted; track->soloed = state.soloed; track->mono = state.mono;
            track->outputBusId = state.outputBusId != channelId && m_project.findTrack(state.outputBusId)
                ? state.outputBusId : std::string{};
            track->sends.clear();
            for (const auto& send : state.sends)
                if (send.destinationTrackId != channelId && m_project.findTrack(send.destinationTrackId))
                    track->sends.push_back(send);
        }
    }
    const ChainReplacement replacement{channelId, state.inserts};
    if (!applyChains(std::span(&replacement, 1))) { m_project = previous; return false; }
    if (auto* groups = mutableRackGroups(channelId)) *groups = state.rackGroups;
    syncAllTrackGains();
    return true;
}

void EngineController::publishAudioFailure(AudioFailureNotice notice) {
    notice.project = m_projectGeneration;
    if (!notice.incident) notice.incident = ++m_audioIncidentSequence;
    const auto key = std::to_string(int(notice.source)) + "/" + recoveryKey(notice.address.channelId, notice.address.slotId) + "/" + notice.operation;
    if (notice.source == AudioFailureNotice::Source::Overload && notice.state != AudioFailureNotice::State::Retired)
        m_audioOverloadNotices[key] = notice;
    const auto transition = std::pair{notice.incident, notice.state};
    const auto previous = m_audioNoticeTransitions.find(key);
    if (previous == m_audioNoticeTransitions.end() || previous->second != transition) {
        DAW_LOG_INFO("audio-health project=%llu instance=%llu source=%d state=%d operation=%s plugin=%s detail=%s",
            static_cast<unsigned long long>(notice.project), static_cast<unsigned long long>(notice.address.instance),
            int(notice.source), int(notice.state), notice.operation.c_str(), notice.name.c_str(), notice.detail.c_str());
        m_audioNoticeTransitions[key] = transition;
    }
    // Bound bursts to one pending update per affected slot/operation.
    const auto found = std::find_if(m_audioFailureNotices.begin(), m_audioFailureNotices.end(),
        [&](const auto& previous) {
            return previous.source == notice.source && previous.project == notice.project &&
                previous.address.channelId == notice.address.channelId &&
                previous.address.slotId == notice.address.slotId && previous.operation == notice.operation;
        });
    if (found == m_audioFailureNotices.end()) m_audioFailureNotices.push_back(std::move(notice));
    else *found = std::move(notice);
}

std::vector<AudioFailureNotice> EngineController::takeAudioFailureNotices() {
    return std::exchange(m_audioFailureNotices, {});
}

void EngineController::reportAudioOperationFailure(std::string operation, const audio::Result& failure) {
    if (failure) return;
    AudioFailureNotice notice;
    notice.source = AudioFailureNotice::Source::Operation;
    notice.state = m_runtime->audioSafetyStopped() ? AudioFailureNotice::State::Stopped : AudioFailureNotice::State::Failed;
    notice.operation = std::move(operation);
    notice.detail = failure.message();
    publishAudioFailure(std::move(notice));
}

void EngineController::serviceAudioHealth(bool allowAutomaticRecovery) {
    if (!m_prepared) return;
    using State = AudioFailureNotice::State;
    const bool projectChanged = m_audioHealthProject != m_projectGeneration;
    const bool runtimeChanged = m_audioHealthRuntime != &m_runtime.get();
    if (projectChanged) {
        m_pluginRecovery.clear();
        m_pluginAutomaticRecoveryUsed.clear();
        m_audioNoticeTransitions.clear();
        m_audioOverloadNotices.clear();
        std::erase_if(m_audioFailureNotices, [&](const auto& n) { return n.project != m_projectGeneration; });
        m_audioHealthProject = m_projectGeneration;
        m_audioSafetyReported = false;
    }
    const auto generation = plugins::PluginNode::faultGeneration();
    if (projectChanged || runtimeChanged || generation != m_audioHealthFaultGeneration) {
        m_audioHealthRuntime = &m_runtime.get();
        m_audioHealthFaultGeneration = generation;
        for (const auto& fault : m_runtime->pluginFaults()) {
            const auto& address = fault.address;
            auto& entry = m_pluginRecovery[recoveryKey(address.channelId, address.slotId)];
            if (const auto used = m_pluginAutomaticRecoveryUsed.find(recoveryKey(address.channelId, address.slotId));
                used != m_pluginAutomaticRecoveryUsed.end()) {
                const auto* slot = insertModel(address.channelId, address.slotId);
                entry.automaticAttempted = slot && slot->uid == used->second;
            }
            if (entry.notice.address.instance == address.instance && entry.notice.state != State::Recovered) continue;
            auto& notice = entry.notice;
            notice = {};
            notice.address = address;
            notice.incident = ++m_audioIncidentSequence;
            notice.project = m_projectGeneration;
            notice.state = entry.automaticAttempted ? State::Failed : State::Waiting;
            notice.detail = fault.detail;
            notice.canRetry = fault.canRetry;
            notice.requiresStop = liveAudioActivity();
            notice.recording = isRecording() || isCountingIn();
            notice.master = address.channelId == kMasterChannelId;
            if (const auto* slot = insertModel(address.channelId, address.slotId)) notice.name = slot->name;
            if (const auto* track = m_project.findTrack(address.channelId)) {
                notice.channelName = track->name;
                notice.instrument = track->instrument.id == address.slotId;
            }
            if (!notice.canRetry) notice.state = State::Failed;
            publishAudioFailure(notice);
        }
    }

    bool attempted = false;
    std::optional<bool> audioActive;
    for (auto it = m_pluginRecovery.begin(); it != m_pluginRecovery.end();) {
        auto& entry = it->second;
        auto& notice = entry.notice;
        const auto current = m_runtime->pluginInstanceId({notice.address.channelId, notice.address.slotId, notice.address.right});
        if (!current || current != notice.address.instance) {
            notice.state = State::Retired;
            publishAudioFailure(notice);
            it = m_pluginRecovery.erase(it);
            continue;
        }
        ++it;
        if (notice.state == State::Recovered) continue;
        if (!audioActive) audioActive = liveAudioActivity() || m_runtime->previewSnapshot().playing || m_runtime->auditionRuntime;
        const bool active = *audioActive;
        const bool recording = isRecording() || isCountingIn();
        if (active != notice.requiresStop || recording != notice.recording) {
            notice.requiresStop = active; notice.recording = recording;
            publishAudioFailure(notice);
        }
        if (allowAutomaticRecovery && !attempted && !active && !entry.automaticAttempted && notice.canRetry) {
            attempted = true;
            entry.automaticAttempted = true;
            if (const auto* slot = insertModel(notice.address.channelId, notice.address.slotId))
                m_pluginAutomaticRecoveryUsed[recoveryKey(notice.address.channelId, notice.address.slotId)] = slot->uid;
            (void)recoverInsertEntry(entry);
        }
    }
    for (auto it = m_audioOverloadNotices.begin(); it != m_audioOverloadNotices.end();) {
        auto& notice = it->second;
        if (m_runtime->pluginInstanceId(notice.address) != notice.address.instance) {
            notice.state = State::Retired;
            publishAudioFailure(notice);
            it = m_audioOverloadNotices.erase(it);
        } else ++it;
    }
    if (m_runtime->audioSafetyStopped() && !m_audioSafetyReported) {
        m_audioSafetyReported = true;
        reportAudioOperationFailure("Audio engine", audio::Result::fail(
            audio::EngineError::AudioThreadError, "Audio rollback failed. The audio engine is stopped; the project can still be saved."));
    }
}

audio::Result EngineController::recoverInsertEntry(PluginRecoveryEntry& entry) {
    auto& notice = entry.notice;
    if (isPlaying() || isRecording() || isCountingIn())
        return audio::Result::fail(audio::EngineError::EngineRunning, "Stop playback and recording before restoring the plugin.");
    notice.state = AudioFailureNotice::State::Recovering;
    publishAudioFailure(notice);
    const auto result = m_runtime->recoverPlugin(notice.address, [&] {
        if (m_pluginRetiring) m_pluginRetiring(notice.address.channelId, notice.address.slotId);
    });
    notice.state = result ? AudioFailureNotice::State::Recovered : AudioFailureNotice::State::Failed;
    notice.detail = result ? std::string{} : result.message();
    if (result) {
        notice.address.instance = m_runtime->pluginInstanceId(
            {notice.address.channelId, notice.address.slotId, notice.address.right});
        notice.canRetry = false;
    }
    publishAudioFailure(notice);
    return result;
}

audio::Result EngineController::retryInsertRecovery(const std::string& channelId, const std::string& slotId,
    PluginIdentity identity) {
    if (!identity || identity.project != m_projectGeneration)
        return audio::Result::fail(audio::EngineError::InvalidArgument, "The project has changed.");
    serviceAudioHealth(false);
    const auto found = m_pluginRecovery.find(recoveryKey(channelId, slotId));
    if (found == m_pluginRecovery.end() || found->second.notice.address.instance != identity.instance ||
        !found->second.notice.canRetry)
        return audio::Result::fail(audio::EngineError::InvalidArgument, "The recovery action is no longer available.");
    // Explicit retries do not grant another automatic retry on the next Stop.
    found->second.automaticAttempted = true;
    if (const auto* slot = insertModel(channelId, slotId))
        m_pluginAutomaticRecoveryUsed[recoveryKey(channelId, slotId)] = slot->uid;
    return recoverInsertEntry(found->second);
}
}
