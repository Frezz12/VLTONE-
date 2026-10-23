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
                  int blocks = 30, bool right = false, int settlingBlocks = 11) {
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
        if (blockIndex >= settlingBlocks)
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
        check(descriptor.uid == "daw.equalizer" && descriptor.name == "EQ" &&
                  !descriptor.isInstrument && descriptor.mainInputChannels == 2 &&
                  descriptor.mainOutputChannels == 2,
              "EQ retains its stable built-in effect identity");
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

        for (const auto mode : {ProcessingMode::ZeroLatency, ProcessingMode::AnalogPhase, ProcessingMode::LinearPhase}) {
            EqualizerInstance solo;
            set(solo, GlobalParam::ProcessingMode, double(mode));
            for (std::uint32_t band = 0; band < 2; ++band) {
                set(solo, band, BandParam::Enabled, 1.0);
                set(solo, band, BandParam::Type, double(FilterType::Bell));
                set(solo, band, BandParam::Frequency, band ? 6000. : 1000.);
                set(solo, band, BandParam::Gain, 12.0);
                set(solo, band, BandParam::Q, 2.0);
            }
            solo.activate({kRate, kBlock, true});
            // Flush both the input FFT window and the overlap-add output after
            // changing the tone or audition target; measure equal steady spans.
            const int settlingBlocks = 11 + int((2 * solo.latencySamples() + kBlock - 1) / kBlock);
            const auto measure = [&](double frequency) {
                return renderTone(solo, frequency, settlingBlocks + 19, false, settlingBlocks);
            };
            const double together = measure(6000.);
            solo.setAuditionBand(0);
            const double otherBand = measure(6000.);
            const double chosenBand = measure(1000.);
            solo.setAuditionBand(-1);
            const double restored = measure(6000.);
            if (!check(otherBand < together * .3 && chosenBand > baseline * 4. &&
                  std::abs(restored / together - 1.) < .05 &&
                  solo.bandState(0).enabled && solo.bandState(1).enabled,
                  "Solo processes only the chosen band and restores the full EQ in every phase mode"))
                std::printf("  phase=%u together=%.3f other=%.3f chosen=%.3f restored=%.3f baseline=%.3f\n",
                            unsigned(mode), together, otherBand, chosenBand, restored, baseline);
        }

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
        set(cuts, 0, BandParam::Q, std::sqrt(0.5));
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

        bool neutralCuts = true;
        for (auto type : {FilterType::LowCut, FilterType::HighCut}) {
            EqualizerInstance cut;
            set(cut, 0, BandParam::Enabled, 1.0);
            set(cut, 0, BandParam::Type, double(type));
            set(cut, 0, BandParam::Q, qForNewBand(type));
            cut.activate({kRate, kBlock, true});
            for (double cutoff : {75.0, 1000.0, 18000.0}) {
                set(cut, 0, BandParam::Frequency, cutoff);
                for (int slope = 0; slope < 8; ++slope) {
                    set(cut, 0, BandParam::Slope, slope);
                    double previousDb = cut.responseDb(10.0);
                    for (int i = 1; i <= 800; ++i) {
                        const double db = cut.responseDb(10.0 * std::pow(2000.0, i / 800.0));
                        neutralCuts &= std::isfinite(db) && db <= .01 &&
                            (type == FilterType::LowCut ? db >= previousDb - .01 : db <= previousDb + .01);
                        previousDb = db;
                    }
                }
            }
        }
        check(neutralCuts, "new low/high cuts stay monotonic without a resonance bump at every slope");

        bool qChangesResponse = true, qMatchesAudio = true, qFinite = true;
        for (auto type : {FilterType::LowShelf, FilterType::HighShelf,
                          FilterType::LowCut, FilterType::HighCut}) {
            EqualizerInstance filter;
            set(filter, 0, BandParam::Enabled, 1.0);
            set(filter, 0, BandParam::Type, double(type));
            set(filter, 0, BandParam::Frequency, 1000.0);
            set(filter, 0, BandParam::Gain, 9.0);
            const bool cut = type == FilterType::LowCut || type == FilterType::HighCut;
            for (int slope = cut ? 1 : 0; slope < 8; ++slope) {
                set(filter, 0, BandParam::Slope, slope);
                set(filter, 0, BandParam::Q, 0.5);
                const double wide = filter.responseDb(1400.0);
                set(filter, 0, BandParam::Q, 2.0);
                qChangesResponse &= std::abs(filter.responseDb(1400.0) - wide) > 0.5;
                for (double q : {0.025, 40.0}) {
                    set(filter, 0, BandParam::Q, q);
                    for (double frequency : {10., 100., 1000., 1400., 10000., 23000.})
                        qFinite &= std::isfinite(filter.responseDb(frequency));
                }
            }
            set(filter, 0, BandParam::Slope, double(Slope::Db12));
            for (double q : {0.5, 2.0}) {
                set(filter, 0, BandParam::Q, q);
                filter.activate({kRate, kBlock, true});
                const double measured = 10.0 * std::log10(renderTone(filter, 1500.0) /
                                                         renderTone(neutral, 1500.0));
                qMatchesAudio &= std::abs(measured - filter.responseDb(1500.0)) < 0.15;
                filter.deactivate();
            }
            if (cut) {
                set(filter, 0, BandParam::Slope, double(Slope::Db6));
                set(filter, 0, BandParam::Q, 0.5);
                const double firstOrder = filter.responseDb(1400.0);
                set(filter, 0, BandParam::Q, 2.0);
                qFinite &= std::abs(filter.responseDb(1400.0) - firstOrder) < 1.e-9;
            }
        }
        check(qChangesResponse, "shelf and 12-96 dB cut widths respond to Q at every slope");
        check(qMatchesAudio, "shelf and cut Q changes agree between rendered audio and the graph");
        check(qFinite, "extreme Q stays finite; first-order cuts remain independent of Q");
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
        for(double rate:{44100.,48000.,96000.,192000.}) for(int resolution=0;resolution<3;++resolution) {
            double maximumError=0;
            for(unsigned upstreamDelay:{0u,48u}) {
                EqualizerInstance flat;
                set(flat,GlobalParam::ProcessingMode,double(ProcessingMode::LinearPhase));
                set(flat,GlobalParam::LinearResolution,resolution); flat.activate({rate,kBlock,true});
                const auto latency=flat.latencySamples(); const auto frames=latency+8192u;
                const auto signal=[](unsigned frame) { return float(.2*std::sin(frame*.137)+.04*std::cos(frame*.19)); };
                Block block(kBlock);
                for(unsigned at=0;at<frames;at+=kBlock) {
                    for(unsigned i=0;i<kBlock;++i) block.left[i]=block.right[i]=at+i<upstreamDelay?0.f:signal(at+i-upstreamDelay);
                    process(flat,block);
                    for(unsigned i=0;i<kBlock;++i) {
                        const auto expected=at+i<latency+upstreamDelay?0.f:signal(at+i-latency-upstreamDelay);
                        maximumError=std::max(maximumError,std::abs(double(block.outLeft[i])-expected));
                    }
                }
            }
            check(maximumError<1e-7,"flat Linear Phase has unity gain at every hop phase and upstream delay");
        }
    }

    {
        constexpr std::size_t window = 8192;
        const auto peak = [](const auto& bins) { return *std::max_element(bins.begin(), bins.end()); };
        const auto silent = [](const auto& bins) {
            return std::ranges::all_of(bins, [](float value) { return value == -120.0f; });
        };
        // Exercise the real EQ taps, including anti-phase stereo and the actual
        // output gain, at rates both below and above the displayed 20 kHz range.
        for (double rate : {16000.0, 44100.0, 48000.0, 96000.0, 192000.0}) {
            EqualizerInstance analyzer;
            set(analyzer, GlobalParam::OutputGain, -6.0);
            check(analyzer.activate({rate, kBlock, true}), "Spectre prepares at the host sample rate");
            analyzer.setAnalyzerConfig({true, true, true, true, false, 1, 0.0});
            check(silent(analyzer.telemetry().pre), "unfilled analyzer windows start at silence");
            Block block(kBlock);
            gAllocations.store(0, std::memory_order_relaxed);
            gCountAllocations = true;
            double outputError = 0.0;
            for (std::size_t at = 0; at < window; at += kBlock) {
                for (std::size_t i = 0; i < kBlock; ++i) {
                    block.left[i] = float(0.5 * std::sin(2.0 * std::numbers::pi * 173 * (at + i) / window));
                    block.right[i] = -block.left[i];
                    block.sideLeft[i] = float(0.25 * std::sin(2.0 * std::numbers::pi * 349 * (at + i) / window));
                    block.sideRight[i] = -block.sideLeft[i];
                }
                process(analyzer, block, true);
                for (std::size_t i = 0; i < kBlock; ++i)
                    outputError = std::max(outputError,
                        std::abs(block.outLeft[i] - block.left[i] * std::pow(10.0, -6.0 / 20.0)));
            }
            const auto measured = analyzer.telemetry();
            const auto editor = analyzer.consumeTelemetry();
            const auto rack = analyzer.telemetry();
            gCountAllocations = false;
            check(gAllocations.load(std::memory_order_relaxed) == 0 && outputError < 1e-7,
                  "Spectre capture and telemetry allocate nothing and leave EQ audio unchanged");
            check(std::abs(peak(measured.pre) + 6.0206f) < 0.2f &&
                  std::abs(peak(measured.post) + 12.0206f) < 0.2f &&
                  std::abs(peak(measured.sidechain) + 12.0412f) < 0.2f,
                  "input/output/sidechain spectra preserve calibrated anti-phase stereo power");
            check(measured.pre == editor.pre && editor.pre == rack.pre &&
                  measured.post == rack.post && measured.sidechain == rack.sidechain &&
                  measured.inputLeft == editor.inputLeft && rack.inputLeft == 0.0f,
                  "editor and rack share one analyzed frame without consuming each other's spectrum");
            auto config = analyzer.analyzerConfig();
            config.tiltDbPerOctave = 6.0;
            analyzer.setAnalyzerConfig(config);
            const auto tilted = analyzer.telemetry();
            bool nyquistSilent = true;
            for (std::size_t i = 0; i < kSpectrumBinCount; ++i)
                if (20.0 * std::pow(1000.0, double(i) / (kSpectrumBinCount - 1)) > rate * 0.5)
                    nyquistSilent &= tilted.pre[i] == -120.0f && tilted.post[i] == -120.0f &&
                                     tilted.sidechain[i] == -120.0f;
            check(nyquistSilent, "display tilt never invents content above Nyquist");

            analyzer.reset();
            check(silent(analyzer.telemetry().pre), "stream reset clears the previous spectrum");
            std::fill(block.left.begin(), block.left.end(), 0.0f);
            std::fill(block.right.begin(), block.right.end(), 0.0f);
            for (std::size_t at = 0; at < window; at += kBlock) process(analyzer, block);
            const auto empty = analyzer.telemetry();
            check(silent(empty.pre) && silent(empty.post) && silent(empty.sidechain),
                  "silence and a disconnected sidechain stay at the floor with display tilt");
        }

        EqualizerInstance analyzer;
        analyzer.activate({kRate, kBlock, true});
        AnalyzerConfig config{true, true, true, true, false, 1, 0.0};
        analyzer.setAnalyzerConfig(config);
        Block block(kBlock);
        const auto feed = [&](std::size_t samples, double amplitude, bool mono = false) {
            block.channelCount = mono ? 1 : 2;
            for (std::size_t at = 0; at < samples; at += kBlock) {
                for (std::size_t i = 0; i < kBlock; ++i) {
                    block.left[i] = float(amplitude * std::sin(2.0 * std::numbers::pi * 173 * (at + i) / window));
                    block.right[i] = 0.0f;
                    block.sideLeft[i] = block.sideRight[i] = block.left[i];
                }
                process(analyzer, block, true);
            }
        };
        feed(window, 0.5);
        check(std::abs(peak(analyzer.telemetry().pre) + 9.0309f) < 0.2f,
              "one active stereo channel measures half the stereo power");
        analyzer.reset();
        feed(window, 0.5, true);
        const auto before = analyzer.telemetry();
        check(std::abs(peak(before.pre) + 6.0206f) < 0.2f,
              "mono input is duplicated without a 3 dB spectrum loss");
        config.frozen = true;
        analyzer.setAnalyzerConfig(config);
        analyzer.telemetry();
        feed(window * 2, 0.0);
        const auto frozen = analyzer.telemetry();
        check(frozen.pre == before.pre && frozen.post == before.post && frozen.sidechain == before.sidechain,
              "Freeze holds all three traces while audio capture continues");
        config.frozen = false;
        analyzer.setAnalyzerConfig(config);
        const auto live = analyzer.telemetry();
        check(silent(live.pre) && silent(live.post) && silent(live.sidechain),
              "unfreeze immediately consumes current audio without a stale window or release tail");
        feed(window, 0.5, true);
        analyzer.telemetry();
        config.pre = config.post = config.sidechain = false;
        analyzer.setAnalyzerConfig(config);
        feed(kBlock, 0.0);
        const auto hidden = analyzer.telemetry();
        check(silent(hidden.pre) && silent(hidden.post) && silent(hidden.sidechain),
              "disabled layers clear in both EQ views");
        config.pre = config.post = config.sidechain = true;
        analyzer.setAnalyzerConfig(config);
        feed(kBlock, 0.0);
        check(silent(analyzer.telemetry().pre), "re-enabled layers wait for fresh audio, not old history");
        feed(window, 0.5, true);
        analyzer.telemetry();
        config.enabled = false;
        analyzer.setAnalyzerConfig(config);
        feed(window, 0.0);
        config.enabled = true;
        analyzer.setAnalyzerConfig(config);
        feed(kBlock, 0.0);
        check(silent(analyzer.telemetry().pre), "reopening an EQ view discards audio from before it was hidden");
        config.tiltDbPerOctave = std::numeric_limits<double>::quiet_NaN();
        analyzer.setAnalyzerConfig(config);
        check(std::isfinite(analyzer.analyzerConfig().tiltDbPerOctave), "invalid display tilt cannot poison the trace");
        analyzer.deactivate();
        check(analyzer.activate({96000.0, kBlock, true}) && silent(analyzer.telemetry().pre),
              "sample-rate reactivation prepares fresh Spectre windows");
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
        const Telemetry rackReadout = maximum.telemetry();
        const Telemetry before = maximum.consumeTelemetry();
        const Telemetry afterConsume = maximum.telemetry();
        check(rackReadout.inputLeft > 0.f && rackReadout.inputLeft == before.inputLeft &&
                  afterConsume.inputLeft == 0.f && rackReadout.pre == before.pre &&
                  afterConsume.pre == before.pre,
              "rack spectrum reads preserve editor peak meters and shared analyzer traces");
        process(maximum, block, true);
        const Telemetry frozen = maximum.consumeTelemetry();
        check(before.pre == frozen.pre && before.post == frozen.post &&
                  before.sidechain == frozen.sidechain,
              "analyzer Freeze holds the last published traces");
    }

    std::printf("%s\n", failures ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return failures ? 1 : 0;
}
