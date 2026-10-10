#include "EngineController.hpp"
#include "fixtures/test_clap/RecoveryTestControl.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <new>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace { std::atomic<bool> countAllocations{false}; std::atomic<unsigned> allocations{0}; }
void* operator new(std::size_t size) {
    if (countAllocations.load(std::memory_order_relaxed)) ++allocations;
    if (void* value = std::malloc(std::max(size, std::size_t(1)))) return value;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

namespace {
using namespace daw;
int failures = 0;
bool check(bool value, const char* text) {
    std::printf("%s %s\n", value ? "PASS" : "FAIL", text); failures += !value; return value;
}
struct Block {
    audio::AudioBuffer in{2, 64}, out{2, 64};
    Block() { std::fill_n(in.getChannel(0), 64, .25f); std::fill_n(in.getChannel(1), 64, .5f); }
    void render(AudioRuntime& runtime, unsigned blocks = 1) {
        audio::AudioCallbackContext context;
        context.inputBuffer = &in; context.outputBuffer = &out; context.numFrames = 64; context.sampleRate = 48000;
        for (unsigned i = 0; i < blocks; ++i) runtime.processDeviceBlock(context);
    }
    void render(EngineController& controller, unsigned blocks = 1) {
        for (unsigned i = 0; i < blocks; ++i) controller.processDeviceBlockForTest(in, out, 64);
    }
    bool equals(float left, float right) const {
        for (unsigned frame = 0; frame < 64; ++frame)
            if (std::abs(out.getChannel(0)[frame] - left) > 1e-6f ||
                std::abs(out.getChannel(1)[frame] - right) > 1e-6f ||
                !std::isfinite(out.getChannel(0)[frame]) || !std::isfinite(out.getChannel(1)[frame])) return false;
        return true;
    }
};
AudioSessionSpec session(const plugins::PluginDescriptor& descriptor, bool dual, bool master = false) {
    AudioSessionSpec spec;
    AudioGraphSpec::Channel channel; channel.id = "track"; channel.name = "Track";
    channel.input = {true, true, 0, 2, 3}; spec.graph.channels.push_back(channel);
    AudioPluginSpec plugin; plugin.id = "slot"; plugin.uid = descriptor.uid; plugin.name = descriptor.name;
    plugin.descriptor = descriptor; plugin.requiredFormat = descriptor.format;
    plugin.channelMode = dual ? PluginChannelMode::DualMono : PluginChannelMode::Stereo;
    plugin.preferredChannels = dual ? 1 : 2;
    AudioPluginChainSpec chain; chain.channelId = master ? AudioGraphSpec::masterChannelId : "track";
    chain.slots.push_back(plugin); spec.pluginChains.push_back(chain);
    return spec;
}
void runtimeCase(RecoveryTestControl& control, const plugins::PluginDescriptor& descriptor,
                 unsigned mode, bool dual, bool right, bool master = false) {
    control.mode = 0; control.target = 0;
    AudioRuntime runtime;
    auto spec = session(descriptor, dual, master);
    check(bool(runtime.prepare(48000, 64)) && bool(runtime.applySession(spec)), "prepare native recovery fixture");
    const auto channel = spec.pluginChains.front().channelId;
    const auto leftId = runtime.pluginInstanceId({channel, "slot"});
    const auto rightId = runtime.pluginInstanceId({channel, "slot", true});
    const auto native = control.next.load() - (dual && !right ? 1 : 0);
    Block block;
    runtime.transportCommand({AudioTransportCommand::Action::Play});
    block.render(runtime, 8);
    check(block.equals(.125f, .25f), "healthy plugin processes both input channels");
    const auto saves = control.saves.load();
    control.target = native; control.mode = mode;
    std::fill_n(block.in.getChannel(0), 64, .75f);
    std::fill_n(block.in.getChannel(1), 64, 1.f);
    allocations = 0; countAllocations = true;
    block.render(runtime);
    countAllocations = false;
    check(allocations == 0, "detecting a native failure allocates no host objects in the callback");
    bool safe = true;
    for (unsigned frame = 0; frame < 64; ++frame) {
        const float left = master ? 0.f : frame < 32 ? .25f : .75f;
        const float right = master ? 0.f : frame < 32 ? .5f : 1.f;
        safe &= std::isfinite(block.out.getChannel(0)[frame]) && std::isfinite(block.out.getChannel(1)[frame]) &&
            std::abs(block.out.getChannel(0)[frame] - left) < 1e-6f && std::abs(block.out.getChannel(1)[frame] - right) < 1e-6f;
    }
    check(safe, "failure block preserves 32 samples of dry latency or final master silence");
    std::fill_n(block.in.getChannel(0), 64, .25f);
    std::fill_n(block.in.getChannel(1), 64, .5f);
    const auto calls = control.calls[native].load();
    const auto fault = runtime.pluginRuntimeStatus(channel, "slot");
    check(fault.state == AudioPluginRuntimeState::Faulted && fault.canRetry, "fault is latched with an available checkpoint");
    check(runtime.pluginStateSnapshot(fault.address).stateCaptured && control.saves == saves,
          "failed state snapshot reuses checkpoint without calling vendor save");
    runtime.transportCommand({AudioTransportCommand::Action::Pause});
    runtime.transportCommand({AudioTransportCommand::Action::Seek});
    spec.pluginChains[0].slots[0].bypassed = true;
    check(bool(runtime.applySession(spec)), "graph edits remain possible around a failed plugin");
    block.render(runtime, 3);
    check(control.calls[native] == calls && runtime.hasPluginFault(fault.address), "seek, bypass and graph publication preserve quarantine");
    control.mode = 0;
    control.failCreate = true;
    check(!runtime.recoverPlugin(fault.address) && runtime.pluginInstanceId(fault.address) == fault.address.instance,
          "failed factory creation leaves the original safe slot intact");
    control.failCreate = false;
    control.failLoad = true;
    check(!runtime.recoverPlugin(fault.address) && runtime.pluginInstanceId(fault.address) == fault.address.instance,
          "rejected checkpoint preserves the failed instance and safe graph");
    control.failLoad = false; control.failActivate = true;
    check(!runtime.recoverPlugin(fault.address), "failed preparation never publishes the candidate");
    control.failActivate = false;
    unsigned retired = 0;
    check(bool(runtime.recoverPlugin(fault.address, [&] { ++retired; })) && retired == 1,
          "prepared replacement commits before editor retirement");
    check(runtime.pluginRuntimeStatus(channel, "slot").state == AudioPluginRuntimeState::Local,
          "successful replacement clears local fault only");
    if (dual) check(right ? runtime.pluginInstanceId({channel, "slot"}) == leftId
                          : runtime.pluginInstanceId({channel, "slot", true}) == rightId,
                    "Dual Mono recovery preserves the healthy instance");
    check(!runtime.recoverPlugin(fault.address), "stale recovery identity cannot affect a successor");
    block.render(runtime, 8);
    check(block.equals(.25f, .5f), "user bypass survives recovery and master safety mute is released");
}
void unavailableAndRollback(RecoveryTestControl& control, const plugins::PluginDescriptor& descriptor) {
    control.mode = 0; control.target = 0; control.failSave = true;
    AudioRuntime runtime; auto spec = session(descriptor, false);
    check(bool(runtime.prepare(48000, 64)) && bool(runtime.applySession(spec)), "a plugin without a checkpoint can still initially load");
    Block block; block.render(runtime, 8); control.mode = 1; block.render(runtime);
    auto status = runtime.pluginRuntimeStatus("track", "slot");
    check(!status.canRetry && !runtime.recoverPlugin(status.address), "missing checkpoint never substitutes factory defaults");
    control.mode = 0; control.failSave = false;
    auto bad = spec; bad.pluginChains[0].slots[0].id = "replacement";
    AudioGraphSpec::Channel aux; aux.id = "aux"; aux.outputBusId = "track";
    bad.graph.channels[0].outputBusId = "aux"; bad.graph.channels.push_back(aux);
    const auto graph = runtime.routingGraph(); unsigned retirements = 0;
    check(!runtime.applySession(bad, false, {}, {}, nullptr, [&] { ++retirements; }) &&
        !retirements && runtime.pluginInstanceId({"track", "slot"}) == status.address.instance && !runtime.audioSafetyStopped(),
        "failed publication rolls back without closing the old editor or clearing the fault");
    block.render(runtime, 3);
    check(block.equals(.25f, .5f), "safe bypass still runs after graph rollback");
}
void stateImportTransaction(RecoveryTestControl& control, const plugins::PluginDescriptor& descriptor) {
    control.mode = 0; control.target = 0;
    AudioRuntime runtime; auto spec = session(descriptor, false);
    if (!check(bool(runtime.prepare(48000, 64)) && bool(runtime.applySession(spec)), "prepare state import transaction")) return;
    const auto identity = runtime.pluginInstanceId({"track", "slot"});
    const auto graph = runtime.routingGraph();
    std::vector<InsertParameter> parameters;
    AudioPluginStateRestore imported; imported.state = {0xff};
    unsigned retired = 0;
    const AudioPluginAddress address{"track", "slot", false, identity};
    check(!runtime.restorePluginState(address, imported, parameters, [&] { ++retired; }) &&
        runtime.routingGraph() == graph && runtime.pluginInstanceId(address) == identity && retired == 0,
        "rejected native loadState never mutates or retires the live instance");
    float gain = 1.25f; imported.state.resize(sizeof(gain));
    std::memcpy(imported.state.data(), &gain, sizeof(gain));
    check(bool(runtime.restorePluginState(address, imported, parameters, [&] { ++retired; })) && retired == 1 &&
        runtime.pluginInstanceId({"track", "slot"}) != identity && runtime.pluginParameter({"track", "slot"}, "0") == gain,
        "successful native state import prepares and publishes a replacement");
    Block block;
    runtime.transportCommand({AudioTransportCommand::Action::Play});
    block.render(runtime, 8);
    const AudioPluginAddress published{"track", "slot", false, runtime.pluginInstanceId({"track", "slot"})};
    check(!runtime.restorePluginState(published, imported, parameters, [] {
        throw std::runtime_error("injected editor retirement failure after graph publication");
    }), "failure after graph publication returns an error instead of claiming rollback");
    const auto native = control.next.load(), calls = control.calls[native].load();
    block.render(runtime);
    check(runtime.audioSafetyStopped() && !runtime.transportSnapshot().playing && block.equals(0, 0) &&
        control.calls[native] == calls, "unrecoverable rollback stops DSP and emits only silence");
    // This runtime never opened a live device; parameter delivery would
    // otherwise use its synchronous preview render path.
    check(!runtime.advancePluginEdits() && control.calls[native] == calls,
        "preview parameter delivery cannot restart DSP after a failed rollback");
}
void controllerCase(RecoveryTestControl& control, const plugins::PluginDescriptor& descriptor) {
    control.mode = 0; control.target = 0;
    EngineController controller{}; check(bool(controller.initialize(48000, 64, false)), "controller initializes");
    auto catalog = controller.pluginManager().catalogSnapshot(); PluginCacheEntry entry;
    entry.format = descriptor.format; entry.path = descriptor.path; entry.ok = true; entry.plugins.push_back(descriptor);
    catalog.cache.put(std::move(entry)); controller.pluginManager().restoreCatalog(std::move(catalog));
    const auto track = controller.addTrack(TrackKind::Audio, "Fault track");
    controller.setTrackInputRouting(track, 0, 2, true); controller.setTrackMonitor(track, true);
    const auto slot = controller.addInsert(track, descriptor);
    check(!slot.empty(), "controller inserts native fixture");
    const auto originalIdentity = controller.insertIdentity(track, slot);
    const auto originalUndo = controller.undoDepth();
    unsigned retirements = 0;
    controller.setPluginRetiringCallback([&](const auto&, const auto&) { ++retirements; });
    controller.seekSeconds(2.5); controller.play();
    const auto position = controller.positionSeconds();
    EngineController::PreparedProject rejected;
    rejected.document = controller.project(); rejected.rate = 48000;
    TrackModel aux; aux.id = "cycle"; aux.kind = TrackKind::Audio; aux.outputBusId = track;
    rejected.document.findTrack(track)->outputBusId = aux.id;
    rejected.document.tracks.push_back(aux);
    check(!controller.openPreparedProject(std::move(rejected)) && controller.insertIdentity(track, slot) == originalIdentity &&
        controller.undoDepth() == originalUndo && controller.isPlaying() && controller.positionSeconds() == position && retirements == 0,
        "failed project publication preserves project generation, Undo, transport and open editor");
    controller.pause();
    auto unavailable = descriptor; unavailable.uid = "test.missing";
    check(!controller.replaceInsert(track, slot, unavailable) && controller.insertIdentity(track, slot) == originalIdentity &&
          controller.insertModel(track, slot)->uid == descriptor.uid && controller.undoDepth() == originalUndo && retirements == 0,
          "failed replacement preserves document, Undo, native identity and editor");
    auto savedChain = controller.copyChannelStrip(track, false);
    savedChain.inserts.front().state = {0xff};
    check(!controller.pasteChannelInserts(track, savedChain), "invalid state import is rejected");
    check(controller.insertIdentity(track, slot) == originalIdentity, "invalid import retains the native slot identity");
    check(retirements == 0, "invalid import leaves the existing editor open");
    check(controller.undoDepth() == originalUndo, "invalid import does not add an Undo entry");
    auto rejectedStrip = savedChain;
    rejectedStrip.hasSettings = true; rejectedStrip.volume = .2f; rejectedStrip.pan = -.4f;
    const auto volume = controller.project().findTrack(track)->volume;
    check(!controller.pasteChannelStrip(track, rejectedStrip) && controller.project().findTrack(track)->volume == volume &&
        controller.insertIdentity(track, slot) == originalIdentity && controller.undoDepth() == originalUndo,
        "failed strip import rolls back plugins, channel controls and Undo together");
    (void)controller.takeAudioFailureNotices();
    const auto params = controller.insertParameters(track, slot);
    if (params.empty()) { check(false, "fixture parameter exists"); return; }
    controller.setInsertParameter(track, slot, params[0].id, 1.25);
    Block block; controller.play(); block.render(controller, 8);
    const auto backgroundSaves = control.saves.load();
    (void)controller.refreshRecoveryPluginStates(1);
    check(control.saves == backgroundSaves, "background checkpoints never serialize during playback");
    controller.pause();
    (void)controller.refreshRecoveryPluginStates(1);
    check(control.saves == backgroundSaves, "background checkpoints wait for input monitoring to stop");
    controller.play();
    const auto generation = controller.projectGeneration(); const auto undo = controller.undoLabel();
    const auto undoDepth = controller.undoDepth();
    control.mode = 2; block.render(controller); controller.serviceAudioHealth();
    auto status = controller.insertRuntimeStatus(track, slot);
    check(status.state == AudioPluginRuntimeState::AwaitingRecovery && control.forbidden == 0,
          "playing defers recovery and performs no reads from the failed native instance");
    controller.refreshRecoveryPluginStates(std::numeric_limits<std::size_t>::max());
    check(control.forbidden == 0, "project snapshot never serializes the failed instance");
    auto notices = controller.takeAudioFailureNotices();
    check(notices.size() == 1 && notices.front().canRetry && notices.front().requiresStop, "one typed notice describes the pending recovery");
    rendering::Spec exportSpec; exportSpec.outputDir = std::filesystem::temp_directory_path().string();
    exportSpec.range = rendering::Range::Custom; exportSpec.customStartSeconds = 0; exportSpec.customEndSeconds = 1;
    RenderSessionSpec exported;
    check(!controller.captureRenderSession(exportSpec, exported) && !exported.valid(), "failed plugin cannot publish an automatic bypass render");
    control.mode = 0; controller.pause(); block.render(controller, 4);
    controller.serviceAudioHealth();
    check(controller.insertRuntimeStatus(track, slot).state != AudioPluginRuntimeState::Local, "input monitoring defers automatic recovery");
    controller.setTrackMonitor(track, false); block.render(controller, 16);
    controller.serviceAudioHealth();
    check(controller.insertRuntimeStatus(track, slot).state == AudioPluginRuntimeState::Local &&
          controller.projectGeneration() == generation && controller.undoLabel() == undo && controller.undoDepth() == undoDepth,
          "automatic recovery preserves project identity and Undo");
    block.render(controller);
    check(std::abs(controller.insertParameter(track, slot, params[0].id) - 1.25) < 1e-6,
          "confirmed host parameter change is restored on top of the checkpoint");
    controller.play(); control.mode = 3; block.render(controller); controller.pause(); block.render(controller, 16);
    control.mode = 0; controller.serviceAudioHealth();
    status = controller.insertRuntimeStatus(track, slot);
    const auto created = control.next.load();
    controller.play(); controller.pause(); controller.serviceAudioHealth();
    check(status.state == AudioPluginRuntimeState::Faulted && control.next == created,
          "a repeat failure cannot gain another automatic attempt by toggling transport");
    check(bool(controller.retryInsertRecovery(track, slot, {generation, status.address.instance})), "explicit retry remains available");
    check(!controller.retryInsertRecovery(track, slot, {generation, status.address.instance}), "old action cannot retry a replacement");
    controller.serviceAudioHealth(false); (void)controller.takeAudioFailureNotices();
    check(bool(controller.newProject()), "new project returns an explicit successful result");
    controller.serviceAudioHealth();
    check(controller.takeAudioFailureNotices().empty() && !controller.retryInsertRecovery(track, slot, {generation, status.address.instance}),
          "project change cancels notifications and stale recovery actions");
}
void recoveryBudgetCase(RecoveryTestControl& control, const plugins::PluginDescriptor& descriptor) {
    control.mode = 0; control.target = 0;
    EngineController controller{};
    if (!check(bool(controller.initialize(48000, 64, false)), "recovery queue fixture initializes")) return;
    auto catalog = controller.pluginManager().catalogSnapshot(); PluginCacheEntry entry;
    entry.format = descriptor.format; entry.path = descriptor.path; entry.ok = true; entry.plugins.push_back(descriptor);
    catalog.cache.put(std::move(entry)); controller.pluginManager().restoreCatalog(std::move(catalog));
    const auto track = controller.addTrack(TrackKind::Audio, "Recovery queue");
    const auto first = controller.addInsert(track, descriptor);
    const auto second = controller.addInsert(track, descriptor);
    if (!check(!first.empty() && !second.empty(), "two recovery candidates are installed")) return;
    controller.setTrackInputRouting(track, 0, 2, true); controller.setTrackMonitor(track, true);
    Block block; controller.play(); block.render(controller, 8);
    control.mode = 1; block.render(controller);
    controller.pause(); controller.setTrackMonitor(track, false); block.render(controller, 16);
    control.mode = 0; control.failActivate = true;
    const auto created = control.next.load();
    controller.serviceAudioHealth();
    check(control.next == created + 1, "one maintenance pass prepares at most one recovery candidate");
    controller.serviceAudioHealth();
    check(control.next == created + 2, "the next maintenance pass handles the other failed slot");
    control.failActivate = false;
    controller.play(); controller.pause(); controller.serviceAudioHealth();
    check(control.next == created + 2 &&
        controller.insertRuntimeStatus(track, first).state == AudioPluginRuntimeState::Faulted &&
        controller.insertRuntimeStatus(track, second).state == AudioPluginRuntimeState::Faulted,
        "failed automatic attempts stay exhausted after transport changes");
    const auto status = controller.insertRuntimeStatus(track, first);
    check(bool(controller.retryInsertRecovery(track, first, {controller.projectGeneration(), status.address.instance})) &&
        controller.insertRuntimeStatus(track, second).state == AudioPluginRuntimeState::Faulted,
        "manual retry restores only its requested slot after automatic failure");
}
}
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#ifdef _WIN32
    auto library = LoadLibraryA(DAW_TEST_RECOVERY_CLAP_PATH);
    auto getControl = reinterpret_cast<RecoveryTestControl*(*)()>(GetProcAddress(library, "recoveryTestControl"));
#else
    auto library = dlopen(DAW_TEST_RECOVERY_CLAP_PATH, RTLD_NOW | RTLD_LOCAL);
    auto getControl = library ? reinterpret_cast<RecoveryTestControl*(*)()>(dlsym(library, "recoveryTestControl")) : nullptr;
#endif
    if (!getControl) { check(false, "fixture control module loads"); return 1; }
    auto& control = *getControl();
    auto* factory = plugins::factoryFor(plugins::Format::Clap);
    const auto descriptors = factory->inspect(DAW_TEST_RECOVERY_CLAP_PATH);
    if (descriptors.empty()) { check(false, "native fixture descriptor loads"); return 1; }
    stateImportTransaction(control, descriptors[0]);
    for (unsigned mode : {1u, 2u, 3u}) runtimeCase(control, descriptors[0], mode, false, false);
    runtimeCase(control, descriptors[0], 1, true, false);
    runtimeCase(control, descriptors[0], 2, true, true);
    runtimeCase(control, descriptors[0], 3, false, false, true);
    unavailableAndRollback(control, descriptors[0]);
    controllerCase(control, descriptors[0]);
    recoveryBudgetCase(control, descriptors[0]);
    check(control.forbidden == 0, "no DSP, state or parameter calls reach a quarantined native instance");
    return failures ? 1 : 0;
}
