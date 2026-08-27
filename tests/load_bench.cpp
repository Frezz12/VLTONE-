// How much of a block period the engine actually costs, on this machine, with
// a session the size of a real one.
//
//   load_bench [--tracks N] [--plugins M] [--seconds S] [--blocks 32,64,128,…]
//              [--name <substring>]
//
// Not a ctest target: it needs the plugins that exist on the machine it runs
// on, and a pass/fail threshold for "fast enough" would be a threshold for
// *this* laptop. It prints the numbers and leaves the judgement to the reader.
//
// It opens the real audio device and plays, so what it measures is the live
// path — the same callback, the same graph, the same DSP meter the transport
// bar shows — rather than an offline render that is allowed to take as long as
// it likes.
#include "EngineController.hpp"
#include "Core/AudioBuffer.hpp"
#include "Recording/RecordingEngine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
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
    int trackCount = 16;
    int pluginsPerTrack = 2;
    double seconds = 4.0;
    std::string wanted;
    std::vector<std::uint32_t> blockSizes = {32, 64, 128, 256, 512};
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--tracks") && i + 1 < argc)
            trackCount = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--plugins") && i + 1 < argc)
            pluginsPerTrack = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc)
            seconds = std::atof(argv[++i]);
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

    const double rate = 48000.0;
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

    const fs::path tone = fs::temp_directory_path() / "daw-load-bench-tone.wav";
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

    std::printf("\n%-8s %9s %9s %9s %9s\n", "block", "period", "mean", "peak", "verdict");
    for (std::uint32_t block : blockSizes) {
        if (auto r = controller.setBufferSizeFrames(block); !r) {
            std::printf("%-8u  refused by the device: %s\n", block, r.message().c_str());
            continue;
        }
        const std::uint32_t actual = controller.bufferSizeFrames();
        controller.seekSeconds(0.0);
        controller.play();

        // The meter is an exponential average, so it needs a moment to catch
        // up before the first reading means anything.
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        double sum = 0.0, peak = 0.0;
        int samples = 0;
        const auto until = std::chrono::steady_clock::now() +
                           std::chrono::milliseconds(int(seconds * 1000));
        while (std::chrono::steady_clock::now() < until) {
            const double load = controller.dspLoad();
            sum += load;
            peak = std::max(peak, load);
            ++samples;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        controller.stop();

        const double mean = samples ? sum / samples : 0.0;
        const double periodMs = 1000.0 * double(actual) / rate;
        std::printf("%-8u %8.2fms %8.0f%% %8.0f%%   %s\n", actual, periodMs,
                    mean * 100.0, peak * 100.0,
                    peak < 0.7 ? "ok" : peak < 1.0 ? "TIGHT" : "OVERRUN");
    }
    std::printf("\n\"period\" is how long one block lasts in real time, and the two\n"
                "percentages are how much of it the graph used. Past 100%% the device\n"
                "callback misses its deadline, which is a click.\n");

    controller.shutdown();
    std::error_code ec;
    fs::remove(tone, ec);
    return 0;
}
