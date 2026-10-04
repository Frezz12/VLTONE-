#include "SamplePitchAnalysis.hpp"
#include "Internal/SamplerInstance.hpp"
#include "Internal/SampleDecoder.hpp"
#include "Internal/SamplerParams.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <random>
#include <vector>

using namespace daw;
namespace sampler = plugins::sampler;
using analysis::SamplePitchStatus;
static int failures = 0;
static void check(bool condition, const char* label) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", label);
    if (!condition) ++failures;
}

static std::shared_ptr<engine::SampleBuffer> tone(double hz, double rate = 48000,
    double seconds = .65, bool harmonic = false) {
    auto audio = std::make_shared<engine::SampleBuffer>(2,
        engine::FrameCount(rate * seconds), rate);
    for (engine::FrameCount i = 0; i < audio->frames(); ++i) {
        const double phase = 2 * std::numbers::pi * hz * i / rate;
        const float value = float(harmonic ? .08 * std::sin(phase) + .35 * std::sin(2 * phase) +
            .22 * std::sin(3 * phase) : .4 * std::sin(phase));
        audio->writableChannel(0)[i] = value + .1f;
        audio->writableChannel(1)[i] = -value - .1f; // AC/DC and opposite-phase stereo.
    }
    return audio;
}

int main() {
    for (const double rate : {44100., 48000., 96000.}) {
        for (const double cents : {-37.4, 0., 18.2}) {
            auto audio = tone(440 * std::exp2(cents / 1200), rate);
            const auto result = analysis::detectSamplePitch(*audio, 0, audio->frames());
            check(result.status == SamplePitchStatus::Detected && result.midiNote == 69 &&
                std::abs(result.cents - cents) < .3,
                "detuned A: note and sub-cent offset at 44.1/48/96 kHz, opposite-phase stereo and DC");
        }
    }
    for (const double hz : {27.5, 65.4064, 220., 1046.5023, 3520.}) {
        auto audio = tone(hz);
        const auto result = analysis::detectSamplePitch(*audio, 0, audio->frames());
        if (result.status != SamplePitchStatus::Detected || std::abs(1200 * std::log2(result.frequencyHz / hz)) >= 1)
            std::printf("frequency %.4f -> %.4f (%+.3f cents), status %d\n", hz,
                result.frequencyHz, 1200 * std::log2(result.frequencyHz / hz), int(result.status));
        check(result.status == SamplePitchStatus::Detected &&
            std::abs(1200 * std::log2(result.frequencyHz / hz)) < 1,
            "bass to treble frequency accuracy");
    }
    auto harmonic = tone(220, 48000, .8, true);
    const auto harmonics = analysis::detectSamplePitch(*harmonic, 0, harmonic->frames());
    check(harmonics.status == SamplePitchStatus::Detected && harmonics.midiNote == 57,
          "strong overtones retain the fundamental");
    for (engine::FrameCount i = 0; i < harmonic->frames(); ++i) {
        const double phase = 2 * std::numbers::pi * 220 * i / 48000;
        harmonic->writableChannel(0)[i] = float(.05 * std::sin(phase) + .4 * std::sin(2 * phase));
        harmonic->writableChannel(1)[i] = -harmonic->writableChannel(0)[i];
    }
    const auto dominantSecond = analysis::detectSamplePitch(*harmonic, 0, harmonic->frames());
    check(dominantSecond.status == SamplePitchStatus::Detected && dominantSecond.midiNote == 57,
          "dominant second harmonic does not shift the root an octave");
    auto silence = std::make_shared<engine::SampleBuffer>(1, 48000, 48000);
    std::fill_n(silence->writableChannel(0), silence->frames(), .2f);
    check(analysis::detectSamplePitch(*silence, 0, silence->frames()).status == SamplePitchStatus::Unavailable,
          "silence/DC does not produce a note");
    std::mt19937 generator(9);
    std::uniform_real_distribution<float> noise(-.5f, .5f);
    for (engine::FrameCount i = 0; i < silence->frames(); ++i)
        silence->writableChannel(0)[i] = noise(generator);
    check(analysis::detectSamplePitch(*silence, 0, silence->frames()).status != SamplePitchStatus::Detected,
          "noise does not enable tuning");
    auto changing = tone(440, 48000, 1.4);
    for (engine::FrameCount i = changing->frames() / 2; i < changing->frames(); ++i)
        for (engine::ChannelCount ch = 0; ch < 2; ++ch)
            changing->writableChannel(ch)[i] = float(.4 * std::sin(2 * std::numbers::pi * 523.2511 * i / 48000));
    check(analysis::detectSamplePitch(*changing, 0, changing->frames()).status == SamplePitchStatus::Unstable,
          "multiple successive notes are not assigned one root");
    const auto trimmed = analysis::detectSamplePitch(*changing, 0, changing->frames() / 2);
    check(trimmed.status == SamplePitchStatus::Detected && trimmed.midiNote == 69,
          "selected stable region is analyzed independently");
    check(analysis::detectSamplePitch(*changing, 0, 800).status == SamplePitchStatus::TooShort,
          "short regions explain why detection is unavailable");
    check(analysis::detectSamplePitch(*changing, 0, changing->frames(), [] { return false; }).status == SamplePitchStatus::Unavailable,
          "analysis can be cancelled");

    const auto source = tone(440 * std::exp2(18.2 / 1200), 48000, 1.0);
    sampler::setSampleDecoder([source](const std::string&) { return source; });
    sampler::SamplerInstance instance;
    instance.activate({48000, 48000});
    instance.startProcessing();
    instance.loadSample("synthetic.wav");
    instance.flushPendingPrecompute();
    instance.setParameter(sampler::indexOf(sampler::Param::RootNote), 69);
    instance.setParameter(sampler::indexOf(sampler::Param::PitchRange), 0);
    instance.setParameter(sampler::kFineTuneIndex, -18.2);
    plugins::PluginEvent note;
    note.kind = plugins::PluginEvent::Kind::NoteOn;
    note.key = 69; note.value = 1;
    auto output = std::make_shared<engine::SampleBuffer>(2, 24000, 48000);
    float* channels[]{output->writableChannel(0), output->writableChannel(1)};
    plugins::PluginProcessContext context;
    context.outputs = channels; context.outputChannels = 2; context.frames = output->frames();
    context.inputEvents = std::span(&note, 1);
    instance.process(context);
    const auto corrected = analysis::detectSamplePitch(*output, 1000, output->frames());
    check(corrected.status == SamplePitchStatus::Detected && corrected.midiNote == 69 &&
        std::abs(corrected.cents) < .5, "Fine Tune corrects rendered audio even with Range zero");
    const auto fine = sampler::parameterTable()[sampler::kFineTuneIndex];
    check(fine.id == "finepitch" && fine.isAutomatable && !fine.isStepped &&
        sampler::kFineTuneIndex == sampler::indexOf(sampler::SlideParam::Count),
        "Fine Tune appends an automatable parameter without moving existing indices");
    std::vector<std::uint8_t> state;
    instance.saveState(state);
    sampler::SamplerInstance restored;
    restored.loadState(state);
    check(restored.parameterValue(sampler::kFineTuneIndex) == -18.2 &&
          restored.parameterValue(sampler::indexOf(sampler::Param::RootNote)) == 69,
          "root and Fine Tune round-trip through sampler state");
    const std::string legacy = R"({"version":1,"sample":"","params":{"rootnote":64}})";
    restored.loadState(std::span(reinterpret_cast<const std::uint8_t*>(legacy.data()), legacy.size()));
    check(restored.parameterValue(sampler::kFineTuneIndex) == 0,
          "legacy states default Fine Tune to zero");
    sampler::setSampleDecoder({});
    return failures ? 1 : 0;
}
