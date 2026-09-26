#include "DSP/LoudnessMeter.hpp"
#include "Engine/RealtimeEngine.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <vector>

using namespace daw::engine;
namespace {
int failures = 0;
void check(bool yes, const char* label) {
    std::printf("%s LUFS: %s\n", yes ? "PASS" : "FAIL", label); failures += !yes;
}
bool near(float a, float b, float tolerance = .06f) { return std::abs(a - b) < tolerance; }
void feed(LoudnessMeter& meter, double rate, double seconds, float amplitude,
          bool integrating = true, bool mono = false, unsigned blockSize = 257, double frequency = 1000) {
    std::vector<float> left(blockSize), right(blockSize);
    std::array<float*, 2> data{left.data(), right.data()};
    const auto total = std::uint64_t(std::llround(seconds * rate));
    for (std::uint64_t at = 0; at < total; at += blockSize) {
        const auto frames = FrameCount(std::min<std::uint64_t>(blockSize, total - at));
        for (unsigned i = 0; i < frames; ++i) {
            left[i] = amplitude * std::sin(2 * std::numbers::pi * frequency * (at + i) / rate);
            right[i] = -left[i];
        }
        meter.process({data.data(), ChannelCount(mono ? 1 : 2), frames}, frames, integrating);
    }
}
struct Tone : Node {
    std::uint64_t at = 0;
    std::string_view name() const noexcept override { return "LUFS reference sine"; }
    MidiNodeRole midiRole() const noexcept override { return MidiNodeRole::None; }
    void process(const ProcessContext& c) override {
        for (unsigned i = 0; i < c.frames; ++i, ++at) {
            const float value = .0707945784f * std::sin(2 * std::numbers::pi * 1000 * at / c.sampleRate);
            for (unsigned ch = 0; ch < c.output.numChannels(); ++ch) c.output.data(ch)[i] = value;
        }
    }
};
}

int main() {
    for (const double rate : {32000, 44100, 48000, 96000}) {
        LoudnessMeter meter; meter.prepare(rate);
        check(std::isnan(meter.levels().shortTerm), "initial window has no invented reading");
        feed(meter, rate, 3.1, .0707945784f);
        const auto levels = meter.levels();
        std::printf("rate %.0f: M %.3f S %.3f I %.3f\n", rate, levels.momentary, levels.shortTerm, levels.integrated);
        check(near(levels.momentary, -23) && near(levels.shortTerm, -23) && near(levels.integrated, -23),
              "calibrated 1 kHz antiphase stereo reads -23 LUFS at each device rate");
        meter.requestReset();
        check(std::isnan(meter.levels().integrated), "reset is immediately visible before another callback");
        feed(meter, rate, 3.1, .0707945784f, true, true);
        check(near(meter.levels().integrated, -26.0103f), "mono counts one channel's energy, not two");
    }
    LoudnessMeter meter; meter.prepare(48000);
    feed(meter, 48000, .3, .1f);
    check(std::isnan(meter.levels().momentary) && std::isnan(meter.levels().integrated), "incomplete 400 ms blocks are not measured");
    feed(meter, 48000, .1, .1f);
    check(std::isfinite(meter.levels().momentary) && std::isnan(meter.levels().shortTerm), "M starts at 400 ms while S waits for 3 s");
    meter.requestReset(); feed(meter, 48000, 30, .0707945784f);
    const float before = meter.levels().integrated;
    feed(meter, 48000, 6, .00316227766f); // -50 LUFS, below the relative gate
    check(near(meter.levels().integrated, before, .12f), "relative gate excludes a long quiet passage");
    feed(meter, 48000, 6, .00001f);
    check(near(meter.levels().integrated, before, .12f), "absolute gate excludes the noise floor");
    const float paused = meter.levels().integrated;
    feed(meter, 48000, 4, .5f, false);
    check(meter.levels().integrated == paused && meter.levels().shortTerm > -7,
          "I pauses with transport while live monitoring still updates M/S");
    meter.requestReset(); feed(meter, 48000, 4, 0);
    check(std::isinf(meter.levels().momentary) && std::isinf(meter.levels().integrated) &&
          meter.levels().momentary < 0, "silence produces minus infinity without NaN poisoning");
    meter.requestReset(); feed(meter, 48000, 4, .1f, true, false, 64, 50);
    const auto bass = meter.levels().shortTerm;
    meter.requestReset(); feed(meter, 48000, 4, .1f, true, false, 1024, 1000);
    check(bass < meter.levels().shortTerm - 4, "K weighting attenuates sub-bass rather than reporting ordinary RMS");
    const auto largeBlocks = meter.levels();
    meter.requestReset(); feed(meter, 48000, 4, .1f, true, false, 31);
    check(near(largeBlocks.integrated, meter.levels().integrated, .0001f) &&
          near(largeBlocks.shortTerm, meter.levels().shortTerm, .0001f), "window boundaries are independent of device block size");

    RealtimeEngine engine(1);
    auto source = std::make_shared<Tone>();
    engine.graph().setSink(engine.graph().adoptNode(source));
    check(bool(engine.prepare(48000, 256, 2)), "realtime engine prepares the meter");
    engine.transport().play();
    std::array<float, 256> left{}, right{};
    std::array<float*, 2> data{left.data(), right.data()};
    for (int i = 0; i < 600; ++i) engine.renderBlock({data.data(), 2, 256}, nullptr, 0, 256);
    check(near(engine.masterLoudness().integrated, -23) && near(engine.masterLoudness().shortTerm, -23),
          "master display receives actual final output loudness");
    const float held = engine.masterLoudness().integrated;
    engine.transport().stop();
    for (int i = 0; i < 10; ++i) engine.renderBlock({data.data(), 2, 256}, nullptr, 0, 256);
    check(engine.masterLoudness().integrated == held, "engine transport stop holds the integrated result");
    engine.resetMasterLoudness();
    check(std::isnan(engine.masterLoudness().integrated), "master reset clears the published result");
    return failures ? 1 : 0;
}
