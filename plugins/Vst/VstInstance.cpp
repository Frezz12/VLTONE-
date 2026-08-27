#include "Vst/VstInstance.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>

namespace daw::plugins {
namespace {

constexpr std::uint8_t kStateVersion = 1;
constexpr std::uint8_t kStateChunk = 1;
constexpr std::uint8_t kStateParameters = 2;
constexpr std::size_t kStateHeaderSize = 16;

void appendU16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(std::uint8_t(value));
    out.push_back(std::uint8_t(value >> 8));
}

void appendU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8)
        out.push_back(std::uint8_t(value >> shift));
}

std::uint16_t readU16(const std::uint8_t* data) noexcept {
    return std::uint16_t(data[0]) | (std::uint16_t(data[1]) << 8);
}

std::uint32_t readU32(const std::uint8_t* data) noexcept {
    return std::uint32_t(data[0]) | (std::uint32_t(data[1]) << 8) |
           (std::uint32_t(data[2]) << 16) | (std::uint32_t(data[3]) << 24);
}

std::string boundedText(const char* data, std::size_t capacity) {
    std::size_t size = 0;
    while (size < capacity && data[size] != '\0') ++size;
    while (size > 0 && std::isspace(static_cast<unsigned char>(data[size - 1])))
        --size;
    return std::string(data, size);
}

std::uint8_t midi7(double value) noexcept {
    return static_cast<std::uint8_t>(
        std::clamp(std::lround(value * 127.0), 0l, 127l));
}

bool toMidi(const PluginEvent& input, VstMidiEvent& output,
            std::uint32_t segmentStart) noexcept {
    std::memset(&output, 0, sizeof(output));
    output.type = kVstMidiType;
    output.byteSize = sizeof(VstMidiEvent);
    output.deltaFrames = static_cast<VstInt32>(input.frameOffset - segmentStart);
    const std::uint8_t channel = std::uint8_t(input.channel) & 0x0F;

    switch (input.kind) {
        case PluginEvent::Kind::NoteOn:
            output.midiData[0] = 0x90 | channel;
            output.midiData[1] = std::uint8_t(input.key) & 0x7F;
            output.midiData[2] = midi7(input.value);
            return true;
        case PluginEvent::Kind::NoteOff:
        case PluginEvent::Kind::NoteChoke:
            output.midiData[0] = 0x80 | channel;
            output.midiData[1] = std::uint8_t(input.key) & 0x7F;
            output.midiData[2] = midi7(input.value);
            return true;
        case PluginEvent::Kind::PolyPressure:
            output.midiData[0] = 0xA0 | channel;
            output.midiData[1] = std::uint8_t(input.key) & 0x7F;
            output.midiData[2] = midi7(input.value);
            return true;
        case PluginEvent::Kind::MidiController:
            if (input.paramIndex < 128) {
                output.midiData[0] = 0xB0 | channel;
                output.midiData[1] = std::uint8_t(input.paramIndex);
                output.midiData[2] = midi7(input.value);
                return true;
            }
            if (input.paramIndex == 128) {
                output.midiData[0] = 0xD0 | channel;
                output.midiData[1] = midi7(input.value);
                return true;
            }
            if (input.paramIndex == 129) {
                const std::uint16_t bend = static_cast<std::uint16_t>(
                    std::clamp(std::lround(input.value * 16383.0), 0l, 16383l));
                output.midiData[0] = 0xE0 | channel;
                output.midiData[1] = std::uint8_t(bend & 0x7F);
                output.midiData[2] = std::uint8_t((bend >> 7) & 0x7F);
                return true;
            }
            if (input.paramIndex == 130) {
                output.midiData[0] = 0xC0 | channel;
                output.midiData[1] = midi7(input.value);
                return true;
            }
            return false;
        default: return false;
    }
}

bool fromMidi(const VstMidiEvent& input, PluginEvent& output) noexcept {
    const std::uint8_t status = input.midiData[0];
    const std::uint8_t type = status & 0xF0;
    output.channel = status & 0x0F;
    output.noteId = -1;
    switch (type) {
        case 0x80:
            output.kind = PluginEvent::Kind::NoteOff;
            output.key = input.midiData[1] & 0x7F;
            output.value = double(input.midiData[2] & 0x7F) / 127.0;
            return true;
        case 0x90:
            output.kind = (input.midiData[2] & 0x7F) == 0
                              ? PluginEvent::Kind::NoteOff
                              : PluginEvent::Kind::NoteOn;
            output.key = input.midiData[1] & 0x7F;
            output.value = double(input.midiData[2] & 0x7F) / 127.0;
            return true;
        case 0xA0:
            output.kind = PluginEvent::Kind::PolyPressure;
            output.key = input.midiData[1] & 0x7F;
            output.value = double(input.midiData[2] & 0x7F) / 127.0;
            return true;
        case 0xB0:
            output.kind = PluginEvent::Kind::MidiController;
            output.paramIndex = input.midiData[1] & 0x7F;
            output.value = double(input.midiData[2] & 0x7F) / 127.0;
            return true;
        case 0xC0:
            output.kind = PluginEvent::Kind::MidiController;
            output.paramIndex = 130;
            output.value = double(input.midiData[1] & 0x7F) / 127.0;
            return true;
        case 0xD0:
            output.kind = PluginEvent::Kind::MidiController;
            output.paramIndex = 128;
            output.value = double(input.midiData[1] & 0x7F) / 127.0;
            return true;
        case 0xE0: {
            output.kind = PluginEvent::Kind::MidiController;
            output.paramIndex = 129;
            const std::uint16_t bend = std::uint16_t(input.midiData[1] & 0x7F) |
                (std::uint16_t(input.midiData[2] & 0x7F) << 7);
            output.value = double(bend) / 16383.0;
            return true;
        }
        default: return false;
    }
}

} // namespace

std::unique_ptr<VstInstance> VstInstance::create(
    std::shared_ptr<VstModule> module, const PluginDescriptor& descriptor,
    VstInt32 currentId) {
    auto instance = std::unique_ptr<VstInstance>(
        new VstInstance(std::move(module), descriptor, currentId));
    if (!instance->initialize()) return nullptr;
    return instance;
}

VstInstance::VstInstance(std::shared_ptr<VstModule> module,
                         PluginDescriptor descriptor, VstInt32 currentId)
    : m_module(std::move(module)), m_descriptor(std::move(descriptor)),
      m_host{this, &VstInstance::hostDispatch, currentId} {}

bool VstInstance::initialize() {
    m_effect = m_module ? m_module->create(m_host) : nullptr;
    if (!m_effect) return false;
    m_effect->dispatcher(m_effect, effOpen, 0, 0, nullptr, 0.0f);
    if (m_host.currentId != 0 && m_effect->uniqueID != 0 &&
        m_effect->uniqueID != m_host.currentId) {
        m_effect->dispatcher(m_effect, effClose, 0, 0, nullptr, 0.0f);
        m_effect = nullptr;
        return false;
    }
    m_descriptor.mainInputChannels = static_cast<std::uint16_t>(
        std::clamp<VstInt32>(m_effect->numInputs, 0,
                             std::numeric_limits<std::uint16_t>::max()));
    m_descriptor.mainOutputChannels = static_cast<std::uint16_t>(
        std::clamp<VstInt32>(m_effect->numOutputs, 0,
                             std::numeric_limits<std::uint16_t>::max()));
    m_descriptor.hasEditor = hasEditor();
    readParameters();
    refreshLatencyAndTail();
    return true;
}

VstInstance::~VstInstance() {
    closeEditor();
    if (m_processing) stopProcessing();
    if (m_active) deactivate();
    if (m_effect) {
        m_effect->dispatcher(m_effect, effClose, 0, 0, nullptr, 0.0f);
        m_effect = nullptr;
    }
}

void VstInstance::readParameters() {
    m_parameters.clear();
    if (!m_effect || m_effect->numParams <= 0) return;
    m_parameters.reserve(std::size_t(m_effect->numParams));
    for (VstInt32 index = 0; index < m_effect->numParams; ++index) {
        std::array<char, 64> name{};
        std::array<char, 64> unit{};
        m_effect->dispatcher(m_effect, effGetParamName, index, 0, name.data(), 0.0f);
        m_effect->dispatcher(m_effect, effGetParamLabel, index, 0, unit.data(), 0.0f);
        ParameterInfo info;
        info.index = std::uint32_t(index);
        info.id = std::to_string(index);
        info.name = boundedText(name.data(), name.size());
        if (info.name.empty()) info.name = "Parameter " + std::to_string(index + 1);
        info.unit = boundedText(unit.data(), unit.size());
        info.defaultValue = std::clamp<double>(m_effect->getParameter(m_effect, index),
                                               0.0, 1.0);
        info.isAutomatable =
            m_effect->dispatcher(m_effect, effCanBeAutomated, index, 0,
                                 nullptr, 0.0f) != 0;
        m_parameters.push_back(std::move(info));
    }
}

bool VstInstance::setBusLayout(const PluginBusLayout&,
                               PluginBusLayout& accepted) {
    accepted = busLayout();
    return true;
}

PluginBusLayout VstInstance::busLayout() const {
    PluginBusLayout layout;
    if (m_effect && m_effect->numInputs > 0)
        layout.inputs.push_back(static_cast<std::uint16_t>(m_effect->numInputs));
    if (m_effect && m_effect->numOutputs > 0)
        layout.outputs.push_back(static_cast<std::uint16_t>(m_effect->numOutputs));
    return layout;
}

bool VstInstance::activate(const PluginProcessInfo& info) {
    if (!m_effect || m_active || info.maxBlockSize == 0 || info.sampleRate <= 0.0)
        return false;
    m_sampleRate = info.sampleRate;
    m_maxBlockSize = info.maxBlockSize;
    m_offline = info.offline;
    m_zeroInput.assign(m_maxBlockSize, 0.0f);
    m_discardOutput.assign(std::size_t(std::max(m_effect->numOutputs, 0)) *
                               m_maxBlockSize,
                           0.0f);
    m_inputPointers.resize(std::size_t(std::max(m_effect->numInputs, 0)));
    m_outputPointers.resize(std::size_t(std::max(m_effect->numOutputs, 0)));
    m_effect->dispatcher(m_effect, effSetSampleRate, 0, 0, nullptr,
                         static_cast<float>(m_sampleRate));
    m_effect->dispatcher(m_effect, effSetBlockSize, 0, m_maxBlockSize,
                         nullptr, 0.0f);
    m_effect->dispatcher(m_effect, effMainsChanged, 0, 1, nullptr, 0.0f);
    m_active = true;
    refreshLatencyAndTail();
    return true;
}

void VstInstance::deactivate() {
    if (!m_effect || !m_active) return;
    if (m_processing) stopProcessing();
    m_effect->dispatcher(m_effect, effMainsChanged, 0, 0, nullptr, 0.0f);
    m_active = false;
    m_zeroInput.clear();
    m_discardOutput.clear();
    m_inputPointers.clear();
    m_outputPointers.clear();
    m_maxBlockSize = 0;
}

void VstInstance::startProcessing() {
    if (!m_effect || !m_active || m_processing) return;
    m_effect->dispatcher(m_effect, effStartProcess, 0, 0, nullptr, 0.0f);
    m_processing = true;
}

void VstInstance::stopProcessing() {
    if (!m_effect || !m_processing) return;
    m_effect->dispatcher(m_effect, effStopProcess, 0, 0, nullptr, 0.0f);
    m_processing = false;
}

std::int32_t VstInstance::parameterIndexForId(std::string_view id) const noexcept {
    std::uint64_t value = 0;
    if (id.empty()) return -1;
    for (char c : id) {
        if (c < '0' || c > '9') return -1;
        value = value * 10 + std::uint64_t(c - '0');
        if (value >= m_parameters.size()) return -1;
    }
    return static_cast<std::int32_t>(value);
}

double VstInstance::parameterValue(std::uint32_t index) const noexcept {
    if (!m_effect || index >= std::uint32_t(m_effect->numParams)) return 0.0;
    return std::clamp<double>(m_effect->getParameter(m_effect, VstInt32(index)),
                              0.0, 1.0);
}

std::string VstInstance::parameterText(std::uint32_t index,
                                       double plainValue) const {
    if (!m_effect || index >= std::uint32_t(m_effect->numParams)) return {};
    if (std::abs(parameterValue(index) - plainValue) < 1.0e-6) {
        std::array<char, 64> text{};
        m_effect->dispatcher(m_effect, effGetParamDisplay, VstInt32(index), 0,
                             text.data(), 0.0f);
        std::string display = boundedText(text.data(), text.size());
        if (!display.empty()) return display;
    }
    return std::to_string(std::clamp(plainValue, 0.0, 1.0));
}

void VstInstance::setParameterFromHost(std::uint32_t index, double plainValue) {
    if (!m_effect || index >= std::uint32_t(m_effect->numParams)) return;
    m_effect->setParameter(m_effect, VstInt32(index),
                           static_cast<float>(std::clamp(plainValue, 0.0, 1.0)));
}

bool VstInstance::saveState(std::vector<std::uint8_t>& out) const {
    if (!m_effect) return false;
    const VstIntPtr program =
        m_effect->dispatcher(m_effect, effGetProgram, 0, 0, nullptr, 0.0f);
    std::vector<std::uint8_t> payload;
    std::uint8_t mode = kStateParameters;

    if ((m_effect->flags & effFlagsProgramChunks) != 0) {
        void* data = nullptr;
        const VstIntPtr size =
            m_effect->dispatcher(m_effect, effGetChunk, 0, 0, &data, 0.0f);
        if (size < 0 || std::uint64_t(size) > std::numeric_limits<std::uint32_t>::max() ||
            (size > 0 && !data)) {
            return false;
        }
        mode = kStateChunk;
        if (size > 0) {
            const auto* bytes = static_cast<const std::uint8_t*>(data);
            payload.assign(bytes, bytes + std::size_t(size));
        }
    } else {
        payload.reserve(std::size_t(m_effect->numParams) * sizeof(float));
        for (VstInt32 i = 0; i < m_effect->numParams; ++i) {
            const std::uint32_t bits = std::bit_cast<std::uint32_t>(
                m_effect->getParameter(m_effect, i));
            appendU32(payload, bits);
        }
    }

    out.clear();
    out.reserve(kStateHeaderSize + payload.size());
    out.insert(out.end(), {'V', 'S', 'T', 'L'});
    appendU16(out, kStateVersion);
    out.push_back(mode);
    out.push_back(0);
    appendU32(out, static_cast<std::uint32_t>(static_cast<std::int32_t>(program)));
    appendU32(out, static_cast<std::uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    return true;
}

bool VstInstance::loadState(std::span<const std::uint8_t> state) {
    if (!m_effect || state.size() < kStateHeaderSize ||
        std::memcmp(state.data(), "VSTL", 4) != 0 ||
        readU16(state.data() + 4) != kStateVersion || state[7] != 0) {
        return false;
    }
    const std::uint8_t mode = state[6];
    const std::int32_t program = static_cast<std::int32_t>(readU32(state.data() + 8));
    const std::uint32_t payloadSize = readU32(state.data() + 12);
    if (state.size() != kStateHeaderSize + std::size_t(payloadSize) ||
        (program < -1 || program >= m_effect->numPrograms)) {
        return false;
    }
    const std::uint8_t* payload = state.data() + kStateHeaderSize;
    if (mode == kStateChunk) {
        if ((m_effect->flags & effFlagsProgramChunks) == 0) return false;
    } else if (mode == kStateParameters) {
        if (payloadSize != std::size_t(m_effect->numParams) * sizeof(float))
            return false;
        // Validate every value before touching the plugin.
        for (VstInt32 i = 0; i < m_effect->numParams; ++i) {
            const float value = std::bit_cast<float>(readU32(payload + i * 4));
            if (!std::isfinite(value) || value < 0.0f || value > 1.0f) return false;
        }
    } else {
        return false;
    }

    if (program >= 0)
        m_effect->dispatcher(m_effect, effSetProgram, 0, program, nullptr, 0.0f);
    if (mode == kStateChunk) {
        m_effect->dispatcher(m_effect, effSetChunk, 0, payloadSize,
                             const_cast<std::uint8_t*>(payload), 0.0f);
    } else {
        for (VstInt32 i = 0; i < m_effect->numParams; ++i) {
            const float value = std::bit_cast<float>(readU32(payload + i * 4));
            m_effect->setParameter(m_effect, i, value);
        }
    }
    refreshLatencyAndTail();
    return true;
}

bool VstInstance::hasEditor() const noexcept {
    return m_effect && (m_effect->flags & effFlagsHasEditor) != 0;
}

bool VstInstance::openEditor(void* parentHandle, PluginEditorHost* host) {
    if (!hasEditor() || !parentHandle || !host || m_editorOpen) return false;
    m_editorHost = host;
    m_effect->dispatcher(m_effect, effEditOpen, 0, 0, parentHandle, 0.0f);
    m_editorOpen = true;
    return true;
}

void VstInstance::closeEditor() {
    if (!m_effect || !m_editorOpen) return;
    m_effect->dispatcher(m_effect, effEditClose, 0, 0, nullptr, 0.0f);
    m_editorOpen = false;
    m_editorHost = nullptr;
}

bool VstInstance::editorSize(std::uint32_t& width,
                             std::uint32_t& height) const {
    if (!hasEditor()) return false;
    ERect* rect = nullptr;
    m_effect->dispatcher(m_effect, effEditGetRect, 0, 0, &rect, 0.0f);
    if (!rect || rect->right <= rect->left || rect->bottom <= rect->top) return false;
    width = std::uint32_t(rect->right - rect->left);
    height = std::uint32_t(rect->bottom - rect->top);
    return true;
}

bool VstInstance::setEditorSize(std::uint32_t& width, std::uint32_t& height) {
    std::uint32_t currentWidth = 0;
    std::uint32_t currentHeight = 0;
    if (!editorSize(currentWidth, currentHeight)) return false;
    width = currentWidth;
    height = currentHeight;
    return true;
}

void VstInstance::refreshLatencyAndTail() {
    if (!m_effect) return;
    m_latency.store(std::uint32_t(std::max(m_effect->initialDelay, 0)),
                    std::memory_order_relaxed);
    const VstIntPtr tail =
        m_effect->dispatcher(m_effect, effGetTailSize, 0, 0, nullptr, 0.0f);
    m_tail.store(tail == -1
                     ? std::numeric_limits<std::uint32_t>::max()
                     : (tail > 0 ? std::uint32_t(std::min<VstIntPtr>(
                                           tail, std::numeric_limits<std::uint32_t>::max()))
                                 : 0),
                 std::memory_order_relaxed);
}

void VstInstance::pumpMainThread() {
    if (!m_effect) return;
    if (m_editorOpen || m_idlePending.exchange(false, std::memory_order_acq_rel))
        m_effect->dispatcher(m_effect, effEditIdle, 0, 0, nullptr, 0.0f);

    const std::uint32_t width = m_requestedWidth.exchange(0, std::memory_order_acq_rel);
    const std::uint32_t height = m_requestedHeight.exchange(0, std::memory_order_acq_rel);
    if (width && height && m_editorHost)
        m_editorHost->onEditorResized(width, height);

    if (m_latencyPending.exchange(false, std::memory_order_acq_rel)) {
        const std::uint32_t before = latencySamples();
        refreshLatencyAndTail();
        if (before != latencySamples()) {
            if (PluginListener* listener = m_listener.load(std::memory_order_acquire))
                listener->onLatencyChanged();
        }
    }
    if (m_restartPending.exchange(false, std::memory_order_acq_rel)) {
        if (PluginListener* listener = m_listener.load(std::memory_order_acquire))
            listener->onRestartRequested();
    }
}

void VstInstance::prepareTime(const PluginProcessContext& context,
                              std::uint32_t segmentStart) noexcept {
    std::memset(&m_timeInfo, 0, sizeof(m_timeInfo));
    m_timeInfo.samplePos = double(context.sampleTime) + segmentStart;
    m_timeInfo.sampleRate = m_sampleRate;
    const double beatOffset = m_sampleRate > 0.0
        ? double(segmentStart) / m_sampleRate * context.transport.tempo / 60.0
        : 0.0;
    m_timeInfo.ppqPos = context.transport.ppqPosition + beatOffset;
    m_timeInfo.tempo = context.transport.tempo;
    const double barLength = context.transport.timeSigDenominator > 0
        ? double(context.transport.timeSigNumerator) * 4.0 /
              context.transport.timeSigDenominator
        : 4.0;
    m_timeInfo.barStartPos = barLength > 0.0
        ? std::floor(m_timeInfo.ppqPos / barLength) * barLength
        : context.transport.barStartPpq;
    m_timeInfo.cycleStartPos = context.transport.loopStartPpq;
    m_timeInfo.cycleEndPos = context.transport.loopEndPpq;
    m_timeInfo.timeSigNumerator = context.transport.timeSigNumerator;
    m_timeInfo.timeSigDenominator = context.transport.timeSigDenominator;
    m_timeInfo.flags = kVstPpqPosValid | kVstTempoValid | kVstBarsValid |
                       kVstTimeSigValid;
    if (context.playing) m_timeInfo.flags |= kVstTransportPlaying;
    if (context.transport.recording) m_timeInfo.flags |= kVstTransportRecording;
    if (context.transport.looping)
        m_timeInfo.flags |= kVstTransportCycleActive | kVstCyclePosValid;
}

void VstInstance::prepareAudio(const PluginProcessContext& context,
                               std::uint32_t start) noexcept {
    for (VstInt32 channel = 0; channel < m_effect->numInputs; ++channel) {
        m_inputPointers[std::size_t(channel)] =
            context.inputs && channel < context.inputChannels && context.inputs[channel]
                ? const_cast<float*>(context.inputs[channel] + start)
                : m_zeroInput.data() + start;
    }
    for (VstInt32 channel = 0; channel < m_effect->numOutputs; ++channel) {
        m_outputPointers[std::size_t(channel)] =
            context.outputs && channel < context.outputChannels && context.outputs[channel]
                ? context.outputs[channel] + start
                : m_discardOutput.data() + std::size_t(channel) * m_maxBlockSize + start;
    }
}

void VstInstance::sendMidi(std::span<const PluginEvent> events,
                           std::uint32_t start, std::uint32_t end) noexcept {
    m_events.numEvents = 0;
    for (const PluginEvent& event : events) {
        if (event.frameOffset < start || event.frameOffset >= end ||
            m_events.numEvents >= VstInt32(kMaxBlockEvents)) {
            continue;
        }
        VstMidiEvent& midi = m_midiEvents[std::size_t(m_events.numEvents)];
        if (!toMidi(event, midi, start)) continue;
        m_events.events[std::size_t(m_events.numEvents)] =
            reinterpret_cast<VstEvent*>(&midi);
        ++m_events.numEvents;
    }
    if (m_events.numEvents > 0)
        m_effect->dispatcher(m_effect, effProcessEvents, 0, 0,
                             reinterpret_cast<VstEvents*>(&m_events), 0.0f);
}

void VstInstance::processSegment(const PluginProcessContext& context,
                                 std::uint32_t start,
                                 std::uint32_t end) noexcept {
    if (end <= start) return;
    const VstInt32 frames = static_cast<VstInt32>(end - start);
    m_segmentStart = start;
    prepareTime(context, start);
    prepareAudio(context, start);
    sendMidi(context.inputEvents, start, end);

    if (m_effect->processReplacing) {
        m_effect->processReplacing(m_effect, m_inputPointers.data(),
                                   m_outputPointers.data(), frames);
    } else {
        for (float* output : m_outputPointers)
            std::fill_n(output, frames, 0.0f);
        m_effect->process(m_effect, m_inputPointers.data(),
                          m_outputPointers.data(), frames);
    }
}

PluginProcessDisposition VstInstance::process(
    const PluginProcessContext& context) noexcept {
    if (!m_effect || !m_processing || context.frames > m_maxBlockSize)
        return PluginProcessDisposition::Continue;
    m_currentProcess = &context;
    m_outputSink = context.outputEvents;

    std::uint32_t cursor = 0;
    std::size_t eventIndex = 0;
    while (eventIndex < context.inputEvents.size()) {
        const PluginEvent& event = context.inputEvents[eventIndex];
        if (event.kind != PluginEvent::Kind::ParamValue) {
            ++eventIndex;
            continue;
        }
        const std::uint32_t offset = std::min(event.frameOffset, context.frames);
        processSegment(context, cursor, offset);
        cursor = offset;
        while (eventIndex < context.inputEvents.size()) {
            const PluginEvent& parameter = context.inputEvents[eventIndex];
            if (parameter.frameOffset != event.frameOffset) break;
            if (parameter.kind == PluginEvent::Kind::ParamValue &&
                parameter.paramIndex < std::uint32_t(m_effect->numParams)) {
                m_effect->setParameter(
                    m_effect, VstInt32(parameter.paramIndex),
                    static_cast<float>(std::clamp(parameter.value, 0.0, 1.0)));
            }
            ++eventIndex;
        }
    }
    processSegment(context, cursor, context.frames);
    m_outputSink = nullptr;
    m_currentProcess = nullptr;
    return PluginProcessDisposition::Continue;
}

void VstInstance::reset() noexcept {
    // AEffect has no realtime-safe reset opcode. The mains/process transitions
    // bracket graph stops and are the format-defined way to clear DSP state.
}

void VstInstance::receiveEvents(const VstEvents* events) noexcept {
    if (!events || !m_outputSink || !m_currentProcess || events->numEvents <= 0)
        return;
    const VstInt32 count = std::min<VstInt32>(events->numEvents,
                                              VstInt32(kMaxBlockEvents));
    for (VstInt32 i = 0; i < count; ++i) {
        const VstEvent* event = events->events[i];
        if (!event || event->type != kVstMidiType) continue;
        const auto* midi = reinterpret_cast<const VstMidiEvent*>(event);
        PluginEvent output;
        if (!fromMidi(*midi, output)) continue;
        const std::int64_t position = std::int64_t(m_segmentStart) +
                                      std::max(midi->deltaFrames, 0);
        output.frameOffset = std::uint32_t(std::min<std::int64_t>(
            position, m_currentProcess->frames));
        m_outputSink->push(output);
    }
}

VstIntPtr VstInstance::hostDispatch(vst::HostContext& host, AEffect*,
                                    VstInt32 opcode, VstInt32 index,
                                    VstIntPtr value, void* ptr,
                                    float opt) noexcept {
    auto* self = static_cast<VstInstance*>(host.owner);
    if (opcode == audioMasterCurrentId) return host.currentId;
    if (!self) return 0;
    switch (opcode) {
        case audioMasterAutomate:
            if (PluginListener* listener = self->m_listener.load(std::memory_order_acquire))
                listener->onParameterChanged(std::uint32_t(index), opt);
            return 0;
        case audioMasterBeginEdit:
        case audioMasterEndEdit:
            if (PluginListener* listener = self->m_listener.load(std::memory_order_acquire))
                listener->onParameterGesture(std::uint32_t(index),
                                             opcode == audioMasterBeginEdit);
            return 0;
        case audioMasterVersion: return kVstVersion;
        case audioMasterWantMidi: return 1;
        case audioMasterGetTime: return reinterpret_cast<VstIntPtr>(&self->m_timeInfo);
        case audioMasterProcessEvents:
            self->receiveEvents(static_cast<const VstEvents*>(ptr));
            return 1;
        case audioMasterTempoAt:
            return self->m_currentProcess
                ? VstIntPtr(std::lround(self->m_currentProcess->transport.tempo * 10000.0))
                : 1200000;
        case audioMasterNeedIdle:
        case kAudioMasterIdle:
            self->m_idlePending.store(true, std::memory_order_release);
            PluginMainThreadWork::request();
            return 1;
        case audioMasterSizeWindow:
            if (index > 0 && value > 0) {
                self->m_requestedWidth.store(std::uint32_t(index),
                                             std::memory_order_release);
                self->m_requestedHeight.store(std::uint32_t(value),
                                              std::memory_order_release);
                PluginMainThreadWork::request();
                return 1;
            }
            return 0;
        case audioMasterGetSampleRate: return VstIntPtr(self->m_sampleRate);
        case audioMasterGetBlockSize: return self->m_maxBlockSize;
        case audioMasterGetCurrentProcessLevel:
            return self->m_offline ? kVstProcessLevelOffline : kVstProcessLevelRealtime;
        case audioMasterGetVendorString:
            if (ptr) std::memcpy(ptr, "VLT Studio", sizeof("VLT Studio"));
            return ptr ? 1 : 0;
        case audioMasterGetProductString:
            if (ptr) std::memcpy(ptr, "VLT Studio Pro", sizeof("VLT Studio Pro"));
            return ptr ? 1 : 0;
        case audioMasterGetVendorVersion: return 1;
        case audioMasterCanDo:
            if (!ptr) return 0;
            if (std::strcmp(static_cast<const char*>(ptr), "receiveVstEvents") == 0 ||
                std::strcmp(static_cast<const char*>(ptr), "receiveVstMidiEvent") == 0 ||
                std::strcmp(static_cast<const char*>(ptr), "sendVstEvents") == 0 ||
                std::strcmp(static_cast<const char*>(ptr), "sendVstMidiEvent") == 0 ||
                std::strcmp(static_cast<const char*>(ptr), "sendVstTimeInfo") == 0 ||
                std::strcmp(static_cast<const char*>(ptr), "sizeWindow") == 0 ||
                std::strcmp(static_cast<const char*>(ptr), "startStopProcess") == 0) {
                return 1;
            }
            return 0;
        case audioMasterUpdateDisplay:
            self->m_restartPending.store(true, std::memory_order_release);
            PluginMainThreadWork::request();
            return 1;
        case kAudioMasterIoChanged:
            self->m_latencyPending.store(true, std::memory_order_release);
            self->m_restartPending.store(true, std::memory_order_release);
            PluginMainThreadWork::request();
            return 1;
        default: return 0;
    }
}

} // namespace daw::plugins
