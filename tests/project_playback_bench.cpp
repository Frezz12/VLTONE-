// Real project, production device callback, no hardware or project writes.
// project_playback_bench manifest block seconds rounds workers paced [profile.csv]
// Profiling is a separate diagnostic pass after the timed measurements.
#include "EngineController.hpp"
#include "ProjectSerializer.hpp"
#include "Core/AudioBuffer.hpp"
#include "Job/AudioWorkerRegistration.hpp"
#include "Memory/PcmReadCache.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
using Clock = std::chrono::steady_clock;
void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}
void pump(daw::EngineController& c) {
#ifdef _WIN32
    MSG message;
    for (unsigned n = 0; n < 256 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++n) {
        if (message.message == WM_QUIT) { PostQuitMessage(int(message.wParam)); break; }
        TranslateMessage(&message); DispatchMessageW(&message);
    }
#endif
    c.pumpPluginEvents();
}
double cpuMs() {
#ifdef _WIN32
    FILETIME created{}, ended{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &ended, &kernel, &user)) return -1;
    const auto ticks = [](FILETIME t) { return (std::uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    return double(ticks(kernel) + ticks(user)) / 10000.;
#else
    return 1000. * std::clock() / CLOCKS_PER_SEC;
#endif
}
}
int main(int argc, char** argv) try {
    require(argc >= 7, "manifest block seconds rounds workers paced [profile.csv]");
    std::cout << std::unitbuf;
    const unsigned frames = std::stoul(argv[2]), rounds = std::stoul(argv[4]), workers = std::stoul(argv[5]);
    const double seconds = std::stod(argv[3]);
    const bool paced = std::stoi(argv[6]) != 0;
    require(frames >= 8 && frames <= 8192 && rounds > 0 && rounds <= 20 &&
            std::isfinite(seconds) && seconds > 0 && seconds <= 30 && workers <= 128, "invalid arguments");
    auto package = std::filesystem::path(argv[1]);
    if (std::filesystem::is_regular_file(package)) package = package.parent_path();
    daw::ProjectModel project;
    require(bool(daw::ProjectSerializer::load(project, package.string())), "load project");
    daw::EngineController c;
    require(bool(c.initialize(project.sampleRate, frames, false)), "initialize");
    c.pluginManager().load();
    require(bool(c.openProject(package.string())), "open project");
    require(c.bufferSizeFrames() == frames, "project changed the requested buffer size");
    const auto rate = project.sampleRate;
    daw::engine::AudioWorkerRegistration caller;
    const auto callerStatus = caller.configure({true, rate, frames, {}});
    c.configureAudioWorkersForTest(true, workers);
    audio::AudioBuffer input(2, frames), output(2, frames); input.clear();
    const auto count = unsigned(std::ceil(seconds * rate / frames));
    const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(frames / rate));
    const auto serviceEvery = std::max(1u, unsigned(rate * .015 / frames));
    const auto block = [&](unsigned i, daw::rt::TimingAccumulator* timings = nullptr) {
        const auto until = Clock::now() + period;
        const auto begin = daw::rt::nowNanos();
        require(c.processDeviceBlockForTest(input, output, frames), "callback refused");
        const auto duration = daw::rt::nowNanos() - begin;
        if (timings) timings->add({duration, std::uint64_t(double(frames) * 1e9 / rate), frames, 0});
        if (i % serviceEvery == 0) pump(c);
        if (paced) std::this_thread::sleep_until(until);
    };
    const auto warm = [&] {
        c.stop(); c.seekSeconds(23); pump(c); c.play();
        for (unsigned i = 0; i < unsigned(std::ceil(2 * rate / frames)); ++i) block(i);
        (void)c.callbackMetrics(true); (void)c.graphMetrics(true);
    };
    std::cout << "PROJECT tracks=" << c.project().tracks.size() << " nodes=" << c.routingGraph()->nodes.size()
        << " workers=" << c.audioWorkerCount() << " rt_workers=" << c.realtimeAudioWorkerCount()
        << " caller_rt=" << (callerStatus & 1) << " rate=" << rate << " block=" << frames
        << " paced=" << paced << " budget_ms=" << frames * 1000. / rate << '\n';
    for (unsigned round = 0; round < rounds; ++round) {
        warm();
        const auto failed = c.failedAudioBlocks(), gated = c.gatedAudioBlocks();
        const auto* cache = daw::engine::PcmReadCache::existing();
        const auto misses = cache ? cache->counters().misses : 0;
        daw::rt::TimingAccumulator callback, graph;
        double square = 0; std::uint64_t nonfinite = 0;
        const auto cpu = cpuMs();
        const auto start = Clock::now();
        for (unsigned i = 0; i < count; ++i) {
            block(i, &callback);
            if (i % 16 == 0 || i + 1 == count) {
                for (const auto& e : c.graphMetrics(true).events) graph.add(e);
            }
            for (unsigned ch = 0; ch < 2; ++ch) for (unsigned n = 0; n < frames; ++n) {
                const double v = output.getChannel(ch)[n];
                nonfinite += !std::isfinite(v); square += v * v;
            }
        }
        const auto cpuCost = (cpuMs() - cpu) / count;
        const auto elapsed = std::chrono::duration<double>(Clock::now() - start).count();
        const auto cb = callback.summary(), gr = graph.summary();
        const auto missing = cache ? cache->counters().misses - misses : 0;
        const double rms = std::sqrt(square / (double(count) * frames * 2));
        std::cout << "RESULT round=" << round << " block=" << frames << " blocks=" << cb.count
            << " callback_ms=" << cb.meanMs << " graph_ms=" << gr.meanMs << " p95_ms=" << cb.p95Ms
            << " p99_ms=" << cb.p99Ms << " max_ms=" << cb.maximumMs << " over_budget=" << cb.overruns
            << " cpu_ms_per_block=" << cpuCost << " elapsed_s=" << elapsed << " rms=" << rms
            << " pcm_misses=" << missing << " failed=" << c.failedAudioBlocks()-failed
            << " gated=" << c.gatedAudioBlocks()-gated << " nonfinite=" << nonfinite << '\n';
        require(cb.count == count && !nonfinite && c.failedAudioBlocks() == failed &&
                c.gatedAudioBlocks() == gated && rms > .01, "invalid measurement/audio");
    }
    if (argc > 7) {
        warm(); c.setAudioProfiling(true);
        struct Cost { std::uint64_t ns = 0, count = 0; };
        std::map<std::pair<unsigned, unsigned>, Cost> costs;
        const auto dropped = c.droppedAudioProfileEvents();
        const auto measured = std::min(count, unsigned(std::ceil(2 * rate / frames)));
        for (unsigned i = 0; i < measured; ++i) {
            block(i);
            for (unsigned worker = 0; worker < c.audioWorkerCount(); ++worker) {
                daw::rt::ProfileEvent event;
                while (c.popAudioProfile(worker, event)) {
                    auto& cost = costs[{event.kind == daw::rt::ProfileEvent::Kind::Node ? event.node : ~0u, worker}];
                    cost.ns += event.elapsedNs; ++cost.count;
                }
            }
        }
        c.setAudioProfiling(false);
        std::ofstream out(argv[7]); out << "node,worker,name,calls,total_ms,ms_per_block\n";
        for (const auto& [key, cost] : costs) {
            std::string name = "wait";
            for (const auto& node : c.routingGraph()->nodes) if (node.id == key.first) name = node.node->name();
            out << key.first << ',' << key.second << ",\"" << name << "\"," << cost.count << ','
                << double(cost.ns)/1e6 << ',' << double(cost.ns)/1e6/measured << '\n';
        }
        std::cout << "PROFILE dropped=" << c.droppedAudioProfileEvents()-dropped << '\n';
    }
    c.stop(); c.configureAudioWorkersForTest(false); c.shutdown();
    return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
