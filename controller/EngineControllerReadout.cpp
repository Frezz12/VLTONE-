#include "EngineController.hpp"

#include <cmath>

namespace daw {

EngineController::PluginRuntimeStatus EngineController::insertRuntimeStatus(
    const std::string& channelId, const std::string& slotId) const {
    auto status = m_runtime->pluginRuntimeStatus(channelId, slotId);
    const auto found = m_pluginRecovery.find(channelId + "/" + slotId);
    if (status.state == AudioPluginRuntimeState::Faulted && found != m_pluginRecovery.end() &&
        found->second.notice.project == m_projectGeneration && found->second.notice.address.instance == status.address.instance) {
        const auto& notice = found->second.notice;
        if (notice.state == AudioFailureNotice::State::Waiting) status.state = AudioPluginRuntimeState::AwaitingRecovery;
        else if (notice.state == AudioFailureNotice::State::Recovering) status.state = AudioPluginRuntimeState::Recovering;
        status.detail = notice.detail;
        status.canRetry = notice.canRetry;
        status.incident = notice.incident;
    }
    return status;
}

AudioPluginAddress EngineController::pluginAddress(const std::string& channelId,
    const std::string& insertId, std::uint64_t instance) const {
    static const std::string master{kMasterChannelId};
    const auto& channel = channelId.empty() ? master : channelId;
    const auto* model = insertModel(channel, insertId);
    return {channel, insertId, model && model->channelMode == PluginChannelMode::DualMono &&
        model->editorChannel == PluginEditorChannel::Right, instance};
}

bool EngineController::hasInsert(const std::string& channelId,
    const std::string& insertId, std::string_view uid) const {
    const auto* slot = insertModel(channelId, insertId);
    return slot && (uid.empty() || slot->uid == uid) && m_runtime->hasPlugin(pluginAddress(channelId, insertId));
}

EffectMeterSnapshot EngineController::effectMeterSnapshot(
    const std::string& channelId, const std::string& insertId) {
    return m_runtime->effectMeterSnapshot(pluginAddress(channelId, insertId));
}

SamplerSnapshot EngineController::samplerSnapshot(
    const std::string& channelId, const std::string& insertId) const {
    return m_runtime->samplerSnapshot(pluginAddress(channelId, insertId));
}

PluginIdentity EngineController::insertIdentity(const std::string& channelId,
    const std::string& insertId) const {
    const auto instance = m_runtime->pluginInstanceId(pluginAddress(channelId, insertId));
    return instance ? PluginIdentity{m_projectGeneration, instance} : PluginIdentity{};
}

std::optional<SlicerSnapshot> EngineController::slicerSnapshot(const std::string& channelId,
    const std::string& insertId, bool includeActivity) const {
    auto out = m_runtime->slicerSnapshot(pluginAddress(channelId, insertId), includeActivity);
    if (out) out->identity.project = m_projectGeneration;
    return out;
}

std::optional<EqualizerSnapshot> EngineController::equalizerSnapshot(const std::string& channelId,
    const std::string& insertId, bool consumeMeters) {
    return m_runtime->equalizerSnapshot(pluginAddress(channelId, insertId), consumeMeters);
}

std::optional<EqualizerResponse> EngineController::equalizerResponse(
    const std::string& channelId, const std::string& insertId) const {
    return m_runtime->equalizerResponse(pluginAddress(channelId, insertId));
}

std::optional<std::array<double, 180>> EngineController::modulationResponse(
    const std::string& channelId, const std::string& insertId) const {
    return m_runtime->modulationResponse(pluginAddress(channelId, insertId));
}

std::optional<GravitySnapshot> EngineController::gravitySnapshot(
    const std::string& channelId, const std::string& insertId) {
    return m_runtime->gravitySnapshot(pluginAddress(channelId, insertId));
}

bool EngineController::setInsertPresetReference(const std::string& channelId,
    const std::string& insertId, std::string kind, std::string name) {
    if (!sharedEditingAllowed() || !sharedGestureAllowed("plugin:" + insertId)) return false;
    return m_runtime->setInsertPresetReference(pluginAddress(channelId, insertId), std::move(kind), std::move(name));
}

bool EngineController::setEqualizerAnalyzer(const std::string& channelId, const std::string& insertId,
    const plugins::equalizer::AnalyzerConfig& config) {
    return m_runtime->setEqualizerAnalyzer(pluginAddress(channelId, insertId), config);
}

bool EngineController::auditionEqualizerBand(const std::string& channelId, const std::string& insertId, int band) {
    return m_runtime->auditionEqualizerBand(pluginAddress(channelId, insertId), band);
}

bool EngineController::switchEqualizerComparison(const std::string& channelId, const std::string& insertId, char slot) {
    if ((slot != 'A' && slot != 'B') || !sharedEditingAllowed() || !sharedGestureAllowed("plugin:" + insertId)) return false;
    const auto before = m_runtime->equalizerSnapshot(pluginAddress(channelId, insertId), false);
    if (!before || before->comparison == slot) return false;
    unfreezeTrack(channelId, false);
    const auto identity = insertIdentity(channelId, insertId);
    const auto address = pluginAddress(channelId, insertId, identity.instance);
    const auto values = m_runtime->captureEqualizerComparison(address, slot);
    if (!values) return false;
    const auto previousValues = m_runtime->pluginParameterValues(address);
    const auto undo = beginUndoGroup();
    for (const auto& info : plugins::equalizer::parameterTable()) {
        const auto previousValue = std::find_if(previousValues.begin(), previousValues.end(),
            [&](const auto& value) { return value.id == info.id; });
        if (previousValue == previousValues.end()) continue;
        const double previous = previousValue->value;
        if (std::abs(previous - (*values)[info.index]) < 1.0e-12) continue;
        if (insertIdentity(channelId, insertId) != identity) {
            releaseUndoGroup(undo);
            return false;
        }
        setInsertParameter(channelId, insertId, info.id, (*values)[info.index]);
        commitInsertParameterEdit(channelId, insertId, info.id, previous, "Switch Equalizer A/B");
    }
    if (insertIdentity(channelId, insertId) != identity) {
        releaseUndoGroup(undo);
        return false;
    }
    collapseUndo(undo, "Switch Equalizer A/B");
    return m_runtime->activateEqualizerComparison(address, slot);
}

bool EngineController::copyEqualizerComparison(const std::string& channelId, const std::string& insertId) {
    if (!sharedEditingAllowed() || !sharedGestureAllowed("plugin:" + insertId)) return false;
    return m_runtime->copyEqualizerComparison(pluginAddress(channelId, insertId));
}

bool EngineController::setGravityFrozen(const std::string& channelId, const std::string& insertId, bool frozen) {
    if (!sharedEditingAllowed() || !sharedGestureAllowed("plugin:" + insertId)) return false;
    return m_runtime->setGravityFrozen(pluginAddress(channelId, insertId), frozen);
}

bool EngineController::clearGravityTail(const std::string& channelId, const std::string& insertId) {
    if (!sharedEditingAllowed() || !sharedGestureAllowed("plugin:" + insertId)) return false;
    return m_runtime->clearGravityTail(pluginAddress(channelId, insertId));
}

} // namespace daw
