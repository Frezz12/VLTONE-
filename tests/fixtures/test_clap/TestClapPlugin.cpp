// A real CLAP plugin, built in-tree, used as the fixture for the host tests.
//
// A mock implementing our own PluginInstance would prove only that our own code
// calls itself. This goes through the whole path a shipped plugin does: a
// bundle on disk, dlopen, the clap_entry symbol, the factory, activation,
// events, state — so the tests fail when any of that is wrong.
//
// Behaviour is chosen to be checkable from outside:
//   out[i] = in[i - kLatency] * gain + offset
// A real 64-sample delay with a matching reported latency is what makes the
// delay-compensation assertions mean something.

#include <clap/clap.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

constexpr std::uint32_t kLatency = 64;
/// Written to the output when the host did not supply every declared input
/// bus. Far outside any signal the tests generate, so it cannot be mistaken
/// for audio.
constexpr float kMissingBusSentinel = -999.0f;
constexpr clap_id kParamGain = 0;
constexpr clap_id kParamOffset = 1;

const char* const kFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
                                 CLAP_PLUGIN_FEATURE_UTILITY, nullptr};

// The instrument half of this fixture. Deliberately the crudest synth that can
// be measured: while a note is held it writes that note's velocity as a DC
// level, and silence otherwise. A host that loses a note-on writes silence, a
// host that loses a note-off writes forever, and a host that mistimes either
// gets the wrong sample index — all three visible in one array of floats.
const char* const kInstrumentFeatures[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT,
                                           CLAP_PLUGIN_FEATURE_SYNTHESIZER, nullptr};

const clap_plugin_descriptor_t kInstrumentDescriptor = {
    CLAP_VERSION_INIT,
    "com.daw.test.tone",
    "DAW Test Tone",
    "DAW",
    "",
    "",
    "",
    "1.0.0",
    "Holds a DC level while a note is on, for MIDI host tests",
    kInstrumentFeatures,
};

const clap_plugin_descriptor_t kDescriptor = {
    CLAP_VERSION_INIT,
    "com.daw.test.gain",
    "DAW Test Gain",
    "DAW",
    "",
    "",
    "",
    "1.0.0",
    "Gain and offset with a known latency, for host tests",
    kFeatures,
};

struct TestPlugin {
    clap_plugin_t plugin{};
    const clap_host_t* host = nullptr;
    /// Instruments declare no audio input and answer notes instead.
    bool isInstrument = false;
    /// -1 when nothing is sounding; otherwise the velocity of the held note.
    float held = -1.0f;
    double gain = 1.0;
    double offset = 0.0;
    bool active = false;
    bool processing = false;
    std::uint64_t processCalls = 0;
    std::uint64_t mainThreadCalls = 0;
    std::uint32_t channels = 0;
    /// One ring per channel, kLatency long, so the reported latency is real.
    std::vector<float> delay;
    std::uint32_t writePosition = 0;

    static TestPlugin* of(const clap_plugin_t* plugin) {
        return static_cast<TestPlugin*>(plugin->plugin_data);
    }
};

// ── params ──

std::uint32_t paramsCount(const clap_plugin_t*) { return 2; }

bool paramsGetInfo(const clap_plugin_t*, std::uint32_t index,
                   clap_param_info_t* info) {
    if (index > 1) return false;
    std::memset(info, 0, sizeof(*info));
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    if (index == 0) {
        info->id = kParamGain;
        std::snprintf(info->name, sizeof(info->name), "Gain");
        info->min_value = 0.0;
        info->max_value = 2.0;
        info->default_value = 1.0;
    } else {
        info->id = kParamOffset;
        std::snprintf(info->name, sizeof(info->name), "Offset");
        info->min_value = -1.0;
        info->max_value = 1.0;
        info->default_value = 0.0;
    }
    return true;
}

bool paramsGetValue(const clap_plugin_t* plugin, clap_id id, double* out) {
    auto* self = TestPlugin::of(plugin);
    if (id == kParamGain) {
        *out = self->gain;
        return true;
    }
    if (id == kParamOffset) {
        *out = self->offset;
        return true;
    }
    return false;
}

bool paramsValueToText(const clap_plugin_t*, clap_id id, double value,
                       char* out, std::uint32_t capacity) {
    std::snprintf(out, capacity, id == kParamGain ? "%.3f x" : "%.3f", value);
    return true;
}

bool paramsTextToValue(const clap_plugin_t*, clap_id, const char* text,
                       double* out) {
    *out = std::atof(text);
    return true;
}

void applyParam(TestPlugin* self, clap_id id, double value) {
    if (id == kParamGain) self->gain = value;
    else if (id == kParamOffset) {
        self->offset = value;
        // A reserved fixture value asks for a real CLAP main-thread callback.
        // It travels from process() through host.request_callback, exactly like
        // a third-party plugin's deferred UI/control work.
        if (value == 0.875 && self->host && self->host->request_callback)
            self->host->request_callback(self->host);
    }
}

void paramsFlush(const clap_plugin_t* plugin, const clap_input_events_t* in,
                 const clap_output_events_t*) {
    auto* self = TestPlugin::of(plugin);
    if (!in) return;
    const std::uint32_t count = in->size(in);
    for (std::uint32_t i = 0; i < count; ++i) {
        const clap_event_header_t* header = in->get(in, i);
        if (header->space_id != CLAP_CORE_EVENT_SPACE_ID) continue;
        if (header->type != CLAP_EVENT_PARAM_VALUE) continue;
        const auto* event = reinterpret_cast<const clap_event_param_value_t*>(header);
        applyParam(self, event->param_id, event->value);
    }
}

const clap_plugin_params_t kParams = {
    paramsCount, paramsGetInfo, paramsGetValue,
    paramsValueToText, paramsTextToValue, paramsFlush,
};

// ── audio ports ──

// Two input buses, one output — the shape most real effects have. The second
// bus exists so the host is forced to hand over *every* declared bus: a plugin
// indexes `process->audio_inputs[i]` itself, so a host that passes only the
// main bus makes this plugin read past the end of the array. That is exactly
// the crash real sidechain-capable plugins hit, and a one-bus fixture cannot
// catch it.
std::uint32_t portsCount(const clap_plugin_t* plugin, bool isInput) {
    if (TestPlugin::of(plugin)->isInstrument) return isInput ? 0 : 1;
    return isInput ? 2 : 1;
}

bool portsGet(const clap_plugin_t* plugin, std::uint32_t index, bool isInput,
              clap_audio_port_info_t* info) {
    if (index >= portsCount(plugin, isInput)) return false;
    std::memset(info, 0, sizeof(*info));
    const bool sidechain = isInput && index == 1;
    info->id = isInput ? index : 100;
    std::snprintf(info->name, sizeof(info->name),
                  sidechain ? "Sidechain" : (isInput ? "In" : "Out"));
    info->channel_count = 2;
    info->flags = sidechain ? 0 : CLAP_AUDIO_PORT_IS_MAIN;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

const clap_plugin_audio_ports_t kAudioPorts = {portsCount, portsGet};

// ── latency ──

std::uint32_t latencyGet(const clap_plugin_t*) { return kLatency; }
const clap_plugin_latency_t kLatencyExt = {latencyGet};

// ── tail ──
//
// Three otherwise unused gain values exercise all CLAP process dispositions
// without adding a test-only parameter to the descriptor:
//   0.0    -> SLEEP
//   0.0625 -> CONTINUE_IF_NOT_QUIET
//   0.125  -> TAIL (256 samples)
std::uint32_t tailGet(const clap_plugin_t* plugin) {
    return TestPlugin::of(plugin)->gain == 0.125 ? 256u : 0u;
}
const clap_plugin_tail_t kTailExt = {tailGet};

// ── state ──
//
// The first two doubles are the actual state. The third and fourth are test
// diagnostics: they let the host test prove process and main-thread callback
// counts through the real dynamically loaded plugin, without a test-only API
// in PluginInstance.

bool stateSave(const clap_plugin_t* plugin, const clap_ostream_t* stream) {
    auto* self = TestPlugin::of(plugin);
    const double values[4] = {self->gain, self->offset,
                              double(self->processCalls),
                              double(self->mainThreadCalls)};
    return stream->write(stream, values, sizeof(values)) == sizeof(values);
}

bool stateLoad(const clap_plugin_t* plugin, const clap_istream_t* stream) {
    auto* self = TestPlugin::of(plugin);
    double values[2] = {1.0, 0.0};
    if (stream->read(stream, values, sizeof(values)) != sizeof(values)) return false;
    self->gain = values[0];
    self->offset = values[1];
    return true;
}

const clap_plugin_state_t kState = {stateSave, stateLoad};

// ── plugin ──

bool pluginInit(const clap_plugin_t*) { return true; }

void pluginDestroy(const clap_plugin_t* plugin) { delete TestPlugin::of(plugin); }

bool pluginActivate(const clap_plugin_t* plugin, double, std::uint32_t,
                    std::uint32_t) {
    auto* self = TestPlugin::of(plugin);
    self->channels = 2;
    self->delay.assign(std::size_t(self->channels) * kLatency, 0.0f);
    self->writePosition = 0;
    self->active = true;
    return true;
}

void pluginDeactivate(const clap_plugin_t* plugin) {
    TestPlugin::of(plugin)->active = false;
}

bool pluginStartProcessing(const clap_plugin_t* plugin) {
    TestPlugin::of(plugin)->processing = true;
    return true;
}

void pluginStopProcessing(const clap_plugin_t* plugin) {
    TestPlugin::of(plugin)->processing = false;
}

void pluginReset(const clap_plugin_t* plugin) {
    auto* self = TestPlugin::of(plugin);
    std::fill(self->delay.begin(), self->delay.end(), 0.0f);
    self->writePosition = 0;
}

clap_process_status pluginProcess(const clap_plugin_t* plugin,
                                  const clap_process_t* process) {
    auto* self = TestPlugin::of(plugin);
    if (!self->processing || !process || process->audio_outputs_count == 0) {
        return CLAP_PROCESS_ERROR;
    }
    ++self->processCalls;

    // Parameter events are applied at their frame offset, which is what makes
    // them worth being events rather than polled values.
    std::uint32_t nextEvent = 0;
    const clap_input_events_t* in = process->in_events;
    const std::uint32_t eventCount = in ? in->size(in) : 0;

    const std::uint32_t frames = process->frames_count;
    float** out = process->audio_outputs[0].data32;

    // ── The instrument half: notes in, DC out ──
    if (self->isInstrument) {
        // Echo musical output so the host tests exercise both halves of the
        // CLAP event adapter with a real dynamically loaded plugin. Parameter
        // and transport events are deliberately excluded: this fixture is a
        // MIDI instrument, not a host-notification loopback.
        if (process->out_events && process->out_events->try_push) {
            for (std::uint32_t index = 0; index < eventCount; ++index) {
                const clap_event_header_t* header = in->get(in, index);
                if (!header || header->space_id != CLAP_CORE_EVENT_SPACE_ID) continue;
                if (header->type == CLAP_EVENT_NOTE_ON ||
                    header->type == CLAP_EVENT_NOTE_OFF ||
                    header->type == CLAP_EVENT_NOTE_CHOKE ||
                    header->type == CLAP_EVENT_NOTE_EXPRESSION ||
                    header->type == CLAP_EVENT_MIDI) {
                    (void)process->out_events->try_push(process->out_events, header);
                }
            }
        }

        std::uint32_t next = 0;
        for (std::uint32_t i = 0; i < frames; ++i) {
            while (next < eventCount) {
                const clap_event_header_t* header = in->get(in, next);
                if (header->time > i) break;
                if (header->space_id == CLAP_CORE_EVENT_SPACE_ID) {
                    if (header->type == CLAP_EVENT_NOTE_ON) {
                        self->held =
                            float(reinterpret_cast<const clap_event_note_t*>(header)->velocity);
                    } else if (header->type == CLAP_EVENT_NOTE_OFF ||
                               header->type == CLAP_EVENT_NOTE_CHOKE) {
                        self->held = -1.0f;
                    }
                }
                ++next;
            }
            const float value = self->held >= 0.0f ? self->held : 0.0f;
            for (std::uint32_t ch = 0; ch < self->channels; ++ch) out[ch][i] = value;
        }
        return CLAP_PROCESS_CONTINUE;
    }

    // A host that did not supply both declared input buses gets a sentinel
    // instead of an out-of-bounds read, so the failure is a visible wrong
    // number in a test rather than a segfault that may or may not reproduce.
    if (process->audio_inputs_count < 2) {
        for (std::uint32_t ch = 0; ch < self->channels; ++ch) {
            for (std::uint32_t i = 0; i < frames; ++i) out[ch][i] = kMissingBusSentinel;
        }
        return CLAP_PROCESS_ERROR;
    }
    float** input = process->audio_inputs[0].data32;
    float** sidechain = process->audio_inputs[1].data32;
    bool anyInput = false;

    for (std::uint32_t i = 0; i < frames; ++i) {
        while (nextEvent < eventCount) {
            const clap_event_header_t* header = in->get(in, nextEvent);
            if (header->time > i) break;
            if (header->space_id == CLAP_CORE_EVENT_SPACE_ID &&
                header->type == CLAP_EVENT_PARAM_VALUE) {
                const auto* event =
                    reinterpret_cast<const clap_event_param_value_t*>(header);
                applyParam(self, event->param_id, event->value);
            }
            ++nextEvent;
        }

        const std::uint32_t slot = self->writePosition;
        for (std::uint32_t ch = 0; ch < self->channels; ++ch) {
            float* ring = self->delay.data() + std::size_t(ch) * kLatency;
            // The sidechain is read, not just accepted: a bus handed over as a
            // dangling or unwritten pointer has to show up as wrong output.
            const float sample = (input ? input[ch][i] : 0.0f) + sidechain[ch][i];
            if (sample != 0.0f) anyInput = true;
            const float delayed = ring[slot];
            ring[slot] = sample;
            out[ch][i] = float(delayed * self->gain + self->offset);
        }
        self->writePosition = (slot + 1) % kLatency;
    }
    if (self->gain == 0.0 && self->offset == 0.0) {
        return CLAP_PROCESS_SLEEP;
    }
    if (self->gain == 0.0625 && !anyInput) {
        return CLAP_PROCESS_CONTINUE_IF_NOT_QUIET;
    }
    if (self->gain == 0.125 && !anyInput) return CLAP_PROCESS_TAIL;
    return CLAP_PROCESS_CONTINUE;
}

const void* pluginGetExtension(const clap_plugin_t*, const char* id) {
    if (std::strcmp(id, CLAP_EXT_PARAMS) == 0) return &kParams;
    if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) return &kAudioPorts;
    if (std::strcmp(id, CLAP_EXT_LATENCY) == 0) return &kLatencyExt;
    if (std::strcmp(id, CLAP_EXT_TAIL) == 0) return &kTailExt;
    if (std::strcmp(id, CLAP_EXT_STATE) == 0) return &kState;
    return nullptr;
}

void pluginOnMainThread(const clap_plugin_t* plugin) {
    ++TestPlugin::of(plugin)->mainThreadCalls;
}

// ── factory ──

std::uint32_t factoryCount(const clap_plugin_factory*) { return 2; }

const clap_plugin_descriptor_t* factoryDescriptor(const clap_plugin_factory*,
                                                  std::uint32_t index) {
    if (index == 0) return &kDescriptor;
    if (index == 1) return &kInstrumentDescriptor;
    return nullptr;
}

const clap_plugin_t* factoryCreate(const clap_plugin_factory*,
                                   const clap_host_t* host, const char* id) {
    if (!host || !id) return nullptr;
    const bool instrument = std::strcmp(id, kInstrumentDescriptor.id) == 0;
    if (!instrument && std::strcmp(id, kDescriptor.id) != 0) return nullptr;

    auto* self = new TestPlugin();
    self->host = host;
    self->isInstrument = instrument;
    self->plugin.desc = instrument ? &kInstrumentDescriptor : &kDescriptor;
    self->plugin.plugin_data = self;
    self->plugin.init = pluginInit;
    self->plugin.destroy = pluginDestroy;
    self->plugin.activate = pluginActivate;
    self->plugin.deactivate = pluginDeactivate;
    self->plugin.start_processing = pluginStartProcessing;
    self->plugin.stop_processing = pluginStopProcessing;
    self->plugin.reset = pluginReset;
    self->plugin.process = pluginProcess;
    self->plugin.get_extension = pluginGetExtension;
    self->plugin.on_main_thread = pluginOnMainThread;
    return &self->plugin;
}

const clap_plugin_factory_t kFactory = {factoryCount, factoryDescriptor,
                                        factoryCreate};

// ── entry ──

bool entryInit(const char*) { return true; }
void entryDeinit() {}

const void* entryGetFactory(const char* id) {
    return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &kFactory : nullptr;
}

} // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT,
    entryInit,
    entryDeinit,
    entryGetFactory,
};
