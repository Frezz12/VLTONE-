#pragma once

#include <QWidget>

// RecordMode is an enum passed by value below, so the real definition is needed.
#include "model/Document.hpp"

namespace daw {
class EngineController;
}

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;

/// Preferences ▸ Recording: what a new take does to what is already there, how
/// loop passes are kept, and how input monitoring is handled around a recording.
///
/// The page writes through to the controller on every change (there is no OK
/// button) and mirrors the same values into QSettings so they survive a restart.
/// The controller stays the single source of truth — this page only ever pushes a
/// whole `RecordingPrefs` and lets it decide what that means.
class RecordingSettingsPage : public QWidget {
    Q_OBJECT
public:
    explicit RecordingSettingsPage(daw::EngineController* controller,
                                   QWidget* parent = nullptr);

    /// Re-read the controller. The mode also changes from the transport's Layers
    /// button and Shift+L, so an open settings window has to be able to catch up.
    void reload();

    /// Load persisted preferences into `controller` at startup, before any UI
    /// exists. Static so main() can call it without building the page.
    static void restore(daw::EngineController& controller);

    /// Persist the global recording mode on its own. The transport's Layers
    /// button and Shift+L change the mode without this page existing, and the
    /// choice still has to survive a restart — this keeps the settings key in
    /// the one file that owns it.
    static void persistMode(daw::RecordMode mode);

    /// Persist the count-in length on its own, for the same reason as the mode:
    /// the context panel that appears when Record is engaged sets it there.
    static void persistCountInBeats(int beats);

    /// Persist the "monitor while recording" switch on its own — the recording
    /// context panel carries the same toggle.
    static void persistAutoMonitor(bool on);

signals:
    /// The global recording mode changed here, so the transport button can follow.
    void recordModeChanged();

private:
    /// Collect every widget into the controller and QSettings.
    void commit();

    daw::EngineController* m_controller;
    bool m_loading = false;   // guard so reload() does not re-commit itself

    QComboBox* m_mode = nullptr;
    QComboBox* m_loopMode = nullptr;
    QComboBox* m_monitorStop = nullptr;
    QComboBox* m_midiComp = nullptr;
    QComboBox* m_countIn = nullptr;
    QCheckBox* m_trimTakes = nullptr;
    QCheckBox* m_autoExpand = nullptr;
    QCheckBox* m_recordKey = nullptr;
    QCheckBox* m_autoMonitor = nullptr;
    QCheckBox* m_manualMonitor = nullptr;
    QDoubleSpinBox* m_crossfade = nullptr;
};
