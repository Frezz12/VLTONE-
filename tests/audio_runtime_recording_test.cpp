#include "AudioRuntime.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <stdexcept>

namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
}

int main() {
    namespace fs = std::filesystem;
    const auto directory = fs::temp_directory_path() /
        ("daw-runtime-recording-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    int result = 0;
    try {
        daw::AudioRuntime runtime;
        constexpr unsigned frames = 64;
        require(bool(runtime.prepare(48000, frames)), "prepare runtime");
        runtime.buildGraph({});
        require(bool(runtime.commitGraph()), "publish empty graph");
        runtime.transportCommand({.action = daw::AudioTransportCommand::Action::Play});
        audio::AudioBuffer input(2, frames), output(2, frames);
        std::fill_n(input.getChannel(0), frames, .25f);
        std::fill_n(input.getChannel(1), frames, .75f);
        audio::AudioCallbackContext block;
        block.inputBuffer = &input; block.outputBuffer = &output;
        block.numFrames = frames; block.sampleRate = 48000;
        const auto render = [&] {
            runtime.processDeviceBlock(block);
            require(block.renderStatus == audio::AudioCallbackContext::RenderStatus::Complete,
                    "device block completes");
        };
        daw::AudioCaptureStarted first, second;
        require(bool(runtime.startCapture({directory.string(), 1, 1, true, 9999}, first)), "start capture");
        runtime.transportCommand({.action = daw::AudioTransportCommand::Action::Pause});
        require(runtime.hasActiveCaptures(), "paused unpublished capture still owns an active WAV writer");
        runtime.transportCommand({.action = daw::AudioTransportCommand::Action::Play});
        render();
        require(runtime.captureStatus(first.id).recordedFrames == 0,
                "capture is invisible to callback before publication");
        const std::array firstOnly{first.id};
        require(runtime.publishCaptures(firstOnly), "publish first capture");
        const auto position = runtime.transportSnapshot().position;
        render();
        const auto initial = runtime.capturePeaks(first.id, 0);
        require(initial.status.recordedFrames == frames && initial.status.startSample == position,
                "capture latches actual first callback position");
        require(initial.values.size() == 1 && std::abs(initial.values.front() - .75f) < 1e-6f,
                "peak batch uses selected hardware input");
        require(bool(runtime.startCapture({directory.string(), 0, 1, true, 0}, second)), "start second capture");
        require(second.id != first.id, "capture identities do not alias");
        const std::array duplicate{first.id, first.id};
        require(!runtime.publishCaptures(duplicate), "reject duplicate capture publication");
        const std::array both{first.id, second.id};
        require(runtime.publishCaptures(both), "publish independent captures");
        runtime.interruptCapture(first.id);
        render();
        audio::RecordingSession closed;
        require(bool(runtime.stopCapture(first.id, closed)), "stop and drain first WAV");
        require(closed.state == audio::RecordingSession::State::Stopped &&
                closed.fileWriteSucceeded && closed.interrupted && closed.capturedFrames == frames * 2 &&
                closed.writtenFrames == closed.capturedFrames && closed.channelCount == 1 &&
                fs::file_size(closed.filePath) > std::uintmax_t(frames * 2), "closed WAV metadata is durable");
        require(!runtime.captureStatus(first.id).available &&
                !runtime.setCaptureInput(first.id, 0, 1, true) &&
                !runtime.publishCaptures(firstOnly), "retired identity rejects stale commands");
        render();
        require(runtime.captureStatus(second.id).recordedFrames == frames * 2,
                "stopping one capture preserves the other published capture");
        require(runtime.setCaptureInput(second.id, 1, 1, true), "retarget capture by identity");
        render();
        const auto retargeted = runtime.capturePeaks(second.id, 0);
        require(retargeted.values.size() == 1 && std::abs(retargeted.values.front() - .75f) < 1e-6f,
                "partial peak bucket grows across input retargeting");
        require(bool(runtime.stopCapture(second.id, closed)) && closed.writtenFrames == frames * 3,
                "second capture finalizes independently");
        require(!runtime.hasActiveCaptures(), "closed captures release runtime ownership");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        result = 1;
    }
    std::error_code ignored;
    fs::remove_all(directory, ignored);
    return result;
}
