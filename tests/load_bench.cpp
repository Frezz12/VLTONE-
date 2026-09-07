// How much of a block period the engine actually costs, on this machine, with
// a session the size of a real one.
//
//   load_bench [--tracks N] [--plugins M] [--seconds S] [--blocks 32,64,128,…]
//              [--name <substring>] [--rate 48000] [--profile events.csv]
//
// Not a ctest target: it needs the plugins that exist on the machine it runs
// on, and a pass/fail threshold for "fast enough" would be a threshold for
// *this* laptop. It prints the numbers and leaves the judgement to the reader.
//
// It opens the real audio device and plays, so what it measures is the live
// path. It drains individual callback/graph timing events on this control
// thread; quantiles never come from the smoothed transport meter.
#include "EngineController.hpp"
#include "Core/AudioBuffer.hpp"
#include "Recording/RecordingEngine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include "Memory/PcmReadCache.hpp"
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

void writeTone(const std::string& path, double rate, std::uint32_t frames) {
    audio::AudioBuffer tone(2, frames);
    for (std::uint32_t f = 0; f < frames; ++f) {
        const float s =
            0.25f * std::sin(2.0f * 3.14159265f * 220.0f * float(f) / float(rate));
        tone.getChannel(0)[f] = s;
        tone.getChannel(1)[f] = s;
    }
    audio::AudioRecorder recorder;
    recorder.initialize(rate, 2);
    recorder.writeWAVFile(path, tone, rate);
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    int trackCount = 64;
    int pluginsPerTrack = 2;
    double seconds = 4.0;
    double rate = 48000.0;
    std::string profilePath;
    std::string wanted;
    std::vector<std::uint32_t> blockSizes = {32, 64, 128, 256, 512};
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--tracks") && i + 1 < argc)
            trackCount = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--plugins") && i + 1 < argc)
            pluginsPerTrack = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc)
            seconds = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--rate") && i + 1 < argc)
            rate = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--profile") && i + 1 < argc)
            profilePath = argv[++i];
        else if (!std::strcmp(argv[i], "--name") && i + 1 < argc)
            wanted = argv[++i];
        else if (!std::strcmp(argv[i], "--blocks") && i + 1 < argc) {
            blockSizes.clear();
            for (char* tok = std::strtok(argv[++i], ","); tok;
                 tok = std::strtok(nullptr, ",")) {
                blockSizes.push_back(std::uint32_t(std::atoi(tok)));
            }
        }
    }

    if (trackCount < 1 || trackCount > 10000 || pluginsPerTrack < 0 || pluginsPerTrack > 1000 || seconds <= 0 || seconds > 86400 || !std::isfinite(seconds) ||
        !std::isfinite(rate) || rate < 8000 || rate > 384000 || (seconds + 2.0) * rate > double(UINT32_MAX) || blockSizes.empty() ||
        std::any_of(blockSizes.begin(), blockSizes.end(), [](auto n) { return n == 0 || n > 8192; })) {
        std::fprintf(stderr, "invalid load profile\n"); return 2;
    }
    std::ofstream profile, nodeMap;
    if (!profilePath.empty()) {
        profile.open(profilePath);
        nodeMap.open(profilePath + ".nodes.csv");
        if (!profile || !nodeMap) { std::fprintf(stderr, "cannot write profile\n"); return 2; }
        profile << "block,generation,position,worker,kind,node,nanoseconds\n";
        nodeMap << "generation,node,name\n";
    }
    daw::EngineController controller;
    if (auto result = controller.initialize(rate, 512, /*openDevice=*/true); !result) {
        std::fprintf(stderr, "initialize failed: %s\n", result.message().c_str());
        return 1;
    }
    if (!controller.isDeviceOpen()) {
        std::fprintf(stderr, "no audio device — this benchmark measures the live path\n");
        return 1;
    }

    controller.pluginManager().load();
    auto effects = controller.pluginManager().effects();
    if (!wanted.empty()) {
        std::erase_if(effects, [&](const daw::plugins::PluginDescriptor& d) {
            return d.name.find(wanted) == std::string::npos;
        });
    }
    if (effects.empty() && pluginsPerTrack > 0) {
        std::fprintf(stderr,
                     "no scanned effects to load — run the app once so the plugin "
                     "cache exists, or pass --plugins 0\n");
        return 1;
    }

    const fs::path tone = fs::temp_directory_path() / ("daw-load-bench-" + daw::newUuid() + ".wav");
    writeTone(tone.string(), rate, std::uint32_t(rate * (seconds + 2.0)));

    std::printf("── building a session: %d tracks × %d plugins ──\n", trackCount,
                pluginsPerTrack);
    const auto buildStart = std::chrono::steady_clock::now();
    int loaded = 0;
    for (int t = 0; t < trackCount; ++t) {
        const std::string track = controller.importAudioToNewTrack(tone.string(), 0.0);
        if (track.empty()) {
            std::fprintf(stderr, "could not import the tone onto track %d\n", t);
            return 1;
        }
        for (int p = 0; p < pluginsPerTrack; ++p) {
            const auto& descriptor =
                effects[std::size_t((t * pluginsPerTrack + p) % effects.size())];
            if (!controller.addInsert(track, descriptor).empty()) ++loaded;
        }
    }
    const double buildMs = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - buildStart).count();
    std::printf("   %d plugin instances live, built in %.0f ms\n", loaded, buildMs);
    if (!effects.empty()) {
        std::printf("   using: ");
        for (std::size_t i = 0; i < std::min<std::size_t>(effects.size(), 4); ++i)
            std::printf("%s%s", i ? ", " : "", effects[i].name.c_str());
        std::printf("%s\n", effects.size() > 4 ? ", …" : "");
    }

    // Exercise the real device without summing a thousand test tones into the speakers.
    controller.setMasterVolume(0.0f);
    std::printf("rate=%.0f tracks=%d plugins=%d duration=%.1fs workers=%u\n", rate, trackCount, loaded, seconds, controller.audioWorkerCount());
    std::printf("block,path,blocks,mean_ms,p95_ms,p99_ms,p99_9_ms,max_ms,over_budget\n");
    bool failed = false;
    for (std::uint32_t block : blockSizes) {
        if (auto r = controller.setBufferSizeFrames(block); !r) {
            std::printf("%-8u  refused by the device: %s\n", block, r.message().c_str());
            failed = true;
            continue;
        }
        const std::uint32_t actual = controller.bufferSizeFrames();
        controller.seekSeconds(0.0);
        auto drainDiscard = [](daw::rt::BlockMetrics& metrics) {
            daw::rt::BlockTiming event;
            while (metrics.pop(event)) {}
        };
        drainDiscard(controller.callbackMetrics()); drainDiscard(controller.graphMetrics());
        const auto callbackBefore = controller.callbackMetrics().counters();
        const auto graphBefore = controller.graphMetrics().counters();
        const auto xrunsBefore = controller.audioXruns();
        const auto gatesBefore = controller.gatedAudioBlocks();
        auto* cache = daw::engine::PcmReadCache::existing();
        const auto cacheBefore = cache ? cache->counters() : daw::engine::PcmReadCache::Counters{};
        const auto profileBefore = controller.droppedAudioProfileEvents();
        controller.setAudioProfiling(profile.is_open());
        controller.play();
        daw::rt::TimingAccumulator callback, graph;
        std::uint64_t mappedGeneration = 0;
        auto drain = [&] {
            if (profile.is_open()) {
                const auto compiled = controller.routingGraph();
                if (compiled && compiled->generation != mappedGeneration) {
                    mappedGeneration = compiled->generation;
                    for (const auto& node : compiled->nodes) {
                        nodeMap << mappedGeneration << ',' << node.id << ",\"";
                        for (char c : node.node->name()) {
                            if (c == '\"') nodeMap << '\"';
                            nodeMap << c;
                        }
                        nodeMap << "\"\n";
                    }
                }
            }
            callback.drain(controller.callbackMetrics()); graph.drain(controller.graphMetrics());
            if (profile.is_open()) for (unsigned worker = 0; worker < controller.audioWorkerCount(); ++worker) {
                daw::rt::ProfileEvent event;
                for (unsigned n = 0; n < 8192 && controller.popAudioProfile(worker, event); ++n)
                    profile << actual << ',' << event.generation << ',' << event.position << ','
                        << event.worker << ',' << (event.kind == daw::rt::ProfileEvent::Kind::Node ? "node" : "wait")
                        << ',' << event.node << ',' << event.elapsedNs << '\n';
            }
        };
        const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
        while (std::chrono::steady_clock::now() < until) {
            drain();
            controller.pumpPluginEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        drain();
        const auto callbackAfter = controller.callbackMetrics().counters();
        const auto graphAfter = controller.graphMetrics().counters();
        const auto xrunsAfter = controller.audioXruns();
        const auto gated = controller.gatedAudioBlocks() - gatesBefore;
        const auto cacheAfter = cache ? cache->counters() : daw::engine::PcmReadCache::Counters{};
        controller.setAudioProfiling(false);
        controller.stop();
        const auto print = [&](const char* path, const daw::rt::TimingSummary& stats, std::uint64_t overruns) {
            std::printf("%u,%s,%zu,%.4f,%.4f,%.4f,%.4f,%.4f,%llu\n", actual, path,
                stats.count, stats.meanMs, stats.p95Ms, stats.p99Ms, stats.p999Ms, stats.maximumMs,
                (unsigned long long)overruns);
        };
        const auto stats = callback.summary();
        print("callback", stats, callbackAfter.overruns - callbackBefore.overruns);
        print("graph", graph.summary(), graphAfter.overruns - graphBefore.overruns);
        std::uint64_t xruns = 0;
        for (std::size_t i = 0; i < 4; ++i) xruns += xrunsAfter[i] - xrunsBefore[i];
        const auto dropped = callbackAfter.dropped - callbackBefore.dropped + graphAfter.dropped - graphBefore.dropped;
        const auto misses = cacheAfter.misses - cacheBefore.misses;
        const bool valid = !dropped && stats.count > 0 && loaded == trackCount * pluginsPerTrack;
        const bool stable = valid && !xruns && !gated && !misses &&
            callbackAfter.overruns == callbackBefore.overruns && stats.p999Load < .8;
        failed |= !stable;
        std::printf("diagnostics block=%u xruns_in_under=%llu in_over=%llu out_under=%llu out_over=%llu gates=%llu pcm_misses=%llu pcm_dropped=%llu pcm_locked_bytes=%zu telemetry_dropped=%llu profile_dropped=%llu rt_workers=%u workgroup_workers=%u verdict=%s\n",
            actual, (unsigned long long)(xrunsAfter[0]-xrunsBefore[0]), (unsigned long long)(xrunsAfter[1]-xrunsBefore[1]),
            (unsigned long long)(xrunsAfter[2]-xrunsBefore[2]), (unsigned long long)(xrunsAfter[3]-xrunsBefore[3]),
            (unsigned long long)gated, (unsigned long long)misses,
            (unsigned long long)(cacheAfter.droppedRequests-cacheBefore.droppedRequests), cacheAfter.lockedBytes, (unsigned long long)dropped,
            (unsigned long long)(controller.droppedAudioProfileEvents()-profileBefore),
            controller.realtimeAudioWorkerCount(), controller.workgroupAudioWorkerCount(),
            !valid ? "INCOMPLETE" : stable ? (seconds >= 600 ? "PASS_10MIN" : "SHORT_PASS") : "UNSTABLE");
    }
    std::printf("Acceptance: >=600 seconds, zero device xruns/gated blocks/PCM misses, callback p99.9 <80%% of its own block budget. Missing telemetry invalidates the result. Profiling adds overhead.\n");

    controller.shutdown();
    std::error_code ec;
    fs::remove(tone, ec);
    return failed ? 1 : 0;
}
