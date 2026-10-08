#include "AudioRuntime.hpp"
#include "AudioSessionCodec.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>

namespace {
using namespace daw;
int failures = 0;
bool check(bool value, const char* message) {
    std::printf("%s %s\n", value ? "PASS" : "FAIL", message);
    failures += !value;
    return value;
}
template<class F> bool rejected(F&& action) {
    try { action(); return false; } catch (const std::exception&) { return true; }
}
struct Directory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("daw-session-codec-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Directory() { std::filesystem::create_directory(path); }
    ~Directory() { std::error_code error; std::filesystem::remove_all(path, error); }
};

AudioSessionPacket session() {
    AudioSessionPacket packet;
    packet.generation = 37; packet.revision = 91; packet.blockSize = 64;
    auto& graph = packet.session.graph;
    AudioGraphSpec::Channel channel;
    channel.id = "source"; channel.name = "Source"; channel.acceptsMidi = true;
    channel.input = {true, true, 0, 2, 3};
    channel.clipFx.push_back({"private", "Private", .75f, -.1f, {}});
    channel.sends.push_back({"send", "aux", .25f, true, true});
    graph.channels.push_back(channel);
    AudioGraphSpec::Channel aux; aux.id = "aux"; graph.channels.push_back(aux);
    auto audio = std::make_shared<engine::SampleBuffer>(2, 1024, 48000);
    for (unsigned i = 0; i < 1024; ++i) {
        audio->writableChannel(0)[i] = float(i) / 2048;
        audio->writableChannel(1)[i] = -float(i) / 2048;
    }
    auto clips = std::make_shared<engine::ClipPlayerNode::ClipList>();
    engine::ClipPlacement placement;
    placement.audio = audio; placement.clipId = "placement";
    placement.startSample = 17; placement.offsetSamples = 3; placement.lengthSamples = 100;
    placement.fadeInSamples = 4; placement.fadeOutSamples = 9; placement.fadeEqualPower = true;
    placement.tapeStartSamples = 5; placement.tapeStopSamples = 8;
    placement.sourceStartFrame = 2.5; placement.sourceEndFrame = 100.5;
    placement.stretchMode = 2; placement.stretchTime = 1.25; placement.stretchPitch = -7.5;
    placement.formant = .3; placement.loopMode = 1; placement.loopStart = .1; placement.loopEnd = .9;
    placement.gain = .3f; placement.pan = -.2f;
    auto warp = std::make_shared<ClipWarpModel>();
    warp->enabled = true; warp->baselineDurationSeconds = .02;
    warp->markers = {{"start", 0, 0, true}, {"end", .02, 1, true}};
    placement.warp = warp; placement.warpTempo = 137;
    clips->push_back(placement);
    AudioSessionSpec::Channel content; content.id = "source";
    content.content.clips = AudioContentSpec::Clips{clips, {{"private", clips}}};
    auto notes = std::make_shared<engine::MidiClipPlayerNode::NoteList>();
    engine::MidiNote note;
    note.startBeats = .125; note.lengthBeats = 1.5; note.key = 72; note.noteId = 314;
    note.startOrder = 1234567890123ull; note.endOrder = 1234567890124ull;
    note.pitch.push_back({.25, 7.25, engine::curve::Shape::SCurve, -.4, .2, .8, -100});
    notes->push_back(note);
    auto controllers = std::make_shared<engine::MidiClipPlayerNode::ControlCurves>();
    controllers->push_back({0, 4, .5, 129, 1, 0, {{1, .75, engine::curve::Shape::Hold, 0, 9}}});
    content.content.midi = AudioContentSpec::Midi{notes, controllers, true};
    content.content.plugins = AudioContentSpec::PluginCurves{{"eq", "output.gain", .5, {{0, .25}, {1, .75}}}};
    auto levels = std::make_shared<engine::LevelAutomation>();
    levels->gain = {{{0, .5}, {2, 1}}, .75, true};
    auto send = std::make_shared<engine::LevelCurve>(); *send = {{{0, .3}}, .2, true};
    content.content.levels = AudioContentSpec::Levels{levels, {{"send", send}}};
    packet.session.channels.push_back(std::move(content));
    AudioPluginChainSpec chain; chain.channelId = "source";
    AudioPluginSpec eq;
    eq.id = "eq"; eq.uid = "daw.equalizer"; eq.name = "Equalizer";
    eq.requiredFormat = eq.descriptor.format = plugins::Format::Internal;
    eq.descriptor.uid = eq.uid; eq.descriptor.name = eq.name;
    eq.channelMode = PluginChannelMode::DualMono; eq.preferredChannels = 1;
    eq.parameters = {{"output.gain", -6}}; eq.rightParameters = {{"output.gain", 3}};
    chain.slots.push_back(eq); packet.session.pluginChains.push_back(chain);
    packet.transport.push_back({.action = AudioTransportCommand::Action::Tempo, .value = 137});
    return packet;
}

void codec() {
    Directory directory;
    ProcessAudioResources resources(directory.path);
    auto original = session();
    const auto bytes = encodeAudioSession(original, resources);
    auto decoded = decodeAudioSession(bytes, directory.path);
    const auto& content = decoded.session.channels.front().content;
    const auto& clip = content.clips->shared->front();
    check(resources.records().size() == 1 && clip.audio == content.clips->individual.at("private")->front().audio &&
          clip.audio != original.session.channels.front().content.clips->shared->front().audio &&
          clip.audio->readSample(0, 71) == 71.f / 2048 && clip.audio->readSample(1, 71) == -71.f / 2048,
          "one immutable PCM mapping serves all resource IDs after crossing the wire");
    check(decoded.generation == 37 && decoded.revision == 91 && decoded.blockSize == 64 &&
          clip.startSample == 17 && clip.offsetSamples == 3 && clip.sourceStartFrame == 2.5 &&
          clip.tapeStopSamples == 8 && clip.stretchPitch == -7.5 && clip.loopEnd == .9 &&
          clip.warp && *clip.warp == *original.session.channels.front().content.clips->shared->front().warp &&
          !clip.stretcher && !clip.warpPlayback,
          "clip wire preserves processing values and excludes live prepared DSP objects");
    check(content.midi->notes->front().startOrder == 1234567890123ull &&
          content.midi->notes->front().pitch.front().phaseTo == .8 &&
          content.midi->controllers->front().points.front().shape == engine::curve::Shape::Hold &&
          content.midi->timelineSuppressed && content.levels->sends.at("send")->active &&
          content.plugins->front().parameterId == "output.gain",
          "MIDI ordering, slide phase, controller curves and automation survive serialization");
    auto bad = bytes; bad[4] = 255;
    check(rejected([&] { decodeAudioSession(bad, directory.path); }), "unknown session version is rejected");
    bad = bytes; bad.resize(bad.size() - 1);
    check(rejected([&] { decodeAudioSession(bad, directory.path); }), "truncated session is rejected");
    bad = bytes; bad.push_back(0);
    check(rejected([&] { decodeAudioSession(bad, directory.path); }), "trailing session data is rejected");
    bad = bytes; std::fill(bad.begin() + 8, bad.begin() + 12, 255);
    check(rejected([&] { decodeAudioSession(bad, directory.path); }), "malformed allocation length is rejected before allocation");
    auto duplicate = original; duplicate.session.graph.channels.push_back(duplicate.session.graph.channels.front());
    check(rejected([&] { encodeAudioSession(duplicate, resources); }), "duplicate channel identities are rejected");
    ProcessAudioResources::Cache cache;
    const auto cached = decodeAudioSession(bytes, directory.path, &cache);
    const auto cachedSource = cached.session.channels.front().content.clips->shared->front().audio;
    auto unused = std::make_shared<engine::SampleBuffer>(1, 1, 48000);
    resources.put(unused);
    for (unsigned i = 0; i < 32; ++i) {
        ++original.revision;
        original.session.graph.channels.front().volume = float(i + 1) / 32.f;
        const auto updated = decodeAudioSession(encodeAudioSession(original, resources), directory.path, &cache);
        if (updated.session.channels.front().content.clips->shared->front().audio != cachedSource)
            check(false, "repeated session update unexpectedly remapped unchanged PCM");
    }
    check(resources.records().size() == 2 && cache.samples.size() == 1,
          "repeated value updates neither rewrite PCM nor include retired resources in each manifest");
    original = {};
    check(clip.audio->readSample(0, 71) == 71.f / 2048, "decoded resource survives disposal of the source session");
}

void controls() {
    AudioControlPacket packet; packet.generation = 7; packet.requestId = 11;
    auto event = engine::MidiEvent::noteOn(17, 2, 65, 110, -.4f, 52);
    event.isPitchExpression = true; event.pitch = {.from = 1.25, .to = 7.5, .phaseFrom = .2, .phaseTo = .8,
        .frames = 29, .shape = engine::curve::Shape::SCurve, .active = true};
    packet.command = AudioMidiCommand{"source", event};
    auto decoded = decodeAudioControl(encodeAudioControl(packet));
    const auto& midi = std::get<AudioMidiCommand>(decoded.command);
    check(decoded.generation == 7 && decoded.requestId == 11 && midi.event.frameOffset == 17 &&
          midi.event.noteId == 52 && midi.event.pitch.phaseTo == .8 && midi.event.pitch.frames == 29,
          "live MIDI command preserves generation, timestamp and pitch expression");
    packet.command = AudioFaderCommand{"source", "private", AudioFaderTarget::Clip, {.gain = .5f, .silent = false}};
    decoded = decodeAudioControl(encodeAudioControl(packet));
    const auto& fader = std::get<AudioFaderCommand>(decoded.command);
    check(fader.change.gain == .5f && fader.change.silent == false && !fader.change.pan && !fader.change.mono,
          "partial fader command preserves absent controls and explicit false");
    auto bad = encodeAudioControl(packet); bad[24] = 255;
    check(rejected([&] { decodeAudioControl(bad); }), "unknown control variant cannot execute");
    packet.command = AudioTransportCommand{.action = AudioTransportCommand::Action::Tempo,
        .value = std::numeric_limits<double>::quiet_NaN()};
    check(rejected([&] { encodeAudioControl(packet); }), "nonfinite control values are rejected");
}

void checkpoint() {
    Directory directory;
    ProcessAudioResources resources(directory.path);
    auto packet = session();
    packet.session.channels.clear(); // focus this pass on native state, without timeline automation
    AudioRuntime live;
    if (!check(bool(live.prepare(48000, 64)), "checkpoint source prepares")) return;
    live.buildSession(packet.session);
    if (!check(bool(live.commitGraph()), "checkpoint source publishes native dual mono")) return;
    live.setPluginParameter({"source", "eq", false}, "output.gain", -4);
    live.setPluginParameter({"source", "eq", true}, "output.gain", 5);
    if (!check(bool(live.capturePluginCheckpoints(packet.checkpoints)) && packet.checkpoints.size() == 1,
               "runtime captures a complete strict plugin checkpoint")) return;
    check(packet.checkpoints.front().left.hasState && packet.checkpoints.front().right &&
          packet.checkpoints.front().right->hasState && !packet.checkpoints.front().left.pending.empty(),
          "checkpoint contains independent opaque sides and pending host overrides");
    packet.session.pluginChains.front().slots.front().parameters = {{"output.gain", -20}};
    packet.session.pluginChains.front().slots.front().rightParameters = {{"output.gain", -20}};
    const auto decoded = decodeAudioSession(encodeAudioSession(packet, resources), directory.path);
    AudioRuntime restored;
    restored.prepare(48000, 64); restored.buildSession(decoded.session);
    auto invalid = decoded.checkpoints; invalid.push_back(invalid.front()); invalid.back().slotId = "missing";
    check(!restored.restorePluginCheckpoints(invalid) &&
          restored.pluginParameter({"source", "eq", false}, "output.gain") == -20,
          "invalid checkpoint batch cannot partially mutate earlier slots");
    if (!check(bool(restored.restorePluginCheckpoints(decoded.checkpoints)) && bool(restored.commitGraph()),
               "decoded native checkpoint restores completely before graph publication")) return;
    check(restored.pluginParameter({"source", "eq", false}, "output.gain") == -4 &&
          restored.pluginParameter({"source", "eq", true}, "output.gain") == 5,
          "opaque state plus pending edits wins over stale parameter mirrors for both sides");
    check(!restored.restorePluginCheckpoints(decoded.checkpoints) &&
          restored.pluginParameter({"source", "eq", true}, "output.gain") == 5,
          "checkpoint import cannot mutate processors in an audible published generation");
}
} // namespace

int main() {
    try { codec(); controls(); checkpoint(); }
    catch (const std::exception& error) { std::printf("FAIL unexpected exception: %s\n", error.what()); return 1; }
    return failures ? 1 : 0;
}
