#include "AudioRuntime.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace daw {

void AudioRuntime::transportCommand(const AudioTransportCommand& command) {
    auto& transport = engine.transport();
    using Action = AudioTransportCommand::Action;
    if (safetyStopped.load(std::memory_order_acquire) &&
        (command.action == Action::Play || command.action == Action::StartPlayback || command.action == Action::Record)) return;
    switch (command.action) {
    case Action::Play: transport.play(); break;
    case Action::StartPlayback:
        if (transport.isLoopEnabled()) {
            const auto from = transport.loopStart(), to = transport.loopEnd();
            const auto at = transport.position();
            if (to > from && (at < from || at >= to)) transport.seek(from);
            engine.preparePlayback(from);
        }
        engine.preparePlayback(transport.position());
        transport.play();
        break;
    case Action::Pause: transport.pause(); break;
    case Action::Stop: transport.stop(); break;
    case Action::Record: transport.startRecording(); break;
    case Action::Seek:
        if (command.prepare) engine.preparePlayback(std::max<engine::SamplePos>(0, command.position));
        transport.seek(command.position);
        break;
    case Action::SeekSeconds: {
        const double samples = command.value * sampleRate;
        if (!std::isfinite(samples) || samples > double(std::numeric_limits<engine::SamplePos>::max() / 2)) break;
        const auto position = engine::SamplePos(std::llround(std::max(0., samples)));
        if (command.prepare) engine.preparePlayback(position);
        transport.seek(position);
        break;
    }
    case Action::Tempo:
        if (std::isfinite(command.value) && command.value > 0) transport.setTempo(command.value);
        break;
    case Action::TimeSignature: transport.setTimeSignature(command.numerator, command.denominator); break;
    case Action::LoopRange: transport.setLoopRange(command.position, command.end); break;
    case Action::LoopEnabled: transport.setLoopEnabled(command.enabled); break;
    case Action::Duration: transport.setDuration(command.position); break;
    }
}

AudioTransportSnapshot AudioRuntime::transportSnapshot() const {
    const auto& transport = engine.transport();
    const auto state = transport.state();
    return {state, transport.position(), transport.duration(), transport.loopStart(), transport.loopEnd(),
        transport.positionSeconds(), transport.presentationPositionSeconds(), transport.tempo(),
        state == engine::TransportState::Playing || state == engine::TransportState::Recording,
        state == engine::TransportState::Recording, transport.isLoopEnabled()};
}

double AudioRuntime::inputBeatsAt(std::uint64_t timeNs) const { return engine.transport().inputBeatsAt(timeNs); }
void AudioRuntime::bindInputClock(engine::InputClock* sink, engine::AudioPresentationClock* presentation) {
    const engine::RealtimeEngine::RenderGate gate(engine);
    engine.transport().bindInputClock(sink, presentation);
}

AudioMeterSnapshot AudioRuntime::meterSnapshot(const std::string& channelId,
    const std::string& clipId, bool sampler) const {
    if (channelId == AudioGraphSpec::masterChannelId && clipId.empty() && !sampler)
        return {engine.masterPeakLeft(), engine.masterPeakRight(), engine.masterPeakHold()};
    const auto found = channels.find(channelId);
    if (found == channels.end()) return {};
    const auto& channel = found->second;
    const engine::MeterNode* meter = sampler ? channel.samplerMeter.get() : channel.meter.get();
    if (!clipId.empty()) {
        const auto clip = channel.clipFx.find(clipId);
        meter = clip != channel.clipFx.end() && clip->second.meterId != engine::kInvalidNode
            ? clip->second.meter.get() : nullptr;
    }
    return meter ? AudioMeterSnapshot{meter->peakLeft(), meter->peakRight(), meter->peakHold()}
                 : AudioMeterSnapshot{};
}

void AudioRuntime::resetMeterHold(const std::string& channelId) {
    if (channelId == AudioGraphSpec::masterChannelId) { engine.resetMasterPeakHold(); return; }
    const auto found = channels.find(channelId);
    if (found != channels.end() && found->second.meter) found->second.meter->resetPeakHold();
}

AudioRuntimeDiagnostics AudioRuntime::diagnostics() const {
    return {engine.dspLoad(), const_cast<engine::RealtimeEngine&>(engine).latencySamples(),
        engine.lastRenderError(), engine.failedBlocks(), engine.gatedBlocks(), engine.droppedProfileEvents(),
        engine.workerCount(), engine.realtimeWorkerCount(), engine.workgroupWorkerCount()};
}

AudioDeviceSnapshot AudioRuntime::deviceSnapshot() const {
    const auto counts = devices->xruns();
    return {devices->isRunning(), devices->hasStream(), devices->diagInputUsesDeviceTime(),
        devices->deviceState(), devices->sampleRate(), devices->bufferSize(),
        devices->diagCallbackCount(), devices->diagRenderCallCount(), devices->diagLastCallbackTimestamp(),
        devices->diagLastRenderStatus(),
        {counts.inputUnderflow, counts.inputOverflow, counts.outputUnderflow, counts.outputOverflow}};
}

AudioTimingSnapshot AudioRuntime::timingSnapshot(bool callback, bool drain) {
    auto& metrics = callback ? devices->callbackMetrics() : engine.graphMetrics();
    AudioTimingSnapshot out{metrics.counters(), {}};
    if (drain) {
        rt::BlockTiming event;
        for (unsigned i = 0; i < 8192 && metrics.pop(event); ++i) out.events.push_back(event);
    }
    return out;
}

void AudioRuntime::setProfiling(bool enabled) { engine.setProfiling(enabled); }
bool AudioRuntime::popProfile(unsigned worker, rt::ProfileEvent& event) { return engine.popProfile(worker, event); }
engine::LoudnessLevels AudioRuntime::masterLoudness() const { return engine.masterLoudness(); }
void AudioRuntime::resetMasterLoudness() { engine.resetMasterLoudness(); }
engine::RealtimeEngine::MasterSpectrum AudioRuntime::masterSpectrum() const { return engine.masterSpectrum(); }
void AudioRuntime::setMasterSpectrumConsumer(bool add) {
    if (add) engine.addMasterSpectrumConsumer(); else engine.removeMasterSpectrumConsumer();
}

std::vector<audio::DeviceInfo> AudioRuntime::enumerateDevices(bool input) {
    return input ? devices->enumerateInputDevices() : devices->enumerateOutputDevices();
}
audio::DeviceInfo AudioRuntime::currentDevice(bool input) const {
    return input ? devices->getCurrentInputDevice() : devices->getCurrentOutputDevice();
}
audio::AudioDeviceConfig AudioRuntime::deviceConfiguration() const { return devices->configuration(); }
bool AudioRuntime::matchesDeviceConfiguration(const audio::AudioDeviceConfig& config) const {
    return devices->isRunning() && devices->matchesConfiguration(config);
}
bool AudioRuntime::deviceNeedsRecovery() const {
    return liveDeviceAllowed && prepared &&
        (!devices->isRunning() || devices->callbackStalled() || !devices->devicesAvailable());
}
audio::Result AudioRuntime::stopDevice() {
    return devices->stop();
}
audio::Result AudioRuntime::detachDeviceCallback() { return devices->setAudioCallback(nullptr); }
audio::Result AudioRuntime::openDevice(const audio::AudioDeviceConfig& config) {
    return devices->applyConfiguration(config);
}
audio::Result AudioRuntime::refreshDevices() { return devices->refreshDevices(); }
audio::Result AudioRuntime::probeDevice(const std::string& uid, bool input, audio::DeviceInfo& out) {
    return devices->probeDevice(uid, input, out);
}
audio::Result AudioRuntime::showDeviceControlPanel(const std::string& uid, void* nativeWindow) {
    return devices->showControlPanel(uid, nativeWindow);
}
float AudioRuntime::inputPeak(std::uint32_t channel) const { return devices->diagInputPeak(audio::ChannelCount(channel)); }
unsigned AudioRuntime::configureWorkersForTest(bool realtime, unsigned maxParallelThreads) {
    if (liveDeviceAllowed || !prepared || engine.transport().isPlaying()) return 0;
    configureAudioWorkers({realtime, sampleRate, bufferSize, {}, maxParallelThreads});
    return engine.realtimeWorkerCount();
}

bool AudioRuntime::setFader(const std::string& channelId, AudioFaderTarget target,
                            const AudioFaderChange& change, const std::string& clipId) {
    const auto found = channels.find(channelId);
    if (found == channels.end()) return false;
    auto& channel = found->second;
    engine::GainNode* fader = nullptr;
    switch (target) {
    case AudioFaderTarget::Channel: fader = channel.fader.get(); break;
    case AudioFaderTarget::Sampler: fader = channel.samplerFader.get(); break;
    case AudioFaderTarget::Clip: {
        const auto clip = channel.clipFx.find(clipId);
        if (clip != channel.clipFx.end()) fader = clip->second.fader.get();
        break;
    }
    }
    if (!fader) return false;
    if (change.gain) fader->setGain(*change.gain);
    if (change.pan) fader->setPan(*change.pan);
    if (change.silent) fader->setSilent(*change.silent);
    if (change.mono) fader->setMono(*change.mono);
    return true;
}

bool AudioRuntime::hasInputRoute(const std::string& channelId) const {
    const auto found = channels.find(channelId);
    return found != channels.end() && found->second.input != nullptr;
}

bool AudioRuntime::setInput(const std::string& channelId, const AudioGraphSpec::Input& input) {
    const auto found = channels.find(channelId);
    if (found == channels.end() || !found->second.input) return false;
    auto& channel = found->second;
    channel.input->setRouting(input.channel, input.channelCount, input.enabled, input.monitorMask);
    channel.inputChannel = input.channel;
    channel.inputChannelCount = input.channelCount;
    return true;
}

bool AudioRuntime::setSend(const std::string& channelId, const std::string& sendId,
                           float level, bool enabled) {
    const auto found = channels.find(channelId);
    if (found == channels.end()) return false;
    auto& channel = found->second;
    const auto send = std::find(channel.sendIds.begin(), channel.sendIds.end(), sendId);
    if (send == channel.sendIds.end()) return false;
    const auto index = std::size_t(send - channel.sendIds.begin());
    if (index >= channel.sends.size() || !channel.sends[index]) return false;
    channel.sends[index]->setLevel(level);
    channel.sends[index]->setEnabled(enabled);
    return true;
}

void AudioRuntime::setMetronomeEnabled(bool enabled) {
    if (metronome) metronome->setEnabled(enabled);
}

void AudioRuntime::setMetronomeSample(std::shared_ptr<const engine::SampleBuffer> sample) {
    if (!metronome && sample) metronome = std::make_shared<engine::MetronomeNode>();
    if (metronome) metronome->setSample(std::move(sample));
}

void AudioRuntime::requestCountIn(int beats) {
    const auto tempo = engine.transport().tempo();
    const auto now = rt::nowNanos();
    const auto duration = beats > 0 && tempo > 0 ? double(beats) * 60.0 / tempo * 1e9 : 0;
    if (beats < 0 || !std::isfinite(duration) || duration >= double(std::numeric_limits<std::uint64_t>::max() - now))
        throw std::invalid_argument("Invalid count-in duration.");
    if (metronome) metronome->requestCountIn(beats);
    countInUntilNs = beats > 0 ? now + std::uint64_t(duration) : 0;
}

bool AudioRuntime::startPreview(std::shared_ptr<const engine::SampleBuffer> audio,
                                bool loop, double pitchSemitones) {
    if (!preview || !audio || !audio->frames() || !std::isfinite(pitchSemitones)) return false;
    preview->setLoop(loop);
    preview->setRate(std::pow(2.0, std::clamp(pitchSemitones, -36.0, 36.0) / 12.0));
    preview->start(std::move(audio));
    return true;
}

void AudioRuntime::previewCommand(const AudioPreviewCommand& command) {
    if (!std::isfinite(command.value)) throw std::invalid_argument("Invalid preview value.");
    if (!preview) return;
    switch (command.action) {
    case AudioPreviewCommand::Action::Stop: preview->stop(); break;
    case AudioPreviewCommand::Action::Loop: preview->setLoop(command.value != 0); break;
    case AudioPreviewCommand::Action::Gain: preview->setGain(float(command.value)); break;
    case AudioPreviewCommand::Action::SeekSeconds:
        if (const auto rate = preview->sourceRate(); rate > 0) {
            const auto frames = std::max(0.0, command.value) * rate;
            if (!std::isfinite(frames) || frames >= double(std::numeric_limits<std::int64_t>::max()))
                throw std::invalid_argument("Invalid preview position.");
            preview->seekFrames(std::int64_t(frames));
        }
        break;
    }
}

AudioPreviewSnapshot AudioRuntime::previewSnapshot() const {
    if (!preview) return {};
    const auto rate = preview->sourceRate();
    return {preview->playing(), rate > 0 ? double(preview->positionFrames()) / rate : 0.0};
}

} // namespace daw
