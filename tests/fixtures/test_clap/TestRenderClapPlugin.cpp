// Fault-injection CLAP fixture for complete export regression tests.
#include <clap/clap.h>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <vector>

namespace {
const char* features[]{CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, nullptr};
const clap_plugin_descriptor_t descriptors[]{
    {CLAP_VERSION_INIT, "review.latency", "Offline latency", "Review", "", "", "", "1", "", features},
    {CLAP_VERSION_INIT, "review.activation", "Offline activation failure", "Review", "", "", "", "1", "", features},
    {CLAP_VERSION_INIT, "review.process", "Offline process failure", "Review", "", "", "", "1", "", features},
    {CLAP_VERSION_INIT, "review.callback", "Main thread callback", "Review", "", "", "", "1", "", features},
    {CLAP_VERSION_INIT, "review.clone", "Clone creation failure", "Review", "", "", "", "1", "", features},
    {CLAP_VERSION_INIT, "review.dual", "Dual mono clone failure", "Review", "", "", "", "1", "", features},
    {CLAP_VERSION_INIT, "review.prepare", "Deferred latency setup", "Review", "", "", "", "1", "", features},
    {CLAP_VERSION_INIT, "review.restart", "Restart during render", "Review", "", "", "", "1", "", features},
    {CLAP_VERSION_INIT, "review.mode", "Offline mode refusal", "Review", "", "", "", "1", "", features},
    {CLAP_VERSION_INIT, "review.hardware", "Hard realtime requirement", "Review", "", "", "", "1", "", features},
    {CLAP_VERSION_INIT, "review.unstable", "Repeated preparation restart", "Review", "", "", "", "1", "", features},
    {CLAP_VERSION_INIT, "review.audio-latency", "Latency discovered from audio", "Review", "", "", "", "1", "", features},
    {CLAP_VERSION_INIT, "review.once-restart", "One deferred restart", "Review", "", "", "", "1", "", features},
};
constexpr unsigned descriptorCount = sizeof(descriptors) / sizeof(descriptors[0]);
unsigned instances[descriptorCount]{};
struct Instance {
    clap_plugin_t plugin{};
    const clap_host_t* host{};
    std::thread::id controlThread;
    int kind{};
    bool offline{}, callbackDone{}, configured{};
    bool active{};
    bool wrongCallbackThread{};
    unsigned latency{}, position{}, blocks{};
    std::vector<float> ring;
    static Instance& get(const clap_plugin_t* plugin) {
        return *static_cast<Instance*>(plugin->plugin_data);
    }
};
uint32_t portCount(const clap_plugin_t*, bool) { return 1; }
bool portInfo(const clap_plugin_t*, uint32_t index, bool, clap_audio_port_info_t* info) {
    if (index) return false;
    *info = {}; info->id = 0; info->channel_count = 2;
    info->flags = CLAP_AUDIO_PORT_IS_MAIN; info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}
const clap_plugin_audio_ports_t ports{portCount, portInfo};
bool hardRealtime(const clap_plugin_t* p) { return Instance::get(p).kind == 9; }
bool setRender(const clap_plugin_t* plugin, clap_plugin_render_mode mode) {
    if (Instance::get(plugin).kind == 8 && mode == CLAP_RENDER_OFFLINE) return false;
    Instance::get(plugin).offline = mode == CLAP_RENDER_OFFLINE;
    return true;
}
const clap_plugin_render_t render{hardRealtime, setRender};
uint32_t latencyGet(const clap_plugin_t* plugin) { return Instance::get(plugin).latency; }
const clap_plugin_latency_t latencyExt{latencyGet};
bool init(const clap_plugin_t*) { return true; }
void destroy(const clap_plugin_t* p) { --instances[Instance::get(p).kind]; delete &Instance::get(p); }
bool activate(const clap_plugin_t* p, double, uint32_t, uint32_t) {
    auto& self = Instance::get(p);
    if (self.kind == 1 && self.offline) return false;
    self.active = true;
    self.latency = self.offline && (self.kind == 0 || ((self.kind == 6 || self.kind == 11) && self.configured)) ? 24 : 0;
    if (self.offline && self.kind == 6 && !self.configured) self.host->request_callback(self.host);
    if (self.offline && self.kind == 10) self.host->request_restart(self.host);
    self.ring.assign(2 * self.latency, 0.f);
    self.position = self.blocks = 0;
    return true;
}
void deactivate(const clap_plugin_t* p) { Instance::get(p).active = false; }
bool start(const clap_plugin_t*) { return true; }
void stop(const clap_plugin_t*) {}
void reset(const clap_plugin_t* p) {
    auto& self = Instance::get(p);
    if (!self.active) std::abort(); // enforce CLAP's reset lifecycle contract
    std::fill(self.ring.begin(), self.ring.end(), 0.f);
    self.position = self.blocks = 0;
    self.callbackDone = false;
}
clap_process_status process(const clap_plugin_t* p, const clap_process_t* block) {
    auto& self = Instance::get(p);
    if (self.wrongCallbackThread) return CLAP_PROCESS_ERROR;
    const unsigned number = self.blocks++;
    // Keep this offline pass alive beyond several normal UI slices. The test
    // does not rely on receiving a callback within any exact sample interval.
    if (self.kind == 3 && self.offline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (self.kind == 2 && self.offline && (number / 64) % 2) return CLAP_PROCESS_ERROR;
    if (self.kind == 3 && number == 0) self.host->request_callback(self.host);
    if (self.kind == 7 && self.offline && number == 16) self.host->request_restart(self.host);
    if (self.offline && !self.configured &&
        ((self.kind == 11 && number == 1) || (self.kind == 12 && number == 16))) {
        self.configured = true;
        self.host->request_restart(self.host);
    }
    // Model deferred setup: the requested main-thread turn enables full level.
    const float gain = self.kind == 1 ? 0.5f
                     : self.kind == 3 && !self.callbackDone && number > 0 ? 0.25f : 1.f;
    for (uint32_t i = 0; i < block->frames_count; ++i) {
        for (unsigned ch = 0; ch < 2; ++ch) {
            const float input = block->audio_inputs[0].data32[ch][i];
            float output = input;
            if (self.latency) {
                auto& slot = self.ring[ch * self.latency + self.position];
                output = slot; slot = input;
            }
            block->audio_outputs[0].data32[ch][i] = output * gain;
        }
        if (self.latency) self.position = (self.position + 1) % self.latency;
    }
    return CLAP_PROCESS_CONTINUE;
}
const void* extension(const clap_plugin_t*, const char* id) {
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &ports;
    if (!std::strcmp(id, CLAP_EXT_RENDER)) return &render;
    if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &latencyExt;
    return nullptr;
}
void mainThread(const clap_plugin_t* p) {
    auto& self = Instance::get(p); self.callbackDone = true;
    self.wrongCallbackThread |= std::this_thread::get_id() != self.controlThread;
    if (self.kind == 6 && !self.configured) {
        self.configured = true; self.host->request_restart(self.host);
    }
}
uint32_t count(const clap_plugin_factory_t*) { return descriptorCount; }
const clap_plugin_descriptor_t* descriptor(const clap_plugin_factory_t*, uint32_t index) {
    return index < descriptorCount ? &descriptors[index] : nullptr;
}
const clap_plugin_t* create(const clap_plugin_factory_t*, const clap_host_t* host, const char* id) {
    unsigned kind = 0;
    while (kind < descriptorCount && std::strcmp(id, descriptors[kind].id)) ++kind;
    if (kind == descriptorCount) return nullptr;
    if ((kind == 4 && instances[kind] >= 1) || (kind == 5 && instances[kind] >= 3)) return nullptr;
    ++instances[kind];
    auto* self = new Instance;
    self->host = host; self->kind = kind;
    self->controlThread = std::this_thread::get_id();
    self->plugin = {&descriptors[kind], self, init, destroy, activate, deactivate,
                    start, stop, reset, process, extension, mainThread};
    return &self->plugin;
}
const clap_plugin_factory_t factory{count, descriptor, create};
bool entryInit(const char*) { return true; }
void entryDeinit() {}
const void* getFactory(const char* id) {
    return !std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &factory : nullptr;
}
}
extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry{
    CLAP_VERSION_INIT, entryInit, entryDeinit, getFactory};
