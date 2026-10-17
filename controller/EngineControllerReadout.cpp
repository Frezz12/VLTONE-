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

#if DAW_ENABLE_REVERB
std::optional<ReverbSnapshot> EngineController::reverbSnapshot(const std::string& channel, const std::string& slot) const {
    auto result = m_runtime->reverbSnapshot(pluginAddress(channel, slot));
    if (result) {
        for (const auto& p : plugins::reverb::parameterTable())
            result->state.values[p.index] = insertParameter(channel, slot, p.id);
        (result->state.comparison == 'A' ? result->state.a : result->state.b) = result->state.values;
    }
    return result;
}

bool EngineController::applyReverbState(const std::string& channel, const std::string& slot,
                                        const plugins::reverb::State& state, const std::string& label) {
    if (!sharedEditingAllowed() || !sharedGestureAllowed("plugin:" + slot) ||
        (state.comparison != 'A' && state.comparison != 'B') || state.presetName.size() > 256 || state.presetKind.size() > 256) return false;
    const auto before = reverbSnapshot(channel, slot);
    if (!before) return false;
    for (const auto* values : {&state.values, &state.a, &state.b})
        for (const auto& p : plugins::reverb::parameterTable())
            if (!std::isfinite((*values)[p.index]) || (*values)[p.index] < p.minValue || (*values)[p.index] > p.maxValue ||
                (p.isStepped && std::round((*values)[p.index]) != (*values)[p.index])) return false;
    unfreezeTrack(channel, false);
    const auto identity = insertIdentity(channel, slot);
    const auto address = pluginAddress(channel, slot, identity.instance);
    const auto undo = beginUndoGroup();
    for (const auto& p : plugins::reverb::parameterTable()) {
        if (before->state.values[p.index] == state.values[p.index]) continue;
        setInsertParameter(channel, slot, p.id, state.values[p.index]);
        commitInsertParameterEdit(channel, slot, p.id, before->state.values[p.index], label);
    }
    if (insertIdentity(channel, slot) != identity || !m_runtime->setReverbComparisonState(address, state)) {
        releaseUndoGroup(undo); return false;
    }
    // Include the banks, selected side and preset label in the same undo as
    // the parameter edits. Merely restoring the audible values loses A/B.
    const auto project = m_projectGeneration;
    const auto restore = [this, channel, slot, project, right = address.right](const plugins::reverb::State& saved) {
        if (m_projectGeneration != project) return;
        auto target = pluginAddress(channel, slot); target.right = right;
        m_runtime->setReverbComparisonState(target, saved);
    };
    m_undo.push(label, [restore, old = before->state] { restore(old); }, [restore, state] { restore(state); });
    collapseUndo(undo, label);
    return true;
}

bool EngineController::switchReverbComparison(const std::string& channel, const std::string& slot, char side) {
    const auto current = reverbSnapshot(channel, slot);
    if (!current || (side != 'A' && side != 'B') || current->state.comparison == side) return false;
    auto state = current->state;
    state.values = side == 'A' ? state.a : state.b;
    state.comparison = side; state.presetKind = "custom"; state.presetName = "Custom";
    return applyReverbState(channel, slot, state, "Switch Reverb A/B");
}

bool EngineController::copyReverbComparison(const std::string& channel, const std::string& slot) {
    const auto current = reverbSnapshot(channel, slot);
    if (!current) return false;
    auto state = current->state;
    (state.comparison == 'A' ? state.b : state.a) = state.values;
    return applyReverbState(channel, slot, state, "Copy Reverb A/B");
}

#endif

bool EngineController::setGravityFrozen(const std::string& channelId, const std::string& insertId, bool frozen) {
    if (!sharedEditingAllowed() || !sharedGestureAllowed("plugin:" + insertId)) return false;
    return m_runtime->setGravityFrozen(pluginAddress(channelId, insertId), frozen);
}

bool EngineController::clearGravityTail(const std::string& channelId, const std::string& insertId) {
    if (!sharedEditingAllowed() || !sharedGestureAllowed("plugin:" + insertId)) return false;
    return m_runtime->clearGravityTail(pluginAddress(channelId, insertId));
}

} // namespace daw
