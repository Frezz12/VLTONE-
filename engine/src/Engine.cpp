#include "daw/Engine.h"
#include "daw/graph/GraphCompiler.h"

#include <cmath>

#include "daw/rt/RtGuard.h"

namespace daw {

Engine::Engine()
    : device_(std::make_unique<audio::AudioDevice>()),
      tempoMap_(std::make_unique<time::TempoMap>(48000.0)),
      session_(std::make_shared<model::Session>(48000.0)) {}

Engine::~Engine() { stop(); }

bool Engine::start(const audio::DeviceConfig& config) {
    auto fresh = std::make_unique<time::TempoMap>(*tempoMap_.current());
    fresh->setSampleRate(config.sampleRate);
    publishTempoMap(std::move(fresh));

    return device_->open(config, this);
}

void Engine::stop() {
    device_->close();
}

bool Engine::isRunning() const noexcept { return device_->isOpen(); }

void Engine::play() {
    EngineCommand c;
    c.type = EngineCommand::Type::TransportPlay;
    if (!commands_.push(c))
        droppedCommands_.fetch_add(1, std::memory_order_relaxed);
}

void Engine::stopTransport() {
    EngineCommand c;
    c.type = EngineCommand::Type::TransportStop;
    if (!commands_.push(c))
        droppedCommands_.fetch_add(1, std::memory_order_relaxed);
}

void Engine::locate(std::int64_t sample) {
    EngineCommand c;
    c.type     = EngineCommand::Type::Locate;
    c.position = sample < 0 ? 0 : sample;
    if (!commands_.push(c))
        droppedCommands_.fetch_add(1, std::memory_order_relaxed);
}

void Engine::publishTempoMap(std::unique_ptr<time::TempoMap> map) {
    tempoMap_.publish(std::move(map));
}

void Engine::setTempo(double bpm) {
    auto fresh = std::make_unique<time::TempoMap>(*tempoMap_.current());
    fresh->setConstantTempo(bpm);
    publishTempoMap(std::move(fresh));
}

void Engine::setTimeSignature(int numerator, int denominator) {
    auto fresh = std::make_unique<time::TempoMap>(*tempoMap_.current());
    fresh->setTimeSignature(numerator, denominator);
    publishTempoMap(std::move(fresh));
}

void Engine::setSession(std::shared_ptr<model::Session> session) {
    session_ = std::move(session);
    rebuildGraph();
}

void Engine::setTrackGain(int trackIndex, float linear) {
    // Модель — иcточник иcтины для будущих переcборок графа, поэтому обновляем
    // и её, и живой узел. UI-поток здеcь единcтвенный пиcатель RCU, так что
    // current() безопаcен; setGain атомарен отноcительно аудио-потока.
    if (session_)
        if (auto* tr = session_->track(trackIndex))
            tr->setGain(linear);

    if (auto* graph = liveGraph_.current())
        if (auto* node = graph->trackGain(trackIndex))
            node->setGain(linear);
}

void Engine::setTrackMuted(int trackIndex, bool muted) {
    if (!session_)
        return;
    auto* tr = session_->track(trackIndex);
    if (!tr || tr->isMuted() == muted)
        return;
    tr->setMuted(muted);
    rebuildGraph();
}

void Engine::setTrackSoloed(int trackIndex, bool soloed) {
    if (!session_)
        return;
    auto* tr = session_->track(trackIndex);
    if (!tr || tr->isSoloed() == soloed)
        return;
    tr->setSoloed(soloed);
    rebuildGraph();
}

void Engine::rebuildGraph() {
    const auto* sess = session_.get();
    if (!sess)
        return;

    auto graph = graph::GraphCompiler::compileSession(
        *sess, sampleRate_, maxBlockSize_);

    liveGraph_.publish(std::move(graph));
}

float Engine::peak(int channel) const noexcept {
    if (channel < 0 || channel >= kMaxMeterChannels) return 0.0f;
    return peaks_[channel].load(std::memory_order_relaxed);
}

float Engine::inputPeak(int channel) const noexcept {
    if (channel < 0 || channel >= kMaxMeterChannels) return 0.0f;
    return inputPeaks_[channel].load(std::memory_order_relaxed);
}

double        Engine::cpuLoad() const noexcept { return device_->cpuLoad(); }
std::uint64_t Engine::xruns()   const noexcept { return device_->xruns(); }

void Engine::prepare(double sampleRate, int maxBlockSize,
                     int /*inputChannels*/, int /*outputChannels*/) {
    sampleRate_ = sampleRate;
    maxBlockSize_ = maxBlockSize;
    tone_.prepare(sampleRate, maxBlockSize);
    metronome_.prepare(sampleRate);

    monitorGainSmoothed_ = 0.0f;
    monitorSmoothing_    = static_cast<float>(std::exp(-1.0 / (sampleRate * 0.020)));

    transport_.positionSamples.store(0, std::memory_order_relaxed);
    for (auto& p : peaks_)
        p.store(0.0f, std::memory_order_relaxed);
    for (auto& p : inputPeaks_)
        p.store(0.0f, std::memory_order_relaxed);

    rebuildGraph();
}

void Engine::drainCommands() noexcept {
    EngineCommand c;
    while (commands_.pop(c)) {
        switch (c.type) {
        case EngineCommand::Type::TransportPlay:
            transport_.playing.store(true, std::memory_order_relaxed);
            break;

        case EngineCommand::Type::TransportStop:
            transport_.playing.store(false, std::memory_order_relaxed);
            metronome_.reset();
            break;

        case EngineCommand::Type::Locate:
            transport_.positionSamples.store(c.position, std::memory_order_relaxed);
            metronome_.reset();
            break;

        case EngineCommand::Type::None:
            break;
        }
    }
}

void Engine::process(const audio::AudioBufferView& input,
                     audio::AudioBufferView& output,
                     int numFrames) noexcept {
    callbacks_.fetch_add(1, std::memory_order_relaxed);

    drainCommands();

    const std::int64_t blockStart = transport_.positionSamples.load(std::memory_order_relaxed);
    const bool playing = transport_.playing.load(std::memory_order_relaxed);

    // ---- метры входа ------------------------------------------------------
    const int inputChannels = input.numChannels();
    for (int c = 0; c < kMaxMeterChannels; ++c) {
        if (c >= inputChannels) {
            inputPeaks_[c].store(0.0f, std::memory_order_relaxed);
            continue;
        }
        const float p    = input.peak(c);
        const float prev = inputPeaks_[c].load(std::memory_order_relaxed);
        inputPeaks_[c].store(p > prev ? p : prev * 0.85f, std::memory_order_relaxed);
    }

    // ---- графический движок ------------------------------------------------
    {
        rt::RcuPublisher<graph::ProcessGraph>::ReadGuard graph(liveGraph_);
        if (playing && graph && !graph->empty()) {
            // У графа нет внешнего входа: его иcточники читают c диcка/из RAM,
            // а cток (AudioOutputNode) домешивает результат в output, который
            // драйвер уже обнулил перед callback'ом.
            audio::AudioBufferView outArr[] = {output};

            graph::ProcessContext ctx;
            ctx.inputs = nullptr;
            ctx.outputs = outArr;
            ctx.numInputs = 0;
            ctx.numOutputs = 1;
            ctx.numFrames = numFrames;
            ctx.playPosition = blockStart;
            ctx.playing = true;

            graph->process(ctx);
        }
    }

    // ---- тестовый генератор ------------------------------------------------
    tone_.process(output, numFrames);

    // ---- мониторинг входа -------------------------------------------------
    const float targetMonitor = inputMonitor_.load(std::memory_order_relaxed)
                              ? inputMonitorGain_.load(std::memory_order_relaxed)
                              : 0.0f;

    if (inputChannels > 0 && (targetMonitor > 0.0f || monitorGainSmoothed_ > 1e-5f)) {
        const int outputChannels = output.numChannels();
        for (int i = 0; i < numFrames; ++i) {
            monitorGainSmoothed_ = targetMonitor
                                 + (monitorGainSmoothed_ - targetMonitor) * monitorSmoothing_;
            for (int c = 0; c < outputChannels; ++c) {
                const int sourceChannel = c < inputChannels ? c : inputChannels - 1;
                output.channel(c)[i] += input.channel(sourceChannel)[i] * monitorGainSmoothed_;
            }
        }
    } else {
        monitorGainSmoothed_ = 0.0f;
    }

    // ---- метроном ---------------------------------------------------------
    {
        rt::RcuPublisher<time::TempoMap>::ReadGuard map(tempoMap_);
        if (playing && map)
            metronome_.process(output, numFrames, *map.get(), blockStart);
    }

    if (playing)
        transport_.positionSamples.store(blockStart + numFrames, std::memory_order_relaxed);

    // ---- метры выхода -----------------------------------------------------
    const int meterChannels = output.numChannels() < kMaxMeterChannels
                            ? output.numChannels() : kMaxMeterChannels;
    for (int c = 0; c < meterChannels; ++c) {
        const float p    = output.peak(c);
        const float prev = peaks_[c].load(std::memory_order_relaxed);
        peaks_[c].store(p > prev ? p : prev * 0.85f, std::memory_order_relaxed);
    }
}

void Engine::release() {
    transport_.playing.store(false, std::memory_order_relaxed);
    metronome_.reset();
    monitorGainSmoothed_ = 0.0f;
    for (auto& p : peaks_)
        p.store(0.0f, std::memory_order_relaxed);
    for (auto& p : inputPeaks_)
        p.store(0.0f, std::memory_order_relaxed);
}

} // namespace daw
