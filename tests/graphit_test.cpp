#include "Internal/GraphitInstance.hpp"
#include "Internal/InternalFactory.hpp"

#include <algorithm>
#include <array>
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
using namespace daw::plugins::graphit;

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

void set(GraphitInstance& instance, Param parameter, double value) {
    instance.setParameterFromHost(std::uint32_t(parameter), value);
}

struct Block {
    explicit Block(std::uint32_t frames)
        : inLeft(frames), inRight(frames), outLeft(frames), outRight(frames) {
        inputs[0] = inLeft.data(); inputs[1] = inRight.data();
        outputs[0] = outLeft.data(); outputs[1] = outRight.data();
    }
    std::vector<float> inLeft, inRight, outLeft, outRight;
    const float* inputs[2]{};
    float* outputs[2]{};
};

void process(GraphitInstance& instance, Block& block,
             std::span<const PluginEvent> events = {}) {
    PluginProcessContext context;
    context.inputs = block.inputs;
    context.inputChannels = 2;
    context.outputs = block.outputs;
    context.outputChannels = 2;
    context.frames = std::uint32_t(block.inLeft.size());
    context.inputEvents = events;
    instance.process(context);
}

std::vector<float> renderTone(Mode mode, double frequency,
                              double amplitude = 0.25,
                              double amount = 1.0,
                              double priority = 0.0) {
    GraphitInstance instance;
    set(instance, Param::Amount, amount);
    set(instance, Param::Mode, double(mode));
    set(instance, Param::Priority, priority);
    instance.activate({kRate, kBlock, true});
    std::vector<float> result;
    result.reserve(24000);
    std::size_t position = 0;
    while (position < 24000) {
        Block block(kBlock);
        for (std::uint32_t i = 0; i < kBlock; ++i) {
            const float sample = float(amplitude * std::sin(
                2.0 * std::numbers::pi * frequency * double(position + i) / kRate));
            block.inLeft[i] = block.inRight[i] = sample;
        }
        process(instance, block);
        if (position >= 4096)
            result.insert(result.end(), block.outLeft.begin(), block.outLeft.end());
        position += kBlock;
    }
    return result;
}

double rms(const std::vector<float>& audio) {
    double energy = 0.0;
    for (float sample : audio) energy += double(sample) * sample;
    return audio.empty() ? 0.0 : std::sqrt(energy / double(audio.size()));
}

double checksum(Mode mode) {
    GraphitInstance instance;
    set(instance, Param::Amount, 1.0);
    set(instance, Param::Mode, double(mode));
    instance.activate({kRate, kBlock, true});
    double total = 0.0;
    for (int blockIndex = 0; blockIndex < 100; ++blockIndex) {
        Block block(kBlock);
        for (std::uint32_t i = 0; i < kBlock; ++i) {
            const double at = double(blockIndex * int(kBlock) + int(i));
            block.inLeft[i] = float(0.31 * std::sin(at * 0.071) +
                                    0.17 * std::sin(at * 0.193));
            block.inRight[i] = float(0.27 * std::sin(at * 0.067) -
                                     0.12 * std::sin(at * 0.149));
        }
        process(instance, block);
        for (std::uint32_t i = 0; i < kBlock; ++i)
            total += std::abs(block.outLeft[i]) * double(1 + i % 31) +
                     std::abs(block.outRight[i]) * double(1 + i % 17);
    }
    return total;
}

double harmonicEnergy(const std::vector<float>& audio, double fundamental) {
    if (audio.empty()) return 0.0;
    double total = 0.0;
    for (int harmonic = 2; harmonic <= 9; ++harmonic) {
        double real = 0.0;
        double imaginary = 0.0;
        const double frequency = fundamental * harmonic;
        for (std::size_t i = 0; i < audio.size(); ++i) {
            const double phase = 2.0 * std::numbers::pi * frequency *
                                 double(i) / kRate;
            real += double(audio[i]) * std::cos(phase);
            imaginary -= double(audio[i]) * std::sin(phase);
        }
        total += real * real + imaginary * imaginary;
    }
    return total / (double(audio.size()) * double(audio.size()));
}

} // namespace

int main() {
    {
        const PluginDescriptor& descriptor = GraphitInstance::staticDescriptor();
        check(descriptor.uid == "daw.graphit" && descriptor.name == "Graphit" &&
                  descriptor.category.find("Saturation") != std::string::npos &&
                  !descriptor.isInstrument && descriptor.stateSchemaVersion == 2,
              "Graphit publishes a stable built-in effect descriptor");
        const auto builtins = builtinPlugins();
        check(std::ranges::any_of(builtins, [](const PluginDescriptor& item) {
                  return item.uid == "daw.graphit";
              }), "the internal factory publishes Graphit");
        InternalFactory factory;
        check(factory.create(descriptor) != nullptr,
              "the internal factory instantiates Graphit");

        GraphitInstance instance;
        PluginBusLayout accepted;
        check(instance.setBusLayout({{1}, {1}}, accepted) &&
                  instance.setBusLayout({{2}, {2}}, accepted) &&
                  !instance.setBusLayout({{6}, {6}}, accepted) &&
                  !instance.setBusLayout({{2, 1}, {2}}, accepted),
              "Graphit accepts matching mono/stereo layouts without sidechain");
        const auto parameters = instance.parameters();
        check(parameters.size() == 3 && parameters[0].id == "amount" &&
                  parameters[0].index == 0 && parameters[0].defaultValue == 0.35 &&
                  parameters[1].id == "mode" && parameters[1].index == 1 &&
                  parameters[1].defaultValue == 0.0 && parameters[1].isStepped &&
                  parameters[2].id == "priority" && parameters[2].index == 2 &&
                  parameters[2].minValue == -1.0 &&
                  parameters[2].maxValue == 1.0 &&
                  parameters[2].defaultValue == 0.0 &&
                  parameters[2].isAutomatable &&
                  instance.latencySamples() == 0 && instance.tailSamples() == 0,
              "Graphit exposes stable Amount, Mode and Priority parameters");
    }

    {
        GraphitInstance source;
        set(source, Param::Amount, 0.82);
        set(source, Param::Mode, double(Mode::C));
        set(source, Param::Priority, -0.6);
        std::vector<std::uint8_t> state;
        check(source.saveState(state), "Graphit state saves");
        GraphitInstance restored;
        check(restored.loadState(state) &&
                  std::abs(restored.parameterValue(std::uint32_t(Param::Amount)) -
                           0.82) < 1.0e-9 &&
                  restored.parameterValue(std::uint32_t(Param::Mode)) ==
                      double(Mode::C) &&
                  restored.parameterValue(std::uint32_t(Param::Priority)) == -0.6,
              "Graphit state restores Amount, Mode and Priority");
        const std::string future =
            R"({"version":99,"params":{"amount":5,"mode":99,"priority":5,"future":3}})";
        check(restored.loadState(std::span<const std::uint8_t>(
                  reinterpret_cast<const std::uint8_t*>(future.data()),
                  future.size())) &&
                  restored.parameterValue(std::uint32_t(Param::Amount)) == 1.0 &&
                  restored.parameterValue(std::uint32_t(Param::Mode)) == 4.0 &&
                  restored.parameterValue(std::uint32_t(Param::Priority)) == 1.0,
              "future state fields are ignored and known values are clamped");
        const std::string versionOne =
            R"({"version":1,"params":{"amount":0.4,"mode":1}})";
        check(restored.loadState(std::span<const std::uint8_t>(
                  reinterpret_cast<const std::uint8_t*>(versionOne.data()),
                  versionOne.size())) &&
                  restored.parameterValue(std::uint32_t(Param::Priority)) == 0.0,
              "v1 Graphit state loads with centred Priority");
    }

    {
        GraphitInstance dry;
        set(dry, Param::Amount, 0.0);
        dry.activate({kRate, kBlock, true});
        Block block(kBlock);
        for (std::uint32_t i = 0; i < kBlock; ++i) {
            block.inLeft[i] = float(i) / float(kBlock) - 0.5f;
            block.inRight[i] = -block.inLeft[i];
        }
        process(dry, block);
        check(block.inLeft == block.outLeft && block.inRight == block.outRight,
              "Amount zero is an exact dry pass-through");

        gAllocations.store(0, std::memory_order_relaxed);
        gCountAllocations = true;
        process(dry, block);
        gCountAllocations = false;
        check(gAllocations.load(std::memory_order_relaxed) == 0,
              "Graphit processing allocates nothing on the realtime thread");
    }

    {
        std::set<long long> signatures;
        for (int mode = 0; mode < 5; ++mode)
            signatures.insert(std::llround(checksum(Mode(mode))));
        check(signatures.size() == 5,
              "all five Graphit profiles produce distinct output");

        const double airLow = rms(renderTone(Mode::A, 1000.0, 0.04));
        const double airHigh = rms(renderTone(Mode::A, 10000.0, 0.04));
        const double bodyLow = rms(renderTone(Mode::B, 120.0, 0.04));
        const double bodyMid = rms(renderTone(Mode::B, 1000.0, 0.04));
        check(airHigh > airLow * 1.25 && bodyLow > bodyMid * 1.20,
              "Air and Body lift their intended frequency ranges");

        const double punchPresence = rms(renderTone(Mode::P, 3200.0, 0.04));
        const double punchNeutral = rms(renderTone(Mode::P, 1000.0, 0.04));
        const double crunchMid = rms(renderTone(Mode::C, 1400.0, 0.025));
        const double crunchLow = rms(renderTone(Mode::C, 300.0, 0.025));
        const double extremeMid = rms(renderTone(Mode::X, 2800.0, 0.01));
        const double extremeBox = rms(renderTone(Mode::X, 450.0, 0.01));
        const bool targetBands = punchPresence > punchNeutral * 1.20 &&
            crunchMid > crunchLow * 1.12 &&
            extremeMid > extremeBox * 1.20;
        check(targetBands,
              "Punch, Crunch and Extreme emphasize their target bands");

        const double lowPriorityBass =
            rms(renderTone(Mode::A, 100.0, 0.025, 1.0, -1.0));
        const double highPriorityBass =
            rms(renderTone(Mode::A, 100.0, 0.025, 1.0, 1.0));
        const double lowPriorityAir =
            rms(renderTone(Mode::A, 9000.0, 0.025, 1.0, -1.0));
        const double highPriorityAir =
            rms(renderTone(Mode::A, 9000.0, 0.025, 1.0, 1.0));
        check(lowPriorityBass > highPriorityBass * 1.35 &&
                  highPriorityAir > lowPriorityAir * 1.35,
              "Priority tilts Graphit toward lows or highs around a neutral centre");

        const auto soft = renderTone(Mode::A, 1000.0, 0.35);
        const auto crunch = renderTone(Mode::C, 1000.0, 0.35);
        const auto extreme = renderTone(Mode::X, 1000.0, 0.35);
        const double softHarmonics = harmonicEnergy(soft, 1000.0);
        check(harmonicEnergy(crunch, 1000.0) > softHarmonics * 1.15 &&
                  harmonicEnergy(extreme, 1000.0) > softHarmonics * 1.4,
              "Crunch and Extreme generate materially more harmonics than Air");
    }

    {
        GraphitInstance automated;
        set(automated, Param::Amount, 0.0);
        automated.activate({kRate, kBlock, true});
        Block block(kBlock);
        std::fill(block.inLeft.begin(), block.inLeft.end(), 0.5f);
        std::fill(block.inRight.begin(), block.inRight.end(), 0.5f);
        PluginEvent event;
        event.kind = PluginEvent::Kind::ParamValue;
        event.paramIndex = std::uint32_t(Param::Amount);
        event.frameOffset = kBlock / 2;
        event.value = 1.0;
        process(automated, block, std::span<const PluginEvent>(&event, 1));
        const bool firstDry = std::equal(block.outLeft.begin(),
                                         block.outLeft.begin() + kBlock / 2,
                                         block.inLeft.begin());
        const float changed = *std::max_element(
            block.outLeft.begin() + kBlock / 2, block.outLeft.end(),
            [](float left, float right) {
                return std::abs(left - 0.5f) < std::abs(right - 0.5f);
            });
        check(firstDry && std::abs(changed - 0.5f) > 1.0e-4f,
              "sample-offset Amount automation leaves only the first slice dry");
    }

    {
        GraphitInstance stressed;
        set(stressed, Param::Amount, 1.0);
        set(stressed, Param::Mode, double(Mode::X));
        stressed.activate({kRate, kBlock, true});
        bool finite = true;
        float maximum = 0.0f;
        float maximumJump = 0.0f;
        float previous = 0.0f;
        for (int blockIndex = 0; blockIndex < 160; ++blockIndex) {
            if (blockIndex < 30)
                set(stressed, Param::Mode, double(blockIndex % 5));
            else if (blockIndex == 30)
                set(stressed, Param::Mode, double(Mode::X));
            Block block(kBlock);
            for (std::uint32_t i = 0; i < kBlock; ++i) {
                const float sample = float(0.95 * std::sin(
                    double(blockIndex * int(kBlock) + int(i)) * 0.13));
                block.inLeft[i] = sample;
                block.inRight[i] = -sample * 0.8f;
            }
            if (blockIndex == 12) {
                block.inRight[13] = std::numeric_limits<float>::quiet_NaN();
                block.inRight[29] = std::numeric_limits<float>::infinity();
            }
            process(stressed, block);
            for (float value : block.outLeft) {
                finite = finite && std::isfinite(value);
                maximum = std::max(maximum, std::abs(value));
                maximumJump = std::max(maximumJump, std::abs(value - previous));
                previous = value;
            }
            for (float value : block.outRight) {
                finite = finite && std::isfinite(value);
                maximum = std::max(maximum, std::abs(value));
            }
        }
        const Telemetry telemetry = stressed.consumeTelemetry();
        GraphitInstance fixed;
        set(fixed, Param::Amount, 1.0);
        set(fixed, Param::Mode, double(Mode::X));
        fixed.activate({kRate, kBlock, true});
        float fixedMaximumJump = 0.0f;
        float fixedPrevious = 0.0f;
        for (int blockIndex = 0; blockIndex < 160; ++blockIndex) {
            Block block(kBlock);
            for (std::uint32_t i = 0; i < kBlock; ++i) {
                const float sample = float(0.95 * std::sin(
                    double(blockIndex * int(kBlock) + int(i)) * 0.13));
                block.inLeft[i] = sample;
                block.inRight[i] = -sample * 0.8f;
            }
            process(fixed, block);
            for (float value : block.outLeft) {
                fixedMaximumJump = std::max(
                    fixedMaximumJump, std::abs(value - fixedPrevious));
                fixedPrevious = value;
            }
        }
        check(finite && maximum <= 4.0f,
              "Graphit rejects invalid samples and remains bounded");
        check(maximumJump <= fixedMaximumJump * 1.25f + 0.05f,
              "rapid mode changes remain click-safe");
        check(telemetry.inputLeft > 0.5f && telemetry.outputLeft > 0.1f &&
                  telemetry.gainReductionDb > 1.0f,
              "Graphit reports compression telemetry");
    }

    std::printf("%s\n", failures ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return failures ? 1 : 0;
}
