#pragma once
//
// Настройки аудио и их сохранение между запусками.
//
// Устройства запоминаются ПО ИМЕНИ, а не по идентификатору. Идентификаторы
// RtAudio не стабильны: они меняются при перезагрузке, при подключении другого
// интерфейса и просто при смене порядка перечисления. Сохранённый ID через
// неделю укажет на чужое устройство — и звук пойдёт не туда, куда ждали.
//
#include <QString>
#include <vector>

#include "daw/audio/AudioDevice.h"

struct AudioSettings {
    QString outputDeviceName;          // пусто = устройство по умолчанию
    QString inputDeviceName;           // пусто = без входа
    int     sampleRate      = 48000;
    int     bufferFrames    = 256;
    bool    autoStart       = true;    // запускать движок при старте приложения
    bool    inputMonitoring = false;   // по умолчанию выключено: обратная связь

    static AudioSettings load();
    void save() const;

    // Превращает имена в идентификаторы, подставляя устройства по умолчанию,
    // если сохранённое не найдено.
    daw::audio::DeviceConfig toConfig(const std::vector<daw::audio::DeviceInfo>& devices,
                                      unsigned int defaultOutputId,
                                      unsigned int defaultInputId) const;

    // Что именно не нашлось при разрешении имён — для честного сообщения
    // пользователю вместо тихой подмены устройства.
    struct Resolution {
        bool    outputFallback = false;
        bool    inputFallback  = false;
        QString missingOutput;
        QString missingInput;
    };
    Resolution resolve(const std::vector<daw::audio::DeviceInfo>& devices) const;
};
