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
class QVBoxLayout;
class QWidget;
class ChannelStrip;

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
    void reject() override;

private:
    void rebuildRack();
    void openEditor(const QString& insertId);
    void reloadPresets();
    void loadPreset();
    void savePreset();
    void startRender();

    daw::EngineController& m_controller;
    std::vector<daw::EngineController::ClipAddress> m_clips;
    daw::EngineController m_scratch;
    std::string m_chainTrackId;

    QLabel* m_warning = nullptr;
    QListWidget* m_clipList = nullptr;
    QWidget* m_rackHost = nullptr;
    QVBoxLayout* m_rackLayout = nullptr;
    ChannelStrip* m_rack = nullptr;
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
