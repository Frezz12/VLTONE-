#include "SettingsDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QTabWidget>
#include <QVBoxLayout>

namespace {

const int kSampleRates[]  = {44100, 48000, 88200, 96000, 176400, 192000};
const int kBufferSizes[]  = {32, 64, 128, 256, 512, 1024, 2048};

// Значение combobox для «вход не используется».
constexpr int kNoInputIndex = 0;

QString describeDevice(const daw::audio::DeviceInfo& device, bool forInput) {
    const int channels = forInput ? device.inputChannels : device.outputChannels;
    QString text = QString::fromStdString(device.name);

    const bool isDefault = forInput ? device.isDefaultInput : device.isDefaultOutput;
    if (isDefault)
        text += QObject::tr("  [по умолчанию]");

    text += QStringLiteral("  — %1 кан.").arg(channels);
    return text;
}

} // namespace

SettingsDialog::SettingsDialog(const AudioSettings& settings,
                               std::vector<daw::audio::DeviceInfo> devices,
                               QString apiName,
                               QWidget* parent)
    : QDialog(parent),
      settings_(settings),
      devices_(std::move(devices)),
      apiName_(std::move(apiName)) {

    setWindowTitle(tr("Настройки"));
    setModal(true);
    resize(620, 520);

    auto* layout = new QVBoxLayout(this);

    tabs_ = new QTabWidget(this);
    tabs_->addTab(buildAudioTab(), tr("Аудио"));
    layout->addWidget(tabs_);

    buttons_ = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply, this);
    layout->addWidget(buttons_);

    connect(buttons_, &QDialogButtonBox::accepted, this, [this] {
        onApply();
        accept();
    });
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons_->button(QDialogButtonBox::Apply), &QPushButton::clicked,
            this, &SettingsDialog::onApply);

    populateDevices();
    updateLatencyEstimate();
}

QWidget* SettingsDialog::buildAudioTab() {
    auto* page   = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    // ---- устройства -------------------------------------------------------
    auto* deviceGroup = new QGroupBox(tr("Устройства"), page);
    auto* deviceForm  = new QFormLayout(deviceGroup);

    outputCombo_ = new QComboBox(deviceGroup);
    deviceForm->addRow(tr("Выход:"), outputCombo_);

    inputCombo_ = new QComboBox(deviceGroup);
    deviceForm->addRow(tr("Вход:"), inputCombo_);

    auto* apiLabel = new QLabel(apiName_.isEmpty() ? tr("неизвестно") : apiName_, deviceGroup);
    apiLabel->setStyleSheet(QStringLiteral("color: #8a9096;"));
    deviceForm->addRow(tr("Драйвер:"), apiLabel);

    layout->addWidget(deviceGroup);

    // ---- параметры потока -------------------------------------------------
    auto* streamGroup = new QGroupBox(tr("Параметры потока"), page);
    auto* streamForm  = new QFormLayout(streamGroup);

    rateCombo_ = new QComboBox(streamGroup);
    for (int rate : kSampleRates)
        rateCombo_->addItem(QStringLiteral("%1 Гц").arg(rate), rate);
    streamForm->addRow(tr("Частота дискретизации:"), rateCombo_);

    bufferCombo_ = new QComboBox(streamGroup);
    for (int frames : kBufferSizes)
        bufferCombo_->addItem(QStringLiteral("%1 сэмплов").arg(frames), frames);
    streamForm->addRow(tr("Размер буфера:"), bufferCombo_);

    latencyLabel_ = new QLabel(streamGroup);
    latencyLabel_->setStyleSheet(QStringLiteral("color: #8a9096;"));
    streamForm->addRow(tr("Задержка:"), latencyLabel_);

    layout->addWidget(streamGroup);

    // ---- поведение --------------------------------------------------------
    auto* behaviourGroup = new QGroupBox(tr("Поведение"), page);
    auto* behaviourLayout = new QVBoxLayout(behaviourGroup);

    autoStartBox_ = new QCheckBox(tr("Запускать звук при открытии программы"), behaviourGroup);
    autoStartBox_->setChecked(settings_.autoStart);
    behaviourLayout->addWidget(autoStartBox_);

    layout->addWidget(behaviourGroup);

    // ---- состояние --------------------------------------------------------
    auto* statusGroup  = new QGroupBox(tr("Состояние"), page);
    auto* statusLayout = new QVBoxLayout(statusGroup);

    statusLabel_ = new QLabel(tr("Звук не запущен"), statusGroup);
    statusLabel_->setWordWrap(true);
    statusLayout->addWidget(statusLabel_);

    warningLabel_ = new QLabel(statusGroup);
    warningLabel_->setWordWrap(true);
    warningLabel_->setStyleSheet(QStringLiteral("color: #d8a03a;"));
    warningLabel_->hide();
    statusLayout->addWidget(warningLabel_);

    layout->addWidget(statusGroup);
    layout->addStretch();

    connect(outputCombo_, &QComboBox::currentIndexChanged, this, &SettingsDialog::onDeviceChanged);
    connect(inputCombo_,  &QComboBox::currentIndexChanged, this, &SettingsDialog::onDeviceChanged);
    connect(rateCombo_,   &QComboBox::currentIndexChanged, this, &SettingsDialog::onDeviceChanged);
    connect(bufferCombo_, &QComboBox::currentIndexChanged, this, &SettingsDialog::onDeviceChanged);

    return page;
}

void SettingsDialog::populateDevices() {
    const QSignalBlocker blockOutput(outputCombo_);
    const QSignalBlocker blockInput(inputCombo_);
    const QSignalBlocker blockRate(rateCombo_);
    const QSignalBlocker blockBuffer(bufferCombo_);

    outputCombo_->clear();
    inputCombo_->clear();

    inputCombo_->addItem(tr("Не использовать"), QString());

    int outputIndex        = -1;
    int outputDefaultIndex = -1;
    int inputIndex         = kNoInputIndex;

    for (const auto& device : devices_) {
        const QString name = QString::fromStdString(device.name);

        if (device.hasOutput()) {
            outputCombo_->addItem(describeDevice(device, false), name);
            if (name == settings_.outputDeviceName)
                outputIndex = outputCombo_->count() - 1;
            if (device.isDefaultOutput && outputDefaultIndex < 0)
                outputDefaultIndex = outputCombo_->count() - 1;
        }

        if (device.hasInput()) {
            inputCombo_->addItem(describeDevice(device, true), name);
            if (name == settings_.inputDeviceName)
                inputIndex = inputCombo_->count() - 1;
        }
    }

    // Если сохранённое имя не нашлось, выбираем устройство ПО УМОЛЧАНИЮ — то
    // же самое, что подставит AudioSettings::toConfig при запуске потока.
    // Иначе диалог показывал бы первое попавшееся устройство из перечисления,
    // противоречил статусной строке, а нажатие OK уводило бы звук на него.
    if (outputIndex < 0)
        outputIndex = outputDefaultIndex >= 0 ? outputDefaultIndex : 0;

    // Пустое имя входа — это осознанное «не использовать», а не «не нашлось».
    if (inputIndex == kNoInputIndex && !settings_.inputDeviceName.isEmpty()) {
        for (int i = 1; i < inputCombo_->count(); ++i) {
            const QString name = inputCombo_->itemData(i).toString();
            for (const auto& device : devices_) {
                if (QString::fromStdString(device.name) == name && device.isDefaultInput) {
                    inputIndex = i;
                    break;
                }
            }
            if (inputIndex != kNoInputIndex)
                break;
        }
    }

    outputCombo_->setCurrentIndex(outputIndex);
    inputCombo_->setCurrentIndex(inputIndex);

    // Сохранённое устройство могло исчезнуть — говорим об этом прямо.
    if (!settings_.outputDeviceName.isEmpty()
        && outputCombo_->currentData().toString() != settings_.outputDeviceName) {
        warningLabel_->setText(tr("Устройство «%1» не найдено, выбрано другое.")
                                   .arg(settings_.outputDeviceName));
        warningLabel_->show();
    }

    int rateIndex = rateCombo_->findData(settings_.sampleRate);
    rateCombo_->setCurrentIndex(rateIndex >= 0 ? rateIndex : 1);

    int bufferIndex = bufferCombo_->findData(settings_.bufferFrames);
    bufferCombo_->setCurrentIndex(bufferIndex >= 0 ? bufferIndex : 3);
}

void SettingsDialog::updateLatencyEstimate() {
    const int    frames = bufferCombo_->currentData().toInt();
    const double rate   = rateCombo_->currentData().toDouble();
    if (rate <= 0.0) {
        latencyLabel_->clear();
        return;
    }

    const double blockMs = frames * 1000.0 / rate;

    // Честно показываем, что это оценка одного буфера. Реальный round-trip
    // включает буферизацию драйвера и АЦП/ЦАП и обычно вдвое-втрое больше;
    // фактическое значение от драйвера показывается в блоке «Состояние».
    latencyLabel_->setText(tr("%1 мс на буфер (оценка; фактическую см. в «Состоянии»)")
                               .arg(blockMs, 0, 'f', 2));
}

void SettingsDialog::onDeviceChanged() {
    updateLatencyEstimate();
}

void SettingsDialog::onApply() {
    settings_.outputDeviceName = outputCombo_->currentData().toString();
    settings_.inputDeviceName  = inputCombo_->currentData().toString();
    settings_.sampleRate       = rateCombo_->currentData().toInt();
    settings_.bufferFrames     = bufferCombo_->currentData().toInt();
    settings_.autoStart        = autoStartBox_->isChecked();

    emit settingsApplied(settings_);
}

AudioSettings SettingsDialog::settings() const { return settings_; }

void SettingsDialog::setStreamInfo(const daw::audio::StreamInfo& info, const QString& warning) {
    if (!info.isOpen) {
        statusLabel_->setText(tr("Звук не запущен"));
    } else {
        const double bufferMs = info.sampleRate > 0.0
                              ? info.bufferFrames * 1000.0 / info.sampleRate : 0.0;
        const double latencyMs = info.sampleRate > 0.0
                               ? info.latencyFrames * 1000.0 / info.sampleRate : 0.0;

        QString text = tr("Выход: %1 (%2 кан.)")
                           .arg(QString::fromStdString(info.outputDeviceName))
                           .arg(info.outputChannels);

        text += info.hasInput()
                    ? tr("\nВход: %1 (%2 кан.)")
                          .arg(QString::fromStdString(info.inputDeviceName))
                          .arg(info.inputChannels)
                    : tr("\nВход: не используется");

        text += tr("\n%1 Гц, буфер %2 сэмплов (%3 мс)")
                    .arg(info.sampleRate, 0, 'f', 0)
                    .arg(info.bufferFrames)
                    .arg(bufferMs, 0, 'f', 2);

        if (info.latencyFrames > 0)
            text += tr("\nЗадержка по данным драйвера: %1 сэмплов (%2 мс)")
                        .arg(info.latencyFrames).arg(latencyMs, 0, 'f', 2);

        statusLabel_->setText(text);
    }

    if (warning.isEmpty()) {
        warningLabel_->hide();
    } else {
        warningLabel_->setText(warning);
        warningLabel_->show();
    }
}
