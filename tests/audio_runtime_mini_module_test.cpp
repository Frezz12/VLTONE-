#include "AudioRuntime.hpp"
#include "AudioMiniModuleCompiler.hpp"
#include "Internal/MiniNodeRegistry.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <thread>

namespace {
using namespace daw;
using namespace std::chrono_literals;
constexpr unsigned frames = 64;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
plugins::mini::MiniModuleDefinition definition(bool revised) {
    using namespace plugins::mini;
    MiniModuleDefinition d;
    d.version = 3; d.id = "runtime.compiler"; d.name = "Runtime gain";
    d.controls = {{"level", "Level", "", 0, 2, 1}};
    d.nodes = {makeNode("input", "in"), makeNode("output", "out"),
               makeNode("interface", "ui"), makeNode("gain", "gain")};
    d.connections = {{"in", "gain", "out", "in"}, {"gain", "out", "out", "in"},
                     {"ui", "gain", "level", "gain"}};
    if (revised) {
        d.nodes.push_back(makeNode("multiply", "half"));
        for (auto& p : d.nodes.back().parameters) if (p.id == "b") p.value = .5;
        d.connections.back() = {"ui", "half", "level", "a"};
        d.connections.push_back({"half", "gain", "out", "gain"});
    }
    return d;
}
AudioMiniModuleCompileStatus wait(AudioMiniModuleCompiler& compiler, std::uint64_t id) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    do {
        auto status = compiler.poll(id);
        if (status.state != AudioMiniModuleCompileStatus::State::Pending) return status;
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("numeric compiler job timed out");
}
void registry() {
    AudioRuntime runtime;
    require(bool(runtime.prepare(48000, frames)), "prepare standalone runtime");
    AudioSessionSpec session;
    AudioGraphSpec::Channel channel;
    channel.id = "track"; channel.name = "Track"; channel.input = {true, true, 0, 2, 3};
    channel.miniModules.push_back({"module", false});
    session.graph.channels.push_back(channel);
    AudioPluginSpec slot;
    slot.id = "module"; slot.uid = std::string(plugins::mini::kUid); slot.name = "Runtime gain";
    slot.descriptor.uid = slot.descriptor.path = slot.uid;
    slot.descriptor.name = slot.name; slot.descriptor.format = slot.requiredFormat = plugins::Format::Internal;
    slot.miniModule = definition(false); slot.parameters = {{"level", .8}};
    AudioPluginChainSpec chain;
    chain.channelId = "track"; chain.kind = AudioPluginChainSpec::Kind::MiniModules;
    chain.slots.push_back(slot); session.pluginChains.push_back(chain);
    runtime.buildSession(session);
    require(bool(runtime.commitGraph()), "publish original module");
    AudioPluginAddress address{"track", "module", false, runtime.pluginInstanceId({"track", "module"})};
    require(address.instance != 0, "original module has an instance generation");
    auto compiler = runtime.miniModuleCompiler().lock();
    require(bool(compiler), "runtime exposes value compiler endpoint");
    AudioMiniModuleCompileRequest request;
    request.info = runtime.preparation();
    slot.miniModule = definition(true); request.targets.push_back({address, slot});
    const auto id = compiler->start(request);
    require(id != 0, "value request creates numeric job");
    const auto ready = wait(*compiler, id);
    require(ready.state == AudioMiniModuleCompileStatus::State::Ready && ready.targets.size() == 1 &&
            ready.targets.front().address.instance == address.instance, "ready result retains expected native generation");
    require(runtime.pluginInstanceId(address) == address.instance, "compilation never replaces the published module");
    require(runtime.stageMiniModulePreparation(id), "ready generation stages atomically");
    require(!runtime.stageMiniModulePreparation(id), "a job cannot publish its processors twice");
    const auto taken = compiler->poll(id);
    require(taken.state == AudioMiniModuleCompileStatus::State::Ready && taken.targets.size() == 1 &&
            taken.targets.front().latency == ready.targets.front().latency,
            "consuming processors retains fade-cancellation metadata until forget");
    session.pluginChains.front().slots.front() = slot;
    runtime.buildSession(session);
    require(bool(runtime.commitGraph()), "publish compiled generation through ordinary reconciliation");
    runtime.clearMiniModulePreparation();
    const auto replacement = runtime.pluginInstanceId({"track", "module"});
    require(replacement && replacement != address.instance, "runtime owns a fresh compiled processor");
    compiler->forget(id);
    require(compiler->poll(id).state == AudioMiniModuleCompileStatus::State::Missing, "forget retires job metadata");

    audio::AudioBuffer input(2, frames), output(2, frames);
    std::fill_n(input.getChannel(0), frames, .2f); std::fill_n(input.getChannel(1), frames, .2f);
    audio::AudioCallbackContext block;
    block.inputBuffer = &input; block.outputBuffer = &output; block.numFrames = frames; block.sampleRate = 48000;
    for (unsigned i = 0; i < 128; ++i) {
        runtime.processDeviceBlock(block);
        require(block.renderStatus == audio::AudioCallbackContext::RenderStatus::Complete, "compiled module renders");
    }
    require(std::abs(output.getChannel(0)[frames - 1] - .08f) < 1e-5f &&
            std::abs(output.getChannel(1)[frames - 1] - .08f) < 1e-5f,
            "prepared DSP and its edited parameter survive numeric handoff");

    // A job prepared for the preceding native generation must not replace the
    // current one, even when its project slot has exactly the same name/ID.
    const auto stale = compiler->start(request);
    require(wait(*compiler, stale).state == AudioMiniModuleCompileStatus::State::Ready, "stale job completes normally");
    require(!runtime.stageMiniModulePreparation(stale) && runtime.pluginInstanceId({"track", "module"}) == replacement,
            "stale numeric preparation cannot overwrite a newer generation");
    compiler->forget(stale);
    request.targets.front().address.instance = replacement;
    const auto cancelled = compiler->start(request);
    compiler->forget(cancelled);
    require(compiler->poll(cancelled).state == AudioMiniModuleCompileStatus::State::Missing &&
            !runtime.stageMiniModulePreparation(cancelled), "forgotten jobs never stage, including pending compilation");
}
}
int main() {
    try { registry(); std::puts("PASS runtime numeric mini compilation / state / generation / cancellation"); return 0; }
    catch (const std::exception& error) { std::fprintf(stderr, "FAIL %s\n", error.what()); return 1; }
}
