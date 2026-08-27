#include "AudioPreferences.hpp"

#include <QSettings>
#include <QTemporaryDir>
#include <QVariantList>

#include <algorithm>

namespace ui {
namespace {

constexpr auto kDriverType = "audio/driverType";
constexpr auto kOutputUid = "audio/outputDeviceUid";
constexpr auto kInputUid = "audio/inputDeviceUid";
constexpr auto kInputEnabled = "audio/inputEnabled";
constexpr auto kSampleRate = "audio/sampleRate";
constexpr auto kBufferFrames = "audio/bufferFrames";
constexpr auto kInputChannels = "audio/asioInputChannels";
constexpr auto kOutputChannels = "audio/asioOutputChannels";

std::vector<int> readChannels(const QVariant& value, std::size_t maximum) {
    std::vector<int> channels;
    for (const QVariant& item : value.toList()) {
        bool ok = false;
        const int channel = item.toInt(&ok);
        if (!ok || channel < 0 ||
            std::find(channels.begin(), channels.end(), channel) != channels.end())
            continue;
        channels.push_back(channel);
        if (channels.size() == maximum) break;
    }
    return channels;
}

QVariantList writeChannels(const std::vector<int>& channels) {
    QVariantList values;
    for (int channel : channels) values.push_back(channel);
    return values;
}

} // namespace

AudioPreferences loadAudioPreferences(QSettings& settings) {
    AudioPreferences preferences;
    preferences.driverType = settings.value(kDriverType).toString();
    preferences.config.outputDeviceUid =
        settings.value(kOutputUid).toString().toStdString();
    preferences.config.inputDeviceUid =
        settings.value(kInputUid).toString().toStdString();
    preferences.config.inputEnabled =
        settings.value(kInputEnabled, true).toBool();
    preferences.config.sampleRate =
        settings.value(kSampleRate, 48000).toDouble();
    preferences.config.bufferSize = static_cast<audio::BufferSize>(
        settings.value(kBufferFrames, 512).toUInt());
    preferences.config.inputChannelSelectors =
        readChannels(settings.value(kInputChannels), 32);
    preferences.config.outputChannelSelectors =
        readChannels(settings.value(kOutputChannels), 2);

    if (preferences.config.sampleRate < audio::kMinSampleRate ||
        preferences.config.sampleRate > audio::kMaxSampleRate)
        preferences.config.sampleRate = 48000.0;
    if (preferences.config.bufferSize < 8 ||
        preferences.config.bufferSize > 8192)
        preferences.config.bufferSize = 512;
    return preferences;
}

AudioPreferences loadAudioPreferences() {
    QSettings settings;
    return loadAudioPreferences(settings);
}

void saveAudioPreferences(const AudioPreferences& preferences,
                          QSettings& settings) {
    settings.setValue(kDriverType, preferences.driverType);
    settings.setValue(kOutputUid,
                      QString::fromStdString(preferences.config.outputDeviceUid));
    settings.setValue(kInputUid,
                      QString::fromStdString(preferences.config.inputDeviceUid));
    settings.setValue(kInputEnabled, preferences.config.inputEnabled);
    settings.setValue(kSampleRate, preferences.config.sampleRate);
    settings.setValue(kBufferFrames, preferences.config.bufferSize);
    settings.setValue(kInputChannels,
                      writeChannels(preferences.config.inputChannelSelectors));
    settings.setValue(kOutputChannels,
                      writeChannels(preferences.config.outputChannelSelectors));
    settings.sync();
}

void saveAudioPreferences(const AudioPreferences& preferences) {
    QSettings settings;
    saveAudioPreferences(preferences, settings);
}

bool checkAudioPreferencesRoundTripForTest() {
    QTemporaryDir dir;
    if (!dir.isValid()) return false;
    const QString path = dir.filePath(QStringLiteral("audio.ini"));

    AudioPreferences expected;
    expected.driverType = QStringLiteral("ASIO");
    expected.config.outputDeviceUid = "ASIO: Test Driver";
    expected.config.inputDeviceUid = "ASIO: Test Driver";
    expected.config.inputEnabled = false;
    expected.config.sampleRate = 48000.0;
    expected.config.bufferSize = 32;
    expected.config.inputChannelSelectors = {0, 3, 7};
    expected.config.outputChannelSelectors = {2, 3};
    {
        QSettings settings(path, QSettings::IniFormat);
        saveAudioPreferences(expected, settings);
    }
    QSettings settings(path, QSettings::IniFormat);
    const auto actual = loadAudioPreferences(settings);
    return actual.driverType == expected.driverType &&
           actual.config.outputDeviceUid == expected.config.outputDeviceUid &&
           actual.config.inputDeviceUid == expected.config.inputDeviceUid &&
           actual.config.inputEnabled == expected.config.inputEnabled &&
           actual.config.sampleRate == expected.config.sampleRate &&
           actual.config.bufferSize == expected.config.bufferSize &&
           actual.config.inputChannelSelectors ==
               expected.config.inputChannelSelectors &&
           actual.config.outputChannelSelectors ==
               expected.config.outputChannelSelectors;
}

} // namespace ui
