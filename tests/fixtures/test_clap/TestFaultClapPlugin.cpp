// Only loaded by the disposable-process tests. Deliberate failures are real
// plugin failures, never production host backdoors or environment switches.
#include <clap/clap.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
const char* features[]{CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, nullptr};
const clap_plugin_descriptor_t descriptors[]{
    {CLAP_VERSION_INIT, "com.daw.test.fault", "Fault test", "DAW", "", "", "", "1", "", features},
    {CLAP_VERSION_INIT, "com.daw.test.fault.create", "Create crash", "DAW", "", "", "", "1", "", features},
    {CLAP_VERSION_INIT, "com.daw.test.fault.activate", "Activate hang", "DAW", "", "", "", "1", "", features},
    {CLAP_VERSION_INIT, "com.daw.test.fault.destroy", "Destroy hang", "DAW", "", "", "", "1", "", features},
    {CLAP_VERSION_INIT, "com.daw.test.fault.editor", "Editor callbacks", "DAW", "", "", "", "1", "", features},
    {CLAP_VERSION_INIT, "com.daw.test.fault.editor_crash", "Editor crash", "DAW", "", "", "", "1", "", features},
    {CLAP_VERSION_INIT, "com.daw.test.fault.editor_hang", "Editor hang", "DAW", "", "", "", "1", "", features},
    {CLAP_VERSION_INIT, "com.daw.test.fault.recover_slow", "Slow recovery", "DAW", "", "", "", "1", "", features},
    {CLAP_VERSION_INIT, "com.daw.test.fault.state_slow", "Slow state import", "DAW", "", "", "", "1", "", features},
    {CLAP_VERSION_INIT, "com.daw.test.fault.editor_idle_hang", "Open editor hang", "DAW", "", "", "", "1", "", features},
    {CLAP_VERSION_INIT, "com.daw.test.fault.editor_modal", "Responsive modal editor", "DAW", "", "", "", "1", "", features},
    {CLAP_VERSION_INIT, "com.daw.test.fault.parameter_burst", "Parameter refresh burst", "DAW", "", "", "", "1", "", features},
    {CLAP_VERSION_INIT, "com.daw.test.fault.state_hang", "State import hang", "DAW", "", "", "", "1", "", features},
};
[[noreturn]] void hang() { for (;;) std::this_thread::sleep_for(std::chrono::seconds(1)); }
struct Instance {
    clap_plugin_t plugin{};
    unsigned kind = 0, mode = 0;
    double gain = 0.5;
    const clap_host_t* host = nullptr;
#ifdef _WIN32
    HWND editor = nullptr;
#endif
    static Instance& get(const clap_plugin_t* p) { return *static_cast<Instance*>(p->plugin_data); }
};
bool init(const clap_plugin_t*) { return true; }
void destroy(const clap_plugin_t* p) {
    if (Instance::get(p).kind == 3) hang();
    delete &Instance::get(p);
}
bool activate(const clap_plugin_t* p, double, uint32_t, uint32_t) {
    if (Instance::get(p).kind == 2) hang();
    if (Instance::get(p).kind == 7) std::this_thread::sleep_for(std::chrono::milliseconds(300));
    if (Instance::get(p).kind == 11) Instance::get(p).host->request_callback(Instance::get(p).host);
    return true;
}
void nothing(const clap_plugin_t*) {}
bool start(const clap_plugin_t*) { return true; }
uint32_t countPorts(const clap_plugin_t*, bool) { return 1; }
bool portInfo(const clap_plugin_t*, uint32_t index, bool, clap_audio_port_info_t* info) {
    if (index) return false;
    *info = {}; info->id = 0; info->channel_count = 2; info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->port_type = CLAP_PORT_STEREO; info->in_place_pair = CLAP_INVALID_ID;
    std::strcpy(info->name, "Stereo"); return true;
}
const clap_plugin_audio_ports_t ports{countPorts, portInfo};
uint32_t countParams(const clap_plugin_t* p) { return Instance::get(p).kind == 11 ? 4096 : 2; }
bool paramInfo(const clap_plugin_t* p, uint32_t index, clap_param_info_t* info) {
    if (index >= countParams(p)) return false;
    *info = {}; info->id = index; info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    info->min_value = 0; info->max_value = index ? 1 : 8;
    info->default_value = index ? 0.5 : 0;
    std::strcpy(info->name, index ? "Gain" : "Fault mode"); return true;
}
bool paramValue(const clap_plugin_t* p, clap_id id, double* value) {
    if (id >= countParams(p)) return false;
    *value = id ? Instance::get(p).gain : Instance::get(p).mode; return true;
}
void events(Instance& self, const clap_input_events_t* input) {
    if (!input) return;
    for (uint32_t i = 0; i < input->size(input); ++i) {
        const auto* h = input->get(input, i);
        if (!h || h->space_id != CLAP_CORE_EVENT_SPACE_ID || h->type != CLAP_EVENT_PARAM_VALUE) continue;
        const auto* e = reinterpret_cast<const clap_event_param_value_t*>(h);
        if (e->param_id == 0) self.mode = unsigned(e->value);
        if (e->param_id == 1) self.gain = e->value;
    }
}
void flush(const clap_plugin_t* p, const clap_input_events_t* input, const clap_output_events_t*) {
    events(Instance::get(p), input);
}
const clap_plugin_params_t params{countParams, paramInfo, paramValue, nullptr, nullptr, flush};
void notifyValues(Instance& self) {
    const auto* params = static_cast<const clap_host_params_t*>(self.host->get_extension(self.host, CLAP_EXT_PARAMS));
    if (params) params->rescan(self.host, CLAP_PARAM_RESCAN_VALUES);
}
bool save(const clap_plugin_t* p, const clap_ostream_t* stream) {
    auto& self = Instance::get(p);
    if (self.mode == 6) std::abort();
    if (self.mode == 7) return false;
    std::array<std::uint8_t, 32768> bytes{}; // larger than the test's 4 KiB mailbox
    std::memcpy(bytes.data(), &self.gain, sizeof(self.gain));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto n = stream->write(stream, bytes.data() + offset, bytes.size() - offset);
        if (n <= 0) return false;
        offset += std::size_t(n);
    }
    return true;
}
bool load(const clap_plugin_t* p, const clap_istream_t* stream) {
    auto& self = Instance::get(p);
    if (self.kind == 12) hang();
    if (self.kind == 8) std::this_thread::sleep_for(std::chrono::milliseconds(300));
    if (self.mode == 8) std::abort();
    std::array<std::uint8_t, 32768> bytes{};
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto n = stream->read(stream, bytes.data() + offset, bytes.size() - offset);
        if (n <= 0) return false;
        offset += std::size_t(n);
    }
    double gain;
    std::memcpy(&gain, bytes.data(), sizeof(gain));
    if (!std::isfinite(gain) || gain < 0 || gain > 1) return false;
    self.gain = gain; self.mode = 0;
    if (self.kind == 11) {
        self.gain = .125; notifyValues(self);
        self.gain = gain; notifyValues(self);
    }
    return true;
}
const clap_plugin_state_t state{save, load};
bool guiApi(const clap_plugin_t*, const char* api, bool floating) {
    return !floating && (!std::strcmp(api, CLAP_WINDOW_API_WIN32) || !std::strcmp(api, CLAP_WINDOW_API_COCOA));
}
bool guiCreate(const clap_plugin_t* p, const char*, bool) {
    if (Instance::get(p).kind == 5) std::abort();
    if (Instance::get(p).kind == 6) hang();
    return true;
}
bool guiSize(const clap_plugin_t*, uint32_t* width, uint32_t* height) { *width = 320; *height = 120; return true; }
void guiDestroy(const clap_plugin_t* p) {
#ifdef _WIN32
    auto& self = Instance::get(p);
    if (IsWindow(self.editor)) DestroyWindow(self.editor);
    self.editor = nullptr;
#endif
}
bool guiParent(const clap_plugin_t* p, const clap_window_t* window) {
    if (!window) return false;
#ifdef _WIN32
    if (!std::strcmp(window->api, CLAP_WINDOW_API_WIN32)) {
        guiDestroy(p);
        // Existing protocol-only tests use Qt's offscreen fake winId. Native
        // parenting is exercised separately by the Windows GUI regression.
        if (!IsWindow(static_cast<HWND>(window->win32))) return true;
        Instance::get(p).editor = CreateWindowExW(0, L"STATIC", L"DAW fixture editor",
            WS_CHILD | WS_VISIBLE, 0, 0, 320, 120, static_cast<HWND>(window->win32),
            nullptr, GetModuleHandleW(nullptr), nullptr);
        return Instance::get(p).editor != nullptr;
    }
#endif
    return true;
}
bool guiShow(const clap_plugin_t* p) {
    auto& self = Instance::get(p);
    self.gain = .75;
    const auto* hostParams = static_cast<const clap_host_params_t*>(self.host->get_extension(self.host, CLAP_EXT_PARAMS));
    if (hostParams) hostParams->rescan(self.host, CLAP_PARAM_RESCAN_VALUES);
    if (self.kind == 9 || self.kind == 10) self.host->request_callback(self.host);
    return true;
}
void mainThread(const clap_plugin_t* p) {
    auto& self = Instance::get(p);
    if (self.kind == 9) hang();
    if (self.kind == 11) {
        self.gain = .75; notifyValues(self);
        self.gain = .25; notifyValues(self);
    }
#ifdef _WIN32
    if (self.kind == 10) {
        // A vendor modal loop can remain inside on_main_thread for longer than
        // the watchdog timeout while continuing to dispatch native UI events.
        // No visible dialog or user interaction is needed by this fixture.
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < until) {
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        self.gain = .375;
        const auto* params = static_cast<const clap_host_params_t*>(self.host->get_extension(self.host, CLAP_EXT_PARAMS));
        if (params) params->rescan(self.host, CLAP_PARAM_RESCAN_VALUES);
    }
#endif
}
bool guiHide(const clap_plugin_t*) { return true; }
const clap_plugin_gui_t gui{guiApi, nullptr, guiCreate, guiDestroy, nullptr, guiSize,
    nullptr, nullptr, nullptr, nullptr, guiParent, nullptr, nullptr, guiShow, guiHide};
clap_process_status process(const clap_plugin_t* p, const clap_process_t* b) {
    auto& self = Instance::get(p);
    events(self, b->in_events);
    if (self.mode == 1) std::abort();
    if (self.mode == 2) hang();
    if (self.mode == 4) return CLAP_PROCESS_ERROR;
    if (self.mode == 5) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (b->audio_outputs_count) for (uint32_t ch = 0; ch < b->audio_outputs[0].channel_count; ++ch) {
        float* out = b->audio_outputs[0].data32[ch];
        const float* in = b->audio_inputs_count && ch < b->audio_inputs[0].channel_count
            ? b->audio_inputs[0].data32[ch] : nullptr;
        for (uint32_t i = 0; i < b->frames_count; ++i)
            out[i] = self.mode == 3 ? std::numeric_limits<float>::quiet_NaN() : float((in ? in[i] : 0) * self.gain);
    }
    return CLAP_PROCESS_CONTINUE;
}
const void* extension(const clap_plugin_t* p, const char* id) {
    const auto kind = Instance::get(p).kind;
    if (!std::strcmp(id, CLAP_EXT_GUI) && ((kind >= 4 && kind <= 6) || kind == 9 || kind == 10)) return &gui;
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &ports;
    if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &params;
    if (!std::strcmp(id, CLAP_EXT_STATE)) return &state;
    return nullptr;
}
uint32_t count(const clap_plugin_factory_t*) { return std::size(descriptors); }
const clap_plugin_descriptor_t* descriptor(const clap_plugin_factory_t*, uint32_t i) {
    return i < std::size(descriptors) ? &descriptors[i] : nullptr;
}
const clap_plugin_t* create(const clap_plugin_factory_t*, const clap_host_t* host, const char* id) {
    for (unsigned i = 0; i < std::size(descriptors); ++i) if (!std::strcmp(id, descriptors[i].id)) {
        if (i == 1) std::abort();
        auto* s = new Instance; s->kind = i; s->host = host;
        s->plugin = {&descriptors[i], s, init, destroy, activate, nothing, start, nothing,
            nothing, process, extension, mainThread};
        return &s->plugin;
    }
    return nullptr;
}
const clap_plugin_factory_t factory{count, descriptor, create};
bool entryInit(const char*) { return true; }
void entryExit() {}
const void* getFactory(const char* id) { return !std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &factory : nullptr; }
}
extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry{CLAP_VERSION_INIT, entryInit, entryExit, getFactory};
