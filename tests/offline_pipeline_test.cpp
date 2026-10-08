#include "Engine/RealtimeEngine.hpp"
#include "Nodes/BasicNodes.hpp"
#include "Nodes/TapNode.hpp"
#include "Graph/OfflineGraphRenderer.hpp"
#include "EngineController.hpp"
#include "Internal/InternalFactory.hpp"
#include "platform/AudioFileDecoder.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace daw::engine;
namespace fs = std::filesystem;
namespace ap = audio::platform;

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Probe : Node {
    bool source = false, barrier = false, slow = false;
    int fault = 0, blocks = 0, services = 0;
    SamplePos next = -1;
    std::thread::id control = std::this_thread::get_id();
    std::atomic<bool> active{false};
    double memory = 0;
    std::vector<std::pair<SamplePos, unsigned>> midiSeen;

    std::string_view name() const noexcept override { return "ordered probe"; }
    bool isSource() const noexcept override { return source; }
    OfflineNodePolicy offlineNodePolicy() const noexcept override {
        return barrier ? OfflineNodePolicy::Barrier : OfflineNodePolicy::Ordered;
    }
    void reset() override { blocks = services = 0; next = -1; memory = 0; midiSeen.clear(); }
    void process(const ProcessContext& c) override {
        require(!active.exchange(true), "one instance processed concurrently");
        struct Exit { std::atomic<bool>& active; ~Exit() { active.store(false); } } exit{active};
        require(next < 0 || next == c.timelinePosition, "blocks reordered on an instance");
        require(blocks == services, "next block ran before main-thread service");
        require(c.offline, "offline flag missing");
        next = c.timelinePosition + c.frames;
        ++blocks;
        // Make the automatic crossover deterministic; this test verifies the
        // handoff after real warm-up blocks, not machine-dependent throughput.
        if (slow) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (fault == 3 && blocks == 3) throw std::runtime_error("intentional DSP exception");
        for (FrameCount f = 0; f < c.frames; ++f) {
            double input = source && c.playing ? std::sin(double(c.timelinePosition + f) * 0.019) * 0.05 : 0;
            for (std::size_t i = 0; i < c.inputs.size(); ++i)
                input += c.inputs[i].data(0)[f] * (c.inputRoles[i] == InputRole::Sidechain ? 0.17 : 0.8);
            memory = memory * 0.71 + input * 0.29;
            for (ChannelCount ch = 0; ch < c.output.numChannels(); ++ch)
                c.output.data(ch)[f] = float(memory * (ch == 0 ? 1.0 : 0.6));
        }
        if (c.midiOutput) {
            for (const auto* midi : c.midiInputs) if (midi)
                for (const auto& event : midi->events()) {
                    require(event.frameOffset < c.frames, "MIDI crossed the wrong block boundary");
                    midiSeen.emplace_back(c.timelinePosition + event.frameOffset,
                        unsigned(event.status) | (unsigned(event.data1) << 8) | (unsigned(event.data2) << 16));
                    (void)c.midiOutput->push(event);
                }
            if (source && c.playing) {
                (void)c.midiOutput->push(MidiEvent::noteOn(0, 0, 60, 100));
                (void)c.midiOutput->push(MidiEvent::noteOff(c.frames - 1, 0, 60));
            }
        }
    }
    Status serviceOffline() override {
        require(control == std::this_thread::get_id(), "main-thread work ran on a worker");
        require(!active.load(), "service raced with DSP");
        services = blocks;
        if (blocks == 3 && fault == 1) invalidatePrepare();
        if (blocks == 3 && fault == 2) return fail(EngineError::ProcessingFailed);
        return {};
    }
};

struct Fixture {
    RealtimeEngine engine;
    std::shared_ptr<Probe> a = std::make_shared<Probe>(), b = std::make_shared<Probe>(), fx = std::make_shared<Probe>();
    std::shared_ptr<TapNode> tap = std::make_shared<TapNode>();
    std::vector<float> mix, stem;

    Fixture(unsigned workers, FrameCount block, bool barrier = false, double rate = 48000)
        : engine(workers) {
        a->source = b->source = true; a->barrier = barrier;
        auto& g = engine.graph();
        const auto aId = g.adoptNode(a), bId = g.adoptNode(b);
        const auto delay = g.addNode(std::make_unique<LatencyNode>("delay", 37));
        const auto gain = g.addNode(std::make_unique<GainNode>("automated gain"));
        auto curves = std::make_shared<LevelAutomation>();
        curves->gain.active = true;
        curves->gain.points = {{0, 0.13}, {0.2, 0.9}, {0.4, 0.4}};
        // Retrieve through a retained owner, so the automation is also tested.
        auto automated = std::make_shared<GainNode>("automation");
        automated->setAutomation(curves);
        const auto automation = g.adoptNode(automated);
        const auto fxId = g.adoptNode(fx);
        const auto eq = g.addNode(std::make_unique<BiquadNode>("stateful EQ", 3));
        const auto tapId = g.adoptNode(tap);
        require(bool(g.connect(aId, delay)) && bool(g.connect(delay, gain)) &&
                bool(g.connect(gain, automation)) && bool(g.connect(automation, fxId)) &&
                bool(g.connect(bId, fxId, InputRole::Sidechain)) && bool(g.connect(fxId, eq)) &&
                bool(g.connect(fxId, tapId)), "wire fixture");
        g.setSink(eq);
        require(bool(engine.prepare(rate, block, 2)), "prepare fixture");
        tap->setCaptureDelay(11);
    }

    Status render(bool pipeline, FrameCount block, int stopAfter = 0, bool throwSink = false, bool force = true) {
        int callbacks = 0;
        return engine.renderOffline(137, 3101, block, [&](const AudioBlock& audio, FrameCount frames) {
            require(tap->capturedFrames() == frames, "stem capture advanced beyond committed block");
            for (FrameCount f = 0; f < frames; ++f) for (ChannelCount ch = 0; ch < 2; ++ch) {
                mix.push_back(audio.data(ch)[f]);
                stem.push_back(tap->captured()[ch][f]);
            }
            if (throwSink) throw std::runtime_error("intentional sink exception");
            return !stopAfter || ++callbacks < stopAfter;
        }, OfflineOptions{.sourcesEndSample = 2679, .pipeline = pipeline, .forcePipeline = force});
    }
    bool idle() const { return !a->active && !b->active && !fx->active; }
};

static void engineChecks() {
    for (const auto block : {17u, 64u, 512u}) for (const double rate : {48000., 96000.}) {
        Fixture reference(4, block, false, rate), pipeline(4, block, false, rate);
        require(bool(reference.render(false, block)) && bool(pipeline.render(true, block)), "render graph");
        require(pipeline.engine.lastOfflineUsedPipeline(), "pipeline was not actually used");
        require(reference.mix == pipeline.mix && reference.stem == pipeline.stem,
                "pipeline differs from reference: DSP/automation/sidechain/PDC/MIDI/tap/tail");
        require(!pipeline.fx->midiSeen.empty() && reference.fx->midiSeen == pipeline.fx->midiSeen,
                "delayed MIDI content/order differs from reference");
        require(pipeline.mix.size() == (3101 - 137) * 2, "partial blocks changed export length");
    }
    for (const auto workers : {1u, 4u}) {
        Fixture fallback(workers, 64, workers == 4);
        require(bool(fallback.render(true, 64)) && !fallback.engine.lastOfflineUsedPipeline(), "compatibility fallback");
    }
    {
        Fixture reference(4, 64), automatic(4, 64);
        reference.a->slow = reference.b->slow = reference.fx->slow = true;
        automatic.a->slow = automatic.b->slow = automatic.fx->slow = true;
        require(bool(reference.render(false, 64)) && bool(automatic.render(true, 64, 0, false, false)), "automatic handoff");
        require(automatic.engine.lastOfflineUsedPipeline(), "expensive graph did not select pipeline");
        require(reference.mix == automatic.mix && reference.stem == automatic.stem &&
                reference.fx->midiSeen == automatic.fx->midiSeen, "warm-up handoff lost DSP or delayed MIDI state");
    }
    {
        Fixture f(4, 64);
        const auto graph = f.engine.compiledGraph();
        std::vector<double> cheap(graph->nodes.size(), 0.0);
        const auto chosen = renderOfflinePipelined(*graph, 4, 0, 1024, 64, 1024,
            f.engine.transport(), [](const auto&, auto) { throw std::runtime_error("cheap graph was rendered"); return true; },
            [](auto, auto) { return Status{}; }, cheap);
        require(!chosen && f.a->blocks == 0, "cost fallback touched DSP");
    }
    {
        RealtimeEngine shared(4);
        auto source = std::make_shared<Probe>(); source->source = true;
        const auto first = shared.graph().adoptNode(source), second = shared.graph().adoptNode(source);
        const auto sum = shared.graph().addNode(std::make_unique<SumNode>());
        require(bool(shared.graph().connect(first, sum)) && bool(shared.graph().connect(second, sum)), "wire shared instance");
        shared.graph().setSink(sum);
        require(bool(shared.prepare(48000, 64)), "prepare shared instance");
        require(offlinePipelineWindow(*shared.compiledGraph(), 4) == 0, "shared instance was allowed concurrent timelines");
    }
    {
        RealtimeEngine large(4);
        auto source = std::make_shared<Probe>(); source->source = true;
        auto head = large.graph().adoptNode(source);
        for (int i = 0; i < 80; ++i) {
            const auto next = large.graph().addNode(std::make_unique<GainNode>());
            require(bool(large.graph().connect(head, next)), "wire bounded workspace"); head = next;
        }
        large.graph().setSink(head);
        require(bool(large.prepare(48000, 8192, 32)), "prepare bounded workspace");
        require(offlinePipelineWindow(*large.compiledGraph(), 4) == 0, "oversized PCM workspace was allowed");
    }
    for (int fault = 1; fault <= 3; ++fault) {
        Fixture f(4, 64); f.fx->fault = fault;
        try {
            const auto status = f.render(true, 64);
            require(fault != 3 && !status, "DSP failure was not propagated");
            require(status.error() == (fault == 1 ? EngineError::RenderRestartRequired : EngineError::ProcessingFailed),
                    "wrong offline failure");
        } catch (const std::runtime_error& e) {
            require(fault == 3 && std::string(e.what()) == "intentional DSP exception", "unexpected exception");
        }
        require(f.idle(), "workers survived error return");
    }
    for (const bool throwSink : {false, true}) {
        Fixture f(4, 64);
        try { require(bool(f.render(true, 64, 3, throwSink)), "cancel render"); }
        catch (const std::runtime_error& e) { require(throwSink && std::string(e.what()) == "intentional sink exception", "sink exception lost"); }
        require(f.idle(), "workers survived cancelled/throwing sink");
        require(f.a->blocks <= (throwSink ? 1 : 3) + 8, "look-ahead exceeded bounded window");
    }
    std::cout << "PASS engine: exact PCM, ordered MIDI and service, sidechain, PDC, stems, tails, fallback, errors, cancellation\n";
}

static void exportChecks() {
    const auto temp = fs::temp_directory_path() / ("daw-frontier-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(temp);
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code ec; fs::remove_all(path, ec); } } cleanup{temp};
    const auto source = (temp / "source.wav").string();
    std::vector<float> samples(16000);
    for (std::size_t i = 0; i < samples.size(); ++i) samples[i] = float(0.12 * std::sin(i * 0.037));
    ap::WriteSpec format; format.encoding = ap::Encoding::Float32;
    ap::AudioFileWriter writer; const float* audio[] = {samples.data(), samples.data()};
    require(bool(writer.open(source, format, 48000, 2, samples.size())) &&
            bool(writer.write(audio, unsigned(samples.size()))) && bool(writer.close()), "write source");
    daw::EngineController c{daw::EngineController::TestRuntime{}};
    require(bool(c.initialize(48000, 64, false)), "initialize export controller");
    const auto a = c.addTrack(daw::TrackKind::Audio, "A"), b = c.addTrack(daw::TrackKind::Audio, "B");
    require(!c.importAudio(source, a, 0).empty() && !c.importAudio(source, b, 0).empty(), "import source");
    const auto builtins = daw::plugins::builtinPlugins();
    auto insert = [&](const std::string& channel, const char* uid) {
        const auto descriptor = std::find_if(builtins.begin(), builtins.end(), [&](const auto& p) { return p.uid == uid; });
        require(descriptor != builtins.end(), "find builtin");
        const auto slot = c.addInsert(channel, *descriptor); require(!slot.empty(), "add builtin"); return slot;
    };
    insert(a, "daw.equalizer");
    const auto compressor = insert(a, "daw.compressor");
    require(c.setInsertSidechainSource(a, compressor, b), "configure sidechain");
    insert(a, "daw.delay"); insert(daw::EngineController::kMasterChannelId, "daw.compressor");
    for (const auto uid : {"daw.gravity", "daw.graphit", "daw.pitch-corrector", "daw.modulation",
                           "daw.doubler", "daw.doubler-pro", "daw.chorus", "daw.flanger", "daw.phaser"})
        insert(a, uid);
    for (const auto channels : {daw::rendering::Channels::Stereo, daw::rendering::Channels::Mono}) {
        std::vector<ap::DecodedAudio> reference;
        for (const bool pipeline : {false, true}) {
            daw::rendering::Spec spec;
            spec.outputDir = temp.string(); spec.file = format; spec.sampleRate = 96000;
            spec.blockSize = 128; spec.pipeline = pipeline; spec.channels = channels;
            spec.forcePipeline = true;
            spec.range = daw::rendering::Range::Custom;
            spec.customStartSeconds = 0.039; spec.customEndSeconds = 0.2; spec.preRollSeconds = 0.025;
            spec.tail = daw::rendering::Tail::Fixed; spec.tailSeconds = 0.057;
            spec.stemChannelIds = {a, b, daw::EngineController::kMasterChannelId};
            daw::rendering::Report report;
            const auto status = c.renderProject(spec, {}, report);
            if (!status) throw std::runtime_error(status.message());
            require(report.usedPipeline == pipeline, "controller did not select expected renderer");
            require(report.files.size() == 4, "missing export files");
            for (std::size_t i = 0; i < report.files.size(); ++i) {
                ap::DecodedAudio decoded; require(bool(ap::decodeAudioFile(report.files[i], decoded)), "decode export");
                if (!pipeline) reference.push_back(std::move(decoded));
                else require(reference[i].interleaved == decoded.interleaved, "export PCM differs from reference");
            }
        }
    }
    std::cout << "PASS export: exact Float32 mix and stems, built-in effects, sidechain, 96 kHz, preroll, mono/stereo\n";
}

int main() try {
    engineChecks(); exportChecks(); return 0;
} catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
