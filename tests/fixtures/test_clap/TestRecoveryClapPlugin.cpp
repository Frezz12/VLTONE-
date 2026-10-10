#include <clap/clap.h>
#include "RecoveryTestControl.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace {
RecoveryTestControl control;
const char* features[]{CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, nullptr};
const clap_plugin_descriptor_t descriptor{CLAP_VERSION_INIT, "test.recovery", "Recovery fixture", "VLTONE tests", "", "", "", "1", "", features};
struct Instance {
    clap_plugin_t plugin{};
    unsigned id{}, position{};
    float gain = .5f;
    bool failed = false;
    float ring[2][32]{};
    static Instance& get(const clap_plugin_t* p) { return *static_cast<Instance*>(p->plugin_data); }
    void checked() const { if (failed) ++control.forbidden; }
};
uint32_t portsCount(const clap_plugin_t*, bool) { return 1; }
bool portsGet(const clap_plugin_t*, uint32_t index, bool, clap_audio_port_info_t* info) {
    if (index) return false;
    *info = {}; info->id = 0; info->channel_count = 2;
    info->flags = CLAP_AUDIO_PORT_IS_MAIN; info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID; return true;
}
const clap_plugin_audio_ports_t ports{portsCount, portsGet};
uint32_t latencyGet(const clap_plugin_t*) { return 32; }
const clap_plugin_latency_t latency{latencyGet};
uint32_t paramsCount(const clap_plugin_t*) { return 1; }
bool paramsInfo(const clap_plugin_t*, uint32_t index, clap_param_info_t* info) {
    if (index) return false;
    *info = {}; info->id = 0; info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    std::strcpy(info->name, "Gain"); info->min_value = 0; info->max_value = 2; info->default_value = .5;
    return true;
}
bool paramsValue(const clap_plugin_t* p, clap_id id, double* value) {
    auto& self = Instance::get(p); self.checked(); *value = self.gain; return id == 0;
}
void events(Instance& self, const clap_input_events_t* input) {
    if (!input) return;
    for (unsigned i = 0; i < input->size(input); ++i) {
        const auto* event = input->get(input, i);
        if (event->space_id == CLAP_CORE_EVENT_SPACE_ID && event->type == CLAP_EVENT_PARAM_VALUE)
            self.gain = float(reinterpret_cast<const clap_event_param_value_t*>(event)->value);
    }
}
void flush(const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t*) {
    auto& self = Instance::get(p); self.checked(); events(self, in);
}
const clap_plugin_params_t params{paramsCount, paramsInfo, paramsValue, nullptr, nullptr, flush};
bool save(const clap_plugin_t* p, const clap_ostream_t* out) {
    auto& self = Instance::get(p); self.checked(); ++control.saves;
    return !control.failSave && out->write(out, &self.gain, sizeof(self.gain)) == sizeof(self.gain);
}
bool load(const clap_plugin_t* p, const clap_istream_t* in) {
    auto& self = Instance::get(p); self.checked(); ++control.loads;
    float value = 0;
    if (control.failLoad || in->read(in, &value, sizeof(value)) != sizeof(value) || !std::isfinite(value)) return false;
    self.gain = value; return true;
}
const clap_plugin_state_t state{save, load};
bool init(const clap_plugin_t*) { return true; }
void destroy(const clap_plugin_t* p) { delete &Instance::get(p); }
bool activate(const clap_plugin_t* p, double, uint32_t, uint32_t) {
    auto& self = Instance::get(p); self.checked();
    for (auto& row : self.ring) std::fill_n(row, 32, 0.f);
    self.position = 0; return !control.failActivate;
}
void nothing(const clap_plugin_t*) {}
bool start(const clap_plugin_t*) { return true; }
void reset(const clap_plugin_t* p) { Instance::get(p).checked(); }
clap_process_status process(const clap_plugin_t* p, const clap_process_t* block) {
    auto& self = Instance::get(p); self.checked(); ++control.calls[self.id];
    events(self, block->in_events);
    const unsigned mode = (!control.target || control.target == self.id) ? control.mode.load() : 0;
    for (unsigned frame = 0; frame < block->frames_count; ++frame) {
        for (unsigned ch = 0; ch < 2; ++ch) {
            auto& ring = self.ring[ch][self.position];
            block->audio_outputs[0].data32[ch][frame] = ring * self.gain;
            ring = block->audio_inputs[0].data32[ch][frame];
        }
        self.position = (self.position + 1) % 32;
    }
    if (mode) {
        self.failed = true;
        if (mode > 1) block->audio_outputs[0].data32[1][0] = mode == 2
            ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
    }
    return mode == 1 ? CLAP_PROCESS_ERROR : CLAP_PROCESS_CONTINUE;
}
const void* extension(const clap_plugin_t*, const char* id) {
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &ports;
    if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &latency;
    if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &params;
    if (!std::strcmp(id, CLAP_EXT_STATE)) return &state;
    return nullptr;
}
uint32_t count(const clap_plugin_factory_t*) { return 1; }
const clap_plugin_descriptor_t* getDescriptor(const clap_plugin_factory_t*, uint32_t i) { return i ? nullptr : &descriptor; }
const clap_plugin_t* create(const clap_plugin_factory_t*, const clap_host_t*, const char* id) {
    if (std::strcmp(id, descriptor.id) || control.failCreate) return nullptr;
    auto* self = new Instance;
    self->id = ++control.next;
    if (self->id >= control.calls.size()) { delete self; return nullptr; }
    self->plugin = {&descriptor, self, init, destroy, activate, nothing, start, nothing, reset, process, extension, nothing};
    return &self->plugin;
}
const clap_plugin_factory_t factory{count, getDescriptor, create};
bool entryInit(const char*) { return true; }
void entryDeinit() {}
const void* getFactory(const char* id) { return !std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &factory : nullptr; }
}
extern "C" CLAP_EXPORT RecoveryTestControl* recoveryTestControl() { return &control; }
extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry{CLAP_VERSION_INIT, entryInit, entryDeinit, getFactory};
