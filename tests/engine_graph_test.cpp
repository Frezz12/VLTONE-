// The rebuilt engine's correctness baseline.
//
// Covers: graph compilation (topological order, cycle rejection), buffer reuse,
// automatic latency compensation, the dependency-driven parallel scheduler
// (parallel output must be bit-identical to serial), and scaling with a
// thousand tracks.
#include "Graph/AudioGraph.hpp"
#include "Graph/GraphProcessor.hpp"
#include "Engine/RealtimeEngine.hpp"
#include "Nodes/BasicNodes.hpp"
#include "Nodes/MetronomeNode.hpp"
#include "Nodes/PlaybackNodes.hpp"
#include "Nodes/PreviewPlayerNode.hpp"
#include "Transport/Transport.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

using namespace daw::engine;

static int failures = 0;
static void check(bool condition, const char* what) {
    std::printf("%s  %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++failures;
}

namespace {

/// A source that writes a constant value, so sums are trivial to predict.
struct ConstantSource {
    float value = 1.0f;

    static void render(void* context, const AudioBlock& output, FrameCount frames,
                       SamplePos) {
        auto* self = static_cast<ConstantSource*>(context);
        for (ChannelCount ch = 0; ch < output.numChannels(); ++ch) {
            float* data = output.data(ch);
            for (FrameCount i = 0; i < frames; ++i) data[i] = self->value;
        }
    }
};

/// A source whose sample value encodes its position, so a delay is visible.
struct RampSource {
    static void render(void*, const AudioBlock& output, FrameCount frames,
                       SamplePos position) {
        for (ChannelCount ch = 0; ch < output.numChannels(); ++ch) {
            float* data = output.data(ch);
            for (FrameCount i = 0; i < frames; ++i) {
                data[i] = float(position + SamplePos(i));
            }
        }
    }
};

/// A transport-aware tone used to prove the compact UI spectrum is measuring
/// frequency content rather than animating copies of the master peak.
struct SineNode final : Node {
    double frequency = 1000.0;

    std::string_view name() const noexcept override { return "spectrum tone"; }
    bool isSource() const noexcept override { return true; }
    void process(const ProcessContext& context) override {
        for (ChannelCount channel = 0; channel < context.output.numChannels();
             ++channel) {
            float* output = context.output.data(channel);
            for (FrameCount frame = 0; frame < context.frames; ++frame) {
                output[frame] = context.playing
                                    ? 0.5f * std::sin(
                                          2.0 * 3.14159265358979323846 * frequency *
                                          double(context.timelinePosition + frame) /
                                          context.sampleRate)
                                    : 0.0f;
            }
        }
    }
};

/// Counts how often the graph prepared it, and how often it was told to
/// suspend. Used to prove that a routing edit does not reinitialise the DSP of
/// nodes that did not change.
struct CountingNode : Node {
    std::string label = "counter";
    int prepareCalls = 0;
    int suspendCalls = 0;
    int resumeCalls = 0;

    std::string_view name() const noexcept override { return label; }
    bool isSource() const noexcept override { return true; }
    void prepare(const PrepareInfo&) override { ++prepareCalls; }
    void suspend() override { ++suspendCalls; }
    void resume() override { ++resumeCalls; }
    void process(const ProcessContext& context) override {
        for (ChannelCount ch = 0; ch < context.output.numChannels(); ++ch) {
            float* data = context.output.data(ch);
            for (FrameCount i = 0; i < context.frames; ++i) data[i] = 1.0f;
        }
    }
};

/// Records the musical time it was handed, so the plumbing from the transport
/// through to a node can be asserted on.
struct TransportProbe : Node {
    TransportInfo seen;
    int blocks = 0;

    std::string_view name() const noexcept override { return "probe"; }
    bool isSource() const noexcept override { return true; }
    void process(const ProcessContext& context) override {
        seen = context.transport;
        ++blocks;
        for (ChannelCount ch = 0; ch < context.output.numChannels(); ++ch) {
            dsp::clear(context.output.channel(ch));
        }
    }
};

/// Synthetic expensive stages for the scheduler regression below. Sleeping is
/// intentional: it makes the dependency gap and successor overlap observable
/// without tying the result to the speed of the machine running the test.
struct SlowRootNode final : Node {
    std::string_view name() const noexcept override { return "slow root"; }
    bool isSource() const noexcept override { return true; }
    void process(const ProcessContext& context) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        for (ChannelCount channel = 0; channel < context.output.numChannels();
             ++channel) {
            std::fill(context.output.data(channel),
                      context.output.data(channel) + context.frames, 1.0f);
        }
    }
};

struct SlowWideNode final : Node {
    SlowWideNode(std::atomic<int>& active, std::atomic<int>& peak)
        : active(active), peak(peak) {}

    std::string_view name() const noexcept override { return "slow wide stage"; }
    void process(const ProcessContext& context) override {
        const int now = active.fetch_add(1, std::memory_order_relaxed) + 1;
        int previous = peak.load(std::memory_order_relaxed);
        while (previous < now &&
               !peak.compare_exchange_weak(previous, now,
                                           std::memory_order_relaxed)) {}

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        for (ChannelCount channel = 0; channel < context.output.numChannels();
             ++channel) {
            dsp::copy(context.output.channel(channel),
                      context.inputs.front().channel(channel));
        }
        active.fetch_sub(1, std::memory_order_relaxed);
    }

    std::atomic<int>& active;
    std::atomic<int>& peak;
};

/// Encodes edge roles into the result: main is added normally, sidechain at
/// 10x. This catches a graph that loses the role array while compiling.
struct InputRoleProbe : Node {
    std::string_view name() const noexcept override { return "input role probe"; }
    void process(const ProcessContext& context) override {
        for (ChannelCount channel = 0; channel < context.output.numChannels();
             ++channel) {
            const std::span<float> out = context.output.channel(channel);
            dsp::clear(out);
            for (std::size_t i = 0; i < context.inputs.size(); ++i) {
                if (context.inputs[i].numChannels() == 0) continue;
                const float scale =
                    i < context.inputRoles.size() &&
                            context.inputRoles[i] == InputRole::Sidechain
                        ? 10.0f
                        : 1.0f;
                dsp::addScaled(out, context.inputs[i].channel(channel), scale);
            }
        }
    }
};

/// Owns the output buffer the processor renders into.
struct OutputBuffer {
    explicit OutputBuffer(ChannelCount channels, FrameCount frames)
        : storage(std::size_t(channels) * frames, 0.0f), pointers(channels),
          channels(channels), frames(frames) {
        for (ChannelCount ch = 0; ch < channels; ++ch) {
            pointers[ch] = storage.data() + std::size_t(ch) * frames;
        }
    }
    AudioBlock block() { return AudioBlock(pointers.data(), channels, frames); }

    std::vector<float> storage;
    std::vector<float*> pointers;
    ChannelCount channels;
    FrameCount frames;
};

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // unbuffered: a hang still shows progress
    constexpr FrameCount kBlock = 256;
    PrepareInfo info;
    info.sampleRate = 48000.0;
    info.maxBlockSize = kBlock;
    info.channels = 2;

    // ── Typed plugin inputs: sidechain stays distinguishable ──
    {
        ConstantSource main{1.0f};
        ConstantSource side{2.0f};
        AudioGraph graph;
        const NodeId mainId = graph.addNode(std::make_unique<SourceNode>(
            "main", &ConstantSource::render, &main));
        const NodeId sideId = graph.addNode(std::make_unique<SourceNode>(
            "sidechain", &ConstantSource::render, &side));
        const NodeId probe = graph.addNode(std::make_unique<InputRoleProbe>());
        graph.connect(mainId, probe);
        graph.connect(sideId, probe, InputRole::Sidechain);
        graph.setSink(probe);
        auto compiled = graph.compile(info);
        check(compiled.has_value(), "a graph with a typed sidechain edge compiles");
        if (compiled) {
            GraphProcessor processor(1);
            processor.setGraph(*compiled);
            OutputBuffer output(2, kBlock);
            processor.process(output.block(), kBlock, 0, true);
            check(std::fabs(output.storage.front() - 21.0f) < 1e-6f,
                  "the compiled ProcessContext preserves main versus sidechain roles");
        }
    }

    // ── A simple mix: three sources → gain → master ──
    {
        AudioGraph graph;
        std::vector<std::unique_ptr<ConstantSource>> sources;
        std::vector<NodeId> sourceNodes;
        for (int i = 0; i < 3; ++i) {
            sources.push_back(std::make_unique<ConstantSource>(ConstantSource{0.25f}));
            sourceNodes.push_back(graph.addNode(std::make_unique<SourceNode>(
                "src", &ConstantSource::render, sources.back().get())));
        }
        auto* gainNode = new GainNode("fader");
        const NodeId gain = graph.addNode(std::unique_ptr<Node>(gainNode));
        const NodeId master = graph.addNode(std::make_unique<SumNode>("master"));
        for (NodeId source : sourceNodes) check(graph.connect(source, gain).has_value(),
                                                "connect source → gain");
        check(graph.connect(gain, master).has_value(), "connect gain → master");
        graph.setSink(master);

        auto compiled = graph.compile(info);
        check(compiled.has_value(), "graph compiles");

        GraphProcessor processor(4);
        processor.setGraph(*compiled);

        OutputBuffer output(2, kBlock);
        check(processor.process(output.block(), kBlock, 0, true).has_value(),
              "parallel process runs");

        // Three sources at 0.25, panned centre through the fader: the
        // constant-power law puts cos(45°) ≈ 0.7071 on each side.
        const float expected = 0.75f;   // centre pan is unity gain
        check(std::fabs(output.storage[0] - expected) < 1e-5f,
              "summed level is correct");
        check(std::fabs(output.storage[kBlock] - expected) < 1e-5f,
              "right channel matches");

        // Buffer reuse: a chain this shape must not need one buffer per node.
        check((*compiled)->arena.bufferCount() <= 4,
              "buffers are recycled rather than one per node");
    }

    // ── Cycles are rejected, not deadlocked ──
    {
        AudioGraph graph;
        const NodeId a = graph.addNode(std::make_unique<SumNode>("a"));
        const NodeId b = graph.addNode(std::make_unique<SumNode>("b"));
        check(graph.connect(a, b).has_value(), "connect a → b");
        check(graph.connect(b, a).has_value(), "connect b → a (accepted as an edge)");
        auto compiled = graph.compile(info);
        check(!compiled.has_value() &&
                  compiled.error() == EngineError::CycleDetected,
              "compilation rejects a feedback cycle");
    }

    // ── Automatic latency compensation ──
    // Two paths into one bus, one of them through a node that delays by 64
    // samples. Without PDC the sum would comb; with it both arrive aligned.
    {
        AudioGraph graph;
        const NodeId direct = graph.addNode(std::make_unique<SourceNode>(
            "ramp-direct", &RampSource::render, nullptr));
        const NodeId delayedSource = graph.addNode(std::make_unique<SourceNode>(
            "ramp-delayed", &RampSource::render, nullptr));
        const NodeId plugin =
            graph.addNode(std::make_unique<LatencyNode>("plugin", 64));
        const NodeId bus = graph.addNode(std::make_unique<SumNode>("bus"));

        check(graph.connect(delayedSource, plugin).has_value(), "source → plugin");
        check(graph.connect(plugin, bus).has_value(), "plugin → bus");
        check(graph.connect(direct, bus).has_value(), "direct → bus");
        graph.setSink(bus);

        auto compiled = graph.compile(info);
        check(compiled.has_value(), "graph with a latent node compiles");
        check((*compiled)->totalLatency == 64,
              "reported graph latency is the longest path");
        check(!(*compiled)->delays.empty(),
              "a compensation delay was inserted on the early path");

        GraphProcessor processor(4);
        processor.setGraph(*compiled);
        OutputBuffer output(2, kBlock);

        // Run enough blocks for the delay lines to fill.
        SamplePos position = 0;
        for (int block = 0; block < 4; ++block) {
            processor.process(output.block(), kBlock, position, true);
            position += kBlock;
        }
        // Both paths now carry the same delayed ramp, so the sum is exactly
        // twice one path — misalignment would show up immediately here.
        const float first = output.storage[0];
        const float expected = 2.0f * float(position - kBlock - 64);
        check(std::fabs(first - expected) < 1e-3f,
              "both paths line up after compensation");
    }

    // ── Parallel and serial must agree, sample for sample ──
    {
        AudioGraph graph;
        std::vector<std::unique_ptr<ConstantSource>> sources;
        const NodeId master = graph.addNode(std::make_unique<SumNode>("master"));
        graph.setSink(master);
        for (int i = 0; i < 64; ++i) {
            sources.push_back(
                std::make_unique<ConstantSource>(ConstantSource{0.01f * float(i + 1)}));
            const NodeId source = graph.addNode(std::make_unique<SourceNode>(
                "src", &ConstantSource::render, sources.back().get()));
            auto* gain = new GainNode("g");
            gain->setGain(0.5f);
            const NodeId gainNode = graph.addNode(std::unique_ptr<Node>(gain));
            graph.connect(source, gainNode);
            graph.connect(gainNode, master);
        }
        auto compiled = graph.compile(info);
        check(compiled.has_value(), "64-track graph compiles");

        GraphProcessor processor(8);
        processor.setGraph(*compiled);

        OutputBuffer parallel(2, kBlock);
        OutputBuffer serial(2, kBlock);
        // One settling block first: the faders ramp from their previous value,
        // so only steady-state blocks are comparable between the two paths.
        processor.process(parallel.block(), kBlock, 0, true);
        processor.process(parallel.block(), kBlock, 0, true);
        processor.processSerial(serial.block(), kBlock, 0, true);

        bool identical = true;
        for (std::size_t i = 0; i < parallel.storage.size(); ++i) {
            if (parallel.storage[i] != serial.storage[i]) identical = false;
        }
        check(identical, "parallel output is bit-identical to serial");

        // Repeat runs must also be identical — a scheduler that summed in
        // completion order would drift here.
        OutputBuffer again(2, kBlock);
        processor.process(again.block(), kBlock, 0, true);
        bool stable = true;
        for (std::size_t i = 0; i < again.storage.size(); ++i) {
            if (again.storage[i] != parallel.storage[i]) stable = false;
        }
        check(stable, "repeated parallel runs are deterministic");
    }

    // ── Compact expensive graph: slow root → wide successor frontier ──
    // At 26 nodes this used to take the serial path solely because it was below
    // the fixed threshold of 64. Forcing the pool alone is insufficient: the
    // one initially requested helper parks while the root sleeps, so the root
    // must also announce the wide frontier when it becomes ready.
    {
        constexpr int kWidth = 24;
        std::atomic<int> active{0};
        std::atomic<int> peak{0};
        AudioGraph graph;
        const NodeId root = graph.addNode(std::make_unique<SlowRootNode>());
        const NodeId sink = graph.addNode(std::make_unique<SumNode>("wide sink"));
        for (int i = 0; i < kWidth; ++i) {
            const NodeId stage = graph.addNode(
                std::make_unique<SlowWideNode>(active, peak));
            graph.connect(root, stage);
            graph.connect(stage, sink);
        }
        graph.setSink(sink);

        auto compiled = graph.compile(info);
        check(compiled.has_value(), "compact slow-root graph compiles");
        check(compiled && (*compiled)->nodes.size() < 64 &&
                  (*compiled)->parallelWidth == kWidth,
              "compiler records the compact graph's wide ready frontier");

        GraphProcessor processor(4);
        processor.setGraph(*compiled);
        OutputBuffer output(2, kBlock);

        const auto parallelStart = std::chrono::steady_clock::now();
        const Status parallelStatus =
            processor.process(output.block(), kBlock, 0, true);
        const auto parallelEnd = std::chrono::steady_clock::now();
        const int parallelPeak = peak.load(std::memory_order_relaxed);
        check(parallelStatus.has_value(),
              "compact wide graph selects the adaptive parallel path");
        check(parallelPeak >= 3,
              "mid-pass fan-out wakes parked helpers, not only the initial pair");
        check(std::fabs(output.storage.front() - float(kWidth)) < 1e-6f,
              "mid-pass waking preserves the graph result");

        active.store(0, std::memory_order_relaxed);
        peak.store(0, std::memory_order_relaxed);
        const auto serialStart = std::chrono::steady_clock::now();
        processor.processSerial(output.block(), kBlock, 0, true);
        const auto serialEnd = std::chrono::steady_clock::now();

        const double parallelMs = std::chrono::duration<double, std::milli>(
                                      parallelEnd - parallelStart).count();
        const double serialMs = std::chrono::duration<double, std::milli>(
                                    serialEnd - serialStart).count();
        std::printf("      slow root -> %d successors: parallel %.2f ms, "
                    "serial %.2f ms, peak width %d\n",
                    kWidth, parallelMs, serialMs, parallelPeak);
        check(peak.load(std::memory_order_relaxed) == 1,
              "serial reference executes the wide stage one node at a time");
    }

    // ── A thousand tracks, shaped like a real project ──
    // Each track is source → fader → 4-band EQ, grouped into 32 buses that feed
    // the master. Flat 1000-into-1 summing would make the master node alone half
    // the total work and cap any scheduler at ~2x, so projects (and this test)
    // sum hierarchically.
    {
        AudioGraph graph;
        std::vector<std::unique_ptr<ConstantSource>> sources;
        sources.reserve(1000);
        const NodeId master = graph.addNode(std::make_unique<SumNode>("master"));
        graph.setSink(master);

        std::vector<NodeId> allNodes{master};
        std::vector<NodeId> buses;
        for (int b = 0; b < 32; ++b) {
            const NodeId bus = graph.addNode(std::make_unique<SumNode>("bus"));
            graph.connect(bus, master);
            buses.push_back(bus);
            allNodes.push_back(bus);
        }

        for (int i = 0; i < 1000; ++i) {
            sources.push_back(std::make_unique<ConstantSource>(ConstantSource{0.001f}));
            const NodeId source = graph.addNode(std::make_unique<SourceNode>(
                "src", &ConstantSource::render, sources.back().get()));
            const NodeId gain = graph.addNode(std::make_unique<GainNode>("fader"));
            const NodeId eq = graph.addNode(std::make_unique<BiquadNode>("eq", 4));
            graph.connect(source, gain);
            graph.connect(gain, eq);
            graph.connect(eq, buses[std::size_t(i % 32)]);
            allNodes.insert(allNodes.end(), {source, gain, eq});
        }

        auto compiled = graph.compile(info);
        check(compiled.has_value(), "1000-track project compiles");
        check((*compiled)->nodes.size() == 3033, "all nodes are scheduled");
        std::printf("      buffers for 3033 nodes: %zu\n",
                    (*compiled)->arena.bufferCount());

        GraphProcessor processor;
        processor.setGraph(*compiled);
        OutputBuffer output(2, kBlock);

        for (int i = 0; i < 10; ++i) processor.process(output.block(), kBlock, 0, true);

        const auto parallelStart = std::chrono::steady_clock::now();
        for (int i = 0; i < 50; ++i) processor.process(output.block(), kBlock, 0, true);
        const auto parallelEnd = std::chrono::steady_clock::now();

        const auto serialStart = std::chrono::steady_clock::now();
        for (int i = 0; i < 50; ++i)
            processor.processSerial(output.block(), kBlock, 0, true);
        const auto serialEnd = std::chrono::steady_clock::now();

        const double parallelMs =
            std::chrono::duration<double, std::milli>(parallelEnd - parallelStart).count();
        const double serialMs =
            std::chrono::duration<double, std::milli>(serialEnd - serialStart).count();
        const double blockBudgetMs = 1000.0 * kBlock / 48000.0;
        std::printf("      1000 tracks x (gain + 4-band EQ), %u workers:\n"
                    "        parallel %.2f ms/block, serial %.2f ms/block,"
                    " speed-up %.2fx (budget %.2f ms)\n",
                    processor.workerCount(), parallelMs / 50.0, serialMs / 50.0,
                    parallelMs > 0.0 ? serialMs / parallelMs : 0.0, blockBudgetMs);
        check(serialMs / parallelMs > 2.0,
              "parallel scheduling scales past 2x on 8 workers");
        // The absolute figure depends on what else the machine is doing, so it
        // is reported rather than asserted; the ratio is the stable signal.
        std::printf("      %s realtime budget on this run\n",
                    parallelMs / 50.0 < blockBudgetMs ? "inside the" : "OVER the");

        // Same graph, same numbers, whichever path renders it. The EQs are
        // stateful, so both runs start from a reset and render the same number
        // of blocks — comparing one parallel block against one serial block
        // afterwards would only be comparing different filter states.
        auto renderFrom = [&](bool parallel, OutputBuffer& into) {
            for (NodeId id : allNodes) {
                if (Node* n = graph.node(id)) n->reset();
            }
            for (int block = 0; block < 4; ++block) {
                if (parallel) {
                    processor.process(into.block(), kBlock, block * kBlock, true);
                } else {
                    processor.processSerial(into.block(), kBlock, block * kBlock, true);
                }
            }
        };
        OutputBuffer parallelOut(2, kBlock);
        OutputBuffer serialOut(2, kBlock);
        renderFrom(true, parallelOut);
        renderFrom(false, serialOut);
        bool identical = true;
        for (std::size_t i = 0; i < parallelOut.storage.size(); ++i) {
            if (std::fabs(parallelOut.storage[i] - serialOut.storage[i]) > 1e-6f) {
                identical = false;
            }
        }
        check(identical, "1000-track parallel mix matches the serial mix");
    }

    // ── The live path: transport + clip playback through renderBlock ──
    // Everything the device callback drives, minus PortAudio itself.
    {
        RealtimeEngine live(4);
        live.prepare(48000.0, kBlock, 2);

        // One second of DC at 0.5 so the arithmetic is obvious.
        auto samples = std::make_shared<SampleBuffer>(2, 48000, 48000.0);
        for (ChannelCount ch = 0; ch < 2; ++ch) {
            float* data = samples->writableChannel(ch);
            for (FrameCount i = 0; i < 48000; ++i) data[i] = 0.5f;
        }

        auto player = std::make_shared<ClipPlayerNode>("clips");
        auto fader = std::make_shared<GainNode>("fader");
        const NodeId playerId = live.graph().adoptNode(player);
        const NodeId faderId = live.graph().adoptNode(fader);
        live.graph().connect(playerId, faderId);
        live.graph().setSink(faderId);
        check(live.commitGraph().has_value(), "live graph commits");

        auto clips = std::make_shared<ClipPlayerNode::ClipList>();
        ClipPlacement placement;
        placement.audio = samples;
        placement.startSample = kBlock;      // starts one block in
        placement.lengthSamples = 48000;
        clips->push_back(placement);
        player->setClips(clips);

        OutputBuffer out(2, kBlock);
        live.transport().seek(0);

        // Stopped: the graph runs but the timeline produces nothing.
        live.renderBlock(out.block(), nullptr, 0, kBlock);
        check(out.storage[0] == 0.0f, "a stopped transport renders silence");
        check(live.transport().position() == 0, "a stopped transport stays put");

        live.transport().play();
        live.renderBlock(out.block(), nullptr, 0, kBlock);
        check(live.transport().position() == SamplePos(kBlock),
              "the playhead advances by one block");
        const double displayedAtCallback =
            live.transport().presentationPositionSeconds();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        const double displayedBetweenCallbacks =
            live.transport().presentationPositionSeconds();
        check(displayedBetweenCallbacks > displayedAtCallback &&
                  displayedBetweenCallbacks <= live.transport().positionSeconds(),
              "the display playhead interpolates inside an audio block");
        check(out.storage[0] == 0.0f, "nothing plays before the clip starts");

        live.renderBlock(out.block(), nullptr, 0, kBlock);
        check(std::fabs(out.storage[kBlock / 2] - 0.5f) < 1e-6f,
              "the clip plays once the playhead reaches it");
        check(std::fabs(live.masterPeakLeft() - 0.5f) < 1e-6f,
              "master metering follows the output");

        // Looping wraps the playhead inside the range.
        live.transport().setLoopRange(0, kBlock * 2);
        live.transport().setLoopEnabled(true);
        live.transport().seek(kBlock * 2 - 16);
        live.renderBlock(out.block(), nullptr, 0, kBlock);
        check(live.transport().position() < SamplePos(kBlock),
              "the loop wraps the playhead");
    }

    // ── User fade curves and tape-speed fades ──
    {
        constexpr FrameCount frames = 1000;
        constexpr SampleRate rate = 1000.0;
        auto ramp = std::make_shared<SampleBuffer>(1, frames, rate);
        for (FrameCount i = 0; i < frames; ++i)
            ramp->writableChannel(0)[i] = float(i) / float(frames);

        ClipPlayerNode player("fade test");
        player.prepare({rate, frames, 1, false});
        OutputBuffer out(1, frames);
        ProcessContext context;
        context.output = out.block();
        context.frames = frames;
        context.sampleRate = rate;
        context.playing = true;

        auto render = [&](ClipPlacement placement) {
            auto list = std::make_shared<ClipPlayerNode::ClipList>();
            placement.audio = ramp;
            placement.lengthSamples = frames;
            placement.fadeInSamples = frames;
            list->push_back(placement);
            player.setClips(list);
            player.process(context);
            return out.storage[500];
        };

        const float linear = render({});
        ClipPlacement curved;
        curved.fadeInCurve = 1.0f;
        const float bent = render(curved);
        check(linear > 0.24f && linear < 0.26f && bent > linear * 1.5f,
              "a positive fade curve reaches level sooner than a linear fade");

        ClipPlacement tape;
        tape.tapeStartSamples = frames;
        const float wound = render(tape);
        check(wound > 0.05f && wound < 0.08f && wound < linear * 0.4f,
              "Tape Start integrates a 0x-to-1x speed ramp without a source jump");
    }

    // ── Master spectrum for the transport display ──
    {
        RealtimeEngine live(1);
        live.prepare(48000.0, kBlock, 2);
        auto tone = std::make_shared<SineNode>();
        const NodeId toneId = live.graph().adoptNode(tone);
        live.graph().setSink(toneId);
        check(live.commitGraph().has_value(), "spectrum test graph commits");

        OutputBuffer out(2, kBlock);
        live.transport().play();
        for (int block = 0; block < 8; ++block) {
            live.renderBlock(out.block(), nullptr, 0, kBlock);
        }
        const RealtimeEngine::MasterSpectrum gated = live.masterSpectrum();
        check(*std::max_element(gated.begin(), gated.end()) == 0.0f &&
                  live.masterPeakLeft() > 0.1f,
              "master spectrum work stays gated with no consumer");

        live.addMasterSpectrumConsumer();
        for (int block = 0; block < 80; ++block) {
            live.renderBlock(out.block(), nullptr, 0, kBlock);
        }

        const RealtimeEngine::MasterSpectrum spectrum = live.masterSpectrum();
        const std::size_t strongest = std::size_t(
            std::max_element(spectrum.begin(), spectrum.end()) - spectrum.begin());
        check(strongest >= 5 && strongest <= 7,
              "a 1 kHz tone raises the middle spectrum bars");
        check(spectrum[strongest] > spectrum.front() * 4.0f &&
                  spectrum[strongest] > spectrum.back() * 4.0f,
              "spectrum bands describe frequency, not twelve copies of level");

        live.transport().stop();
        for (int block = 0; block < 200; ++block) {
            live.renderBlock(out.block(), nullptr, 0, kBlock);
        }
        const RealtimeEngine::MasterSpectrum silent = live.masterSpectrum();
        check(*std::max_element(silent.begin(), silent.end()) < 0.005f,
              "spectrum releases to silence after transport stops");

        live.removeMasterSpectrumConsumer();
        live.renderBlock(out.block(), nullptr, 0, kBlock);
        const RealtimeEngine::MasterSpectrum disabled = live.masterSpectrum();
        check(*std::max_element(disabled.begin(), disabled.end()) == 0.0f,
              "removing the last spectrum consumer clears and gates the bands");
    }

    // ── A recompile only prepares what actually changed ──
    //
    // Every routing edit re-adopts the same node objects into a fresh graph.
    // Preparing them all again is not just wasted work: `prepare()` reallocates
    // the scratch `process()` reads, and the previously published snapshot —
    // pointing at these very objects — may still be rendering.
    {
        AudioGraph graph;
        auto counter = std::make_shared<CountingNode>();
        const NodeId source = graph.adoptNode(counter);
        const NodeId master = graph.addNode(std::make_unique<SumNode>("master"));
        graph.connect(source, master);
        graph.setSink(master);

        check(graph.compile(info).has_value(), "graph with a counting node compiles");
        check(counter->prepareCalls == 1, "a node new to the graph is prepared once");

        auto second = graph.compile(info);
        check(second.has_value(), "recompiling with identical settings works");
        check(counter->prepareCalls == 1,
              "an unchanged node is NOT re-prepared on recompile");

        // A fresh AudioGraph is exactly what EngineController::rebuildGraph
        // builds on every edit, so the stamp has to survive that too.
        AudioGraph rebuilt;
        const NodeId again = rebuilt.adoptNode(counter);
        const NodeId master2 = rebuilt.addNode(std::make_unique<SumNode>("master"));
        rebuilt.connect(again, master2);
        rebuilt.setSink(master2);
        check(rebuilt.compile(info).has_value(), "rebuilt graph compiles");
        check(counter->prepareCalls == 1,
              "the prepare stamp survives a whole-graph rebuild");

        // Changed settings must still reach the node.
        PrepareInfo bigger = info;
        bigger.maxBlockSize = kBlock * 2;
        check(rebuilt.compile(bigger).has_value(), "compiling at a new block size works");
        check(counter->prepareCalls == 2,
              "a changed PrepareInfo does re-prepare the node");

        // suspend/resume belong to whoever owns the node, not to the graph.
        check(counter->suspendCalls == 0 && counter->resumeCalls == 0,
              "compiling never suspends or resumes a node behind its owner's back");
    }

    // ── Compensation delay lines survive a recompile ──
    //
    // Before this, every project edit rebuilt every EdgeDelay from silence, so
    // any graph with latency in it clicked on every click in the mixer.
    {
        auto build = [&](AudioGraph& graph) {
            const NodeId direct = graph.addNode(std::make_unique<SourceNode>(
                "ramp-direct", &RampSource::render, nullptr));
            const NodeId delayedSource = graph.addNode(std::make_unique<SourceNode>(
                "ramp-delayed", &RampSource::render, nullptr));
            const NodeId plugin =
                graph.addNode(std::make_unique<LatencyNode>("plugin", 64));
            const NodeId bus = graph.addNode(std::make_unique<SumNode>("bus"));
            graph.connect(delayedSource, plugin);
            graph.connect(plugin, bus);
            graph.connect(direct, bus);
            graph.setSink(bus);
        };

        AudioGraph graph;
        build(graph);
        auto first = graph.compile(info);
        check(first.has_value() && !(*first)->delays.empty(),
              "the compensated graph compiles with a delay line");

        GraphProcessor processor(4);
        processor.setGraph(*first);
        OutputBuffer output(2, kBlock);
        SamplePos position = 0;
        for (int block = 0; block < 4; ++block) {
            processor.process(output.block(), kBlock, position, true);
            position += kBlock;
        }

        // Recompile handing over the published snapshot: the ring is adopted,
        // so the next block continues the ramp exactly where it left off.
        auto carried = graph.compile(info, first->get());
        check(carried.has_value(), "recompile with a previous snapshot works");
        check(!(*carried)->delays.empty() &&
                  (*carried)->delays[0] == (*first)->delays[0],
              "the delay line object itself is carried over");

        processor.setGraph(*carried);
        processor.process(output.block(), kBlock, position, true);
        const float continued = output.storage[0];
        const float expected = 2.0f * float(position - 64);
        check(std::fabs(continued - expected) < 1e-3f,
              "the compensated path continues without a discontinuity");

        PrepareInfo changedRate = info;
        changedRate.sampleRate = 44100.0;
        auto rateChanged = graph.compile(changedRate, carried->get());
        check(rateChanged.has_value() &&
                  (*rateChanged)->delays[0] != (*carried)->delays[0],
              "a sample-rate change cannot adopt delay history from the old clock");

        // And the opposite: no previous snapshot means a fresh, silent ring —
        // this is what used to happen on every single edit.
        auto restarted = graph.compile(info);
        check(restarted.has_value() &&
                  (*restarted)->delays[0] != (*first)->delays[0],
              "compiling without a previous snapshot builds a new delay line");
    }

    // ── Publishing a graph while the renderer runs ──
    //
    // The per-block scratch used to live beside the pointer instead of inside
    // the snapshot, so a rebuild landing mid-block let the renderer index a
    // moved-from vector. Hammer that window: one thread renders continuously
    // while another republishes, and every block must still sum exactly.
    {
        AudioGraph graph;
        std::vector<std::unique_ptr<ConstantSource>> sources;
        const NodeId master = graph.addNode(std::make_unique<SumNode>("master"));
        for (int i = 0; i < 80; ++i) {
            sources.push_back(std::make_unique<ConstantSource>(ConstantSource{0.01f}));
            graph.connect(graph.addNode(std::make_unique<SourceNode>(
                              "src", &ConstantSource::render, sources.back().get())),
                          master);
        }
        graph.setSink(master);

        GraphProcessor processor(4);
        auto initial = graph.compile(info);
        check(initial.has_value(), "the 80-source graph compiles");
        processor.setGraph(*initial);

        std::atomic<bool> stop{false};
        std::atomic<int> blocks{0};
        std::atomic<int> wrongSums{0};
        std::thread renderer([&] {
            OutputBuffer out(2, kBlock);
            while (!stop.load()) {
                if (processor.process(out.block(), kBlock, 0, true)) {
                    // 80 sources × 0.01 — the value must never be partial.
                    if (std::fabs(out.storage[0] - 0.8f) > 1e-3f) ++wrongSums;
                }
                ++blocks;
            }
        });

        std::shared_ptr<const CompiledGraph> latest = *initial;
        for (int i = 0; i < 500; ++i) {
            auto next = graph.compile(info, latest.get());
            if (!next) { ++wrongSums; break; }
            latest = *next;
            processor.setGraph(latest);
        }
        stop.store(true);
        renderer.join();

        check(blocks.load() > 0, "the renderer kept running across 500 rebuilds");
        check(wrongSums.load() == 0,
              "every block summed exactly while graphs were being republished");
    }

    // ── The render gate ──
    //
    // What makes it safe to re-prepare live nodes: while the gate is held the
    // renderer is parked and outputs silence, and the gate does not close until
    // any block already in flight has finished.
    {
        RealtimeEngine live;
        check(live.prepare(48000.0, kBlock, 2).has_value(), "engine prepares");
        auto source = std::make_shared<CountingNode>();   // writes 1.0 everywhere
        const NodeId id = live.graph().adoptNode(source);
        live.graph().setSink(id);
        check(live.commitGraph().has_value(), "gate-test graph commits");

        OutputBuffer out(2, kBlock);
        live.renderBlock(out.block(), nullptr, 0, kBlock);
        check(std::fabs(out.storage[0] - 1.0f) < 1e-6f, "the node renders normally");

        {
            const RealtimeEngine::RenderGate gate(live);
            live.renderBlock(out.block(), nullptr, 0, kBlock);
            check(out.storage[0] == 0.0f, "a gated renderer outputs silence");
        }

        live.renderBlock(out.block(), nullptr, 0, kBlock);
        check(std::fabs(out.storage[0] - 1.0f) < 1e-6f,
              "audio returns once the gate is released");

        // A gate taken while a block is running must wait for that block. Run
        // the renderer continuously and grab the gate underneath it: every
        // block must come out either fully rendered or fully silent, never a
        // half-written buffer, and the gate must never fail to close.
        std::atomic<bool> stop{false};
        std::atomic<int> rendered{0};
        std::atomic<int> torn{0};
        std::thread renderer([&] {
            OutputBuffer local(2, kBlock);
            while (!stop.load()) {
                live.renderBlock(local.block(), nullptr, 0, kBlock);
                // The node writes 1.0 to every sample, so a block is either all
                // ones (rendered) or all zeros (gated). Anything else means the
                // control thread reconfigured underneath a live block.
                const float first = local.storage[0];
                const float last = local.storage[kBlock - 1];
                if (first != last || (first != 0.0f && std::fabs(first - 1.0f) > 1e-6f)) {
                    ++torn;
                }
                ++rendered;
            }
        });

        // Do not start gating until the renderer is actually running, or the
        // loop below can finish before that thread is ever scheduled.
        while (rendered.load() == 0) std::this_thread::yield();

        PrepareInfo liveInfo;
        liveInfo.sampleRate = live.sampleRate();
        liveInfo.maxBlockSize = live.maxBlockSize();
        liveInfo.channels = live.channels();
        const int before = rendered.load();
        for (int i = 0; i < 200; ++i) {
            const RealtimeEngine::RenderGate gate(live);
            // Inside the gate the renderer is parked, so this is the window in
            // which prepare() may reallocate the scratch process() reads.
            source->prepare(liveInfo);
        }
        stop.store(true);
        renderer.join();

        check(rendered.load() > before,
              "the renderer kept producing blocks across 200 gate acquisitions");
        check(torn.load() == 0,
              "no block was reconfigured while it was being rendered");
    }

    // ── Musical time: sample ↔ quarter note, and the bar it falls in ──
    {
        Transport transport;
        transport.setSampleRate(48000.0);
        transport.setTempo(120.0);              // 24000 samples per quarter note
        transport.setTimeSignature(4, 4);

        check(std::fabs(transport.samplesPerBeat() - 24000.0) < 1e-9,
              "120 BPM at 48 kHz is 24000 samples per beat");
        check(std::fabs(transport.ppqAt(24000) - 1.0) < 1e-9,
              "one beat in equals ppq 1.0");
        check(std::fabs(transport.ppqAt(12000) - 0.5) < 1e-9,
              "half a beat in equals ppq 0.5");

        // Bar length is measured in quarter notes, so the denominator matters.
        check(std::fabs(transport.beatsPerBar() - 4.0) < 1e-9, "4/4 is 4 quarters");
        transport.setTimeSignature(3, 4);
        check(std::fabs(transport.beatsPerBar() - 3.0) < 1e-9, "3/4 is 3 quarters");
        transport.setTimeSignature(6, 8);
        check(std::fabs(transport.beatsPerBar() - 3.0) < 1e-9,
              "6/8 is 3 quarters, not 6");

        // Bar starts, a bar and a bit into the timeline.
        transport.setTimeSignature(4, 4);
        TransportInfo info = transport.infoAt(24000 * 5);   // ppq 5.0
        check(std::fabs(info.ppqPosition - 5.0) < 1e-9, "ppq at five beats in");
        check(std::fabs(info.barStartPpq - 4.0) < 1e-9,
              "in 4/4, beat five is in the bar starting at ppq 4");

        transport.setTimeSignature(3, 4);
        info = transport.infoAt(24000 * 5);
        check(std::fabs(info.barStartPpq - 3.0) < 1e-9,
              "in 3/4, beat five is in the bar starting at ppq 3");

        transport.setTimeSignature(4, 4);
        transport.setLoopRange(0, 24000 * 8);
        transport.setLoopEnabled(true);
        info = transport.infoAt(0);
        check(std::fabs(info.loopEndPpq - 8.0) < 1e-9 && info.looping,
              "the loop range is reported in quarter notes");
    }

    // ── The transport reaches a node, on both scheduling paths ──
    {
        AudioGraph graph;
        auto probe = std::make_shared<TransportProbe>();
        const NodeId id = graph.adoptNode(probe);
        graph.setSink(id);
        auto compiled = graph.compile(info);
        check(compiled.has_value(), "probe graph compiles");

        GraphProcessor processor(4);
        processor.setGraph(*compiled);
        OutputBuffer out(2, kBlock);

        TransportInfo musical;
        musical.tempo = 90.0;
        musical.timeSigNumerator = 3;
        musical.timeSigDenominator = 4;
        musical.ppqPosition = 7.5;
        musical.barStartPpq = 6.0;

        processor.processSerial(out.block(), kBlock, 0, true, false, musical);
        check(probe->seen.tempo == 90.0 && probe->seen.timeSigNumerator == 3 &&
                  std::fabs(probe->seen.ppqPosition - 7.5) < 1e-9,
              "the serial path hands the transport to the node");

        probe->seen = TransportInfo{};
        processor.setParallelThreshold(1);      // force the pool for one node
        processor.process(out.block(), kBlock, 0, true, false, musical);
        check(probe->seen.tempo == 90.0 &&
                  std::fabs(probe->seen.barStartPpq - 6.0) < 1e-9,
              "the parallel path hands over the same transport");
        processor.setParallelThreshold(64);
    }

    // ── Offline rendering must reproduce the live path exactly ──
    //
    // The metronome is a pure function of position and musical time, so it is
    // the sharpest possible check that both paths derive the beat identically.
    {
        auto renderWith = [&](bool offline) {
            RealtimeEngine engine;
            engine.prepare(48000.0, kBlock, 2);
            engine.transport().setTempo(120.0);
            engine.transport().setTimeSignature(4, 4);

            auto click = std::make_shared<MetronomeNode>();
            click->setEnabled(true);
            const NodeId id = engine.graph().adoptNode(click);
            engine.graph().setSink(id);
            engine.commitGraph();

            std::vector<float> captured;
            if (offline) {
                engine.renderOffline(0, SamplePos(kBlock) * 8, kBlock,
                                     [&](const AudioBlock& block, FrameCount frames) {
                                         for (FrameCount i = 0; i < frames; ++i) {
                                             captured.push_back(block.data(0)[i]);
                                         }
                                         return true;
                                     });
            } else {
                OutputBuffer out(2, kBlock);
                engine.transport().play();
                for (int block = 0; block < 8; ++block) {
                    engine.renderBlock(out.block(), nullptr, 0, kBlock);
                    for (FrameCount i = 0; i < kBlock; ++i) {
                        captured.push_back(out.storage[i]);
                    }
                }
            }
            return captured;
        };

        const std::vector<float> live = renderWith(false);
        const std::vector<float> bounced = renderWith(true);
        check(live.size() == bounced.size() && !live.empty(),
              "both paths rendered the same span");
        bool identical = live.size() == bounced.size();
        for (std::size_t i = 0; identical && i < live.size(); ++i) {
            if (live[i] != bounced[i]) identical = false;
        }
        check(identical, "the offline render is bit-identical to the live one");

        bool anySound = false;
        for (float sample : live) {
            if (sample != 0.0f) anySound = true;
        }
        check(anySound, "the metronome actually clicked (a silent match is no match)");
    }

    // ── The metronome follows the transport it is given ──
    {
        RealtimeEngine engine;
        engine.prepare(48000.0, kBlock, 2);
        auto click = std::make_shared<MetronomeNode>();
        click->setEnabled(true);
        const NodeId id = engine.graph().adoptNode(click);
        engine.graph().setSink(id);
        engine.commitGraph();

        // The downbeat accent is louder than the off-beat click, so peak level
        // over a beat tells the two apart without decoding the waveform.
        auto peakOverBeat = [&](double bpm, int numerator, int denominator,
                                SamplePos startSample) {
            engine.transport().setTempo(bpm);
            engine.transport().setTimeSignature(numerator, denominator);
            engine.transport().seek(startSample);
            engine.transport().play();
            OutputBuffer out(2, kBlock);
            float peak = 0.0f;
            for (int block = 0; block < 8; ++block) {
                engine.renderBlock(out.block(), nullptr, 0, kBlock);
                for (FrameCount i = 0; i < kBlock; ++i) {
                    peak = std::max(peak, std::fabs(out.storage[i]));
                }
            }
            return peak;
        };

        // Beat 0 is a downbeat in any metre; in 4/4 beat 1 is not.
        const float downbeat = peakOverBeat(120.0, 4, 4, 0);
        const float offbeat = peakOverBeat(120.0, 4, 4, 24000);
        check(downbeat > offbeat,
              "the downbeat accent is louder than an off-beat click");

        // In 3/4 the accent returns every three beats, so beat 3 is a downbeat
        // where in 4/4 it would not be.
        const float threeFour = peakOverBeat(120.0, 3, 4, 24000 * 3);
        const float fourFour = peakOverBeat(120.0, 4, 4, 24000 * 3);
        check(threeFour > fourFour,
              "the metre from the transport decides where the accent falls");
    }

    // ── The count-in clicks with the transport parked and the metronome off ──
    // A count-in is heard before anything is rolling, which is exactly the case
    // the beat-locked clicks above produce nothing for.
    {
        RealtimeEngine engine;
        engine.prepare(48000.0, kBlock, 2);
        engine.transport().setTempo(120.0);        // one beat = 24000 frames
        engine.transport().setTimeSignature(4, 4);

        auto click = std::make_shared<MetronomeNode>();
        click->setEnabled(false);                  // deliberately switched off
        const NodeId id = engine.graph().adoptNode(click);
        engine.graph().setSink(id);
        engine.commitGraph();

        OutputBuffer out(2, kBlock);
        auto renderBeats = [&](double beats) {
            // How many clicks land in the next `beats` beats' worth of blocks.
            int clicks = 0;
            bool inClick = false;
            const int blocks = int(beats * 24000.0 / double(kBlock));
            for (int block = 0; block < blocks; ++block) {
                engine.renderBlock(out.block(), nullptr, 0, kBlock);
                float peak = 0.0f;
                for (FrameCount i = 0; i < kBlock; ++i)
                    peak = std::max(peak, std::fabs(out.storage[i]));
                const bool loud = peak > 0.01f;
                if (loud && !inClick) ++clicks;
                inClick = loud;
            }
            return clicks;
        };

        check(renderBeats(3.0) == 0,
              "a metronome that is off and parked stays silent");

        click->requestCountIn(3);
        const int counted = renderBeats(3.5);
        check(counted == 3,
              "three count-in clicks are heard although the transport never rolled");
        check(renderBeats(3.0) == 0, "and it goes quiet again afterwards");
    }

    // ── PreviewPlayerNode: auditioning a file ──
    //
    // The one thing ClipPlayerNode cannot do — sound with the transport parked
    // — plus the loop seam, the rate conversion and the two ways it has to stay
    // out of an export.
    {
        constexpr FrameCount kFrames = 512;
        constexpr SampleRate kRate = 48000.0;

        // A short buffer of a constant value: any read is trivially checkable,
        // and the end is unambiguous.
        auto source = std::make_shared<SampleBuffer>(2, 1000, kRate);
        for (ChannelCount ch = 0; ch < 2; ++ch) {
            float* data = const_cast<float*>(source->channel(ch));
            for (FrameCount i = 0; i < 1000; ++i) data[i] = 0.5f;
        }

        PreviewPlayerNode preview;
        preview.prepare({kRate, kFrames, 2, false});

        OutputBuffer out(2, kFrames);
        ProcessContext context;
        context.output = out.block();
        context.frames = kFrames;
        context.sampleRate = kRate;
        context.playing = false;          // the whole point: the transport is parked
        context.offline = false;

        const auto peak = [&] {
            float loudest = 0.0f;
            for (float sample : out.storage)
                loudest = std::max(loudest, std::fabs(sample));
            return loudest;
        };
        const auto render = [&] { preview.process(context); };

        render();
        check(peak() < 1e-6f, "a preview node is silent until it is asked");

        preview.start(source);
        render();
        check(peak() > 0.4f,
              "and audible with the transport stopped, which is what it is for");
        check(preview.playing() && preview.positionFrames() == kFrames,
              "the position it publishes is where the audio thread has read to");

        // 1000 frames of source, 512 a block: the second block runs out.
        render();
        check(peak() > 0.4f, "the second block still has source left");
        render();
        check(peak() < 1e-6f && !preview.playing(),
              "a one-shot goes quiet at the end of the file");
        check(preview.positionFrames() == 0,
              "and republishes position 0 so a playhead goes home");

        // Looping: the same run must never fall silent, and the seam must be
        // filled from the head of the source rather than left as a gap.
        preview.setLoop(true);
        preview.start(source);
        bool everSilent = false;
        for (int block = 0; block < 8; ++block) {
            render();
            if (peak() < 0.4f) everSilent = true;
        }
        check(!everSilent && preview.playing(),
              "a looping preview plays across the seam without a gap");

        // Offline: an export must contain the project, not the audition.
        context.offline = true;
        render();
        check(peak() < 1e-6f, "and nothing at all reaches an offline render");
        context.offline = false;

        preview.reset();
        render();
        check(peak() < 1e-6f && !preview.playing(),
              "reset stops it — which is what keeps it out of a mixdown");

        // A 44.1 kHz file on a 48 kHz device: read with a step, and never past
        // the end of the buffer.
        auto offRate = std::make_shared<SampleBuffer>(1, 441, 44100.0);
        for (FrameCount i = 0; i < 441; ++i)
            const_cast<float*>(offRate->channel(0))[i] = 0.5f;
        preview.setLoop(false);
        preview.start(offRate);
        // `playing()` only becomes true once the audio thread has consumed the
        // command, so the first block is unconditional.
        int blocks = 0;
        do {
            render();
            ++blocks;
        } while (preview.playing() && blocks < 16);
        check(!preview.playing() && blocks == 1,
              "a file at another rate is read with a step and ends cleanly");

        // Seeking, and the gain the browser's level control writes.
        preview.setGain(0.5f);
        preview.start(source);
        preview.seekFrames(900);
        render();
        check(peak() > 0.2f && peak() < 0.3f, "the preview gain scales the audition");
        check(!preview.playing(),
              "and a seek near the end still ends where the file does");
    }

    std::printf("\n%s\n", failures == 0 ? "ALL PASSED" : "FAILURES PRESENT");
    return failures;
}
