#pragma once

#include <QMainWindow>
#include <memory>

#include "AudioSettings.h"
#include "daw/Engine.h"
#include "daw/graph/PeakFile.h"

class QCheckBox;
class QDockWidget;
class QLabel;
class QPushButton;
class QSlider;
class QTimer;
class LevelMeter;
class MixerView;
class PeakBuilder;
class SettingsDialog;
class TimelineRuler;
class TimelineView;
class TransportBar;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void onOpenSettings();
    void onOpenAudioFile();
    void onApplySettings(const AudioSettings& settings);
    void onTogglePlay(bool playing);
    void onFrequencyChanged(int value);
    void onGainChanged(int value);
    void onToggleMonitoring(bool enabled);
    void onTelemetryTick();
    void onPeakReady(const QString& audioPath,
                     std::shared_ptr<daw::graph::PeakFile> peakFile);
    void onLocateRequested(std::int64_t sample);

private:
    void buildMenus();
    void buildUi();
    void connectTransport();

    void restartAudio();
    void updateAudioStatus();

    // Движок создаётся первым и уничтожается последним: UI не должен пережить
    // остановку аудио-потока.
    std::unique_ptr<daw::Engine> engine_;

    AudioSettings   settings_;
    SettingsDialog* settingsDialog_ = nullptr;   // живёт, пока открыт
    QString         startupWarning_;
    QString         lastOpenedFile_;

    TransportBar* transport_ = nullptr;

    QCheckBox*   monitoringBox_ = nullptr;
    QPushButton* toneButton_    = nullptr;
    QSlider*     freqSlider_    = nullptr;
    QSlider*     gainSlider_    = nullptr;
    QLabel*      freqLabel_     = nullptr;
    QLabel*      gainLabel_     = nullptr;
    QLabel*      inputStateLabel_ = nullptr;

    LevelMeter* meterInL_  = nullptr;
    LevelMeter* meterInR_  = nullptr;
    LevelMeter* meterOutL_ = nullptr;
    LevelMeter* meterOutR_ = nullptr;

    QPushButton* statusDevice_     = nullptr;   // кликабельно: открывает настройки
    QLabel*      statusLoad_       = nullptr;
    QLabel*      statusXruns_      = nullptr;
    QLabel*      statusViolations_ = nullptr;

    QTimer* telemetryTimer_ = nullptr;

    PeakBuilder*  peakBuilder_   = nullptr;
    TimelineRuler* timelineRuler_ = nullptr;
    TimelineView*  timelineView_  = nullptr;
    MixerView*     mixerView_     = nullptr;
    QDockWidget*   rightDock_     = nullptr;
    QDockWidget*   mixerDock_     = nullptr;
    QWidget*       labelArea_     = nullptr;

    // Предупреждение про обратную связь показывается один раз за сеанс.
    bool monitoringWarningShown_ = false;
};
