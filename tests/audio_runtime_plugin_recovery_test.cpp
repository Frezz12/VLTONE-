#include "AudioRuntime.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <thread>

namespace {
using namespace daw;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
constexpr unsigned frames = 1024;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

struct Rig {
    AudioRuntime runtime;
    AudioSessionSpec session;
    audio::AudioBuffer input{2, frames}, output{2, frames};
    audio::AudioCallbackContext block;
    AudioPluginAddress left{"bus", "effect"}, right{"bus", "effect", true};

    Rig() {
        require(bool(runtime.prepare(48000, frames)), "prepare standalone runtime");
        session.hosting = {plugins::HostingMode::Isolated, DAW_PLUGIN_HOST_PATH};
        AudioGraphSpec::Channel source, bus;
        source.id = "source"; source.name = "Source"; source.outputBusId = "bus";
        source.input = {true, true, 0, 2, 3}; source.volume = .8f;
        bus.id = "bus"; bus.name = "Bus"; bus.volume = .5f;
        session.graph.channels = {source, bus}; session.graph.masterVolume = .9f;
        AudioPluginSpec slot;
        slot.id = left.slotId; slot.uid = "com.daw.test.fault.recover_slow"; slot.name = "Isolated gain";
        slot.descriptor.uid = slot.uid; slot.descriptor.name = slot.name;
        slot.descriptor.path = DAW_FAULT_CLAP_PATH; slot.descriptor.format = plugins::Format::Clap;
        slot.requiredFormat = plugins::Format::Clap;
        slot.channelMode = PluginChannelMode::DualMono; slot.preferredChannels = 1;
        slot.parameters = {{"0", 0}, {"1", .7}};
        slot.rightParameters = {{"0", 0}, {"1", .3}};
        slot.mix = .25f;
        AudioPluginChainSpec chain;
        chain.channelId = left.channelId; chain.slots.push_back(std::move(slot));
        session.pluginChains.push_back(std::move(chain));
        runtime.buildSession(session);
        require(bool(runtime.commitGraph()), "publish standalone dual mono graph");
        std::fill_n(input.getChannel(0), frames, .2f);
        std::fill_n(input.getChannel(1), frames, .2f);
        block.inputBuffer = &input; block.outputBuffer = &output;
        block.sampleRate = 48000; block.numFrames = frames;
        settle();
        std::vector<AudioPluginCheckpoint> checkpoints;
        require(bool(runtime.capturePluginCheckpoints(checkpoints)), "checkpoint both sides before crash");
    }
    bool render() {
        runtime.processDeviceBlock(block);
        return block.renderStatus == audio::AudioCallbackContext::RenderStatus::Complete;
    }
    void settle() { for (int i = 0; i < 8; ++i) require(render(), "healthy standalone block"); }
    void expect(float l, float r, const char* message) {
        require(std::abs(output.getChannel(0)[frames - 1] - l) < .00002f &&
                std::abs(output.getChannel(1)[frames - 1] - r) < .00002f, message);
    }
    void crash() {
        require(runtime.setPluginParameter(left, "0", 1), "queue crash on left side only");
        require(!render(), "child crash is reported by audio block");
        runtime.pumpIsolatedPlugins();
        require(runtime.pluginRuntimeStatus(left.channelId, left.slotId).state == AudioPluginRuntimeState::Failed,
                "runtime reports failed slot without controller pump");
    }
    void start() {
        const auto began = Clock::now();
        require(runtime.restartPlugin(left.channelId, left.slotId), "begin asynchronous recovery");
        require(Clock::now() - began < 100ms, "recovery does not wait for slow child activation");
        require(!runtime.restartPlugin(left.channelId, left.slotId), "duplicate recovery is coalesced");
    }
    void finish() {
        const auto deadline = Clock::now() + 10s;
        do {
            runtime.pumpIsolatedPlugins();
            if (runtime.pluginRuntimeStatus(left.channelId, left.slotId).state != AudioPluginRuntimeState::Restarting)
                return;
            (void)render();
            std::this_thread::sleep_for(2ms);
        } while (Clock::now() < deadline);
        require(false, "recovery completes within its bounded process timeout");
    }
};

void recoveryAfterTopologyAndControlEdits() {
    Rig r;
    const auto oldLeft = r.runtime.pluginInstanceId(r.left);
    const auto oldRight = r.runtime.pluginInstanceId(r.right);
    require(oldLeft && oldRight && oldLeft != oldRight, "dual mono has separate identities");
    r.expect(.0666f, .0594f, "initial dual mono state and wet mix");
    r.crash();
    r.start();
    const auto oldNode = r.runtime.trackNodes("bus")->inserts.front();
    // Graph handles change while a replacement is being prepared. Recovery
    // must replace by current ownership, never by the captured old handles.
    std::reverse(r.session.graph.channels.begin(), r.session.graph.channels.end());
    r.session.pluginChains.front().slots.front().mix = .5f;
    r.runtime.buildSession(r.session);
    require(bool(r.runtime.commitGraph()), "publish reordered graph during recovery");
    const auto currentNode = r.runtime.trackNodes("bus")->inserts.front();
    require(currentNode != oldNode, "intervening assembly changes topology handles");
    require(r.runtime.setFader("source", AudioFaderTarget::Channel, {.gain = .6f}) &&
            r.runtime.setFader("bus", AudioFaderTarget::Channel, {.gain = .4f}) &&
            r.runtime.setFader("master", AudioFaderTarget::Channel, {.gain = .8f}),
            "apply immediate controls newer than the prepared session");
    r.finish();
    require(r.runtime.pluginRuntimeStatus("bus", "effect").state == AudioPluginRuntimeState::Running,
            "runtime publishes a recovered processor");
    const auto freshLeft = r.runtime.pluginInstanceId(r.left);
    require(freshLeft && freshLeft != oldLeft && r.runtime.pluginInstanceId(r.right) == oldRight,
            "only the failed side receives a fresh identity");
    require(r.runtime.trackNodes("bus")->inserts.front() == currentNode,
            "recovery retains current topology handles");
    require(!r.runtime.setPluginParameter({"bus", "effect", false, oldLeft}, "1", .1),
            "a delayed command cannot target the preceding native generation");
    r.settle();
    r.expect(.03264f, .02496f, "recovery retains live faders, current mix, routing and healthy right state");

    // The new processor must receive automation installed after recovery began,
    // rather than replaying its older opaque checkpoint over that content.
    r.crash(); r.start();
    AudioContentSpec content;
    content.plugins = AudioContentSpec::PluginCurves{{"effect", "1", .4, {{0, .4}}}};
    require(r.runtime.applyContent("bus", std::move(content)), "publish automation during recovery");
    r.finish(); r.settle();
    r.expect(.02688f, .02688f, "recovery rebinds the newest stable-ID automation on both sides");
}

void supersededGenerationIsNotResurrected() {
    Rig r;
    r.crash(); r.start();
    const auto failedGeneration = r.runtime.pluginInstanceId(r.left);
    auto& chain = r.session.pluginChains.front();
    auto slots = chain.slots;
    chain.slots.clear();
    r.runtime.buildSession(r.session);
    require(bool(r.runtime.commitGraph()), "publish removal while recovery is running");
    require(!r.runtime.hasPlugin(r.left), "removed slot is absent immediately");
    chain.slots = std::move(slots);
    r.runtime.buildSession(r.session);
    require(bool(r.runtime.commitGraph()), "publish new instance under the same durable slot ID");
    const auto replacement = r.runtime.pluginInstanceId(r.left);
    const auto right = r.runtime.pluginInstanceId(r.right);
    require(replacement && replacement != failedGeneration, "replacement has a new generation");
    // A pending future can finish even after its slot has ceased to report
    // Restarting. Drain it long enough to include slow fixture activation.
    const auto deadline = Clock::now() + 1s;
    do { r.runtime.pumpIsolatedPlugins(); std::this_thread::sleep_for(2ms); }
    while (Clock::now() < deadline);
    require(r.runtime.pluginInstanceId(r.left) == replacement && r.runtime.pluginInstanceId(r.right) == right,
            "late recovery never overwrites a replacement under the same slot ID");
    r.settle();
    r.expect(.0666f, .0594f, "replacement retains its independent sound and topology");
}
}

int main() {
    try {
        recoveryAfterTopologyAndControlEdits();
        supersededGenerationIsNotResurrected();
        std::puts("PASS standalone runtime recovery / topology edits / identity / automation / stale generation");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s\n", error.what()); return 1;
    }
}
