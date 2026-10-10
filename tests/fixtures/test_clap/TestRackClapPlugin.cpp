// Real external processors with 0, 5 and 1,033 parameters exercise the rack's
// generic editor and virtualization through the production CLAP host.
#include <algorithm>
#include <array>
#include <clap/clap.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
namespace {
const char* const features[]{CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, nullptr};
const clap_plugin_descriptor_t descriptors[]{{CLAP_VERSION_INIT, "com.daw.test.rack-empty",
                                              "No published parameters", "VLTONE", "", "", "", "1",
                                              "Rack fixture", features},
                                             {CLAP_VERSION_INIT, "com.daw.test.rack-small", "Five parameters",
                                              "VLTONE", "", "", "", "1", "Rack fixture", features},
                                             {CLAP_VERSION_INIT, "com.daw.test.rack-large",
                                              "A very long external plugin name — 1033 published controls",
                                              "VLTONE", "", "", "", "1", "Rack fixture", features}};
struct Instance {
    clap_plugin_t plugin{};
    unsigned count = 0;
    std::array<double, 1033> values{};
};
Instance* self(const clap_plugin_t* plugin) {
    return static_cast<Instance*>(plugin->plugin_data);
}
uint32_t count(const clap_plugin_t* p) {
    return self(p)->count;
}
bool info(const clap_plugin_t* p, uint32_t index, clap_param_info_t* result) {
    if (index >= count(p))
        return false;
    *result = {};
    result->id = index;
    result->flags = CLAP_PARAM_IS_AUTOMATABLE;
    std::snprintf(result->name, sizeof(result->name),
                  index == 0 ? "A parameter with a long name %u" : "Control %u", index + 1);
    result->min_value = 0;
    result->max_value = 1;
    result->default_value = (index % 100) * .01;
    return true;
}
bool value(const clap_plugin_t* p, clap_id id, double* out) {
    if (id >= count(p))
        return false;
    *out = self(p)->values[id];
    return true;
}
bool text(const clap_plugin_t*, clap_id, double value, char* out, uint32_t capacity) {
    std::snprintf(out, capacity, "%.1f %%", value * 100);
    return true;
}
bool fromText(const clap_plugin_t*, clap_id, const char* input, double* out) {
    *out = std::atof(input) * .01;
    return true;
}
void flush(const clap_plugin_t* p, const clap_input_events_t* events, const clap_output_events_t*) {
    if (!events)
        return;
    for (unsigned i = 0; i < events->size(events); ++i) {
        const auto* h = events->get(events, i);
        if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE) {
            const auto* v = reinterpret_cast<const clap_event_param_value_t*>(h);
            if (v->param_id < count(p))
                self(p)->values[v->param_id] = std::clamp(v->value, 0., 1.);
        }
    }
}
const clap_plugin_params_t params{count, info, value, text, fromText, flush};
uint32_t ports(const clap_plugin_t*, bool input) {
    return input ? 2 : 1;
}
bool port(const clap_plugin_t*, uint32_t index, bool input, clap_audio_port_info_t* result) {
    if (index >= (input ? 2u : 1u))
        return false;
    *result = {};
    result->id = index;
    result->channel_count = 2;
    result->flags = index == 0 ? CLAP_AUDIO_PORT_IS_MAIN : 0;
    result->port_type = CLAP_PORT_STEREO;
    result->in_place_pair = CLAP_INVALID_ID;
    std::strcpy(result->name, index == 1 ? "Sidechain" : "Audio");
    return true;
}
const clap_plugin_audio_ports_t audioPorts{ports, port};
bool save(const clap_plugin_t* p, const clap_ostream_t* stream) {
    const auto bytes = int64_t(count(p) * sizeof(double));
    return stream->write(stream, self(p)->values.data(), bytes) == bytes;
}
bool load(const clap_plugin_t* p, const clap_istream_t* stream) {
    const auto bytes = int64_t(count(p) * sizeof(double));
    return stream->read(stream, self(p)->values.data(), bytes) == bytes;
}
const clap_plugin_state_t state{save, load};
const void* extension(const clap_plugin_t*, const char* id) {
    if (!std::strcmp(id, CLAP_EXT_PARAMS))
        return &params;
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS))
        return &audioPorts;
    if (!std::strcmp(id, CLAP_EXT_STATE))
        return &state;
    return nullptr;
}
clap_process_status process(const clap_plugin_t* p, const clap_process_t* block) {
    flush(p, block->in_events, block->out_events);
    for (unsigned bus = 0; bus < block->audio_outputs_count; ++bus) {
        const auto& out = block->audio_outputs[bus];
        for (unsigned ch = 0; ch < out.channel_count; ++ch)
            if (out.data32 && out.data32[ch]) {
                const float* in = block->audio_inputs_count && block->audio_inputs[0].data32 &&
                                          ch < block->audio_inputs[0].channel_count
                                      ? block->audio_inputs[0].data32[ch]
                                      : nullptr;
                for (unsigned f = 0; f < block->frames_count; ++f)
                    out.data32[ch][f] = in ? in[f] : 0;
            }
    }
    return CLAP_PROCESS_CONTINUE;
}
uint32_t factoryCount(const clap_plugin_factory_t*) {
    return 3;
}
const clap_plugin_descriptor_t* descriptor(const clap_plugin_factory_t*, uint32_t i) {
    return i < 3 ? &descriptors[i] : nullptr;
}
const clap_plugin_t* create(const clap_plugin_factory_t*, const clap_host_t*, const char* id) {
    for (unsigned i = 0; i < 3; ++i)
        if (!std::strcmp(id, descriptors[i].id)) {
            auto* p = new Instance;
            p->count = i == 0 ? 0 : i == 1 ? 5 : 1033;
            for (unsigned n = 0; n < p->count; ++n)
                p->values[n] = (n % 100) * .01;
            p->plugin.desc = &descriptors[i];
            p->plugin.plugin_data = p;
            p->plugin.init = [](const clap_plugin_t*) { return true; };
            p->plugin.destroy = [](const clap_plugin_t* p) { delete self(p); };
            p->plugin.activate = [](const clap_plugin_t*, double, uint32_t, uint32_t) { return true; };
            p->plugin.deactivate = [](const clap_plugin_t*) {};
            p->plugin.start_processing = [](const clap_plugin_t*) { return true; };
            p->plugin.stop_processing = [](const clap_plugin_t*) {};
            p->plugin.reset = [](const clap_plugin_t*) {};
            p->plugin.process = process;
            p->plugin.get_extension = extension;
            p->plugin.on_main_thread = [](const clap_plugin_t*) {};
            return &p->plugin;
        }
    return nullptr;
}
const clap_plugin_factory_t factory{factoryCount, descriptor, create};
} // namespace
extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry{
    CLAP_VERSION_INIT, [](const char*) { return true; }, []() {},
    [](const char* id) -> const void* {
        return !std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &factory : nullptr;
    }};
