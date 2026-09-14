#pragma once

#include "EngineController.hpp"

#include <QDialog>
#include <QHash>
#include <QPointer>

class QCheckBox;
class QDialogButtonBox;
class QLabel;
class QListWidget;
class QProgressBar;
class QPushButton;
class QTimer;
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
        QWidget* parent = nullptr);
    ~OfflineRenderDialog() override;
    static bool checkForTest(const QString& screenshotPath = {});

    bool rendered() const noexcept { return m_rendered; }
    void reject() override;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void rebuildRack();
    void rackChanged(bool settle);
    void scheduleAutoRender(int delayMs = 0);
    void openEditor(const QString& insertId);
    void closeEditors();
    void reloadPresets();
    void loadPreset(const QString& path);
    void savePreset();
    void startRender();
    void updateRenderAvailability();
    void applyTheme();

    daw::EngineController& m_controller;
    std::vector<daw::EngineController::ClipAddress> m_clips;
    daw::EngineController m_scratch;
    std::string m_chainTrackId;

    QLabel* m_clipSummary = nullptr;
    QListWidget* m_clipList = nullptr;
    QWidget* m_rackHost = nullptr;
    QVBoxLayout* m_rackLayout = nullptr;
    ChannelStrip* m_rack = nullptr;
    QListWidget* m_presets = nullptr;
    QPushButton* m_savePreset = nullptr;
    QPushButton* m_autoRender = nullptr;
    QCheckBox* m_includeTail = nullptr;
    QLabel* m_status = nullptr;
    QLabel* m_progressPercent = nullptr;
    QProgressBar* m_progress = nullptr;
    QDialogButtonBox* m_buttons = nullptr;
    QPushButton* m_renderButton = nullptr;
    QPushButton* m_closeButton = nullptr;
    QTimer* m_autoRenderTimer = nullptr;
    QHash<QString, QPointer<QDialog>> m_editors;
    bool m_rendering = false;
    bool m_cancelRequested = false;
    bool m_autoRenderPending = false;
    bool m_rendered = false;
};
