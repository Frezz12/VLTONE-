#include "Internal/Cla2aInstance.hpp"
#include "Internal/InternalFactory.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <numbers>
#include <tuple>
#include <vector>

namespace {
std::atomic<bool> countAllocations{false};
std::atomic<unsigned> allocations{0};
}
void* operator new(std::size_t size) {
    if (countAllocations.load(std::memory_order_relaxed)) ++allocations;
    if (void* p = std::malloc(std::max(size, std::size_t(1)))) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
using namespace daw::plugins;
using namespace daw::plugins::cla2a;
int failures = 0;
void check(bool ok, const char* text) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", text); failures += !ok; }
double db(double x) { return 20 * std::log10(std::max(std::abs(x), 1e-20)); }
void set(Cla2aInstance& c, Param p, double value) { c.setParameterFromHost(unsigned(p), value); }
struct Block {
    std::vector<float> left, right, out, other;
    const float* input[2]; float* output[2];
    explicit Block(unsigned size) : left(size), right(size), out(size), other(size),
        input{left.data(), right.data()}, output{out.data(), other.data()} {}
    void run(Cla2aInstance& c, unsigned size = 0, std::span<const PluginEvent> events = {}) {
        PluginProcessContext context;
        context.inputs = input; context.outputs = output; context.frames = size ? size : unsigned(left.size());
        context.inputChannels = context.outputChannels = 2; context.inputEvents = events;
        c.process(context);
    }
    void sine(double rate, double hz, double amplitude, std::uint64_t start, unsigned frames) {
        for (unsigned i = 0; i < frames; ++i)
            left[i] = right[i] = float(amplitude * std::sin(2 * std::numbers::pi * hz * (start + i) / rate));
    }
};
std::vector<float> render(double rate, double hz, double amplitude, double gain, double peak,
                          unsigned block = 257, bool automate = false, double seconds = 2) {
    Cla2aInstance c; set(c, Param::Gain, gain); set(c, Param::PeakReduction, peak); c.activate({rate, block, true});
    Block b(block); const unsigned total = unsigned(rate * seconds);
    std::vector<float> out(total);
    for (unsigned start = 0; start < total; start += block) {
        const unsigned frames = std::min(block, total - start);
        b.sine(rate, hz, amplitude, start, frames);
        std::array<PluginEvent, 5> events{}; unsigned n = 0;
        if (automate) for (const auto [at, parameter, value] : std::array<std::tuple<unsigned, unsigned, double>, 5>{{
            {143, 1, 55}, {1701, 2, 1}, {2518, 0, 52}, {3737, 2, 0}, {6043, 1, 20}}}) {
            if (at >= start && at < start + frames) {
                events[n].frameOffset = at - start; events[n].paramIndex = parameter; events[n++].value = value;
            }
        }
        b.run(c, frames, {events.data(), n}); std::copy_n(b.out.begin(), frames, out.begin() + start);
    }
    return out;
}
double amplitude(const std::vector<float>& x, double hz, double rate, unsigned count) {
    double re = 0, im = 0;
    const unsigned start = unsigned(x.size()) - count;
    for (unsigned i = 0; i < count; ++i) {
        const double phase = 2 * std::numbers::pi * hz * i / rate;
        re += x[start + i] * std::cos(phase); im += x[start + i] * std::sin(phase);
    }
    return 2 * std::hypot(re, im) / count;
}
double runSine(Cla2aInstance& c, Block& b, double seconds, double amplitudeValue, std::uint64_t& position, double rate = 48000) {
    const unsigned total = unsigned(std::round(rate * seconds)); double reduction = 0;
    for (unsigned done = 0; done < total; done += unsigned(b.left.size())) {
        const unsigned frames = std::min(unsigned(b.left.size()), total - done);
        b.sine(rate, 1000, amplitudeValue, position, frames); position += frames;
        b.run(c, frames); reduction = c.consumeTelemetry().reduction;
    }
    return reduction;
}
std::vector<double> recovery(double seconds) {
    Cla2aInstance c; set(c, Param::PeakReduction, 50); c.activate({48000, 16});
    Block b(16); std::uint64_t position = 0;
    const double before = runSine(c, b, seconds, std::pow(10., -18. / 20), position);
    std::vector<double> curve{before};
    for (unsigned i = 0; i < 15000; ++i) { b.sine(48000, 1000, 0, position, 16); position += 16; b.run(c); curve.push_back(c.consumeTelemetry().reduction); }
    return curve;
}
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    InternalFactory factory; auto plugin = factory.create(Cla2aInstance::staticDescriptor());
    check(plugin && plugin->parameters().size() == 3 && plugin->descriptor().uid == "daw.cla2a" &&
          plugin->descriptor().name == "VLT 2A" && plugin->descriptor().vendor == "VLTONE" && plugin->descriptor().stateSchemaVersion == 1,
          "CLA-2A factory registration and three stable parameters");
    bool table = true;
    for (const auto& p : parameterTable()) table &= p.isAutomatable && plugin->parameterIndexForId(p.id) == int(p.index) && plugin->parameterValue(p.index) == p.defaultValue;
    check(table && plugin->supportsOfflinePipelining(), "parameter identity/defaults and ordered offline support");
    for (const auto& p : parameterTable()) plugin->setParameterFromHost(p.index, p.maxValue);
    std::vector<std::uint8_t> state; plugin->saveState(state); Cla2aInstance restored;
    bool saved = restored.loadState(state); for (const auto& p : parameterTable()) saved &= restored.parameterValue(p.index) == p.maxValue;
    check(saved, "opaque state round-trip");
    const std::string bad = R"({"version":1,"params":{"gain":40,"peakReduction":"bad"}})";
    check(!restored.loadState({reinterpret_cast<const std::uint8_t*>(bad.data()), bad.size()}) && restored.parameterValue(0) == 100,
          "malformed state does not partially apply");
    for (const auto& p : parameterTable()) {
        restored.setParameterFromHost(p.index, std::numeric_limits<double>::quiet_NaN());
        table &= restored.parameterValue(p.index) == p.defaultValue;
    }
    check(table, "invalid parameter values become finite defaults");
    PluginBusLayout accepted;
    check(restored.setBusLayout({{1}, {1}}, accepted) && accepted.outputs[0] == 1 &&
          !restored.setBusLayout({{2, 2}, {2}}, accepted) && !restored.setBusLayout({{4}, {4}}, accepted),
          "mono/stereo layout contract rejects external sidechain and surround");

    Cla2aInstance impulse; impulse.activate({48000, 1024}); Block b(1024); b.left[0] = b.right[0] = .0001f; b.run(impulse);
    const auto maximum = std::max_element(b.out.begin(), b.out.end(), [](float a, float z) { return std::abs(a) < std::abs(z); });
    check(unsigned(maximum - b.out.begin()) == kLatency && impulse.latencySamples() == 48 && impulse.tailSamplesKnown(),
          "4x 65-tap cascades deliver exactly 48 samples of FIR latency");

    const auto shortRelease = recovery(.100), longRelease = recovery(2.0);
    const auto halfTime = [](const auto& x) {
        for (unsigned i = 1; i < x.size(); ++i) if (x[i] <= x[0] * .5) return i / 3.0;
        return 5000.;
    };
    const auto fullTime = [](const auto& x) {
        for (unsigned i = 1; i < x.size(); ++i) if (x[i] <= .1) return i / 3.0;
        return 5000.;
    };
    std::printf("MEASURE release short/long half %.2f / %.2f ms; to 0.1 dB %.2f / %.2f ms\n",
                halfTime(shortRelease), halfTime(longRelease), fullTime(shortRelease), fullTime(longRelease));
    check(halfTime(shortRelease) >= 30 && halfTime(longRelease) <= 90 && fullTime(shortRelease) >= 250 && fullTime(longRelease) < 5000 &&
          longRelease[1500] > shortRelease[1500] * 3,
          "T4 two-stage release retains more attenuation after sustained illumination");

    Cla2aInstance dynamics; set(dynamics, Param::PeakReduction, 50); dynamics.activate({48000, 16}); Block probe(16); std::uint64_t position = 0;
    const double steady = runSine(dynamics, probe, 2, std::pow(10., -18. / 20), position);
    dynamics.reset(); position = 0; double attackMs = 0;
    for (unsigned i = 0; i < 150; ++i) {
        const double gr = runSine(dynamics, probe, 16. / 48000, std::pow(10., -18. / 20), position);
        if (!attackMs && gr >= steady * (1 - std::exp(-1.))) attackMs = (i + 1) / 3.;
    }
    std::printf("MEASURE nominal GR %.4f dB; 63%% attack %.2f ms\n", steady, attackMs);
    check(std::abs(steady - 6) < .15 && attackMs >= 8 && attackMs <= 16, "reference calibration and approximately 10 ms attack");
    set(dynamics, Param::Mode, 1); const double limit = runSine(dynamics, probe, .2, std::pow(10., -18. / 20), position);
    check(limit > steady + 2, "Limit increases attenuation while retaining optical state");
    set(dynamics, Param::Gain, 80); const double changedGain = runSine(dynamics, probe, .2, std::pow(10., -18. / 20), position);
    check(std::abs(changedGain - limit) < .05, "makeup gain does not drive the compression detector");

    bool curve = true; double previous = 0;
    for (double level : {.0001, .001, .01, .03, .06, .126, .25, .5, 1.}) {
        Cla2aInstance c; set(c, Param::PeakReduction, 50); c.activate({48000, 257}); Block p(257); std::uint64_t cursor = 0;
        const double gr = runSine(c, p, .5, level, cursor); curve &= gr >= previous && std::isfinite(gr); previous = gr;
    }
    check(curve, "optical characteristic is smooth and monotonic across input levels");
    const auto automated = render(48000, 431, .3, 40, 0, 127, true, .25);
    bool partition = true;
    for (unsigned block : {1u, 16u, 257u, 1024u}) {
        const auto other = render(48000, 431, .3, 40, 0, block, true, .25);
        for (unsigned i = 0; i < other.size(); ++i) partition &= std::abs(other[i] - automated[i]) < 1e-7;
    }
    check(partition, "sample-offset automation is independent of block partitioning");

    for (double rate : {44100., 48000., 96000., 192000.}) {
        const auto nominal = render(rate, 1000, std::pow(10., -12. / 20), 40, 0);
        const double fundamental = amplitude(nominal, 1000, rate, unsigned(rate));
        double distortion = 0;
        for (unsigned harmonic = 2; harmonic <= 8 && harmonic * 1000 < rate / 2; ++harmonic)
            distortion += std::pow(amplitude(nominal, harmonic * 1000, rate, unsigned(rate)), 2);
        const double thd = std::sqrt(distortion) / fundamental;
        bool response = true; double low = 0, high = 0;
        const auto reference = render(rate, 1000, .02, 40, 0);
        const double unity = amplitude(reference, 1000, rate, unsigned(rate));
        for (double hz : {30., 70., 8000., 15000.}) {
            const auto tone = render(rate, hz, .02, 40, 0);
            const double deviation = db(amplitude(tone, hz, rate, unsigned(rate)) / unity);
            response &= deviation >= -1.1 && deviation <= .15;
            if (hz == 30) low = deviation; if (hz == 15000) high = deviation;
        }
        const double frequency = rate * 7 / 32;
        const auto highDrive = render(rate, frequency, .4, 68, 0, 257, false, .8);
        const double alias = db(amplitude(highDrive, rate * 11 / 32, rate, 8192) /
                                amplitude(highDrive, frequency, rate, 8192));
        double dc = 0; for (unsigned i = unsigned(nominal.size()) - unsigned(rate); i < nominal.size(); ++i) dc += nominal[i];
        dc /= rate;
        std::printf("MEASURE %.0f Hz: THD %.4f%%; 30/15000 Hz %.3f / %.3f dB; alias %.2f dBc; DC %.3g\n", rate, thd * 100, low, high, alias, dc);
        check(thd > .001 && thd < .005 && response && alias < -65 && std::abs(dc) < 1e-6,
              "nominal tube harmonics, transformer response, suppressed aliases and no DC");
    }

    Cla2aInstance stereo; set(stereo, Param::PeakReduction, 65); stereo.activate({48000, 257}); Block pair(257);
    bool linked = true;
    for (unsigned start = 0; start < 48000; start += 257) {
        pair.sine(48000, 431, .01, start, 257);
        for (unsigned i = 0; i < pair.left.size(); ++i) pair.right[i] = pair.left[i] * .5f;
        pair.run(stereo);
        if (start > 24000) for (unsigned i = 0; i < pair.left.size(); ++i) linked &= std::abs(pair.other[i] - pair.out[i] * .5f) < 2e-6;
    }
    check(linked, "stereo uses shared optical attenuation with independent amplifier channels");
    Cla2aInstance quiet; quiet.activate({48000, 257}); Block silence(257); silence.run(quiet);
    check(std::all_of(silence.out.begin(), silence.out.end(), [](float x) { return x == 0; }), "reset silence emits no noise or hum");
    bool finite = true;
    for (double rate : {8000., 44100., 48000., 96000., 192000., 384000.}) {
        Cla2aInstance c; for (const auto& p : parameterTable()) c.setParameterFromHost(p.index, p.maxValue); c.activate({rate, 257});
        Block extreme(257); std::fill(extreme.left.begin(), extreme.left.end(), 64.f);
        extreme.right[0] = std::numeric_limits<float>::quiet_NaN(); extreme.right[1] = std::numeric_limits<float>::infinity();
        allocations = 0; countAllocations = true; for (unsigned i = 0; i < 8; ++i) extreme.run(c); countAllocations = false;
        finite &= allocations == 0;
        for (float x : extreme.out) finite &= std::isfinite(x) && std::abs(x) < 10;
        c.reset();
    }
    check(finite, "extreme rates/values remain bounded and process allocates no memory");
    Cla2aInstance bench; set(bench, Param::PeakReduction, 50); bench.activate({48000, 256}); Block benchBlock(256); benchBlock.sine(48000, 431, .3, 0, 256);
    const auto start = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < 375; ++i) benchBlock.run(bench);
    std::printf("MEASURE stereo realtime CPU %.2f%% (2 s audio, 48 kHz/256)\n",
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() / 2 * 100);
    return failures ? 1 : 0;
}
