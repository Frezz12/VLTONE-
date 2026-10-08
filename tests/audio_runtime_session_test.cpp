#include "AudioRuntime.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <latch>
#include <thread>

namespace {
using namespace daw;
int failures = 0;
bool check(bool value, const char* message) {
    std::printf("%s %s\n", value ? "PASS" : "FAIL", message);
    failures += !value; return value;
}
AudioSessionSpec session(float sample = .5f) {
    AudioSessionSpec value;
    AudioGraphSpec::Channel source; source.id = "source"; source.name = "Source";
    source.input = {true, false, 0, 2, 3}; source.acceptsMidi = true;
    source.sends.push_back({"send", "aux", .2f, false, true});
    AudioGraphSpec::Channel aux; aux.id = "aux"; aux.name = "Aux";
    value.graph.channels = {source, aux};
    auto pcm = std::make_shared<engine::SampleBuffer>(2, 32768, 48000);
    for (unsigned channel = 0; channel < 2; ++channel) std::fill_n(pcm->writableChannel(channel), pcm->frames(), sample);
    engine::ClipPlacement placement; placement.audio = pcm; placement.clipId = "clip"; placement.lengthSamples = pcm->frames();
    auto clips = std::make_shared<engine::ClipPlayerNode::ClipList>(); clips->push_back(std::move(placement));
    AudioSessionSpec::Channel content; content.id = "source";
    content.content.clips = AudioContentSpec::Clips{clips, {}};
    auto notes = std::make_shared<engine::MidiClipPlayerNode::NoteList>();
    notes->push_back({});
    content.content.midi = AudioContentSpec::Midi{notes, {}, false};
    value.channels.push_back(std::move(content));
    AudioPluginChainSpec chain; chain.channelId = "source";
    AudioPluginSpec eq; eq.id = "eq"; eq.uid = "daw.equalizer"; eq.name = "EQ";
    eq.requiredFormat = eq.descriptor.format = plugins::Format::Internal;
    eq.descriptor.uid = eq.uid; eq.descriptor.name = eq.name;
    eq.parameters = {{"output.gain", 0}};
    chain.slots.push_back(eq); value.pluginChains.push_back(std::move(chain));
    return value;
}
template<class Node> std::shared_ptr<Node> node(const AudioRuntime& runtime, std::string_view name) {
    const auto graph = runtime.routingGraph();
    if (graph) for (const auto& entry : graph->nodes)
        if (entry.node->name() == name) return std::dynamic_pointer_cast<Node>(entry.owner);
    return {};
}
void transactions() {
    AudioRuntime runtime;
    auto original = session();
    if (!check(bool(runtime.prepare(48000, 64)) && bool(runtime.applySession(original)),
               "initial transaction publishes a session without a controller")) return;
    const auto native = runtime.pluginInstanceId({"source", "eq"});
    auto fader = node<engine::GainNode>(runtime, "Source Fader");
    auto clips = node<engine::ClipPlayerNode>(runtime, "Source Clips");
    auto midi = node<engine::MidiClipPlayerNode>(runtime, "Source Notes");
    if (!check(native && fader && clips && midi, "session contains retained plugin, fader and immutable playback owners")) return;
    runtime.setFader("source", AudioFaderTarget::Channel, {.gain = .25f, .pan = -.1f});
    runtime.setPluginParameter({"source", "eq"}, "output.gain", -3);
    auto plugin = node<plugins::PluginNode>(runtime, "EQ");
    if (!check(bool(plugin), "retained plugin is available for queued MIDI regression")) return;
    plugins::PluginEvent release;
    release.kind = plugins::PluginEvent::Kind::NoteOff; release.key = 67; release.noteId = 42;
    check(plugin->pushEvent(release), "a MIDI release is queued before the failed edit");
    const auto oldClips = clips->controlState();
    const auto oldMidi = midi->controlState();
    auto bad = session(.9f);
    bad.graph.channels[0].volume = .9f; bad.graph.channels[0].pan = .7f;
    bad.graph.channels[0].outputBusId = "aux"; bad.graph.channels[1].outputBusId = "source";
    bad.pluginChains[0].slots[0].bypassed = true;
    check(!runtime.applySession(std::move(bad)), "a cyclic incremental session is rejected");
    const auto restored = fader->controlState();
    check(runtime.pluginInstanceId({"source", "eq"}) == native &&
          node<engine::GainNode>(runtime, "Source Fader") == fader &&
          restored.gain == .25f && restored.pan == -.1f,
          "failed reconciliation restores live controls and retains native identity");
    check(clips->controlState() == oldClips && midi->controlState().notes == oldMidi.notes,
          "rollback republishes original indexed clip/MIDI schedules without reconstructing DSP state");
    check(runtime.pluginParameter({"source", "eq"}, "output.gain") == -3,
          "pending host parameter edits survive rollback independently of the document mirror");
    const auto restoredEvents = plugin->controlState().pending;
    check(std::count_if(restoredEvents.begin(), restoredEvents.end(), [](const auto& event) {
        return event.kind == plugins::PluginEvent::Kind::NoteOff && event.key == 67 && event.noteId == 42;
    }) == 1, "rollback preserves queued MIDI releases exactly once");

    original.graph.channels[1].name = "Renamed Aux";
    check(bool(runtime.applySession(original)) && runtime.pluginInstanceId({"source", "eq"}) == native &&
          node<engine::GainNode>(runtime, "Source Fader") == fader,
          "successful incremental topology publication reuses healthy plugin and fader owners");
    const auto token = runtime.captureTransaction();
    check(bool(runtime.applySession({})), "empty projection retires session nodes through the shared publication path");
    auto replacement = session(.1f);
    check(bool(runtime.applySession(std::move(replacement))) && runtime.pluginInstanceId({"source", "eq"}) != native,
          "document activation may explicitly load a new set of session nodes");
    check(bool(runtime.restoreTransaction(token)) && runtime.pluginInstanceId({"source", "eq"}) == native &&
          node<engine::GainNode>(runtime, "Source Fader") == fader,
          "runtime-owned token restores owners after a successful intermediate publication");
    runtime.releaseTransaction(token);
    check(!runtime.restoreTransaction(token), "released transaction tokens cannot restore stale state");

    runtime.transportCommand({AudioTransportCommand::Action::Play});
    audio::AudioBuffer output(2, 64); output.clear();
    audio::AudioCallbackContext context; context.outputBuffer = &output; context.numFrames = 64; context.sampleRate = 48000;
    for (unsigned block = 0; block < 16; ++block) runtime.processDeviceBlock(context);
    check(context.renderStatus == audio::AudioCallbackContext::RenderStatus::Complete &&
          runtime.meterSnapshot("source").left > .1f,
          "restored session continues rendering through the real callback");

    original.pluginChains.clear();
    check(bool(runtime.applySession(original)) && !runtime.hasPlugin({"source", "eq"}),
          "a complete incremental projection removes omitted plugin chains");
}

void atomicCheckpointImports() {
    AudioRuntime donor, runtime;
    auto original = session();
    auto savedSession = session();
    auto& imported = savedSession.pluginChains[0].slots[0];
    imported.id = "imported"; imported.channelMode = PluginChannelMode::DualMono; imported.preferredChannels = 1;
    imported.parameters = {{"output.gain", -6}}; imported.rightParameters = {{"output.gain", -12}};
    if (!check(bool(donor.prepare(48000, 64)) && bool(donor.applySession(savedSession)) &&
               bool(runtime.prepare(48000, 64)) && bool(runtime.applySession(original)),
               "independent state source and live session prepare for atomic dual-mono import")) return;
    std::vector<AudioPluginCheckpoint> checkpoints;
    if (!check(bool(donor.capturePluginCheckpoints(checkpoints)) && checkpoints.size() == 1 && checkpoints[0].right,
               "opaque import preserves independently captured left/right checkpoints")) return;
    const auto graph = runtime.routingGraph();
    const auto healthy = runtime.pluginInstanceId({"source", "eq"});
    auto candidate = original;
    auto replacement = imported;
    replacement.parameters = replacement.rightParameters = {{"output.gain", 9}};
    candidate.pluginChains[0].slots.push_back(replacement);
    auto invalid = checkpoints; invalid.push_back(checkpoints.front()); invalid.back().slotId = "absent";
    check(!runtime.applySession(candidate, false, {}, invalid) && runtime.routingGraph() == graph,
          "all checkpoint addresses are validated before any native candidate is restored");
    invalid = checkpoints; invalid.front().right->state = {1, 2, 3};
    const auto before = runtime.diagnostics();
    check(!runtime.applySession(candidate, false, {}, invalid) && runtime.routingGraph() == graph &&
          runtime.pluginInstanceId({"source", "eq"}) == healthy && runtime.diagnostics().gatedBlocks == before.gatedBlocks,
          "a rejected right opaque chunk discards the prepared left side without disturbing the healthy graph");
    check(bool(runtime.applySession(candidate, false, {}, checkpoints)) &&
          runtime.pluginInstanceId({"source", "eq"}) == healthy &&
          runtime.pluginParameter({"source", "imported"}, "output.gain") == -6 &&
          runtime.pluginParameter({"source", "imported", true}, "output.gain") == -12,
          "both opaque sides and their pending values are applied before a single publication");
    const auto committed = runtime.routingGraph();
    check(!runtime.applySession(candidate, false, {}, checkpoints) && runtime.routingGraph() == committed,
          "checkpoint import cannot mutate an already published healthy slot");
}

#ifdef DAW_FAULT_CLAP_PATH
void stagedOpaqueState() {
    using namespace std::chrono_literals;
    AudioRuntime runtime;
    auto original = session();
    original.hosting = {plugins::HostingMode::Isolated, DAW_PLUGIN_HOST_PATH};
    original.channels.clear(); original.graph.channels[0].acceptsMidi = false;
    original.graph.channels[0].input.enabled = true; original.graph.channels[0].sends.clear();
    if (!check(bool(runtime.prepare(48000, 512)) && bool(runtime.applySession(original)),
               "opaque import fixture publishes a healthy monitored session")) return;
    runtime.configureWorkersForTest(false, 1);
    const auto healthy = runtime.pluginInstanceId({"source", "eq"});
    auto candidate = original;
    AudioPluginSpec processor;
    processor.id = "imported"; processor.uid = "com.daw.test.fault.state_slow"; processor.name = "State import";
    processor.requiredFormat = processor.descriptor.format = plugins::Format::Clap;
    processor.descriptor.uid = processor.uid; processor.descriptor.name = processor.name;
    processor.descriptor.path = DAW_FAULT_CLAP_PATH;
    processor.parameters = {{"1", .9}}; // Opaque state is authoritative.
    candidate.pluginChains[0].slots.push_back(processor);
    AudioPluginStateEdit imported;
    imported.address = {"source", "imported"};
    imported.state.clearPending = true;
    imported.parameters = processor.parameters;

    struct Observation {
        std::chrono::steady_clock::time_point at;
        bool complete, unchanged;
    };
    const auto observe = [&](const AudioPluginStateEdit& state, bool expectedSuccess) {
        std::vector<Observation> observations;
        std::latch ready(1);
        std::jthread audioThread([&](std::stop_token stop) {
            audio::AudioBuffer input(2, 512), output(2, 512);
            for (unsigned channel = 0; channel < 2; ++channel) std::fill_n(input.getChannel(channel), 512, .5f);
            audio::AudioCallbackContext context;
            context.inputBuffer = &input; context.outputBuffer = &output; context.numFrames = 512; context.sampleRate = 48000;
            for (unsigned i = 0; i < 16; ++i) runtime.processDeviceBlock(context);
            ready.count_down();
            while (!stop.stop_requested()) {
                runtime.processDeviceBlock(context);
                bool unchanged = true;
                for (unsigned channel = 0; channel < 2; ++channel)
                    for (unsigned i = 0; i < 512; ++i)
                        unchanged &= std::abs(output.getChannel(channel)[i] - .5f) < .0001f;
                observations.push_back({std::chrono::steady_clock::now(),
                    context.renderStatus == audio::AudioCallbackContext::RenderStatus::Complete, unchanged});
                std::this_thread::sleep_for(5ms);
            }
        });
        ready.wait();
        const auto before = runtime.diagnostics();
        const auto applied = runtime.applySession(candidate, false, std::span(&state, 1));
        const auto finished = std::chrono::steady_clock::now();
        audioThread.request_stop(); audioThread.join();
        const auto stableUntil = expectedSuccess ? finished - 100ms : finished;
        unsigned observed = 0, changed = 0;
        for (const auto& value : observations) if (value.at < stableUntil) {
            ++observed; changed += !value.complete || !value.unchanged;
        }
        check(bool(applied) == expectedSuccess && observed >= 20 && changed == 0,
              expectedSuccess ? "slow successful opaque import keeps the old callback sounding until final publication"
                              : "slow rejected opaque import keeps every observed old callback complete and unchanged");
        if (!expectedSuccess) check(runtime.diagnostics().gatedBlocks == before.gatedBlocks,
            "rejected opaque state never acquires the live render gate");
        return bool(applied);
    };
    const auto graph = runtime.routingGraph();
    imported.state.state = {1, 2, 3}; // Real CLAP state reader rejects truncation after its delay.
    observe(imported, false);
    check(runtime.routingGraph() == graph && runtime.pluginInstanceId({"source", "eq"}) == healthy &&
          !runtime.hasPlugin(imported.address), "failed opaque import does not publish or replace any live processor");
    imported.state.state.assign(32768, 0);
    const double restoredGain = .25;
    std::memcpy(imported.state.state.data(), &restoredGain, sizeof(restoredGain));
    if (!observe(imported, true)) return;
    check(runtime.pluginInstanceId({"source", "eq"}) == healthy && runtime.pluginParameter(imported.address, "1") == .25,
          "new processor is published with its opaque gain, without replaying stale plain fallback");
    audio::AudioBuffer input(2, 512), output(2, 512);
    for (unsigned channel = 0; channel < 2; ++channel) std::fill_n(input.getChannel(channel), 512, .5f);
    audio::AudioCallbackContext context;
    context.inputBuffer = &input; context.outputBuffer = &output; context.numFrames = 512; context.sampleRate = 48000;
    for (unsigned i = 0; i < 16; ++i) runtime.processDeviceBlock(context);
    check(context.renderStatus == audio::AudioCallbackContext::RenderStatus::Complete &&
          std::abs(output.getChannel(0)[511] - .125f) < .0001f && std::abs(output.getChannel(1)[511] - .125f) < .0001f,
          "the first published native state remains authoritative after real audio blocks consume pending events");
    const auto committed = runtime.routingGraph();
    check(!runtime.applySession(candidate, false, std::span(&imported, 1)) && runtime.routingGraph() == committed &&
          runtime.pluginParameter(imported.address, "1") == .25,
          "atomic imports reject an already healthy target instead of mutating its published processor");
}

void stagingContinuity() {
    using namespace std::chrono_literals;
    AudioRuntime runtime;
    auto initial = session();
    initial.hosting = {plugins::HostingMode::Isolated, DAW_PLUGIN_HOST_PATH};
    initial.channels.clear(); // Monitor a constant hardware input while stopped.
    initial.graph.channels[0].acceptsMidi = false;
    initial.graph.channels[0].input.enabled = true;
    initial.graph.channels[0].sends.clear();
    if (!check(bool(runtime.prepare(48000, 512)) && bool(runtime.applySession(initial)),
               "staging continuity fixture publishes its original monitored session")) return;
    runtime.configureWorkersForTest(false, 1);
    const auto original = runtime.pluginInstanceId({"source", "eq"});
    const auto graph = runtime.routingGraph();
    const auto before = runtime.diagnostics();

    auto candidate = initial;
    candidate.graph.channels[0].volume = .1f; // Must never reach the live graph.
    AudioPluginSpec hung;
    hung.id = "activation-hang"; hung.uid = "com.daw.test.fault.activate";
    hung.name = "Hung activation";
    hung.requiredFormat = hung.descriptor.format = plugins::Format::Clap;
    hung.descriptor.path = DAW_FAULT_CLAP_PATH;
    hung.descriptor.uid = hung.uid; hung.descriptor.name = hung.name;
    candidate.pluginChains[0].slots.push_back(std::move(hung));

    std::latch ready(1);
    std::atomic<bool> observing{false};
    std::uint64_t callbacks = 0, incomplete = 0, changedSamples = 0;
    std::jthread audioThread([&](std::stop_token stop) {
        audio::AudioBuffer input(2, 512), output(2, 512);
        for (unsigned channel = 0; channel < 2; ++channel)
            std::fill_n(input.getChannel(channel), 512, .5f);
        audio::AudioCallbackContext context;
        context.inputBuffer = &input; context.outputBuffer = &output;
        context.numFrames = 512; context.sampleRate = 48000;
        // Settle the normal monitor/fader de-click ramps before measuring.
        for (unsigned block = 0; block < 16; ++block) runtime.processDeviceBlock(context);
        ready.count_down();
        while (!stop.stop_requested()) {
            runtime.processDeviceBlock(context);
            if (observing.load(std::memory_order_acquire)) {
                ++callbacks;
                incomplete += context.renderStatus != audio::AudioCallbackContext::RenderStatus::Complete;
                for (unsigned channel = 0; channel < 2; ++channel)
                    for (unsigned frame = 0; frame < 512; ++frame)
                        changedSamples += !std::isfinite(output.getChannel(channel)[frame]) ||
                            std::abs(output.getChannel(channel)[frame] - .5f) > .0001f;
            }
            std::this_thread::sleep_for(5ms);
        }
    });
    ready.wait();
    observing.store(true, std::memory_order_release);
    const auto started = std::chrono::steady_clock::now();
    const auto applied = runtime.applySession(std::move(candidate));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    observing.store(false, std::memory_order_release);
    audioThread.request_stop();
    audioThread.join();

    check(!applied && applied.message().find("Could not activate plugin:") != std::string::npos &&
          elapsed >= 1s && elapsed < 30s,
          "a real isolated activation hang fails within its bounded control deadline");
    check(callbacks >= 50 && incomplete == 0 && changedSamples == 0,
          "the callback keeps rendering unchanged 0.5 audio throughout native activation failure");
    const auto after = runtime.diagnostics();
    check(after.gatedBlocks == before.gatedBlocks && after.failedBlocks == before.failedBlocks,
          "preparing a hung plugin never parks or fails the live render callback");
    check(runtime.routingGraph() == graph && runtime.pluginInstanceId({"source", "eq"}) == original &&
          !runtime.hasPlugin({"source", "activation-hang"}),
          "a failed staged edit leaves the published graph and healthy plugin identity intact");
}
#endif
} // namespace
int main() {
    transactions();
    atomicCheckpointImports();
#ifdef DAW_FAULT_CLAP_PATH
    stagingContinuity();
    stagedOpaqueState();
#endif
    return failures ? 1 : 0;
}
