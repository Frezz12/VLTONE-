#pragma once
#include "EngineController.hpp"
#include <QDialog>
#include <atomic>
#include <future>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QProgressBar;
class SilencePreview;

class StripSilenceDialog final : public QDialog {
    Q_OBJECT
public:
    StripSilenceDialog(daw::EngineController& controller,
        std::vector<daw::EngineController::ClipAddress> clips, QWidget* parent = nullptr);
    ~StripSilenceDialog() override;
    bool applied() const { return m_applied; }
    const std::vector<daw::EngineController::ClipAddress>& createdClips() const { return m_created; }
    static bool checkForTest(const QString& screenshot);

private:
    void updatePreview();
    void pollAnalysis();
    void apply();
    void rememberSettings();
    daw::EngineController& m_controller;
    daw::StripSilenceSettings m_settings;
    std::vector<daw::EngineController::StripSilenceSource> m_sources;
    std::vector<daw::SilenceEnvelope> m_envelopes;
    std::vector<daw::EngineController::ClipAddress> m_created;
    std::future<std::vector<daw::SilenceEnvelope>> m_analysis;
    std::shared_ptr<std::atomic<bool>> m_cancel = std::make_shared<std::atomic<bool>>(false);
    QComboBox* m_clip = nullptr;
    QComboBox* m_grid = nullptr;
    QCheckBox* m_internal = nullptr;
    QCheckBox* m_auto = nullptr;
    SilencePreview* m_preview = nullptr;
    QLabel* m_status = nullptr;
    QProgressBar* m_progress = nullptr;
    QPushButton* m_apply = nullptr;
    bool m_settingsOnly = false, m_ready = false, m_applied = false;
};
