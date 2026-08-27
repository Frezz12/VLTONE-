#include "Device/AudioDeviceManager.hpp"
#include "platform/Clock.hpp"
#include "platform/Log.hpp"

#include <portaudio.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace audio {

#if defined(_WIN32) && defined(__has_include)
#  if __has_include(<pa_asio.h>)
#    include <pa_asio.h>
#    define DAW_HAVE_ASIO 1
#  endif
#endif
#ifndef DAW_HAVE_ASIO
#  define DAW_HAVE_ASIO 0
#endif

namespace {

// Copy a std::string into a fixed char buffer, always NUL-terminated.
void copyName(char* dest, size_t cap, const std::string& src) {
    if (cap == 0) return;
    const size_t n = std::min(cap - 1, src.size());
    std::memcpy(dest, src.data(), n);
    dest[n] = '\0';
}

std::string hostApiName(PaHostApiIndex api) {
    const PaHostApiInfo* info = Pa_GetHostApiInfo(api);
    return info && info->name ? info->name : "Audio";
}

// Stable-within-a-run identifier. PortAudio has no persistent device UID, so we
// synthesise one from the host API and device name.
std::string makeUID(const PaDeviceInfo* di) {
    if (!di) return {};
    return hostApiName(di->hostApi) + ": " + (di->name ? di->name : "");
}

/// Is this device reached over ASIO? The host API *type* is declared in
/// portaudio.h on every platform, so this needs no conditional compilation —
/// only the calls into the ASIO extension do.
bool isAsioDevice(const PaDeviceInfo* di) {
    if (!di) return false;
    const PaHostApiInfo* api = Pa_GetHostApiInfo(di->hostApi);
    return api && api->type == paASIO;
}

/// The buffer sizes worth offering when nobody has a better answer. Down to 8
/// frames: whether a device will actually run there is the device's answer, and
/// a refusal already puts the last working size back.
constexpr BufferSize kStandardBufferSizes[] = {8,   16,  32,   64,  128,
                                               256, 512, 1024, 2048};

/// Sample rates worth asking a device about.
constexpr SampleRate kCandidateSampleRates[] = {44100.0,  48000.0,  88200.0,
                                                96000.0,  176400.0, 192000.0};

/// What buffer sizes this device will actually run at.
///
/// An ASIO driver owns its buffer size and publishes the set it allows; asking
/// is the difference between the user picking a size that works and PortAudio
/// silently interposing a converting adaptor that adds a block of latency.
/// Everything else gets the standard list, which is what the settings page
/// offered before any of this existed.
std::vector<BufferSize> availableBufferSizes(PaDeviceIndex index,
                                             const PaDeviceInfo* di,
                                             BufferSize* preferredOut = nullptr) {
    if (preferredOut) *preferredOut = 0;
    const std::vector<BufferSize> standard(std::begin(kStandardBufferSizes),
                                           std::end(kStandardBufferSizes));
#if DAW_HAVE_ASIO
    if (isAsioDevice(di)) {
        long smallest = 0, largest = 0, preferred = 0, granularity = 0;
        if (PaAsio_GetAvailableBufferSizes(index, &smallest, &largest,
                                           &preferred, &granularity) == paNoError &&
            smallest > 0 && largest >= smallest) {
            if (preferredOut && preferred > 0)
                *preferredOut = static_cast<BufferSize>(preferred);
            std::vector<BufferSize> sizes;
            if (granularity == -1) {
                // Powers of two between the two ends, which is what most
                // interfaces report.
                for (long size = smallest; size <= largest && sizes.size() < 32;
                     size *= 2) {
                    sizes.push_back(static_cast<BufferSize>(size));
                }
            } else if (granularity > 0) {
                for (long size = smallest; size <= largest && sizes.size() < 64;
                     size += granularity) {
                    sizes.push_back(static_cast<BufferSize>(size));
                }
            } else if (preferred > 0) {
                // A driver that allows exactly one size.
                sizes.push_back(static_cast<BufferSize>(preferred));
            }
            if (!sizes.empty()) return sizes;
        }
    }
#else
    (void)index;
    (void)di;
#endif
    return standard;
}

std::vector<std::string> channelNames(PaDeviceIndex index,
                                      const PaDeviceInfo* di,
                                      bool input) {
    const int count = di ? (input ? di->maxInputChannels
                                  : di->maxOutputChannels)
                         : 0;
    std::vector<std::string> names;
    names.reserve(static_cast<std::size_t>(std::max(count, 0)));
    for (int channel = 0; channel < count; ++channel) {
        std::string name = (input ? "Input " : "Output ") +
                           std::to_string(channel + 1);
#if DAW_HAVE_ASIO
        if (isAsioDevice(di)) {
            const char* driverName = nullptr;
            const PaError err = input
                ? PaAsio_GetInputChannelName(index, channel, &driverName)
                : PaAsio_GetOutputChannelName(index, channel, &driverName);
            if (err == paNoError && driverName && *driverName)
                name = driverName;
        }
#else
        (void)index;
#endif
        names.push_back(std::move(name));
    }
    return names;
}

bool validSelectors(const std::vector<int>& selectors, int channelCount,
                    std::size_t maximum) {
    if (selectors.size() > maximum) return false;
    for (std::size_t i = 0; i < selectors.size(); ++i) {
        if (selectors[i] < 0 || selectors[i] >= channelCount) return false;
        if (std::find(selectors.begin(), selectors.begin() +
                      static_cast<std::ptrdiff_t>(i), selectors[i]) !=
            selectors.begin() + static_cast<std::ptrdiff_t>(i)) {
            return false;
        }
    }
    return true;
}

/// What sample rates this device will actually run at.
///
/// ASIO devices are asked nothing: `Pa_IsFormatSupported` opens the driver to
/// answer, ASIO drivers are exclusive, and doing that for every rate of every
/// device while one of them may already be in use is a good way to hang the
/// settings dialog. They get the candidate list unfiltered — the driver has the
/// last word anyway, and its control panel is where the rate really lives.
std::vector<SampleRate> supportedSampleRates(PaDeviceIndex index,
                                             const PaDeviceInfo* di,
                                             bool wantInput) {
    if (!di) return {};
    if (isAsioDevice(di)) {
        return {std::begin(kCandidateSampleRates), std::end(kCandidateSampleRates)};
    }

    PaStreamParameters params{};
    params.device = index;
    params.channelCount =
        std::min(2, wantInput ? di->maxInputChannels : di->maxOutputChannels);
    params.sampleFormat = paFloat32 | paNonInterleaved;
    params.suggestedLatency =
        wantInput ? di->defaultLowInputLatency : di->defaultLowOutputLatency;
    params.hostApiSpecificStreamInfo = nullptr;
    if (params.channelCount <= 0) return {di->defaultSampleRate};

    std::vector<SampleRate> rates;
    for (SampleRate rate : kCandidateSampleRates) {
        const PaError supported = Pa_IsFormatSupported(
            wantInput ? &params : nullptr, wantInput ? nullptr : &params, rate);
        if (supported == paFormatIsSupported) rates.push_back(rate);
    }
    // A device that refuses every candidate still runs at its own default.
    if (rates.empty()) rates.push_back(di->defaultSampleRate);
    return rates;
}

// The PortAudio C callback. Trampolines into the owning manager. Kept free so
// its signature matches PaStreamCallback exactly.
int paTrampoline(const void* input, void* output, unsigned long frameCount,
                 const PaStreamCallbackTimeInfo* /*timeInfo*/,
                 PaStreamCallbackFlags /*statusFlags*/, void* userData) {
    return static_cast<AudioDeviceManager*>(userData)
        ->processStream(input, output, frameCount);
}

} // namespace

AudioDeviceManager::AudioDeviceManager() = default;

AudioDeviceManager::~AudioDeviceManager() { shutdown(); }

Result AudioDeviceManager::ensurePortAudio() {
    if (m_paInitialized) return Result::ok();
    const PaError err = Pa_Initialize();
    if (err != paNoError) {
        return Result::fail(EngineError::DeviceError,
                            std::string("Pa_Initialize: ") +
                                Pa_GetErrorText(err));
    }
    m_paInitialized = true;
    return Result::ok();
}

Result AudioDeviceManager::initialize(SampleRate sampleRate,
                                      BufferSize bufferSize,
                                      const std::string& preferredDeviceUID,
                                      const std::string& preferredInputDeviceUID,
                                      const std::string& preferredOutputDeviceUID) {
    AudioDeviceConfig config;
    config.sampleRate = sampleRate;
    config.bufferSize = bufferSize;
    config.outputDeviceUid = !preferredOutputDeviceUID.empty()
                                 ? preferredOutputDeviceUID
                                 : preferredDeviceUID;
    config.inputDeviceUid = preferredInputDeviceUID;
    config.inputEnabled = true;
    return initialize(config);
}

Result AudioDeviceManager::adoptConfiguration(
    const AudioDeviceConfig& config) {
    if (config.sampleRate < kMinSampleRate || config.sampleRate > kMaxSampleRate)
        return Result::fail(EngineError::InvalidArgument, "invalid sample rate");
    if (config.bufferSize == 0 || config.bufferSize > 8192)
        return Result::fail(EngineError::InvalidArgument, "invalid buffer size");

    const int outputIndex = config.outputDeviceUid.empty()
        ? Pa_GetDefaultOutputDevice()
        : resolveDeviceIndex(config.outputDeviceUid, /*wantInput=*/false);
    if (outputIndex < 0) {
        return Result::fail(EngineError::DeviceNotFound,
                            config.outputDeviceUid.empty()
                                ? "no audio output device available"
                                : config.outputDeviceUid);
    }
    const PaDeviceInfo* outDi = Pa_GetDeviceInfo(outputIndex);
    if (!outDi || outDi->maxOutputChannels <= 0)
        return Result::fail(EngineError::DeviceNotFound, "invalid output device");

    int inputIndex = -1;
    if (config.inputEnabled) {
        inputIndex = config.inputDeviceUid.empty()
            ? Pa_GetDefaultInputDevice()
            : resolveDeviceIndex(config.inputDeviceUid, /*wantInput=*/true);
        if (inputIndex < 0) {
            return Result::fail(EngineError::DeviceNotFound,
                                config.inputDeviceUid.empty()
                                    ? "no audio input device available"
                                    : config.inputDeviceUid);
        }
        const PaDeviceInfo* inDi = Pa_GetDeviceInfo(inputIndex);
        if (!inDi || inDi->maxInputChannels <= 0)
            return Result::fail(EngineError::DeviceNotFound, "invalid input device");
        if (inDi->hostApi != outDi->hostApi) {
            return Result::fail(EngineError::InvalidArgument,
                                "input and output must use the same driver type");
        }
        if (isAsioDevice(outDi) && inputIndex != outputIndex) {
            return Result::fail(EngineError::InvalidArgument,
                                "ASIO input and output must use the same driver");
        }
    }

    std::vector<int> inputSelectors;
    std::vector<int> outputSelectors;
    if (isAsioDevice(outDi)) {
        outputSelectors = config.outputChannelSelectors;
        if (outputSelectors.empty()) {
            for (int channel = 0; channel < std::min(2, outDi->maxOutputChannels);
                 ++channel) {
                outputSelectors.push_back(channel);
            }
        }
        if (!validSelectors(outputSelectors, outDi->maxOutputChannels, 2) ||
            outputSelectors.empty()) {
            return Result::fail(EngineError::InvalidArgument,
                                "invalid ASIO output channel selection");
        }

        if (inputIndex >= 0) {
            const PaDeviceInfo* inDi = Pa_GetDeviceInfo(inputIndex);
            inputSelectors = config.inputChannelSelectors;
            if (inputSelectors.empty()) {
                for (int channel = 0;
                     channel < std::min(2, inDi->maxInputChannels); ++channel) {
                    inputSelectors.push_back(channel);
                }
            }
            if (!validSelectors(inputSelectors, inDi->maxInputChannels, 32) ||
                inputSelectors.empty()) {
                return Result::fail(EngineError::InvalidArgument,
                                    "invalid ASIO input channel selection");
            }
        }
    }

    m_outputDeviceIndex = outputIndex;
    m_inputDeviceIndex = inputIndex;
    m_preferredOutputDeviceUID = config.outputDeviceUid;
    m_preferredInputDeviceUID = config.inputDeviceUid;
    m_inputEnabled = config.inputEnabled;
    m_inputChannelSelectors = std::move(inputSelectors);
    m_outputChannelSelectors = std::move(outputSelectors);
    m_sampleRate.store(config.sampleRate);
    m_bufferSize.store(config.bufferSize);
    m_diagRequestedBufferSize.store(config.bufferSize);
    return Result::ok();
}

Result AudioDeviceManager::initialize(const AudioDeviceConfig& config) {
    if (m_isInitialized.load()) return Result::ok();

    auto paResult = ensurePortAudio();
    if (!paResult) {
        m_deviceState.store(AudioDeviceState::Failed);
        return paResult;
    }

    auto adopted = adoptConfiguration(config);
    if (!adopted) {
        m_deviceState.store(AudioDeviceState::Failed);
        return adopted;
    }

    auto opened = openStream();
    if (!opened) {
        m_deviceState.store(AudioDeviceState::Failed);
        return opened;
    }

    m_isInitialized.store(true);
    m_deviceState.store(AudioDeviceState::Initialized);
    DAW_LOG_INFO("[Device] Initialized out='%s' in='%s' rate=%.0f buffer=%u",
                 m_diagOutputDeviceName, m_diagInputDeviceName,
                 m_sampleRate.load(), m_bufferSize.load());
    return Result::ok();
}

AudioDeviceConfig AudioDeviceManager::configuration() const {
    AudioDeviceConfig config;
    config.outputDeviceUid = !m_outputDeviceUID.empty()
                                 ? m_outputDeviceUID
                                 : m_preferredOutputDeviceUID;
    config.inputDeviceUid = !m_inputDeviceUID.empty()
                                ? m_inputDeviceUID
                                : m_preferredInputDeviceUID;
    config.inputEnabled = m_inputEnabled && m_inputDeviceIndex >= 0;
    config.sampleRate = m_sampleRate.load();
    config.bufferSize = m_bufferSize.load();
    config.inputChannelSelectors = m_inputChannelSelectors;
    config.outputChannelSelectors = m_outputChannelSelectors;
    return config;
}

Result AudioDeviceManager::applyConfiguration(
    const AudioDeviceConfig& config) {
    auto ready = ensurePortAudio();
    if (!ready) return ready;
    if (!m_isInitialized.load()) return initialize(config);

    const AudioDeviceConfig previous = configuration();
    const bool wasRunning = m_isRunning.load();
    auto adopted = adoptConfiguration(config);
    if (!adopted) return adopted;

    closeStream();
    auto restore = [&](Result failure) {
        closeStream();
        auto restored = adoptConfiguration(previous);
        if (restored) restored = openStream();
        if (restored && wasRunning) restored = start();
        if (!restored) {
            m_deviceState.store(AudioDeviceState::Failed);
            return Result::fail(
                failure.error(), failure.message() +
                    "; restoring the previous audio device also failed: " +
                    restored.message());
        }
        return failure;
    };

    auto opened = openStream();
    if (!opened) return restore(opened);
    if (wasRunning) {
        auto started = start();
        if (!started) return restore(started);
    }
    m_isInitialized.store(true);
    return Result::ok();
}

Result AudioDeviceManager::openStream() {
    closeStream();

    const PaDeviceInfo* outDi = Pa_GetDeviceInfo(m_outputDeviceIndex);
    if (!outDi || outDi->maxOutputChannels <= 0) {
        return Result::fail(EngineError::DeviceNotFound,
                            "output device has no output channels");
    }
    m_outputChannels = isAsioDevice(outDi) && !m_outputChannelSelectors.empty()
        ? static_cast<ChannelCount>(m_outputChannelSelectors.size())
        : static_cast<ChannelCount>(std::min(2, outDi->maxOutputChannels));

    PaStreamParameters outParams{};
    outParams.device = m_outputDeviceIndex;
    outParams.channelCount = static_cast<int>(m_outputChannels);
    outParams.sampleFormat = paFloat32 | paNonInterleaved;
    outParams.suggestedLatency = outDi->defaultLowOutputLatency;
    outParams.hostApiSpecificStreamInfo = nullptr;
#if DAW_HAVE_ASIO
    PaAsioStreamInfo outAsioInfo{};
    if (isAsioDevice(outDi) && !m_outputChannelSelectors.empty()) {
        outAsioInfo.size = sizeof(outAsioInfo);
        outAsioInfo.hostApiType = paASIO;
        outAsioInfo.version = 1;
        outAsioInfo.flags = paAsioUseChannelSelectors;
        outAsioInfo.channelSelectors = m_outputChannelSelectors.data();
        outParams.hostApiSpecificStreamInfo = &outAsioInfo;
    }
#endif

    PaStreamParameters inParams{};
#if DAW_HAVE_ASIO
    PaAsioStreamInfo inAsioInfo{};
#endif
    bool haveInput = false;
    if (m_inputDeviceIndex >= 0) {
        const PaDeviceInfo* inDi = Pa_GetDeviceInfo(m_inputDeviceIndex);
        if (inDi && inDi->maxInputChannels > 0) {
            m_inputChannels = isAsioDevice(inDi) &&
                                      !m_inputChannelSelectors.empty()
                ? static_cast<ChannelCount>(m_inputChannelSelectors.size())
                : static_cast<ChannelCount>(std::min(2, inDi->maxInputChannels));
            inParams.device = m_inputDeviceIndex;
            inParams.channelCount = static_cast<int>(m_inputChannels);
            inParams.sampleFormat = paFloat32 | paNonInterleaved;
            inParams.suggestedLatency = inDi->defaultLowInputLatency;
            inParams.hostApiSpecificStreamInfo = nullptr;
#if DAW_HAVE_ASIO
            if (isAsioDevice(inDi) && !m_inputChannelSelectors.empty()) {
                inAsioInfo.size = sizeof(inAsioInfo);
                inAsioInfo.hostApiType = paASIO;
                inAsioInfo.version = 1;
                inAsioInfo.flags = paAsioUseChannelSelectors;
                inAsioInfo.channelSelectors = m_inputChannelSelectors.data();
                inParams.hostApiSpecificStreamInfo = &inAsioInfo;
            }
#endif
            haveInput = true;
        }
    }
    if (!haveInput) m_inputChannels = 0;

    double rate = m_sampleRate.load();
    if (Pa_IsFormatSupported(haveInput ? &inParams : nullptr, &outParams,
                             rate) != paFormatIsSupported) {
        // Fall back to the output device's native rate rather than failing.
        rate = outDi->defaultSampleRate;
        m_sampleRate.store(rate);
    }

    PaStream* stream = nullptr;
    const PaError err = Pa_OpenStream(
        &stream, haveInput ? &inParams : nullptr, &outParams, rate,
        m_bufferSize.load(), paNoFlag, &paTrampoline, this);
    if (err != paNoError) {
        m_diagLastStartResult.store(err);
        return Result::fail(EngineError::DeviceError,
                            std::string("Pa_OpenStream: ") +
                                Pa_GetErrorText(err));
    }
    m_stream = stream;

    if (const PaStreamInfo* si = Pa_GetStreamInfo(stream)) {
        m_sampleRate.store(si->sampleRate);
        m_diagDeviceSampleRate = si->sampleRate;
    }
    m_diagActualBufferSize.store(m_bufferSize.load());
    captureCurrentDeviceInfo();
    m_deviceState.store(AudioDeviceState::Configured);
    return Result::ok();
}

void AudioDeviceManager::closeStream() {
    if (m_stream) {
        auto* stream = static_cast<PaStream*>(m_stream);
        if (Pa_IsStreamActive(stream) == 1) {
            Pa_StopStream(stream);
        }
        Pa_CloseStream(stream);
        m_stream = nullptr;
    }
    m_isRunning.store(false);
}

Result AudioDeviceManager::start() {
    if (!m_stream) {
        return Result::fail(EngineError::DeviceError, "stream not open");
    }
    m_deviceState.store(AudioDeviceState::Starting);
    const PaError err = Pa_StartStream(static_cast<PaStream*>(m_stream));
    m_diagLastStartResult.store(err);
    if (err != paNoError) {
        m_deviceState.store(AudioDeviceState::Failed);
        return Result::fail(EngineError::DeviceError,
                            std::string("Pa_StartStream: ") +
                                Pa_GetErrorText(err));
    }
    m_isRunning.store(true);
    m_deviceState.store(AudioDeviceState::Running);
    return Result::ok();
}

Result AudioDeviceManager::stop() {
    if (m_stream) {
        auto* stream = static_cast<PaStream*>(m_stream);
        if (Pa_IsStreamActive(stream) == 1) {
            Pa_StopStream(stream);
        }
    }
    m_isRunning.store(false);
    m_deviceState.store(AudioDeviceState::Stopped);
    return Result::ok();
}

Result AudioDeviceManager::shutdown() {
    closeStream();
    if (m_paInitialized) {
        Pa_Terminate();
        m_paInitialized = false;
    }
    m_isInitialized.store(false);
    m_deviceState.store(AudioDeviceState::Stopped);
    return Result::ok();
}

int AudioDeviceManager::processStream(const void* input, void* output,
                                      unsigned long frameCount) {
    m_diagCallbackCount.fetch_add(1, std::memory_order_relaxed);
    m_diagRenderCallCount.fetch_add(1, std::memory_order_relaxed);
    m_diagLastCallbackTimestamp.store(platform::nowNanos(),
                                      std::memory_order_relaxed);
    m_diagCallbackFrameCount.store(static_cast<uint32_t>(frameCount),
                                   std::memory_order_relaxed);

    const BufferSize frames = static_cast<BufferSize>(frameCount);

    // Non-interleaved float: PortAudio hands us float** — one buffer per
    // channel — which aliases straight into an AudioBuffer with no copy.
    const AudioBuffer* inBuffer = nullptr;
    if (input && m_inputChannels > 0) {
        auto** in = static_cast<float**>(const_cast<void*>(input));
        m_inputWrapper.setNonOwning(in, m_inputChannels, frames);
        inBuffer = &m_inputWrapper;

        for (ChannelCount ch = 0; ch < m_inputChannels && ch < 2; ++ch) {
            const float* samples = in[ch];
            float peak = 0.0f, sumSq = 0.0f;
            for (BufferSize f = 0; f < frames; ++f) {
                const float s = samples[f];
                peak = std::max(peak, std::fabs(s));
                sumSq += s * s;
            }
            m_diagInputPeak[ch].store(peak, std::memory_order_relaxed);
            m_diagInputRMS[ch].store(
                frames ? std::sqrt(sumSq / frames) : 0.0f,
                std::memory_order_relaxed);
        }
        m_diagInputChannels.store(m_inputChannels, std::memory_order_relaxed);
    }

    if (!output) return paContinue;
    auto** out = static_cast<float**>(output);
    m_outputWrapper.setNonOwning(out, m_outputChannels, frames);
    IAudioCallback* cb = m_audioCallback.load(std::memory_order_relaxed);
    // Generic callbacks may be partial writers, and no callback must produce
    // silence. The production engine explicitly guarantees a complete block,
    // so do not burn memory bandwidth clearing data it immediately overwrites.
    if (!cb || !cb->writesCompleteOutput()) m_outputWrapper.clear(frames);

    if (cb) {
        AudioCallbackContext ctx;
        ctx.outputBuffer = &m_outputWrapper;
        ctx.inputBuffer = inBuffer;
        ctx.numFrames = frames;
        ctx.sampleRate = m_sampleRate.load(std::memory_order_relaxed);
        ctx.sampleTime = 0;
        ctx.isRealtime = true;
        cb->onAudioCallback(ctx);
    }

    m_diagLastRenderStatus.store(0, std::memory_order_relaxed);
    return paContinue;
}

Result AudioDeviceManager::probeDevice(const std::string& uid, bool wantInput,
                                      DeviceInfo& out) {
    auto ready = ensurePortAudio();
    if (!ready) return ready;

    const int index = resolveDeviceIndex(uid, wantInput);
    if (index < 0) {
        return Result::fail(EngineError::DeviceNotFound, "no such device");
    }
    const PaDeviceInfo* di = Pa_GetDeviceInfo(index);
    if (!di) return Result::fail(EngineError::DeviceNotFound, "no such device");

    out.sampleRates = supportedSampleRates(index, di, wantInput);
    out.bufferSizes = availableBufferSizes(index, di, &out.preferredBufferSize);
    if (wantInput)
        out.inputChannelNames = channelNames(index, di, true);
    else
        out.outputChannelNames = channelNames(index, di, false);
    return Result::ok();
}

Result AudioDeviceManager::showControlPanel(const std::string& uid,
                                            void* nativeWindow) {
    auto ready = ensurePortAudio();
    if (!ready) return ready;

    // A device can be an output, an input, or both; the panel belongs to the
    // driver either way, so look under both.
    int index = resolveDeviceIndex(uid, /*wantInput=*/false);
    if (index < 0) index = resolveDeviceIndex(uid, /*wantInput=*/true);
    if (index < 0) {
        return Result::fail(EngineError::DeviceNotFound, "no such device");
    }

#if DAW_HAVE_ASIO
    const PaDeviceInfo* di = Pa_GetDeviceInfo(index);
    if (!isAsioDevice(di)) {
        return Result::fail(EngineError::NotSupported,
                            "this device is configured by the operating system, "
                            "not by a driver panel");
    }
    // ASIO panels often need exclusive access to the driver. Release our
    // stream, then restore it before returning so Hardware Setup never leaves
    // the application silent.
    const bool hadStream = m_isInitialized.load();
    const bool wasRunning = m_isRunning.load();
    const bool panelIsCurrent = index == m_outputDeviceIndex;
    if (hadStream) closeStream();

    const PaError err = PaAsio_ShowControlPanel(index, nativeWindow);
    if (err == paNoError && panelIsCurrent) {
        BufferSize preferred = 0;
        (void)availableBufferSizes(index, Pa_GetDeviceInfo(index), &preferred);
        if (preferred > 0) {
            m_bufferSize.store(preferred);
            m_diagRequestedBufferSize.store(preferred);
        }
    }

    Result restored = Result::ok();
    if (hadStream) {
        restored = openStream();
        if (restored && wasRunning) restored = start();
    }
    if (err != paNoError) {
        return Result::fail(EngineError::DeviceError,
                            std::string("PaAsio_ShowControlPanel: ") +
                                Pa_GetErrorText(err));
    }
    return restored;
#else
    (void)nativeWindow;
    return Result::fail(EngineError::NotSupported,
                        "this build has no ASIO support");
#endif
}

// ── Device enumeration ─────────────────────────────────────────────────────

int AudioDeviceManager::resolveDeviceIndex(const std::string& uid,
                                           bool wantInput) const {
    const int count = Pa_GetDeviceCount();
    for (int i = 0; i < count; ++i) {
        const PaDeviceInfo* di = Pa_GetDeviceInfo(i);
        if (!di) continue;
        if (wantInput && di->maxInputChannels <= 0) continue;
        if (!wantInput && di->maxOutputChannels <= 0) continue;
        if (makeUID(di) == uid) return i;
    }
    return -1;
}

std::vector<DeviceInfo> AudioDeviceManager::enumerateInputDevices() {
    ensurePortAudio();
    std::vector<DeviceInfo> devices;
    const int count = Pa_GetDeviceCount();
    const PaDeviceIndex def = Pa_GetDefaultInputDevice();
    for (int i = 0; i < count; ++i) {
        const PaDeviceInfo* di = Pa_GetDeviceInfo(i);
        if (!di || di->maxInputChannels <= 0) continue;
        DeviceInfo info;
        info.uid = makeUID(di);
        info.name = di->name ? di->name : "";
        info.manufacturer = hostApiName(di->hostApi);
        info.hostApi = info.manufacturer;
        info.isAsio = isAsioDevice(di);
        info.hasControlPanel = info.isAsio && DAW_HAVE_ASIO;
        info.inputChannels = static_cast<ChannelCount>(di->maxInputChannels);
        info.outputChannels = static_cast<ChannelCount>(di->maxOutputChannels);
        // Cheap by design — see `probeDevice`. These are the candidates, not
        // an answer from the device.
        info.sampleRates = {std::begin(kCandidateSampleRates),
                            std::end(kCandidateSampleRates)};
        info.bufferSizes = {std::begin(kStandardBufferSizes),
                            std::end(kStandardBufferSizes)};
        info.isDefaultInput = (i == def);
        devices.push_back(std::move(info));
    }
    return devices;
}

std::vector<DeviceInfo> AudioDeviceManager::enumerateOutputDevices() {
    ensurePortAudio();
    std::vector<DeviceInfo> devices;
    const int count = Pa_GetDeviceCount();
    const PaDeviceIndex def = Pa_GetDefaultOutputDevice();
    for (int i = 0; i < count; ++i) {
        const PaDeviceInfo* di = Pa_GetDeviceInfo(i);
        if (!di || di->maxOutputChannels <= 0) continue;
        DeviceInfo info;
        info.uid = makeUID(di);
        info.name = di->name ? di->name : "";
        info.manufacturer = hostApiName(di->hostApi);
        info.hostApi = info.manufacturer;
        info.isAsio = isAsioDevice(di);
        info.hasControlPanel = info.isAsio && DAW_HAVE_ASIO;
        info.inputChannels = static_cast<ChannelCount>(di->maxInputChannels);
        info.outputChannels = static_cast<ChannelCount>(di->maxOutputChannels);
        info.sampleRates = {std::begin(kCandidateSampleRates),
                            std::end(kCandidateSampleRates)};
        info.bufferSizes = {std::begin(kStandardBufferSizes),
                            std::end(kStandardBufferSizes)};
        info.isDefaultOutput = (i == def);
        devices.push_back(std::move(info));
    }
    return devices;
}

DeviceInfo AudioDeviceManager::getCurrentInputDevice() const {
    DeviceInfo info;
    if (m_inputDeviceIndex < 0) return info;
    if (const PaDeviceInfo* di = Pa_GetDeviceInfo(m_inputDeviceIndex)) {
        info.uid = makeUID(di);
        info.name = di->name ? di->name : "";
        info.manufacturer = hostApiName(di->hostApi);
        info.hostApi = info.manufacturer;
        info.isAsio = isAsioDevice(di);
        info.hasControlPanel = info.isAsio && DAW_HAVE_ASIO;
        info.inputChannels = m_inputChannels;
        const auto physical = channelNames(m_inputDeviceIndex, di, true);
        if (info.isAsio && !m_inputChannelSelectors.empty()) {
            for (int selected : m_inputChannelSelectors) {
                if (selected >= 0 && selected < static_cast<int>(physical.size()))
                    info.inputChannelNames.push_back(physical[std::size_t(selected)]);
            }
        } else {
            info.inputChannelNames.assign(
                physical.begin(),
                physical.begin() + std::min<std::size_t>(physical.size(),
                                                         m_inputChannels));
        }
        info.sampleRates = {di->defaultSampleRate};
    }
    return info;
}

DeviceInfo AudioDeviceManager::getCurrentOutputDevice() const {
    DeviceInfo info;
    if (m_outputDeviceIndex < 0) return info;
    if (const PaDeviceInfo* di = Pa_GetDeviceInfo(m_outputDeviceIndex)) {
        info.uid = makeUID(di);
        info.name = di->name ? di->name : "";
        info.manufacturer = hostApiName(di->hostApi);
        info.hostApi = info.manufacturer;
        info.isAsio = isAsioDevice(di);
        info.hasControlPanel = info.isAsio && DAW_HAVE_ASIO;
        info.outputChannels = m_outputChannels;
        const auto physical = channelNames(m_outputDeviceIndex, di, false);
        if (info.isAsio && !m_outputChannelSelectors.empty()) {
            for (int selected : m_outputChannelSelectors) {
                if (selected >= 0 && selected < static_cast<int>(physical.size()))
                    info.outputChannelNames.push_back(physical[std::size_t(selected)]);
            }
        } else {
            info.outputChannelNames.assign(
                physical.begin(),
                physical.begin() + std::min<std::size_t>(physical.size(),
                                                         m_outputChannels));
        }
        info.sampleRates = {di->defaultSampleRate};
    }
    return info;
}

void AudioDeviceManager::captureCurrentDeviceInfo() {
    if (const PaDeviceInfo* outDi = Pa_GetDeviceInfo(m_outputDeviceIndex)) {
        copyName(m_diagOutputDeviceName, sizeof(m_diagOutputDeviceName),
                 outDi->name ? outDi->name : "");
        m_outputDeviceUID = makeUID(outDi);
    }
    if (m_inputDeviceIndex >= 0) {
        if (const PaDeviceInfo* inDi = Pa_GetDeviceInfo(m_inputDeviceIndex)) {
            copyName(m_diagInputDeviceName, sizeof(m_diagInputDeviceName),
                     inDi->name ? inDi->name : "");
            m_inputDeviceUID = makeUID(inDi);
            m_diagDeviceInputChannels =
                static_cast<uint32_t>(inDi->maxInputChannels);
        }
    } else {
        copyName(m_diagInputDeviceName, sizeof(m_diagInputDeviceName), "None");
        m_inputDeviceUID.clear();
        m_diagDeviceInputChannels = 0;
    }
    m_diagDeviceBufferFrames = m_bufferSize.load();
    m_diagBackendInputChannels = m_inputChannels;
}

// ── Reconfiguration ────────────────────────────────────────────────────────
// Each setter updates state and, if the stream is already open, reopens it —
// restarting playback only if it was running.

Result AudioDeviceManager::setInputDevice(const std::string& uid) {
    auto config = configuration();
    config.inputEnabled = !uid.empty();
    config.inputDeviceUid = uid;
    config.inputChannelSelectors.clear();
    return applyConfiguration(config);
}

Result AudioDeviceManager::setOutputDevice(const std::string& uid) {
    auto config = configuration();
    config.outputDeviceUid = uid;
    config.outputChannelSelectors.clear();
    return applyConfiguration(config);
}

Result AudioDeviceManager::setSampleRate(SampleRate rate) {
    auto config = configuration();
    config.sampleRate = rate;
    return applyConfiguration(config);
}

Result AudioDeviceManager::setBufferSize(BufferSize size) {
    auto config = configuration();
    config.bufferSize = size;
    return applyConfiguration(config);
}

void AudioDeviceManager::setAudioCallback(IAudioCallback* callback) {
    m_audioCallback.store(callback, std::memory_order_relaxed);
}

void AudioDeviceManager::setDeviceNotification(
    IAudioDeviceNotification* notification) {
    m_deviceNotification = notification;
}

} // namespace audio
