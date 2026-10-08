#include "EngineController.hpp"

namespace daw {

EngineController::PluginRuntimeStatus EngineController::insertRuntimeStatus(
    const std::string& channelId, const std::string& slotId, AudioRuntimeEndpoint::Readout readout) const {
    return m_runtime.pluginRuntimeStatus(channelId, slotId, readout);
}

bool EngineController::restartInsert(const std::string& channelId, const std::string& slotId) {
    return m_runtime.restartPlugin(channelId, slotId);
}

} // namespace daw
