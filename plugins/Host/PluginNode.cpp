#include "Host/PluginNode.hpp"

#include "DSP/Simd.hpp"
#include "Common/RealtimeSort.hpp"
#include "RealtimeMetrics.hpp"

#include <algorithm>
#include <limits>
#include <cmath>

namespace daw::plugins {

/// The same SIMD kernels the engine's own nodes use — a plugin node moves the
/// same audio around and has no reason to hand-roll its copies and sums.
namespace dsp = engine::dsp;
// Synchronous notifications from processing describe playback, not a hand on a control.
static thread_local const PluginNode* processingPlugin=nullptr;
static std::atomic<std::uint64_t> nextInstanceId{1};

PluginNode::PluginNode(std::string name, std::unique_ptr<PluginInstance> instance)
    : m_instanceId(nextInstanceId.fetch_add(1, std::memory_order_relaxed)),
      m_name(std::move(name)), m_instance(std::move(instance)), m_sink(*this) {
    if (m_instance) {
        m_instance->setListener(this);
        const PluginDescriptor& descriptor = m_instance->descriptor();
        m_descriptor = descriptor;
        m_offlinePipelining = m_instance->supportsOfflinePipelining();
        // An instrument produces sound with no audio input, which is exactly
        // what the graph means by a source.
        m_isSource = descriptor.isInstrument || descriptor.mainInputChannels == 0;
        m_wantsMidi = descriptor.wantsMidi || descriptor.isInstrument;
        if (m_wantsMidi || descriptor.producesMidi || descriptor.format == Format::Vst || descriptor.format == Format::Unknown)
            m_midiRole = engine::MidiNodeRole::InputOutput;
        m_pluginInputChannels = descriptor.mainInputChannels;
        m_pluginOutputChannels = descriptor.mainOutputChannels;
        m_latency.store(m_instance->latencySamples(), std::memory_order_relaxed);
        // New instances get one control-thread turn even if an older format
        // queued work during construction without an explicit wake callback.
        requestMainThreadPump();
    }
}

PluginNode::~PluginNode() {
    if (m_instance) m_instance->setListener(nullptr);
}

std::vector<PluginEvent> PluginNode::pendingParameterEvents() {
    auto events = m_instance && !faultBypassed() ? m_instance->pendingParameterEvents()
                             : std::vector<PluginEvent>{};
    auto hostEvents = pendingHostEvents();
    events.insert(events.end(), hostEvents.begin(), hostEvents.end());
    std::erase_if(events, [](const PluginEvent& pending) {
        return pending.kind != PluginEvent::Kind::ParamValue;
    });
    return events;
}

std::vector<PluginEvent> PluginNode::pendingHostEvents() {
    std::vector<PluginEvent> events;
    if (m_inbound.empty()) return events;
    // Allocate before touching the queue: even an allocation failure must
    // leave the live session's parameters and MIDI exactly as they were.
    events.reserve(kEventQueueCapacity);
    PluginEvent event;
    while (m_inbound.pop(event)) events.push_back(event);
    for (const auto& pending : events) (void)m_inbound.push(pending);
    return events;
}

void PluginNode::prepare(const engine::PrepareInfo& info) {
    const bool wasPrepared = m_blockEvents.capacity() != 0;
    const auto previousLatency = m_dryDelaySamples;
    m_processFailed.store(false, std::memory_order_relaxed);
    if (!info.offline) m_hasLastOfflineConfiguration = false;
    // These requests are satisfied by this activation. New requests arriving
    // during it remain pending, except latency which is read below.
    m_restartRequested.store(false, std::memory_order_release);
    m_latencyChanged.store(false, std::memory_order_release);
    m_maxBlockSize = info.maxBlockSize;
    m_arenaChannels = info.channels;
    m_bypassWarmupRemaining = 0;
    m_pluginSleeping = false;
    m_tailFramesRemaining = 0;
    m_lastProcessDisposition = PluginProcessDisposition::Continue;
    m_sleepTransportValid = false;

    if (m_instance && !faultBypassed()) {
        if (m_instance->isActive()) {
            m_instance->stopProcessing();
            m_instance->deactivate();
        }
        PluginBusLayout current = m_instance->busLayout();
        PluginBusLayout wanted = current;
        const std::uint16_t preferred = m_preferredChannelCount.load(
            std::memory_order_acquire);
        if (!wanted.inputs.empty()) wanted.inputs[0] = preferred;
        if (!wanted.outputs.empty()) wanted.outputs[0] = preferred;
        PluginBusLayout accepted = current;
        (void)m_instance->setBusLayout(wanted, accepted);
        if (!accepted.inputs.empty()) m_pluginInputChannels = accepted.inputs[0];
        m_pluginSidechainChannels =
            accepted.inputs.size() > 1 ? accepted.inputs[1] : 0;
        if (!accepted.outputs.empty()) m_pluginOutputChannels = accepted.outputs[0];
    }

    // The plugin's own layout may be narrower or wider than the arena's fixed
    // width; allocate for whichever is larger so neither side can overrun.
    const std::uint16_t inputChannels =
        std::max<std::uint16_t>(m_pluginInputChannels, info.channels);
    const std::uint16_t outputChannels =
        std::max<std::uint16_t>(m_pluginOutputChannels, info.channels);

    m_inputStorage.assign(std::size_t(inputChannels) * info.maxBlockSize, 0.0f);
    m_sidechainStorage.assign(
        std::size_t(m_pluginSidechainChannels) * info.maxBlockSize, 0.0f);
    m_outputStorage.assign(std::size_t(outputChannels) * info.maxBlockSize, 0.0f);
    m_dryStorage.assign(std::size_t(info.channels) * info.maxBlockSize, 0.0f);
    m_inputPointers.assign(inputChannels, nullptr);
    m_sidechainPointers.assign(m_pluginSidechainChannels, nullptr);
    m_outputPointers.assign(outputChannels, nullptr);
    m_curveCursor.assign(2048, 0);

    if (!m_instance) return;
    if (faultBypassed()) {
        m_dryDelayStorage.assign(std::size_t(info.channels) * m_dryDelaySamples, 0.f);
        m_dryDelayPosition = 0;
        return;
    }
    m_ready.store(false, std::memory_order_release);

    // Reconfiguring a plugin is exactly what RealtimeEngine::RenderGate exists
    // for, and AudioGraph only calls prepare() when the settings really
    // changed — so getting here means a genuine reconfiguration.
    PluginProcessInfo processInfo;
    processInfo.sampleRate = info.sampleRate;
    processInfo.maxBlockSize = info.maxBlockSize;
    processInfo.offline = info.offline;
    processInfo.sidechainConnected = m_sidechainConnected;

    if (m_instance->activate(processInfo)) {
        m_pitchCapabilities=m_instance->pitchCapabilities();
        m_instance->startProcessing();
        const bool ready = m_instance->isProcessing();
        m_ready.store(ready, std::memory_order_release);
        const engine::FrameCount latency = ready ? m_instance->latencySamples() : 0;
        m_latency.store(latency, std::memory_order_relaxed);
        m_dryDelaySamples = latency;
        m_dryDelayPosition = 0;
        m_dryDelayStorage.assign(std::size_t(info.channels) * latency, 0.0f);
        m_latencyChanged.store(false, std::memory_order_release);
    } else {
        m_latency.store(0, std::memory_order_relaxed);
        m_dryDelaySamples = 0;
        m_dryDelayPosition = 0;
        m_dryDelayStorage.clear();
    }
    if (wasPrepared && !info.offline && !m_ready.load(std::memory_order_acquire)) {
        m_latency.store(previousLatency, std::memory_order_relaxed);
        m_dryDelaySamples = previousLatency;
        m_dryDelayStorage.assign(std::size_t(info.channels) * previousLatency, 0.f);
        m_dryDelayPosition = 0;
        latchFault(Fault::ActivationFailed);
        return;
    }
    // Activation may replace a parameter table or event-bus layout.
    const auto& descriptor = m_instance->descriptor();
    m_descriptor = descriptor;
    m_wantsMidi = descriptor.wantsMidi || descriptor.isInstrument;
    m_midiRole = m_wantsMidi || descriptor.producesMidi || descriptor.format == Format::Vst || descriptor.format == Format::Unknown
        ? engine::MidiNodeRole::InputOutput : engine::MidiNodeRole::Passthrough;
    const auto eventCapacity = pluginBlockEventCapacity(
        descriptor, processInfo, m_instance->parameters().size());
    m_blockEvents.reserve(eventCapacity);
    m_pitchDelivery.prepare(m_wantsMidi ? eventCapacity : 0);
    m_pitchCapabilities = m_instance->pitchCapabilities();
}

void PluginNode::reset() {
    m_pitchDelivery.reset();
    m_processFailed.store(false, std::memory_order_relaxed);
    // A bypassed insert may have declined activation. CLAP reset requires an
    // active instance even though that slot contributes only the dry signal.
    if (m_instance && !faultBypassed() && m_instance->isActive()) m_instance->resetForTransport();
    m_heldMidiOutput.fill(0);
    m_heldMidiOutputCount = 0;
    const bool bypassed = m_bypassed.load(std::memory_order_relaxed);
    m_wet = bypassed ? 0.0f : m_mix.load(std::memory_order_relaxed);
    m_bypassProcessorReset = bypassed;
    m_bypassWarmupRemaining = bypassed ? m_dryDelaySamples : 0;
    m_tailFramesRemaining = 0;
    m_lastProcessDisposition = PluginProcessDisposition::Continue;
    // If the format left the processor stopped for an explicit sleep, keep it
    // stopped. The invalid snapshot forces the next audible block to start it
    // again, while a reset followed by more silence remains allocation-free.
    m_sleepTransportValid = false;
    std::fill(m_dryDelayStorage.begin(), m_dryDelayStorage.end(), 0.0f);
    m_dryDelayPosition = 0;
}

engine::Status PluginNode::offlineStatus() const noexcept {
    if (faultBypassed()) return engine::fail(engine::EngineError::ProcessingFailed);
    if (isBypassed() || m_mix.load(std::memory_order_relaxed) == 0.f) return {};
    if (!m_instance || !isReady()) return engine::fail(engine::EngineError::ProcessorUnavailable);
    if (m_processFailed.load(std::memory_order_relaxed))
        return engine::fail(engine::EngineError::ProcessingFailed);
    return {};
}

engine::Status PluginNode::serviceOffline() {
    if (faultBypassed()) return engine::fail(engine::EngineError::ProcessingFailed);
    if (!m_instance || isBypassed() || m_mix.load(std::memory_order_relaxed) == 0.f) return {};
    beginMainThreadPump();
    m_instance->pumpMainThread();
    if (takeReloadRequested())
        return engine::fail(engine::EngineError::RenderConfigurationChanged);
    const bool latencyChanged = takeLatencyChanged();
    const bool restartRequested = takeRestartRequested();
    const bool restartNeedsPrepare =
        restartRequested && m_instance->serviceOfflineRestart();
    const bool reportedLatencyChanged = latencyChanged &&
        m_instance->latencySamples() !=
            m_latency.load(std::memory_order_relaxed);
    if (restartNeedsPrepare || reportedLatencyChanged) {
        const OfflineConfiguration current{
            m_instance->latencySamples(), m_instance->tailSamples(),
            m_pluginInputChannels, m_pluginSidechainChannels,
            m_pluginOutputChannels};
        // Some processors emit the same structural notification after every
        // activation. Honour it once, then keep the replacement pass when the
        // host-visible configuration is unchanged; a real change still gets a
        // fresh pass and remains protected by the controller's retry limit.
        if (!m_hasLastOfflineConfiguration ||
            current != m_lastOfflineConfiguration) {
            m_lastOfflineConfiguration = current;
            m_hasLastOfflineConfiguration = true;
            invalidatePrepare();
        }
    }
    return {};
}

void PluginNode::suspend() {
    if (m_instance) m_instance->stopProcessing();
    m_pluginSleeping = false;
    m_tailFramesRemaining = 0;
    m_lastProcessDisposition = PluginProcessDisposition::Continue;
    m_sleepTransportValid = false;
    m_ready.store(false, std::memory_order_release);
}

void PluginNode::resume() {
    if (m_instance && !faultBypassed()) {
        m_instance->startProcessing();
        m_pluginSleeping = false;
        m_tailFramesRemaining = 0;
        m_lastProcessDisposition = PluginProcessDisposition::Continue;
        m_sleepTransportValid = false;
        m_ready.store(m_instance->isProcessing(), std::memory_order_release);
    }
}

void PluginNode::Sink::push(const PluginEvent& event) noexcept {
    if(event.kind==PluginEvent::Kind::NoteEnd){m_owner.m_pitchDelivery.voiceEnded(event.noteId,event.channel,event.key);return;}
    // A plugin may echo an automation value back through its output event list.
    // The original host gesture has already been captured; playback must not
    // become a second live gesture on the next UI pump.
    if(processingPlugin==&m_owner && event.kind==PluginEvent::Kind::ParamValue &&
       std::any_of(m_owner.m_blockEvents.begin(),m_owner.m_blockEvents.end(),[&](const auto& input){return input.kind==PluginEvent::Kind::ParamValue && input.paramIndex==event.paramIndex && input.value==event.value;}))return;
    if (m_owner.m_currentMidiOutput &&
        (event.kind == PluginEvent::Kind::NoteOn ||
         event.kind == PluginEvent::Kind::NoteOff ||
         event.kind == PluginEvent::Kind::NoteChoke ||
         event.kind == PluginEvent::Kind::MidiController ||
         event.kind == PluginEvent::Kind::PolyPressure || event.kind == PluginEvent::Kind::NotePitch)) {
        engine::MidiEvent midi;
        midi.frameOffset = event.frameOffset; midi.noteId=m_owner.m_pitchDelivery.sourceNoteId(event.noteId); midi.notePan=float(event.notePan);
        const std::uint8_t channel = std::uint8_t(event.channel) & 0x0F;
        if (event.kind == PluginEvent::Kind::NoteOn) {
            midi.status = engine::MidiEvent::kNoteOn | channel;
            midi.data1 = std::uint8_t(std::clamp<int>(event.key, 0, 127));
            midi.data2 = std::uint8_t(std::lround(std::clamp(event.value, 0.0, 1.0) * 127.0));
        } else if (event.kind == PluginEvent::Kind::NoteOff ||
                   event.kind == PluginEvent::Kind::NoteChoke) {
            midi.isNoteChoke = event.kind == PluginEvent::Kind::NoteChoke;
            midi.status = engine::MidiEvent::kNoteOff | channel;
            midi.data1 = std::uint8_t(std::clamp<int>(event.key, 0, 127));
            midi.data2 = std::uint8_t(std::lround(std::clamp(event.value, 0.0, 1.0) * 127.0));
        } else if (event.kind == PluginEvent::Kind::NotePitch) {
            midi.status=channel; midi.data1=std::uint8_t(std::clamp<int>(event.key,0,127)); midi.isPitchExpression=true; midi.pitch=event.pitch;
        } else if (event.kind == PluginEvent::Kind::PolyPressure) {
            midi.status = engine::MidiEvent::kPolyPressure | channel;
            midi.data1 = std::uint8_t(std::clamp<int>(event.key, 0, 127));
            midi.data2 = std::uint8_t(std::lround(std::clamp(event.value, 0.0, 1.0) * 127.0));
        } else if (event.paramIndex <= 127) {
            midi.status = engine::MidiEvent::kControlChange | channel;
            midi.data1 = std::uint8_t(event.paramIndex);
            midi.data2 = std::uint8_t(std::lround(std::clamp(event.value, 0.0, 1.0) * 127.0));
        } else if (event.paramIndex == 128) {
            midi.status = engine::MidiEvent::kChannelPressure | channel;
            midi.data1 = std::uint8_t(std::lround(std::clamp(event.value, 0.0, 1.0) * 127.0));
        } else if (event.paramIndex == 129) {
            const int bend = int(std::lround(std::clamp(event.value, 0.0, 1.0) * 16383.0));
            midi.status = engine::MidiEvent::kPitchBend | channel;
            midi.data1 = std::uint8_t(bend & 0x7F);
            midi.data2 = std::uint8_t((bend >> 7) & 0x7F);
        } else if (event.paramIndex == 130) {
            midi.status = engine::MidiEvent::kProgramChange | channel;
            midi.data1 = std::uint8_t(std::lround(std::clamp(event.value, 0.0, 1.0) * 127.0));
        } else {
            return;
        }
        if (!m_owner.m_currentMidiOutput->push(midi)) m_owner.noteOverload();
        return;
    }
    // Dropping is correct when the control thread has not drained in a while:
    // a full ring must never stall the audio thread.
    if (m_owner.m_outbound.push(event)) m_owner.requestMainThreadPump();
}

void PluginNode::rememberMidiOutput(const engine::MidiEvent& event) noexcept {
    const std::size_t identity = std::size_t(event.channel()) * 128 +
                                 std::size_t(event.data1 & 0x7F);
    std::uint16_t& held = m_heldMidiOutput[identity];
    if (event.isNoteOn()) {
        if (held != std::numeric_limits<std::uint16_t>::max()) {
            ++held;
            ++m_heldMidiOutputCount;
        }
    } else if (event.isNoteOff() && held > 0) {
        --held;
        --m_heldMidiOutputCount;
    }
}

void PluginNode::releaseHeldMidi(engine::MidiBuffer* output) noexcept {
    if (!output || m_heldMidiOutputCount == 0) return;
    for (std::size_t identity = 0; identity < m_heldMidiOutput.size(); ++identity) {
        std::uint16_t& held = m_heldMidiOutput[identity];
        while (held > 0) {
            const auto channel = std::uint8_t(identity / 128);
            const auto key = std::uint8_t(identity % 128);
            if (!output->push(engine::MidiEvent::noteOff(0, channel, key))) return;
            --held;
            --m_heldMidiOutputCount;
        }
    }
}

void PluginNode::requestMainThreadPump() noexcept {
    if (!m_mainThreadWorkPending.exchange(true, std::memory_order_acq_rel)) {
        PluginMainThreadWork::request();
    }
}

void PluginNode::onParameterChanged(std::uint32_t index, double plainValue) noexcept {
    if(processingPlugin==this)return;
    PluginEvent event;
    event.kind = PluginEvent::Kind::ParamValue;
    event.paramIndex = index;
    event.value = plainValue;
    if (m_outbound.push(event)) requestMainThreadPump();
}

void PluginNode::onParameterGesture(std::uint32_t index, bool begin) noexcept {
    PluginEvent event;
    event.kind = begin ? PluginEvent::Kind::ParamGestureBegin
                       : PluginEvent::Kind::ParamGestureEnd;
    event.paramIndex = index;
    if (m_outbound.push(event)) requestMainThreadPump();
}

void PluginNode::onLatencyChanged() noexcept {
    m_latencyChanged.store(true, std::memory_order_release);
    requestMainThreadPump();
}

void PluginNode::onRestartRequested() noexcept {
    m_restartRequested.store(true, std::memory_order_release);
    requestMainThreadPump();
}

void PluginNode::onReloadRequested() noexcept {
    m_reloadRequested.store(true, std::memory_order_release);
    requestMainThreadPump();
}

void PluginNode::onStateChanged() noexcept {
    m_stateChanged.store(true, std::memory_order_release);
    requestMainThreadPump();
}

void PluginNode::forwardMidi(const engine::ProcessContext& context) noexcept {
    if (!context.midiOutput) return;
    // Returning to the dry route changes note identity. Release transformed
    // voices before forwarding original notes and their releases downstream.
    releaseHeldMidi(context.midiOutput);
    for (std::size_t i = 0; i < context.midiInputs.size(); ++i) {
        if (i < context.inputRoles.size() && context.inputRoles[i] == engine::InputRole::Sidechain) continue;
        const auto* midi = context.midiInputs[i];
        if (!midi) continue;
        for (const auto& event : midi->events()) (void)context.midiOutput->push(event);
    }
}

void PluginNode::process(const engine::ProcessContext& context) {
    if (!context.offline) m_processFailed.store(false, std::memory_order_relaxed);
    const engine::ChannelCount outChannels = context.output.numChannels();
    const engine::FrameCount frames = context.frames;
    const auto steadyTime = m_steadyTime;
    m_steadyTime = m_steadyTime >= 0 &&
            m_steadyTime <= std::numeric_limits<std::int64_t>::max() - frames
        ? m_steadyTime + frames : -1;
    auto isSidechain = [&](std::size_t index) noexcept {
        return index < context.inputRoles.size() &&
               context.inputRoles[index] == engine::InputRole::Sidechain;
    };
    // Whatever happens below, this block must be fully defined: the arena
    // recycles buffers and hands them over holding the previous owner's audio.
    auto passThrough = [&] {
        for (engine::ChannelCount ch = 0; ch < outChannels; ++ch) {
            const std::span<float> out = context.output.channel(ch).first(frames);
            bool wrote = false;
            for (std::size_t index = 0; index < context.inputs.size(); ++index) {
                if (isSidechain(index)) continue;
                const engine::AudioBlock& input = context.inputs[index];
                if (ch >= input.numChannels()) continue;
                const std::span<const float> in = input.channel(ch).first(frames);
                if (!wrote) {
                    dsp::copyScaled(out, in, 1.0f);
                    wrote = true;
                } else {
                    dsp::addScaled(out, in, 1.0f);
                }
            }
            if (!wrote) dsp::clear(out);
        }
    };

    if (!m_instance || (!m_ready.load(std::memory_order_acquire) && !faultBypassed()) ||
        frames > m_maxBlockSize) {
        passThrough();
        // A missing/unprepared audio insert is still a transparent part of the
        // MIDI route. Otherwise opening a project while a plugin is unavailable
        // can leave the downstream instrument silent or holding notes.
        forwardMidi(context);
        return;
    }

    // ── Sum the main input bus ──
    //
    // Several producers can feed one insert (clips plus a live input, say), and
    // the plugin sees one bus, so they are summed here — the same thing SumNode
    // does, done in place so no extra node is needed in the chain.
    const std::uint16_t inChannels =
        std::min<std::uint16_t>(m_pluginInputChannels,
                                std::uint16_t(m_inputPointers.size()));
    const bool preferMono =
        m_preferredChannelCount.load(std::memory_order_acquire) == 1;
    for (std::uint16_t ch = 0; ch < inChannels; ++ch) {
        float* destination = m_inputStorage.data() + std::size_t(ch) * m_maxBlockSize;
        m_inputPointers[ch] = destination;
        const std::span<float> out(destination, frames);
        bool wrote = false;
        for (std::size_t index = 0; index < context.inputs.size(); ++index) {
            if (isSidechain(index)) continue;
            const engine::AudioBlock& input = context.inputs[index];
            if (input.numChannels() == 0) continue;

            // A mono track is folded *before* its inserts, so a flexible
            // plugin receives its mono variant and a stereo-only fallback gets
            // identical L/R rather than processing an accidental left-only
            // signal. A natively mono plugin gets the same correct fold even
            // when it was inserted on a stereo track.
            if (preferMono || inChannels == 1) {
                const float scale = 1.0f / float(input.numChannels());
                for (engine::ChannelCount source = 0;
                     source < input.numChannels(); ++source) {
                    const std::span<const float> in =
                        input.channel(source).first(frames);
                    if (!wrote) {
                        dsp::copyScaled(out, in, scale);
                        wrote = true;
                    } else {
                        dsp::addScaled(out, in, scale);
                    }
                }
            } else {
                // A stereo plugin fed a genuinely mono producer repeats that
                // producer into both sides; normal stereo maps straight across.
                const engine::ChannelCount source =
                    ch < input.numChannels() ? ch : engine::ChannelCount(0);
                const std::span<const float> in =
                    input.channel(source).first(frames);
                if (!wrote) {
                    dsp::copyScaled(out, in, 1.0f);
                    wrote = true;
                } else {
                    dsp::addScaled(out, in, 1.0f);
                }
            }
        }
        if (!wrote) dsp::clear(out);
    }

    // ── Sum the first auxiliary input bus ──
    //
    // Sidechain edges share the graph's PDC and scheduler with ordinary audio,
    // but are kept out of the main/dry signal here. Formats without an aux bus
    // simply leave `m_pluginSidechainChannels` at zero.
    const std::uint16_t sidechainChannels =
        std::min<std::uint16_t>(m_pluginSidechainChannels,
                                std::uint16_t(m_sidechainPointers.size()));
    for (std::uint16_t ch = 0; ch < sidechainChannels; ++ch) {
        float* destination = m_sidechainStorage.data() +
                             std::size_t(ch) * m_maxBlockSize;
        m_sidechainPointers[ch] = destination;
        const std::span<float> out(destination, frames);
        bool wrote = false;
        for (std::size_t index = 0; index < context.inputs.size(); ++index) {
            if (!isSidechain(index)) continue;
            const engine::AudioBlock& input = context.inputs[index];
            if (input.numChannels() == 0) continue;
            const engine::ChannelCount source =
                ch < input.numChannels() ? ch : engine::ChannelCount(0);
            const std::span<const float> in = input.channel(source).first(frames);
            if (!wrote) {
                dsp::copyScaled(out, in, 1.0f);
                wrote = true;
            } else {
                dsp::addScaled(out, in, 1.0f);
            }
        }
        if (!wrote) dsp::clear(out);
    }

    const bool hostEventPending = !m_inbound.empty();
    const bool failed = faultBypassed();
    const bool pluginWakeRequested = !failed && m_instance->takeProcessWakeRequest();

    // ── Keep the dry signal when a crossfade is in flight ──
    const bool bypassed = m_bypassed.load(std::memory_order_relaxed);
    // A reset processor has an empty latency line. Keep the aligned dry path
    // audible until enough fresh input has passed through it, then crossfade.
    const bool warming = !bypassed && m_bypassWarmupRemaining != 0;
    const float targetWet = bypassed || warming ? 0.0f : m_mix.load(std::memory_order_relaxed);
    const bool needsDry = failed || (m_wet != 1.0f) || (targetWet != 1.0f);
    if (!bypassed) m_bypassProcessorReset = false;
    if (needsDry || m_dryDelaySamples > 0) {
        for (engine::ChannelCount ch = 0; ch < outChannels; ++ch) {
            float* destination = m_dryStorage.data() + std::size_t(ch) * m_maxBlockSize;
            const std::span<float> dry(destination, frames);
            if (ch < inChannels) {
                dsp::copyScaled(dry,
                                std::span<const float>(
                                    m_inputStorage.data() +
                                        std::size_t(ch) * m_maxBlockSize,
                                    frames),
                                1.0f);
            } else if (inChannels > 0) {
                dsp::copyScaled(
                    dry, std::span<const float>(m_inputStorage.data(), frames), 1.0f);
            } else {
                dsp::clear(dry);
            }
        }

        if (m_dryDelaySamples > 0 && !m_dryDelayStorage.empty()) {
            for (engine::ChannelCount ch = 0; ch < outChannels; ++ch) {
                float* dry = m_dryStorage.data() + std::size_t(ch) * m_maxBlockSize;
                float* ring = m_dryDelayStorage.data() + std::size_t(ch) * m_dryDelaySamples;
                engine::FrameCount position = m_dryDelayPosition, offset = 0;
                while (offset < frames) {
                    const auto count = std::min(frames - offset, m_dryDelaySamples - position);
                    std::swap_ranges(dry + offset, dry + offset + count, ring + position);
                    offset += count; position += count;
                    if (position == m_dryDelaySamples) position = 0;
                }
            }
            m_dryDelayPosition = (m_dryDelayPosition + frames) % m_dryDelaySamples;
        }
    }

    if (failed) {
        renderFaultFallback(context, true);
        return;
    }

    // Once the wet-to-dry crossfade has completed, the plugin cannot
    // contribute to either audio or MIDI. Keep feeding the host-owned dry
    // latency line, but stop calling third-party DSP until bypass is released.
    // VST2 has no realtime reset: keep its DSP current so an old tail cannot
    // reappear on release, and reset it only on the parked control thread.
    // A queued host edit is the one deliberate exception: PluginInstance has
    // no format-independent way to apply processor parameters without a
    // process callback, and postponing it would make saveState() capture stale
    // opaque state for a bypassed insert. One event-bearing block updates that
    // state; the following quiet block sleeps again. Automation needs no such
    // exception because its current value is recomputed at the wake playhead.
    const bool processorUnavailable = !m_instance->isActive();
    if (bypassed && (processorUnavailable || (m_wet == 0.0f && !hostEventPending &&
        !pluginWakeRequested && m_instance->supportsRealtimeReset()))) {
        m_curveCursorFor = nullptr;
        if (!m_bypassProcessorReset && !processorUnavailable) {
            m_instance->reset();
            m_bypassProcessorReset = true;
            m_bypassWarmupRemaining = m_dryDelaySamples;
        }
        m_tailFramesRemaining = 0;
        m_wet = 0.0f;
        m_lastProcessDisposition = PluginProcessDisposition::Continue;
        for (engine::ChannelCount ch = 0; ch < outChannels; ++ch) {
            dsp::copyScaled(
                context.output.channel(ch).first(frames),
                std::span<const float>(
                    m_dryStorage.data() + std::size_t(ch) * m_maxBlockSize,
                    frames),
                1.0f);
        }
        forwardMidi(context);
        rememberTransport(context);
        return;
    }



    // Exact, contract-grade silence information. This is deliberately not a
    // threshold: denormals, NaNs and every non-zero sample are real input and
    // must wake a sleeping plugin. The scan is bounded by the configured block
    // and channel counts and uses no callback-time storage. It intentionally
    // sits after the permanent-bypass return, so bypassed DSP pays no scan cost.
    bool anyNonZeroInput = false;
    auto exactSilenceMask = [&](const std::vector<float>& storage,
                                std::uint16_t channels) noexcept {
        std::uint64_t mask = 0;
        for (std::uint16_t ch = 0; ch < channels; ++ch) {
            const float* samples = storage.data() +
                                   std::size_t(ch) * m_maxBlockSize;
            const bool silent = dsp::isSilent({samples, frames});
            anyNonZeroInput |= !silent;
            if (silent && ch < 64) mask |= std::uint64_t{1} << ch;
        }
        return mask;
    };
    const std::uint64_t inputSilenceMask =
        exactSilenceMask(m_inputStorage, inChannels);
    const std::uint64_t sidechainSilenceMask =
        exactSilenceMask(m_sidechainStorage, sidechainChannels);

    bool hasMainMidiInput = false;
    if (m_wantsMidi) {
        for (std::size_t midiIndex = 0; midiIndex < context.midiInputs.size();
             ++midiIndex) {
            if (isSidechain(midiIndex)) continue;
            const engine::MidiBuffer* midi = context.midiInputs[midiIndex];
            if (midi && !midi->empty()) {
                hasMainMidiInput = true;
                break;
            }
        }
    }
    auto automation = m_automation.read();
    const bool automationActive = automation && !automation->empty();

    // Track only transport properties that are expected to stay constant from
    // one uninterrupted block to the next. PPQ and bar position advance during
    // normal playback, so comparing them directly would wake on every block;
    // timeline continuity detects seeks and loop wraps without that false hit.
    bool transportChanged = false;
    if (m_sleepTransportValid) {
        const engine::SamplePos expectedPosition =
            m_sleepTimelinePosition +
            (m_sleepPlaying ? engine::SamplePos(m_sleepBlockFrames) : 0);
        const engine::TransportInfo& before = m_sleepTransport;
        const engine::TransportInfo& now = context.transport;
        transportChanged =
            context.timelinePosition != expectedPosition ||
            context.playing != m_sleepPlaying || context.offline != m_sleepOffline ||
            now.tempo != before.tempo ||
            now.timeSigNumerator != before.timeSigNumerator ||
            now.timeSigDenominator != before.timeSigDenominator ||
            now.looping != before.looping || now.recording != before.recording ||
            now.loopStartPpq != before.loopStartPpq ||
            now.loopEndPpq != before.loopEndPpq;
    } else if (m_pluginSleeping) {
        // reset() deliberately invalidates the snapshot; the first subsequent
        // block must re-establish the format's processing state.
        transportChanged = true;
    }

    // Only a format's explicit process disposition may put the instance here.
    // Output amplitude alone is insufficient: a generator or synced LFO can
    // become audible later without receiving audio input. Every host-visible
    // stimulus wakes before this block's event list is consumed.
    auto renderSleepingOutput = [&](bool forceMidiPassthrough) noexcept {
        const float dryGain = 1.0f - m_wet;
        for (engine::ChannelCount ch = 0; ch < outChannels; ++ch) {
            const std::span<float> out = context.output.channel(ch).first(frames);
            if (dryGain == 0.0f) {
                dsp::clear(out);
            } else {
                dsp::copyScaled(
                    out,
                    std::span<const float>(
                        m_dryStorage.data() + std::size_t(ch) * m_maxBlockSize,
                        frames),
                    dryGain);
            }
        }
        if (!m_wantsMidi || forceMidiPassthrough) forwardMidi(context);
    };
    const bool mixTransition = m_wet != targetWet;
    if (m_pluginSleeping) {
        const bool mustWake = anyNonZeroInput || hasMainMidiInput ||
                              hostEventPending || automationActive ||
                              transportChanged || pluginWakeRequested ||
                              mixTransition;
        if (!mustWake) {
            renderSleepingOutput(false);
            rememberTransport(context);
            return;
        }
        if (!m_instance->wakeProcessing()) {
            latchFault(Fault::ActivationFailed);
            // A failed format transition cannot be repaired in the callback.
            // Define audio and preserve MIDI rather than invoking process while
            // the format says the instance is stopped.
            renderFaultFallback(context, needsDry || m_dryDelaySamples > 0);
            return;
        }
        m_pluginSleeping = false;
        m_tailFramesRemaining = 0;
    }

    // ── Output pointers, one channel at a time ──
    //
    // Never `base + ch * frames`: the arena pads every channel to a cache line,
    // so the channels of a block are not contiguous.
    const std::uint16_t outCount =
        std::min<std::uint16_t>(m_pluginOutputChannels,
                                std::uint16_t(m_outputPointers.size()));
    for (std::uint16_t ch = 0; ch < outCount; ++ch) {
        // Channels past the arena's width go to this node's own scratch, not
        // to `m_inputStorage`: that buffer is only as wide as the *input*, so a
        // plugin with more outputs than inputs — every surround variant — wrote
        // straight past the end of it. A heap overflow on the audio thread,
        // which is as bad as it sounds.
        m_outputPointers[ch] =
            ch < outChannels ? context.output.data(ch)
                             : m_outputStorage.data() + std::size_t(ch) * m_maxBlockSize;
    }

    // A third-party plugin is supposed to define every output sample (or use
    // its format's explicit silence indication), but a number of otherwise
    // working plugins simply stop touching their output when the transport is
    // parked. These buffers are recycled, so leaving one untouched repeats
    // the previous block forever — the short buzz heard after pressing Space.
    // Start from silence at the plugin boundary: a conforming plugin overwrites
    // it, while a plugin that writes nothing now produces silence instead of a
    // stale block. This also covers auxiliary channels that land in our own
    // scratch rather than in the graph arena.
    for (std::uint16_t ch = 0; ch < outCount; ++ch) {
        dsp::clear(std::span<float>(m_outputPointers[ch], frames));
    }

    // ── Events for this block ──
    //
    // Two sources, one list: parameter changes the host queued, and the MIDI
    // arriving on this node's edges. They are sorted together because a plugin
    // is entitled to see them in time order — a parameter set before a note-on
    // at the same frame must land before it.
    m_blockEvents.clear();

    if (m_eventResyncPending) {
        m_eventResyncPending = false;
        if (m_instance->supportsRealtimeReset()) m_instance->reset();
        else if (m_wantsMidi) for (std::int16_t channel = 0; channel < 16; ++channel) {
            PluginEvent release;
            release.kind = PluginEvent::Kind::MidiController;
            release.paramIndex = 123; release.channel = channel;
            m_blockEvents.push_back(release);
        }
    }

    PluginEvent event;
    while (m_inbound.pop(event)) {
        if (event.frameOffset >= frames) event.frameOffset = frames > 0 ? frames - 1 : 0;
        if (m_blockEvents.size() < m_blockEvents.capacity()) {
            m_blockEvents.push_back(event);
        } else noteOverload();
    }
    // ── MIDI notes ──
    //
    // Collected before automation on purpose: a dense automation curve must
    // never starve the note stream. Everything is stable-sorted by frameOffset
    // below, so the collection order does not affect what the plugin sees.
    // Audio-only inserts forward the original buffer below. Converting every
    // event into PluginEvent here would make a dense MIDI project pay twice for
    // a stream the third-party processor cannot consume.
    auto appendBlockEvent = [&](const PluginEvent& incoming) noexcept {
        if (m_blockEvents.size() < m_blockEvents.capacity()) {
            m_blockEvents.push_back(incoming);
            return true;
        }

        const bool isRelease = incoming.kind == PluginEvent::Kind::NoteOff ||
                               incoming.kind == PluginEvent::Kind::NoteChoke;
        noteOverload();
        if (!isRelease) return false;

        auto sameVoice = [&](const PluginEvent& queued) noexcept {
            if (incoming.noteId >= 0 && queued.noteId >= 0) {
                return incoming.noteId == queued.noteId;
            }
            return incoming.channel == queued.channel &&
                   incoming.key == queued.key;
        };
        auto replacement = m_blockEvents.end();
        for (auto it = m_blockEvents.end(); it != m_blockEvents.begin();) {
            --it;
            if (it->kind != PluginEvent::Kind::NoteOn &&
                it->kind != PluginEvent::Kind::NoteOff &&
                it->kind != PluginEvent::Kind::NoteChoke) {
                replacement = it;
                break;
            }
        }
        if (replacement == m_blockEvents.end()) {
            for (auto it = m_blockEvents.end(); it != m_blockEvents.begin();) {
                --it;
                if (it->kind == PluginEvent::Kind::NoteOn && !sameVoice(*it)) {
                    replacement = it;
                    break;
                }
            }
        }
        if (replacement == m_blockEvents.end()) {
            for (auto it = m_blockEvents.end(); it != m_blockEvents.begin();) {
                --it;
                if (it->kind == PluginEvent::Kind::NoteOn) {
                    replacement = it;
                    break;
                }
            }
        }
        if (replacement == m_blockEvents.end()) return false;

        // Preserve arrival order at equal frame offsets. An in-place replacement
        // could put the rescued release before its matching note-on; shifting the
        // victim out and appending keeps the release last without allocating.
        std::move(replacement + 1, m_blockEvents.end(), replacement);
        m_blockEvents.back() = incoming;
        return true;
    };

    if (m_wantsMidi) {
        for (std::size_t midiIndex = 0; midiIndex < context.midiInputs.size();
             ++midiIndex) {
            if (isSidechain(midiIndex)) continue;
            const engine::MidiBuffer* midi = context.midiInputs[midiIndex];
            if (!midi) continue;
            for (const engine::MidiEvent& note : midi->events()) {
                PluginEvent converted;
                converted.frameOffset = std::min<std::uint32_t>(
                    note.frameOffset, frames > 0 ? frames - 1 : 0);
                converted.channel = std::int16_t(note.channel());
                converted.key = std::int16_t(note.data1);
                converted.noteId = note.noteId;
                converted.pitch = note.pitch;
                if (note.isPitchExpression) {
                    converted.kind = PluginEvent::Kind::NotePitch;
                    converted.value = note.pitch.at(0);
                } else if (note.isNoteOn()) {
                    converted.kind = PluginEvent::Kind::NoteOn;
                    converted.value = double(note.data2) / 127.0;
                    converted.notePan =
                        std::clamp(double(note.notePan), -1.0, 1.0);
                } else if (note.isNoteOff()) {
                    converted.kind = note.isNoteChoke ? PluginEvent::Kind::NoteChoke
                                                      : PluginEvent::Kind::NoteOff;
                    converted.value = double(note.data2) / 127.0;
                } else if (note.type() == engine::MidiEvent::kControlChange) {
                    converted.kind = PluginEvent::Kind::MidiController;
                    converted.paramIndex = note.data1;
                    converted.value = double(note.data2) / 127.0;
                } else if (note.type() == engine::MidiEvent::kPitchBend) {
                    converted.kind = PluginEvent::Kind::MidiController;
                    converted.paramIndex = 129;
                    converted.value =
                        double(std::uint16_t(note.data1) |
                               (std::uint16_t(note.data2) << 7)) /
                        16383.0;
                } else if (note.type() == engine::MidiEvent::kChannelPressure) {
                    converted.kind = PluginEvent::Kind::MidiController;
                    converted.paramIndex = 128;
                    converted.value = double(note.data1) / 127.0;
                } else if (note.type() == engine::MidiEvent::kPolyPressure) {
                    converted.kind = PluginEvent::Kind::PolyPressure;
                    converted.value = double(note.data2) / 127.0;
                } else if (note.type() == engine::MidiEvent::kProgramChange) {
                    converted.kind = PluginEvent::Kind::MidiController;
                    converted.paramIndex = 130;
                    converted.value = double(note.data1) / 127.0;
                } else {
                    continue;
                }
                (void)appendBlockEvent(converted);
            }
        }
    }
    // ── Automation: the curves, sampled against this block's playhead ──
    //
    // Sample the curve independently of device/export block size. Constant
    // runs only need their first event; moving segments preserve every sample.
    if ((m_overrideWasPlaying && !context.playing) ||
        (context.playing && context.transport.ppqPosition < m_overrideLastBeat))
        m_overrideEpoch.fetch_add(1, std::memory_order_acq_rel);
    m_overrideWasPlaying = context.playing; m_overrideLastBeat = context.transport.ppqPosition;
    const auto overrides = m_automationOverrides.read();
    const auto overrideEpoch = m_overrideEpoch.load(std::memory_order_acquire);
    if (automation) {
        const AutomationCurves* curves = automation.get();
        const double tempo =
            context.transport.tempo > 0.0 ? context.transport.tempo : 120.0;
        const double samplesPerBeat = context.sampleRate * 60.0 / tempo;
        const double blockStartBeats = context.transport.ppqPosition;

        // The per-curve cursors are only valid while the playhead moves forward
        // through the same snapshot. A seek, a loop restart, or a fresh set of
        // curves all invalidate them; resetting here is cheap and rare.
        const std::size_t curveCount =
            std::min(curves->size(), m_curveCursor.size());
        if (m_curveCursorFor != curves ||
            blockStartBeats < m_lastBlockStartBeats) {
            for (std::size_t ci = 0; ci < curveCount; ++ci) {
                const auto& points = (*curves)[ci].points;
                m_curveCursor[ci] = std::size_t(std::upper_bound(
                    points.begin(), points.end(), blockStartBeats,
                    [](double beat, const auto& point) {
                        return beat < point.first;
                    }) - points.begin());
            }
            m_curveCursorFor = curves;
        }
        m_lastBlockStartBeats = blockStartBeats;

        for (std::size_t ci = 0; ci < curveCount; ++ci) {
            const AutomationCurve& curve = (*curves)[ci];
            if (overrides && std::any_of(overrides->begin(), overrides->end(), [&](const auto& entry) {
                    return entry.first == curve.parameterIndex && entry.second == overrideEpoch;
                })) continue;
            std::size_t& cursor = m_curveCursor[ci];
            // The preceding pass may overlap this one (a locate, or an offline
            // probe). Per-sample evaluation advanced its cursor to that pass's
            // end, even when the new block starts after its old start.
            if (cursor && curve.points[cursor - 1].first > blockStartBeats)
                cursor = std::size_t(std::upper_bound(curve.points.begin(), curve.points.end(),
                    blockStartBeats, [](double beat, const auto& point) { return beat < point.first; })
                    - curve.points.begin());

            double previous = std::numeric_limits<double>::quiet_NaN();
            double previousBeat = blockStartBeats;
            for (std::uint32_t frame = 0; frame < frames; ++frame) {
                const double beat = context.ppqAtOffset(frame);
                if (beat < previousBeat) cursor = 0;
                previousBeat = beat;
                // Points inside the same sample retain their final value, as
                // in the event path. Interpolate every intervening sample too:
                // CLAP/VST2 need actual values; VST3 receives the same curve.
                const double sampleEnd = beat + 1.0 / std::max(samplesPerBeat, 1.0);
                while (cursor < curve.points.size() &&
                       curve.points[cursor].first < sampleEnd - 1e-12) ++cursor;
                double value = curve.defaultValue;
                if (cursor) {
                    const auto& left = curve.points[cursor - 1];
                    value = left.second;
                    if (left.first <= beat && cursor < curve.points.size()) {
                        const auto& right = curve.points[cursor];
                        const double span = right.first - left.first;
                        if (span > 0.0)
                            value += (right.second - left.second) * ((beat - left.first) / span);
                    }
                }
                if (frame && value == previous) continue;
                previous = value;
                if (m_blockEvents.size() == m_blockEvents.capacity()) {
                    noteOverload();
                    break; // report overload; never export a silently truncated curve
                }
                PluginEvent event;
                event.kind = PluginEvent::Kind::ParamValue;
                event.paramIndex = curve.parameterIndex;
                event.frameOffset = frame;
                event.value = value;
                m_blockEvents.push_back(event);
            }
        }
    }

    for (std::uint32_t i = 0; i < m_blockEvents.size(); ++i) {
        m_blockEvents[i].sortOrder = i;
    }
    // std::sort is allocation-free introsort: O(n log n) even for dense
    // automation, with sortOrder preserving the former stable semantics.
    std::sort(m_blockEvents.begin(), m_blockEvents.end(),
              [](const PluginEvent& a, const PluginEvent& b) {
                  if (a.frameOffset != b.frameOffset) {
                      return a.frameOffset < b.frameOffset;
                  }
                  return a.sortOrder < b.sortOrder;
              });

    PluginProcessContext processContext;
    processContext.inputs = inChannels > 0 ? m_inputPointers.data() : nullptr;
    processContext.inputChannels = inChannels;
    processContext.sidechainInputs =
        sidechainChannels > 0 ? m_sidechainPointers.data() : nullptr;
    processContext.sidechainInputChannels = sidechainChannels;
    processContext.inputSilenceMask = inputSilenceMask;
    processContext.sidechainSilenceMask = sidechainSilenceMask;
    processContext.outputs = m_outputPointers.data();
    processContext.outputChannels = outCount;
    processContext.frames = frames;
    processContext.inputEvents = m_wantsMidi ? m_pitchDelivery.process(m_blockEvents, frames, context.sampleRate,
        slideDelivery(), (m_slideMode.load()==SlideDelivery::Auto && slideDelivery()==SlideDelivery::MPE && m_slideRange.load()==2 ? 48. : m_slideRange.load()), m_slideTail.load(), m_instance->tailSamplesKnown() ? m_instance->tailSamples() : 0xffffffffu, m_pitchCapabilities.continuous)
        : std::span<const PluginEvent>(m_blockEvents);
    m_slideOverloaded.store(m_pitchDelivery.overloaded);
    m_slideClipped.store(m_pitchDelivery.clipped);
    processContext.outputEvents = &m_sink;
    processContext.transport = context.transport;
    processContext.sampleTime = context.timelinePosition;
    processContext.steadyTime = steadyTime;
    processContext.playing = context.playing;
    processContext.offline = context.offline;

    // Ordinary audio effects are transparent to the MIDI stream. A plugin that
    // explicitly declares MIDI/event input owns the stream and must emit what
    // it wants downstream, which is how MIDI transformers avoid duplicates.
    if (!m_wantsMidi || bypassed) forwardMidi(context);
    // While bypass is crossing over, the plugin still renders its audible wet
    // tail but no longer owns the MIDI path: forwarding both its transformed
    // events and the transparent dry stream would duplicate notes downstream.
    m_currentMidiOutput = bypassed ? nullptr : context.midiOutput;
    const BlockState block{context, processContext.outputChannels, targetWet, bypassed, needsDry,
        !anyNonZeroInput && !automationActive,
        anyNonZeroInput || hasMainMidiInput || hostEventPending || automationActive ||
        transportChanged || pluginWakeRequested || mixTransition,
        context.midiOutput ? context.midiOutput->size() : 0};
    // Overflow is exceptional. Keep the normal callback read-only on this
    // control-thread flag instead of taking cache-line ownership every block.
    if (m_inboundOverload.load(std::memory_order_acquire) &&
        m_inboundOverload.exchange(false, std::memory_order_acquire)) noteOverload();
    if (m_processFailed.load(std::memory_order_relaxed)) {
        m_currentMidiOutput = nullptr;
        renderFaultFallback(context, needsDry || m_dryDelaySamples > 0);
        return;
    }
    const auto* previousProcessingPlugin=processingPlugin;
    processingPlugin=this;
    const auto disposition = m_instance->process(processContext);
    processingPlugin=previousProcessingPlugin;
    m_currentMidiOutput = nullptr;
    finishAudio(block, disposition);
}

void PluginNode::rememberTransport(const engine::ProcessContext& context) noexcept {
    m_sleepTimelinePosition = context.timelinePosition;
    m_sleepBlockFrames = context.frames;
    m_sleepPlaying = context.playing;
    m_sleepOffline = context.offline;
    m_sleepTransport = context.transport;
    m_sleepTransportValid = true;
}

void PluginNode::latchFault(Fault reason) noexcept {
    if (m_fault.exchange(reason, std::memory_order_acq_rel) == Fault::None) {
        m_faultGroup->sides.fetch_or(1u << m_faultSide, std::memory_order_release);
        if (m_masterMute) m_masterMute->store(true, std::memory_order_release);
        s_faultGeneration.fetch_add(1, std::memory_order_release);
        PluginMainThreadWork::request();
    }
}

void PluginNode::noteOverload() noexcept {
    m_eventResyncPending = true;
    m_processFailed.store(true, std::memory_order_relaxed);
    if (!m_overloadNotice.exchange(true, std::memory_order_acq_rel)) PluginMainThreadWork::request();
}

void PluginNode::renderFaultFallback(const engine::ProcessContext& context, bool dryReady) noexcept {
    // The dry ring was maintained before the foreign call, including at 100% wet.
    // Never crossfade a foreign NaN or partially written output into this signal.
    for (engine::ChannelCount ch = 0; ch < context.output.numChannels(); ++ch) {
        const auto out = context.output.channel(ch).first(context.frames);
        if (m_isSource || (m_masterMute && m_masterMute->load(std::memory_order_acquire)) || !m_pluginInputChannels)
            dsp::clear(out);
        else {
            const auto sourceChannel = std::min<unsigned>(ch, m_pluginInputChannels - 1);
            const float* source = dryReady ? m_dryStorage.data() + std::size_t(ch) * m_maxBlockSize
                : m_inputStorage.data() + std::size_t(sourceChannel) * m_maxBlockSize;
            dsp::copyScaled(out, {source, context.frames}, 1.f);
            for (auto& sample : out) if (!std::isfinite(sample)) sample = 0.f;
        }
    }
    // Discard foreign output, release transformed voices and preserve trusted
    // dry MIDI. Dropping an audio effect's note-off would strand a later synth.
    if (context.midiOutput) context.midiOutput->clear();
    if (!m_isSource) forwardMidi(context);
    else releaseHeldMidi(context.midiOutput);
    rememberTransport(context);
}

void PluginNode::finishAudio(const BlockState& block, PluginProcessDisposition disposition) noexcept {
    const auto& context = block.context;
    const auto frames = context.frames;
    const auto outChannels = context.output.numChannels();
    const auto outCount = block.outputChannels;
    const bool bypassed = block.bypassed, needsDry = block.needsDry;
    const float targetWet = block.targetWet;
    m_bypassWarmupRemaining -= std::min(m_bypassWarmupRemaining, frames);
    bool finiteOutput = true;
    for (engine::ChannelCount ch = 0; ch < outCount; ++ch) {
        // Surround channels can live in our scratch beyond the graph's width.
        const auto samples = std::span<const float>(m_outputPointers[ch], frames);
        finiteOutput &= std::all_of(samples.begin(), samples.end(), [](float x) { return std::isfinite(x); });
    }
    if (disposition == PluginProcessDisposition::Error || !finiteOutput) {
        latchFault(finiteOutput ? Fault::ProcessError : Fault::NonFiniteOutput);
        m_processFailed.store(true, std::memory_order_relaxed);
        renderFaultFallback(context, needsDry || m_dryDelaySamples > 0);
        return;
    }
    if (bypassed) m_bypassProcessorReset = false;
    if (m_processFailed.load(std::memory_order_relaxed) || faultBypassed()) {
        renderFaultFallback(context, needsDry || m_dryDelaySamples > 0);
        return;
    }
    // Output MIDI is committed only after DSP succeeds. A vendor can emit a
    // note-off and then fail; discarding that block must still release the
    // previously audible note through the trusted fallback path.
    if (context.midiOutput) {
        const auto events = context.midiOutput->events();
        for (std::size_t i = block.firstPluginMidi; i < events.size(); ++i) rememberMidiOutput(events[i]);
    }

    // A plugin with fewer output channels than the arena leaves the rest
    // holding another node's audio. Fill them from what it did write.
    for (engine::ChannelCount ch = outCount; ch < outChannels; ++ch) {
        const std::span<float> out = context.output.channel(ch).first(frames);
        if (outCount > 0) {
            dsp::copyScaled(out,
                            std::span<const float>(context.output.data(0), frames),
                            1.0f);
        } else {
            dsp::clear(out);
        }
    }

    // ── Bypass crossfade ──
    if (needsDry) {
        // One linear ramp per block, the same shape GainNode uses: short enough
        // to be inaudible as a fade, long enough to remove the click.
        const float startWet = m_wet;
        for (engine::ChannelCount ch = 0; ch < outChannels; ++ch) {
            float* wetData = context.output.data(ch);
            const float* dryData =
                m_dryStorage.data() + std::size_t(ch) * m_maxBlockSize;
            dsp::wetDryRamp({wetData, frames}, {dryData, frames}, startWet, targetWet);
        }
        m_wet = targetWet;
    }

    // CLAP's TAIL status delegates the countdown to the host. A finite tail is
    // measured from the last block that could have introduced new sound; an
    // infinite tail is never slept. Explicit SLEEP may take effect immediately
    // after this successful callback, except while non-zero input or automation
    // would force a stop/start transition on every block.
    bool enterSleep = false;
    if (disposition == PluginProcessDisposition::Sleep) {
        m_tailFramesRemaining = 0;
        enterSleep = block.canSleep;
    } else if (disposition == PluginProcessDisposition::Tail) {
        const std::uint64_t tail = m_instance->tailSamples();
        if (tail >= std::uint64_t(std::numeric_limits<std::int32_t>::max())) {
            m_tailFramesRemaining = tail;
        } else {
            const bool tailStimulus = block.tailStimulus;
            if (tailStimulus ||
                m_lastProcessDisposition != PluginProcessDisposition::Tail) {
                m_tailFramesRemaining = tail;
            }
            if (!tailStimulus) {
                if (m_tailFramesRemaining <= frames) {
                    m_tailFramesRemaining = 0;
                    enterSleep = true;
                } else {
                    m_tailFramesRemaining -= frames;
                }
            }
        }
    } else {
        m_tailFramesRemaining = 0;
    }
    m_lastProcessDisposition = disposition;
    rememberTransport(context);
    if (enterSleep) {
        m_instance->sleepProcessing();
        m_pluginSleeping = true;
    }
}

} // namespace daw::plugins
