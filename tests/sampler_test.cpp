// The built-in sampler: its parameter surface, its DSP, its state chunk, and
// one end-to-end pass through a real project.
//
// The sample under test is synthesised rather than read from disk wherever the
// file layer is not the thing being checked — a constant 0.5 DC block makes
// every gain, envelope, fade and precomputed effect readable as a single
// number, which a sine would hide behind its own shape.
#include "EngineController.hpp"
#include "Internal/InternalFactory.hpp"
#include "Internal/SampleDecoder.hpp"
#include "Internal/SamplerInstance.hpp"
#include "Internal/SamplerParams.hpp"
#include "Recording/RecordingEngine.hpp"
#include "Core/AudioBuffer.hpp"
#include "platform/AudioFileDecoder.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <numbers>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace daw;
using namespace daw::plugins;
using sampler::Param;
using sampler::ModParam;
using sampler::ModTarget;

static int failures = 0;
static bool check(bool condition, const char* what) {
    std::printf("%s  %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++failures;
    return condition;
}

namespace {

constexpr double kRate = 48000.0;
constexpr std::uint32_t kBlock = 512;
/// The synthetic sample: 4800 frames (0.1 s) of constant 0.5, both channels.
constexpr std::uint32_t kSampleFrames = 4800;
constexpr const char* kFakePath = "/synthetic/dc.wav";

std::shared_ptr<const engine::SampleBuffer> makeDcSample(float level = 0.5f,
                                                         std::uint32_t frames = kSampleFrames) {
    auto buffer = std::make_shared<engine::SampleBuffer>(2, frames, kRate);
    for (engine::ChannelCount ch = 0; ch < 2; ++ch) {
        float* data = buffer->writableChannel(ch);
        std::fill_n(data, frames, level);
    }
    return buffer;
}

/// A block of output plus the pointer array the plugin API wants.
struct Output {
    explicit Output(std::uint32_t frames) : left(frames, 0.0f), right(frames, 0.0f) {
        pointers[0] = left.data();
        pointers[1] = right.data();
    }
    std::vector<float> left;
    std::vector<float> right;
    float* pointers[2] = {nullptr, nullptr};
};

/// Render `frames` through the instance, optionally with events at frame 0.
Output render(sampler::SamplerInstance& instance, std::uint32_t frames,
              const std::vector<PluginEvent>& events = {}) {
    Output out(frames);
    PluginProcessContext context;
    context.outputs = out.pointers;
    context.outputChannels = 2;
    context.frames = frames;
    context.inputEvents = events;
    context.playing = true;
    instance.process(context);
    return out;
}

PluginEvent noteOn(int key, double velocity, std::uint32_t offset = 0,
                   double pan = 0.0) {
    PluginEvent event;
    event.kind = PluginEvent::Kind::NoteOn;
    event.key = std::int16_t(key);
    event.value = velocity;
    event.frameOffset = offset;
    event.notePan = pan;
    return event;
}

PluginEvent noteOff(int key, std::uint32_t offset = 0) {
    PluginEvent event;
    event.kind = PluginEvent::Kind::NoteOff;
    event.key = std::int16_t(key);
    event.frameOffset = offset;
    return event;
}

float peakOf(const std::vector<float>& data) {
    float peak = 0.0f;
    for (float sample : data) peak = std::max(peak, std::abs(sample));
    return peak;
}

/// How many frames at the head of the block are non-silent — the length of a
/// one-shot, measured.
std::size_t soundingFrames(const std::vector<float>& data) {
    std::size_t last = 0;
    for (std::size_t i = 0; i < data.size(); ++i) {
        if (std::abs(data[i]) > 1e-5f) last = i + 1;
    }
    return last;
}

void set(sampler::SamplerInstance& instance, Param parameter, double value) {
    instance.setParameter(std::uint32_t(parameter), value);
}

void setMod(sampler::SamplerInstance& instance, ModTarget target, ModParam parameter,
            double value) {
    instance.setParameter(sampler::indexOf(target, parameter), value);
}

std::unique_ptr<sampler::SamplerInstance> makeSampler(bool withSample = true) {
    auto instance = std::make_unique<sampler::SamplerInstance>();
    PluginProcessInfo info;
    info.sampleRate = kRate;
    info.maxBlockSize = kBlock * 8;
    instance->activate(info);
    instance->startProcessing();
    if (withSample) instance->loadSample(kFakePath);
    return instance;
}

} // namespace

int main() {
    // Every standalone test reads the same synthetic buffer, whatever path it
    // is asked for — the file layer is exercised separately at the end.
    sampler::setSampleDecoder([](const std::string&) { return makeDcSample(); });

    // ── The parameter table ──
    {
        const std::span<const ParameterInfo> table = sampler::parameterTable();
        check(table.size() == sampler::kParameterCount, "table has every parameter");

        bool indicesMatch = true;
        std::set<std::string> ids;
        bool unique = true;
        bool defaultsInRange = true;
        for (std::uint32_t i = 0; i < table.size(); ++i) {
            if (table[i].index != i) indicesMatch = false;
            if (!ids.insert(table[i].id).second) unique = false;
            if (table[i].defaultValue < table[i].minValue ||
                table[i].defaultValue > table[i].maxValue) {
                defaultsInRange = false;
            }
        }
        check(indicesMatch, "every row's index is its position");
        check(unique, "parameter ids are unique");
        check(defaultsInRange, "every default sits inside its range");

        check(std::abs(table[std::uint32_t(Param::Volume)].defaultValue - 0.55) < 1e-9,
              "volume defaults to 55 % (-5.2 dB), as the sampler is documented to");
        check(!table[std::uint32_t(Param::PreNormalize)].isAutomatable,
              "a precomputed knob is not automatable");
        check(table[std::uint32_t(Param::Volume)].isAutomatable,
              "an ordinary knob is");
        check(table[std::uint32_t(Param::CutItself)].id == "cutitself" &&
                  table[std::uint32_t(Param::CutItself)].defaultValue == 0.0,
              "Cut Itself has a stable id and is off for old projects");
        check(table[std::uint32_t(Param::EndOffset)].id == "endoffset" &&
                  table[std::uint32_t(Param::EndOffset)].defaultValue == 1.0,
              "the new End marker defaults to the physical sample end");
        check(table[std::uint32_t(Param::Formant)].id == "formant" &&
                  table[std::uint32_t(Param::Formant)].minValue == -12.0 &&
                  table[std::uint32_t(Param::Formant)].maxValue == 12.0,
              "Formant has a stable bipolar semitone range");
        check(sampler::isPrecomputed(std::uint32_t(Param::PreBoost)) &&
                  !sampler::isPrecomputed(std::uint32_t(Param::Volume)),
              "the precomputed range covers the right knobs");
        check(sampler::indexOf(ModTarget::Pitch, ModParam::LfoShape) ==
                  sampler::kParameterCount - 1,
              "the INS matrix ends exactly at the table's end");
    }

    // ── The factory ──
    {
        PluginFactory* factory = factoryFor(Format::Internal);
        if (check(factory != nullptr, "there is an internal factory")) {
            const std::vector<PluginDescriptor> found = factory->inspect({});
            const auto samplerDescriptor = std::find_if(
                found.begin(), found.end(), [](const PluginDescriptor& descriptor) {
                    return descriptor.uid == "daw.sampler";
                });
            check(samplerDescriptor != found.end(),
                  "it advertises the sampler");
            check(samplerDescriptor != found.end() && samplerDescriptor->isInstrument &&
                      samplerDescriptor->mainOutputChannels == 2,
                  "the sampler is a stereo instrument");
            check(factory->enumerateCandidates("/anywhere").empty(),
                  "a built-in is never a scan candidate");
            auto instance = samplerDescriptor != found.end()
                ? factory->create(*samplerDescriptor) : nullptr;
            check(instance != nullptr, "the factory instantiates it");
        }
        check(!builtinPlugins().empty(), "builtinPlugins() lists it for the menus");
    }

    // ── Nothing loaded ──
    {
        auto instance = makeSampler(/*withSample=*/false);
        const Output out = render(*instance, kBlock, {noteOn(60, 1.0)});
        check(peakOf(out.left) == 0.0f, "a sampler with no sample is silent");
    }

    // ── The plain one-shot ──
    {
        auto instance = makeSampler();
        set(*instance, Param::Volume, 1.0);
        const Output out = render(*instance, kBlock, {noteOn(60, 1.0)});
        // 0.5 sample × velocity 1 × volume 1, panned centre (−3 dB a side).
        const float expected =
            0.5f * float(std::cos(std::numbers::pi_v<double> * 0.25));
        check(std::abs(out.left[10] - expected) < 0.01f,
              "a note at the root note plays the sample at its own level");
        check(std::abs(out.left[10] - out.right[10]) < 1e-6f, "centred by default");
    }

    // ── Velocity and volume ──
    {
        auto instance = makeSampler();
        set(*instance, Param::Volume, 1.0);
        const Output full = render(*instance, kBlock, {noteOn(60, 1.0)});
        auto quiet = makeSampler();
        set(*quiet, Param::Volume, 0.5);
        const Output half = render(*quiet, kBlock, {noteOn(60, 0.5)});
        check(std::abs(half.left[10] - full.left[10] * 0.25f) < 0.01f,
              "velocity and the volume knob both scale the voice");
    }

    // ── Pitch follows the key ──
    {
        auto instance = makeSampler();
        // An octave up reads the sample twice as fast, so the one-shot is half
        // as long — the cleanest observable a constant sample offers.
        const Output up = render(*instance, kSampleFrames, {noteOn(72, 1.0)});
        const std::size_t sounding = soundingFrames(up.left);
        check(sounding > kSampleFrames / 2 - 64 && sounding < kSampleFrames / 2 + 64,
              "a note an octave up plays the sample in half the time");

        auto same = makeSampler();
        const Output root = render(*same, kSampleFrames + kBlock, {noteOn(60, 1.0)});
        check(soundingFrames(root.left) > kSampleFrames - 64,
              "at the root note it plays the whole sample");
    }

    // ── Root note ──
    {
        auto instance = makeSampler();
        set(*instance, Param::RootNote, 72);
        const Output out = render(*instance, kSampleFrames, {noteOn(72, 1.0)});
        check(soundingFrames(out.left) > kSampleFrames - 64,
              "moving the root note moves which key plays untransposed");
    }

    // ── Start offset ──
    {
        auto instance = makeSampler();
        set(*instance, Param::StartOffset, 0.5);
        const Output out = render(*instance, kSampleFrames, {noteOn(60, 1.0)});
        const std::size_t sounding = soundingFrames(out.left);
        check(sounding > kSampleFrames / 2 - 64 && sounding < kSampleFrames / 2 + 64,
              "the start offset skips into the sample");
    }

    // ── End offset ──
    {
        auto instance = makeSampler();
        set(*instance, Param::EndOffset, 0.5);
        const Output out = render(*instance, kSampleFrames, {noteOn(60, 1.0)});
        const std::size_t sounding = soundingFrames(out.left);
        check(sounding > kSampleFrames / 2 - 64 && sounding < kSampleFrames / 2 + 64,
              "the End marker truncates one-shot playback non-destructively");
    }

    // ── Fades ──
    {
        auto instance = makeSampler();
        set(*instance, Param::FadeIn, 0.5);
        const Output out = render(*instance, kSampleFrames, {noteOn(60, 1.0)});
        check(out.left[0] < 0.01f && out.left[kSampleFrames / 4] > 0.05f &&
                  out.left[kSampleFrames / 4] < out.left[kSampleFrames / 2 + 100],
              "the fade-in ramps the sample up");
    }

    // ── One-shot vs. loop on note-off ──
    {
        auto instance = makeSampler();
        // The amplitude envelope is off, so a released one-shot must play on.
        Output out(kSampleFrames);
        PluginProcessContext context;
        context.outputs = out.pointers;
        context.outputChannels = 2;
        context.frames = kSampleFrames;
        const std::vector<PluginEvent> events{noteOn(60, 1.0), noteOff(60, 100)};
        context.inputEvents = events;
        instance->process(context);
        check(soundingFrames(out.left) > kSampleFrames - 64,
              "with the envelope off a released one-shot plays to its end");
    }
    {
        auto instance = makeSampler();
        set(*instance, Param::LoopMode, 1);
        const Output out = render(*instance, kSampleFrames * 3, {noteOn(60, 1.0)});
        check(std::abs(out.left[kSampleFrames * 5 / 2]) > 0.1f,
              "a forward loop keeps sounding past the end of the sample");

        auto released = makeSampler();
        set(*released, Param::LoopMode, 1);
        const Output stopped = render(*released, kSampleFrames * 3,
                                      {noteOn(60, 1.0), noteOff(60, 100)});
        check(soundingFrames(stopped.left) < 1000,
              "releasing a looping note stops it, envelope or no envelope");
    }

    // ── The amplitude envelope ──
    {
        auto instance = makeSampler();
        set(*instance, Param::Volume, 1.0);
        set(*instance, Param::AmpEnvOn, 1);
        set(*instance, Param::AmpAttack, 0.05);   // 2400 frames
        set(*instance, Param::AmpSustain, 1.0);
        set(*instance, Param::LoopMode, 1);       // keep sounding through the attack
        const Output out = render(*instance, 4800, {noteOn(60, 1.0)});
        check(out.left[10] < 0.02f, "the attack starts from silence");
        check(out.left[1200] > 0.1f && out.left[1200] < out.left[2300],
              "and rises through it");
        check(out.left[3000] > out.left[2300] * 0.9f, "reaching sustain after it");
    }
    {
        auto instance = makeSampler();
        set(*instance, Param::AmpEnvOn, 1);
        set(*instance, Param::AmpRelease, 0.02);   // 960 frames
        set(*instance, Param::LoopMode, 1);
        const Output out = render(*instance, 4800, {noteOn(60, 1.0), noteOff(60, 1000)});
        check(std::abs(out.left[1200]) > 0.05f, "the release starts from the held level");
        check(std::abs(out.left[2400]) < 1e-4f, "and reaches silence");
    }

    // ── Tension bends the ramp but keeps its ends ──
    {
        check(std::abs(sampler::applyTension(0.0, 0.7)) < 1e-9 &&
                  std::abs(sampler::applyTension(1.0, 0.7) - 1.0) < 1e-9,
              "tension pins both ends of the ramp");
        check(sampler::applyTension(0.5, 0.7) > 0.5, "positive tension starts fast");
        check(sampler::applyTension(0.5, -0.7) < 0.5, "negative tension starts slow");
        check(std::abs(sampler::applyTension(0.5, 0.0) - 0.5) < 1e-9,
              "zero tension is a straight line");
    }

    // ── Pan, and the pan modulation target ──
    {
        auto instance = makeSampler();
        set(*instance, Param::Volume, 1.0);
        set(*instance, Param::Pan, 1.0);
        const Output out = render(*instance, kBlock, {noteOn(60, 1.0)});
        check(std::abs(out.left[10]) < 1e-4f && out.right[10] > 0.3f,
              "pan hard right leaves nothing on the left");
    }
    {
        auto instance = makeSampler();
        set(*instance, Param::Volume, 1.0);
        const Output out =
            render(*instance, kBlock, {noteOn(60, 1.0, 0, 1.0)});
        check(std::abs(out.left[10]) < 1e-4f && out.right[10] > 0.3f,
              "Piano Roll note pan reaches the Sampler voice");
    }

    // ── The filter, driven by MOD X ──
    {
        auto instance = makeSampler();
        set(*instance, Param::Volume, 1.0);
        set(*instance, Param::ModX, 0.0);   // 20 Hz: a DC block barely gets through
        const Output out = render(*instance, kBlock, {noteOn(60, 1.0)});
        check(peakOf(out.left) < 0.2f, "MOD X closes the filter");

        auto open = makeSampler();
        set(*open, Param::Volume, 1.0);
        const Output through = render(*open, kBlock, {noteOn(60, 1.0)});
        check(peakOf(through.left) > 0.3f, "and is open by default");
    }

    // ── Precomputed effects ──
    {
        sampler::setSampleDecoder([](const std::string&) { return makeDcSample(0.25f); });
        auto instance = makeSampler();
        auto before = instance->sample();
        check(before && std::abs(before->audio->channel(0)[0] - 0.25f) < 1e-6f,
              "the raw sample is used when nothing is baked into it");

        set(*instance, Param::PreNormalize, 1);
        instance->rebuildProcessedSample();
        auto normalized = instance->sample();
        check(normalized && std::abs(normalized->audio->channel(0)[0] - 1.0f) < 1e-4f,
              "Normalize lifts the sample to full scale");

        set(*instance, Param::PreNormalize, 0);
        set(*instance, Param::PreReversePolarity, 1);
        instance->rebuildProcessedSample();
        auto flipped = instance->sample();
        check(flipped && std::abs(flipped->audio->channel(0)[0] + 0.25f) < 1e-6f,
              "Reverse polarity inverts it");

        set(*instance, Param::PreReversePolarity, 0);
        set(*instance, Param::PreBoost, 1.0);
        instance->rebuildProcessedSample();
        auto boosted = instance->sample();
        check(boosted && std::abs(boosted->audio->channel(0)[0] - 1.0f) < 1e-6f,
              "Boost drives it into the clipper");

        // Keep on disk is the documented escape hatch: the same knobs, no bake.
        set(*instance, Param::KeepOnDisk, 1);
        instance->rebuildProcessedSample();
        auto raw = instance->sample();
        check(raw && std::abs(raw->audio->channel(0)[0] - 0.25f) < 1e-6f,
              "Keep on disk skips the precomputed stage");
        set(*instance, Param::KeepOnDisk, 0);

        set(*instance, Param::PreBoost, 0.0);
        set(*instance, Param::PreReverb, 0.5);
        set(*instance, Param::PreReverbType, 1);
        instance->rebuildProcessedSample();
        auto reverbed = instance->sample();
        check(reverbed && reverbed->audio->frames() > reverbed->baseFrames,
              "the reverb appends a tail past the sample's own length");
        check(reverbed && reverbed->baseFrames == kSampleFrames,
              "and leaves the base length — what the markers address — alone");

        sampler::setSampleDecoder([](const std::string&) { return makeDcSample(); });
    }

    // ── Main-thread pump only queues; the bake worker publishes later ──
    {
        constexpr std::uint32_t kHeavyFrames = 2'000'000;
        sampler::setSampleDecoder([](const std::string&) {
            return makeDcSample(0.25f, kHeavyFrames);
        });

        auto instance = makeSampler();
        instance->flushPendingPrecompute();
        instance->setParameterFromHost(std::uint32_t(Param::PreRingMix), 1.0);
        const auto pumpStart = std::chrono::steady_clock::now();
        instance->pumpMainThread();
        const auto pumpEnd = std::chrono::steady_clock::now();
        const double pumpMs = std::chrono::duration<double, std::milli>(
                                  pumpEnd - pumpStart).count();
        std::printf("      heavy bake pump: %.3f ms\n", pumpMs);
        check(pumpMs < 20.0 && instance->precomputePending(),
              "pumpMainThread returns while a heavy bake remains pending");

        // Supersede the expensive ring-mod request while it is in flight. The
        // worker cancels/discards it and only the newest polarity bake lands.
        instance->setParameterFromHost(std::uint32_t(Param::PreRingMix), 0.0);
        instance->setParameterFromHost(
            std::uint32_t(Param::PreReversePolarity), 1.0);
        instance->pumpMainThread();
        instance->flushPendingPrecompute();
        auto data = instance->sample();
        check(data && std::abs(data->audio->channel(0)[0] + 0.25f) < 1e-6f &&
                  !instance->precomputePending(),
              "latest precompute generation wins and stale work is discarded");

        // Destruction advances the cancellation token and joins the worker;
        // no task retains or publishes through the dead instance.
        auto doomed = makeSampler();
        doomed->flushPendingPrecompute();
        doomed->setParameterFromHost(std::uint32_t(Param::PreRingMix), 1.0);
        doomed->pumpMainThread();
        check(doomed->precomputePending(),
              "lifetime test destroys an instance with work outstanding");
        const auto destroyStart = std::chrono::steady_clock::now();
        doomed.reset();
        const double destroyMs = std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() -
                                     destroyStart).count();
        std::printf("      cancelled bake destruction: %.3f ms\n", destroyMs);
        check(destroyMs < 500.0,
              "destroy cancels and joins an outstanding bake promptly");

        sampler::setSampleDecoder([](const std::string&) { return makeDcSample(); });
    }

    // ── State ──
    {
        auto instance = makeSampler();
        set(*instance, Param::Volume, 0.31);
        set(*instance, Param::LoopMode, 2);
        set(*instance, Param::CutItself, 1.0);
        set(*instance, Param::EndOffset, 0.8);
        set(*instance, Param::Formant, -3.0);
        setMod(*instance, ModTarget::Cutoff, ModParam::LfoAmount, -0.75);
        std::vector<std::uint8_t> chunk;
        check(instance->saveState(chunk) && !chunk.empty(), "state saves");

        auto restored = makeSampler(/*withSample=*/false);
        check(restored->loadState(chunk), "state loads");
        check(std::abs(restored->parameterValue(std::uint32_t(Param::Volume)) - 0.31) < 1e-9,
              "a knob survives the round trip");
        check(std::abs(restored->parameterValue(std::uint32_t(Param::LoopMode)) - 2.0) < 1e-9,
              "so does a stepped one");
        check(restored->parameterValue(std::uint32_t(Param::CutItself)) == 1.0,
              "and Cut Itself survives the state round trip");
        check(std::abs(restored->parameterValue(std::uint32_t(Param::EndOffset)) - 0.8) <
                  1e-9 &&
                  std::abs(restored->parameterValue(std::uint32_t(Param::Formant)) + 3.0) <
                  1e-9,
              "End and Formant survive the sampler state round trip");
        check(std::abs(restored->parameterValue(
                           sampler::indexOf(ModTarget::Cutoff, ModParam::LfoAmount)) +
                       0.75) < 1e-9,
              "and one from the INS matrix");
        check(restored->samplePath() == kFakePath, "the sample comes back with it");
        check(restored->sample() != nullptr, "and is reloaded");
    }
    {
        // A state chunk from a build with a knob this one does not have must
        // still apply the knobs it does.
        auto instance = makeSampler(/*withSample=*/false);
        const std::string text =
            R"({"version":1,"sample":"","params":{"vol":0.2,"nonsuch.knob":9}})";
        const std::vector<std::uint8_t> chunk(text.begin(), text.end());
        check(instance->loadState(chunk), "a state with an unknown id still loads");
        check(std::abs(instance->parameterValue(std::uint32_t(Param::Volume)) - 0.2) < 1e-9,
              "and the known ids are applied");
        check(instance->parameterValue(std::uint32_t(Param::CutItself)) == 0.0,
              "an older state leaves Cut Itself safely off");
    }

    // ── Polyphony ──
    {
        auto instance = makeSampler();
        set(*instance, Param::Volume, 1.0);
        const Output one = render(*instance, kBlock, {noteOn(60, 1.0)});
        auto two = makeSampler();
        set(*two, Param::Volume, 1.0);
        const Output both = render(*two, kBlock, {noteOn(60, 1.0), noteOn(60, 1.0)});
        check(both.left[10] > one.left[10] * 1.9f,
              "two notes sound at once rather than stealing each other");
    }
    {
        auto instance = makeSampler();
        set(*instance, Param::Volume, 1.0);
        set(*instance, Param::CutItself, 1.0);
        const Output cut = render(*instance, kBlock,
                                  {noteOn(60, 1.0), noteOn(64, 1.0, 64)});

        auto reference = makeSampler();
        set(*reference, Param::Volume, 1.0);
        const Output one = render(*reference, kBlock, {noteOn(64, 1.0, 64)});
        check(std::abs(cut.left[96] - one.left[96]) < 0.01f,
              "Cut Itself chokes the previous voice when the next note begins");
    }

    // ── The stretch engine ──
    {
        auto instance = makeSampler();
        set(*instance, Param::StretchMode, 1);
        set(*instance, Param::StretchTime, 2.0);
        const Output out = render(*instance, kSampleFrames * 3, {noteOn(60, 1.0)});
        const std::size_t sounding = soundingFrames(out.left);
        check(sounding > kSampleFrames * 3 / 2,
              "stretch time makes the sample last longer without changing its key");

        auto shifted = makeSampler();
        set(*shifted, Param::StretchMode, 1);
        set(*shifted, Param::StretchPitch, 12.0);
        const Output up = render(*shifted, kSampleFrames * 2, {noteOn(60, 1.0)});
        check(soundingFrames(up.left) > kSampleFrames - 200,
              "and a pitch shift in that mode does not shorten it");
    }

    // The formant control tilts the spectral envelope in the two modes that
    // claim to support it — and stays out of the way in the ones that do not,
    // which is what its greyed-out knob promises there.
    {
        auto plain = makeSampler();
        set(*plain, Param::StretchMode, 3);
        const Output flat = render(*plain, kSampleFrames, {noteOn(60, 1.0)});

        auto tilted = makeSampler();
        set(*tilted, Param::StretchMode, 3);
        set(*tilted, Param::Formant, 12.0);
        const Output shifted = render(*tilted, kSampleFrames, {noteOn(60, 1.0)});
        check(std::abs(peakOf(shifted.left) - peakOf(flat.left)) > 0.02f,
              "formant changes the sound in Vocal mode");

        // …and in the plain resampling mode the sampler starts in, which is
        // where a knob that did nothing used to sit greyed out.
        auto resample = makeSampler();
        set(*resample, Param::Formant, 12.0);
        const Output untouched = render(*resample, kSampleFrames, {noteOn(60, 1.0)});
        auto reference = makeSampler();
        const Output baseline = render(*reference, kSampleFrames, {noteOn(60, 1.0)});
        check(std::abs(peakOf(untouched.left) - peakOf(baseline.left)) > 0.02f,
              "and in Resample mode too");
    }

    // ── End to end: a sampler on an instrument track, rendered to a file ──
    {
        const fs::path dir = fs::temp_directory_path() / "daw-sampler-test";
        fs::remove_all(dir);
        fs::create_directories(dir);
        const std::string wav = (dir / "tone.wav").string();
        {
            audio::AudioBuffer tone(2, 24000);
            for (audio::BufferSize f = 0; f < 24000; ++f) {
                const float s =
                    0.8f * std::sin(2.0f * 3.14159265f * 220.0f * float(f) / 48000.0f);
                tone.getChannel(0)[f] = s;
                tone.getChannel(1)[f] = s;
            }
            audio::AudioRecorder recorder;
            recorder.initialize(48000, 2);
            recorder.writeWAVFile(wav, tone, 48000);
        }

        EngineController controller;
        check(controller.initialize(48000, 512, /*openDevice=*/false).isOk(),
              "controller initialises offline");

        const std::string track = controller.addTrack(TrackKind::Instrument, "Sampler");
        const auto descriptor =
            controller.pluginManager().find(Format::Internal, "daw.sampler");
        check(descriptor.has_value(), "the manager lists the sampler with no scan");

        const auto contains = [](const std::vector<PluginDescriptor>& list) {
            return std::any_of(list.begin(), list.end(), [](const PluginDescriptor& d) {
                return d.uid == "daw.sampler";
            });
        };
        check(contains(controller.pluginManager().instruments()),
              "and offers it in the instrument menu");
        check(!contains(controller.pluginManager().effects()),
              "but not in the effects menu");

        if (descriptor) {
            controller.setTrackInstrumentPlugin(track, *descriptor);
            const TrackModel* model = controller.project().findTrack(track);
            const std::string slot = model ? model->instrument.id : std::string{};
            check(!slot.empty() && controller.samplerInstance(track, slot) != nullptr,
                  "it loads into the instrument slot");

            check(controller.loadSamplerSample(track, slot, wav),
                  "and takes a sample through the controller");
            controller.setInsertParameter(track, slot, "vol", 1.0);

            // A key played rather than written: it must find the track's MIDI
            // path, and a track that has none must say so instead of pretending
            // to have played something.
            check(controller.liveNoteTarget({}) == track,
                  "the instrument track is where a played note goes");
            check(controller.liveNoteOn(track, 60, 100) &&
                      controller.liveNoteOff(track, 60),
                  "a live note reaches the instrument");
            check(controller.liveMidiEvent(track, 0xB3, 1, 64) &&
                      !controller.liveMidiEvent(track, 0xF8, 0, 0) &&
                      !controller.liveMidiEvent(track, 0x90, 128, 100),
                  "generic live MIDI accepts channel voice data only");
            const std::string audio = controller.addTrack(TrackKind::Audio, "Audio");
            check(!controller.liveNoteOn(audio, 60, 100),
                  "an audio track takes no notes");
            check(controller.liveNoteTarget(audio) == track,
                  "and asking for one falls back to a track that does");

            // Dropping a sample on a track's instrument slot: the sampler and
            // the sample are one gesture, so they must be one undo entry —
            // otherwise undoing a drag leaves an empty sampler behind.
            {
                EngineController drop;
                drop.initialize(48000, 512, /*openDevice=*/false);
                const std::string lane =
                    drop.addTrack(TrackKind::Instrument, "Dropped");
                check(drop.loadInstrumentSampler(lane, wav),
                      "a sample dropped on the instrument slot loads a sampler");
                const TrackModel* dropped = drop.project().findTrack(lane);
                check(dropped && dropped->instrument.isLoaded(),
                      "the slot is filled");
                if (dropped && dropped->instrument.isLoaded()) {
                    sampler::SamplerInstance* instance =
                        drop.samplerInstance(lane, dropped->instrument.id);
                    check(instance && instance->samplePath() == wav,
                          "with the dropped file in it");
                }
                drop.undo();
                dropped = drop.project().findTrack(lane);
                check(dropped && !dropped->instrument.isLoaded(),
                      "and one undo takes the whole drop back");

                const std::string audioLane = drop.addTrack(TrackKind::Audio, "Wave");
                check(!drop.loadInstrumentSampler(audioLane, wav),
                      "an audio track has no instrument slot to drop onto");
            }

            const std::string clip = controller.addMidiClip(track, 0.0, 1.0);
            check(!clip.empty(), "a MIDI clip goes on the track");
            controller.addNote(track, clip, 60, 0.0, 2.0, 100);

            sampler::SamplerInstance* liveSampler =
                controller.samplerInstance(track, slot);

            // Drain the slot's one-time construction wake, then prove idle UI
            // ticks take the generation fast path instead of walking every
            // hosted instance. Keep the run shorter than the deliberately
            // rare compatibility sweep interval.
            (void)controller.pumpPluginEvents();
            const std::uint64_t idleScanCount =
                controller.pluginEventScanCountForTest();
            for (int i = 0; i < 8; ++i) (void)controller.pumpPluginEvents();
            check(controller.pluginEventScanCountForTest() == idleScanCount,
                  "idle plugin-event pumps do not scan the hosted slots");

            controller.setInsertParameter(track, slot, "pre.polarity", 1.0);
            check(liveSampler && liveSampler->precomputePending(),
                  "a controller parameter change queues the sampler bake");
            (void)controller.pumpPluginEvents();
            check(controller.pluginEventScanCountForTest() == idleScanCount + 1,
                  "a sampler precompute request wakes exactly one slot sweep");

            const std::uint64_t wakeScanCount =
                controller.pluginEventScanCountForTest();
            for (int i = 0; i < 63; ++i) (void)controller.pumpPluginEvents();
            check(controller.pluginEventScanCountForTest() == wakeScanCount,
                  "a real wake resets the compatibility-sweep idle interval");
            (void)controller.pumpPluginEvents();
            check(controller.pluginEventScanCountForTest() == wakeScanCount + 1,
                  "the rare compatibility sweep still services legacy plugins");
            controller.play();
            check(liveSampler && !liveSampler->precomputePending(),
                  "pressing play synchronously flushes the latest sampler bake");
            controller.stop();

            controller.setInsertParameter(track, slot, "pre.polarity", 0.0);

            const std::string mix = (dir / "mix.wav").string();
            check(controller.exportMixdown(mix, false).isOk(), "the project renders");
            check(liveSampler && !liveSampler->precomputePending(),
                  "export synchronously flushes a pending sampler bake");
            audio::platform::DecodedAudio decoded;
            check(audio::platform::decodeAudioFile(mix, decoded).isOk() &&
                      decoded.frames > 1000,
                  "the mixdown decodes");
            float peak = 0.0f;
            for (std::size_t i = 0; i < decoded.interleaved.size(); ++i) {
                peak = std::max(peak, std::abs(decoded.interleaved[i]));
            }
            check(peak > 0.1f, "and the sampler is audible in it");

            // The same knob, driven the way the panel drives it — through the
            // controller, as a plugin parameter — reaches the voice and is
            // heard. The DSP being right is not enough: the panel writes here.
            {
                controller.setInsertParameter(track, slot, "stretch.mode", 3.0);
                const std::string flatPath = (dir / "formant-flat.wav").string();
                check(controller.exportMixdown(flatPath, false).isOk(),
                      "renders in Vocal mode");
                controller.setInsertParameter(track, slot, "formant", 12.0);
                const std::string tiltedPath = (dir / "formant-tilted.wav").string();
                check(controller.exportMixdown(tiltedPath, false).isOk(),
                      "renders again with the formant up");

                audio::platform::DecodedAudio flat;
                audio::platform::DecodedAudio tilted;
                audio::platform::decodeAudioFile(flatPath, flat);
                audio::platform::decodeAudioFile(tiltedPath, tilted);
                double difference = 0.0;
                const std::size_t count =
                    std::min(flat.interleaved.size(), tilted.interleaved.size());
                for (std::size_t i = 0; i < count; ++i) {
                    difference = std::max(difference,
                                          double(std::abs(flat.interleaved[i] -
                                                          tilted.interleaved[i])));
                }
                check(count > 1000 && difference > 0.01,
                      "the formant knob is heard through the controller's own path");
            }

            // Save, reopen, and the sample must still be there — Content owns
            // the portable audio while the state chunk carries its basename.
            const std::string package = (dir / "project.vlt").string();
            check(controller.saveProject(package).isOk(), "the project saves");

            EngineController reopened;
            reopened.initialize(48000, 512, false);
            check(reopened.openProject(package).isOk(), "and reopens");
            const TrackModel* reloaded = reopened.project().tracks.empty()
                                             ? nullptr
                                             : &reopened.project().tracks.front();
            if (check(reloaded != nullptr && reloaded->instrument.isLoaded(),
                      "with its instrument")) {
                sampler::SamplerInstance* instance =
                    reopened.samplerInstance(reloaded->id, reloaded->instrument.id);
                check(instance != nullptr, "which is the sampler again");
                check(instance &&
                          instance->samplePath() ==
                              (fs::path(package) / "Content" /
                               fs::path(wav).filename()).string() &&
                          instance->rawSample(),
                      "still holding its packaged sample");
                check(instance && std::abs(instance->parameterValue(
                                      std::uint32_t(Param::Volume)) - 1.0) < 1e-6,
                      "and its knobs");
            }
        }
        fs::remove_all(dir);
    }

    std::printf(failures == 0 ? "\nALL PASSED\n" : "\n%d FAILURES PRESENT\n", failures);
    return failures == 0 ? 0 : 1;
}
