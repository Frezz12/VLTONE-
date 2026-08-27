#pragma once

#include "Device/AudioDeviceManager.hpp"

#include <QWidget>

#include <vector>

namespace daw { class EngineController; }

class QComboBox;
class QLabel;
class QPushButton;

/// Ableton-style audio setup: driver family first, then either one ASIO driver
/// with physical-channel configuration or separate portable input/output
/// endpoints. Apply commits the complete form in one device transaction.
class AudioSettingsPage : public QWidget {
    Q_OBJECT
public:
    explicit AudioSettingsPage(daw::EngineController* controller,
                               QWidget* parent = nullptr);

    void apply();
    bool checkForTest() const;

private slots:
    void populateDeviceLists();
    void refreshDeviceCapabilities();
    void configureInputs();
    void configureOutput();

private:
    bool isAsioSelected() const;
    const audio::DeviceInfo* selectedOutput() const;
    void resetChannelsForDevice(const audio::DeviceInfo& device);
    void updateChannelButtonText();
    void syncSettledConfiguration();

    daw::EngineController* m_controller = nullptr;
    std::vector<audio::DeviceInfo> m_outputs;
    std::vector<audio::DeviceInfo> m_inputs;
    audio::DeviceInfo m_selectedDevice;

    QComboBox* m_driverCombo = nullptr;
    QComboBox* m_asioDeviceCombo = nullptr;
    QComboBox* m_outputCombo = nullptr;
    QComboBox* m_inputCombo = nullptr;
    QComboBox* m_sampleRateCombo = nullptr;
    QComboBox* m_bufferCombo = nullptr;
    QLabel* m_asioDeviceLabel = nullptr;
    QLabel* m_outputLabel = nullptr;
    QLabel* m_inputLabel = nullptr;
    QLabel* m_channelLabel = nullptr;
    QWidget* m_channelRow = nullptr;
    QPushButton* m_inputConfig = nullptr;
    QPushButton* m_outputConfig = nullptr;
    QLabel* m_bufferNote = nullptr;
    QPushButton* m_controlPanel = nullptr;
    QLabel* m_deviceNote = nullptr;

    QString m_channelDeviceUid;
    std::vector<int> m_inputSelectors;
    std::vector<int> m_outputSelectors;
};
