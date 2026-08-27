#pragma once

#include "Device/AudioDeviceManager.hpp"

#include <QString>

class QSettings;

namespace ui {

struct AudioPreferences {
    AudioPreferences() { config.sampleRate = 48000.0; }
    QString driverType;
    audio::AudioDeviceConfig config;
};

AudioPreferences loadAudioPreferences();
AudioPreferences loadAudioPreferences(QSettings& settings);
void saveAudioPreferences(const AudioPreferences& preferences);
void saveAudioPreferences(const AudioPreferences& preferences,
                          QSettings& settings);

/// Deterministic persistence regression check used by the existing app
/// self-test. It writes only to a temporary INI file.
bool checkAudioPreferencesRoundTripForTest();

} // namespace ui
