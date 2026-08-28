#include "Internal/GravityInstance.hpp"
#include "Internal/InternalFactory.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <numbers>
#include <set>
#include <string>
#include <vector>

using namespace daw;
using namespace daw::plugins;
using namespace daw::plugins::gravity;

namespace {
thread_local bool gCountAllocations = false;
std::atomic<std::uint64_t> gAllocations{0};
}

void* operator new(std::size_t size) {
    if (gCountAllocations) gAllocations.fetch_add(1, std::memory_order_relaxed);
    if (void* memory = std::malloc(std::max<std::size_t>(1, size))) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace {

constexpr double kRate = 48000.0;
constexpr std::uint32_t kBlock = 256;
int failures = 0;

bool check(bool condition, const char* message) {
    std::printf("%s  %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition) ++failures;
    return condition;
}

void set(GravityInstance& instance, Param parameter, double value) {
    instance.setParameterFromHost(std::uint32_t(parameter), value);
}

struct Block {
    explicit Block(std::uint32_t frames)
        : inLeft(frames), inRight(frames), sideLeft(frames), sideRight(frames),
          outLeft(frames), outRight(frames) {
        inputs[0] = inLeft.data(); inputs[1] = inRight.data();
        sidechain[0] = sideLeft.data(); sidechain[1] = sideRight.data();
        outputs[0] = outLeft.data(); outputs[1] = outRight.data();
    }
    std::vector<float> inLeft, inRight, sideLeft, sideRight, outLeft, outRight;
    const float* inputs[2]{};
    const float* sidechain[2]{};
    float* outputs[2]{};
};

PluginProcessDisposition process(GravityInstance& instance, Block& block,
                                 double tempo = 120.0,
                                 bool withSidechain = false) {
    PluginProcessContext context;
    context.inputs = block.inputs;
    context.inputChannels = 2;
    if (withSidechain) {
        context.sidechainInputs = block.sidechain;
        context.sidechainInputChannels = 2;
    }
    context.outputs = block.outputs;
    context.outputChannels = 2;
    context.frames = std::uint32_t(block.inLeft.size());
    context.transport.tempo = tempo;
    return instance.process(context);
}

double featureChecksum(std::initializer_list<std::pair<Param, double>> values,
                       bool stereoSource = false) {
    GravityInstance instance;
    set(instance, Param::Gravity, 1.0);
    set(instance, Param::Feedback, 0.58);
    set(instance, Param::Decay, 5.0);
    set(instance, Param::Size, 0.48);
    set(instance, Param::TimingSync, 0.0);
    set(instance, Param::TimingMs, 45.0);
    for (const auto& [parameter, value] : values) set(instance, parameter, value);
    instance.activate({kRate, kBlock, true});
    double checksum = 0.0;
    for (int blockIndex = 0; blockIndex < 150; ++blockIndex) {
        Block block(kBlock);
        for (std::uint32_t i = 0; i < kBlock; ++i) {
            const std::size_t at = std::size_t(blockIndex) * kBlock + i;
            const float pulse = at % 2400 < 20 ? 0.65f : 0.0f;
            block.inLeft[i] = pulse;
            block.inRight[i] = stereoSource ? -pulse * 0.55f : pulse;
        }
        process(instance, block);
        for (std::uint32_t i = 0; i < kBlock; ++i) {
            const double weight = 1.0 + double((blockIndex * int(kBlock) + int(i)) % 61);
            checksum += (std::abs(block.outLeft[i]) + 1.37 * std::abs(block.outRight[i])) * weight;
        }
    }
    return checksum;
}

std::vector<float> renderTone(Algorithm algorithm, double pitch,
                              double seconds = 0.35,
                              PitchSnap snap = PitchSnap::Off) {
    GravityInstance instance;
    set(instance, Param::Gravity, 1.0);
    set(instance, Param::Pitch, pitch);
    set(instance, Param::Feedback, 0.0);
    set(instance, Param::Size, 0.18);
    set(instance, Param::Algorithm, double(algorithm));
    set(instance, Param::PitchSnap, double(snap));
    set(instance, Param::TimingSync, 0.0);
    set(instance, Param::TimingMs, 100.0);
    instance.activate({kRate, kBlock, true});
    instance.startProcessing();

    const std::size_t total = std::size_t(seconds * kRate);
    std::vector<float> rendered;
    rendered.reserve(total);
    std::size_t position = 0;
    while (position < total) {
        const std::uint32_t count = std::uint32_t(std::min<std::size_t>(kBlock, total - position));
        Block block(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            const float sample = float(std::sin(2.0 * std::numbers::pi * 440.0 *
                                                double(position + i) / kRate));
            block.inLeft[i] = block.inRight[i] = sample;
        }
        process(instance, block);
        rendered.insert(rendered.end(), block.outLeft.begin(), block.outLeft.end());
        position += count;
    }
    return rendered;
}

std::pair<std::vector<float>, std::vector<float>> renderSpreadTone(double spread) {
    GravityInstance instance;
    set(instance, Param::Gravity, 1.0);
    set(instance, Param::Feedback, 0.0);
    set(instance, Param::Size, 0.14);
    set(instance, Param::Density, 0.75);
    set(instance, Param::PitchSpread, spread);
    set(instance, Param::Algorithm, double(Algorithm::Orbit));
    set(instance, Param::TimingSync, 0.0);
    set(instance, Param::TimingMs, 70.0);
    instance.activate({kRate, kBlock, true});
    std::pair<std::vector<float>, std::vector<float>> rendered;
    const std::size_t total = std::size_t(0.40 * kRate);
    rendered.first.reserve(total);
    rendered.second.reserve(total);
    std::size_t position = 0;
    while (position < total) {
        const std::uint32_t count = std::uint32_t(
            std::min<std::size_t>(kBlock, total - position));
        Block block(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            const float sample = float(std::sin(2.0 * std::numbers::pi * 440.0 *
                                                double(position + i) / kRate));
            block.inLeft[i] = block.inRight[i] = sample;
        }
        process(instance, block);
        rendered.first.insert(rendered.first.end(), block.outLeft.begin(), block.outLeft.end());
        rendered.second.insert(rendered.second.end(), block.outRight.begin(), block.outRight.end());
        position += count;
    }
    return rendered;
}

double zeroCrossingFrequency(const std::vector<float>& audio,
                             double fromSeconds, double toSeconds) {
    const std::size_t from = std::min(audio.size(), std::size_t(fromSeconds * kRate));
    const std::size_t to = std::min(audio.size(), std::size_t(toSeconds * kRate));
    int crossings = 0;
    for (std::size_t i = from + 1; i < to; ++i)
        if (audio[i - 1] <= 0.0f && audio[i] > 0.0f) ++crossings;
    return to > from ? double(crossings) * kRate / double(to - from) : 0.0;
}

double algorithmChecksum(Algorithm algorithm) {
    GravityInstance instance;
    set(instance, Param::Gravity, 1.0);
    set(instance, Param::Feedback, 0.75);
    set(instance, Param::Size, 0.55);
    set(instance, Param::Algorithm, double(algorithm));
    set(instance, Param::TimingSync, 0.0);
    set(instance, Param::TimingMs, 60.0);
    instance.activate({kRate, kBlock, true});
    double checksum = 0.0;
    for (int blockIndex = 0; blockIndex < 120; ++blockIndex) {
        Block block(kBlock);
        if (blockIndex == 0) block.inLeft[0] = block.inRight[0] = 1.0f;
        process(instance, block);
        for (std::uint32_t i = 0; i < kBlock; ++i)
            checksum += std::abs(block.outLeft[i]) * double(1 + ((blockIndex * kBlock + i) % 97));
    }
    return checksum;
}

} // namespace

int main() {
    {
        const PluginDescriptor& descriptor = GravityInstance::staticDescriptor();
        check(descriptor.uid == "daw.gravity" && !descriptor.isInstrument &&
                  descriptor.mainInputChannels == 2 && descriptor.mainOutputChannels == 2,
              "Gravity is a stereo built-in effect with a stable uid");
        const auto builtins = builtinPlugins();
        check(std::any_of(builtins.begin(), builtins.end(), [](const PluginDescriptor& d) {
                  return d.uid == "daw.gravity";
              }), "the internal factory publishes Gravity");

        GravityInstance instance;
        PluginBusLayout accepted;
        check(instance.setBusLayout({{1}, {1}}, accepted) &&
                  accepted.inputs == std::vector<std::uint16_t>{1} &&
                  accepted.outputs == std::vector<std::uint16_t>{1},
              "mono layout is accepted");
        check(instance.setBusLayout({{2}, {2}}, accepted) &&
                  !instance.setBusLayout({{6}, {6}}, accepted),
              "stereo is accepted and surround is rejected");
        check(instance.setBusLayout({{2, 1}, {2}}, accepted) &&
                  accepted.inputs == std::vector<std::uint16_t>({2, 1}) &&
                  !instance.setBusLayout({{2, 6}, {2}}, accepted) &&
                  !instance.setBusLayout({{2, 1, 1}, {2}}, accepted) &&
                  !instance.setBusLayout({{2}, {2, 2}}, accepted),
              "optional mono/stereo sidechain layout is exposed");

        std::set<std::string> ids;
        for (const ParameterInfo& info : instance.parameters()) ids.insert(info.id);
        check(ids == std::set<std::string>{
                  "algorithm", "damping", "decay", "density", "detector.source",
                  "diffusion", "drive", "ducking", "feedback", "feedback.lowcut",
                  "gravity", "mass", "motion", "pitch", "pitch.snap", "pitch.spread",
                  "reverse", "size", "stereo.input", "stereo.width", "timing.division",
                  "timing.ms", "timing.sync", "transient"},
              "the public parameter ids are complete and stable");
        static constexpr std::array<std::string_view, 9> legacyIds{
            "gravity", "pitch", "feedback", "decay", "size", "algorithm",
            "timing.sync", "timing.division", "timing.ms"};
        bool stableLegacyIndices = instance.parameters().size() == kParameterCount;
        for (std::uint32_t i = 0; i < legacyIds.size(); ++i)
            stableLegacyIndices = stableLegacyIndices &&
                instance.parameters()[i].id == legacyIds[i] &&
                instance.parameters()[i].index == i;
        check(stableLegacyIndices, "Gravity v1 parameter ids keep their original indices");
        check(int(Division::Sixteenth) == 0 &&
                  int(Division::EighthTriplet) == 1 &&
                  int(Division::Eighth) == 2 &&
                  int(Division::EighthDotted) == 3 &&
                  int(Division::Quarter) == 4 &&
                  int(Division::ThirtySecond) == 5 &&
                  int(Division::Whole) == 10,
              "legacy sync division values stay stable and new values append");
        bool legacyPresetsNeutral = true;
        const auto parameters = parameterTable();
        for (std::size_t preset = 0; preset < 8; ++preset) {
            for (std::uint32_t parameter = 9; parameter < kParameterCount; ++parameter) {
                legacyPresetsNeutral = legacyPresetsNeutral &&
                    factoryPresets()[preset].values[parameter] ==
                        parameters[parameter].defaultValue;
            }
        }
        check(legacyPresetsNeutral,
              "legacy factory presets keep neutral Gravity 2 parameters");
        check(factoryPresets().size() == 16 &&
                  factoryPresets()[8].name == "PIANO BLOOM" &&
                  factoryPresets()[15].name == "BINARY STARS",
              "eight musical Gravity 2 factory presets are appended");
    }

    {
        GravityInstance source;
        set(source, Param::Pitch, 7.5);
        set(source, Param::Decay, 12.0);
        set(source, Param::Mass, 0.8);
        source.setPresetReference("user", "My Cloud");
        std::vector<std::uint8_t> state;
        check(source.saveState(state), "state saves");
        GravityInstance restored;
        check(restored.loadState(state) &&
                  std::abs(restored.parameterValue(std::uint32_t(Param::Pitch)) - 7.5) < 1e-9 &&
                  std::abs(restored.parameterValue(std::uint32_t(Param::Mass)) - 0.8) < 1e-9 &&
                  restored.presetReference() == std::pair<std::string, std::string>{"user", "My Cloud"} &&
                  !restored.frozen(),
              "state v2 restores advanced parameters and preset reference but never Freeze");

        set(restored, Param::Mass, 0.9);
        const std::string legacy =
            R"({"version":1,"preset":4,"params":{"gravity":0.4,"pitch":3}})";
        check(restored.loadState(std::span<const std::uint8_t>(
                  reinterpret_cast<const std::uint8_t*>(legacy.data()), legacy.size())) &&
                  restored.lastPreset() == 4 &&
                  restored.parameterValue(std::uint32_t(Param::Mass)) == 0.5 &&
                  restored.parameterValue(std::uint32_t(Param::DetectorSource)) ==
                      double(DetectorSource::Auto),
              "state v1 migration resets missing Gravity 2 parameters to neutral defaults");

        const std::string future =
            R"({"version":99,"preset":99,"params":{"gravity":5,"pitch":-99,"future.knob":42}})";
        check(restored.loadState(std::span<const std::uint8_t>(
                  reinterpret_cast<const std::uint8_t*>(future.data()), future.size())) &&
                  restored.parameterValue(std::uint32_t(Param::Gravity)) == 1.0 &&
                  restored.parameterValue(std::uint32_t(Param::Pitch)) == -12.0,
              "future fields are ignored and known values are clamped");
    }

    {
        GravityInstance dry;
        set(dry, Param::Gravity, 0.0);
        dry.activate({kRate, kBlock, true});
        Block block(kBlock);
        for (std::uint32_t i = 0; i < kBlock; ++i)
            block.inLeft[i] = block.inRight[i] = float(i) / float(kBlock);
        process(dry, block);
        check(block.outLeft == block.inLeft && block.outRight == block.inRight,
              "Gravity zero is an exact dry pass-through");

        gAllocations.store(0, std::memory_order_relaxed);
        gCountAllocations = true;
        process(dry, block);
        gCountAllocations = false;
        check(gAllocations.load(std::memory_order_relaxed) == 0,
              "processing allocates and frees nothing on the realtime thread");
    }

    {
        GravityInstance delay;
        set(delay, Param::Gravity, 1.0);
        set(delay, Param::Feedback, 0.0);
        set(delay, Param::Size, 0.12);
        set(delay, Param::TimingSync, 1.0);
        set(delay, Param::TimingDivision, double(Division::Eighth));
        delay.activate({kRate, kBlock, true});
        std::size_t first = std::numeric_limits<std::size_t>::max();
        std::size_t rendered = 0;
        for (int b = 0; b < 80; ++b) {
            Block block(kBlock);
            if (b == 0) block.inLeft[0] = block.inRight[0] = 1.0f;
            process(delay, block, 120.0);
            for (std::size_t i = 0; i < block.outLeft.size(); ++i) {
                if (first == std::numeric_limits<std::size_t>::max() &&
                    std::abs(block.outLeft[i]) > 1.0e-5f) first = rendered + i;
            }
            rendered += kBlock;
        }
        check(first >= 11000 && first <= 13000,
              "1/8 sync at 120 BPM starts near 12000 samples");

        GravityInstance freeDelay;
        set(freeDelay, Param::Gravity, 1.0);
        set(freeDelay, Param::Feedback, 0.0);
        set(freeDelay, Param::Size, 0.12);
        set(freeDelay, Param::TimingSync, 0.0);
        set(freeDelay, Param::TimingMs, 50.0);
        freeDelay.activate({kRate, kBlock, true});
        first = std::numeric_limits<std::size_t>::max();
        rendered = 0;
        for (int b = 0; b < 20; ++b) {
            Block block(kBlock);
            if (b == 0) block.inLeft[0] = block.inRight[0] = 1.0f;
            process(freeDelay, block);
            for (std::size_t i = 0; i < block.outLeft.size(); ++i) {
                if (first == std::numeric_limits<std::size_t>::max() &&
                    std::abs(block.outLeft[i]) > 1.0e-5f) first = rendered + i;
            }
            rendered += kBlock;
        }
        check(first >= 2200 && first <= 2700,
              "50 ms free timing starts near 2400 samples");
    }

    {
        const double base = zeroCrossingFrequency(renderTone(Algorithm::Orbit, 0.0), 0.12, 0.18);
        const double octaveDown = zeroCrossingFrequency(renderTone(Algorithm::Orbit, -12.0), 0.12, 0.18);
        const double octave = zeroCrossingFrequency(renderTone(Algorithm::Orbit, 12.0), 0.12, 0.18);
        const double fall = zeroCrossingFrequency(renderTone(Algorithm::Fall, 0.0), 0.24, 0.32);
        const double rise = zeroCrossingFrequency(renderTone(Algorithm::Rise, 0.0), 0.24, 0.32);
        check(base > 300.0 && octave > base * 1.65 && octaveDown < base * 0.68,
              "pitch ±12 st moves rendered grains by roughly an octave");
        check(rise > fall * 1.15,
              "Rise trajectories are audibly higher than Fall trajectories");

        const double unsnapped = zeroCrossingFrequency(
            renderTone(Algorithm::Orbit, 5.0, 0.35, PitchSnap::Off), 0.12, 0.30);
        const double perfect = zeroCrossingFrequency(
            renderTone(Algorithm::Orbit, 5.0, 0.35, PitchSnap::Perfect), 0.12, 0.30);
        const double octaveSnap = zeroCrossingFrequency(
            renderTone(Algorithm::Orbit, 5.0, 0.35, PitchSnap::Octave), 0.12, 0.30);
        check(perfect > unsnapped * 1.07 && octaveSnap < unsnapped * 0.80,
              "Pitch Snap selects perfect intervals and octaves");

        const auto spread = renderSpreadTone(12.0);
        const double spreadLeft = zeroCrossingFrequency(spread.first, 0.13, 0.31);
        const double spreadRight = zeroCrossingFrequency(spread.second, 0.13, 0.31);
        check(spreadRight > spreadLeft * 1.35,
              "linked A/B attractors create opposite pitch populations");
    }

    {
        std::set<long long> signatures;
        for (int algorithm = 0; algorithm < 6; ++algorithm)
            signatures.insert(std::llround(algorithmChecksum(Algorithm(algorithm)) * 1000.0));
        check(signatures.size() == 6,
              "all six algorithms produce distinct deterministic output");
    }

    {
        const double neutral = featureChecksum({});
        const double mass = featureChecksum({{Param::Mass, 0.9}});
        const double motion = featureChecksum({{Param::Motion, 0.8}});
        const double density = featureChecksum({{Param::Density, 0.9}});
        const double space = featureChecksum({{Param::Diffusion, 0.9}});
        const double reverse = featureChecksum({{Param::Reverse, 1.0}});
        const double drive = featureChecksum({{Param::Drive, 0.8}});
        std::set<long long> signatures{
            std::llround(neutral), std::llround(mass), std::llround(motion),
            std::llround(density), std::llround(space), std::llround(reverse),
            std::llround(drive)};
        check(signatures.size() == 7,
              "Mass, Motion, Density, Diffusion, Reverse and Drive are sonically distinct");

        const double collapsedStereo = featureChecksum(
            {{Param::StereoInput, 0.0}}, true);
        const double preservedStereo = featureChecksum(
            {{Param::StereoInput, 1.0}}, true);
        check(std::abs(collapsedStereo - preservedStereo) > neutral * 0.03,
              "Source Stereo preserves information that legacy mono grains collapse");

        GravityInstance monoWidth;
        set(monoWidth, Param::Gravity, 1.0);
        set(monoWidth, Param::StereoWidth, 0.0);
        set(monoWidth, Param::StereoInput, 1.0);
        set(monoWidth, Param::TimingSync, 0.0);
        set(monoWidth, Param::TimingMs, 30.0);
        monoWidth.activate({kRate, kBlock, true});
        float difference = 0.0f;
        for (int b = 0; b < 40; ++b) {
            Block block(kBlock);
            for (std::uint32_t i = 0; i < kBlock; ++i) {
                block.inLeft[i] = b == 0 && i < 32 ? 0.8f : 0.0f;
                block.inRight[i] = b == 0 && i < 32 ? -0.3f : 0.0f;
            }
            process(monoWidth, block);
            for (std::uint32_t i = 0; i < kBlock; ++i)
                difference = std::max(difference,
                    std::abs(block.outLeft[i] - block.outRight[i]));
        }
        check(difference < 1.0e-5f, "Stereo Width zero collapses the wet field to mono");

        const double lowcutOff = featureChecksum({{Param::FeedbackLowcut, 0.0}});
        const double lowcutOn = featureChecksum({{Param::FeedbackLowcut, 1.0}});
        const double bright = featureChecksum({{Param::Damping, 0.0}});
        const double dark = featureChecksum({{Param::Damping, 1.0}});
        check(std::abs(lowcutOff - lowcutOn) > neutral * 0.01 &&
                  std::abs(bright - dark) > neutral * 0.01,
              "feedback Low Cut and Damping reshape the tail");

        const double unducked = featureChecksum({{Param::Ducking, 0.0}});
        const double ducked = featureChecksum({{Param::Ducking, 1.0}});
        check(ducked < unducked * 0.9, "Ducking preserves the attack by lowering wet energy");
    }

    {
        auto transientPulse = [](DetectorSource source, bool mainPulse,
                                 bool sidechainPulse) {
            GravityInstance instance;
            set(instance, Param::Gravity, 1.0);
            set(instance, Param::Transient, 1.0);
            set(instance, Param::DetectorSource, double(source));
            instance.activate({kRate, kBlock, true});
            Block block(kBlock);
            for (int i = 0; i < 64; ++i) {
                if (mainPulse) block.inLeft[std::size_t(i)] = block.inRight[std::size_t(i)] = 1.0f;
                if (sidechainPulse)
                    block.sideLeft[std::size_t(i)] = block.sideRight[std::size_t(i)] = 1.0f;
            }
            process(instance, block, 120.0, sidechainPulse);
            return instance.consumeTelemetry();
        };
        const Telemetry mainIgnored = transientPulse(DetectorSource::Main, false, true);
        const Telemetry sideTriggered = transientPulse(DetectorSource::Sidechain, false, true);
        const Telemetry autoTriggered = transientPulse(DetectorSource::Auto, false, true);
        const Telemetry autoFallback = transientPulse(DetectorSource::Auto, true, false);
        check(mainIgnored.transientPulse < 0.01f &&
                  sideTriggered.transientPulse > 0.2f &&
                  autoTriggered.transientPulse > 0.2f &&
                  autoFallback.transientPulse > 0.2f &&
                  sideTriggered.grainSerial >= 5,
              "Transient detector follows Main, Sidechain and Auto sources");

        GravityInstance automated;
        set(automated, Param::Gravity, 0.0);
        automated.activate({kRate, kBlock, true});
        Block block(kBlock);
        std::fill(block.inLeft.begin(), block.inLeft.end(), 0.5f);
        std::fill(block.inRight.begin(), block.inRight.end(), 0.5f);
        PluginEvent event;
        event.kind = PluginEvent::Kind::ParamValue;
        event.paramIndex = std::uint32_t(Param::Gravity);
        event.frameOffset = kBlock / 2;
        event.value = 1.0;
        PluginProcessContext context;
        context.inputs = block.inputs;
        context.inputChannels = 2;
        context.outputs = block.outputs;
        context.outputChannels = 2;
        context.frames = kBlock;
        context.inputEvents = std::span<const PluginEvent>(&event, 1);
        automated.process(context);
        const bool firstHalfDry = std::equal(block.outLeft.begin(),
                                             block.outLeft.begin() + kBlock / 2,
                                             block.inLeft.begin());
        const bool secondHalfChanged = !std::equal(block.outLeft.begin() + kBlock / 2,
                                                   block.outLeft.end(),
                                                   block.inLeft.begin() + kBlock / 2);
        check(firstHalfDry && secondHalfChanged,
              "Gravity 2 parameter events remain sample-accurate within a block");
    }

    {
        GravityInstance instance;
        set(instance, Param::Gravity, 1.0);
        set(instance, Param::Feedback, 1.0);
        set(instance, Param::Decay, 20.0);
        set(instance, Param::Size, 1.0);
        set(instance, Param::Mass, 1.0);
        set(instance, Param::Motion, 1.0);
        set(instance, Param::Density, 1.0);
        set(instance, Param::Diffusion, 1.0);
        set(instance, Param::Damping, 1.0);
        set(instance, Param::Reverse, 1.0);
        set(instance, Param::StereoWidth, 1.0);
        set(instance, Param::StereoInput, 1.0);
        set(instance, Param::Ducking, 1.0);
        set(instance, Param::Transient, 1.0);
        set(instance, Param::Drive, 1.0);
        set(instance, Param::PitchSpread, 12.0);
        set(instance, Param::FeedbackLowcut, 1.0);
        set(instance, Param::TimingSync, 0.0);
        set(instance, Param::TimingMs, 40.0);
        instance.activate({kRate, kBlock, true});
        check(instance.tailSamples() == std::uint32_t(22.5 * kRate),
              "reported tail covers Decay, maximum delay and grain reserve");
        bool finite = true;
        float maximum = 0.0f;
        for (int b = 0; b < 300; ++b) {
            Block block(kBlock);
            if (b == 0) block.inLeft[0] = block.inRight[0] = 1.0f;
            process(instance, block);
            for (float value : block.outLeft) {
                finite = finite && std::isfinite(value);
                maximum = std::max(maximum, std::abs(value));
            }
        }
        check(finite && maximum < 32.0f,
              "maximum feedback remains finite and bounded");

        Block realtime(kBlock);
        std::fill(realtime.inLeft.begin(), realtime.inLeft.end(), 0.7f);
        std::fill(realtime.inRight.begin(), realtime.inRight.end(), -0.4f);
        gAllocations.store(0, std::memory_order_relaxed);
        gCountAllocations = true;
        process(instance, realtime);
        gCountAllocations = false;
        check(gAllocations.load(std::memory_order_relaxed) == 0,
              "maximum Gravity 2 processing stays allocation-free");

        instance.setFrozen(true);
        Block frozen(kBlock);
        std::fill(frozen.inLeft.begin(), frozen.inLeft.end(), 0.9f);
        std::fill(frozen.inRight.begin(), frozen.inRight.end(), 0.9f);
        const PluginProcessDisposition frozenDisposition = process(instance, frozen);
        check(frozenDisposition == PluginProcessDisposition::Continue &&
                  *std::max_element(frozen.outLeft.begin(), frozen.outLeft.end()) < 0.9f,
              "Freeze ignores fresh input while the wet field continues");
        instance.clearTail();
        Block cleared(kBlock);
        process(instance, cleared);
        const float clearPeak = *std::max_element(cleared.outLeft.begin(), cleared.outLeft.end(),
                                                  [](float a, float b) {
                                                      return std::abs(a) < std::abs(b);
                                                  });
        check(std::abs(clearPeak) < 1.0e-6f, "Clear removes grains, feedback and diffusion");

        instance.reset();
        Block resetBlock(kBlock);
        process(instance, resetBlock);
        check(!instance.frozen() && std::ranges::all_of(
                  resetBlock.outLeft, [](float sample) {
                      return std::abs(sample) < 1.0e-7f;
                  }),
              "reset clears runtime DSP state and releases Freeze");
    }

    std::printf("%s\n", failures ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return failures ? 1 : 0;
}
