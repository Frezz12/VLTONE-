#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

#include "daw/audio/AudioDevice.h"
#include "daw/audio/Metronome.h"
#include "daw/audio/ToneSource.h"
#include "daw/graph/ProcessGraph.h"
#include "daw/model/Session.h"
#include "daw/rt/RcuPublisher.h"
#include "daw/rt/SpscQueue.h"
#include "daw/time/TempoMap.h"

namespace daw {

struct EngineCommand {
    enum class Type : std::uint16_t {
        None = 0,
        TransportPlay,
        TransportStop,
        Locate,
    };
    Type          type     = Type::None;
    std::uint16_t target   = 0;
    std::int64_t  position = 0;
};

struct TransportState {
    std::atomic<bool>         playing{false};
    std::atomic<std::int64_t> positionSamples{0};
};

class Engine final : public audio::AudioCallback {
public:
    Engine();
    ~Engine() override;

    bool start(const audio::DeviceConfig& config);
    void stop();
    bool isRunning() const noexcept;

    audio::AudioDevice&       device()       noexcept { return *device_; }
    const audio::AudioDevice& device() const noexcept { return *device_; }

    void play();
    void stopTransport();
    void locate(std::int64_t sample);
    void returnToZero() { locate(0); }

    bool         isPlaying()    const noexcept { return transport_.playing.load(std::memory_order_relaxed); }
    std::int64_t playPosition() const noexcept { return transport_.positionSamples.load(std::memory_order_relaxed); }

    void setTempo(double bpm);
    void setTimeSignature(int numerator, int denominator);

    const time::TempoMap& tempoMap() const noexcept { return *tempoMap_.current(); }

    void collectRetired()
    {
        tempoMap_.collect();
    }

    void setSession(std::shared_ptr<model::Session> session);
    model::Session* session() noexcept { return session_.get(); }

    // ---- Микшер дорожек ---------------------------------------------------
    // Громкоcть меняетcя «вживую»: указатель на GainNode берётcя из текущего
    // графа, а cам setGain атомарен и cглажен — переcборка графа не нужна.
    void setTrackGain(int trackIndex, float linear);
    // Mute/solo меняют топологию (заглушённая дорожка в граф не попадает),
    // поэтому граф переcобираетcя и публикуетcя через RCU.
    void setTrackMuted(int trackIndex, bool muted);
    void setTrackSoloed(int trackIndex, bool soloed);

    void setMetronomeEnabled(bool on) noexcept { metronome_.setEnabled(on); }
    void setMetronomeGain(float linear) noexcept { metronome_.setGain(linear); }
    bool isMetronomeEnabled() const noexcept { return metronome_.isEnabled(); }

    void setInputMonitoring(bool on) noexcept { inputMonitor_.store(on, std::memory_order_relaxed); }
    void setInputMonitorGain(float linear) noexcept { inputMonitorGain_.store(linear, std::memory_order_relaxed); }
    bool isInputMonitoring() const noexcept { return inputMonitor_.load(std::memory_order_relaxed); }

    float inputPeak(int channel) const noexcept;
    int   inputChannelCount() const noexcept { return device_->streamInfo().inputChannels; }

    void setToneEnabled(bool on) noexcept { tone_.setActive(on); }
    void setToneFrequency(float hz) noexcept { tone_.setFrequency(hz); }
    void setToneGain(float linear) noexcept { tone_.setGain(linear); }
    bool isToneEnabled() const noexcept { return tone_.isActive(); }

    float         peak(int channel) const noexcept;
    double        cpuLoad() const noexcept;
    std::uint64_t xruns() const noexcept;
    std::uint64_t callbackCount() const noexcept { return callbacks_.load(std::memory_order_relaxed); }
    std::uint64_t droppedCommands() const noexcept { return droppedCommands_.load(std::memory_order_relaxed); }

    void prepare(double sampleRate, int maxBlockSize,
                 int inputChannels, int outputChannels) override;
    void process(const audio::AudioBufferView& input,
                 audio::AudioBufferView& output,
                 int numFrames) noexcept override;
    void release() override;

private:
    void drainCommands() noexcept;
    void publishTempoMap(std::unique_ptr<time::TempoMap> map);
    void rebuildGraph();

    std::unique_ptr<audio::AudioDevice> device_;
    audio::ToneSource                   tone_;
    audio::Metronome                    metronome_;
    TransportState                      transport_;

    rt::RcuPublisher<time::TempoMap>     tempoMap_;
    std::shared_ptr<model::Session>      session_;
    rt::RcuPublisher<graph::ProcessGraph> liveGraph_;
    rt::SpscQueue<EngineCommand, 1024>  commands_;

    std::atomic<std::uint64_t>          droppedCommands_{0};
    std::atomic<std::uint64_t>          callbacks_{0};

    std::atomic<bool>  inputMonitor_{false};
    std::atomic<float> inputMonitorGain_{1.0f};
    float              monitorGainSmoothed_ = 0.0f;
    float              monitorSmoothing_    = 0.999f;

    static constexpr int kMaxMeterChannels = 2;
    std::atomic<float>   peaks_[kMaxMeterChannels]{};
    std::atomic<float>   inputPeaks_[kMaxMeterChannels]{};

    double sampleRate_ = 48000.0;
    int    maxBlockSize_ = 256;
};

} // namespace daw
