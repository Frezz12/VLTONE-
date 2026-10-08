#include "EngineController.hpp"
#include "model/MiniModules.hpp"

#include <algorithm>
#include <cmath>

namespace daw {
namespace {

std::vector<AudioGraphSpec::MiniModuleRoute> miniModuleRoutes(const std::vector<InsertModel>& slots) {
    std::vector<AudioGraphSpec::MiniModuleRoute> routes;
    routes.reserve(slots.size());
    for (const auto& slot : slots) routes.push_back({slot.id, slot.miniModulePostFx});
    return routes;
}

void appendSidechainRoutes(std::vector<AudioGraphSpec::SidechainRoute>& routes,
                          std::span<const InsertModel> slots) {
    for (const auto& slot : slots)
        if (!slot.sidechainTrackIds.empty()) routes.push_back({slot.id, slot.sidechainTrackIds});
}

void excludeSidechainFeedback(const ProjectModel& project, const std::string& channelId,
                             std::vector<AudioGraphSpec::SidechainRoute>& routes) {
    if (routes.empty()) return;
    const auto feedback = sidechainFeedbackSources(project, channelId);
    for (auto& route : routes)
        std::erase_if(route.sourceChannelIds, [&](const auto& id) { return feedback.contains(id); });
    std::erase_if(routes, [](const auto& route) { return route.sourceChannelIds.empty(); });
}

} // namespace

AudioSessionSpec EngineController::prepareAudioSession(AudioPluginLoadPolicy loadPolicy) {
    // Preparation belongs to the document/control thread. Runtime assembly
    // receives only routing values and resources, never callbacks into here.
    if (m_clipSampleBake) cancelClipSampleBake();
    m_deferredClipSync.clear();
    m_clipSampleViewRequests.clear();
    struct FreezeRebuildScope { bool& flag; bool previous; ~FreezeRebuildScope() { flag = previous; } };
    FreezeRebuildScope freezeScope{m_rebuildingFrozenGraph, m_rebuildingFrozenGraph};
    m_rebuildingFrozenGraph = true;
    for (auto& track : m_project.tracks) {
        if (!track.freeze.active()) continue;
        const auto fingerprint = freezeFingerprint(track);
        if (!freezeUnavailableReason(track.id).empty() ||
            !std::isfinite(track.freeze.durationSeconds) || track.freeze.durationSeconds <= 0 ||
            (!track.freeze.sourceFingerprint.empty() && track.freeze.sourceFingerprint != fingerprint))
            track.freeze = {};
        else track.freeze.sourceFingerprint = fingerprint;
    }
    m_clipPositionEdit.indices.clear();
    m_clipPositionEdit.patternsIndexed = false;
    ++m_clipGeometryRevision;
    m_project.invalidateTrackIndex();
    migrateMiniModules(m_project, false);
    m_project.useExplicitStructureCache();
    const AutomationIndexScope automationScope(*this);
    ++m_graphRebuildCount;

    AudioSessionSpec session;
    session.hosting = {m_pluginManager.hostingMode(), m_pluginManager.pluginHostPath()};
    const auto plugins = [&](const std::string& channelId, AudioPluginChainSpec::Kind kind,
                             const std::vector<InsertModel>& slots, const std::string& clipId = {}) {
        auto chain = preparePluginChain(channelId, kind, slots, clipId, loadPolicy);
        session.pluginChains.push_back(std::move(chain));
    };
    auto& spec = session.graph;
    spec.channels.reserve(m_project.tracks.size());
    spec.masterVolume = m_project.masterVolume;
    spec.masterPan = m_project.masterPan;
    spec.masterMiniModules = miniModuleRoutes(m_project.masterMiniModules);
    spec.metronomeEnabled = m_metronomeEnabled;
    spec.auditionCapture = m_pluginAuditionCapture;
    plugins(kMasterChannelId, AudioPluginChainSpec::Kind::Inserts, m_project.masterInserts);
    plugins(kMasterChannelId, AudioPluginChainSpec::Kind::MiniModules, m_project.masterMiniModules);
    appendSidechainRoutes(spec.masterSidechains, m_project.masterInserts);
    excludeSidechainFeedback(m_project, kMasterChannelId, spec.masterSidechains);

    const SoloState solo = soloState();
    for (auto& track : m_project.tracks) {
        if (!carriesAudio(track)) continue;
        auto& description = spec.channels.emplace_back();
        description.id = track.id;
        description.name = track.name;
        description.outputBusId = track.outputBusId;
        description.acceptsMidi = trackAccepts(track.kind, ClipKind::Midi);
        description.capturing = std::find(m_recordingTracks.begin(), m_recordingTracks.end(), track.id)
                                != m_recordingTracks.end();
        description.input = {
            acceptsRecording(track) && (track.monitor || track.monitorAuto || track.armed || (m_prepared && m_runtime.hasInputRoute(track.id))),
            track.monitor && track.inputEnabled, track.inputChannel, track.inputChannelCount,
            track.monitorInputMask};
        description.volume = track.volume;
        description.pan = track.pan;
        description.mono = track.mono;
        description.silent = track.muted || (solo.any && !solo.open.contains(track.id));
        description.sends = track.sends;
        description.miniModules = miniModuleRoutes(track.miniModules);
        description.samplerOwned = track.samplerFx.isOwnedBy(track.instrument);
        description.samplerVolume = track.samplerFx.volume;
        description.samplerPan = track.samplerFx.pan;

        for (const auto& clip : track.clips) {
            if (clip.kind != ClipKind::Audio || (clip.inserts.empty() && !clip.playbackInjection.active())) continue;
            const bool cached = offlineProcessCacheValid(ClipAddress{track.id, clip.id});
            description.clipFx.push_back({clip.id, clip.name, cached ? 1.0f : clip.gain,
                                          cached ? 0.0f : clip.pan, clip.playbackInjection});
            plugins(track.id, AudioPluginChainSpec::Kind::ClipFx, clip.inserts, clip.id);
            appendSidechainRoutes(description.sidechains, clip.inserts);
        }
        plugins(track.id, AudioPluginChainSpec::Kind::Inserts, track.inserts);
        appendSidechainRoutes(description.sidechains, track.inserts);
        const std::vector<InsertModel> instrument = description.acceptsMidi && track.instrument.isLoaded()
            ? std::vector<InsertModel>{track.instrument} : std::vector<InsertModel>{};
        plugins(track.id, AudioPluginChainSpec::Kind::Instrument, instrument);
        appendSidechainRoutes(description.sidechains, instrument);
        if (description.samplerOwned) {
            plugins(track.id, AudioPluginChainSpec::Kind::SamplerInserts, track.samplerFx.inserts);
            appendSidechainRoutes(description.sidechains, track.samplerFx.inserts);
        } else plugins(track.id, AudioPluginChainSpec::Kind::SamplerInserts, {});
        plugins(track.id, AudioPluginChainSpec::Kind::MiniModules, track.miniModules);
        excludeSidechainFeedback(m_project, track.id, description.sidechains);

        if (track.freeze.active()) {
            description.frozenAudio = loadSamples(track.freeze.filePath);
            if (description.frozenAudio) description.frozenFrames = toSamples(track.freeze.durationSeconds);
            else track.freeze = {};
        }
    }

    session.channels.reserve(spec.channels.size() + 1);
    for (const auto& track : m_project.tracks) {
        if (!carriesAudio(track)) continue;
        auto& playback = session.channels.emplace_back();
        playback.id = track.id;
        playback.content.clips = prepareTrackClips(track);
        if (trackAccepts(track.kind, ClipKind::Midi)) playback.content.midi = prepareTrackNotes(track);
        playback.content.plugins = prepareTrackAutomation(track);
        playback.content.levels = prepareTrackLevelAutomation(track);
    }
    TrackModel masterAutomation;
    masterAutomation.id = kMasterChannelId;
    auto& masterPlayback = session.channels.emplace_back();
    masterPlayback.id = kMasterChannelId;
    masterPlayback.content.plugins = prepareTrackAutomation(masterAutomation);
    masterPlayback.content.levels = prepareTrackLevelAutomation(masterAutomation);
    return session;
}

audio::Result EngineController::publishAudioSession(AudioSessionSpec session,
    bool reconfigurePlugins, std::span<const AudioPluginStateEdit> restores) {
    if (m_prepared && m_pluginRetiring) {
        auto retiring = m_runtime.retiringPlugins(session.pluginChains);
        for (const auto& edit : restores) if (edit.replaceExisting &&
            std::none_of(retiring.begin(), retiring.end(), [&](const auto& address) {
                return address.channelId == edit.address.channelId && address.slotId == edit.address.slotId;
            })) retiring.push_back(edit.address);
        for (const auto& address : retiring) m_pluginRetiring(address.channelId, address.slotId);
    }
    const auto committed = m_runtime.applySession(std::move(session), reconfigurePlugins, restores);
    if (!committed) return committed;
    if (!m_liveDeviceAllowed) m_previewParameterEditsPending = true;
    refreshAutomaticMonitoring(false);
    m_runtime.suspendRecordingClipFx(m_recordingTracks);
    updateTimelineDuration();
    return committed;
}

audio::Result EngineController::rebuildGraph(bool reconfigurePlugins,
    AudioPluginLoadPolicy loadPolicy, std::span<const AudioPluginStateEdit> restores) {
    if (m_prepared && m_runtime.isRemote() && !m_runtime.metadata().connected)
        return audio::Result::fail(audio::EngineError::NotInitialized, m_runtime.metadata().error);
    return publishAudioSession(prepareAudioSession(loadPolicy), reconfigurePlugins, restores);
}

void EngineController::appendInsertStateEdits(std::vector<AudioPluginStateEdit>& edits,
    const std::string& channelId, const ChainSlotSnapshot& slot, bool applyAllParameters) const {
    if (!slot.model.isLoaded()) return;
    for (bool right : {false, true}) {
        if (right && slot.model.channelMode != PluginChannelMode::DualMono) continue;
        auto& edit = edits.emplace_back();
        edit.address = {channelId.empty() ? kMasterChannelId : channelId, slot.model.id, right};
        edit.state.state = right && !slot.rightState.empty() ? slot.rightState : slot.state;
        edit.state.applyAllParameters = applyAllParameters;
        edit.state.clearPending = true;
        edit.state.source = right && slot.rightSource ? slot.rightSource : slot.source;
        edit.state.sourcePath = right && slot.rightSource ? slot.rightSourcePath : slot.sourcePath;
        attachPluginStateSample(slot.model.uid, edit.state);
        edit.parameters = right && !slot.model.rightParameters.empty()
            ? slot.model.rightParameters : slot.model.parameters;
    }
}

} // namespace daw
