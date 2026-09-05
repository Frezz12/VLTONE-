#include "AudioSettingsPage.hpp"
#include "AudioPreferences.hpp"
#include "EngineController.hpp"
#include "UiConstants.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSet>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <algorithm>

namespace {

QString deviceLabel(const audio::DeviceInfo& device) {
    return QString::fromStdString(device.name);
}

QString channelName(const std::vector<std::string>& names, int channel,
                    const QString& fallback) {
    return channel >= 0 && channel < static_cast<int>(names.size())
        ? QString::fromStdString(names[std::size_t(channel)])
        : fallback.arg(channel + 1);
}

} // namespace

AudioSettingsPage::AudioSettingsPage(daw::EngineController* controller,
                                     QWidget* parent)
    : QWidget(parent), m_controller(controller) {
    m_outputs = m_controller->enumerateOutputDevices();
    m_inputs = m_controller->enumerateInputDevices();

    auto* form = new QFormLayout;
    m_driverCombo = new QComboBox;
    m_driverCombo->setObjectName(QStringLiteral("AudioDriverType"));
    form->addRow(tr("Driver Type"), m_driverCombo);

    m_asioDeviceCombo = new QComboBox;
    m_asioDeviceCombo->setObjectName(QStringLiteral("AsioAudioDevice"));
    m_asioDeviceLabel = new QLabel(tr("Audio Device"));
    form->addRow(m_asioDeviceLabel, m_asioDeviceCombo);

    m_outputCombo = new QComboBox;
    m_outputCombo->setObjectName(QStringLiteral("AudioOutputDevice"));
    m_outputLabel = new QLabel(tr("Output Device"));
    form->addRow(m_outputLabel, m_outputCombo);

    m_inputCombo = new QComboBox;
    m_inputCombo->setObjectName(QStringLiteral("AudioInputDevice"));
    m_inputLabel = new QLabel(tr("Input Device"));
    form->addRow(m_inputLabel, m_inputCombo);

    m_channelRow = new QWidget;
    auto* channelButtons = new QHBoxLayout(m_channelRow);
    channelButtons->setContentsMargins(0, 0, 0, 0);
    m_inputConfig = new QPushButton(tr("Input Config..."));
    m_inputConfig->setObjectName(QStringLiteral("AsioInputConfig"));
    m_outputConfig = new QPushButton(tr("Output Config..."));
    m_outputConfig->setObjectName(QStringLiteral("AsioOutputConfig"));
    channelButtons->addWidget(m_inputConfig);
    channelButtons->addWidget(m_outputConfig);
    m_channelLabel = new QLabel(tr("Channel Configuration"));
    form->addRow(m_channelLabel, m_channelRow);

    m_sampleRateCombo = new QComboBox;
    m_sampleRateCombo->setObjectName(QStringLiteral("AudioSampleRate"));
    form->addRow(tr("Sample Rate"), m_sampleRateCombo);
    m_bufferCombo = new QComboBox;
    m_bufferCombo->setObjectName(QStringLiteral("AudioBufferSize"));
    form->addRow(tr("Buffer Size"), m_bufferCombo);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);

    m_deviceNote = new QLabel;
    m_deviceNote->setWordWrap(true);
    m_deviceNote->setStyleSheet(QStringLiteral("color:palette(mid);"));
    layout->addWidget(m_deviceNote);

    m_bufferNote = new QLabel;
    m_bufferNote->setWordWrap(true);
    m_bufferNote->setStyleSheet(QStringLiteral("color:#c08040;"));
    m_bufferNote->setVisible(false);
    layout->addWidget(m_bufferNote);

    m_cpuStatusBar = new QCheckBox(
        tr("Show audio CPU load in the bottom status bar"), this);
    m_cpuStatusBar->setObjectName(QStringLiteral("ShowCpuStatusBar"));
    m_cpuStatusBar->setAccessibleName(
        tr("Show audio CPU load in the bottom status bar"));
    m_cpuStatusBar->setChecked(
        QSettings().value(ui::kCpuStatusBarVisibleSetting, true).toBool());
    connect(m_cpuStatusBar, &QCheckBox::toggled, this, [this](bool visible) {
        QSettings().setValue(ui::kCpuStatusBarVisibleSetting, visible);
        emit cpuStatusBarVisibilityChanged(visible);
    });
    layout->addWidget(m_cpuStatusBar);
    layout->addStretch(1);

    m_controlPanel = new QPushButton(tr("Hardware Setup..."));
    m_controlPanel->setObjectName(QStringLiteral("AsioHardwareSetup"));
    auto* applyButton = new QPushButton(tr("Apply"));
    applyButton->setObjectName(QStringLiteral("ApplyAudioSettings"));
    auto* buttons = new QHBoxLayout;
    buttons->addWidget(m_controlPanel);
    buttons->addStretch(1);
    buttons->addWidget(applyButton);
    layout->addLayout(buttons);

    QSet<QString> hostApis;
    for (const auto& device : m_outputs)
        hostApis.insert(QString::fromStdString(device.hostApi));
    for (const auto& device : m_inputs)
        hostApis.insert(QString::fromStdString(device.hostApi));
    QStringList drivers = hostApis.values();
    drivers.sort(Qt::CaseInsensitive);
    for (const QString& driver : drivers) m_driverCombo->addItem(driver, driver);

    QString currentDriver;
    const std::string currentOutput = m_controller->currentOutputDeviceUid();
    for (const auto& device : m_outputs) {
        if (device.uid == currentOutput) {
            currentDriver = QString::fromStdString(device.hostApi);
            break;
        }
    }
    if (currentDriver.isEmpty()) currentDriver = ui::loadAudioPreferences().driverType;
    if (const int index = m_driverCombo->findData(currentDriver); index >= 0)
        m_driverCombo->setCurrentIndex(index);

    connect(m_driverCombo, &QComboBox::currentIndexChanged, this,
            &AudioSettingsPage::populateDeviceLists);
    connect(m_asioDeviceCombo, &QComboBox::currentIndexChanged, this,
            &AudioSettingsPage::refreshDeviceCapabilities);
    connect(m_outputCombo, &QComboBox::currentIndexChanged, this,
            &AudioSettingsPage::refreshDeviceCapabilities);
    connect(m_inputConfig, &QPushButton::clicked, this,
            &AudioSettingsPage::configureInputs);
    connect(m_outputConfig, &QPushButton::clicked, this,
            &AudioSettingsPage::configureOutput);
    connect(applyButton, &QPushButton::clicked, this, &AudioSettingsPage::apply);
    connect(m_controlPanel, &QPushButton::clicked, this, [this] {
        const auto* device = selectedOutput();
        if (!device) return;
        const auto shown = m_controller->showDeviceControlPanel(
            device->uid,
            reinterpret_cast<void*>(static_cast<quintptr>(winId())));
        if (!shown) {
            QMessageBox::warning(this, tr("Hardware Setup"),
                                 QString::fromStdString(shown.message()));
            return;
        }
        refreshDeviceCapabilities();
        const auto settled = m_controller->audioConfiguration();
        if (settled.outputDeviceUid == device->uid) {
            m_inputSelectors = settled.inputChannelSelectors;
            m_outputSelectors = settled.outputChannelSelectors;
            syncSettledConfiguration();
            ui::AudioPreferences preferences;
            preferences.driverType = m_driverCombo->currentData().toString();
            preferences.config = settled;
            ui::saveAudioPreferences(preferences);
        }
    });

    populateDeviceLists();
}

bool AudioSettingsPage::isAsioSelected() const {
    return m_driverCombo->currentData().toString().compare(
               QStringLiteral("ASIO"), Qt::CaseInsensitive) == 0;
}

const audio::DeviceInfo* AudioSettingsPage::selectedOutput() const {
    const QString uid = (isAsioSelected() ? m_asioDeviceCombo : m_outputCombo)
                            ->currentData().toString();
    for (const auto& device : m_outputs) {
        if (device.uid == uid.toStdString()) return &device;
    }
    return nullptr;
}

void AudioSettingsPage::populateDeviceLists() {
    const QString driver = m_driverCombo->currentData().toString();
    const bool asio = isAsioSelected();
    const std::string currentOutput = m_controller->currentOutputDeviceUid();
    const std::string currentInput = m_controller->currentInputDeviceUid();

    const QSignalBlocker asioBlock(m_asioDeviceCombo);
    const QSignalBlocker outputBlock(m_outputCombo);
    const QSignalBlocker inputBlock(m_inputCombo);
    m_asioDeviceCombo->clear();
    m_outputCombo->clear();
    m_inputCombo->clear();
    m_inputCombo->addItem(tr("None"), QString());

    for (const auto& device : m_outputs) {
        if (QString::fromStdString(device.hostApi) != driver) continue;
        QComboBox* combo = asio ? m_asioDeviceCombo : m_outputCombo;
        combo->addItem(deviceLabel(device), QString::fromStdString(device.uid));
        if (device.uid == currentOutput)
            combo->setCurrentIndex(combo->count() - 1);
    }
    if (!asio) {
        for (const auto& device : m_inputs) {
            if (QString::fromStdString(device.hostApi) != driver) continue;
            m_inputCombo->addItem(deviceLabel(device),
                                  QString::fromStdString(device.uid));
            if (device.uid == currentInput)
                m_inputCombo->setCurrentIndex(m_inputCombo->count() - 1);
        }
    }

    m_asioDeviceLabel->setVisible(asio);
    m_asioDeviceCombo->setVisible(asio);
    m_outputLabel->setVisible(!asio);
    m_outputCombo->setVisible(!asio);
    m_inputLabel->setVisible(!asio);
    m_inputCombo->setVisible(!asio);
    m_channelLabel->setVisible(asio);
    m_channelRow->setVisible(asio);
    m_controlPanel->setVisible(asio);
    refreshDeviceCapabilities();
}

void AudioSettingsPage::resetChannelsForDevice(
    const audio::DeviceInfo& device) {
    const QString uid = QString::fromStdString(device.uid);
    const bool changed = uid != m_channelDeviceUid;
    if (changed) {
        m_channelDeviceUid = uid;
        const auto current = m_controller->audioConfiguration();
        if (current.outputDeviceUid == device.uid) {
            m_inputSelectors = current.inputEnabled
                ? current.inputChannelSelectors : std::vector<int>{};
            m_outputSelectors = current.outputChannelSelectors;
        } else {
            m_inputSelectors.clear();
            for (int channel = 0;
                 channel < std::min<int>(2, device.inputChannels); ++channel)
                m_inputSelectors.push_back(channel);
            m_outputSelectors.clear();
        }
    }

    std::erase_if(m_inputSelectors, [&](int channel) {
        return channel < 0 || channel >= int(device.inputChannels);
    });
    std::erase_if(m_outputSelectors, [&](int channel) {
        return channel < 0 || channel >= int(device.outputChannels);
    });
    if (m_outputSelectors.size() != 2 && device.outputChannels >= 2)
        m_outputSelectors = {0, 1};
    else if (m_outputSelectors.empty() && device.outputChannels == 1)
        m_outputSelectors = {0};
    updateChannelButtonText();
}

void AudioSettingsPage::refreshDeviceCapabilities() {
    const auto* listed = selectedOutput();
    if (!listed) {
        m_sampleRateCombo->clear();
        m_bufferCombo->clear();
        m_controlPanel->setEnabled(false);
        return;
    }

    const int wantedRate = m_sampleRateCombo->currentData().isValid()
        ? m_sampleRateCombo->currentData().toInt()
        : int(m_controller->sampleRate());
    const int wantedBuffer = m_bufferCombo->currentData().isValid()
        ? m_bufferCombo->currentData().toInt()
        : int(m_controller->bufferSizeFrames());

    m_selectedDevice = *listed;
    (void)m_controller->probeDevice(listed->uid, false, m_selectedDevice);
    if (isAsioSelected() && listed->inputChannels > 0)
        (void)m_controller->probeDevice(listed->uid, true, m_selectedDevice);

    const QSignalBlocker rateBlocker(m_sampleRateCombo);
    m_sampleRateCombo->clear();
    for (double rate : m_selectedDevice.sampleRates)
        m_sampleRateCombo->addItem(QString::number(int(rate)), int(rate));
    if (m_sampleRateCombo->count() == 0)
        m_sampleRateCombo->addItem(QString::number(wantedRate), wantedRate);
    if (const int at = m_sampleRateCombo->findData(wantedRate); at >= 0)
        m_sampleRateCombo->setCurrentIndex(at);

    const QSignalBlocker bufferBlocker(m_bufferCombo);
    m_bufferCombo->clear();
    for (uint32_t size : m_selectedDevice.bufferSizes)
        m_bufferCombo->addItem(QString::number(size), int(size));
    if (m_bufferCombo->count() == 0)
        m_bufferCombo->addItem(QString::number(wantedBuffer), wantedBuffer);
    int bufferIndex = m_bufferCombo->findData(wantedBuffer);
    if (bufferIndex < 0 && m_selectedDevice.preferredBufferSize > 0)
        bufferIndex = m_bufferCombo->findData(
            int(m_selectedDevice.preferredBufferSize));
    if (bufferIndex >= 0) m_bufferCombo->setCurrentIndex(bufferIndex);

    if (isAsioSelected()) {
        resetChannelsForDevice(m_selectedDevice);
        m_deviceNote->setText(
            tr("ASIO uses one driver for input and output. Choose the physical "
               "channels, then press Apply once."));
    } else {
        m_deviceNote->setText(
            tr("Input and output are limited to the selected driver type so "
               "PortAudio never receives an invalid mixed-driver stream."));
    }
    m_controlPanel->setEnabled(m_selectedDevice.hasControlPanel);
}

void AudioSettingsPage::configureInputs() {
    if (!isAsioSelected() || m_selectedDevice.uid.empty()) return;
    QDialog dialog(this);
    dialog.setWindowTitle(tr("ASIO Input Configuration"));
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(
        tr("Enable the physical inputs that should be available to tracks.")));
    auto* list = new QListWidget;
    for (int channel = 0; channel < int(m_selectedDevice.inputChannels); ++channel) {
        auto* item = new QListWidgetItem(
            channelName(m_selectedDevice.inputChannelNames, channel,
                        tr("Input %1")), list);
        item->setData(Qt::UserRole, channel);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(std::find(m_inputSelectors.begin(),
                                      m_inputSelectors.end(), channel) !=
                                    m_inputSelectors.end()
                                ? Qt::Checked : Qt::Unchecked);
    }
    layout->addWidget(list);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
                                         QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) return;

    std::vector<int> selected;
    for (int row = 0; row < list->count(); ++row) {
        if (list->item(row)->checkState() == Qt::Checked)
            selected.push_back(list->item(row)->data(Qt::UserRole).toInt());
    }
    if (selected.size() > 32) {
        QMessageBox::warning(this, tr("ASIO Input Configuration"),
                             tr("At most 32 input channels can be enabled."));
        return;
    }
    m_inputSelectors = std::move(selected);
    updateChannelButtonText();
}

void AudioSettingsPage::configureOutput() {
    if (!isAsioSelected() || m_selectedDevice.uid.empty()) return;
    QStringList labels;
    std::vector<std::vector<int>> pairs;
    for (int channel = 0; channel + 1 < int(m_selectedDevice.outputChannels);
         channel += 2) {
        labels.push_back(QStringLiteral("%1 + %2").arg(
            channelName(m_selectedDevice.outputChannelNames, channel,
                        tr("Output %1")),
            channelName(m_selectedDevice.outputChannelNames, channel + 1,
                        tr("Output %1"))));
        pairs.push_back({channel, channel + 1});
    }
    if (pairs.empty() && m_selectedDevice.outputChannels == 1) {
        labels.push_back(channelName(m_selectedDevice.outputChannelNames, 0,
                                     tr("Output %1")));
        pairs.push_back({0});
    }
    if (pairs.empty()) return;

    int current = 0;
    for (int i = 0; i < int(pairs.size()); ++i) {
        if (pairs[std::size_t(i)] == m_outputSelectors) current = i;
    }
    bool ok = false;
    const QString picked = QInputDialog::getItem(
        this, tr("ASIO Output Configuration"), tr("Master Output"), labels,
        current, false, &ok);
    if (!ok) return;
    const int index = labels.indexOf(picked);
    if (index >= 0) m_outputSelectors = pairs[std::size_t(index)];
    updateChannelButtonText();
}

void AudioSettingsPage::updateChannelButtonText() {
    m_inputConfig->setText(
        tr("Input Config... (%1 enabled)").arg(int(m_inputSelectors.size())));
    QString output = tr("not selected");
    if (!m_outputSelectors.empty()) {
        QStringList names;
        for (int channel : m_outputSelectors)
            names.push_back(channelName(m_selectedDevice.outputChannelNames,
                                        channel, tr("Output %1")));
        output = names.join(QStringLiteral(" + "));
    }
    m_outputConfig->setText(tr("Output Config... (%1)").arg(output));
}

void AudioSettingsPage::syncSettledConfiguration() {
    const auto settled = m_controller->audioConfiguration();
    if (const int at = m_sampleRateCombo->findData(int(settled.sampleRate)); at >= 0)
        m_sampleRateCombo->setCurrentIndex(at);
    if (const int at = m_bufferCombo->findData(int(settled.bufferSize)); at >= 0)
        m_bufferCombo->setCurrentIndex(at);
    m_inputSelectors = settled.inputChannelSelectors;
    m_outputSelectors = settled.outputChannelSelectors;
    updateChannelButtonText();
}

void AudioSettingsPage::apply() {
    audio::AudioDeviceConfig config;
    config.sampleRate = m_sampleRateCombo->currentData().toDouble();
    config.bufferSize = static_cast<uint32_t>(m_bufferCombo->currentData().toUInt());

    if (isAsioSelected()) {
        config.outputDeviceUid =
            m_asioDeviceCombo->currentData().toString().toStdString();
        config.inputDeviceUid = config.outputDeviceUid;
        config.inputEnabled = !m_inputSelectors.empty();
        config.inputChannelSelectors = m_inputSelectors;
        config.outputChannelSelectors = m_outputSelectors;
    } else {
        config.outputDeviceUid =
            m_outputCombo->currentData().toString().toStdString();
        config.inputDeviceUid =
            m_inputCombo->currentData().toString().toStdString();
        config.inputEnabled = !config.inputDeviceUid.empty();
    }

    if (config.outputDeviceUid.empty()) {
        QMessageBox::warning(this, tr("Audio Settings"),
                             tr("Choose an output device."));
        return;
    }
    const auto result = m_controller->applyAudioConfiguration(config);
    if (!result) {
        syncSettledConfiguration();
        m_bufferNote->setText(
            tr("Could not apply this configuration. The previous audio device "
               "is still active."));
        m_bufferNote->setVisible(true);
        QMessageBox::warning(this, tr("Audio Settings"),
                             QString::fromStdString(result.message()));
        return;
    }

    const auto settled = m_controller->audioConfiguration();
    ui::AudioPreferences preferences;
    preferences.driverType = m_driverCombo->currentData().toString();
    preferences.config = settled;
    ui::saveAudioPreferences(preferences);
    syncSettledConfiguration();
    m_bufferNote->setVisible(false);
}

bool AudioSettingsPage::checkForTest() const {
    if (!m_driverCombo || !m_asioDeviceCombo || !m_outputCombo ||
        !m_inputCombo || !m_inputConfig || !m_outputConfig ||
        !m_controlPanel || !m_cpuStatusBar ||
        m_cpuStatusBar->isChecked() !=
            QSettings().value(ui::kCpuStatusBarVisibleSetting, true).toBool()) {
        return false;
    }
    const QString driver = m_driverCombo->currentData().toString();
    const QComboBox* output = isAsioSelected() ? m_asioDeviceCombo : m_outputCombo;
    for (int row = 0; row < output->count(); ++row) {
        const std::string uid = output->itemData(row).toString().toStdString();
        const auto found = std::find_if(m_outputs.begin(), m_outputs.end(),
                                        [&](const auto& device) {
            return device.uid == uid &&
                   QString::fromStdString(device.hostApi) == driver;
        });
        if (found == m_outputs.end()) return false;
    }
    return true;
}
