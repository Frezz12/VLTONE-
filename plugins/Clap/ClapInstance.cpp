#include "Clap/ClapInstance.hpp"
#include "Common/RealtimeSort.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace daw::plugins {
namespace {

/// CLAP carries musical positions as 31-bit fixed point, not doubles.
constexpr clap_beattime toBeatTime(double beats) noexcept {
    return clap_beattime(beats * double(CLAP_BEATTIME_FACTOR));
}

/// A `clap_istream` over a byte span, for handing saved state back.
struct SpanReader {
    std::span<const std::uint8_t> data;
    std::size_t position = 0;

    static std::int64_t read(const clap_istream_t* stream, void* buffer,
                             std::uint64_t size) {
        auto* self = static_cast<SpanReader*>(stream->ctx);
        const std::size_t remaining = self->data.size() - self->position;
        const std::size_t count = std::min<std::size_t>(remaining, size);
        std::memcpy(buffer, self->data.data() + self->position, count);
        self->position += count;
        return std::int64_t(count);
    }
};

/// A `clap_ostream` that appends to a vector.
struct VectorWriter {
    std::vector<std::uint8_t>* out = nullptr;

    static std::int64_t write(const clap_ostream_t* stream, const void* buffer,
                              std::uint64_t size) {
        auto* self = static_cast<VectorWriter*>(stream->ctx);
        const auto* bytes = static_cast<const std::uint8_t*>(buffer);
        self->out->insert(self->out->end(), bytes, bytes + size);
        return std::int64_t(size);
    }
};

/// Collects what the plugin emits during a block and forwards it to the host's
/// sink. Sized for nothing: `try_push` may refuse, and CLAP allows that.
struct OutputEvents {
    EventSink* sink = nullptr;
    const std::vector<clap_id>* parameterIds = nullptr;

    static bool validSize(const clap_event_header_t* event,
                          std::size_t wanted) noexcept {
        return event && event->size >= wanted;
    }

    static bool convertMidi(const clap_event_midi_t& midi,
                            PluginEvent& out) noexcept {
        const std::uint8_t status = midi.data[0];
        const std::uint8_t type = status & 0xF0;
        out.channel = std::int16_t(status & 0x0F);
        out.noteId = -1;

        switch (type) {
            case 0x80:
                out.kind = PluginEvent::Kind::NoteOff;
                out.key = midi.data[1] & 0x7F;
                out.value = double(midi.data[2] & 0x7F) / 127.0;
                return true;
            case 0x90:
                out.kind = (midi.data[2] & 0x7F) == 0
                               ? PluginEvent::Kind::NoteOff
                               : PluginEvent::Kind::NoteOn;
                out.key = midi.data[1] & 0x7F;
                out.value = double(midi.data[2] & 0x7F) / 127.0;
                return true;
            case 0xA0:
                out.kind = PluginEvent::Kind::PolyPressure;
                out.key = midi.data[1] & 0x7F;
                out.value = double(midi.data[2] & 0x7F) / 127.0;
                return true;
            case 0xB0:
                out.kind = PluginEvent::Kind::MidiController;
                out.paramIndex = midi.data[1] & 0x7F;
                out.value = double(midi.data[2] & 0x7F) / 127.0;
                return true;
            case 0xC0:
                out.kind = PluginEvent::Kind::MidiController;
                out.paramIndex = 130; // VST3-compatible program-change id.
                out.value = double(midi.data[1] & 0x7F) / 127.0;
                return true;
            case 0xD0:
                out.kind = PluginEvent::Kind::MidiController;
                out.paramIndex = 128; // VST3-compatible channel-pressure id.
                out.value = double(midi.data[1] & 0x7F) / 127.0;
                return true;
            case 0xE0: {
                out.kind = PluginEvent::Kind::MidiController;
                out.paramIndex = 129; // VST3-compatible pitch-bend id.
                const std::uint32_t bend = std::uint32_t(midi.data[1] & 0x7F) |
                                           (std::uint32_t(midi.data[2] & 0x7F) << 7);
                out.value = double(bend) / 16383.0;
                return true;
            }
            default:
                return false;
        }
    }

    static bool tryPush(const clap_output_events_t* list,
                        const clap_event_header_t* event) {
        auto* self = static_cast<OutputEvents*>(list->ctx);
        if (!self || !self->sink || !event ||
            event->space_id != CLAP_CORE_EVENT_SPACE_ID) {
            return false;
        }

        PluginEvent out;
        out.frameOffset = event->time;
        switch (event->type) {
            case CLAP_EVENT_PARAM_VALUE:
            case CLAP_EVENT_PARAM_GESTURE_BEGIN:
            case CLAP_EVENT_PARAM_GESTURE_END: {
                // Gesture events carry only a param id; value events carry both.
                clap_id id = 0;
                if (event->type == CLAP_EVENT_PARAM_VALUE) {
                    if (!validSize(event, sizeof(clap_event_param_value_t))) return false;
                    const auto* value =
                        reinterpret_cast<const clap_event_param_value_t*>(event);
                    id = value->param_id;
                    out.value = value->value;
                    out.kind = PluginEvent::Kind::ParamValue;
                } else {
                    if (!validSize(event, sizeof(clap_event_param_gesture_t))) return false;
                    const auto* gesture =
                        reinterpret_cast<const clap_event_param_gesture_t*>(event);
                    id = gesture->param_id;
                    out.kind = event->type == CLAP_EVENT_PARAM_GESTURE_BEGIN
                                   ? PluginEvent::Kind::ParamGestureBegin
                                   : PluginEvent::Kind::ParamGestureEnd;
                }
                // Map the opaque id back to our index; drop what we do not know.
                if (!self->parameterIds) return false;
                const auto found = std::find(self->parameterIds->begin(),
                                             self->parameterIds->end(), id);
                if (found == self->parameterIds->end()) return false;
                out.paramIndex =
                    std::uint32_t(found - self->parameterIds->begin());
                break;
            }
            case CLAP_EVENT_NOTE_ON:
            case CLAP_EVENT_NOTE_OFF:
            case CLAP_EVENT_NOTE_CHOKE: {
                if (!validSize(event, sizeof(clap_event_note_t))) return false;
                const auto* note = reinterpret_cast<const clap_event_note_t*>(event);
                out.kind = event->type == CLAP_EVENT_NOTE_ON
                               ? PluginEvent::Kind::NoteOn
                               : (event->type == CLAP_EVENT_NOTE_OFF
                                      ? PluginEvent::Kind::NoteOff
                                      : PluginEvent::Kind::NoteChoke);
                out.channel = note->channel;
                out.key = note->key;
                out.noteId = note->note_id;
                out.value = note->velocity;
                break;
            }
            case CLAP_EVENT_NOTE_EXPRESSION: {
                if (!validSize(event, sizeof(clap_event_note_expression_t))) return false;
                const auto* expression =
                    reinterpret_cast<const clap_event_note_expression_t*>(event);
                if (expression->expression_id != CLAP_NOTE_EXPRESSION_PRESSURE) {
                    return false;
                }
                out.kind = PluginEvent::Kind::PolyPressure;
                out.channel = expression->channel;
                out.key = expression->key;
                out.noteId = expression->note_id;
                out.value = expression->value;
                break;
            }
            case CLAP_EVENT_MIDI: {
                if (!validSize(event, sizeof(clap_event_midi_t))) return false;
                if (!convertMidi(*reinterpret_cast<const clap_event_midi_t*>(event), out)) {
                    return false;
                }
                break;
            }
            default:
                return false;
        }
        self->sink->push(out);
        return true;
    }
};

} // namespace

ClapInstance::ClapInstance(std::shared_ptr<ClapModule> module,
                           const clap_plugin_t* plugin,
                           PluginDescriptor descriptor)
    : m_module(std::move(module)), m_plugin(plugin),
      m_descriptor(std::move(descriptor)) {
    m_params = static_cast<const clap_plugin_params_t*>(
        m_plugin->get_extension(m_plugin, CLAP_EXT_PARAMS));
    m_state = static_cast<const clap_plugin_state_t*>(
        m_plugin->get_extension(m_plugin, CLAP_EXT_STATE));
    m_latencyExt = static_cast<const clap_plugin_latency_t*>(
        m_plugin->get_extension(m_plugin, CLAP_EXT_LATENCY));
    m_tailExt = static_cast<const clap_plugin_tail_t*>(
        m_plugin->get_extension(m_plugin, CLAP_EXT_TAIL));
    m_audioPorts = static_cast<const clap_plugin_audio_ports_t*>(
        m_plugin->get_extension(m_plugin, CLAP_EXT_AUDIO_PORTS));
    m_gui = static_cast<const clap_plugin_gui_t*>(
        m_plugin->get_extension(m_plugin, CLAP_EXT_GUI));
    m_render = static_cast<const clap_plugin_render_t*>(
        m_plugin->get_extension(m_plugin, CLAP_EXT_RENDER));

    readParameters();
    readDescriptorPorts();
    refreshLatency();
    refreshTail();
}

ClapInstance::~ClapInstance() {
    // Before deactivate: a plugin's editor holds references into its DSP state,
    // and tearing that state down underneath an open view crashes.
    closeEditor();
    if (m_processing) stopProcessing();
    if (m_active) deactivate();
    if (m_plugin && m_plugin->destroy) m_plugin->destroy(m_plugin);
}

void ClapInstance::fillHost(clap_host_t& host, void* hostData) noexcept {
    host.clap_version = CLAP_VERSION;
    host.host_data = hostData;
    host.name = VLT_STUDIO_PRO_NAME;
    host.vendor = "VLT Studio";
    host.url = "";
    host.version = VLT_STUDIO_PRO_VERSION;
    host.get_extension = &ClapInstance::hostGetExtension;
    host.request_restart = &ClapInstance::hostRequestRestart;
    host.request_process = &ClapInstance::hostRequestProcess;
    host.request_callback = &ClapInstance::hostRequestCallback;
}

const void* ClapInstance::hostGetExtension(const clap_host_t*, const char* id) noexcept {
    // Static: the plugin keeps the pointer we hand back for as long as it
    // lives, so these must outlive every instance.
    static const clap_host_gui_t kHostGui = {
        &ClapInstance::hostGuiResizeHintsChanged, &ClapInstance::hostGuiRequestResize,
        &ClapInstance::hostGuiRequestShow, &ClapInstance::hostGuiRequestHide,
        &ClapInstance::hostGuiClosed};
    static const clap_host_latency_t kHostLatency = {&ClapInstance::hostLatencyChanged};
    static const clap_host_tail_t kHostTail = {&ClapInstance::hostTailChanged};
    static const clap_host_params_t kHostParams = {
        &ClapInstance::hostParamsRescan, &ClapInstance::hostParamsClear,
        &ClapInstance::hostParamsRequestFlush};

    if (std::strcmp(id, CLAP_EXT_GUI) == 0) return &kHostGui;
    if (std::strcmp(id, CLAP_EXT_LATENCY) == 0) return &kHostLatency;
    if (std::strcmp(id, CLAP_EXT_TAIL) == 0) return &kHostTail;
    if (std::strcmp(id, CLAP_EXT_PARAMS) == 0) return &kHostParams;
    // Anything else: null is always a legal answer, and every plugin has to
    // cope with a host that does not offer an extension.
    return nullptr;
}

void ClapInstance::hostRequestRestart(const clap_host_t* host) noexcept {
    auto* self = static_cast<ClapInstance*>(host->host_data);
    if (!self) return;
    self->m_processRequested.store(true, std::memory_order_release);
    if (auto* listener = self->m_listener.load(std::memory_order_acquire)) {
        listener->onRestartRequested();
    }
}

void ClapInstance::hostRequestProcess(const clap_host_t* host) noexcept {
    auto* self = static_cast<ClapInstance*>(host->host_data);
    if (self) self->m_processRequested.store(true, std::memory_order_release);
}

void ClapInstance::hostRequestCallback(const clap_host_t* host) noexcept {
    auto* self = static_cast<ClapInstance*>(host->host_data);
    if (self &&
        !self->m_callbackRequested.exchange(true, std::memory_order_acq_rel)) {
        PluginMainThreadWork::request();
    }
}

void ClapInstance::hostParamsRescan(const clap_host_t* host,
                                    clap_param_rescan_flags flags) noexcept {
    auto* self = static_cast<ClapInstance*>(host->host_data);
    if (!self) return;
    self->m_paramRescanFlags.fetch_or(flags, std::memory_order_release);
    // A preset may apply non-breaking DSP changes immediately and report only
    // RESCAN_VALUES. If the processor had returned SLEEP, its new state still
    // needs one process opportunity even though no host automation event exists.
    if (flags & (CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_ALL)) {
        self->m_processRequested.store(true, std::memory_order_release);
    }

    // Preset loads use RESCAN_VALUES instead of emitting one event per knob.
    // CLAP makes the plugin responsible for applying the DSP values itself;
    // the host still has to re-read them so project state and controls follow.
    if (flags & CLAP_PARAM_RESCAN_VALUES) {
        if (auto* listener = self->m_listener.load(std::memory_order_acquire)) {
            for (std::uint32_t index = 0; index < self->m_parameterIds.size(); ++index) {
                listener->onParameterChanged(index, self->parameterValue(index));
            }
        }
    }
    if (flags & (CLAP_PARAM_RESCAN_INFO | CLAP_PARAM_RESCAN_ALL)) {
        if (auto* listener = self->m_listener.load(std::memory_order_acquire)) {
            listener->onRestartRequested();
        }
    }
}

void ClapInstance::hostParamsClear(const clap_host_t*, clap_id,
                                   clap_param_clear_flags) noexcept {
    // References/cookies are not retained in host events, and automation is
    // stored by stable string id, so there is no volatile reference to clear.
}

void ClapInstance::hostParamsRequestFlush(const clap_host_t* host) noexcept {
    // A sleeping node has no scheduled process call, so request_flush is also
    // a wake request. The audio thread consumes the atomic and delivers the
    // plugin's next process/flush opportunity without blocking this caller.
    auto* self = static_cast<ClapInstance*>(host->host_data);
    if (self) self->m_processRequested.store(true, std::memory_order_release);
}

void ClapInstance::pumpMainThread() {
    if (!m_callbackRequested.exchange(false, std::memory_order_acq_rel)) return;
    if (m_plugin && m_plugin->on_main_thread) m_plugin->on_main_thread(m_plugin);
}

void ClapInstance::readParameters() {
    m_parameters.clear();
    m_parameterIds.clear();
    if (!m_params || !m_params->count || !m_params->get_info) return;

    const std::uint32_t count = m_params->count(m_plugin);
    m_parameters.reserve(count);
    m_parameterIds.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        clap_param_info_t raw{};
        if (!m_params->get_info(m_plugin, i, &raw)) continue;

        ParameterInfo info;
        info.index = std::uint32_t(m_parameters.size());
        // CLAP has no string parameter id, so the numeric one is the stable
        // identity a project file has to store.
        info.id = std::to_string(raw.id);
        info.name = raw.name;
        info.minValue = raw.min_value;
        info.maxValue = raw.max_value;
        info.defaultValue = raw.default_value;
        info.isAutomatable = (raw.flags & CLAP_PARAM_IS_AUTOMATABLE) != 0;
        info.isStepped = (raw.flags & CLAP_PARAM_IS_STEPPED) != 0;
        info.isBypass = (raw.flags & CLAP_PARAM_IS_BYPASS) != 0;
        m_parameters.push_back(std::move(info));
        m_parameterIds.push_back(raw.id);
    }
}

void ClapInstance::readDescriptorPorts() {
    if (!m_audioPorts || !m_audioPorts->count || !m_audioPorts->get) return;
    clap_audio_port_info_t info{};
    if (m_audioPorts->count(m_plugin, true) > 0 &&
        m_audioPorts->get(m_plugin, 0, true, &info)) {
        m_descriptor.mainInputChannels = std::uint16_t(info.channel_count);
    } else {
        m_descriptor.mainInputChannels = 0;
    }
    if (m_audioPorts->count(m_plugin, false) > 0 &&
        m_audioPorts->get(m_plugin, 0, false, &info)) {
        m_descriptor.mainOutputChannels = std::uint16_t(info.channel_count);
    }
}

void ClapInstance::allocateBuses(std::uint32_t maxBlockSize) {
    m_maxBlockSize = maxBlockSize;
    m_inputBuses.clear();
    m_outputBuses.clear();
    m_inputBufferDescs.clear();
    m_outputBufferDescs.clear();
    if (!m_audioPorts || !m_audioPorts->count || !m_audioPorts->get) return;

    auto build = [&](bool isInput, std::vector<AudioBus>& buses,
                     std::vector<clap_audio_buffer_t>& descs) {
        const std::uint32_t count = m_audioPorts->count(m_plugin, isInput);
        buses.resize(count);
        descs.resize(count);
        for (std::uint32_t bus = 0; bus < count; ++bus) {
            clap_audio_port_info_t info{};
            // A port that refuses to describe itself still has to be handed
            // something valid, so fall back to stereo rather than to nothing.
            if (!m_audioPorts->get(m_plugin, bus, isInput, &info)) info.channel_count = 2;
            AudioBus& target = buses[bus];
            target.channels = info.channel_count;
            target.storage.assign(std::size_t(info.channel_count) * maxBlockSize, 0.0f);
            target.pointers.resize(info.channel_count);
            for (std::uint32_t channel = 0; channel < info.channel_count; ++channel) {
                target.pointers[channel] = target.storage.data() + std::size_t(channel) * maxBlockSize;
            }
        }
        // Only after every bus is built: `resize` above may reallocate, and a
        // pointer taken into an earlier bus's vector would dangle.
        for (std::uint32_t bus = 0; bus < count; ++bus) {
            descs[bus] = {};
            descs[bus].data32 = buses[bus].pointers.data();
            descs[bus].channel_count = buses[bus].channels;
        }
    };
    build(true, m_inputBuses, m_inputBufferDescs);
    build(false, m_outputBuses, m_outputBufferDescs);
}

PluginBusLayout ClapInstance::busLayout() const {
    PluginBusLayout layout;
    if (m_audioPorts && m_audioPorts->count && m_audioPorts->get) {
        const auto append = [&](bool input, std::vector<std::uint16_t>& buses) {
            const std::uint32_t count = m_audioPorts->count(m_plugin, input);
            for (std::uint32_t i = 0; i < count; ++i) {
                clap_audio_port_info_t info{};
                if (m_audioPorts->get(m_plugin, i, input, &info)) {
                    buses.push_back(std::uint16_t(info.channel_count));
                }
            }
        };
        append(true, layout.inputs);
        append(false, layout.outputs);
    } else {
        if (m_descriptor.mainInputChannels > 0) {
            layout.inputs.push_back(m_descriptor.mainInputChannels);
        }
        layout.outputs.push_back(m_descriptor.mainOutputChannels);
    }
    return layout;
}

bool ClapInstance::setBusLayout(const PluginBusLayout&, PluginBusLayout& accepted) {
    // CLAP plugins declare their ports; negotiating them needs the optional
    // `audio-ports-config` extension. Until that is wired, report what the
    // plugin already has and let PluginNode adapt around it.
    accepted = busLayout();
    return true;
}

bool ClapInstance::activate(const PluginProcessInfo& info) {
    if (m_active) deactivate();
    if (!m_plugin || !m_plugin->activate) return false;
    const clap_param_rescan_flags rescan =
        m_paramRescanFlags.exchange(0, std::memory_order_acq_rel);
    if (rescan & (CLAP_PARAM_RESCAN_INFO | CLAP_PARAM_RESCAN_ALL)) {
        readParameters();
    }
    if (m_render && m_render->set) {
        (void)m_render->set(m_plugin, info.offline ? CLAP_RENDER_OFFLINE
                                                   : CLAP_RENDER_REALTIME);
    }
    if (!m_plugin->activate(m_plugin, info.sampleRate, 1, info.maxBlockSize)) {
        return false;
    }
    m_active = true;

    // Match PluginNode's fixed block-event budget. Keeping all event layouts
    // in one union means a dense chord/CC block can use the whole budget
    // without four independent worst-case allocations per plugin instance.
    constexpr std::size_t kBlockEventCapacity = 2048;
    const std::size_t capacity =
        std::max(kBlockEventCapacity, m_parameters.size() + 256);
    m_inputEventScratch.resize(capacity);
    m_eventOrder.resize(capacity);
    allocateBuses(info.maxBlockSize);

    refreshLatency();
    refreshTail();
    return true;
}

void ClapInstance::deactivate() {
    if (!m_active) return;
    if (m_processing) stopProcessing();
    if (m_plugin && m_plugin->deactivate) m_plugin->deactivate(m_plugin);
    m_active = false;
}

void ClapInstance::startProcessing() {
    if (!m_active || m_processing) return;
    if (m_plugin && m_plugin->start_processing) {
        m_processing = m_plugin->start_processing(m_plugin);
    }
}

void ClapInstance::stopProcessing() {
    if (!m_processing) return;
    if (m_plugin && m_plugin->stop_processing) m_plugin->stop_processing(m_plugin);
    m_processing = false;
}

void ClapInstance::reset() noexcept {
    if (m_plugin && m_plugin->reset) m_plugin->reset(m_plugin);
}

void ClapInstance::refreshLatency() {
    if (m_latencyExt && m_latencyExt->get) {
        m_latency.store(m_latencyExt->get(m_plugin), std::memory_order_relaxed);
    }
}

void ClapInstance::refreshTail() noexcept {
    if (m_tailExt && m_tailExt->get) {
        m_tail.store(m_tailExt->get(m_plugin), std::memory_order_relaxed);
    }
}

std::int32_t ClapInstance::parameterIndexForId(std::string_view id) const noexcept {
    for (std::size_t i = 0; i < m_parameters.size(); ++i) {
        if (m_parameters[i].id == id) return std::int32_t(i);
    }
    return -1;
}

double ClapInstance::parameterValue(std::uint32_t index) const noexcept {
    if (index >= m_parameterIds.size() || !m_params || !m_params->get_value) return 0.0;
    double value = 0.0;
    if (!m_params->get_value(m_plugin, m_parameterIds[index], &value)) return 0.0;
    return value;
}

std::string ClapInstance::parameterText(std::uint32_t index, double plainValue) const {
    if (index >= m_parameterIds.size() || !m_params || !m_params->value_to_text) {
        return {};
    }
    char buffer[CLAP_NAME_SIZE] = {};
    if (!m_params->value_to_text(m_plugin, m_parameterIds[index], plainValue,
                                 buffer, sizeof(buffer))) {
        return {};
    }
    return std::string(buffer);
}

bool ClapInstance::saveState(std::vector<std::uint8_t>& out) const {
    out.clear();
    if (!m_state || !m_state->save) return false;
    VectorWriter writer{&out};
    clap_ostream_t stream{};
    stream.ctx = &writer;
    stream.write = &VectorWriter::write;
    return m_state->save(m_plugin, &stream);
}

bool ClapInstance::loadState(std::span<const std::uint8_t> state) {
    if (!m_state || !m_state->load) return false;
    SpanReader reader{state, 0};
    clap_istream_t stream{};
    stream.ctx = &reader;
    stream.read = &SpanReader::read;
    if (!m_state->load(m_plugin, &stream)) return false;
    m_processRequested.store(true, std::memory_order_release);
    // A preset can move the latency, and the graph has to be told.
    const std::uint32_t before = m_latency.load(std::memory_order_relaxed);
    refreshLatency();
    if (m_latency.load(std::memory_order_relaxed) != before) {
        if (auto* listener = m_listener.load(std::memory_order_acquire)) {
            listener->onLatencyChanged();
        }
    }
    return true;
}

// ── Editor ────────────────────────────────────────────────────────────────

const char* ClapInstance::editorApi() noexcept {
#if defined(__APPLE__)
    return CLAP_WINDOW_API_COCOA;
#elif defined(_WIN32)
    return CLAP_WINDOW_API_WIN32;
#else
    return CLAP_WINDOW_API_X11;
#endif
}

bool ClapInstance::hasEditor() const noexcept {
    if (!m_gui || !m_gui->is_api_supported) return false;
    return m_gui->is_api_supported(m_plugin, editorApi(), false);
}

bool ClapInstance::openEditor(void* parentHandle, PluginEditorHost* host) {
    if (m_editorOpen) return true;
    if (!parentHandle || !hasEditor() || !m_gui->create || !m_gui->set_parent) return false;

    m_editorHost = host;
    if (!m_gui->create(m_plugin, editorApi(), false)) {
        m_editorHost = nullptr;
        return false;
    }

    // No `set_scale` on macOS: Cocoa views are already in logical points, and
    // telling a plugin to scale again gives a double-scaled editor on Retina.
#if !defined(__APPLE__)
    if (m_gui->set_scale) m_gui->set_scale(m_plugin, 1.0);
#endif

    clap_window_t window{};
    window.api = editorApi();
#if defined(__APPLE__)
    window.cocoa = parentHandle;
#elif defined(_WIN32)
    window.win32 = parentHandle;
#else
    window.x11 = reinterpret_cast<unsigned long>(parentHandle);
#endif
    if (!m_gui->set_parent(m_plugin, &window)) {
        if (m_gui->destroy) m_gui->destroy(m_plugin);
        m_editorHost = nullptr;
        return false;
    }

    if (m_gui->show) m_gui->show(m_plugin);
    m_editorOpen = true;
    return true;
}

void ClapInstance::closeEditor() {
    if (!m_editorOpen) return;
    // Cleared first: `destroy` can call back into the host, and a callback
    // arriving while the window is being torn down must not be forwarded.
    m_editorHost = nullptr;
    m_editorOpen = false;
    if (m_gui) {
        if (m_gui->hide) m_gui->hide(m_plugin);
        if (m_gui->destroy) m_gui->destroy(m_plugin);
    }
}

bool ClapInstance::editorSize(std::uint32_t& width, std::uint32_t& height) const {
    if (!m_gui || !m_gui->get_size) return false;
    return m_gui->get_size(m_plugin, &width, &height);
}

bool ClapInstance::editorCanResize() const {
    return m_gui && m_gui->can_resize && m_gui->can_resize(m_plugin);
}

bool ClapInstance::setEditorSize(std::uint32_t& width, std::uint32_t& height) {
    if (!m_gui || !m_gui->set_size) return false;
    // `adjust_size` first: the plugin snaps the request to something it can
    // actually draw, and setting an unsnapped size is what produces editors
    // with a strip of garbage down one edge.
    if (m_gui->adjust_size) m_gui->adjust_size(m_plugin, &width, &height);
    return m_gui->set_size(m_plugin, width, height);
}

void ClapInstance::hostGuiResizeHintsChanged(const clap_host_t*) noexcept {
    // Only affects aspect-ratio hinting during a live drag, which the window
    // does not implement; nothing to do.
}

bool ClapInstance::hostGuiRequestResize(const clap_host_t* host, std::uint32_t width,
                                        std::uint32_t height) noexcept {
    auto* self = static_cast<ClapInstance*>(host->host_data);
    if (!self || !self->m_editorHost) return false;
    self->m_editorHost->onEditorResized(width, height);
    return true;
}

bool ClapInstance::hostGuiRequestShow(const clap_host_t*) noexcept {
    // The window is shown by the host when it opens the editor; a plugin
    // asking to be shown when it already is has nothing to be granted.
    return false;
}

bool ClapInstance::hostGuiRequestHide(const clap_host_t*) noexcept { return false; }

void ClapInstance::hostGuiClosed(const clap_host_t* host, bool) noexcept {
    auto* self = static_cast<ClapInstance*>(host->host_data);
    if (!self || !self->m_editorHost) return;
    // The host tears the window down from here, which is why `closeEditor`
    // must not be called inside this callback — see PluginEditorHost.
    self->m_editorHost->onEditorClosed();
}

void ClapInstance::hostLatencyChanged(const clap_host_t* host) noexcept {
    auto* self = static_cast<ClapInstance*>(host->host_data);
    if (!self) return;
    self->refreshLatency();
    if (auto* listener = self->m_listener.load(std::memory_order_acquire)) {
        listener->onLatencyChanged();
    }
}

void ClapInstance::hostTailChanged(const clap_host_t* host) noexcept {
    auto* self = static_cast<ClapInstance*>(host->host_data);
    if (!self) return;
    self->refreshTail();
    self->m_processRequested.store(true, std::memory_order_release);
}

PluginProcessDisposition ClapInstance::process(
    const PluginProcessContext& context) noexcept {
    // Nothing will be written, so say so rather than leaving the caller's
    // buffer holding whatever the arena recycled into it.
    auto silence = [&] {
        for (std::uint16_t channel = 0; channel < context.outputChannels; ++channel) {
            std::fill_n(context.outputs[channel], context.frames, 0.0f);
        }
    };
    if (!m_processing || !m_plugin || !m_plugin->process) {
        // Returning here without writing left the caller holding the arena's
        // previous contents, which the graph then plays again every block.
        silence();
        return PluginProcessDisposition::Continue;
    }
    // A block longer than what the plugin was activated for would overrun the
    // per-bus buffers sized in `activate`.
    if (context.frames > m_maxBlockSize || m_outputBuses.empty()) {
        silence();
        return PluginProcessDisposition::Continue;
    }

    // ── Events in, converted to CLAP's layout ──
    std::uint32_t orderCount = 0;
    for (const PluginEvent& event : context.inputEvents) {
        switch (event.kind) {
            case PluginEvent::Kind::ParamValue: {
                if (event.paramIndex >= m_parameterIds.size()) break;
                if (orderCount >= m_inputEventScratch.size()) break;
                clap_event_param_value_t& out =
                    m_inputEventScratch[orderCount].parameter;
                out = {};
                out.header.size = sizeof(out);
                out.header.time = event.frameOffset;
                out.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                out.header.type = CLAP_EVENT_PARAM_VALUE;
                out.param_id = m_parameterIds[event.paramIndex];
                out.note_id = -1;
                out.port_index = -1;
                out.channel = -1;
                out.key = -1;
                out.value = event.value;
                m_eventOrder[orderCount++] = &out.header;
                break;
            }
            case PluginEvent::Kind::NoteOn:
            case PluginEvent::Kind::NoteOff:
            case PluginEvent::Kind::NoteChoke: {
                if (orderCount >= m_inputEventScratch.size()) break;
                clap_event_note_t& out = m_inputEventScratch[orderCount].note;
                out = {};
                out.header.size = sizeof(out);
                out.header.time = event.frameOffset;
                out.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                out.header.type = event.kind == PluginEvent::Kind::NoteOn
                                      ? CLAP_EVENT_NOTE_ON
                                      : (event.kind == PluginEvent::Kind::NoteOff
                                             ? CLAP_EVENT_NOTE_OFF
                                             : CLAP_EVENT_NOTE_CHOKE);
                out.note_id = event.noteId;
                out.port_index = 0;
                out.channel = event.channel;
                out.key = event.key;
                out.velocity = event.value;
                m_eventOrder[orderCount++] = &out.header;
                break;
            }
            case PluginEvent::Kind::MidiController: {
                if (event.channel < 0 || event.channel >= 16 ||
                    event.paramIndex > 130 ||
                    orderCount >= m_inputEventScratch.size()) {
                    break;
                }

                clap_event_midi_t& out = m_inputEventScratch[orderCount].midi;
                out = {};
                out.header.size = sizeof(out);
                out.header.time = event.frameOffset;
                out.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                out.header.type = CLAP_EVENT_MIDI;
                out.port_index = 0;
                const std::uint8_t channel = std::uint8_t(event.channel);
                const auto midi7 = [](double value) noexcept {
                    return std::uint8_t(std::clamp(
                        std::lround(std::clamp(value, 0.0, 1.0) * 127.0), 0L, 127L));
                };

                if (event.paramIndex <= 127) {
                    out.data[0] = std::uint8_t(0xB0 | channel);
                    out.data[1] = std::uint8_t(event.paramIndex);
                    out.data[2] = midi7(event.value);
                } else if (event.paramIndex == 128) {
                    out.data[0] = std::uint8_t(0xD0 | channel);
                    out.data[1] = midi7(event.value);
                } else if (event.paramIndex == 129) {
                    const auto bend = std::uint32_t(std::clamp(
                        std::lround(std::clamp(event.value, 0.0, 1.0) * 16383.0),
                        0L, 16383L));
                    out.data[0] = std::uint8_t(0xE0 | channel);
                    out.data[1] = std::uint8_t(bend & 0x7F);
                    out.data[2] = std::uint8_t((bend >> 7) & 0x7F);
                } else if (event.paramIndex == 130) {
                    out.data[0] = std::uint8_t(0xC0 | channel);
                    out.data[1] = midi7(event.value);
                }

                m_eventOrder[orderCount++] = &out.header;
                break;
            }
            case PluginEvent::Kind::PolyPressure: {
                if (orderCount >= m_inputEventScratch.size()) break;
                clap_event_note_expression_t& out =
                    m_inputEventScratch[orderCount].noteExpression;
                out = {};
                out.header.size = sizeof(out);
                out.header.time = event.frameOffset;
                out.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                out.header.type = CLAP_EVENT_NOTE_EXPRESSION;
                out.expression_id = CLAP_NOTE_EXPRESSION_PRESSURE;
                out.note_id = event.noteId;
                out.port_index = 0;
                out.channel = event.channel;
                out.key = event.key;
                out.value = std::clamp(event.value, 0.0, 1.0);
                m_eventOrder[orderCount++] = &out.header;
                break;
            }
            default:
                break;   // host-side gestures are not sent to the plugin
        }
    }

    // CLAP requires the input list to be sorted by time. The caller's events
    // already are, but interleaving params and notes into one list can break
    // that, so sort — stable, to keep same-frame ordering deterministic.
    engine::stableRealtimeSort(
        m_eventOrder.begin(), m_eventOrder.begin() + orderCount,
        [](const clap_event_header_t* a, const clap_event_header_t* b) {
            return a->time < b->time;
        });

    struct InputEvents {
        const clap_event_header_t* const* items;
        std::uint32_t count;
    } inputs{m_eventOrder.data(), orderCount};

    clap_input_events_t inEvents{};
    inEvents.ctx = &inputs;
    inEvents.size = [](const clap_input_events_t* list) -> std::uint32_t {
        return static_cast<const InputEvents*>(list->ctx)->count;
    };
    inEvents.get = [](const clap_input_events_t* list,
                      std::uint32_t index) -> const clap_event_header_t* {
        return static_cast<const InputEvents*>(list->ctx)->items[index];
    };

    OutputEvents outputs{context.outputEvents, &m_parameterIds};
    clap_output_events_t outEvents{};
    outEvents.ctx = &outputs;
    outEvents.try_push = &OutputEvents::tryPush;

    // ── Transport ──
    clap_event_transport_t transport{};
    transport.header.size = sizeof(transport);
    transport.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    transport.header.type = CLAP_EVENT_TRANSPORT;
    transport.flags = CLAP_TRANSPORT_HAS_TEMPO |
                      CLAP_TRANSPORT_HAS_BEATS_TIMELINE |
                      CLAP_TRANSPORT_HAS_TIME_SIGNATURE;
    if (context.playing) transport.flags |= CLAP_TRANSPORT_IS_PLAYING;
    if (context.transport.recording) transport.flags |= CLAP_TRANSPORT_IS_RECORDING;
    if (context.transport.looping) transport.flags |= CLAP_TRANSPORT_IS_LOOP_ACTIVE;
    transport.tempo = context.transport.tempo;
    transport.song_pos_beats = toBeatTime(context.transport.ppqPosition);
    transport.bar_start = toBeatTime(context.transport.barStartPpq);
    transport.loop_start_beats = toBeatTime(context.transport.loopStartPpq);
    transport.loop_end_beats = toBeatTime(context.transport.loopEndPpq);
    transport.tsig_num = std::uint16_t(context.transport.timeSigNumerator);
    transport.tsig_denom = std::uint16_t(context.transport.timeSigDenominator);

    // ── Buses ──
    //
    // Every bus the plugin declared is handed over, not just the main one: the
    // plugin indexes `audio_inputs[i]` itself, so passing a shorter array is an
    // out-of-bounds read inside third-party code — a segfault on the audio
    // thread, not a graceful degradation. Buses the host has nothing for get
    // this instance's own buffers: silence going in, discarded coming out.
    const std::uint32_t frames = context.frames;

    for (std::size_t bus = 0; bus < m_inputBuses.size(); ++bus) {
        AudioBus& target = m_inputBuses[bus];
        const float* const* source =
            bus == 0 ? context.inputs
                     : (bus == 1 ? context.sidechainInputs : nullptr);
        const std::uint16_t sourceChannels =
            bus == 0 ? context.inputChannels
                     : (bus == 1 ? context.sidechainInputChannels : 0);
        const std::uint64_t silenceMask =
            bus == 0 ? context.inputSilenceMask
                     : (bus == 1 ? context.sidechainSilenceMask : ~std::uint64_t{0});
        const std::uint32_t supplied =
            std::min<std::uint32_t>(sourceChannels, target.channels);
        std::uint64_t constant = 0;
        for (std::uint32_t channel = 0; channel < target.channels; ++channel) {
            if (channel < supplied) {
                target.pointers[channel] = const_cast<float*>(source[channel]);
                if (channel < 64 &&
                    (silenceMask & (std::uint64_t{1} << channel)) != 0) {
                    constant |= std::uint64_t{1} << channel;
                }
                continue;
            }
            // Re-point and re-zero every block: a plugin is allowed to write
            // into an input buffer it was given, so last block's silence
            // cannot be assumed to still be silent.
            float* own = target.storage.data() + std::size_t(channel) * m_maxBlockSize;
            target.pointers[channel] = own;
            std::fill_n(own, frames, 0.0f);
            if (channel < 64) constant |= std::uint64_t(1) << channel;
        }
        m_inputBufferDescs[bus].data32 = target.pointers.data();
        // Truthful, and it lets a plugin skip the silent sidechain entirely.
        m_inputBufferDescs[bus].constant_mask = constant;
    }

    for (std::size_t bus = 0; bus < m_outputBuses.size(); ++bus) {
        AudioBus& target = m_outputBuses[bus];
        const std::uint32_t supplied =
            bus == 0 ? std::min<std::uint32_t>(context.outputChannels, target.channels) : 0;
        for (std::uint32_t channel = 0; channel < target.channels; ++channel) {
            target.pointers[channel] =
                channel < supplied
                    ? context.outputs[channel]
                    : target.storage.data() + std::size_t(channel) * m_maxBlockSize;
        }
        m_outputBufferDescs[bus].data32 = target.pointers.data();
        m_outputBufferDescs[bus].constant_mask = 0;
    }

    clap_process_t process{};
    process.steady_time = context.sampleTime;
    process.frames_count = frames;
    process.transport = &transport;
    process.audio_inputs = m_inputBufferDescs.empty() ? nullptr : m_inputBufferDescs.data();
    process.audio_inputs_count = std::uint32_t(m_inputBufferDescs.size());
    process.audio_outputs = m_outputBufferDescs.empty() ? nullptr : m_outputBufferDescs.data();
    process.audio_outputs_count = std::uint32_t(m_outputBufferDescs.size());
    process.in_events = &inEvents;
    process.out_events = &outEvents;

    // CLAP defines the output buffers as valid only when the plugin did not
    // fail. On an error they hold whatever the arena recycled into them, and
    // playing that back block after block is a buzz rather than silence.
    const clap_process_status status = m_plugin->process(m_plugin, &process);
    if (status == CLAP_PROCESS_ERROR) {
        silence();
        return PluginProcessDisposition::Continue;
    }
    if (status == CLAP_PROCESS_SLEEP) return PluginProcessDisposition::Sleep;
    if (status == CLAP_PROCESS_TAIL) {
        // TAIL delegates the stopping decision to clap.tail. Without that
        // extension there is no contract-grade duration to count down, so keep
        // calling rather than guessing zero and truncating an audible tail.
        if (!m_tailExt || !m_tailExt->get) {
            return PluginProcessDisposition::Continue;
        }
        // The extension may be dynamic and is explicitly audio-thread safe.
        refreshTail();
        return PluginProcessDisposition::Tail;
    }
    if (status == CLAP_PROCESS_CONTINUE_IF_NOT_QUIET) {
        for (const AudioBus& bus : m_outputBuses) {
            for (std::uint32_t channel = 0; channel < bus.channels; ++channel) {
                const float* samples = bus.pointers[channel];
                if (!samples) return PluginProcessDisposition::Continue;
                for (std::uint32_t frame = 0; frame < frames; ++frame) {
                    if (samples[frame] != 0.0f) {
                        return PluginProcessDisposition::Continue;
                    }
                }
            }
        }
        return PluginProcessDisposition::Sleep;
    }
    return PluginProcessDisposition::Continue;
}

} // namespace daw::plugins
