#include "daw/audio/AudioDevice.h"

#include <RtAudio.h>

#include <atomic>
#include <chrono>
#include <cstring>

#include "daw/rt/RtGuard.h"

namespace daw::audio {

struct AudioDevice::Impl {
    RtAudio        rt{RtAudio::UNSPECIFIED};
    AudioCallback* callback = nullptr;

    StreamInfo info;

    // Массивы указателей на каналы. Выделяются при open(); в аудио-потоке
    // только перезаписываются значениями — аллокация в callback'е недопустима.
    std::vector<float*> outputPtrs;
    std::vector<float*> inputPtrs;

    std::atomic<double>        cpuLoad{0.0};
    std::atomic<std::uint64_t> xruns{0};

    std::string lastError;
    std::string lastWarning;

    static int rtCallback(void* out, void* in, unsigned int nFrames,
                          double /*streamTime*/, RtAudioStreamStatus status,
                          void* userData) noexcept {
        auto* self = static_cast<Impl*>(userData);

        if (status & (RTAUDIO_OUTPUT_UNDERFLOW | RTAUDIO_INPUT_OVERFLOW))
            self->xruns.fetch_add(1, std::memory_order_relaxed);

        const auto t0 = std::chrono::steady_clock::now();

        {
            // С этого момента и до конца блока любая аллокация — нарушение.
            daw::rt::ScopedAudioThread audioThread;
            daw::rt::ScopedNoDenormals noDenormals;

            // RTAUDIO_NONINTERLEAVED: буферы уложены канал за каналом, что
            // совпадает с внутренним планарным форматом движка.
            const int outCh = self->info.outputChannels;
            auto* outBase = static_cast<float*>(out);
            for (int c = 0; c < outCh; ++c)
                self->outputPtrs[static_cast<std::size_t>(c)] =
                    outBase + static_cast<std::size_t>(c) * nFrames;

            AudioBufferView outputView(self->outputPtrs.data(), outCh, static_cast<int>(nFrames));
            outputView.clear();

            // Вход может отсутствовать: тогда обработчик получает пустой вид.
            AudioBufferView inputView;
            const int inCh = self->info.inputChannels;
            if (in != nullptr && inCh > 0) {
                auto* inBase = static_cast<float*>(in);
                for (int c = 0; c < inCh; ++c)
                    self->inputPtrs[static_cast<std::size_t>(c)] =
                        inBase + static_cast<std::size_t>(c) * nFrames;
                inputView = AudioBufferView(self->inputPtrs.data(), inCh, static_cast<int>(nFrames));
            }

            if (self->callback)
                self->callback->process(inputView, outputView, static_cast<int>(nFrames));
        }

        const auto t1 = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(t1 - t0).count();
        const double budget  = static_cast<double>(nFrames) / self->info.sampleRate;
        if (budget > 0.0) {
            // Сглаживаем, иначе показание скачет и по нему ничего не понять.
            const double instant = elapsed / budget;
            const double prev = self->cpuLoad.load(std::memory_order_relaxed);
            self->cpuLoad.store(prev + (instant - prev) * 0.05, std::memory_order_relaxed);
        }

        return 0;
    }
};

AudioDevice::AudioDevice() : impl_(std::make_unique<Impl>()) {}

AudioDevice::~AudioDevice() { close(); }

std::vector<DeviceInfo> AudioDevice::devices() const {
    std::vector<DeviceInfo> result;
    const std::string apiName = RtAudio::getApiDisplayName(impl_->rt.getCurrentApi());

    for (unsigned int id : impl_->rt.getDeviceIds()) {
        RtAudio::DeviceInfo info = impl_->rt.getDeviceInfo(id);
        if (info.outputChannels == 0 && info.inputChannels == 0)
            continue;

        DeviceInfo d;
        d.id                  = id;
        d.name                = info.name;
        d.apiName             = apiName;
        d.outputChannels      = static_cast<int>(info.outputChannels);
        d.inputChannels       = static_cast<int>(info.inputChannels);
        d.duplexChannels      = static_cast<int>(info.duplexChannels);
        d.sampleRates         = info.sampleRates;
        d.preferredSampleRate = info.preferredSampleRate;
        d.isDefaultOutput     = info.isDefaultOutput;
        d.isDefaultInput      = info.isDefaultInput;
        result.push_back(std::move(d));
    }
    return result;
}

unsigned int AudioDevice::defaultOutputDeviceId() const {
    return impl_->rt.getDefaultOutputDevice();
}

unsigned int AudioDevice::defaultInputDeviceId() const {
    return impl_->rt.getDefaultInputDevice();
}

std::string AudioDevice::currentApiName() const {
    return RtAudio::getApiDisplayName(impl_->rt.getCurrentApi());
}

bool AudioDevice::open(const DeviceConfig& config, AudioCallback* callback) {
    close();
    impl_->lastError.clear();
    impl_->lastWarning.clear();

    if (!callback) {
        impl_->lastError = "Не задан обработчик";
        return false;
    }

    RtAudio::DeviceInfo outInfo = impl_->rt.getDeviceInfo(config.outputDeviceId);
    if (outInfo.outputChannels == 0) {
        impl_->lastError = "Устройство вывода недоступно или не имеет выходных каналов";
        return false;
    }

    const int outChannels = outInfo.outputChannels >= 2 ? 2 : 1;

    // Вход опционален. Если запрошенное устройство пропало (отключили
    // интерфейс между запусками), это не повод не запускать звук вообще.
    int         inChannels = 0;
    std::string inDeviceName;
    bool        wantInput = config.inputDeviceId != kNoDevice;
    if (wantInput) {
        RtAudio::DeviceInfo inInfo = impl_->rt.getDeviceInfo(config.inputDeviceId);
        if (inInfo.inputChannels == 0) {
            impl_->lastWarning = "Устройство входа недоступно, работаем только на выход";
            wantInput = false;
        } else {
            inChannels = static_cast<int>(inInfo.inputChannels);
            if (inChannels > config.maxInputChannels)
                inChannels = config.maxInputChannels;
            // Имя держим в локальной переменной: tryOpen() ниже обнуляет
            // impl_->info целиком, и запись прямо в неё была бы потеряна.
            inDeviceName = inInfo.name;
        }
    }

    RtAudio::StreamParameters outParams;
    outParams.deviceId     = config.outputDeviceId;
    outParams.nChannels    = static_cast<unsigned int>(outChannels);
    outParams.firstChannel = 0;

    RtAudio::StreamParameters inParams;
    inParams.deviceId     = config.inputDeviceId;
    inParams.nChannels    = static_cast<unsigned int>(inChannels);
    inParams.firstChannel = 0;

    RtAudio::StreamOptions options;
    options.flags      = RTAUDIO_NONINTERLEAVED | RTAUDIO_SCHEDULE_REALTIME | RTAUDIO_MINIMIZE_LATENCY;
    options.priority   = 90;
    options.streamName = "DAW";

    auto tryOpen = [&](bool withInput) -> RtAudioErrorType {
        unsigned int frames = static_cast<unsigned int>(config.bufferFrames);

        impl_->info                = StreamInfo{};
        impl_->info.outputChannels = outChannels;
        impl_->info.inputChannels  = withInput ? inChannels : 0;
        impl_->info.sampleRate     = config.sampleRate;

        impl_->outputPtrs.assign(static_cast<std::size_t>(outChannels), nullptr);
        impl_->inputPtrs.assign(static_cast<std::size_t>(withInput ? inChannels : 0), nullptr);

        impl_->callback = callback;

        const RtAudioErrorType err = impl_->rt.openStream(
            &outParams, withInput ? &inParams : nullptr, RTAUDIO_FLOAT32,
            static_cast<unsigned int>(config.sampleRate), &frames,
            &Impl::rtCallback, impl_.get(), &options);

        if (err == RTAUDIO_NO_ERROR)
            impl_->info.bufferFrames = static_cast<int>(frames);
        return err;
    };

    // Неудачный openStream() НЕ обязан оставить backend закрытым. RtApi
    // пробует сначала выход, потом вход, и на отказе входа возвращает ошибку,
    // не убирая за собой уже открытый выход. Тогда повторная попытка упрётся
    // в проверку «a stream is already open!», откат не сработает, а устройство
    // останется захваченным. Поэтому перед повтором закрываемся явно.
    auto forceClose = [this] {
        if (impl_->rt.isStreamOpen())
            impl_->rt.closeStream();
    };

    RtAudioErrorType err = tryOpen(wantInput);

    // Дуплекс может не открыться там, где выход открывается прекрасно:
    // разные устройства, разные частоты, занятый вход. Откатываемся на выход
    // и обязательно сообщаем — молчаливый откат заставит искать, почему
    // не пишется звук.
    if (err != RTAUDIO_NO_ERROR && wantInput) {
        // Текст ошибки снимаем ДО закрытия: closeStream() затирает errorText_.
        impl_->lastWarning = "Не удалось открыть вход (" + impl_->rt.getErrorText()
                           + "). Работаем только на выход.";
        forceClose();
        wantInput = false;
        err = tryOpen(false);
    }

    if (err != RTAUDIO_NO_ERROR) {
        impl_->lastError = impl_->rt.getErrorText();
        forceClose();
        impl_->callback  = nullptr;
        impl_->info      = StreamInfo{};
        return false;
    }

    impl_->info.isOpen           = true;
    impl_->info.outputDeviceId   = config.outputDeviceId;
    impl_->info.inputDeviceId    = wantInput ? config.inputDeviceId : kNoDevice;
    impl_->info.outputDeviceName = outInfo.name;
    impl_->info.inputDeviceName  = wantInput ? inDeviceName : std::string{};
    impl_->info.latencyFrames    = static_cast<int>(impl_->rt.getStreamLatency());

    // prepare() — вне аудио-потока, здесь аллокации разрешены и ожидаемы.
    callback->prepare(config.sampleRate, impl_->info.bufferFrames,
                      impl_->info.inputChannels, impl_->info.outputChannels);

    err = impl_->rt.startStream();
    if (err != RTAUDIO_NO_ERROR) {
        impl_->lastError = impl_->rt.getErrorText();
        impl_->rt.closeStream();
        callback->release();
        impl_->callback = nullptr;
        impl_->info     = StreamInfo{};
        return false;
    }

    impl_->xruns.store(0, std::memory_order_relaxed);
    impl_->cpuLoad.store(0.0, std::memory_order_relaxed);
    return true;
}

void AudioDevice::close() {
    if (impl_->rt.isStreamRunning())
        impl_->rt.stopStream();
    if (impl_->rt.isStreamOpen())
        impl_->rt.closeStream();

    if (impl_->callback) {
        impl_->callback->release();
        impl_->callback = nullptr;
    }
    impl_->info = StreamInfo{};
}

bool AudioDevice::isOpen() const noexcept { return impl_->info.isOpen && impl_->rt.isStreamOpen(); }

const StreamInfo& AudioDevice::streamInfo() const noexcept { return impl_->info; }

double AudioDevice::sampleRate() const noexcept { return impl_->info.sampleRate; }
int    AudioDevice::bufferSize() const noexcept { return impl_->info.bufferFrames; }

double AudioDevice::cpuLoad() const noexcept {
    return impl_->cpuLoad.load(std::memory_order_relaxed);
}

std::uint64_t AudioDevice::xruns() const noexcept {
    return impl_->xruns.load(std::memory_order_relaxed);
}

const std::string& AudioDevice::lastError()   const noexcept { return impl_->lastError; }
const std::string& AudioDevice::lastWarning() const noexcept { return impl_->lastWarning; }

} // namespace daw::audio
