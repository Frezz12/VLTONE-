#pragma once

#include <QWidget>

namespace daw {
class EngineController;
}

class QComboBox;

/// Preferences for the transport's play/pause behaviour — the "Transport" tab
/// of the settings window.
class TransportSettingsPage : public QWidget {
    Q_OBJECT
public:
    explicit TransportSettingsPage(daw::EngineController* controller,
                                   QWidget* parent = nullptr);

private:
    daw::EngineController* m_controller;
    QComboBox* m_modeCombo = nullptr;
};
