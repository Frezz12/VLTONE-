#include "AudioRuntime.hpp"
#include "Graph/GraphProcessor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace {
using namespace daw;
constexpr unsigned frames = 64;
int failures = 0;

bool check(bool ok, const char* description) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", description);
    failures += !ok;
    return ok;
}

bool near(float a, float b) { return std::abs(a - b) < 2e-6f; }

const engine::CompiledGraph::CompiledNode* findNode(const engine::CompiledGraph& graph, engine::NodeId id) {
    const auto found = std::find_if(graph.nodes.begin(), graph.nodes.end(),
                                  [id](const auto& node) { return node.id == id; });
    return found == graph.nodes.end() ? nullptr : &*found;
}

bool hasNamedNode(const engine::CompiledGraph& graph, std::string_view name) {
    return std::any_of(graph.nodes.begin(), graph.nodes.end(),
                      [name](const auto& node) { return node.node->name().find(name) != std::string_view::npos; });
}

struct DeviceBlock {
    audio::AudioBuffer input{2, frames}, output{2, frames};
    audio::AudioCallbackContext context;
    DeviceBlock() {
        input.clear(); output.clear();
        context.inputBuffer = &input; context.outputBuffer = &output;
        context.numFrames = frames; context.sampleRate = 48000;
    }
    void fill(float left, float right) {
        std::fill_n(input.getChannel(0), frames, left);
        std::fill_n(input.getChannel(1), frames, right);
    }
    bool render(AudioRuntime& runtime, unsigned blocks = 16) {
        for (unsigned i = 0; i < blocks; ++i) {
            runtime.processDeviceBlock(context);
            if (context.renderStatus != audio::AudioCallbackContext::RenderStatus::Complete) return false;
        }
        return true;
    }
    bool equals(float left, float right) const {
        for (unsigned i = 0; i < frames; ++i)
            if (!near(output.getChannel(0)[i], left) || !near(output.getChannel(1)[i], right)) return false;
        return true;
    }
};

void routingAndPublication() {
    AudioRuntime runtime;
    if (!check(bool(runtime.prepare(48000, frames)), "runtime prepares without a controller or device")) return;
    AudioGraphSpec spec;
    AudioGraphSpec::Channel source, bus, aux;
    source.id = "source"; source.name = "Source";
    source.input = {true, true, 0, 2, 3};
    source.volume = .5f; source.outputBusId = "bus";
    source.sends.push_back({"send", "aux", .25f, true, true});
    bus.id = "bus"; bus.name = "Bus"; bus.volume = .8f;
    aux.id = "aux"; aux.name = "Aux"; aux.volume = .4f;
    spec.channels = {source, bus, aux}; spec.masterVolume = .5f;
    runtime.buildGraph(spec);
    if (!check(bool(runtime.commitGraph()), "runtime publishes hardware input, bus and aux routes")) return;
    const auto first = runtime.routingGraph();
    const auto sourceFader = findNode(*first, runtime.trackNodes("source")->fader)->owner;
    check(first->totalLatency == 0, "routing introduces no sample latency");
    check(runtime.trackNodes("bus")->sum != engine::kInvalidNode &&
          runtime.trackNodes("aux")->sum != engine::kInvalidNode,
          "bus and send receivers have their own merge points");

    DeviceBlock block;
    block.fill(.4f, .2f);
    check(block.render(runtime) && block.equals(.1f, .05f),
          "pre-fader send and bus arrive at the master at their exact levels");
    block.fill(0, 0);
    check(block.render(runtime), "input route settles before impulse measurement");
    block.input.getChannel(0)[17] = 1;
    block.input.getChannel(1)[31] = 1;
    bool immediate = block.render(runtime, 1);
    for (unsigned i = 0; i < frames; ++i)
        immediate &= near(block.output.getChannel(0)[i], i == 17 ? .25f : 0) &&
                     near(block.output.getChannel(1)[i], i == 31 ? .25f : 0);
    check(immediate, "both impulses leave in the same block at the original sample offsets");

    spec.channels[0].sends[0].preFader = false;
    runtime.buildGraph(spec);
    check(runtime.routingGraph() == first, "assembly alone does not publish a partial graph");
    if (!check(bool(runtime.commitGraph()), "post-fader routing publishes")) return;
    const auto second = runtime.routingGraph();
    check(findNode(*second, runtime.trackNodes("source")->fader)->owner == sourceFader,
          "topology changes reuse existing DSP objects");
    block.fill(.4f, .2f);
    check(block.render(runtime) && block.equals(.09f, .045f), "post-fader send follows source volume");

    spec.channels[1].outputBusId = "source";
    runtime.buildGraph(spec);
    const auto failed = runtime.commitGraph();
    check(!failed && !failed.message().empty() && runtime.routingGraph() == second,
          "a cyclic topology is rejected without replacing the working snapshot");
    check(block.render(runtime) && block.equals(.09f, .045f), "working audio continues after rejected publication");
    spec.channels[1].outputBusId.clear();
    spec.channels[0].outputBusId.clear();
    spec.channels.erase(spec.channels.begin() + 1);
    runtime.buildGraph(spec);
    check(!runtime.trackNodes("bus") && runtime.trackNodes("source"), "removed channels leave runtime lookup");
    check(bool(runtime.commitGraph()) && hasNamedNode(*first, "Bus Fader"),
          "an earlier snapshot retains nodes after channel removal");
}

void ownedResourcesAndDormantClips() {
    AudioRuntime runtime;
    if (!check(bool(runtime.prepare(48000, frames)), "resource runtime prepares")) return;
    std::weak_ptr<const engine::SampleBuffer> resource;
    {
        AudioGraphSpec spec;
        auto& track = spec.channels.emplace_back();
        track.id = "frozen"; track.name = "Frozen source";
        std::vector<float> samples(1024);
        for (std::size_t i = 0; i < samples.size(); i += 2) {
            samples[i] = .25f; samples[i + 1] = -.5f;
        }
        track.frozenAudio = engine::SampleBuffer::fromInterleaved(samples, 2, 512, 48000);
        track.frozenFrames = 512;
        resource = track.frozenAudio;
        runtime.buildGraph(spec);
    }
    if (!check(!resource.expired() && bool(runtime.commitGraph()),
               "runtime owns frozen resources after the description has been destroyed")) return;
    const auto frozen = runtime.routingGraph();
    runtime.buildGraph({});
    check(bool(runtime.commitGraph()) && !runtime.trackNodes("frozen") && !resource.expired(),
          "removing a frozen track preserves the resource in its outstanding snapshot");
    engine::GraphProcessor processor(1);
    processor.setGraph(frozen);
    audio::AudioBuffer output(2, frames);
    float* planes[] = {output.getChannel(0), output.getChannel(1)};
    bool rendered = bool(processor.processSerial({planes, 2, frames}, frames, 128, true, true));
    for (unsigned i = 0; i < frames; ++i)
        rendered &= near(planes[0][i], .25f) && near(planes[1][i], -.5f);
    check(rendered, "outstanding frozen snapshot still renders its owned audio");
    processor.setGraph({});

    AudioGraphSpec spec;
    auto& channel = spec.channels.emplace_back();
    channel.id = "capture"; channel.name = "Capture";
    channel.clipFx.push_back({"clip", "Private", 1, 0, {}});
    runtime.buildGraph(spec);
    check(bool(runtime.commitGraph()) && hasNamedNode(*runtime.routingGraph(), "Private Player"),
          "private clip topology is present before recording");
    channel.capturing = true;
    runtime.buildGraph(spec);
    check(bool(runtime.commitGraph()) && !hasNamedNode(*runtime.routingGraph(), "Private"),
          "recording omits the complete previous-take chain");
    channel.capturing = false;
    runtime.buildGraph(spec);
    check(bool(runtime.commitGraph()) && hasNamedNode(*runtime.routingGraph(), "Private Player"),
          "stopping recording restores the private clip topology");
}
void sessionContentAndIncrementalUpdates() {
    AudioRuntime runtime;
    if (!check(bool(runtime.prepare(48000, frames)), "session runtime prepares")) return;
    std::weak_ptr<const engine::SampleBuffer> resource;
    auto notes = std::make_shared<engine::MidiClipPlayerNode::NoteList>();
    engine::MidiNote note;
    note.startBeats = 21.0 / 24000.0; note.lengthBeats = 1; note.key = 60; note.noteId = 123;
    notes->push_back(note);
    auto controls = std::make_shared<engine::MidiClipPlayerNode::ControlCurves>();
    controls->push_back({0, 1, .25, 1, 0, 0, {{0, .25}}});
    {
        AudioSessionSpec session;
        auto& source = session.graph.channels.emplace_back();
        source.id = "play"; source.name = "Playback";
        source.clipFx.push_back({"private", "Private", .5f, 0, {}});
        // An unavailable destination creates a hole in the executable send IDs.
        source.sends = {{"unavailable", "missing", 1, true, true}, {"aux-send", "aux", .5f, true, true}};
        auto& aux = session.graph.channels.emplace_back(); aux.id = "aux"; aux.name = "Aux";
        auto& midi = session.graph.channels.emplace_back(); midi.id = "notes"; midi.name = "Notes"; midi.acceptsMidi = true;
        auto samples = engine::SampleBuffer::fromInterleaved(std::vector<float>(2048, .2f), 2, 1024, 48000);
        resource = samples;
        auto clips = std::make_shared<engine::ClipPlayerNode::ClipList>();
        engine::ClipPlacement clip; clip.audio = samples; clip.lengthSamples = 1024;
        clips->push_back(clip);
        auto privateClips = std::make_shared<engine::ClipPlayerNode::ClipList>(*clips);
        privateClips->front().gain = 2;
        auto& playback = session.channels.emplace_back(); playback.id = "play";
        playback.content.clips = AudioContentSpec::Clips{clips, {{"private", privateClips}}};
        auto levels = std::make_shared<engine::LevelAutomation>(); levels->gain.active = true; levels->gain.defaultValue = .5;
        auto send = std::make_shared<engine::LevelCurve>(); send->active = true; send->defaultValue = .25;
        playback.content.levels = AudioContentSpec::Levels{levels, {{"aux-send", send}}};
        auto& performance = session.channels.emplace_back(); performance.id = "notes";
        performance.content.midi = AudioContentSpec::Midi{notes, controls, false};
        runtime.buildSession(std::move(session));
    }
    if (!check(bool(runtime.commitGraph()) && !resource.expired(),
               "session owns clips, private audio, notes and curves after its source is destroyed")) return;
    const auto graph = runtime.routingGraph();
    engine::GraphProcessor processor(1); processor.setGraph(graph);
    audio::AudioBuffer output(2, frames);
    float* planes[] = {output.getChannel(0), output.getChannel(1)};
    const auto render = [&](engine::SamplePos position, float level) {
        bool correct = bool(processor.processSerial({planes, 2, frames}, frames, position, true, true));
        for (unsigned i = 0; i < frames; ++i) correct &= near(planes[0][i], level) && near(planes[1][i], level);
        if (!correct) std::printf("  at %lld: L %.6f..%.6f, R %.6f..%.6f; expected %.6f\n",
            static_cast<long long>(position), planes[0][0], planes[0][frames - 1],
            planes[1][0], planes[1][frames - 1], level);
        return correct;
    };
    // Static faders retain their existing one-block smoothing, including the
    // private clip fader on first use and a return from automation to manual.
    check(bool(processor.processSerial({planes, 2, frames}, frames, 0, true, true)),
          "session starts audio and MIDI in its first block");
    const auto* midiNode = findNode(*graph, runtime.trackNodes("notes")->midiClips);
    const auto& events = graph->midiBuffers[midiNode->midiOutputBuffer];
    bool noteAtOffset = false, controller = false;
    for (const auto& event : events.events()) {
        noteAtOffset |= event.isNoteOn() && event.data1 == 60 && event.frameOffset == 21;
        controller |= (event.status & 0xf0) == 0xb0 && event.data1 == 1 && event.data2 == 32;
    }
    check(noteAtOffset && controller, "prepared MIDI notes and controller curves retain sample positions");
    check(render(64, .3f), "session renders shared/private audio and automates the correct send across a missing destination");

    AudioContentSpec edit;
    edit.levels.emplace();
    check(runtime.applyContent("play", std::move(edit)) && runtime.routingGraph() == graph &&
          bool(processor.processSerial({planes, 2, frames}, frames, 128, true, true)) && render(192, .6f),
          "clearing automation restores static levels without rebuilding or erasing clips");
    edit = {}; edit.clips.emplace();
    check(runtime.applyContent("play", std::move(edit)) && render(256, 0),
          "an empty clip section clears both shared and private players");
    edit = {}; edit.midi = AudioContentSpec::Midi{notes, controls, true};
    runtime.applyContent("notes", std::move(edit));
    check(runtime.sendLiveMidi("notes", engine::MidiEvent::noteOn(0, 0, 72, 100)) && render(320, 0),
          "live MIDI remains available while timeline playback is suppressed");
    bool released = false, live = false, oldRestarted = false;
    for (const auto& event : events.events()) {
        released |= event.isNoteOff() && event.data1 == 60;
        live |= event.isNoteOn() && event.data1 == 72;
        oldRestarted |= event.isNoteOn() && event.data1 == 60;
    }
    check(released && live && !oldRestarted, "capture suppression releases the timeline voice and keeps the live voice");
    check(!runtime.applyContent("missing", {}) &&
          !runtime.sendLiveMidi("missing", engine::MidiEvent::noteOff(0, 0, 72)) && runtime.routingGraph() == graph,
          "updates for a removed channel do not alter the published session");
}

void pluginLifecycle() {
    AudioRuntime runtime;
    if (!check(bool(runtime.prepare(48000, frames)), "plugin runtime prepares without a controller")) return;
    AudioSessionSpec session;
    AudioGraphSpec::Channel channel;
    channel.id = "plugins"; channel.name = "Plugins";
    channel.input = {true, true, 0, 2, 3};
    session.graph.channels.push_back(channel);
    AudioPluginChainSpec chain;
    chain.channelId = channel.id;
    AudioPluginSpec slot;
    slot.id = "eq"; slot.uid = "daw.equalizer"; slot.name = "Runtime EQ";
    slot.descriptor.format = slot.requiredFormat = plugins::Format::Internal;
    slot.descriptor.uid = slot.uid; slot.descriptor.name = slot.name;
    slot.channelMode = PluginChannelMode::DualMono; slot.preferredChannels = 1;
    slot.parameters = {{"output.gain", -6.0}};
    slot.rightParameters = {{"output.gain", 3.0}};
    chain.slots.push_back(slot);
    session.pluginChains.push_back(chain);
    check(runtime.buildSession(session), "a value session creates plugins and restores stored parameters");
    if (!check(bool(runtime.commitGraph()), "runtime-created dual mono plugins compile")) return;
    const auto pluginNodes = [&] {
        std::vector<plugins::PluginNode*> result;
        for (const auto& node : runtime.routingGraph()->nodes)
            if (auto* plugin = dynamic_cast<plugins::PluginNode*>(node.node)) result.push_back(plugin);
        std::sort(result.begin(), result.end(), [](const auto* a, const auto* b) { return a->name() < b->name(); });
        return result;
    };
    auto nodes = pluginNodes();
    if (!check(nodes.size() == 2, "one dual mono slot creates exactly two native processors")) return;
    const auto parameter = nodes[0]->instance()->parameterIndexForId("output.gain");
    if (!check(parameter >= 0, "runtime resolves stable plugin parameter IDs")) return;
    DeviceBlock io;
    io.fill(.1f, .1f);
    check(io.render(runtime), "runtime-created plugins execute through the device callback");
    check(near(float(nodes[0]->instance()->parameterValue(parameter)), -6) &&
          near(float(nodes[1]->instance()->parameterValue(parameter)), 3) &&
          nodes[0]->preferredChannelCount() == 1 && nodes[1]->preferredChannelCount() == 1,
          "dual mono restores independent left and right state and channel layouts");
    const auto left = nodes[0]->instanceId(), right = nodes[1]->instanceId();
    const AudioPluginAddress leftAddress{"plugins", "eq", false, left};
    const AudioPluginAddress rightAddress{"plugins", "eq", true, right};
    const auto publication = runtime.routingGraph();
    const auto capabilities = runtime.pluginCapabilities(leftAddress);
    const auto parameterInfo = runtime.pluginParameterInfo(leftAddress, "output.gain");
    check(capabilities.available && capabilities.dualMono && parameterInfo &&
          parameterInfo->id == "output.gain" &&
          runtime.setPluginControls("plugins", "eq", {.mix = 0.f}) &&
          runtime.routingGraph() == publication && io.render(runtime) && io.equals(.1f, .1f),
          "value controls set both native sides to dry without graph publication");
    runtime.setPluginControls("plugins", "eq", {.mix = 1.f});
    check(runtime.setPluginParameter(leftAddress, "output.gain", -3),
          "parameter commands resolve a stable slot and native instance identity");
    std::array<PluginParameterReadout, 2> readout{{{"output.gain", 999}, {"missing", parameter}}};
    runtime.readPluginParameters(leftAddress, readout);
    const auto rightEditor = runtime.pluginEditorSnapshot(rightAddress);
    check(readout[0].available && near(float(readout[0].value), -3) && !readout[1].available &&
          rightEditor && rightEditor->identity.instance == right &&
          runtime.equalizerSnapshot(leftAddress, false).has_value(),
          "plugin readouts validate index hints and expose values for the requested native side");
    session.pluginChains[0].slots[0].parameters[0].value = -15;
    session.pluginChains[0].slots[0].bypassed = true;
    check(runtime.retiringPlugins(session.pluginChains).empty() && runtime.hasInputRoute("plugins"),
          "projection facts describe retained slots and existing input routes without native pointers");
    runtime.buildSession(session);
    check(bool(runtime.commitGraph()), "a topology update keeps the loaded plugin chain usable");
    nodes = pluginNodes();
    check(nodes.size() == 2 && nodes[0]->instanceId() == left && nodes[1]->instanceId() == right &&
          nodes[0]->isBypassed() && nodes[1]->isBypassed() &&
          near(float(nodes[0]->instance()->parameterValue(parameter)), -3),
          "reconciliation keeps native state and identities while updating wrapper settings");

    auto differentFormat = session.pluginChains[0];
    differentFormat.slots[0].requiredFormat = plugins::Format::Vst3;
    const auto changedFormat = runtime.retiringPlugins(std::span(&differentFormat, 1));
    check(changedFormat.size() == 1 && changedFormat[0].slotId == "eq",
          "an equal UID from another format cannot reuse the previous native processor");
    const auto oldGraph = runtime.routingGraph();
    session.pluginChains[0].slots.clear();
    const auto removedSlot = runtime.retiringPlugins(session.pluginChains);
    check(removedSlot.size() == 1 && removedSlot[0].slotId == "eq",
          "slot removal is announced before either native side is retired");
    const auto retiring = runtime.retiringPlugins({});
    check(retiring.size() == 1 && retiring[0].channelId == "plugins" && retiring[0].slotId == "eq",
          "an omitted whole chain emits one retirement notice for both native sides");
    runtime.buildSession(session);
    check(bool(runtime.commitGraph()) && pluginNodes().empty() &&
          nodes[0]->instanceId() == left && nodes[1]->instanceId() == right,
          "removed processors survive only in retained graph snapshots");
    session.pluginChains[0].slots.push_back(slot);
    runtime.buildSession(session);
    check(bool(runtime.commitGraph()) && runtime.hasPlugin({"plugins", "eq"}) &&
          !runtime.setPluginParameter(leftAddress, "output.gain", 12) &&
          !runtime.pluginEditorSnapshot(rightAddress) && !runtime.closePluginEditor(leftAddress, false) &&
          !runtime.setPluginAutomationOverride(leftAddress, "output.gain"),
          "delayed edits and editor commands cannot reach a replacement with the same slot ID");
    const auto beforeReload = runtime.routingGraph();
    nodes = pluginNodes();
    const auto reloadLeft = nodes[0]->instanceId(), preservedRight = nodes[1]->instanceId();
    runtime.setPluginParameter({"plugins", "eq", false, reloadLeft}, "output.gain", -9);
    nodes[0]->onReloadRequested();
    const auto service = runtime.servicePlugins();
    check(service.changed && service.scanned && service.error.empty() &&
          runtime.pluginInstanceId({"plugins", "eq"}) != reloadLeft &&
          runtime.pluginInstanceId({"plugins", "eq", true}) == preservedRight &&
          runtime.pluginParameter({"plugins", "eq"}, "output.gain") == -9 && io.render(runtime),
          "standalone runtime reload preserves pending parameters and the healthy native side");
}
void runtimeControls() {
    AudioRuntime runtime;
    if (!check(bool(runtime.prepare(48000, frames)), "control runtime prepares")) return;
    AudioGraphSpec spec;
    AudioGraphSpec::Channel source;
    source.id = "input";
    source.input = {true, true, 0, 2, 3};
    spec.channels.push_back(source);
    runtime.buildGraph(spec);
    if (!check(bool(runtime.commitGraph()), "control runtime publishes its input")) return;
    using Action = AudioTransportCommand::Action;
    runtime.transportCommand({.action = Action::Tempo, .value = 137});
    runtime.transportCommand({.action = Action::Duration, .position = 96000});
    runtime.transportCommand({.action = Action::LoopRange, .position = 1000, .end = 1100});
    runtime.transportCommand({.action = Action::LoopEnabled, .enabled = true});
    runtime.transportCommand({.action = Action::Seek, .position = 2000});
    const auto stopped = runtime.transportSnapshot();
    runtime.transportCommand({.action = Action::StartPlayback});
    check(stopped.position == 2000 && !stopped.playing &&
          runtime.transportSnapshot().position == 1000 && runtime.transportSnapshot().playing,
          "owned transport snapshot survives start and loop admission remains sample exact");
    DeviceBlock block;
    block.fill(.2f, .4f);
    check(block.render(runtime, 2) && runtime.transportSnapshot().position == 1028,
          "transport commands reach the device callback and loop within the current block");
    const auto meter = runtime.meterSnapshot("input");
    check(near(meter.left, .2f) && near(meter.right, .4f) && meter.hold >= .4f &&
          runtime.meterSnapshot("missing").left == 0 &&
          runtime.meterSnapshot("input", "missing").right == 0,
          "meter snapshots report channel values and missing owners return silence");
    const auto counters = runtime.timingSnapshot(false, false);
    const auto drained = runtime.timingSnapshot(false, true);
    check(counters.counters.blocks == 2 && counters.events.empty() && drained.events.size() == 2 &&
          runtime.timingSnapshot(false, true).events.empty(),
          "timing reads preserve the stream until an explicit bounded drain");
    runtime.transportCommand({.action = Action::Pause});
    check(!runtime.transportSnapshot().playing && runtime.transportSnapshot().position == 1028,
          "pause preserves the playhead without exposing the live transport");
    runtime.transportCommand({.action = Action::SeekSeconds, .value = 21.0 / 48000, .prepare = true});
    runtime.transportCommand({.action = Action::Tempo, .value = std::numeric_limits<double>::quiet_NaN()});
    runtime.transportCommand({.action = Action::SeekSeconds, .value = std::numeric_limits<double>::infinity()});
    check(runtime.transportSnapshot().position == 21 && runtime.transportSnapshot().tempo == 137 &&
          runtime.transportSnapshot().duration == 96000,
          "seek rounds to the original sample and invalid values cannot poison transport");
    runtime.transportCommand({.action = Action::Record});
    check(runtime.transportSnapshot().recording && runtime.transportSnapshot().playing,
          "record is distinguishable from ordinary playback in a value snapshot");
    runtime.transportCommand({.action = Action::Stop});
    check(!runtime.transportSnapshot().playing && !runtime.transportSnapshot().recording &&
          !runtime.deviceSnapshot().hasStream,
          "stop leaves a usable runtime without opening hardware");
}
} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    routingAndPublication();
    ownedResourcesAndDormantClips();
    sessionContentAndIncrementalUpdates();
    pluginLifecycle();
    runtimeControls();
    return failures ? 1 : 0;
}
