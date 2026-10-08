#include "ProcessPluginInstance.hpp"
#include "Host/PluginNode.hpp"
#include "Graph/GraphProcessor.hpp"
#include "Nodes/BasicNodes.hpp"
#include "plugins/PluginManager.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

using namespace daw;
using namespace daw::plugins;
using namespace daw::engine;
namespace {
constexpr FrameCount frames = 128;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
struct Source final : Node {
    bool notes = false;
    float level = 0.2f;
    std::string_view name() const noexcept override { return "isolation test source"; }
    void process(const ProcessContext& c) override {
        for (ChannelCount ch = 0; ch < c.output.numChannels(); ++ch)
            for (FrameCount i = 0; i < c.frames; ++i)
                c.output.data(ch)[i] = level + float((c.timelinePosition + i + ch * 7) % 31) / 1000.f;
        if (notes && c.midiOutput) {
            (void)c.midiOutput->push(MidiEvent::noteOn(11, 0, 60, 95));
            (void)c.midiOutput->push(MidiEvent::noteOff(93, 0, 60));
        }
    }
};
struct Rig {
    std::shared_ptr<PluginNode> plugin;
    AudioGraph graph;
    GraphProcessor renderer{4};
    std::array<float, frames> left{}, right{};
    float* channels[2]{left.data(), right.data()};
    std::shared_ptr<const CompiledGraph> snapshot;
    bool offline;
    Rig(PluginManager& manager, const PluginDescriptor& descriptor, bool offlineMode,
        bool mono = false, bool sidechain = false, bool midi = false) : offline(offlineMode) {
        auto instance = manager.instantiate(descriptor);
        require(bool(instance), "instantiate graph plugin");
        plugin = std::make_shared<PluginNode>(descriptor.name, std::move(instance));
        if (mono) plugin->setPreferredChannelCount(1);
        auto input = std::make_unique<Source>(); input->notes = descriptor.isInstrument || midi;
        const auto source = graph.addNode(std::move(input));
        const auto effect = graph.adoptNode(plugin);
        require(bool(graph.connect(source, effect)), "connect plugin input");
        if (sidechain) {
            auto aux = std::make_unique<Source>(); aux->level = 0.1f;
            require(bool(graph.connect(graph.addNode(std::move(aux)), effect, InputRole::Sidechain)), "connect sidechain");
        }
        const auto sum = graph.addNode(std::make_unique<SumNode>());
        require(bool(graph.connect(effect, sum)) && bool(graph.connect(source, sum)), "parallel dry path for PDC");
        graph.setSink(sum);
        compile();
        renderer.setParallelThreshold(0);
    }
    void compile() {
        const auto compiled = graph.compile({48000, frames, 2, offline}, snapshot.get());
        require(bool(compiled), "compile isolated graph");
        snapshot = *compiled; renderer.setGraph(snapshot);
        require(plugin->isReady(), "activate graph plugin");
    }
    Status render(unsigned block, bool serial = false) {
        TransportInfo time; time.tempo = 137; time.timeSigNumerator = 7; time.timeSigDenominator = 8;
        time.ppqPosition = double(block * frames) * 137 / (48000 * 60.0);
        const AudioBlock output(channels, 2, frames);
        return serial ? renderer.processSerial(output, frames, block * frames, true, offline, time)
                      : renderer.process(output, frames, block * frames, true, offline, time);
    }
    ProcessPluginInstance& remote() { return dynamic_cast<ProcessPluginInstance&>(*plugin->instance()); }
};

struct Bank {
    std::vector<std::shared_ptr<PluginNode>> plugins;
    AudioGraph graph;
    GraphProcessor renderer{4};
    std::array<float, frames> left{}, right{};
    float* channels[2]{left.data(), right.data()};
    Bank(PluginManager& manager, const PluginDescriptor& descriptor, unsigned count, bool chain) {
        const auto source = graph.addNode(std::make_unique<Source>());
        const auto sum = chain ? kInvalidNode : graph.addNode(std::make_unique<SumNode>());
        auto previous = source;
        for (unsigned i = 0; i < count; ++i) {
            auto instance = manager.instantiate(descriptor);
            require(bool(instance), "instantiate plugin bank");
            auto node = std::make_shared<PluginNode>(descriptor.name, std::move(instance));
            const auto effect = graph.adoptNode(node);
            require(bool(graph.connect(chain ? previous : source, effect)), "connect plugin bank");
            if (!chain) require(bool(graph.connect(effect, sum)), "connect bank sum");
            previous = effect; plugins.push_back(std::move(node));
        }
        graph.setSink(chain ? previous : sum);
        const auto compiled = graph.compile({48000, frames, 2});
        require(bool(compiled), "compile plugin bank");
        renderer.setGraph(*compiled);
        for (const auto& node : plugins) require(node->isReady(), "activate plugin bank");
    }
    Status render(unsigned block) {
        return renderer.process(AudioBlock(channels, 2, frames), frames, block * frames, true);
    }
};

void compareBanks(PluginManager& local, PluginManager& isolated, const PluginDescriptor& descriptor) {
    for (bool chain : {false, true}) {
        Bank reference(local, descriptor, 10, chain), remote(isolated, descriptor, 10, chain);
        for (unsigned block = 0; block < 16; ++block) {
            require(bool(reference.render(block)) && bool(remote.render(block)), "render ten-plugin graph");
            require(reference.left == remote.left && reference.right == remote.right,
                    "parallel bank and serial chain remain sample-identical");
        }
    }
    std::puts("PASS ten-plugin banks: dependent chains and independent process jobs");
}

int benchmark(PluginManager& local, PluginManager& isolated, const PluginDescriptor& descriptor) {
    bool ok = true;
    for (unsigned count : {1u, 10u}) for (bool chain : {true, false})
        for (auto* manager : {&local, &isolated}) {
            Bank rig(*manager, descriptor, count, chain);
            std::vector<double> samples; samples.reserve(2000);
            unsigned failedAt = 0;
            for (unsigned block = 0; block < 2064; ++block) {
                const auto started = std::chrono::steady_clock::now();
                const auto status = rig.render(block);
                const auto elapsed = std::chrono::duration<double, std::micro>(
                    std::chrono::steady_clock::now() - started).count();
                if (!status) { failedAt = block + 1; ok = false; break; }
                if (block >= 64) samples.push_back(elapsed);
            }
            std::sort(samples.begin(), samples.end());
            const auto percentile = [&](double p) { return samples.empty() ? 0.0
                : samples[std::min(samples.size() - 1, std::size_t(p * double(samples.size() - 1)))]; };
            std::printf("BENCH %s %u %s: n=%zu median=%.2f p99=%.2f p99.9=%.2f max=%.2f us failed_block=%u\n",
                manager == &local ? "local" : "isolated", count, chain ? "chain" : "parallel",
                samples.size(), percentile(.5), percentile(.99), percentile(.999), percentile(1), failedAt);
        }
    return ok ? 0 : 1;
}

void compare(PluginManager& localManager, PluginManager& isolatedManager, const PluginDescriptor& descriptor) {
    for (bool offline : {false, true}) for (bool mono : {false, true}) {
        Rig local(localManager, descriptor, offline, mono, true);
        Rig isolated(isolatedManager, descriptor, offline, mono, true);
        require(isolated.snapshot->deferredNodes.size() == 1 && local.snapshot->deferredNodes.empty(), "compiled graph selects asynchronous instance");
        require(isolated.snapshot->totalLatency == local.snapshot->totalLatency, "IPC introduces no extra sample latency");
        if (!local.plugin->instance()->parameters().empty()) {
            const auto p = local.plugin->instance()->parameters().front();
            PluginEvent edit; edit.paramIndex = p.index; edit.frameOffset = 37;
            edit.value = p.minValue + (p.maxValue - p.minValue) * 0.25;
            require(local.plugin->pushEvent(edit) && isolated.plugin->pushEvent(edit), "queue timestamped automation");
        }
        for (unsigned block = 0; block < 12; ++block) {
            if (block == 4) { local.plugin->setMix(0.4f); isolated.plugin->setMix(0.4f); }
            if (block == 6) { local.plugin->setBypassed(true); isolated.plugin->setBypassed(true); }
            if (block == 9) { local.plugin->setBypassed(false); isolated.plugin->setBypassed(false); }
            require(bool(local.render(block, block % 2 == 0)), "reference graph DSP");
            if (!isolated.render(block, block % 2 == 0)) {
                std::fprintf(stderr, "%s: %s\n", descriptor.name.c_str(), isolated.remote().error().c_str());
                throw std::runtime_error("isolated graph DSP");
            }
            for (FrameCount i = 0; i < frames; ++i)
                require(std::abs(local.left[i] - isolated.left[i]) < 1e-6f &&
                        std::abs(local.right[i] - isolated.right[i]) < 1e-6f,
                        "local/isolated samples match including PDC, MIDI, mono, sidechain and bypass");
        }
        std::vector<std::uint8_t> state;
        if (isolated.plugin->instance()->supportsState()) {
            require(isolated.plugin->instance()->saveState(state), "isolated state snapshot");
            require(isolated.plugin->instance()->loadState(state), "isolated state restore");
        }
    }
    std::printf("PASS %s: live/offline, serial/parallel, mono/stereo, automation/MIDI, PDC, sidechain, bypass, state\n",
                descriptor.name.c_str());
}

#if DAW_ENABLE_CLAP
PluginDescriptor faultDescriptor() {
    PluginDescriptor descriptor;
    descriptor.format = Format::Clap; descriptor.uid = "com.daw.test.fault";
    descriptor.path = DAW_FAULT_CLAP_PATH; descriptor.name = "Fault fixture";
    return descriptor;
}
void faults(PluginManager& manager) {
    for (double mode : {1., 2., 3., 4., 5.}) {
        Rig rig(manager, faultDescriptor(), false, false, false, true);
        require(bool(rig.render(0)), "initial healthy block");
        std::vector<std::uint8_t> checkpoint;
        require(rig.plugin->instance()->saveState(checkpoint), "checkpoint before graph fault");
        PluginEvent fault; fault.paramIndex = 0; fault.value = mode;
        require(rig.plugin->pushEvent(fault), "queue DSP fault");
        const auto start = std::chrono::steady_clock::now();
        require(!rig.render(1), "crash/hang/NaN/error/late plugin result fails the block");
        require(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(200), "graph does not wait for hung foreign DSP");
        const auto entry = std::find_if(rig.snapshot->nodes.begin(), rig.snapshot->nodes.end(),
            [&](const auto& node) { return node.node == rig.plugin.get(); });
        require(entry != rig.snapshot->nodes.end() && entry->midiOutputBuffer != kInvalidNode, "faulted audio effect retains MIDI route");
        const auto notes = rig.snapshot->midiBuffers[entry->midiOutputBuffer].events();
        require(notes.size() == 2 && notes[0].isNoteOn() && notes[1].isNoteOff() &&
                notes[0].frameOffset == 11 && notes[1].frameOffset == 93,
                "failure preserves trusted MIDI including downstream note-off");
        for (FrameCount i = 0; i < frames; ++i)
            require(std::abs(rig.left[i] - (0.2f + float((frames + i) % 31) / 1000.f)) < 1e-6f,
                    "independent dry branch survives a faulted plugin");
        rig.plugin->setBypassed(true);
        require(bool(rig.render(2)) && bool(rig.render(3)), "explicit bypass remains usable after process failure");
        require(!rig.remote().service(), "reap failed process off the renderer");
        require(rig.remote().restart(), "restart isolated graph instance from checkpoint");
        rig.plugin->setBypassed(false); rig.plugin->invalidatePrepare(); rig.compile();
        require(bool(rig.render(4)), "recompiled slot renders after restart");
    }
    {
        Bank chain(manager, faultDescriptor(), 2, true);
        require(bool(chain.render(0)), "healthy downstream process before upstream hang");
        auto& downstream = dynamic_cast<ProcessPluginInstance&>(*chain.plugins[1]->instance());
        const auto pid = downstream.processId();
        PluginEvent hang; hang.paramIndex = 0; hang.value = 2;
        PluginEvent gain; gain.paramIndex = 1; gain.value = 0.25;
        require(chain.plugins[0]->pushEvent(hang) && chain.plugins[1]->pushEvent(gain), "queue upstream hang and downstream edit");
        require(!chain.render(1), "upstream hang exhausts common deadline");
        require(downstream.failure() == PluginProcessFailure::None && downstream.processId() == pid,
                "missed upstream deadline does not fault an unsubmitted healthy child");
        chain.plugins[0]->setBypassed(true);
        require(bool(chain.render(2)), "healthy downstream resumes after bypassing failed predecessor");
        require(std::abs(chain.left.back() - (0.2f + float((2 * frames + frames - 1) % 31) / 1000.f) * 0.25f) < 1e-6f,
                "downstream host edits survive an upstream deadline skip");
    }
    std::puts("PASS graph faults: bounded failure, independent branch, bypass and restart");
}
#endif
}

int main(int argc, char** argv) try {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    PluginManager local, isolated;
    isolated.setHostingMode(PluginManager::HostingMode::Isolated, DAW_PLUGIN_HOST_PATH);
    PluginManager clone; clone.copyCatalogFrom(isolated);
    require(clone.hostingMode() == PluginManager::HostingMode::Isolated &&
            clone.pluginHostPath() == isolated.pluginHostPath(), "render clone retains isolation configuration");
#if DAW_ENABLE_CLAP
    const auto clap = factoryFor(Format::Clap)->inspect(DAW_TEST_CLAP_PATH);
    require(!clap.empty(), "CLAP fixture catalogue");
    const auto gain = std::find_if(clap.begin(), clap.end(), [](const auto& d) { return d.uid == "com.daw.test.gain"; });
    require(gain != clap.end(), "CLAP gain fixture");
    if (argc == 2 && std::strcmp(argv[1], "--benchmark") == 0) return benchmark(local, isolated, *gain);
    for (const auto& descriptor : clap) compare(local, isolated, descriptor);
    compareBanks(local, isolated, *gain);
    faults(isolated);
#endif
#if DAW_ENABLE_VST3
    const auto vst3 = factoryFor(Format::Vst3)->inspect(DAW_TEST_VST3_PATH);
    require(!vst3.empty(), "VST3 fixture catalogue");
    for (const auto& descriptor : vst3) compare(local, isolated, descriptor);
#endif
#if DAW_ENABLE_VST
    for (const auto* path : {DAW_TEST_VST_SHELL_PATH, DAW_TEST_VST1_PATH}) {
        const auto vst = factoryFor(Format::Vst)->inspect(path);
        require(!vst.empty(), "VST1/2 fixture catalogue");
        for (const auto& descriptor : vst) compare(local, isolated, descriptor);
    }
#endif
#if DAW_ENABLE_AU
    const auto units = factoryFor(Format::AudioUnit)->inspect("");
    const auto eq = std::find_if(units.begin(), units.end(), [](const auto& d) { return d.name == "AUNBandEQ"; });
    require(eq != units.end(), "system Audio Unit fixture");
    compare(local, isolated, *eq);
#endif
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL %s\n", error.what()); return 1;
}
