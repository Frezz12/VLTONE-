#include "Internal/PitchCorrectorInstance.hpp"
#include "Internal/InternalFactory.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <numbers>
#include <set>
#include <string>
#include <vector>

using namespace daw::plugins;
using namespace daw::plugins::pitch;
namespace {
thread_local bool countAllocations = false;
std::atomic<unsigned> allocations{0};
int failures = 0;
bool check(bool passed, const char* message) {
    std::printf("%s  %s\n", passed ? "PASS" : "FAIL", message);
    failures += !passed;
    return passed;
}
}
void* operator new(std::size_t size) {
    if (countAllocations) allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* result = std::malloc(std::max<std::size_t>(size, 1))) return result;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

namespace {
constexpr double pi = std::numbers::pi;
void set(PitchCorrectorInstance& plugin, Param parameter, double value) {
    plugin.setParameterFromHost(std::uint32_t(parameter), value);
}
double get(const PitchCorrectorInstance& plugin, Param parameter) { return plugin.parameterValue(std::uint32_t(parameter)); }

std::vector<float> tone(double rate, double hz, double seconds = 0.7, bool harmonics = false) {
    std::vector<float> result(std::size_t(std::ceil(rate * seconds)));
    for (std::size_t i = 0; i < result.size(); ++i) {
        const double phase = 2 * pi * hz * double(i) / rate;
        double sample = std::sin(phase);
        if (harmonics) sample += 0.4 * std::sin(2 * phase) + 0.25 * std::sin(3 * phase) + 0.12 * std::sin(5 * phase);
        result[i] = float(0.2 * sample);
    }
    return result;
}
std::vector<float> noise(std::size_t frames) {
    std::uint32_t seed = 7361;
    std::vector<float> result(frames);
    for (auto& sample : result) {
        seed = seed * 1664525u + 1013904223u;
        sample = float(double(seed) / double(UINT32_MAX) - 0.5) * 0.3f;
    }
    return result;
}

struct Result {
    std::vector<float> left, right;
    Telemetry telemetry;
    std::uint32_t latency = 0;
    double cpuMilliseconds = 0;
    unsigned allocationCount = 0;
};
Result render(PitchCorrectorInstance& plugin, const std::vector<float>& source,
              double rate, std::span<const unsigned> blocks, bool mono = false,
              std::span<const PluginEvent> allEvents = {}, bool checkAllocation = false, double rightGain = -0.7) {
    PluginBusLayout layout;
    plugin.setBusLayout({{std::uint16_t(mono ? 1 : 2)}, {std::uint16_t(mono ? 1 : 2)}}, layout);
    plugin.activate({rate, 512, false});
    plugin.startProcessing();
    Result result;
    result.left.resize(source.size()); result.right.resize(source.size());
    std::vector<float> right(source.size());
    for (std::size_t i = 0; i < source.size(); ++i) right[i] = float(source[i] * rightGain);
    result.latency = plugin.latencySamples();
    std::size_t offset = 0, blockIndex = 0;
    std::array<PluginEvent, 32> localEvents{};
    allocations.store(0);
    const auto begin = std::chrono::steady_clock::now();
    while (offset < source.size()) {
        const auto frames = unsigned(std::min<std::size_t>(blocks[blockIndex++ % blocks.size()], source.size() - offset));
        const float* inputs[]{source.data() + offset, right.data() + offset};
        float* outputs[]{result.left.data() + offset, result.right.data() + offset};
        PluginProcessContext context;
        context.inputs = inputs; context.outputs = outputs;
        context.inputChannels = context.outputChannels = mono ? 1 : 2;
        context.frames = frames; context.sampleTime = std::int64_t(offset);
        unsigned eventCount = 0;
        for (const auto& event : allEvents) if (event.frameOffset >= offset && event.frameOffset < offset + frames && eventCount < localEvents.size()) {
            localEvents[eventCount] = event;
            localEvents[eventCount++].frameOffset -= unsigned(offset);
        }
        context.inputEvents = {localEvents.data(), eventCount};
        countAllocations = checkAllocation;
        plugin.process(context);
        countAllocations = false;
        offset += frames;
    }
    result.cpuMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    result.allocationCount = allocations.load();
    result.telemetry = plugin.telemetrySnapshot();
    return result;
}
constexpr std::array<unsigned, 1> block128{128};
constexpr std::array<unsigned, 7> irregular{1, 32, 117, 512, 7, 64, 251};

double maxDifference(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return 1e9;
    double worst = 0;
    for (std::size_t i = 0; i < a.size(); ++i) worst = std::max(worst, std::abs(double(a[i]) - b[i]));
    return worst;
}
double frequency(const std::vector<float>& audio, double rate) {
    // Interpolated positive zero crossings, over the settled final third. The
    // signal is deliberately a sine so harmonics cannot mask a failed shifter.
    double first = -1, last = -1;
    unsigned count = 0;
    for (std::size_t i = audio.size() * 2 / 3 + 1; i < audio.size(); ++i) {
        if (audio[i - 1] <= 0 && audio[i] > 0) {
            const double crossing = double(i - 1) - audio[i - 1] / double(audio[i] - audio[i - 1]);
            if (first < 0) first = crossing;
            last = crossing; ++count;
        }
    }
    return count > 2 ? (count - 1) * rate / (last - first) : 0;
}
bool finiteBounded(const Result& result, double limit = 4) {
    return std::ranges::all_of(result.left, [limit](float x) { return std::isfinite(x) && std::abs(x) < limit; }) &&
           std::ranges::all_of(result.right, [limit](float x) { return std::isfinite(x) && std::abs(x) < limit; });
}
void hard(PitchCorrectorInstance& plugin, int quality = 0, bool formants = true) {
    set(plugin, Param::Tune, 100); set(plugin, Param::Humanize, 0); set(plugin, Param::Vibrato, 0);
    set(plugin, Param::Quality, quality); set(plugin, Param::Formants, formants ? 1 : 0);
}
bool load(PitchCorrectorInstance& plugin, std::string_view text) {
    return plugin.loadState({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}
void contracts() {
    bool retuneRoundtrip = true;
    for (double ms : {0., .1, .5, 2., 18., 72., 200.})
        retuneRoundtrip &= std::abs(retuneMilliseconds(tuneFromMilliseconds(ms))-ms) < 1e-10;
    check(retuneRoundtrip && parameterText(0, 100) == "0.0 ms" && parameterText(0, 40) == "72.0 ms",
          "millisecond presentation preserves legacy tune automation and exact zero endpoint");
    InternalFactory factory;
    const auto& descriptor = PitchCorrectorInstance::staticDescriptor();
    check(descriptor.uid == "daw.pitch-corrector" && descriptor.name == "VLT Pitch" && !descriptor.isInstrument &&
              descriptor.stateSchemaVersion == 1 && factory.create(descriptor) != nullptr,
          "factory discovers and instantiates the stable native effect");
    PitchCorrectorInstance plugin;
    std::set<std::string> ids;
    for (const auto& parameter : plugin.parameters()) ids.insert(parameter.id);
    check(ids.size() == kParameterCount && std::abs(retuneMilliseconds(get(plugin, Param::Tune))-20) < 1e-10 &&
              get(plugin, Param::Humanize) == 0 && get(plugin, Param::Vibrato) == 0 && get(plugin, Param::A4Hz) == 440 &&
              get(plugin, Param::Amount) == 100 && get(plugin, Param::Formants) == 1 &&
              !plugin.parameters()[unsigned(Param::Quality)].isAutomatable,
          "unique stable IDs, 20 ms/0/0 defaults and structural quality flag");
    PluginBusLayout accepted;
    check(plugin.setBusLayout({{1}, {1}}, accepted) && plugin.setBusLayout({{2}, {2}}, accepted) &&
              !plugin.setBusLayout({{2}, {1}}, accepted) && !plugin.setBusLayout({{2, 1}, {2}}, accepted) &&
              !plugin.setBusLayout({{6}, {6}}, accepted), "mono/stereo layouts reject sidechains and mismatched buses");
    set(plugin, Param::A4Hz, 432.12); set(plugin, Param::Tune, 78); set(plugin, Param::Quality, 1);
    set(plugin, Param::NoteMask, 1 << 9); set(plugin, Param::Scale, 7);
    std::vector<std::uint8_t> state;
    plugin.saveState(state);
    PitchCorrectorInstance restored;
    check(restored.loadState(state) && get(restored, Param::A4Hz) == 432.1 && get(restored, Param::NoteMask) == 512 &&
              get(restored, Param::Quality) == 1, "state restores calibration, custom notes and selected quality");
    check(!load(restored, R"({"version":1,"params":{"tune":12,"a4_hz":"bad"}})") && get(restored, Param::Tune) == 78 &&
              !load(restored, R"({"version":999,"params":{}})") && !load(restored, "bad json"),
          "malformed/future states fail atomically");
    set(restored, Param::A4Hz, std::numeric_limits<double>::quiet_NaN()); set(restored, Param::NoteMask, 0);
    check(get(restored, Param::A4Hz) == 440 && get(restored, Param::NoteMask) >= 1, "non-finite parameters and empty note masks stay valid");
    check(!plugin.activate({0, 128}) && !plugin.activate({48000, 0}), "invalid processing configurations are rejected");

    PitchCorrectorInstance mode;
    mode.activate({48000, 128});
    const auto realTimeLatency = mode.latencySamples();
    set(mode, Param::Quality, 1);
    check(mode.qualityChangePending() && mode.activeQuality() == 0 && mode.latencySamples() == realTimeLatency,
          "quality request does not change active audio latency");
    mode.deactivate(); mode.activate({44100, 64});
    check(mode.activeQuality() == 0 && mode.qualityChangePending(), "unrelated device reprepare cannot apply pending HD");
    check(mode.applyPendingQuality(), "parked controller can authorize pending mode");
    mode.deactivate(); mode.activate({48000, 128});
    check(mode.activeQuality() == 1 && !mode.qualityChangePending() && mode.latencySamples() > realTimeLatency,
          "authorized prepare applies HD and publishes its latency");
}
void delayAndRealtime() {
    for (double rate : {44100., 48000., 96000.}) for (int quality : {0, 1}) {
        PitchCorrectorInstance plugin;
        hard(plugin, quality); set(plugin, Param::Amount, 0);
        const auto input = noise(std::size_t(rate * 0.22));
        const auto result = render(plugin, input, rate, irregular, false, {}, true);
        double error = 0;
        for (std::size_t i = 0; i < input.size(); ++i) {
            const float expected = i >= result.latency ? input[i - result.latency] : 0;
            error = std::max(error, std::abs(double(result.left[i]) - expected));
            error = std::max(error, std::abs(double(result.right[i]) + expected * 0.7f));
        }
        std::printf("  mode=%d rate=%.0f latency=%u (%.3f ms) dry_error=%.9g CPU=%.2f ms allocations=%u\n",
                    quality, rate, result.latency, 1000 * result.latency / rate, error, result.cpuMilliseconds, result.allocationCount);
        check(error < 1e-6 && result.latency < input.size() &&
                  (quality || result.latency == unsigned(std::ceil(rate * 0.010))), "reported PDC equals exact delayed dry on both channels");
        check(result.allocationCount == 0, "processing and initial audio callback allocate nothing");
        allocations.store(0); countAllocations = true; plugin.reset(); countAllocations = false;
        check(allocations.load() == 0, "reset is allocation-free");
    }
}
void correction() {
    for (int quality : {0, 1}) for (double reference : {432., 440., 442.}) {
        for (double base : {65.40639132514966, 110., 220., 440., 1046.5022612023945}) {
            const double target = base * reference / 440.;
            const double inputHz = target * std::exp2(31. / 1200.);
            PitchCorrectorInstance plugin; hard(plugin, quality);
            set(plugin, Param::A4Hz, reference);
            const auto result = render(plugin, tone(48000, inputHz), 48000, block128);
            const double actual = frequency(result.left, 48000);
            const double cents = actual > 0 ? 1200 * std::log2(actual / target) : 1e9;
            std::printf("  mode=%d A4=%.0f input=%.3f target=%.3f actual=%.3f cents=%+.3f detector=%.3f confidence=%.3f\n",
                quality, reference, inputHz, target, actual, cents, result.telemetry.inputHz, result.telemetry.confidence);
            check(std::abs(cents) <= 5 && finiteBounded(result), "hard correction settles within five cents across vocal registers and A4 calibration");
        }
    }
    for (int quality : {0, 1}) {
        PitchCorrectorInstance custom; hard(custom, quality);
        set(custom, Param::Scale, 7); set(custom, Param::NoteMask, 1 << 9);
        const auto result = render(custom, tone(48000, 210), 48000, irregular, true);
        check(std::abs(1200 * std::log2(frequency(result.left, 48000) / 220)) <= 5,
              "custom note mask quantizes to the selected absolute pitch class");
        PitchCorrectorInstance scale; hard(scale, quality);
        set(scale, Param::Key, 2); set(scale, Param::Scale, 1);
        const auto major = render(scale, tone(48000, 270), 48000, block128);
        const double cSharp = 440 * std::exp2(-8. / 12.);
        check(std::abs(1200 * std::log2(frequency(major.left, 48000) / cSharp)) <= 5,
              "D major resolves C region to allowed C sharp");
    }
    for (double rate : {44100., 96000.}) for (int quality : {0, 1}) {
        for (double target : {61.735412657, 110., 440., 1174.65907167}) {
            for (double offset : {-19., 31.}) {
                // Stay inside the documented detector range at the two edges.
                const double inputHz = target * std::exp2(offset / 1200);
                PitchCorrectorInstance plugin; hard(plugin, quality);
                const auto result = render(plugin, tone(rate, inputHz, .8), rate, irregular);
                const double actual = frequency(result.left, rate);
                const double error = actual > 0 ? 1200 * std::log2(actual / target) : 1e9;
                std::printf("  edge mode=%d rate=%.0f input=%.3f target=%.3f actual=%.3f cents=%+.3f\n", quality, rate, inputHz, target, actual, error);
                check(std::abs(error) <= 5 && finiteBounded(result), "up/down correction and register edges remain accurate at 44.1/96 kHz");
            }
        }
    }
}
void streaming() {
    for (int quality : {0, 1}) {
        const auto input = tone(48000, 227.2, 0.8, true);
        PitchCorrectorInstance a, b; hard(a, quality); hard(b, quality);
        std::array<PluginEvent, 6> events{};
        const std::array<Param, 6> parameters{Param::Tune, Param::A4Hz, Param::Formants, Param::OutputDb, Param::Formants, Param::Tune};
        const std::array<double, 6> values{40, 432, 0, -6, 1, 100};
        for (std::size_t i = 0; i < events.size(); ++i) {
            events[i].frameOffset = 4800 + unsigned(i) * 3901;
            events[i].paramIndex = unsigned(parameters[i]); events[i].value = values[i];
        }
        const auto fixed = render(a, input, 48000, block128, false, events, true);
        const auto varied = render(b, input, 48000, irregular, false, events, true);
        std::printf("  mode=%d partition_error=%.9g CPU=%.2f ms per800ms alloc=%u\n", quality,
                    maxDifference(fixed.left, varied.left), fixed.cpuMilliseconds, fixed.allocationCount);
        check(maxDifference(fixed.left, varied.left) < 2e-6 && maxDifference(fixed.right, varied.right) < 2e-6,
              "sample-offset automation is independent of block partitioning");
        check(fixed.allocationCount == 0 && varied.allocationCount == 0 && finiteBounded(fixed) && finiteBounded(varied),
              "voiced processing and automation remain finite and allocation-free");
    }
    // An exact anti-correlated stereo voice must not disappear in the detector
    // or diverge because independent shifters chose different grain boundaries.
    PitchCorrectorInstance stereo; hard(stereo);
    const auto result = render(stereo, tone(48000, 227.2, 0.7), 48000, irregular, false, {}, false, -1);
    double error = 0;
    for (std::size_t i = 0; i < result.left.size(); ++i) error = std::max(error, std::abs(double(result.right[i]) + result.left[i]));
    check(error < 1e-4 && result.telemetry.confidence > 0.75, "linked stereo processing preserves image and detects anti-correlated vocals");
}
void transitions() {
    const double rate = 48000;
    auto input = noise(48000);
    double phase = 0;
    for (std::size_t i = 0; i < input.size(); ++i) {
        const double time = i / rate;
        if (time < .08 || (time >= .55 && time < .65) || time > .92) continue;
        const double hz = time < .3 ? 112. : time < .55 ? 227. : 450. * std::exp2(0.03 * std::sin(2 * pi * 5.3 * time));
        phase += 2 * pi * hz / rate;
        input[i] = float(.2 * std::sin(phase) + .05 * std::sin(3 * phase));
    }
    for (int quality : {0, 1}) {
        PitchCorrectorInstance plugin; hard(plugin, quality);
        const auto result = render(plugin, input, rate, irregular, false, {}, true);
        double peak = 0;
        for (float sample : result.left) peak = std::max(peak, std::abs(double(sample)));
        check(finiteBounded(result, 1.5) && result.allocationCount == 0 && peak > .05,
              "voiced/noise/low notes/octave changes and vibrato have bounded finite output");
    }
}

void onsetTiming() {
    constexpr double rate = 48000;
    constexpr unsigned frames = 72000;
    std::vector<float> source(frames, 0);
    constexpr std::array<unsigned, 3> starts{4800, 28800, 50400};
    constexpr std::array<double, 3> frequencies{67.2, 227.2, 454.4};
    for (std::size_t burst = 0; burst < starts.size(); ++burst) {
        for (unsigned i = 0; i < 15000; ++i) {
            const double envelope = std::min({1., i / 192., (15000-i) / 384.});
            source[starts[burst]+i] = float(.2 * envelope * std::sin(2*pi*frequencies[burst]*i/rate));
        }
    }
    for (int quality : {0, 1}) {
        PitchCorrectorInstance plugin; hard(plugin, quality);
        const auto result = render(plugin, source, rate, irregular, true);
        bool aligned = true;
        for (unsigned start : starts) {
            auto crossing = [](const std::vector<float>& audio, std::size_t begin) {
                for (std::size_t i = begin; i < std::min(audio.size(), begin + 2400); ++i)
                    if (std::abs(audio[i]) > .025f) return i;
                return audio.size();
            };
            const auto inputAt = crossing(source, start);
            const auto outputAt = crossing(result.left, start + result.latency);
            const auto deviation = std::int64_t(outputAt) - std::int64_t(inputAt) - result.latency;
            std::printf("  attack mode=%d start=%u deviation=%lld samples\n", quality, start, static_cast<long long>(deviation));
            aligned = aligned && std::abs(deviation) <= 48;
        }
        check(aligned, "low/high voiced attacks stay within one millisecond of reported PDC");
    }
}
} // namespace

int main(int argc, char** argv) {
    const bool quick = argc > 1 && std::string_view(argv[1]) == "--quick";
    contracts(); delayAndRealtime();
    if (!quick) { correction(); streaming(); transitions(); onsetTiming(); }
    std::printf("VLT Pitch: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
