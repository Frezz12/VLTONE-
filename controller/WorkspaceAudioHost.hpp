#pragma once

#include "EngineController.hpp"

namespace daw {

/// The sole authority allowed to power a document's live runtime. Viewing a
/// different document never calls this class. Controllers keep stable addresses
/// because their undo actions and editors capture them.
class WorkspaceAudioHost final {
public:
    EngineController* active() const noexcept { return m_active; }
    bool busy() const noexcept { return m_busy; }
    void adoptInitial(EngineController& controller, const audio::AudioDeviceConfig& config, bool hardware) {
        m_active = controller.audioEnabled() && (!hardware || controller.isDeviceOpen()) ? &controller : nullptr;
        m_config = config; m_hardware = hardware;
    }
    audio::Result activate(EngineController* target) {
        if (m_busy) return audio::Result::fail(audio::EngineError::NotSupported, "Audio engine is switching.");
        if (target == m_active) return audio::Result::ok();
        m_busy = true;
        struct Finish { bool& busy; ~Finish() { busy = false; } } finish{m_busy};
        if (m_active) {
            if (m_active->isDeviceOpen()) m_config = m_active->audioConfiguration();
            if (const auto stopped = m_active->deactivateAudio(); !stopped) return stopped;
            m_active = nullptr;
        }
        if (!target) return audio::Result::ok();
        const auto started = target->activateAudio(m_config, m_hardware);
        if (started) m_active = target;
        return started;
    }
    const audio::AudioDeviceConfig& configuration() const noexcept { return m_config; }
    void setConfiguration(const audio::AudioDeviceConfig& config) { if (!m_busy) m_config = config; }
private:
    EngineController* m_active = nullptr;
    audio::AudioDeviceConfig m_config;
    bool m_hardware = true;
    bool m_busy = false;
};

} // namespace daw
