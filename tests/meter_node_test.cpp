#include "DSP/Simd.hpp"
#include "Nodes/MeterNode.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <span>
#include <vector>

using namespace daw::engine;

static int failures = 0;
static void check(bool condition, const char* what) {
    std::printf("%s  %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++failures;
}

namespace {

struct PlanarBlock {
    PlanarBlock(ChannelCount channels, FrameCount frames, float initial = 0.0f)
        : storage(std::size_t(channels) * frames, initial), pointers(channels),
          channels(channels), frames(frames) {
        for (ChannelCount channel = 0; channel < channels; ++channel) {
            pointers[channel] = storage.data() + std::size_t(channel) * frames;
        }
    }

    AudioBlock block() { return AudioBlock(pointers.data(), channels, frames); }

    std::vector<float> storage;
    std::vector<float*> pointers;
    ChannelCount channels;
    FrameCount frames;
};

ProcessContext contextFor(const AudioBlock& output,
                          std::span<const AudioBlock> inputs) {
    ProcessContext context;
    context.output = output;
    context.inputs = inputs;
    context.frames = output.frames();
    return context;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    constexpr FrameCount kFrames = 257; // exercises the SIMD tail too

    // One input is the dominant track-meter shape. Output and measurement must
    // both match the old copy-then-peak reference exactly.
    MeterNode meter;
    PlanarBlock input(2, kFrames);
    for (FrameCount frame = 0; frame < kFrames; ++frame) {
        input.storage[frame] = (int(frame % 17) - 8) * 0.125f;
        input.storage[kFrames + frame] = (int(frame % 23) - 11) * -0.0625f;
    }
    PlanarBlock output(2, kFrames, 99.0f);
    const std::array<AudioBlock, 1> oneInput{input.block()};
    ProcessContext context = contextFor(output.block(), oneInput);
    meter.process(context);

    check(output.storage == input.storage,
          "single-input meter copies every sample exactly");
    check(meter.peakLeft() == dsp::peak(input.block().channel(0)) &&
              meter.peakRight() == dsp::peak(input.block().channel(1)),
          "fused single-input peaks equal the SIMD reference exactly");

    // Multiple contributors retain input order and measure the final sum, not
    // the louder pre-cancellation inputs.
    PlanarBlock a(2, kFrames);
    PlanarBlock b(2, kFrames);
    PlanarBlock c(2, kFrames);
    for (FrameCount frame = 0; frame < kFrames; ++frame) {
        a.storage[frame] = 3.0f + float(frame % 5) * 0.1f;
        b.storage[frame] = -3.0f;
        c.storage[frame] = -float(frame % 3) * 0.25f;
        a.storage[kFrames + frame] = -2.0f;
        b.storage[kFrames + frame] = 0.75f;
        c.storage[kFrames + frame] = float(frame % 7) * 0.125f;
    }
    const std::array<AudioBlock, 3> threeInputs{a.block(), b.block(), c.block()};
    PlanarBlock expected(2, kFrames, -77.0f);
    dsp::sumInto(expected.block(), threeInputs);
    const float expectedLeft = dsp::peak(expected.block().channel(0));
    const float expectedRight = dsp::peak(expected.block().channel(1));

    std::fill(output.storage.begin(), output.storage.end(), -77.0f);
    context = contextFor(output.block(), threeInputs);
    meter.process(context);
    check(output.storage == expected.storage,
          "multi-input fused aggregation is bit-identical to sumInto");
    check(meter.peakLeft() == expectedLeft && meter.peakRight() == expectedRight,
          "multi-input peaks measure the completed sum exactly");

    // Empty and changing layouts fully publish the meter state instead of
    // retaining values from a prior block.
    const std::span<const AudioBlock> noInputs;
    std::fill(output.storage.begin(), output.storage.end(), 1.0f);
    context = contextFor(output.block(), noInputs);
    meter.process(context);
    check(std::all_of(output.storage.begin(), output.storage.end(),
                      [](float sample) { return sample == 0.0f; }) &&
              meter.peakLeft() == 0.0f && meter.peakRight() == 0.0f,
          "empty meter input clears audio and both peaks");

    PlanarBlock monoInput(1, kFrames, -0.5f);
    PlanarBlock monoOutput(1, kFrames, 0.0f);
    const std::array<AudioBlock, 1> monoInputs{monoInput.block()};
    context = contextFor(monoOutput.block(), monoInputs);
    meter.process(context);
    check(meter.peakLeft() == 0.5f && meter.peakRight() == 0.0f,
          "mono processing resets the no-longer-present right peak");

    // Diagnostic microbenchmark against the removed two-pass implementation.
    // Atomic stores are included on both sides so only aggregation differs.
    constexpr FrameCount kBenchFrames = 512;
    constexpr int kIterations = 200000;
    PlanarBlock benchInput(2, kBenchFrames);
    for (std::size_t i = 0; i < benchInput.storage.size(); ++i) {
        benchInput.storage[i] = std::sin(float(i) * 0.017f);
    }
    PlanarBlock oldOutput(2, kBenchFrames);
    PlanarBlock fusedOutput(2, kBenchFrames);
    const std::array<AudioBlock, 1> benchInputs{benchInput.block()};
    ProcessContext fusedContext = contextFor(fusedOutput.block(), benchInputs);
    std::atomic<float> oldLeft{0.0f};
    std::atomic<float> oldRight{0.0f};

    auto oldPass = [&] {
        dsp::sumInto(oldOutput.block(), benchInputs);
        oldLeft.store(dsp::peak(oldOutput.block().channel(0)),
                      std::memory_order_relaxed);
        oldRight.store(dsp::peak(oldOutput.block().channel(1)),
                       std::memory_order_relaxed);
    };
    auto fusedPass = [&] { meter.process(fusedContext); };
    for (int i = 0; i < 1000; ++i) {
        oldPass();
        fusedPass();
    }

    const auto oldStart = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) oldPass();
    const auto oldEnd = std::chrono::steady_clock::now();
    const auto fusedStart = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) fusedPass();
    const auto fusedEnd = std::chrono::steady_clock::now();
    const double oldMs =
        std::chrono::duration<double, std::milli>(oldEnd - oldStart).count();
    const double fusedMs =
        std::chrono::duration<double, std::milli>(fusedEnd - fusedStart).count();
    std::printf("      stereo 512-frame meter x %d: old %.2f ms, fused %.2f ms, "
                "speed-up %.2fx\n",
                kIterations, oldMs, fusedMs, fusedMs > 0.0 ? oldMs / fusedMs : 0.0);
    check(oldLeft.load(std::memory_order_relaxed) == meter.peakLeft() &&
              oldRight.load(std::memory_order_relaxed) == meter.peakRight(),
          "benchmark paths publish identical peaks");

    if (failures == 0) {
        std::puts("\nALL PASSED");
        return 0;
    }
    std::printf("\n%d FAILURE(S)\n", failures);
    return 1;
}
