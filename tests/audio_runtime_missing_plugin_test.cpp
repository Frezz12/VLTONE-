#include "AudioRuntime.hpp"
#include "platform/PathUtils.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <stdexcept>

namespace {
using namespace daw;
int failures = 0;
bool check(bool value, const char* message) {
    std::printf("%s %s\n", value ? "PASS" : "FAIL", message);
    std::fflush(stdout);
    failures += !value;
    return value;
}
struct Directory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("daw-missing-plugin-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ~Directory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};

AudioPluginSpec plugin(std::string id, std::string uid) {
    AudioPluginSpec value;
    value.id = value.name = std::move(id); value.uid = std::move(uid);
    value.requiredFormat = value.descriptor.format = plugins::Format::Internal;
    value.descriptor.uid = value.uid; value.descriptor.name = value.name;
    return value;
}
AudioSessionSpec session() {
    AudioSessionSpec value;
    AudioGraphSpec::Channel channel;
    channel.id = "source"; channel.name = "Source";
    channel.input = {true, true, 0, 2, 3};
    value.graph.channels.push_back(channel);
    AudioPluginChainSpec chain; chain.channelId = "source";
    chain.slots.push_back(plugin("healthy", "daw.equalizer"));
    value.pluginChains.push_back(std::move(chain));
    return value;
}
std::shared_ptr<engine::Node> node(const AudioRuntime& runtime, const std::string& name) {
    const auto graph = runtime.routingGraph();
    if (graph) for (const auto& entry : graph->nodes)
        if (entry.node->name() == name) return entry.owner;
    return {};
}
bool rendersTransparent(AudioRuntime& runtime) {
    audio::AudioBuffer input(2, 64), output(2, 64);
    std::fill_n(input.getChannel(0), 64, .25f);
    std::fill_n(input.getChannel(1), 64, -.5f);
    audio::AudioCallbackContext context;
    context.inputBuffer = &input; context.outputBuffer = &output;
    context.numFrames = 64; context.sampleRate = 48000;
    for (unsigned block = 0; block < 16; ++block) runtime.processDeviceBlock(context);
    if (context.renderStatus != audio::AudioCallbackContext::RenderStatus::Complete) return false;
    for (unsigned i = 0; i < 64; ++i)
        if (std::abs(output.getChannel(0)[i] - .25f) > .0001f ||
            std::abs(output.getChannel(1)[i] + .5f) > .0001f) return false;
    return true;
}

void policies() {
    AudioRuntime runtime;
    auto original = session();
    if (!check(bool(runtime.prepare(48000, 64)) && bool(runtime.applySession(original)),
               "healthy project publishes before a missing-plugin edit")) return;
    const auto healthy = runtime.pluginInstanceId({"source", "healthy"});
    const auto graph = runtime.routingGraph();
    auto imported = original;
    auto unavailable = plugin("missing", "daw.test.unavailable-processor");
    unavailable.parameters = {{"threshold", -18, true}};
    unavailable.rightParameters = {{"threshold", -9, false}};
    unavailable.channelMode = PluginChannelMode::DualMono; unavailable.preferredChannels = 1;
    imported.pluginChains.front().slots.push_back(unavailable);
    check(!runtime.applySession(imported) && runtime.routingGraph() == graph &&
          runtime.pluginInstanceId({"source", "healthy"}) == healthy && rendersTransparent(runtime),
          "new missing insert is rejected without publishing or disturbing healthy audio");

    auto& wanted = imported.pluginChains.front().slots.back();
    wanted.loadPolicy = AudioPluginLoadPolicy::PreserveUnavailable;
    if (!check(bool(runtime.applySession(imported)), "import explicitly accepts an unavailable document slot")) return;
    const auto missing = node(runtime, "missing");
    const auto missingRight = node(runtime, "missing Right");
    const auto status = runtime.pluginRuntimeStatus("source", "missing");
    check(missing && missingRight && !runtime.hasPlugin({"source", "missing"}) &&
          !runtime.hasPlugin({"source", "missing", true}) && !runtime.pluginInstanceId({"source", "missing"}) &&
          status.state == AudioPluginRuntimeState::Missing && !status.detail.empty(),
          "both dual-mono positions survive as diagnosed placeholders without fake native identities");
    check(runtime.pluginInstanceId({"source", "healthy"}) == healthy && rendersTransparent(runtime),
          "missing dual-mono processors preserve stereo PCM and healthy native identity");
    imported.graph.channels.front().name = "Renamed";
    check(bool(runtime.applySession(imported)) && node(runtime, "missing") == missing &&
          node(runtime, "missing Right") == missingRight && runtime.pluginInstanceId({"source", "healthy"}) == healthy,
          "unrelated edits reuse diagnosed placeholders instead of repeatedly loading unavailable processors");
    const auto retained = runtime.routingGraph();
    wanted.loadPolicy = AudioPluginLoadPolicy::Required;
    check(!runtime.applySession(imported) && runtime.routingGraph() == retained && node(runtime, "missing") == missing,
          "explicit activation must load successfully and otherwise keeps the existing placeholder graph");

    auto blocked = original;
    auto next = plugin("blocked", "daw.equalizer");
    next.loadPolicy = AudioPluginLoadPolicy::PlaceholderOnly;
    next.unavailableReason = "Saved state is still downloading.";
    blocked.pluginChains.front().slots.push_back(next);
    check(bool(runtime.applySession(blocked)) && !runtime.hasPlugin({"source", "blocked"}) &&
          runtime.pluginRuntimeStatus("source", "blocked").detail == next.unavailableReason,
          "blocked native state produces a reasoned placeholder even when the plugin is installed");
    blocked.pluginChains.front().slots.back().loadPolicy = AudioPluginLoadPolicy::Required;
    blocked.pluginChains.front().slots.back().unavailableReason.clear();
    check(bool(runtime.applySession(blocked)) && runtime.hasPlugin({"source", "blocked"}) &&
          runtime.pluginInstanceId({"source", "healthy"}) == healthy,
          "explicit activation replaces a resolved placeholder while retaining healthy neighbors");

    auto mismatch = original;
    auto incompatible = plugin("incompatible", "daw.equalizer");
    incompatible.requireExactVersion = true; incompatible.requiredVersion = "definitely-unavailable-version";
    mismatch.pluginChains.front().slots.push_back(incompatible);
    check(!runtime.applySession(mismatch), "strict insertion rejects an incompatible resolved native version");
    mismatch.pluginChains.front().slots.back().loadPolicy = AudioPluginLoadPolicy::PreserveUnavailable;
    check(bool(runtime.applySession(mismatch)) && !runtime.hasPlugin({"source", "incompatible"}) &&
          runtime.pluginRuntimeStatus("source", "incompatible").state == AudioPluginRuntimeState::Missing,
          "project compatibility failure preserves a placeholder without loading the wrong native version");
}

void opaqueRoundTrip() {
    auto imported = session();
    imported.pluginChains.front().slots.clear();
    auto unavailable = plugin("missing", "daw.test.unavailable-processor");
    unavailable.loadPolicy = AudioPluginLoadPolicy::PreserveUnavailable;
    unavailable.channelMode = PluginChannelMode::DualMono; unavailable.preferredChannels = 1;
    unavailable.parameters = {{"left-fallback", -18, true}};
    unavailable.rightParameters = {{"right-fallback", -9, false}};
    imported.pluginChains.front().slots.push_back(unavailable);
    AudioRuntime runtime;
    if (!check(bool(runtime.prepare(48000, 64)), "missing-state runtime prepares")) return;
    runtime.buildSession(imported);
    std::vector<AudioPluginCheckpoint> original;
    if (!check(bool(runtime.capturePluginCheckpoints(original)) && original.size() == 1 && original[0].right,
               "unpublished placeholders expose preserved inline fallback for each side")) return;
    check(original[0].left.parameters.size() == 1 && original[0].right->parameters.size() == 1 &&
          original[0].left.parameters[0].id == "left-fallback" &&
          original[0].right->parameters[0].id == "right-fallback",
          "dual-mono fallback parameters remain independent while the plugin is unavailable");
    original[0].left.hasState = original[0].right->hasState = true;
    original[0].left.state = {0, 1, 2, 255, 0}; original[0].right->state = {99, 0, 42};
    original[0].left.pending = {{"last-edit", .75, true}};
    if (!check(bool(runtime.restorePluginCheckpoints(original)), "opaque unavailable state is retained without native loading")) return;
    std::vector<AudioPluginCheckpoint> captured;
    check(bool(runtime.capturePluginCheckpoints(captured)) && captured == original,
          "opaque bytes, independent sides, inline flags and pending edits survive capture unchanged");
    auto invalid = original;
    invalid.front().left.state = {123};
    invalid.push_back(original.front()); invalid.back().slotId = "absent";
    check(!runtime.restorePluginCheckpoints(invalid) && bool(runtime.capturePluginCheckpoints(captured)) &&
          captured == original,
          "an invalid checkpoint batch cannot partially replace retained unavailable state");

    AudioRuntime restored;
    if (!check(bool(restored.prepare(48000, 64)), "replacement missing-state runtime prepares")) return;
    restored.buildSession(std::move(imported));
    check(bool(restored.restorePluginCheckpoints(original)) && bool(restored.commitGraph()) &&
          bool(restored.capturePluginCheckpoints(captured)) && captured == original &&
          !restored.hasPlugin({"source", "missing"}) && rendersTransparent(restored),
          "replacement generation retains unavailable state and plays through the original stereo topology");
    check(!restored.restorePluginCheckpoints(original), "published placeholder targets still reject out-of-transaction checkpoint replacement");
}

void projectStateOrigin() {
    Directory directory;
    struct PreparedSession {
        AudioSessionSpec session;
        std::vector<AudioPluginStateEdit> restores;
        std::vector<AudioPluginCheckpoint> checkpoints;
    } packet;
    packet.session = session();
    auto sampler = plugin("sampler", "daw.sampler");
    sampler.loadPolicy = AudioPluginLoadPolicy::PlaceholderOnly;
    sampler.unavailableReason = "Project state is not yet available.";
    packet.session.pluginChains.front().slots = {sampler};
    AudioPluginStateEdit edit; edit.address = {"source", "sampler"};
    const std::string json = R"({"version":1,"sample":"portable.wav","params":{"vol":0.25,"pan":0}})";
    edit.state.state.assign(json.begin(), json.end());
    edit.state.stateFile = "sampler.bin";
    edit.state.contentDirectory = platform::pathToUtf8(directory.path / "Content");
    edit.state.sourcePath = platform::pathToUtf8(directory.path / "Content" / "portable.wav");
    auto source = std::make_shared<engine::SampleBuffer>(1, 64, 48000);
    std::fill_n(source->writableChannel(0), source->frames(), .375f);
    edit.state.source = source;
    edit.parameters = {{"vol", .9, false}, {"pan", .25, true}};
    packet.restores.push_back(edit);
    AudioRuntime unavailable;
    if (!check(bool(unavailable.prepare(48000, 64)) &&
               bool(unavailable.applySession(packet.session, false, packet.restores)) &&
               bool(unavailable.capturePluginCheckpoints(packet.checkpoints)) && packet.checkpoints.size() == 1,
               "blocked Sampler retains its original project import and immutable PCM")) return;
    check(packet.checkpoints.front().left.projectState && packet.checkpoints.front().left.state == edit.state.state &&
          packet.checkpoints.front().left.parameters == edit.parameters,
          "placeholder checkpoint marks project bytes instead of claiming a captured native state");
    packet.checkpoints.front().left.pending = {{"pan", -.5}};
    packet.session.pluginChains.front().slots.front().loadPolicy = AudioPluginLoadPolicy::Required;
    auto decoded = packet;
    check(decoded.checkpoints.front().left.projectState && decoded.restores.size() == 1 &&
          decoded.restores.front().state.source && decoded.restores.front().state.source->readSample(0, 31) == .375f,
          "project-state origin and immutable original import survive a runtime copy");

    AudioRuntime missingImport;
    missingImport.prepare(48000, 64);
    check(!missingImport.applySession(decoded.session, false, {}, decoded.checkpoints) &&
          !missingImport.hasPlugin({"source", "sampler"}),
          "project-origin checkpoint cannot load without its original state import");
    AudioRuntime restored;
    if (!check(bool(restored.prepare(48000, 64)) &&
               bool(restored.applySession(decoded.session, false, decoded.restores, decoded.checkpoints)),
               "resolved Sampler applies its packaged project state once before checkpoint overrides")) return;
    const auto current = restored.pluginStateSnapshot({"source", "sampler"}, false);
    check(current.exists && current.sample && current.sample->readSample(0, 31) == .375f &&
          current.samplePath == edit.state.sourcePath && restored.pluginParameter({"source", "sampler"}, "vol") == .25 &&
          restored.pluginParameter({"source", "sampler"}, "pan") == -.5,
          "checkpoint replay preserves shared PCM, preset authority and the newer pending host edit");
    std::vector<AudioPluginCheckpoint> native;
    check(bool(restored.capturePluginCheckpoints(native)) && native.size() == 1 && !native.front().left.projectState,
          "a healthy native capture replaces the project-origin marker with an actual processor checkpoint");
}
} // namespace

int main() {
    try { policies(); opaqueRoundTrip(); projectStateOrigin(); }
    catch (const std::exception& error) { check(false, error.what()); }
    return failures ? 1 : 0;
}
