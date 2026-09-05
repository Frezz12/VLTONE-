#pragma once

#include "EngineController.hpp"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QLabel;
class QProgressBar;
class QPushButton;

/// Configures and runs one atomic Bounce in Place operation.
class BounceInPlaceDialog final : public QDialog {
    Q_OBJECT
public:
    BounceInPlaceDialog(daw::EngineController& controller,
                        daw::EngineController::BounceRequest request,
                        QString sourceDescription,
                        QWidget* parent = nullptr);

    bool rendered() const noexcept { return m_rendered; }
    void reject() override;

private:
    void syncControls();
    void updateSummary();
    void startRender();
    daw::EngineController::BounceRequest requestFromControls() const;

    daw::EngineController& m_controller;
    daw::EngineController::BounceRequest m_baseRequest;

    QComboBox* m_ending = nullptr;
    QDoubleSpinBox* m_start = nullptr;
    QDoubleSpinBox* m_end = nullptr;
    QDoubleSpinBox* m_preRoll = nullptr;
    QCheckBox* m_clipFx = nullptr;
    QCheckBox* m_trackFx = nullptr;
    QCheckBox* m_sends = nullptr;
    QCheckBox* m_summing = nullptr;
    QCheckBox* m_masterFx = nullptr;
    QComboBox* m_destination = nullptr;
    QLabel* m_summary = nullptr;
    QLabel* m_warning = nullptr;
    QLabel* m_status = nullptr;
    QProgressBar* m_progress = nullptr;
    QDialogButtonBox* m_buttons = nullptr;
    QPushButton* m_renderButton = nullptr;
    bool m_rendering = false;
    bool m_cancelRequested = false;
    bool m_rendered = false;
};
