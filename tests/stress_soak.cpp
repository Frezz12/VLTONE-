// Everything a session does to the engine while it is rendering, in a loop, on
// one thread, with the audio device open and real plugins loaded.
//
//   stress_soak [--seconds S] [--seed N] [--tracks N] [--name <substring>]
//               [--quiet]
//
// Not a ctest target: it needs the plugins on the machine it runs on, and it is
// meant to be run for minutes under a heap checker rather than for a second in
// CI. What it is for is making an intermittent heap corruption reproducible —
// the edits it makes are the ones that reallocate buffers the audio thread is
// reading, which is where an overrun that only shows up an hour later comes
// from. Run it under Guard Malloc and the overrun faults on the instruction
// that does it:
//
//   DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib MALLOC_STRICT_SIZE=1 \
//       ./build/bin/stress_soak --seconds 60
//
// The seed is printed on the way in and every operation is logged, so a crash
// is replayable: same seed, same sequence.
#include "EngineController.hpp"
#include "Core/AudioBuffer.hpp"
#include "Recording/RecordingEngine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

bool g_quiet = false;
std::uint64_t g_step = 0;

void say(const char* what, const std::string& detail = {}) {
    if (g_quiet) return;
    std::printf("%6llu  %-22s %s\n", (unsigned long long)g_step, what, detail.c_str());
}

void writeTone(const std::string& path, double rate, std::uint32_t frames) {
    audio::AudioBuffer tone(2, frames);
    for (std::uint32_t f = 0; f < frames; ++f) {
        const float s =
            0.2f * std::sin(2.0f * 3.14159265f * 196.0f * float(f) / float(rate));
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
    double seconds = 30.0;
    unsigned seed = 1;
    int trackCount = 6;
    std::string wanted;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) seed = unsigned(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--tracks") && i + 1 < argc) trackCount = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--name") && i + 1 < argc) wanted = argv[++i];
        else if (!std::strcmp(argv[i], "--quiet")) g_quiet = true;
    }
    std::printf("── soak: seed %u, %.0f s, %d tracks ──\n", seed, seconds, trackCount);

    const double rate = 48000.0;
    daw::EngineController controller;
    if (auto r = controller.initialize(rate, 256, /*openDevice=*/true); !r) {
        std::fprintf(stderr, "initialize failed: %s\n", r.message().c_str());
        return 1;
    }
    controller.pluginManager().load();
    auto effects = controller.pluginManager().effects();
    if (!wanted.empty()) {
        std::erase_if(effects, [&](const daw::plugins::PluginDescriptor& d) {
            return d.name.find(wanted) == std::string::npos;
        });
    }
    if (effects.empty()) {
        std::fprintf(stderr, "no scanned effects — run the app once so the cache exists\n");
        return 1;
    }
    std::printf("   %zu effects available, device %s\n", effects.size(),
                controller.isDeviceOpen() ? "open" : "CLOSED");

    const fs::path tone = fs::temp_directory_path() / "daw-soak-tone.wav";
    writeTone(tone.string(), rate, std::uint32_t(rate * 8));

    std::vector<std::string> tracks;
    for (int t = 0; t < trackCount; ++t) {
        std::string id = controller.importAudioToNewTrack(tone.string(), 0.0);
        if (!id.empty()) tracks.push_back(std::move(id));
    }
    for (const std::string& id : tracks) {
        controller.addInsert(id, effects[std::size_t(std::rand()) % effects.size()]);
    }
    controller.play();

    std::mt19937 rng(seed);
    auto pick = [&](std::size_t n) { return n ? rng() % n : 0; };
    const std::uint32_t blockChoices[] = {32, 64, 128, 256, 512};

    const auto until = std::chrono::steady_clock::now() +
                       std::chrono::milliseconds(std::int64_t(seconds * 1000));
    while (std::chrono::steady_clock::now() < until) {
        ++g_step;
        // Weighted towards the edits that reallocate: those are the ones the
        // audio thread can catch mid-flight.
        switch (rng() % 13) {
            case 0: {   // a plugin arrives on a channel that is being rendered
                if (tracks.empty()) break;
                const std::string& id = tracks[pick(tracks.size())];
                const auto& d = effects[pick(effects.size())];
                say("addInsert", d.name);
                controller.addInsert(id, d);
                break;
            }
            case 1: {   // and leaves again
                if (tracks.empty()) break;
                const std::string& id = tracks[pick(tracks.size())];
                const auto* slots = controller.channelInserts(id);
                if (!slots || slots->empty()) break;
                const std::string slot = (*slots)[pick(slots->size())].id;
                say("removeInsert", slot);
                controller.removeInsert(id, slot);
                break;
            }
            case 2: {   // mono/stereo/dual-mono renegotiates the plugin's buses
                if (tracks.empty()) break;
                const std::string& id = tracks[pick(tracks.size())];
                const auto* slots = controller.channelInserts(id);
                if (!slots || slots->empty()) break;
                const std::string slot = (*slots)[pick(slots->size())].id;
                static const daw::PluginChannelMode modes[] = {
                    daw::PluginChannelMode::Auto, daw::PluginChannelMode::Mono,
                    daw::PluginChannelMode::Stereo, daw::PluginChannelMode::DualMono};
                const auto mode = modes[pick(4)];
                say("setInsertChannelMode", slot);
                controller.setInsertChannelMode(id, slot, mode);
                break;
            }
            case 3: {   // the track's own width, which its inserts follow
                if (tracks.empty()) break;
                const std::string& id = tracks[pick(tracks.size())];
                const bool mono = (rng() & 1) != 0;
                say("setTrackMono", id + (mono ? " mono" : " stereo"));
                controller.setTrackMono(id, mono);
                break;
            }
            case 4: {   // every buffer in the engine is resized under the renderer
                const std::uint32_t block = blockChoices[pick(5)];
                say("setBufferSizeFrames", std::to_string(block));
                controller.setBufferSizeFrames(block);
                break;
            }
            case 5: {
                if (tracks.empty()) break;
                const std::string& id = tracks[pick(tracks.size())];
                const auto* slots = controller.channelInserts(id);
                if (!slots || slots->empty()) break;
                const std::string slot = (*slots)[pick(slots->size())].id;
                say("setInsertBypassed", slot);
                controller.setInsertBypassed(id, slot, (rng() & 1) != 0);
                break;
            }
            case 6: {   // a whole channel strip appears and disappears
                say("addTrack", "");
                std::string id = controller.importAudioToNewTrack(tone.string(), 0.0);
                if (!id.empty()) {
                    controller.addInsert(id, effects[pick(effects.size())]);
                    tracks.push_back(std::move(id));
                }
                break;
            }
            case 7: {
                if (tracks.size() <= 2) break;
                const std::size_t at = pick(tracks.size());
                say("removeTrack", tracks[at]);
                controller.removeTrack(tracks[at]);
                tracks.erase(tracks.begin() + std::ptrdiff_t(at));
                break;
            }
            case 8: {
                say("seek", "");
                controller.seekSeconds(double(pick(6)));
                break;
            }
            case 9: {
                const bool playing = controller.isPlaying();
                say(playing ? "stop" : "play", "");
                if (playing) controller.stop(); else controller.play();
                break;
            }
            case 10: {   // undo/redo replays the same reallocations backwards
                if ((rng() & 1) && controller.undoDepth() > 0) {
                    say("undo", "");
                    controller.undo();
                } else {
                    say("redo", "");
                    controller.redo();
                }
                // The undo may have taken a track with it.
                std::erase_if(tracks, [&](const std::string& id) {
                    return controller.project().findTrack(id) == nullptr;
                });
                break;
            }
            case 11: {   // recording and monitoring: a second thread, a shared
                         // ring, and a take turning into a clip on stop
                if (tracks.empty()) break;
                const std::string& id = tracks[pick(tracks.size())];
                if (controller.isRecording()) {
                    say("stopRecording", "");
                    controller.stopRecording();
                } else if (rng() & 1) {
                    say("setTrackArmed", id);
                    controller.setTrackArmed(id, (rng() & 1) != 0);
                } else if (rng() & 1) {
                    say("setTrackMonitor", id);
                    controller.setTrackMonitor(id, (rng() & 1) != 0);
                } else {
                    say("startRecording", id);
                    controller.setTrackArmed(id, true);
                    controller.startRecording(id);
                }
                break;
            }
            default: break;
        }

        // The UI tick: this is where plugin notifications are drained and a
        // restart request turns into a graph rebuild.
        controller.pumpPluginEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(12));
    }

    if (controller.isRecording()) controller.stopRecording();
    controller.stop();

    // Saving walks every live plugin for its state and writes the document —
    // a different traversal of the same objects the loop was churning.
    const fs::path package = fs::temp_directory_path() / "daw-soak-project";
    std::error_code packageEc;
    fs::remove_all(package, packageEc);
    if (auto r = controller.saveProject(package.string()); !r) {
        std::printf("   saveProject: %s\n", r.message().c_str());
    } else {
        say("saveProject", package.string());
    }
    fs::remove_all(package, packageEc);

    std::printf("\n── survived %llu operations ──\n", (unsigned long long)g_step);
    controller.shutdown();
    std::error_code ec;
    fs::remove(tone, ec);
    return 0;
}
