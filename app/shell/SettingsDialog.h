#pragma once
//
// Диалог настроек с вкладками. Сейчас содержательна одна вкладка — «Аудио»;
// остальные появятся по мере готовности подсистем, и добавлять пустые
// заглушки заранее незачем.
//
// Диалог ничего не меняет в движке напрямую: он собирает настройки и отдаёт
// их сигналом. Решение, когда перезапускать поток, принимает MainWindow.
//
#include <QDialog>
#include <vector>

#include "AudioSettings.h"
#include "daw/audio/AudioDevice.h"

class QComboBox;
class QCheckBox;
class QDialogButtonBox;
class QLabel;
class QTabWidget;

class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    SettingsDialog(const AudioSettings& settings,
                   std::vector<daw::audio::DeviceInfo> devices,
                   QString apiName,
                   QWidget* parent = nullptr);

    AudioSettings settings() const;

    // Текущее состояние потока — показывается в блоке «Состояние».
    void setStreamInfo(const daw::audio::StreamInfo& info, const QString& warning);

signals:
    // Испускается по «Применить» и по OK. MainWindow перезапускает движок.
    void settingsApplied(const AudioSettings& settings);

private slots:
    void onDeviceChanged();
    void onApply();

private:
    QWidget* buildAudioTab();
    void     populateDevices();
    void     updateLatencyEstimate();

    AudioSettings                       settings_;
    std::vector<daw::audio::DeviceInfo> devices_;
    QString                             apiName_;

    QTabWidget*       tabs_          = nullptr;
    QComboBox*        outputCombo_   = nullptr;
    QComboBox*        inputCombo_    = nullptr;
    QComboBox*        rateCombo_     = nullptr;
    QComboBox*        bufferCombo_   = nullptr;
    QCheckBox*        autoStartBox_  = nullptr;
    QLabel*           latencyLabel_  = nullptr;
    QLabel*           statusLabel_   = nullptr;
    QLabel*           warningLabel_  = nullptr;
    QDialogButtonBox* buttons_       = nullptr;
};
