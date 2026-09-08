// Deterministic headless A/B benchmark. No hardware xrun claim is possible here.
#include "Graph/AudioGraph.hpp"
#include "Graph/GraphProcessor.hpp"
#include "Nodes/BasicNodes.hpp"
#include "Nodes/MeterNode.hpp"
#include "Nodes/PlaybackNodes.hpp"
#include "RealtimeMetrics.hpp"
#include "Job/AudioWorkerRegistration.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <ctime>
#include <thread>
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
    unsigned tracks = 1000, frames = 256, workers = 0, blocks = 300, rounds = 3;
    double rate = 48000;
    bool heavy = false, paced = false, realtimeWorkers = false, serial = false, clips = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--heavy")) heavy = true;
        else if (!std::strcmp(argv[i], "--paced")) paced = true;
        else if (!std::strcmp(argv[i], "--realtime-workers")) realtimeWorkers = true;
        else if (!std::strcmp(argv[i], "--serial")) serial = true;
        else if (!std::strcmp(argv[i], "--clips")) clips = true;
        else if (i + 1 < argc) {
            const auto* option = argv[i]; const auto* value = argv[++i];
            if (!std::strcmp(option, "--tracks")) tracks = std::atoi(value);
            else if (!std::strcmp(option, "--frames")) frames = std::atoi(value);
            else if (!std::strcmp(option, "--workers")) workers = std::atoi(value);
            else if (!std::strcmp(option, "--blocks")) blocks = std::atoi(value);
            else if (!std::strcmp(option, "--rounds")) rounds = std::atoi(value);
            else if (!std::strcmp(option, "--rate")) rate = std::atof(value);
            else return 2;
        } else return 2;
    }
    if (!tracks || tracks > 10000 || !frames || frames > 8192 || !blocks || blocks > 1000000 || !rounds || rounds > 100 || rate < 8000 || rate > 384000) return 2;
    AudioGraph graph;
    auto master = graph.addNode(std::make_unique<SumNode>("master")); graph.setSink(master);
    for (unsigned track = 0; track < tracks; ++track) {
        NodeId previous;
        if (clips) {
            auto sample = std::make_shared<SampleBuffer>(2, 262144, rate);
            for (unsigned ch = 0; ch < 2; ++ch)
                std::fill_n(sample->writableChannel(ch), sample->frames(), .001f);
            auto player = std::make_unique<ClipPlayerNode>();
            auto placements = std::make_shared<ClipPlayerNode::ClipList>();
            ClipPlacement placement;
            placement.audio = std::move(sample);
            placement.lengthSamples = placement.audio->frames();
            // Match EngineController's ordinary clip placements, including the
            // explicit source bounds even when no sample edits are present.
            placement.sourceStartFrame = 0;
            placement.sourceEndFrame = placement.audio->frames();
            placements->push_back(std::move(placement));
            player->setClips(std::move(placements));
            previous = graph.addNode(std::move(player));
        } else previous = graph.addNode(std::make_unique<Tone>());
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
    AudioWorkerRegistration callerRegistration;
    unsigned callerStatus = 0;
    if (realtimeWorkers) {
        callerStatus = callerRegistration.configure({true, rate, frames, {}});
        processor.configureAudioWorkers({true, rate, frames, {}});
    }
    std::vector<float> left(frames), right(frames); float* pointers[]{left.data(), right.data()}; AudioBlock output(pointers, 2, frames);
    std::printf("tracks=%u nodes=%zu fused_tasks=%u workers=%u rate=%.0f frames=%u EQ=%d\n", tracks, (*compiled)->nodes.size(), (*compiled)->taskCount, processor.workerCount(), rate, frames, heavy);
    std::printf("clips=%d paced=%d serial=%d realtime_workers=%u caller_realtime=%u (no device workgroup) budget_ms=%.4f\n",
                clips, paced, serial, processor.realtimeWorkerCount(), callerStatus & 1u, frames * 1000. / rate);
    std::printf("round,fusion,mean_ms,p95_ms,p99_ms,p99_9_ms,max_ms,over_budget,cpu_ms_per_block,pcm_misses\n");
    const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(double(frames) / rate));
    const auto render = [&](unsigned block) {
        const auto position = clips ? (std::uint64_t(block) * frames) % (262144 / frames * frames)
                                    : std::uint64_t(block) * frames;
        if (serial) return processor.processSerial(output, frames, position, true);
        return processor.process(output, frames, position, true);
    };
    for (unsigned round = 0; round < rounds; ++round) for (unsigned mode = 0; mode < 2; ++mode) {
        const bool fusion = (mode ^ (round & 1)) != 0;
        processor.setTaskFusion(fusion);
        for (unsigned i = 0; i < 20; ++i) if (!render(i)) return 1;
        auto metrics = std::make_unique<daw::rt::BlockMetrics>(); daw::rt::TimingAccumulator samples;
        const auto* cache = PcmReadCache::existing();
        const auto missesBefore = cache ? cache->counters().misses : 0;
        const auto cpuStart = std::clock();
        for (unsigned i = 0; i < blocks; ++i) {
            const auto deadline = std::chrono::steady_clock::now() + period;
            const auto start = daw::rt::nowNanos(); if (!render(i)) return 1;
            metrics->record(daw::rt::nowNanos() - start, frames, rate);
            samples.drain(*metrics);
            // Pace outside the measured render. This exposes parked-worker wake
            // latency hidden by throughput loops; it is not a hardware xrun test.
            if (paced) std::this_thread::sleep_until(deadline);
        }
        const double cpuMs = 1000. * (std::clock() - cpuStart) / CLOCKS_PER_SEC / blocks;
        const auto misses = cache ? cache->counters().misses - missesBefore : 0;
        const auto s = samples.summary();
        std::printf("%u,%u,%.4f,%.4f,%.4f,%.4f,%.4f,%llu,%.4f,%llu\n", round, fusion, s.meanMs, s.p95Ms, s.p99Ms, s.p999Ms, s.maximumMs, (unsigned long long)s.overruns, cpuMs, (unsigned long long)misses);
        std::fflush(stdout);
    }
    if (realtimeWorkers) processor.configureAudioWorkers({});
}
