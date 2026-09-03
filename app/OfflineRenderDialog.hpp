#pragma once

#include "EngineController.hpp"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QListWidget;
class QProgressBar;
class QPushButton;
class QToolButton;

/// A headless draft plugin rack for non-destructive clip processing.
class OfflineRenderDialog final : public QDialog {
    Q_OBJECT
public:
    OfflineRenderDialog(
        daw::EngineController& controller,
        std::vector<daw::EngineController::ClipAddress> clips,
        bool chainsDiffer, QWidget* parent = nullptr);
    ~OfflineRenderDialog() override;

    bool rendered() const noexcept { return m_rendered; }

private:
    void refreshChain(const QString& keepSlot = {});
    QString selectedSlot() const;
    void moveSelected(int delta);
    void openSelectedEditor();
    void reloadPresets();
    void loadPreset();
    void savePreset();
    void startRender();

    daw::EngineController& m_controller;
    std::vector<daw::EngineController::ClipAddress> m_clips;
    daw::EngineController m_scratch;
    std::string m_chainTrackId;

    QLabel* m_warning = nullptr;
    QListWidget* m_chain = nullptr;
    QToolButton* m_add = nullptr;
    QPushButton* m_remove = nullptr;
    QPushButton* m_up = nullptr;
    QPushButton* m_down = nullptr;
    QPushButton* m_bypass = nullptr;
    QPushButton* m_edit = nullptr;
    QComboBox* m_presets = nullptr;
    QPushButton* m_loadPreset = nullptr;
    QPushButton* m_savePreset = nullptr;
    QCheckBox* m_includeTail = nullptr;
    QLabel* m_status = nullptr;
    QProgressBar* m_progress = nullptr;
    QDialogButtonBox* m_buttons = nullptr;
    QPushButton* m_renderButton = nullptr;
    bool m_rendering = false;
    bool m_cancelRequested = false;
    bool m_rendered = false;
};
