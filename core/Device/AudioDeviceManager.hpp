#pragma once

#include "Core/Types.hpp"
#include "Core/Result.hpp"
#include "Core/AudioBuffer.hpp"
#include "Core/IAudioCallback.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <atomic>

namespace audio {

enum class AudioDeviceState {
    Created,
    Configured,
    Initialized,
    Starting,
    Running,
    Stopped,
    Failed
};

struct DeviceInfo {
    // Name-based identity persisted across launches; unlike PortAudio's index,
    // it stays stable while the driver keeps the same advertised name.
    std::string uid;          // "<host-api>: <device-name>"
    std::string name;
    std::string manufacturer;
    /// The driver family this device is reached through — "ASIO", "Core Audio",
    /// "Windows WASAPI", "MME". Worth showing: on Windows the same hardware
    /// appears once per host API under the same name, and which one is picked
    /// decides the latency the user gets.
    std::string hostApi;
    /// This device is reached over ASIO, so it is exclusive, its buffer sizes
    /// come from the driver rather than from us, and it has a control panel.
    bool isAsio = false;
    /// The driver owns a settings dialog we can open. ASIO only: everywhere
    /// else the device is configured by the operating system.
    bool hasControlPanel = false;
    ChannelCount inputChannels = 0;
    ChannelCount outputChannels = 0;
    /// Physical channel names. ASIO supplies the vendor names; other APIs use
    /// the portable "Input N" / "Output N" fallback.
    std::vector<std::string> inputChannelNames;
    std::vector<std::string> outputChannelNames;
    std::vector<SampleRate> sampleRates;
    std::vector<BufferSize> bufferSizes;
    BufferSize preferredBufferSize = 0;
    bool isDefaultInput = false;
    bool isDefaultOutput = false;
    bool isAlive = true;
};

/// One complete device choice. Applying this as a unit is important: opening
/// an ASIO output with the previous WASAPI input (or vice versa) is an invalid
/// intermediate configuration and used to leave the application silent.
struct AudioDeviceConfig {
    std::string outputDeviceUid;      // empty = system default
    std::string inputDeviceUid;       // empty + inputEnabled = system default
    bool inputEnabled = true;
    SampleRate sampleRate = kDefaultSampleRate;
    BufferSize bufferSize = kDefaultBufferSize;
    std::vector<int> inputChannelSelectors;
    std::vector<int> outputChannelSelectors;
};

// Cross-platform audio device I/O built on PortAudio (CoreAudio on macOS,
// WASAPI/ASIO on Windows, ALSA/JACK on Linux). Replaces the previous
// CoreAudio/AUHAL implementation. The public interface is unchanged so the
// engine above it is untouched; only the backend differs.
class AudioDeviceManager {
public:
    AudioDeviceManager();
    ~AudioDeviceManager();

    AudioDeviceManager(const AudioDeviceManager&) = delete;
    AudioDeviceManager& operator=(const AudioDeviceManager&) = delete;

    Result initialize(SampleRate sampleRate = kDefaultSampleRate,
                      BufferSize bufferSize = kDefaultBufferSize,
                      const std::string& preferredDeviceUID = "",
                      const std::string& preferredInputDeviceUID = "",
                      const std::string& preferredOutputDeviceUID = "");
    Result initialize(const AudioDeviceConfig& config);
    Result shutdown();

    Result start();
    Result stop();

    bool isRunning() const { return m_isRunning.load(); }
    bool isInitialized() const { return m_isInitialized.load(); }
    AudioDeviceState deviceState() const { return m_deviceState.load(); }

    SampleRate sampleRate() const { return m_sampleRate.load(std::memory_order_relaxed); }
    BufferSize bufferSize() const { return m_bufferSize.load(std::memory_order_relaxed); }

    std::vector<DeviceInfo> enumerateInputDevices();
    std::vector<DeviceInfo> enumerateOutputDevices();
    DeviceInfo getCurrentInputDevice() const;
    DeviceInfo getCurrentOutputDevice() const;
    AudioDeviceConfig configuration() const;

    /// Reopen once with a complete configuration. If opening or starting the
    /// candidate fails, the previous stream is restored before the error is
    /// returned.
    Result applyConfiguration(const AudioDeviceConfig& config);

    Result setInputDevice(const std::string& uid);
    Result setOutputDevice(const std::string& uid);
    Result setSampleRate(SampleRate rate);
    Result setBufferSize(BufferSize size);

    /// Ask one device what it can actually do: the sample rates it accepts and,
    /// for an ASIO driver, the buffer sizes it allows. Fills those two fields of
    /// `out` and leaves the rest alone.
    ///
    /// Deliberately not folded into enumeration. Answering costs a stream
    /// negotiation per rate — measured at 2–40 ms each here, and far worse on
    /// some virtual devices, which put a whole-list probe at 3.5 seconds — and
    /// on Windows it means loading the ASIO driver. Doing that for every device
    /// merely to populate a combo box would freeze the settings dialog and
    /// would take exclusive ASIO drivers away from whatever else is using them.
    /// So enumeration stays cheap and this is called for the one device the
    /// user actually selected.
    Result probeDevice(const std::string& uid, bool wantInput, DeviceInfo& out);

    /// Open the driver's own settings dialog for a device.
    ///
    /// ASIO drivers own their sample rate and buffer size — the panel is where
    /// they are actually set, and a host that hides it leaves the user unable
    /// to configure their interface. Fails with NotSupported anywhere else,
    /// which is every device on macOS and Linux.
    Result showControlPanel(const std::string& uid,
                            void* nativeWindow = nullptr);

    void setAudioCallback(IAudioCallback* callback);
    void setDeviceNotification(IAudioDeviceNotification* notification);

    // Invoked from the PortAudio C callback trampoline (see the .cpp). Public
    // only so that trampoline can reach it; not part of the intended API.
    int processStream(const void* input, void* output,
                      unsigned long frameCount);

private:
    Result ensurePortAudio();
    Result openStream();
    void closeStream();
    Result adoptConfiguration(const AudioDeviceConfig& config);
    int resolveDeviceIndex(const std::string& uid, bool wantInput) const;
    void captureCurrentDeviceInfo();

    // PortAudio handles kept opaque so <portaudio.h> stays out of this header.
    void* m_stream = nullptr;          // PaStream*
    int m_inputDeviceIndex = -1;       // paNoDevice
    int m_outputDeviceIndex = -1;      // paNoDevice
    bool m_paInitialized = false;

    std::string m_inputDeviceUID;
    std::string m_outputDeviceUID;
    std::string m_preferredInputDeviceUID;
    std::string m_preferredOutputDeviceUID;
    bool m_inputEnabled = true;
    std::vector<int> m_inputChannelSelectors;
    std::vector<int> m_outputChannelSelectors;

    std::atomic<bool> m_isInitialized{false};
    std::atomic<bool> m_isRunning{false};
    std::atomic<AudioDeviceState> m_deviceState{AudioDeviceState::Created};

    std::atomic<SampleRate> m_sampleRate{kDefaultSampleRate};
    std::atomic<BufferSize> m_bufferSize{kDefaultBufferSize};
    ChannelCount m_inputChannels = 0;
    ChannelCount m_outputChannels = 2;

    std::atomic<IAudioCallback*> m_audioCallback{nullptr};
    IAudioDeviceNotification* m_deviceNotification = nullptr;

    // Non-owning wrappers reused every callback (allocation-free): they alias
    // PortAudio's per-channel buffers so no interleave/copy is needed.
    AudioBuffer m_outputWrapper;
    AudioBuffer m_inputWrapper;

    // Diagnostics — portable types (no OSStatus / UInt32).
    std::atomic<float> m_diagInputRMS[2] = {0.0f, 0.0f};
    std::atomic<float> m_diagInputPeak[2] = {0.0f, 0.0f};
    std::atomic<int> m_diagLastRenderStatus{-1};
    std::atomic<uint32_t> m_diagInputChannels{0};
    std::atomic<uint64_t> m_diagCallbackCount{0};
    std::atomic<uint64_t> m_diagRenderCallCount{0};
    std::atomic<uint64_t> m_diagRenderFailCount{0};
    std::atomic<uint64_t> m_diagLastCallbackTimestamp{0};
    std::atomic<int> m_diagLastStartResult{0};
    std::atomic<uint32_t> m_diagRequestedBufferSize{0};
    std::atomic<uint32_t> m_diagActualBufferSize{0};
    std::atomic<uint32_t> m_diagCallbackFrameCount{0};
    char m_diagOutputDeviceName[256] = {};
    char m_diagInputDeviceName[256] = {};
    double m_diagDeviceSampleRate = 0;
    uint32_t m_diagDeviceInputChannels = 0;
    uint32_t m_diagDeviceBufferFrames = 0;
    uint32_t m_diagBackendInputChannels = 0;

public:
    float diagInputRMS(ChannelCount ch) const {
        if (ch >= 2) return 0.0f;
        return m_diagInputRMS[ch].load(std::memory_order_relaxed);
    }
    float diagInputPeak(ChannelCount ch) const {
        if (ch >= 2) return 0.0f;
        return m_diagInputPeak[ch].load(std::memory_order_relaxed);
    }
    int diagLastRenderStatus() const {
        return m_diagLastRenderStatus.load(std::memory_order_relaxed);
    }
    uint32_t diagInputChannels() const {
        return m_diagInputChannels.load(std::memory_order_relaxed);
    }
    uint64_t diagCallbackCount() const {
        return m_diagCallbackCount.load(std::memory_order_relaxed);
    }
    uint64_t diagRenderCallCount() const {
        return m_diagRenderCallCount.load(std::memory_order_relaxed);
    }
    uint64_t diagRenderFailCount() const {
        return m_diagRenderFailCount.load(std::memory_order_relaxed);
    }
    uint64_t diagLastCallbackTimestamp() const {
        return m_diagLastCallbackTimestamp.load(std::memory_order_relaxed);
    }
    int diagLastStartResult() const {
        return m_diagLastStartResult.load(std::memory_order_relaxed);
    }
    uint32_t diagRequestedBufferSize() const {
        return m_diagRequestedBufferSize.load(std::memory_order_relaxed);
    }
    uint32_t diagActualBufferSize() const {
        return m_diagActualBufferSize.load(std::memory_order_relaxed);
    }
    uint32_t diagCallbackFrameCount() const {
        return m_diagCallbackFrameCount.load(std::memory_order_relaxed);
    }
    const char* diagOutputDeviceName() const { return m_diagOutputDeviceName; }
    const char* diagInputDeviceName() const { return m_diagInputDeviceName; }
    double diagDeviceSampleRate() const { return m_diagDeviceSampleRate; }
    uint32_t diagDeviceInputChannels() const { return m_diagDeviceInputChannels; }
    uint32_t diagDeviceBufferFrames() const { return m_diagDeviceBufferFrames; }
    uint32_t diagBackendInputChannels() const { return m_diagBackendInputChannels; }
};

} // namespace audio
