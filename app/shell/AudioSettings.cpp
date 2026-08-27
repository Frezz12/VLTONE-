#include "AudioSettings.h"

#include <QSettings>

namespace {

constexpr auto kKeyOutput      = "audio/outputDeviceName";
constexpr auto kKeyInput       = "audio/inputDeviceName";
constexpr auto kKeySampleRate  = "audio/sampleRate";
constexpr auto kKeyBuffer      = "audio/bufferFrames";
constexpr auto kKeyAutoStart   = "audio/autoStart";
constexpr auto kKeyMonitoring  = "audio/inputMonitoring";

// Поиск по имени. Точное совпадение: имена устройств стабильны, а нечёткий
// поиск подобрал бы «Speakers (Realtek)» вместо «Speakers (USB Audio)».
const daw::audio::DeviceInfo* findByName(const std::vector<daw::audio::DeviceInfo>& devices,
                                         const QString& name, bool needInput) {
    if (name.isEmpty())
        return nullptr;

    for (const auto& d : devices) {
        if (QString::fromStdString(d.name) != name)
            continue;
        if (needInput ? d.hasInput() : d.hasOutput())
            return &d;
    }
    return nullptr;
}

} // namespace

AudioSettings AudioSettings::load() {
    QSettings settings;
    AudioSettings result;

    result.outputDeviceName = settings.value(kKeyOutput).toString();
    result.inputDeviceName  = settings.value(kKeyInput).toString();
    result.sampleRate       = settings.value(kKeySampleRate, 48000).toInt();
    result.bufferFrames     = settings.value(kKeyBuffer, 256).toInt();
    result.autoStart        = settings.value(kKeyAutoStart, true).toBool();
    result.inputMonitoring  = settings.value(kKeyMonitoring, false).toBool();

    // Испорченный или устаревший конфиг не должен ронять запуск.
    if (result.sampleRate < 8000 || result.sampleRate > 384000)
        result.sampleRate = 48000;
    if (result.bufferFrames < 16 || result.bufferFrames > 8192)
        result.bufferFrames = 256;

    return result;
}

void AudioSettings::save() const {
    QSettings settings;
    settings.setValue(kKeyOutput,     outputDeviceName);
    settings.setValue(kKeyInput,      inputDeviceName);
    settings.setValue(kKeySampleRate, sampleRate);
    settings.setValue(kKeyBuffer,     bufferFrames);
    settings.setValue(kKeyAutoStart,  autoStart);
    settings.setValue(kKeyMonitoring, inputMonitoring);
}

AudioSettings::Resolution
AudioSettings::resolve(const std::vector<daw::audio::DeviceInfo>& devices) const {
    Resolution result;

    if (!outputDeviceName.isEmpty() && !findByName(devices, outputDeviceName, false)) {
        result.outputFallback = true;
        result.missingOutput  = outputDeviceName;
    }
    if (!inputDeviceName.isEmpty() && !findByName(devices, inputDeviceName, true)) {
        result.inputFallback = true;
        result.missingInput  = inputDeviceName;
    }
    return result;
}

daw::audio::DeviceConfig
AudioSettings::toConfig(const std::vector<daw::audio::DeviceInfo>& devices,
                        unsigned int defaultOutputId,
                        unsigned int defaultInputId) const {
    daw::audio::DeviceConfig config;
    config.sampleRate   = sampleRate;
    config.bufferFrames = bufferFrames;

    if (const auto* d = findByName(devices, outputDeviceName, false))
        config.outputDeviceId = d->id;
    else
        config.outputDeviceId = defaultOutputId;

    if (inputDeviceName.isEmpty()) {
        // Пустое имя означает осознанный отказ от входа, а не «подставь любой».
        config.inputDeviceId = daw::audio::kNoDevice;
    } else if (const auto* d = findByName(devices, inputDeviceName, true)) {
        config.inputDeviceId = d->id;
    } else {
        config.inputDeviceId = defaultInputId;
    }

    return config;
}
