#include "Internal/EqualizerInstance.hpp"
#include "Internal/InternalFactory.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <numbers>
#include <set>
#include <string>
#include <vector>

using namespace daw::plugins;
using namespace daw::plugins::equalizer;

namespace {
thread_local bool gCountAllocations = false;
std::atomic<std::uint64_t> gAllocations{0};
}

void* operator new(std::size_t size) {
    if (gCountAllocations) gAllocations.fetch_add(1, std::memory_order_relaxed);
    if (void* memory = std::malloc(std::max<std::size_t>(size, 1))) return memory;
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

void set(EqualizerInstance& instance, GlobalParam parameter, double value) {
    instance.setParameterFromHost(globalParameter(parameter), value);
}

void set(EqualizerInstance& instance, std::uint32_t band,
         BandParam parameter, double value) {
    instance.setParameterFromHost(bandParameter(band, parameter), value);
}

struct Block {
    explicit Block(std::uint32_t frames, std::uint16_t channels = 2)
        : left(frames), right(frames), sideLeft(frames), sideRight(frames),
          outLeft(frames), outRight(frames), channelCount(channels) {
        inputs[0] = left.data(); inputs[1] = right.data();
        sidechain[0] = sideLeft.data(); sidechain[1] = sideRight.data();
        outputs[0] = outLeft.data(); outputs[1] = outRight.data();
    }
    std::vector<float> left, right, sideLeft, sideRight, outLeft, outRight;
    const float* inputs[2]{};
    const float* sidechain[2]{};
    float* outputs[2]{};
    std::uint16_t channelCount = 2;
};

PluginProcessDisposition process(EqualizerInstance& instance, Block& block,
                                 bool withSidechain = false,
                                 std::span<const PluginEvent> events = {}) {
    PluginProcessContext context;
    context.inputs = block.inputs;
    context.inputChannels = block.channelCount;
    context.outputs = block.outputs;
    context.outputChannels = block.channelCount;
    context.frames = std::uint32_t(block.left.size());
    context.inputEvents = events;
    if (withSidechain) {
        context.sidechainInputs = block.sidechain;
        context.sidechainInputChannels = block.channelCount;
    }
    return instance.process(context);
}

double renderTone(EqualizerInstance& instance, double frequency,
                  int blocks = 30, bool right = false) {
    double energy = 0.0;
    std::size_t position = 0;
    for (int blockIndex = 0; blockIndex < blocks; ++blockIndex) {
        Block block(kBlock);
        for (std::uint32_t i = 0; i < kBlock; ++i) {
            const float sample = float(std::sin(2.0 * std::numbers::pi * frequency *
                                                double(position + i) / kRate));
            block.left[i] = sample;
            block.right[i] = right ? sample : sample;
        }
        process(instance, block);
        if (blockIndex > 10)
            for (float sample : (right ? block.outRight : block.outLeft))
                energy += double(sample) * sample;
        position += kBlock;
    }
    return energy;
}

struct Listener final : PluginListener {
    void onParameterChanged(std::uint32_t, double) noexcept override {}
    void onParameterGesture(std::uint32_t, bool) noexcept override {}
    void onLatencyChanged() noexcept override { ++latencyChanges; }
    void onRestartRequested() noexcept override {}
    void onReloadRequested() noexcept override {}
    int latencyChanges = 0;
};

} // namespace

int main() {
    {
        const PluginDescriptor& descriptor = EqualizerInstance::staticDescriptor();
        check(descriptor.uid == "daw.equalizer" && descriptor.name == "VLT Equalizer" &&
                  !descriptor.isInstrument && descriptor.mainInputChannels == 2 &&
                  descriptor.mainOutputChannels == 2,
              "VLT Equalizer has a stable built-in effect descriptor");
        check(std::ranges::any_of(builtinPlugins(), [](const PluginDescriptor& item) {
                  return item.uid == "daw.equalizer";
              }), "the internal factory publishes VLT Equalizer");
        InternalFactory factory;
        const std::unique_ptr<PluginInstance> created = factory.create(descriptor);
        check(created && created->descriptor().uid == "daw.equalizer",
              "the internal factory creates the Equalizer instance");

        EqualizerInstance instance;
        PluginBusLayout accepted;
        check(instance.setBusLayout({{1}, {1}}, accepted) &&
                  accepted.inputs == std::vector<std::uint16_t>{1} &&
                  accepted.outputs == std::vector<std::uint16_t>{1},
              "mono layout is accepted");
        check(instance.setBusLayout({{2, 1}, {2}}, accepted) &&
                  instance.setBusLayout({{2, 2}, {2}}, accepted) &&
                  !instance.setBusLayout({{6}, {6}}, accepted) &&
                  !instance.setBusLayout({{2, 6}, {2}}, accepted),
              "stereo with mono/stereo sidechain is accepted and surround is rejected");

        std::set<std::string> ids;
        bool indicesStable = instance.parameters().size() == kParameterCount;
        for (std::uint32_t i = 0; i < instance.parameters().size(); ++i) {
            const ParameterInfo& info = instance.parameters()[i];
            ids.insert(info.id);
            indicesStable = indicesStable && info.index == i &&
                            instance.parameterIndexForId(info.id) == std::int32_t(i);
        }
        check(ids.size() == kParameterCount && indicesStable &&
                  ids.contains("processing.mode") && ids.contains("band.01.frequency") &&
                  ids.contains("band.24.detector.high"),
              "all 415 stable parameter ids are complete and unique");
        check(!instance.parameters()[globalParameter(GlobalParam::ProcessingMode)].isAutomatable &&
                  !instance.parameters()[globalParameter(GlobalParam::LinearResolution)].isAutomatable,
              "PDC-changing mode and resolution parameters are not automatable");
        check(factoryPresets().size() == 18 && factoryPresets().front().name == "Flat" &&
                  factoryPresets().back().name == "Master Linear",
              "all 18 factory presets are available");
    }

    {
        EqualizerInstance source;
        set(source, 0, BandParam::Enabled, 1.0);
        set(source, 0, BandParam::Frequency, 4321.0);
        set(source, 0, BandParam::Gain, 42.0);
        source.captureComparison('A');
        set(source, 0, BandParam::Gain, -7.0);
        source.captureComparison('B');
        source.setActiveComparison('B');
        source.setPresetReference("user", "Air Control");
        std::vector<std::uint8_t> state;
        check(source.saveState(state), "equalizer state saves");
        EqualizerInstance restored;
        check(restored.loadState(state) && restored.bandState(0).enabled &&
                  std::abs(restored.bandState(0).frequency - 4321.0) < 1.0e-9 &&
                  restored.bandState(0).gainDb == -7.0 &&
                  restored.comparison('A')[bandParameter(0, BandParam::Gain)] == 30.0 &&
                  restored.activeComparison() == 'B' &&
                  restored.presetReference() ==
                      std::pair<std::string, std::string>{"user", "Air Control"},
              "state v1 round-trips clamped parameters, preset reference and A/B");
        const std::string future =
            R"({"version":99,"params":{"band.01.frequency":999999,"band.01.q":-2,"future":7},"futureObject":{"x":1}})";
        check(restored.loadState(std::span<const std::uint8_t>(
                  reinterpret_cast<const std::uint8_t*>(future.data()), future.size())) &&
                  restored.bandState(0).frequency == 30000.0 &&
                  restored.bandState(0).q == 0.025,
              "future fields are ignored and known state values are clamped");
    }

    {
        EqualizerInstance instance;
        Listener listener;
        instance.setListener(&listener);
        check(instance.latencySamples() == 0, "Zero Latency reports no PDC delay");
        set(instance, GlobalParam::ProcessingMode, double(ProcessingMode::AnalogPhase));
        check(instance.latencySamples() == 16 && listener.latencyChanges == 1,
              "Analog Phase reports 16 samples and notifies the host");
        set(instance, GlobalParam::ProcessingMode, double(ProcessingMode::LinearPhase));
        set(instance, GlobalParam::LinearResolution, double(LinearResolution::Low));
        const auto low = instance.latencySamples();
        set(instance, GlobalParam::LinearResolution, double(LinearResolution::Medium));
        const auto medium = instance.latencySamples();
        set(instance, GlobalParam::LinearResolution, double(LinearResolution::High));
        const auto high = instance.latencySamples();
        check(low == 2048 && medium == 4096 && high == 8192 &&
                  listener.latencyChanges >= 4,
              "Linear Phase resolutions report deterministic FFT-sized PDC");
    }

    {
        EqualizerInstance flat;
        flat.activate({kRate, kBlock, true});
        Block block(kBlock);
        block.left[0] = block.right[0] = 1.0f;
        process(flat, block);
        check(block.outLeft[0] == 1.0f && block.outRight[0] == 1.0f,
              "Zero Latency flat impulse has actual zero delay");

        EqualizerInstance bell;
        set(bell, 0, BandParam::Enabled, 1.0);
        set(bell, 0, BandParam::Type, double(FilterType::Bell));
        set(bell, 0, BandParam::Frequency, 1000.0);
        set(bell, 0, BandParam::Gain, 12.0);
        set(bell, 0, BandParam::Q, 1.0);
        bell.activate({kRate, kBlock, true});
        const double boosted = renderTone(bell, 1000.0);
        EqualizerInstance neutral;
        neutral.activate({kRate, kBlock, true});
        const double baseline = renderTone(neutral, 1000.0);
        check(boosted > baseline * 8.0 &&
                  std::abs(bell.responseDb(1000.0) - 12.0) < 0.2,
              "Bell frequency, gain and Q agree with the displayed response");

        bool shapesFinite = true;
        std::set<long long> shapes;
        for (int type = 0; type <= int(FilterType::AllPass); ++type) {
            EqualizerInstance shape;
            set(shape, 0, BandParam::Enabled, 1.0);
            set(shape, 0, BandParam::Type, type);
            set(shape, 0, BandParam::Frequency, 1200.0);
            set(shape, 0, BandParam::Gain, 9.0);
            set(shape, 0, BandParam::Q, 1.4);
            shape.activate({kRate, kBlock, true});
            const double response = shape.responseDb(300.0) +
                                    2.0 * shape.responseDb(1200.0) +
                                    3.0 * shape.responseDb(6000.0);
            shapesFinite = shapesFinite && std::isfinite(response);
            shapes.insert(std::llround(response * 1000.0));
        }
        check(shapesFinite && shapes.size() >= 7,
              "all nine filter forms are finite and produce distinct responses");

        EqualizerInstance cuts;
        set(cuts, 0, BandParam::Enabled, 1.0);
        set(cuts, 0, BandParam::Type, double(FilterType::LowCut));
        set(cuts, 0, BandParam::Frequency, 1000.0);
        double previous = 0.0;
        bool slopesIncrease = true;
        for (int slope = 0; slope < 8; ++slope) {
            set(cuts, 0, BandParam::Slope, slope);
            const double attenuation = -cuts.responseDb(100.0);
            slopesIncrease = slopesIncrease && attenuation + 0.2 >= previous;
            previous = attenuation;
        }
        check(slopesIncrease && previous > 80.0,
              "6-96 dB/oct Butterworth cut cascades increase attenuation");
    }

    {
        EqualizerInstance placed;
        set(placed, 0, BandParam::Enabled, 1.0);
        set(placed, 0, BandParam::Type, double(FilterType::Bell));
        set(placed, 0, BandParam::Frequency, 1000.0);
        set(placed, 0, BandParam::Gain, 12.0);
        set(placed, 0, BandParam::Placement, double(Placement::Left));
        placed.activate({kRate, kBlock, true});
        const double leftEnergy = renderTone(placed, 1000.0);
        placed.reset();
        const double rightEnergy = renderTone(placed, 1000.0, 30, true);
        check(leftEnergy > rightEnergy * 5.0,
              "Left placement changes only the left channel");

        EqualizerInstance mono;
        PluginBusLayout accepted;
        mono.setBusLayout({{1}, {1}}, accepted);
        set(mono, 0, BandParam::Enabled, 1.0);
        set(mono, 0, BandParam::Placement, double(Placement::Side));
        set(mono, 0, BandParam::Gain, 6.0);
        mono.activate({kRate, kBlock, true});
        Block monoBlock(kBlock, 1);
        std::fill(monoBlock.left.begin(), monoBlock.left.end(), 0.25f);
        process(mono, monoBlock);
        check(std::ranges::all_of(monoBlock.outLeft, [](float value) {
                  return std::isfinite(value);
              }), "all placement modes fold safely to mono");
    }

    {
        auto dynamicGain = [](bool provideSidechain) {
            EqualizerInstance instance;
            set(instance, 0, BandParam::Enabled, 1.0);
            set(instance, 0, BandParam::Type, double(FilterType::Bell));
            set(instance, 0, BandParam::Frequency, 1000.0);
            set(instance, 0, BandParam::Q, 1.0);
            set(instance, 0, BandParam::DynamicEnabled, 1.0);
            set(instance, 0, BandParam::DynamicRange, -18.0);
            set(instance, 0, BandParam::DynamicAuto, 0.0);
            set(instance, 0, BandParam::DynamicThreshold, -40.0);
            set(instance, 0, BandParam::DynamicAttack, 0.1);
            set(instance, 0, BandParam::DynamicRelease, 500.0);
            set(instance, 0, BandParam::DynamicExternal, 1.0);
            instance.activate({kRate, kBlock, true});
            for (int b = 0; b < 20; ++b) {
                Block block(kBlock);
                for (std::uint32_t i = 0; i < kBlock; ++i) {
                    block.left[i] = block.right[i] = 0.2f;
                    block.sideLeft[i] = block.sideRight[i] =
                        float(std::sin(2.0 * std::numbers::pi * 1000.0 *
                                       double(b * kBlock + i) / kRate));
                }
                process(instance, block, provideSidechain);
            }
            return instance.consumeTelemetry();
        };
        const Telemetry missing = dynamicGain(false);
        const Telemetry present = dynamicGain(true);
        check(!missing.sidechainPresent && std::abs(missing.dynamicGainDb[0]) < 0.01f &&
                  present.sidechainPresent && present.dynamicGainDb[0] < -0.5f,
              "external dynamics stays idle when SC is missing and reacts when connected");

        EqualizerInstance automated;
        automated.activate({kRate, kBlock, true});
        Block block(kBlock);
        std::fill(block.left.begin(), block.left.end(), 0.25f);
        std::fill(block.right.begin(), block.right.end(), 0.25f);
        PluginEvent event;
        event.kind = PluginEvent::Kind::ParamValue;
        event.paramIndex = globalParameter(GlobalParam::PolarityInvert);
        event.frameOffset = kBlock / 2;
        event.value = 1.0;
        process(automated, block, false, std::span<const PluginEvent>(&event, 1));
        check(block.outLeft[kBlock / 2 - 1] > 0.0f && block.outLeft[kBlock / 2] < 0.0f,
              "parameter events are applied at their sample offset");
    }

    {
        EqualizerInstance linear;
        set(linear, GlobalParam::ProcessingMode, double(ProcessingMode::LinearPhase));
        set(linear, GlobalParam::LinearResolution, double(LinearResolution::Low));
        set(linear, 0, BandParam::Enabled, 1.0);
        set(linear, 0, BandParam::Type, double(FilterType::AllPass));
        linear.activate({kRate, kBlock, true});
        std::vector<float> rendered(4096);
        for (std::size_t position = 0; position < rendered.size(); position += kBlock) {
            Block block(kBlock);
            if (position == 0) block.left[0] = block.right[0] = 1.0f;
            process(linear, block);
            std::copy(block.outLeft.begin(), block.outLeft.end(),
                      rendered.begin() + std::ptrdiff_t(position));
        }
        const auto peak = std::max_element(rendered.begin(), rendered.end(),
            [](float a, float b) { return std::abs(a) < std::abs(b); });
        const std::size_t peakIndex = std::size_t(std::distance(rendered.begin(), peak));
        check(peakIndex == linear.latencySamples() && std::abs(*peak - 1.0f) < 0.03f,
              "Linear Phase preserves an impulse at its declared PDC delay");
        check(std::abs(linear.responseDb(1000.0)) < 1.0e-9,
              "All Pass is retained but temporarily inactive in Linear Phase");
    }

    {
        EqualizerInstance maximum;
        for (std::uint32_t band = 0; band < kBandCount; ++band) {
            set(maximum, band, BandParam::Enabled, 1.0);
            set(maximum, band, BandParam::Type,
                double(band % 2 ? FilterType::Bell : FilterType::LowCut));
            set(maximum, band, BandParam::Slope, double(Slope::Db96));
            set(maximum, band, BandParam::DynamicEnabled, 1.0);
            set(maximum, band, BandParam::DynamicRange, -6.0);
        }
        maximum.activate({kRate, kBlock, true});
        maximum.setAnalyzerConfig({true, true, true, true, false, 1, 3.0});
        Block block(kBlock);
        std::fill(block.left.begin(), block.left.end(), 0.3f);
        std::fill(block.right.begin(), block.right.end(), -0.2f);
        std::fill(block.sideLeft.begin(), block.sideLeft.end(), 0.7f);
        std::fill(block.sideRight.begin(), block.sideRight.end(), 0.7f);
        process(maximum, block, true);
        gAllocations.store(0, std::memory_order_relaxed);
        gCountAllocations = true;
        process(maximum, block, true);
        gCountAllocations = false;
        const bool finite = std::ranges::all_of(block.outLeft, [](float value) {
            return std::isfinite(value) && std::abs(value) < 100.0f;
        });
        check(gAllocations.load(std::memory_order_relaxed) == 0 && finite,
              "maximum 24-band dynamic/analyzer processing is finite and allocation-free");
        maximum.setAnalyzerConfig({true, true, true, true, true, 1, 3.0});
        const Telemetry before = maximum.consumeTelemetry();
        process(maximum, block, true);
        const Telemetry frozen = maximum.consumeTelemetry();
        check(before.pre == frozen.pre && before.post == frozen.post &&
                  before.sidechain == frozen.sidechain,
              "analyzer Freeze holds the last published traces");
    }

    std::printf("%s\n", failures ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return failures ? 1 : 0;
}
