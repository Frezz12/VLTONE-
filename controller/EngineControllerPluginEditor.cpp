#include "EngineController.hpp"

namespace daw {

std::optional<PluginEditorSnapshot> EngineController::insertEditorSnapshot(
    const std::string& channelId, const std::string& insertId) const {
    auto out = m_runtime->pluginEditorSnapshot(pluginAddress(channelId, insertId));
    if (out) out->identity.project = m_projectGeneration;
    return out;
}

bool EngineController::openInsertEditor(const std::string& channelId, const std::string& insertId,
    PluginIdentity identity, void* parent, plugins::PluginEditorHost* host) {
    if (!identity || identity.project != m_projectGeneration || !sharedEditingAllowed() ||
        !sharedGestureAllowed("plugin:" + insertId)) return false;
    return m_runtime->openPluginEditor(pluginAddress(channelId, insertId, identity.instance), parent, host);
}

bool EngineController::closeInsertEditor(const std::string& channelId, const std::string& insertId,
    PluginIdentity identity, bool onlyUnattached) {
    if (!identity || identity.project != m_projectGeneration) return false;
    return m_runtime->closePluginEditor(pluginAddress(channelId, insertId, identity.instance), onlyUnattached);
}

std::optional<PluginEditorSize> EngineController::insertEditorSize(const std::string& channelId,
    const std::string& insertId, PluginIdentity identity) const {
    if (!identity || identity.project != m_projectGeneration) return std::nullopt;
    return m_runtime->pluginEditorSize(pluginAddress(channelId, insertId, identity.instance));
}

std::optional<PluginEditorSize> EngineController::resizeInsertEditor(const std::string& channelId,
    const std::string& insertId, PluginIdentity identity, PluginEditorSize requested) {
    if (!identity || identity.project != m_projectGeneration) return std::nullopt;
    return m_runtime->resizePluginEditor(pluginAddress(channelId, insertId, identity.instance), requested);
}

bool EngineController::pumpInsertEditor(const std::string& channelId, const std::string& insertId,
    PluginIdentity identity) {
    if (!identity || identity.project != m_projectGeneration) return false;
    return m_runtime->pumpPluginEditor(pluginAddress(channelId, insertId, identity.instance));
}

void EngineController::readInsertParameters(const std::string& channelId, const std::string& insertId,
    std::span<PluginParameterReadout> values) const {
    m_runtime->readPluginParameters(pluginAddress(channelId, insertId), values);
}

std::string EngineController::insertParameterText(const std::string& channelId, const std::string& insertId,
    const std::string& parameterId, double value, std::int32_t indexHint) const {
    return m_runtime->pluginParameterText(pluginAddress(channelId, insertId), parameterId, value, indexHint);
}

} // namespace daw
