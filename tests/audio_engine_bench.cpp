// Deterministic headless A/B benchmark. No hardware xrun claim is possible here.
#include "Graph/AudioGraph.hpp"
#include "Graph/GraphProcessor.hpp"
#include "Nodes/BasicNodes.hpp"
#include "Nodes/MeterNode.hpp"
#include "RealtimeMetrics.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>
using namespace daw::engine;
struct Tone final : Node {
    std::string_view name() const noexcept override { return "source"; }
    MidiNodeRole midiRole() const noexcept override { return MidiNodeRole::None; }
    void process(const ProcessContext& c) override {
        for (ChannelCount ch = 0; ch < c.output.numChannels(); ++ch)
            std::fill(c.output.channel(ch).begin(), c.output.channel(ch).end(), .001f);
    }
};
int main(int argc, char** argv) {
    unsigned tracks = 1000, frames = 256, workers = 0, blocks = 300; double rate = 48000; bool heavy = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--heavy")) heavy = true;
        else if (i + 1 < argc) {
            const auto* option = argv[i]; const auto* value = argv[++i];
            if (!std::strcmp(option, "--tracks")) tracks = std::atoi(value);
            else if (!std::strcmp(option, "--frames")) frames = std::atoi(value);
            else if (!std::strcmp(option, "--workers")) workers = std::atoi(value);
            else if (!std::strcmp(option, "--blocks")) blocks = std::atoi(value);
            else if (!std::strcmp(option, "--rate")) rate = std::atof(value);
            else return 2;
        } else return 2;
    }
    if (!tracks || tracks > 10000 || !frames || frames > 8192 || !blocks || blocks > 1000000 || rate < 8000 || rate > 384000) return 2;
    AudioGraph graph;
    auto master = graph.addNode(std::make_unique<SumNode>("master")); graph.setSink(master);
    for (unsigned track = 0; track < tracks; ++track) {
        auto previous = graph.addNode(std::make_unique<Tone>());
        if (heavy) {
            const auto eq = graph.addNode(std::make_unique<BiquadNode>("EQ", 4));
            graph.connect(previous, eq); previous = eq;
        }
        for (unsigned i = 0; i < 3; ++i) {
            const auto next = graph.addNode(std::make_unique<GainNode>("gain"));
            graph.connect(previous, next); previous = next;
        }
        const auto meter = graph.addNode(std::make_unique<MeterNode>("meter"));
        graph.connect(previous, meter); graph.connect(meter, master);
    }
    auto compiled = graph.compile({rate, frames, 2}); if (!compiled) return 1;
    GraphProcessor processor(workers); processor.setGraph(*compiled);
    std::vector<float> left(frames), right(frames); float* pointers[]{left.data(), right.data()}; AudioBlock output(pointers, 2, frames);
    std::printf("tracks=%u nodes=%zu fused_tasks=%u workers=%u rate=%.0f frames=%u EQ=%d\n", tracks, (*compiled)->nodes.size(), (*compiled)->taskCount, processor.workerCount(), rate, frames, heavy);
    std::printf("round,fusion,mean_ms,p95_ms,p99_ms,p99_9_ms,max_ms,over_budget\n");
    for (unsigned round = 0; round < 3; ++round) for (unsigned mode = 0; mode < 2; ++mode) {
        const bool fusion = (mode ^ (round & 1)) != 0;
        processor.setTaskFusion(fusion);
        for (unsigned i = 0; i < 20; ++i) processor.process(output, frames, i * frames, true);
        auto metrics = std::make_unique<daw::rt::BlockMetrics>(); daw::rt::TimingAccumulator samples;
        for (unsigned i = 0; i < blocks; ++i) {
            const auto start = daw::rt::nowNanos(); processor.process(output, frames, i * frames, true);
            metrics->record(daw::rt::nowNanos() - start, frames, rate);
            samples.drain(*metrics);
        }
        const auto s = samples.summary();
        std::printf("%u,%u,%.4f,%.4f,%.4f,%.4f,%.4f,%llu\n", round, fusion, s.meanMs, s.p95Ms, s.p99Ms, s.p999Ms, s.maximumMs, (unsigned long long)s.overruns);
    }
}
