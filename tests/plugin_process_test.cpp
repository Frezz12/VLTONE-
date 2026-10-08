#include "PluginProcess.hpp"
#include "ProcessPluginInstance.hpp"
#include "PluginProcessProtocol.hpp"
#include "SharedProcess.hpp"
#include "Clap/ClapFactory.hpp"
#include "platform/PathUtils.hpp"
#include "RealtimeMetrics.hpp"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <new>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif

using namespace daw::plugins;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
namespace {
int failures = 0;
void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++failures;
}
PluginDescriptor descriptor(std::string uid = "com.daw.test.fault") {
    PluginDescriptor d;
    d.format = Format::Clap; d.path = DAW_FAULT_CLAP_PATH; d.uid = std::move(uid); d.name = "Fault test";
    return d;
}
PluginProcessLimits limits() { return {128, 8, 4096, 4096}; }
struct Block {
    std::array<float, 128> left, right, outputLeft{}, outputRight{};
    const float* inputs[2]{left.data(), right.data()};
    float* outputs[2]{outputLeft.data(), outputRight.data()};
    PluginProcessContext context;
    Block() {
        left.fill(1); right.fill(0.5);
        context.inputs = inputs; context.inputChannels = 2;
        context.outputs = outputs; context.outputChannels = 2; context.frames = 128;
        context.playing = true; context.steadyTime = 0;
    }
};
bool ready(PluginProcess& p, PluginDescriptor d = descriptor()) {
    const bool ok = p.load(d) && p.activate({48000, 128, false, false}) && p.startProcessing();
    if (!ok) std::fprintf(stderr, "prepare: %s\n", p.error().c_str());
    return ok;
}
PluginProcess::Poll finish(PluginProcess& p, Block& b, std::chrono::milliseconds budget = 2s) {
    const auto deadline = Clock::now() + budget;
    PluginProcessDisposition disposition;
    for (;;) {
        const auto status = p.poll(b.context, disposition);
        if (status != PluginProcess::Poll::Pending) return status;
        if (Clock::now() >= deadline) { p.expire(); return p.poll(b.context, disposition); }
        std::this_thread::sleep_for(1ms); // test driver, never a realtime callback
    }
}
bool process(PluginProcess& p, Block& b) {
    return p.submit(b.context) && finish(p, b) == PluginProcess::Poll::Complete;
}
PluginEvent parameter(unsigned index, double value, unsigned frame = 0) {
    PluginEvent e; e.paramIndex = index; e.value = value; e.frameOffset = frame; return e;
}
void fault(PluginProcess& p, Block& b, double mode) {
    const auto e = parameter(0, mode); b.context.inputEvents = std::span(&e, 1);
    check(p.submit(b.context), "submit faulting DSP"); b.context.inputEvents = {};
}
bool silent(const Block& b) {
    return std::all_of(b.outputLeft.begin(), b.outputLeft.end(), [](float x) { return x == 0; }) &&
        std::all_of(b.outputRight.begin(), b.outputRight.end(), [](float x) { return x == 0; });
}

struct Events final : EventSink {
    std::array<PluginEvent, 32> values;
    unsigned count = 0;
    void push(const PluginEvent& e) noexcept override { if (count < values.size()) values[count++] = e; }
};

// A controlled peer supplies exact completion timestamps. A separate marker is
// published after the response, so the regression never relies on sleep timing
// or on the scheduler finishing a real process inside a test-sized deadline.
int deadlinePeer(ipc::SharedProcess& channel, bool invalidClock) {
    using Json = nlohmann::json;
    auto& h = *reinterpret_cast<ipc::Header*>(channel.data());
    const ipc::View view{channel.data(), ipc::Layout(h.limits)};
    Json metadata;
    std::string marker;
    std::uint64_t control = 0, audio = 0;
    while (channel.wait(1)) {
        const auto request = h.controlRequest.load(std::memory_order_acquire);
        if (request != control) {
            control = request;
            const auto command = Json::parse(view.text(), view.text() + h.textBytes);
            const auto op = command.at("op").get<std::string>();
            Json response{{"ok", true}};
            if (op == "hello") {
                response["version"] = ipc::kVersion;
                response["clockNanos"] = invalidClock ? std::uint64_t{0} : daw::rt::nowNanos();
            } else if (op == "load") {
                marker = command.at("descriptor").at("path").get<std::string>();
                metadata = {{"descriptor", command.at("descriptor")},
                    {"inputs", {2}}, {"outputs", {2}}, {"parameters", Json::array()},
                    {"latency", 0}, {"tail", 0}, {"tailKnown", true}, {"supportsState", false},
                    {"active", false}, {"processing", false}, {"realtimeReset", true}, {"hasEditor", false},
                    {"pitch", {{"perNote", false}, {"mpe", false}, {"bend", false}, {"continuous", false}}}};
            } else if (op == "activate") metadata["active"] = true;
            else if (op == "start") metadata["processing"] = true;
            else if (op == "stop") metadata["processing"] = false;
            else if (op == "deactivate") metadata["active"] = metadata["processing"] = false;
            if (!metadata.is_null()) response["metadata"] = metadata;
            const auto text = response.dump();
            std::memcpy(view.text(), text.data(), text.size()); h.textBytes = std::uint32_t(text.size());
            h.response.store(request, std::memory_order_release);
            if (op == "quit") return 0;
        }
        const auto requestedAudio = h.audioRequest.load(std::memory_order_acquire);
        if (requestedAudio != audio) {
            audio = requestedAudio;
            for (unsigned ch = 0; ch < h.block.outputs; ++ch)
                std::fill_n(view.channel(2, ch), h.block.frames, 0.75f);
            h.block.disposition = std::uint32_t(PluginProcessDisposition::Continue);
            h.completedNanos = std::uint64_t(h.block.steadyTime);
            h.response.store(audio, std::memory_order_release);
            std::ofstream(daw::platform::pathFromUtf8(marker)).put('1');
        }
    }
    return 1;
}

void deadlineRegression(const std::string& selfPath) {
    const auto environment = [](const char* value) {
#ifdef _WIN32
        _putenv_s("DAW_TEST_PROCESS_DEADLINE_PEER", value);
#else
        if (*value) setenv("DAW_TEST_PROCESS_DEADLINE_PEER", value, 1);
        else unsetenv("DAW_TEST_PROCESS_DEADLINE_PEER");
#endif
    };
    environment("clock");
    PluginProcess wrongClock(selfPath, limits(), 2s);
    check(!wrongClock.load(descriptor()) && wrongClock.failure() == PluginProcessFailure::Protocol,
          "peer with a different monotonic epoch is rejected during handshake");
    environment("audio");
    for (const std::uint64_t completed : {999ull, 1000ull, 1001ull}) {
        const auto marker = std::filesystem::temp_directory_path() / ("daw-deadline-" +
            std::to_string(daw::rt::nowNanos()) + ".ready");
        auto d = descriptor(); d.path = daw::platform::pathToUtf8(marker);
        auto instance = ProcessPluginInstance::create(d, selfPath);
        check(instance && instance->activate({48000, 128}), "prepare controlled completion peer");
        if (!instance || !instance->isActive()) continue;
        instance->startProcessing();
        Block block; block.context.deadlineNanos = 1000; block.context.steadyTime = std::int64_t(completed);
        PluginProcessDisposition disposition;
        check(!instance->beginProcess(block.context, disposition), "submit controlled completion block");
        const auto waitUntil = Clock::now() + 3s;
        while (!std::filesystem::exists(marker) && Clock::now() < waitUntil) std::this_thread::sleep_for(1ms);
        check(std::filesystem::exists(marker), "child published response before parent polls");
        const bool onTime = completed < block.context.deadlineNanos;
        check(instance->finishProcess(block.context, disposition, true) &&
                  (disposition != PluginProcessDisposition::Error) == onTime &&
                  instance->failure() == (onTime ? PluginProcessFailure::None : PluginProcessFailure::AudioDeadline) &&
                  (onTime ? block.outputLeft.back() == .75f : silent(block)),
              onTime ? "late parent poll preserves a response completed before deadline"
                     : "a response completed at or after deadline fails even when already published");
        instance.reset();
        std::error_code ignored; std::filesystem::remove(marker, ignored);
    }
    environment("");
}

int benchmark() {
    PluginDescriptor d;
    d.format = Format::Clap; d.path = DAW_TEST_CLAP_PATH; d.uid = "com.daw.test.gain";
    PluginProcess remote(DAW_PLUGIN_HOST_PATH, limits(), 2s);
    ClapFactory factory; auto local = factory.create(d);
    if (!local || !ready(remote, d)) return 1;
    local->activate({48000, 128}); local->startProcessing();
    Block block;
    std::array<double, 2000> direct{}, isolated{};
    for (unsigned i = 0; i < direct.size() + 64; ++i) {
        block.context.sampleTime = block.context.steadyTime = std::int64_t(i) * 128;
        const auto start = Clock::now();
        local->process(block.context);
        const auto middle = Clock::now();
        if (!remote.submit(block.context)) return 1;
        PluginProcessDisposition disposition;
        PluginProcess::Poll status;
        do {
            status = remote.poll(block.context, disposition);
            if (Clock::now() - middle > 1s) { remote.expire(); return 1; }
        } while (status == PluginProcess::Poll::Pending);
        if (status != PluginProcess::Poll::Complete) return 1;
        const auto end = Clock::now();
        if (i >= 64) {
            direct[i - 64] = std::chrono::duration<double, std::micro>(middle - start).count();
            isolated[i - 64] = std::chrono::duration<double, std::micro>(end - middle).count();
        }
    }
    const auto report = [](auto values, const char* label) {
        std::sort(values.begin(), values.end());
        std::printf("%s microseconds: median=%.2f p99=%.2f p99.9=%.2f max=%.2f\n",
            label, values[999], values[1979], values[1997], values.back());
    };
    std::puts("HEADLESS IPC MICROBENCHMARK: 2000 blocks, 128 frames, 48000 Hz, one gain plugin.");
    std::puts("Normal-priority caller; child uses AudioWorkerRegistration. No device or xrun certification.");
    report(direct, "in-process"); report(isolated, "isolated round trip");
    std::printf("Shared mapping: %zu bytes\n", ipc::Layout(limits()).bytes);
    local->stopProcessing(); local->deactivate();
    return 0;
}
}

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--benchmark") == 0) return benchmark();
#ifdef _WIN32
    if (argc == 2 && std::strcmp(argv[1], "--automation-keyboard") == 0) {
        PluginProcess remote(DAW_PLUGIN_HOST_PATH, limits(), 3s);
        if (!ready(remote, descriptor("com.daw.test.fault.editor"))) return 1;
        remote.setAutomationShortcutEnabled(true);
        remote.requestEditor(true);
        const auto deadline = Clock::now() + 3s;
        while (remote.editorStatus() != 1 && remote.editorStatus() != 2 && Clock::now() < deadline)
            std::this_thread::sleep_for(5ms);
        struct Search { DWORD pid; HWND window = nullptr; } search{DWORD(remote.processId())};
        EnumWindows([](HWND window, LPARAM data) -> BOOL {
            auto& search = *reinterpret_cast<Search*>(data);
            DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
            if (pid == search.pid && IsWindowVisible(window)) { search.window = window; return FALSE; }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&search));
        check(remote.editorStatus() == 1 && search.window, "isolated editor has a native window");
        const auto collect = [&] {
            std::uint32_t count = 0;
            const auto end = Clock::now() + 150ms;
            while (Clock::now() < end) { count += remote.takeAutomationShortcuts(); std::this_thread::sleep_for(5ms); }
            return count;
        };
        const auto press = [&](bool repeat) {
            PostMessageW(search.window, WM_KEYDOWN, 'A', 1 | (0x1e << 16) | (repeat ? (1u << 30) : 0));
        };
        if (search.window) {
            press(false); press(true);
            PostMessageW(search.window, WM_KEYUP, 'A', 1 | (0x1e << 16) | (3u << 30));
            check(collect() == 1, "isolated A reaches the host once despite key repeat");
            remote.setAutomationShortcutEnabled(false);
            (void)collect();
            press(false);
            check(collect() == 0, "disabled A remains with the isolated plugin");
            remote.setAutomationShortcutEnabled(true);
            (void)collect();
            press(false); press(false);
            check(collect() == 2, "two quick A presses survive the process mailbox");
        }
        remote.requestEditor(false);
        return failures;
    }
#endif
    // A hostile protocol peer is a test executable, not a production host mode.
    if (argc > 1) {
        ipc::SharedProcess child;
        std::string error;
        if (!child.attach(argc, argv, error)) return 2;
        if (const auto* mode = std::getenv("DAW_TEST_PROCESS_DEADLINE_PEER"))
            return deadlinePeer(child, std::strcmp(mode, "clock") == 0);
        auto& h = *reinterpret_cast<ipc::Header*>(child.data());
        if (h.reserved == 1) {
            PluginProcess grandchild(DAW_PLUGIN_HOST_PATH, limits(), 2s);
            if (!ready(grandchild)) std::_Exit(4);
            h.block.sampleTime = std::int64_t(grandchild.processId());
            h.response.store(1, std::memory_order_release);
            child.wait();
            std::_Exit(0); // deliberately bypass the process owner's destructor
        }
        if (!child.wait()) return 3;
        const auto seq = h.request.load(std::memory_order_acquire);
        h.textBytes = std::numeric_limits<std::uint32_t>::max();
        h.response.store(seq, std::memory_order_release);
        child.wait();
        std::_Exit(0);
    }
    PluginProcess missing("this-plugin-host-does-not-exist", limits(), 500ms);
    check(!missing.load(descriptor()) && missing.failure() == PluginProcessFailure::Launch,
          "missing helper fails without an in-process fallback");
    std::string creationError;
    check(!ProcessPluginInstance::create(descriptor(), "this-plugin-host-does-not-exist", &creationError) &&
        creationError.find("plugin host could not start") != std::string::npos,
        "failed slot construction preserves the concrete host error for the caller");
    PluginProcess retiredClass(DAW_PLUGIN_HOST_PATH, limits(), 2s);
    check(!retiredClass.load(descriptor("com.daw.test.removed-version")) &&
        retiredClass.error().find("installed module does not contain plugin class") != std::string::npos &&
        retiredClass.error().find("Fault test") != std::string::npos,
        "a stale plugin identity reports the installed alternatives without substituting another class");
    {
        unsigned outer = 0, inner = 0;
        const auto observed = [](void* count, std::chrono::nanoseconds, bool) noexcept {
            ++*static_cast<unsigned*>(count);
        };
        {
            ScopedPluginControlProgress scope(&outer, observed);
            try {
                ScopedPluginControlProgress nested(&inner, observed);
                ScopedPluginControlProgress::report(1ms, false);
                throw 1;
            } catch (int) {}
            ScopedPluginControlProgress::report({}, true);
            std::thread([] { ScopedPluginControlProgress::report(1ms, false); }).join();
        }
        ScopedPluginControlProgress::report({}, true);
        check(outer == 1 && inner == 1,
            "nested control progress restores its scope after exceptions and never reaches another thread");
    }
    const auto selfPath = daw::platform::pathToUtf8(std::filesystem::absolute(daw::platform::pathFromUtf8(argv[0])));
    deadlineRegression(selfPath);
    {
        PluginProcess many(DAW_PLUGIN_HOST_PATH, {128, 8, 8192, 2 * 1024 * 1024}, 3s);
        check(ready(many, descriptor("com.daw.test.fault.parameter_burst")), "load plugin with 4096 parameters");
        const auto drain = [&](double expected) {
            std::vector<double> values(4096, -1);
            const auto deadline = Clock::now() + 3s;
            while (Clock::now() < deadline && many.failure() == PluginProcessFailure::None) {
                PluginEvent event;
                while (many.popNotification(event))
                    if (event.kind == PluginEvent::Kind::ParamValue && event.paramIndex < values.size())
                        values[event.paramIndex] = event.value;
                if (values[0] == 0 && std::all_of(values.begin() + 1, values.end(),
                    [&](double value) { return value == expected; })) return true;
                std::this_thread::sleep_for(2ms);
            }
            return false;
        };
        check(drain(.25), "full parameter refresh larger than notice ring preserves every latest value");
        std::vector<std::uint8_t> preset;
        check(many.saveState(preset) && preset.size() >= sizeof(double), "capture parameter burst preset");
        for (const double gain : {.5, .875}) {
            if (preset.size() < sizeof(double)) break;
            std::memcpy(preset.data(), &gain, sizeof(gain));
            check(many.loadState(preset) && drain(gain), "repeated preset bursts preserve all parameters without failing isolation");
        }
        Block block;
        check(many.failure() == PluginProcessFailure::None && process(many, block) && block.outputLeft.back() == .875f,
            "plugin still processes after parameter notification backpressure");
    }
    PluginProcess hostile(selfPath, limits(), 1s);
    check(!hostile.load(descriptor()) && hostile.failure() == PluginProcessFailure::Protocol,
          "oversized response rejected before parsing shared memory");

    {
        ipc::SharedProcess child;
        const ipc::Layout layout(limits()); std::string error;
        check(child.create(layout.bytes, error), "create private protocol mapping");
        auto* h = ::new (child.data()) ipc::Header{};
        h->limits = limits(); h->version = 999;
        check(child.launch(DAW_PLUGIN_HOST_PATH, error), "launch mismatched peer");
        const auto deadline = Clock::now() + 2s;
        while (child.running() && Clock::now() < deadline) std::this_thread::sleep_for(1ms);
        check(!child.running(), "incompatible wire version exits before loading native code");
    }

    {
        ipc::SharedProcess parent;
        const ipc::Layout layout(limits()); std::string error;
        if (!parent.create(layout.bytes, error)) return 1;
        auto* h = ::new (parent.data()) ipc::Header{};
        h->limits = limits(); h->reserved = 1;
        if (!parent.launch(selfPath, error)) return 1;
        const auto deadline = Clock::now() + 3s;
        while (h->response.load(std::memory_order_acquire) != 1 && parent.running() && Clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        const auto pid = h->block.sampleTime;
        check(pid > 0, "nested parent owns a live plugin process");
#ifdef _WIN32
        HANDLE handle = ::OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid));
        check(handle != nullptr, "observe child before parent exits");
#endif
        parent.signal();
#ifdef _WIN32
        check(handle && ::WaitForSingleObject(handle, 3000) == WAIT_OBJECT_0,
              "parent death terminates plugin without destructor cleanup");
        if (handle) ::CloseHandle(handle);
#else
        const auto deathDeadline = Clock::now() + 3s;
        while (pid > 0 && ::kill(pid_t(pid), 0) == 0 && Clock::now() < deathDeadline)
            std::this_thread::sleep_for(1ms);
        check(pid > 0 && ::kill(pid_t(pid), 0) != 0 && errno == ESRCH,
              "parent death terminates plugin without destructor cleanup");
#endif
    }

    PluginProcess first(DAW_PLUGIN_HOST_PATH, limits(), 2s), second(DAW_PLUGIN_HOST_PATH, limits(), 2s);
    if (!ready(first) || !ready(second)) return 1;
    check(first.processId() != second.processId(), "same DLL has independent process instances");
    Block a, b;
    check(process(first, a) && a.outputLeft[127] == 0.5f && a.outputRight[127] == 0.25f,
          "real CLAP DSP crosses private audio transport");
    auto gain = parameter(1, 0.25); a.context.inputEvents = std::span(&gain, 1);
    check(process(first, a) && a.outputLeft[127] == 0.25f, "parameter event reaches isolated processor");
    a.context.inputEvents = {};
    std::vector<std::uint8_t> saved;
    check(first.saveState(saved) && saved.size() == 32768 && first.hasCheckpoint(),
          "opaque checkpoint spans multiple bounded control messages");
    const auto oldPid = first.processId();
    fault(first, a, 1);
    const auto crashDeadline = Clock::now() + 2s;
    while (first.service() && Clock::now() < crashDeadline) std::this_thread::sleep_for(1ms);
    PluginProcessDisposition disposition;
    check(first.failure() == PluginProcessFailure::Exited && first.poll(a.context, disposition) == PluginProcess::Poll::Failed && silent(a),
          "native abort is confined to its process and returns defined silence");
    check(process(second, b) && b.outputLeft[127] == 0.5f, "independent instance survives the other plugin crash");
    check(first.restart() && first.processId() != oldPid && process(first, a) && a.outputLeft[127] == 0.25f,
          "restart creates a new generation and restores confirmed state");

    fault(first, a, 2);
    check(finish(first, a, 20ms) == PluginProcess::Poll::Failed && silent(a) &&
          first.failure() == PluginProcessFailure::AudioDeadline, "hung DSP expires without waiting for its return");
    check(!first.submit(a.context), "expired mapping cannot be reused");
    check(process(second, b), "healthy process continues while another is hung");
    const auto reapStart = Clock::now();
    check(!first.service() && Clock::now() - reapStart < 2s, "control thread terminates hung child");
    check(first.restart() && process(first, a) && a.outputLeft[127] == 0.25f, "restart after hang preserves checkpoint");

    fault(first, a, 5);
    first.expire();
    std::this_thread::sleep_for(150ms);
    check(first.poll(a.context, disposition) == PluginProcess::Poll::Failed && silent(a), "late result is never accepted after expiry");
    check(first.restart(), "fresh mapping after late response");
    for (double mode : {3., 4.}) {
        fault(first, a, mode);
        check(finish(first, a) == PluginProcess::Poll::Failed && silent(a), "NaN and process error never reach the output");
        check(first.restart(), "restart after invalid output");
    }
    fault(first, a, 7);
    check(finish(first, a) == PluginProcess::Poll::Complete, "save-failure mode still processes audio");
    const auto previous = saved;
    check(!first.saveState(saved) && saved == previous && first.hasCheckpoint(), "failed save preserves caller bytes and previous checkpoint");
    check(first.restart() && process(first, a) && a.outputLeft[127] == 0.25f, "failed save does not replace recoverable state");
    fault(first, a, 6);
    check(finish(first, a) == PluginProcess::Poll::Complete, "prepare save crash");
    check(!first.saveState(saved) && first.failure() == PluginProcessFailure::Exited && saved == previous,
          "crash during state capture is contained");
    check(first.restart() && process(first, a), "restore after crashed state capture");
    fault(first, a, 8);
    check(finish(first, a) == PluginProcess::Poll::Complete, "prepare state-load crash");
    check(!first.loadState(saved) && first.failure() == PluginProcessFailure::Exited && first.hasCheckpoint(),
          "crash during state restore preserves the confirmed checkpoint");
    check(first.restart() && process(first, a) && a.outputLeft[127] == 0.25f,
          "new process recovers after a crashed state restore");

    a.context.frames = 129;
    check(!first.submit(a.context) && first.failure() == PluginProcessFailure::InvalidBlock, "oversized block rejected before reading input");
    a.context.frames = 128;
    check(first.restart(), "recover after rejected block");
    check(first.submit(a.context) && !first.submit(a.context), "only one block can own a mailbox");
    check(!first.refreshMetadata(), "control cannot overlap outstanding DSP");
    check(finish(first, a) == PluginProcess::Poll::Complete, "rejected overlap preserves the pending block");
    const auto invalid = parameter(0, 1, 128); a.context.inputEvents = std::span(&invalid, 1);
    check(!first.submit(a.context), "event outside block rejected"); a.context.inputEvents = {};

    PluginProcess createCrash(DAW_PLUGIN_HOST_PATH, limits(), 1s);
    check(!createCrash.load(descriptor("com.daw.test.fault.create")) && createCrash.failure() == PluginProcessFailure::Exited,
          "crash in factory does not escape child");
    PluginProcess activateHang(DAW_PLUGIN_HOST_PATH, limits(), 200ms);
    check(activateHang.load(descriptor("com.daw.test.fault.activate")), "load activation-hang fixture");
    const auto timeoutStart = Clock::now();
    check(!activateHang.activate({48000, 128}) && activateHang.failure() == PluginProcessFailure::ControlTimeout &&
          Clock::now() - timeoutStart < 2s, "control operation has a bounded timeout");
    activateHang.close();
    PluginProcess destroyHang(DAW_PLUGIN_HOST_PATH, limits(), 1s);
    check(ready(destroyHang, descriptor("com.daw.test.fault.destroy")), "load destruction-hang fixture");
    const auto closeStart = Clock::now(); destroyHang.close();
    check(Clock::now() - closeStart < 2s, "hung destructor cannot hold parent shutdown");

    // Compare a real format adapter on both sides, including its 64-frame
    // latency, sample-accurate parameter change and host transport context.
    PluginDescriptor gainDescriptor;
    gainDescriptor.format = Format::Clap; gainDescriptor.path = DAW_TEST_CLAP_PATH;
    gainDescriptor.uid = "com.daw.test.gain";
    ClapFactory factory;
    auto local = factory.create(gainDescriptor);
    PluginProcess remote(DAW_PLUGIN_HOST_PATH, limits(), 2s);
    check(local && ready(remote, gainDescriptor), "load reference gain plugin through both paths");
    if (local) {
        local->activate({48000, 128}); local->startProcessing();
        Block reference, isolated;
        auto event = parameter(0, 0.25, 37);
        reference.context.inputEvents = isolated.context.inputEvents = std::span(&event, 1);
        for (int n = 0; n < 4; ++n) {
            reference.context.sampleTime = isolated.context.sampleTime = n * 128;
            reference.context.steadyTime = isolated.context.steadyTime = n * 128;
            local->process(reference.context);
            check(process(remote, isolated) && reference.outputLeft == isolated.outputLeft &&
                  reference.outputRight == isolated.outputRight, "local and process audio are sample-identical");
            reference.context.inputEvents = isolated.context.inputEvents = {};
        }
        check(remote.metadata().latency == local->latencySamples(), "reported plugin latency survives transport");
        local->stopProcessing(); local->deactivate();
    }
    gainDescriptor.uid = "com.daw.test.tone";
    PluginProcess instrument(DAW_PLUGIN_HOST_PATH, limits(), 2s);
    check(ready(instrument, gainDescriptor), "load isolated MIDI instrument");
    Block notes; Events emitted;
    notes.context.inputChannels = 0; notes.context.inputs = nullptr; notes.context.outputEvents = &emitted;
    std::array<PluginEvent, 2> midi;
    midi[0].kind = PluginEvent::Kind::NoteOn; midi[0].frameOffset = 11; midi[0].value = 0.75;
    midi[1].kind = PluginEvent::Kind::NoteOff; midi[1].frameOffset = 93;
    notes.context.inputEvents = midi;
    check(process(instrument, notes) && notes.outputLeft[10] == 0 && notes.outputLeft[11] == 0.75f &&
          notes.outputLeft[92] == 0.75f && notes.outputLeft[93] == 0, "MIDI retains sample-accurate note boundaries");
    check(emitted.count == 2 && emitted.values[0].frameOffset == 11 && emitted.values[1].frameOffset == 93 &&
          emitted.values[0].kind == PluginEvent::Kind::NoteOn && emitted.values[1].kind == PluginEvent::Kind::NoteOff,
          "output MIDI survives the process boundary");
    std::printf("%d failures\n", failures);
    return failures ? 1 : 0;
}
