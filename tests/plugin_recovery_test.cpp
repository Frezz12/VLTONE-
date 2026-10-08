#include "EngineController.hpp"
#include "ProcessPluginInstance.hpp"
#include "platform/AudioFileDecoder.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <thread>

using namespace daw;
using namespace daw::plugins;
using namespace std::chrono_literals;
namespace ap = audio::platform;
namespace {
using Clock = std::chrono::steady_clock;
constexpr unsigned frames = 1024;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
PluginDescriptor descriptor(const char* suffix = "") {
    PluginDescriptor d;
    d.format = Format::Clap; d.path = DAW_FAULT_CLAP_PATH;
    d.uid = std::string("com.daw.test.fault") + suffix; d.name = "Recovery test";
    return d;
}
struct Temp {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("vlt-recovery-" + std::to_string(Clock::now().time_since_epoch().count()));
    Temp() { std::filesystem::create_directories(path); }
    ~Temp() { std::error_code error; std::filesystem::remove_all(path, error); }
};
struct Rig {
    EngineController controller{EngineController::TestRuntime{}};
    engine::GraphProcessor renderer{2};
    std::array<float, frames> left{}, right{};
    float* outputs[2]{left.data(), right.data()};
    explicit Rig(bool isolated = true) {
        if (isolated)
            controller.pluginManager().setHostingMode(PluginManager::HostingMode::Isolated, DAW_PLUGIN_HOST_PATH);
        require(bool(controller.initialize(48000, frames, false)), "initialize controller");
    }
    bool block() {
        renderer.setGraph(controller.routingGraph());
        return bool(renderer.processSerial(engine::AudioBlock(outputs, 2, frames), frames, 0, true));
    }
    void settle(const char* message = "healthy block") { for (int i = 0; i < 4; ++i) require(block(), message); }
};
void controllerRecovery(const std::string& scope, const std::string& audio) {
    Rig r;
    auto& c = r.controller;
    const auto track = c.addTrack(TrackKind::Audio, "Signal");
    const auto clip = c.importAudio(audio, track, 0);
    require(!clip.empty(), "import signal");
    const auto channel = scope == "master" ? std::string(EngineController::kMasterChannelId) :
        scope == "group" ? c.addTrack(TrackKind::Group, "Bus") : track;
    if (scope == "group") require(c.setTrackOutputBus(track, channel), "route group");
    const auto slot = scope == "clip" ? c.addClipFxInsert(track, clip, descriptor(".recover_slow")) :
        c.addInsert(channel, descriptor(".recover_slow"));
    require(!slot.empty(), "create slot");
    const bool dual = scope == "dual";
    if (dual) require(c.setInsertChannelMode(channel, slot, PluginChannelMode::DualMono), "dual mono");
    c.setInsertParameter(channel, slot, "1", .7);
    if (dual) {
        c.setInsertEditorChannel(channel, slot, PluginEditorChannel::Right);
        c.setInsertParameter(channel, slot, "1", .3);
    }
    r.settle();
    auto* original = dynamic_cast<ProcessPluginInstance*>(c.insertInstance(channel, slot));
    require(original, "isolated slot");
    if (scope == "group") {
        require(!original->recovery().parameters.empty(), "completed edits follow the initial checkpoint");
        original->stopProcessing(); original->deactivate();
        require(original->activate({48000, frames * 2, false}), "grow transport with a fresh checkpoint");
        original->startProcessing();
        require(original->recovery().parameters.empty(), "fresh checkpoint absorbs the prior parameter journal");
        r.settle("larger transport preserves the confirmed sound");
    }
    const auto oldPid = original->processId();
    std::vector<std::uint8_t> saved;
    require(original->saveState(saved), "checkpoint");
    const float expectedLeft = scope == "track" ? .12f : .14f;
    if (scope == "track") {
        c.setInsertParameter(channel, slot, "1", .6);
        r.settle("confirmed edit before fault"); // newer than the opaque checkpoint
    }
    c.setInsertParameter(channel, slot, "0", 1); // actual abort in the child
    require(!r.block(), "fault reported");
    c.pumpPluginEvents();
    require(c.insertRuntimeStatus(channel, slot).state == EngineController::PluginRuntimeState::Failed, "failed slot status");
    std::vector<std::uint8_t> retained;
    require(original->saveState(retained) && retained == saved, "failed slot retains confirmed state");
    require(original->parameterValue(0) == 0, "failed DSP edit not replayed on recovery");
    if (scope == "track") {
        Temp savedProject;
        rendering::Spec spec;
        spec.outputDir = savedProject.path.string(); spec.baseName = "must-not-use-old-state";
        rendering::Report report;
        require(!c.renderProject(spec, {}, report) && report.files.empty(), "failed slot rejects stale-state export");
        const auto package = (savedProject.path / "checkpoint.vltone").string();
        require(bool(c.saveProject(package)), "save project with failed slot");
        for (const bool isolated : {true, false}) {
            Rig reopened(isolated);
            require(bool(reopened.controller.openProject(package)), "reopen project with confirmed snapshot");
            reopened.settle("reopened failed project processes");
            require(std::abs(reopened.left.back() - expectedLeft) < .0001f,
                    "saved failed slot restores last confirmed sound in either hosting mode");
        }
    }
    const auto start = Clock::now();
    require(c.restartInsert(channel, slot), "start recovery");
    require(Clock::now() - start < 100ms, "recovery starts without waiting for activation");
    require(!c.restartInsert(channel, slot), "duplicate restart coalesced");
    unsigned blocks = 0;
    while (c.insertRuntimeStatus(channel, slot).state == EngineController::PluginRuntimeState::Restarting &&
           Clock::now() - start < 10s) {
        (void)r.block(); ++blocks;
        c.pumpPluginEvents();
        std::this_thread::sleep_for(2ms);
    }
    require(blocks > 10, "graph continues during slow recovery");
    require(c.insertRuntimeStatus(channel, slot).state == EngineController::PluginRuntimeState::Running, "recovered slot status");
    auto* restored = dynamic_cast<ProcessPluginInstance*>(c.insertInstance(channel, slot));
    require(restored && restored->processId() != oldPid, "fresh process in same slot");
    r.settle();
    require(std::abs(r.left.back() - expectedLeft) < .0001f &&
            std::abs(r.right.back() - (dual ? .06f : expectedLeft)) < .0001f, "state and dual-mono sides restored");
    if (scope == "group") {
        const auto checkpoint = c.captureIncrementalRecoverySnapshot({}, true);
        bool confirmed = false;
        for (const auto& part : checkpoint.trackParts) if (part->id == channel)
            for (const auto& savedSlot : part->inserts) if (savedSlot.id == slot)
                for (const auto& parameter : savedSlot.parameters)
                    if (parameter.id == "0") confirmed = parameter.value == 0;
        require(confirmed, "incremental autosave does not resurrect the rejected DSP edit");
    }
    // A late result must never resurrect a deleted/replaced/undone slot.
    c.setInsertParameter(channel, slot, "0", 1);
    require(!r.block(), "second real crash");
    c.pumpPluginEvents();
    require(c.restartInsert(channel, slot), "start cancellable recovery");
    if (scope == "track") {
        c.removeInsert(channel, slot);
        c.undo(); // same durable ID, different live generation
    } else c.newProject();
    const auto cancelled = Clock::now();
    while (Clock::now() - cancelled < 800ms) { c.pumpPluginEvents(); std::this_thread::sleep_for(4ms); }
    if (scope == "track") {
        require(c.insertInstance(channel, slot) && !c.insertInstance(channel, slot)->hasFailed(), "Undo keeps replacement generation");
        r.settle("Undo processes without replaying the failed edit");
        require(std::abs(r.left.back() - expectedLeft) < .0001f, "Undo restores confirmed state and journal");
    } else require(!c.insertInstance(channel, slot), "new project rejects old recovery result");
    std::printf("PASS recovery %s: %u blocks during activation\n", scope.c_str(), blocks);
}

#ifdef DAW_TEST_HOST_GUI
template<class Predicate>
bool waitUntil(Predicate ready, std::chrono::milliseconds timeout = 2s) {
    const auto deadline = Clock::now() + timeout;
    do {
        if (ready()) return true;
        std::this_thread::sleep_for(2ms);
    } while (Clock::now() < deadline);
    return false;
}
struct Environment {
    std::string key;
    std::optional<std::string> previous;
    static void set(const char* key, const char* value) {
#ifdef _WIN32
        _putenv_s(key, value ? value : "");
#else
        if (value) setenv(key, value, 1); else unsetenv(key);
#endif
    }
    Environment(const char* name, const std::string& value) : key(name) {
        if (const auto* old = std::getenv(name)) previous = old;
        set(name, value.c_str());
    }
    ~Environment() { set(key.c_str(), previous ? previous->c_str() : nullptr); }
};
void guiRecovery() {
    PluginProcess p(DAW_PLUGIN_HOST_PATH, {128, 8, 4096, 65536}, 1500ms);
    require(p.load(descriptor(".editor")) && p.metadata().hasEditor, "native editor capability");
    p.requestEditor(true);
    const auto start = Clock::now();
    while (p.editorStatus() != 1 && Clock::now() - start < 3s) { p.checkHealth(); std::this_thread::sleep_for(2ms); }
    require(p.editorStatus() == 1, "native editor opens in child");
    PluginEvent event; bool changed = false;
    require(waitUntil([&] {
        while (p.popNotification(event)) changed |= event.paramIndex == 1 && event.value == .75;
        return changed;
    }), "editor callback reaches parent outside DSP");
    const auto responsive = Clock::now();
    while (p.checkHealth() && Clock::now() - responsive < 2s) std::this_thread::sleep_for(5ms);
    require(p.checkHealth(), "idle healthy editor keeps its heartbeat beyond the watchdog timeout");
    p.requestEditor(false);
    const auto closing = Clock::now();
    while (p.editorStatus() != 0 && Clock::now() - closing < 2s) std::this_thread::sleep_for(2ms);
    require(p.editorStatus() == 0, "native editor closes asynchronously");
    require(p.load(descriptor(".editor_crash")), "load editor-crash fixture");
    p.requestEditor(true);
    const auto crash = Clock::now();
    while (p.checkHealth() && Clock::now() - crash < 3s) std::this_thread::sleep_for(2ms);
    require(p.failure() == PluginProcessFailure::Exited, "editor crash remains inside helper");
    require(p.load(descriptor(".editor_hang")) && p.activate({48000, 128, false}) && p.startProcessing(), "prepare GUI hang");
    p.requestEditor(true);
    std::this_thread::sleep_for(40ms);
    std::array<float, 128> in{}, out{}; in.fill(.4f);
    const float* inputs[]{in.data()}; float* outputs[]{out.data()};
    PluginProcessContext block;
    block.inputs = inputs; block.inputChannels = 1; block.outputs = outputs;
    block.outputChannels = 1; block.frames = 128;
    require(p.submit(block), "submit DSP while GUI is stuck");
    const auto audioStart = Clock::now();
    PluginProcessDisposition disposition;
    PluginProcess::Poll result;
    do { result = p.poll(block, disposition); std::this_thread::yield(); }
    while (result == PluginProcess::Poll::Pending && Clock::now() - audioStart < 250ms);
    require(result == PluginProcess::Poll::Complete && out.back() == .2f, "GUI hang does not block audio mailbox");
    while (p.checkHealth() && Clock::now() - audioStart < 3s) std::this_thread::sleep_for(5ms);
    require(p.failure() == PluginProcessFailure::ControlTimeout, "GUI watchdog marks unresponsive helper");
    require(!p.service(), "hung GUI process reaped");

    require(p.load(descriptor(".editor_idle_hang")), "load post-open GUI hang fixture");
    p.requestEditor(true);
    require(waitUntil([&] { return p.editorStatus() == 1; }), "editor acknowledges open before its event thread hangs");
    const auto idleHang = Clock::now();
    while (p.checkHealth() && Clock::now() - idleHang < 3s) std::this_thread::sleep_for(5ms);
    require(p.failure() == PluginProcessFailure::EditorUnresponsive,
            "GUI heartbeat detects an already-open hung editor without a pending control request");
    require(!p.service() && p.processId() == 0, "post-open hang reaps only that helper");
    require(p.restart(), "editor failure remains restartable");
    require(p.checkHealth(), "restarted helper has a fresh watchdog generation");
#ifdef _WIN32
    require(p.load(descriptor(".editor_modal")), "load nested modal-loop fixture");
    p.requestEditor(true);
    require(waitUntil([&] { return p.editorStatus() == 1; }), "responsive nested editor opens");
    const auto modal = Clock::now();
    while (p.checkHealth() && Clock::now() - modal < 3500ms) std::this_thread::sleep_for(5ms);
    require(p.checkHealth(), "native modal event loop stays alive beyond the control timeout");
    require(p.refreshMetadata() && p.metadata().values[1] == .375,
            "responsive modal callback completed rather than being skipped");
#endif
    p.requestEditor(false);
    require(waitUntil([&] { return p.editorStatus() == 0; }), "responsive editor still closes after watchdog checks");
    std::puts("PASS isolated editor open / callback / close / crash / open and idle hang / modal responsiveness");
}

void formatEditors() {
#if DAW_ENABLE_VST3
    {
        Temp temp;
        const auto trace = temp.path / "editor.txt";
        Environment tracing("DAW_TEST_VST3_EDITOR_TRACE", trace.string());
        Environment gesture("DAW_TEST_VST3_EDITOR_GESTURE", "2");
        const auto descriptors = factoryFor(Format::Vst3)->inspect(DAW_TEST_VST3_PATH);
        require(!descriptors.empty(), "VST3 editor catalogue");
        PluginProcess p(DAW_PLUGIN_HOST_PATH);
        require(p.load(descriptors.front()) && p.metadata().hasEditor, "VST3 editor capability");
        p.requestEditor(true);
        require(waitUntil([&] { return p.editorStatus() == 1; }), "VST3 native view attached");
        std::vector<PluginEvent> notices;
        require(waitUntil([&] {
            PluginEvent event;
            while (p.popNotification(event)) notices.push_back(event);
            return notices.size() >= 6;
        }), "VST3 editor gesture delivered");
        require(notices[0].kind == PluginEvent::Kind::ParamGestureBegin &&
                notices[1].kind == PluginEvent::Kind::ParamValue &&
                notices[2].kind == PluginEvent::Kind::ParamGestureEnd &&
                notices[0].paramIndex == notices[1].paramIndex &&
                notices[1].paramIndex == notices[2].paramIndex,
                "VST3 begin/value/end retain order and parameter identity");
        require(notices[3].kind == PluginEvent::Kind::ParamGestureBegin &&
                notices[4].kind == PluginEvent::Kind::ParamValue &&
                notices[5].kind == PluginEvent::Kind::ParamGestureEnd &&
                notices[3].paramIndex == notices[1].paramIndex &&
                notices[4].paramIndex == notices[1].paramIndex &&
                notices[5].paramIndex == notices[1].paramIndex &&
                notices[1].value == .25 && notices[4].value == .75,
                "two queued VST3 gestures retain their own final values");
        const auto pending = p.pendingParameterEvents();
        require(!pending.empty() && pending.front().value == notices[1].value,
                "VST3 pending editor value survives before first DSP block");
        p.requestEditor(false);
        require(waitUntil([&] { return p.editorStatus() == 0; }), "VST3 native view closed");
        p.close();
        std::ifstream file(trace);
        const std::string events{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        require(events.find("attach\n") != std::string::npos &&
                events.find("remove\n", events.find("attach\n")) != std::string::npos,
                "VST3 view removed before helper shutdown");
        std::puts("PASS isolated VST3 editor lifecycle / ordered gestures / pending edit");
    }
#endif
#if DAW_ENABLE_VST
    {
        const auto descriptors = factoryFor(Format::Vst)->inspect(DAW_TEST_VST_SHELL_PATH);
        require(!descriptors.empty(), "VST2 editor catalogue");
        bool tested = false;
        for (const auto& descriptor : descriptors) {
            PluginProcess p(DAW_PLUGIN_HOST_PATH);
            require(p.load(descriptor), "VST2 editor load");
            if (!p.metadata().hasEditor) continue;
            p.requestEditor(true);
            require(waitUntil([&] { return p.editorStatus() == 1; }), "VST2 editor opens");
            std::this_thread::sleep_for(100ms);
            require(p.refreshMetadata() && p.metadata().values.size() > 4 && p.metadata().values[4] > 0,
                    "VST2 child services editor idle");
            p.requestEditor(false);
            require(waitUntil([&] { return p.editorStatus() == 0; }), "VST2 editor closes");
            require(p.refreshMetadata(), "VST2 closed editor metadata");
            const auto idle = p.metadata().values[4];
            std::this_thread::sleep_for(100ms);
            require(p.refreshMetadata() && p.metadata().values[4] == idle, "VST2 idle stops when editor closes");
            tested = true;
        }
        require(tested, "VST2 native editor fixture exercised");
        std::puts("PASS isolated VST2 editor lifecycle / idle / close");
    }
#endif
}
#endif
}
int main() {
    try {
        Temp temp;
        const auto audio = (temp.path / "signal.wav").string();
        ap::AudioFileWriter writer;
        ap::WriteSpec format; format.encoding = ap::Encoding::Float32;
        std::vector<float> source(48000, .2f); const float* channels[]{source.data(), source.data()};
        require(bool(writer.open(audio, format, 48000, 2, 48000)) &&
                bool(writer.write(channels, 48000)) && bool(writer.close()), "create signal");
        for (const auto* scope : {"track", "group", "master", "clip", "dual"}) controllerRecovery(scope, audio);
#ifdef DAW_TEST_HOST_GUI
        guiRecovery();
        formatEditors();
#endif
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "FAIL %s\n", error.what()); return 1; }
}
