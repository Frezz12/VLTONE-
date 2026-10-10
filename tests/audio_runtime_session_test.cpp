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


} // namespace
int main() {
    transactions();
    atomicCheckpointImports();

    return failures ? 1 : 0;
}
