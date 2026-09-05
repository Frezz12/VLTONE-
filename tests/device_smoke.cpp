// Manual hardware smoke test — NOT part of ctest, since it opens a real audio
// device. Run ./bin/device_smoke by hand to confirm the device layer and the
// graph engine actually produce sound on this machine.
#include "EngineController.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

int main(int argc, char** argv) {
    daw::EngineController controller;
    if (argc > 1 && std::string(argv[1]) == "--list") {
        for (const auto& device : controller.enumerateOutputDevices()) {
            std::printf("%s | %s | %u output / %u input\n",
                        device.uid.c_str(), device.hostApi.c_str(),
                        device.outputChannels, device.inputChannels);
        }
        return 0;
    }

    audio::AudioDeviceConfig config;
    config.sampleRate = 48000;
    config.bufferSize = argc > 2
        ? static_cast<audio::BufferSize>(std::strtoul(argv[2], nullptr, 10))
        : 512;
    if (argc > 1) {
        config.outputDeviceUid = argv[1];
        config.inputDeviceUid = argv[1];
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
    std::printf("nodes in the compiled graph: %zu\n",
                controller.routingGraph() ? controller.routingGraph()->nodes.size() : 0);
    controller.play();

    if (argc > 3) {
        auto alternate = config;
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
        if (auto restored = controller.applyAudioConfiguration(config); !restored) {
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
    }

    std::this_thread::sleep_for(std::chrono::seconds(2));
    std::printf("position after 2 s: %.2f s, dsp load %.1f%%\n",
                controller.positionSeconds(), controller.dspLoad() * 100.0);
    controller.stop();
    controller.shutdown();
    std::printf("done\n");
    return 0;
}
