#pragma once

#include <QJsonObject>

/// Privacy-bounded platform metrics. It intentionally exposes no user name,
/// serial number, MAC address, project name, file path, audio, or MIDI data.
class PlatformDiagnostics {
public:
    static QJsonObject hardwareSnapshot();
    static QJsonObject processSample();
};
