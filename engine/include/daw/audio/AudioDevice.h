#pragma once
//
// Абстракция над аудио-устройством. Реализация на RtAudio спрятана в .cpp:
// заголовки движка не должны тянуть за собой RtAudio, чтобы backend можно было
// заменить (позже — на нативный WASAPI/CoreAudio) без правки всего остального.
//
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "daw/audio/AudioBuffer.h"

namespace daw::audio {

// Отсутствие устройства. Ноль не годится: RtAudio выдаёт валидные ID с единицы,
// но нулевой ID тоже встречается как признак ошибки.
inline constexpr unsigned int kNoDevice = 0xFFFFFFFFu;

struct DeviceInfo {
    unsigned int              id = kNoDevice;
    std::string               name;
    std::string               apiName;
    int                       outputChannels = 0;
    int                       inputChannels  = 0;
    int                       duplexChannels = 0;
    std::vector<unsigned int> sampleRates;
    unsigned int              preferredSampleRate = 0;
    bool                      isDefaultOutput = false;
    bool                      isDefaultInput  = false;

    bool hasOutput() const noexcept { return outputChannels > 0; }
    bool hasInput()  const noexcept { return inputChannels  > 0; }
};

struct DeviceConfig {
    unsigned int outputDeviceId = kNoDevice;
    unsigned int inputDeviceId  = kNoDevice;   // kNoDevice = работать без входа
    double       sampleRate     = 48000.0;
    int          bufferFrames   = 256;
    int          maxInputChannels = 2;
};

// Что реально получилось открыть. Драйвер вправе выдать не то, что просили,
// и об этом должен узнать пользователь, а не только лог.
struct StreamInfo {
    bool         isOpen         = false;
    unsigned int outputDeviceId = kNoDevice;
    unsigned int inputDeviceId  = kNoDevice;
    std::string  outputDeviceName;
    std::string  inputDeviceName;
    int          outputChannels = 0;
    int          inputChannels  = 0;
    double       sampleRate     = 0.0;
    int          bufferFrames   = 0;
    int          latencyFrames  = 0;   // по данным драйвера, вход + выход

    bool hasInput() const noexcept { return inputChannels > 0; }
};

// Интерфейс обработчика. process() вызывается из аудио-потока и обязан
// соблюдать все правила реального времени.
class AudioCallback {
public:
    virtual ~AudioCallback() = default;

    // Вызывается вне аудио-потока до старта. Здесь можно и нужно аллоцировать.
    virtual void prepare(double sampleRate, int maxBlockSize,
                         int inputChannels, int outputChannels) = 0;

    // Аудио-поток. Никаких аллокаций, локов, файлов, исключений.
    // input может быть пустым (0 каналов), если вход не открыт.
    virtual void process(const AudioBufferView& input,
                         AudioBufferView& output,
                         int numFrames) noexcept = 0;

    // Вызывается вне аудио-потока после остановки.
    virtual void release() = 0;
};

class AudioDevice {
public:
    AudioDevice();
    ~AudioDevice();
    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;

    // Все устройства разом: у одного и того же интерфейса обычно есть и вход,
    // и выход, и разделять перечисление незачем.
    std::vector<DeviceInfo> devices() const;
    unsigned int            defaultOutputDeviceId() const;
    unsigned int            defaultInputDeviceId() const;
    std::string             currentApiName() const;

    bool open(const DeviceConfig& config, AudioCallback* callback);
    void close();
    bool isOpen() const noexcept;

    const StreamInfo& streamInfo() const noexcept;

    double sampleRate() const noexcept;
    int    bufferSize() const noexcept;

    // Доля бюджета реального времени, занятая обработкой: 0..1+.
    // Больше 1 означает, что обработка не укладывается в буфер.
    double        cpuLoad() const noexcept;
    std::uint64_t xruns() const noexcept;

    const std::string& lastError() const noexcept;

    // Непустая строка означает, что поток открыт, но не так, как просили:
    // например, вход не завёлся и работаем только на выход. Молчать об этом
    // нельзя — пользователь будет искать, почему не пишется звук.
    const std::string& lastWarning() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace daw::audio
