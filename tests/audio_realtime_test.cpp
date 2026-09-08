#include "Graph/AudioGraph.hpp"
#include "Graph/GraphProcessor.hpp"
#include "Engine/RealtimeEngine.hpp"
#include "Nodes/BasicNodes.hpp"
#include "Nodes/PlaybackNodes.hpp"
#include "Nodes/MeterNode.hpp"
#include "Job/BackgroundExecutor.hpp"
#include "Memory/PcmReadCache.hpp"
#include "ScopedNoDenormals.hpp"
#include "RealtimeMetrics.hpp"
#include "Device/AudioDeviceManager.hpp"
#include <atomic>
#include <bit>
#include <cstdio>
#include <future>
#include <limits>
#include <thread>
using namespace daw::engine;
using namespace std::chrono_literals;
static int failures = 0;
static void check(bool ok, const char* text) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", text); failures += !ok; }
struct Output {
    std::vector<float> l, r; float* pointers[2];
    explicit Output(unsigned frames) : l(frames), r(frames), pointers{l.data(), r.data()} {}
    AudioBlock block() { return {pointers, 2, FrameCount(l.size())}; }
};
struct Source : Node {
    std::string_view name() const noexcept override { return "test source"; }
    MidiNodeRole midiRole() const noexcept override { return MidiNodeRole::None; }
    void process(const ProcessContext& c) override {
        for (auto ch = 0u; ch < c.output.numChannels(); ++ch)
            for (auto f = 0u; f < c.frames; ++f) c.output.data(ch)[f] = float(f + ch) / 8192.f;
    }
};
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    {
        daw::rt::DiagnosticRing<daw::rt::BlockTiming, 4> ring;
        check(ring.push({1}) && ring.push({2}) && ring.push({3}) && !ring.push({4}), "bounded telemetry drops without overwriting");
        daw::rt::BlockTiming event;
        bool ordered = true;
        for (unsigned i = 1; i < 4; ++i) ordered &= ring.pop(event) && event.elapsedNs == i;
        check(ordered && !ring.pop(event) && ring.dropped() == 1, "telemetry preserves FIFO and dropped count");
        auto metrics = std::make_unique<daw::rt::BlockMetrics>();
        for (unsigned i = 1; i <= 1000; ++i) metrics->record(i * 1000, 1, 2000);
        daw::rt::TimingAccumulator samples; samples.drain(*metrics);
        auto summary = samples.summary();
        check(summary.count == 1000 && summary.p95Ms == .95 && summary.p99Ms == .99 &&
            summary.p999Ms == .999 && summary.maximumMs == 1.0 && summary.overruns == 500,
            "per-block quantiles and budget overruns are exact");
        for (unsigned i = 0; i < 9000; ++i) metrics->record(2000000, 1, 2000);
        check(metrics->counters().maximumNs == 2000000 && metrics->counters().overruns == 9500 &&
            metrics->counters().dropped > 0, "lifetime peaks and overruns survive telemetry overflow");
    }
    {
        auto device = std::make_unique<audio::AudioDeviceManager>();
        device->processStream(nullptr, nullptr, 256, 0x0f); // PA's four xrun bits.
        const auto counts = device->xruns();
        check(counts.inputUnderflow == 1 && counts.inputOverflow == 1 && counts.outputUnderflow == 1 && counts.outputOverflow == 1 && device->callbackMetrics().counters().blocks == 1,
            "PortAudio xrun flags and complete callback are counted even on early return");
    }
    {
        std::array<float, 19> values{};
        bool correct = true;
        const daw::rt::ScopedNoDenormals outer;
        { const daw::rt::ScopedNoDenormals nested; check(dsp::isSilent(values), "exact zero remains silent with FTZ"); }
        for (std::uint32_t bits : {1u, 0x80000001u, 0x7fc00000u, 0x7f800000u, 0x3f800000u})
            for (unsigned i = 0; i < values.size(); ++i) {
                values.fill(0); values[i] = std::bit_cast<float>(bits);
                correct &= !dsp::isSilent(values);
            }
        values.fill(-0.f);
        check(correct && dsp::isSilent(values), "FTZ silence contract preserves subnormals, NaN, infinity and signed zero");
    }
    {
        auto ownedCache = std::make_unique<PcmReadCache>(8 * PcmReadCache::kPageSamples * sizeof(float));
        auto& cache = *ownedCache;
        std::vector<float> original(32 * PcmReadCache::kPageSamples, .375f);
        const auto source = cache.addSource(original.data(), original.size());
        cache.warm(source, 0, PcmReadCache::kPageSamples);
        { PcmReadScope scope(true); check(scope.view(cache, source, 12, 7).front() == .375f, "warm PCM reads pinned RAM"); }
        { PcmReadScope scope(true); check(scope.view(cache, source, 24 * PcmReadCache::kPageSamples, 1).front() == 0, "cold PCM returns silence without reading its mapping"); }
        bool ready = false;
        for (int i = 0; i < 200 && !ready; ++i) {
            std::this_thread::sleep_for(1ms);
            PcmReadScope scope(true); ready = scope.view(cache, source, 24 * PcmReadCache::kPageSamples, 1).front() == .375f;
        }
        check(ready && cache.counters().misses > 0, "background read resolves a measured PCM miss");
        const auto budget = cache.counters().capacityBytes;
        for (unsigned page = 0; page < 32; ++page) cache.warm(source, page * PcmReadCache::kPageSamples, PcmReadCache::kPageSamples);
        check(cache.counters().capacityBytes == budget, "PCM cache capacity stays fixed under eviction");
        cache.warm(source, 0, 4 * PcmReadCache::kPageSamples);
        const auto missesBefore = cache.counters().misses;
        std::atomic<bool> allReady{true}; std::vector<std::thread> readers;
        for (unsigned n = 0; n < 8; ++n) readers.emplace_back([&] {
            for (unsigned i = 0; i < 2000; ++i) {
                PcmReadScope scope(true);
                if (scope.view(cache, source, 0, 16).front() != .375f) allReady = false;
            }
        });
        for (auto& reader : readers) reader.join();
        check(allReady && cache.counters().misses == missesBefore, "concurrent PCM readers do not manufacture cache misses");
        cache.invalidateRequests(); cache.removeSource(source);
        const auto other = cache.addSource(original.data(), original.size());
        check(other != source, "retired PCM requests cannot alias a new mapping");
        cache.removeSource(other);
    }
    {
        auto& pool = BackgroundExecutor::instance();
        BackgroundPlaybackLease playing; playing.setPlaying(true);
        std::atomic<int> active{0}, peak{0}, done{0};
        std::vector<BackgroundExecutor::Handle> tasks;
        for (int i = 0; i < 8; ++i) tasks.push_back(pool.submit([&] {
            const int count = ++active; int seen = peak.load();
            while (seen < count && !peak.compare_exchange_weak(seen, count)) {}
            std::this_thread::sleep_for(2ms); --active; ++done;
        }));
        for (int n = 0; n < 2000 && done != 8; ++n) std::this_thread::sleep_for(1ms);
        for (const auto& task : tasks) pool.cancelAndWait(task);
        check(done == 8 && peak == 1, "sampler admission limits playback to one preparation");
        playing.setPlaying(false);
        std::promise<void> release; auto wait = release.get_future().share();
        std::atomic<unsigned> started{0};
        auto a = pool.submit([&] { ++started; wait.wait(); });
        auto b = pool.submit([&] { ++started; wait.wait(); });
        for (int n = 0; n < 1000 && started != 2; ++n) std::this_thread::sleep_for(1ms);
        auto cancelled = pool.submit([&] { ++started; }); pool.cancelAndWait(cancelled);
        check(started == 2, "stopped transport admits two workers and cancels queued destruction");
        release.set_value(); pool.cancelAndWait(a); pool.cancelAndWait(b);
    }
    for (double rate : {48000., 96000.}) for (unsigned frames : {8u, 16u, 32u, 256u, 512u}) {
        AudioGraph graph; const auto sink = graph.addNode(std::make_unique<SumNode>()); graph.setSink(sink);
        for (int i = 0; i < 64; ++i) {
            auto previous = graph.addNode(std::make_unique<Source>());
            for (int j = 0; j < 4; ++j) {
                auto node = graph.addNode(std::make_unique<GainNode>());
                graph.connect(previous, node); previous = node;
            }
            const auto meter = graph.addNode(std::make_unique<MeterNode>());
            graph.connect(previous, meter); graph.connect(meter, sink);
        }
        const auto compiled = graph.compile({rate, frames, 2});
        if (!compiled) { check(false, "fusion graph compiles"); continue; }
        GraphProcessor processor(4); processor.setGraph(*compiled);
        Output fused(frames), plain(frames), serial(frames);
        processor.process(fused.block(), frames, 0, true);
        processor.setTaskFusion(false); processor.process(plain.block(), frames, 0, true);
        processor.processSerial(serial.block(), frames, 0, true);
        check((*compiled)->taskCount < (*compiled)->nodes.size() && fused.l == plain.l && fused.r == plain.r && plain.l == serial.l,
              "fused, unfused and serial graph outputs agree at 48/96k and small/large buffers");
        processor.configureAudioWorkers({true, rate, frames, {}});
        processor.configureAudioWorkers({});
    }
    {
        bool correct = true;
        for (unsigned channels : {1u, 2u}) for (unsigned frames : {8u, 16u, 32u, 256u})
            for (bool fading : {false, true}) {
                auto sample = std::make_shared<SampleBuffer>(channels, 262144, 48000);
                for (unsigned ch = 0; ch < channels; ++ch)
                    for (unsigned i = 0; i < sample->frames(); ++i)
                        sample->writableChannel(ch)[i] = float(int((i + ch * 17) % 127) - 63) / 128.f;
                auto player = std::make_shared<ClipPlayerNode>();
                auto clips = std::make_shared<ClipPlayerNode::ClipList>();
                ClipPlacement clip;
                clip.audio = sample;
                clip.startSample = 17;
                clip.lengthSamples = 64;
                clip.sourceStartFrame = 4092; // Cross a pinned page boundary.
                clip.sourceEndFrame = 4147.25; // Last integral frame remains audible.
                clip.gain = .5f;
                clip.pan = .25f;
                clip.fadeInSamples = fading ? 7 : 0;
                clip.fadeOutSamples = fading ? 11 : 0;
                clips->push_back(clip);
                player->setClips(clips);
                AudioGraph graph;
                const auto id = graph.adoptNode(player); graph.setSink(id);
                const auto compiled = graph.compile({48000, frames, 2});
                if (!compiled) { correct = false; continue; }
                GraphProcessor processor(1); processor.setGraph(*compiled);
                player->preparePlayback(0);
                Output realtime(frames), offline(frames);
                for (SamplePos position = 0; position < 100; position += frames) {
                    correct &= bool(processor.process(realtime.block(), frames, position, true));
                    correct &= bool(processor.processSerial(offline.block(), frames, position, true, true));
                    correct &= realtime.l == offline.l && realtime.r == offline.r;
                    for (unsigned ch = 0; ch < 2; ++ch) for (unsigned i = 0; i < frames; ++i) {
                        const auto relative = position + i - clip.startSample;
                        float expected = 0;
                        if (relative >= 0 && relative < 64 && 4092 + relative < clip.sourceEndFrame) {
                            const auto sourceCh = std::min(ch, channels - 1);
                            const float gain = ch == 0 ? .375f : .5f;
                            const float fade = fading ? float(std::clamp(std::min(
                                double(relative) / 7, double(64 - relative) / 11), 0., 1.)) : 1.f;
                            expected = sample->channel(sourceCh)[4092 + relative] * gain * fade;
                        }
                        correct &= std::abs(realtime.block().data(ch)[i] - expected) < 1e-7f;
                    }
                }
            }
        check(correct, "ordinary bounded clips preserve page crossings, trim ends, fades, pan and mono at 8/16/32/256 frames");
    }
    {
        ClipPlayerNode player;
        player.prepare({48000, 256, 2});
        auto sample = std::make_shared<SampleBuffer>(2, 256, 48000);
        for (unsigned ch = 0; ch < 2; ++ch) std::fill_n(sample->writableChannel(ch), 256, .001f);
        auto clips = std::make_shared<ClipPlayerNode::ClipList>();
        for (int i = 0; i < 100000; ++i) { ClipPlacement c; c.audio = sample; c.startSample = i * 512; c.lengthSamples = 256; clips->push_back(c); }
        for (int i = 0; i < 3000; ++i) { ClipPlacement c; c.audio = sample; c.startSample = 60000000; c.lengthSamples = 256; clips->push_back(c); }
        player.setClips(clips);
        Output output(256); ProcessContext c; c.output = output.block(); c.frames = 256; c.playing = true;
        c.timelinePosition = 99999 * 512; player.process(c);
        check(output.l.front() == .001f, "seek through 100k past clips finds only intersecting material");
        c.timelinePosition = 60000000; player.process(c);
        float expected = 0; for (int i = 0; i < 3000; ++i) expected += .001f;
        check(output.l.front() == expected, "3000 overlapping clips preserve exact mixing order without overflow fallback");
        c.timelinePosition = 256; player.process(c); check(dsp::isSilent(output.l), "interval index honours exclusive clip ends on backwards seek");
    }
    {
        auto sample = std::make_shared<SampleBuffer>(2, 262144, 48000);
        for (unsigned ch = 0; ch < 2; ++ch) for (unsigned frame = 0; frame < sample->frames(); ++frame)
            sample->writableChannel(ch)[frame] = float((frame + ch) % 127) / 256.f;
        AudioGraph graph;
        auto player = std::make_shared<ClipPlayerNode>();
        auto clips = std::make_shared<ClipPlayerNode::ClipList>();
        ClipPlacement placement; placement.audio = sample; placement.lengthSamples = sample->frames();
        clips->push_back(placement); player->setClips(clips);
        const auto id = graph.adoptNode(player); graph.setSink(id);
        auto compiled = graph.compile({48000, 256, 2});
        GraphProcessor processor(2); processor.setGraph(*compiled);
        player->preparePlayback(200000);
        Output realtime(256), offline(256);
        processor.process(realtime.block(), 256, 200000, true, false);
        processor.processSerial(offline.block(), 256, 200000, true, true);
        check(sample->fileBacked() && realtime.l == offline.l && realtime.r == offline.r,
            "mapped PCM seek is sample-identical in prepared realtime and direct offline paths");
    }
    {
        RealtimeEngine engine(2); engine.prepare(48000, 256, 2); Output output(256);
        { RealtimeEngine::RenderGate gate(engine); engine.renderBlock(output.block(), nullptr, 0, 256); }
        check(engine.gatedBlocks() == 1 && dsp::isSilent(output.l), "RenderGate silence is counted separately");
    }
    return failures ? 1 : 0;
}
