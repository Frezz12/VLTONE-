// Manual hardware smoke test — NOT part of ctest, since it opens a real audio
// device. Run ./bin/device_smoke [output UID] [buffer] [alternate output UID].
// The graph is silent; all control and telemetry use the production process.
// DAW_DEVICE_SMOKE_RATE selects the rate (defaults to 48000 Hz).
#include "EngineController.hpp"
#include "Internal/EqualizerInstance.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace {
template<class F> bool eventually(F&& ready) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    do {
        if (ready()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < end);
    return false;
}

bool playbackHealthy(daw::EngineController& controller, const char* phase) {
    const auto before = controller.positionSeconds();
    auto last = before;
    const auto presentationBefore = controller.presentationPositionSeconds();
    auto presentationLast = presentationBefore;
    unsigned updates = 0, presentationUpdates = 0, recoveries = 0;
    // Exercise the same sustained control/readout traffic as the UI timer.
    for (unsigned i = 0; i < 80; ++i) {
        controller.pumpPreviewPluginEvents();
        (void)controller.masterLoudness(); // silence produces a valid -infinity LUFS readout
        const auto position = controller.positionSeconds();
        updates += position > last;
        last = position;
        const auto presentation = controller.presentationPositionSeconds();
        presentationUpdates += presentation > presentationLast;
        presentationLast = presentation;
        recoveries += controller.audioDeviceNeedsRecovery();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    const bool ok = controller.isDeviceOpen() && controller.isPlaying() &&
        last - before > .5 && updates > 10 && !recoveries &&
        presentationLast - presentationBefore > .5 && presentationUpdates > 50;
    std::printf("%s %s: advanced=%.3f s blockUpdates=%u presentationUpdates=%u recoveryRequests=%u\n",
        ok ? "PASS" : "FAIL", phase, last - before, updates, presentationUpdates, recoveries);
    std::fflush(stdout);
    return ok;
}
}

int main(int argc, char** argv) try {
    daw::EngineController controller;
    if (argc > 1 && std::string(argv[1]) == "--list") {
        if (const auto ready = controller.initialize(48000, 512, false); !ready) {
            std::printf("FAILED to start audio process: %s\n", ready.message().c_str());
            return 1;
        }
        for (const auto& device : controller.enumerateOutputDevices()) {
            std::printf("%s | %s | %u output / %u input\n",
                        device.uid.c_str(), device.hostApi.c_str(),
                        device.outputChannels, device.inputChannels);
        }
        return 0;
    }

    audio::AudioDeviceConfig config;
    const auto* requestedRate = std::getenv("DAW_DEVICE_SMOKE_RATE");
    config.sampleRate = requestedRate ? std::strtod(requestedRate, nullptr) : 48000;
    config.inputEnabled = false;
    config.bufferSize = argc > 2
        ? static_cast<audio::BufferSize>(std::strtoul(argv[2], nullptr, 10))
        : 512;
    if (argc > 1) {
        config.outputDeviceUid = argv[1];
    }
    auto r = controller.initialize(config, /*openDevice=*/true);
    if (!r) {
        std::printf("FAILED to open the audio device: %s\n", r.message().c_str());
        return 1;
    }
    std::printf("device open: %s, %.0f Hz, %u frames, %u workers, out=%s\n",
                controller.isDeviceOpen() ? "yes" : "no", controller.sampleRate(),
                controller.bufferSizeFrames(), controller.workerCount(),
                controller.currentOutputDeviceUid().c_str());

    const std::string track = controller.addTrack(daw::TrackKind::Audio, "Smoke");
    const auto effect = controller.addInsert(track,
        daw::plugins::equalizer::EqualizerInstance::staticDescriptor());
    if (track.empty() || effect.empty()) {
        std::printf("FAILED to publish track and built-in plugin\n");
        return 2;
    }
    controller.setInsertParameter(track, effect, "output.gain", -6);
    if (!eventually([&] { return std::abs(controller.insertParameter(track, effect, "output.gain") + 6) < 1e-6; })) {
        std::printf("FAILED to acknowledge plugin parameter\n");
        return 2;
    }
    controller.play();
    if (!playbackHealthy(controller, "initial device")) return 3;

    const auto original = controller.audioConfiguration();
    auto changed = original;
    changed.bufferSize = original.bufferSize == 512 ? 256 : 512;
    if (const auto switched = controller.applyAudioConfiguration(changed); !switched) {
        std::printf("FAILED to change buffer: %s\n", switched.message().c_str());
        return 4;
    }
    if (controller.bufferSizeFrames() != changed.bufferSize ||
        !playbackHealthy(controller, "changed buffer") ||
        !eventually([&] { return std::abs(controller.insertParameter(track, effect, "output.gain") + 6) < 1e-6; })) return 4;
    if (const auto restored = controller.applyAudioConfiguration(original); !restored) {
        std::printf("FAILED to restore buffer: %s\n", restored.message().c_str());
        return 5;
    }
    if (!playbackHealthy(controller, "restored buffer")) return 5;

    auto invalid = original;
    invalid.outputDeviceUid = "vlt-device-smoke-missing-device";
    if (controller.applyAudioConfiguration(invalid) ||
        !playbackHealthy(controller, "rejected device preserves playback")) return 6;

    if (argc > 3) {
        auto alternate = original;
        alternate.outputDeviceUid = argv[3];
        alternate.inputEnabled = false;
        alternate.inputDeviceUid.clear();
        alternate.inputChannelSelectors.clear();
        alternate.outputChannelSelectors.clear();
        alternate.bufferSize = 512;
        if (auto switched = controller.applyAudioConfiguration(alternate); !switched) {
            std::printf("FAILED to switch device: %s\n", switched.message().c_str());
            return 2;
        }
        std::printf("switched without restart: out=%s, %u frames\n",
                    controller.currentOutputDeviceUid().c_str(),
                    controller.bufferSizeFrames());
        if (!controller.isPlaying()) {
            std::printf("FAILED: playback stopped while switching device\n");
            return 4;
        }
        if (!playbackHealthy(controller, "alternate output")) return 7;
        if (auto restored = controller.applyAudioConfiguration(original); !restored) {
            std::printf("FAILED to switch back: %s\n", restored.message().c_str());
            return 3;
        }
        std::printf("restored without restart: out=%s, %u frames\n",
                    controller.currentOutputDeviceUid().c_str(),
                    controller.bufferSizeFrames());
        if (!controller.isPlaying()) {
            std::printf("FAILED: playback stopped while restoring device\n");
            return 5;
        }
        if (!playbackHealthy(controller, "restored output")) return 8;
    }

    controller.stop();
    controller.shutdown();
    std::printf("done\n");
    return 0;
} catch (const std::exception& error) {
    std::printf("FAILED: %s\n", error.what());
    return 1;
}
