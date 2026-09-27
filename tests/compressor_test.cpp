#include "Internal/CompressorInstance.hpp"
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
#include <tuple>
#include <vector>

using namespace daw::plugins;
using namespace daw::plugins::compressor;
namespace { thread_local bool countAllocations = false; unsigned allocations = 0; }
void* operator new(std::size_t n) { if (countAllocations) ++allocations; if (void* p = std::malloc(std::max(n, std::size_t(1)))) return p; throw std::bad_alloc(); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
namespace {
int failures = 0;
void check(bool ok, const char* message) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", message); failures += !ok; }
void set(CompressorInstance& c, Param p, double v) { c.setParameterFromHost(unsigned(p), v); }
struct Block {
    std::vector<float> left, right, out, other, side;
    const float* inputs[2]; float* outputs[2]; const float* sides[2];
    explicit Block(unsigned size, float level = 0) : left(size, level), right(size, level), out(size), other(size), side(size) {
        inputs[0] = left.data(); inputs[1] = right.data(); outputs[0] = out.data(); outputs[1] = other.data(); sides[0] = side.data(); sides[1] = side.data();
    }
    void run(CompressorInstance& c, std::span<const PluginEvent> events = {}, unsigned sideChannels = 0) {
        PluginProcessContext context; context.inputs = inputs; context.outputs = outputs;
        context.inputChannels = context.outputChannels = 2; context.frames = unsigned(left.size()); context.inputEvents = events;
        if (sideChannels) { context.sidechainInputs = sides; context.sidechainInputChannels = std::uint16_t(sideChannels); }
        c.process(context);
    }
};
double db(double v) { return 20 * std::log10(std::max(v, 1e-20)); }
std::vector<float> render(double rate, unsigned block, double mix, int mode, bool automation = false) {
    CompressorInstance c; set(c, Param::Mix, mix); set(c, Param::Mode, mode); c.activate({rate, block, true});
    std::vector<float> result; const unsigned count = unsigned(rate / 3);
    for (unsigned start = 0; start < count; start += block) {
        Block b(std::min(block, count - start));
        for (unsigned i = 0; i < b.left.size(); ++i) b.left[i] = b.right[i] = float(.6 * std::sin((start + i) * .041));
        std::array<PluginEvent, 4> events{}; unsigned n = 0;
        if (automation) for (const auto [position, param, value] : std::array<std::tuple<unsigned, unsigned, double>, 4>{{
             {143, 1, -36}, {1701, 7, 1}, {2518, 8, 1}, {3737, 7, 0}}}) {
            if (position >= start && position < start + b.left.size()) {
                events[n].frameOffset = position - start; events[n].paramIndex = param; events[n++].value = value;
            }
        }
        b.run(c, {events.data(), n}); result.insert(result.end(), b.out.begin(), b.out.end());
    }
    return result;
}
double binAmplitude(const std::vector<float>& x, double hz, double rate, unsigned skip) {
    double real = 0, imag = 0;
    for (unsigned i = skip; i < x.size(); ++i) {
        const double phase = 2 * std::numbers::pi * hz * (i - skip) / rate;
        real += x[i] * std::cos(phase); imag += x[i] * std::sin(phase);
    }
    return 2 * std::hypot(real, imag) / (x.size() - skip);
}
}
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    InternalFactory factory;
    auto plugin = factory.create(CompressorInstance::staticDescriptor());
    check(plugin && plugin->parameters().size() == 9 && plugin->descriptor().category == "Effect|Dynamics|Compressor" && plugin->descriptor().stateSchemaVersion == 1,
          "registered compressor has nine stable automatable parameters and schema 1");
    bool table = true;
    for (const auto& p : parameterTable()) table &= p.isAutomatable && plugin->parameterIndexForId(p.id) == int(p.index) && plugin->parameterValue(p.index) == p.defaultValue;
    check(table, "parameter IDs, defaults and automation round-trip");
    for (const auto& p : parameterTable()) plugin->setParameterFromHost(p.index, p.maxValue);
    std::vector<std::uint8_t> state; plugin->saveState(state); CompressorInstance restored;
    bool saved = restored.loadState(state);
    for (const auto& p : parameterTable()) saved &= restored.parameterValue(p.index) == p.maxValue;
    check(saved, "all nine values survive opaque state save/load");
    const std::string invalid = R"({"version":1,"params":{"ratio":2,"mix":"bad"}})";
    check(!restored.loadState({reinterpret_cast<const std::uint8_t*>(invalid.data()), invalid.size()}) && restored.parameterValue(0) == 20,
          "invalid state fails atomically");

    bool staticCurve = true, kneeContinuous = true;
    for (double ratio : {1., 3., 20.}) for (double threshold : {-60., -18., 0.}) for (double knee : {0., 6., 24.}) {
        for (double input : {-66., -36., -18., -15., 0., 6.}) {
            const double delta = input - threshold;
            const double expected = delta < -knee / 2 ? 0 : delta > knee / 2 || knee == 0
                ? std::max(0., delta) * (1 - 1 / ratio) : (1 - 1 / ratio) * std::pow(delta + knee / 2, 2) / (2 * knee);
            staticCurve &= std::abs(reductionDb(input, threshold, ratio, knee) - expected) < 1e-10;
        }
        for (double edge : {threshold - knee / 2, threshold + knee / 2})
            kneeContinuous &= std::abs(reductionDb(edge - 1e-6, threshold, ratio, knee) - reductionDb(edge + 1e-6, threshold, ratio, knee)) < 2e-6;
    }
    check(staticCurve && kneeContinuous, "static ratio/threshold/knee curve matches analytical law and is continuous");
    check(autoGainDb(-60, 20, 24) == 12 && autoGainDb(0, 3, 6) == 0 && std::abs(autoGainDb(-30, 3, 0) - 8) < 1e-10,
          "Auto Gain references -18 dB and clamps compensation to +12 dB");

    for (double rate : {44100., 48000., 96000.}) {
        bool latency = true;
        for (double mix : {0., 50., 100.}) for (int mode : {0, 1}) {
            CompressorInstance c; set(c, Param::Ratio, 1); set(c, Param::Mode, mode); set(c, Param::Mix, mix); c.activate({rate, 127, true});
            Block b(127); b.left[0] = b.right[0] = 1e-4f; b.run(c);
            const auto peak = std::max_element(b.out.begin(), b.out.end());
            latency &= c.latencySamples() == 16 && std::distance(b.out.begin(), peak) == 16;
            if (!mode || mix == 0) for (unsigned i = 0; i < 127; ++i) latency &= b.out[i] == (i == 16 ? 1e-4f : 0.f);
        }
        check(latency, "44.1/48/96 kHz impulse: all modes/mixes peak at reported 16-sample latency");
        const auto dry = render(rate, 257, 0, 1), wet = render(rate, 257, 100, 1), half = render(rate, 257, 50, 1);
        bool mix = true; for (unsigned i = 0; i < dry.size(); ++i) mix &= std::abs(half[i] - .5f * (dry[i] + wet[i])) < 1e-6;
        check(mix, "Mix 0/50/100 is linear with aligned dry and processed branches");
        const auto reference = render(rate, 1, 100, 0, true);
        bool invariant = true;
        for (unsigned size : {17u, 128u, 257u, 1024u}) {
            const auto candidate = render(rate, size, 100, 0, true);
            for (unsigned i = 0; i < reference.size(); ++i) invariant &= std::abs(candidate[i] - reference[i]) < 1e-7;
        }
        check(invariant, "timestamped automation is invariant across 1/17/128/257/1024-sample blocks");
    }
    {
        bool measured = true;
        for (double ratio : {1., 3., 20.}) for (double knee : {0., 6., 24.}) for (double inputDb : {-40., -18., -6.}) {
            CompressorInstance c; set(c, Param::Ratio, ratio); set(c, Param::Knee, knee); c.activate({48000, 48000, true});
            Block b(48000, float(std::pow(10, inputDb / 20))); b.run(c);
            measured &= std::abs(db(std::abs(b.out.back())) - (inputDb - reductionDb(inputDb, -18, ratio, knee))) < .015;
        }
        check(measured, "measured steady-state DSP agrees with static characteristic across ratios/knees/levels");
    }
    {
        CompressorInstance c; set(c, Param::Mode, 1); set(c, Param::Knee, 0); c.activate({48000, 480, true});
        Block high(480, 1); high.run(c); const double attack = c.consumeTelemetry().reduction;
        check(std::abs(attack - 12 * (1 - std::exp(-1.))) < .0001, "10 ms attack reaches 63.2 percent of target dB reduction");
        for (int i = 0; i < 100; ++i) high.run(c); c.consumeTelemetry();
        Block quiet(5760); quiet.run(c); c.consumeTelemetry(); Block one(1); one.run(c);
        check(std::abs(c.consumeTelemetry().reduction - 12 * std::exp(-5761. / 5760)) < .0001, "120 ms release decays reduction to 36.8 percent");
        CompressorInstance soft, punch; set(punch, Param::Mode, 1); soft.activate({48000, 16}); punch.activate({48000, 16});
        Block transient(16, 1); transient.run(soft); transient.run(punch);
        check(punch.consumeTelemetry().reduction > soft.consumeTelemetry().reduction * 2, "Soft 10 ms RMS and Punch peak detection differ on a transient");
    }
    {
        CompressorInstance linked; linked.activate({48000, 48000}); Block b(48000, .8f); std::fill(b.right.begin(), b.right.end(), .2f); b.run(linked);
        bool stereo = true; for (unsigned i = 16; i < b.out.size(); ++i) stereo &= std::abs(b.other[i] - .25f * b.out[i]) < 1e-7;
        check(stereo, "linked stereo applies the same gain to both channels");
        bool sidechain = true;
        for (unsigned channels : {0u, 1u, 2u}) {
            CompressorInstance c; c.activate({48000, 48000, false, true}); b.run(c, {}, channels);
            sidechain &= std::abs(b.out.back() - .8) < 1e-6;
            if (channels) { std::fill(b.side.begin(), b.side.end(), 1.f); c.reset(); b.run(c, {}, channels); sidechain &= b.out.back() < .21; std::fill(b.side.begin(), b.side.end(), 0.f); }
        }
        check(sidechain, "connected silent/missing sidechain stays silent; mono and stereo external signals control gain");
        CompressorInstance c; set(c, Param::Threshold, -30); set(c, Param::Knee, 0); set(c, Param::AutoGain, 1); set(c, Param::Makeup, 3); c.activate({48000, 48000});
        Block reference(48000, float(std::pow(10., -18. / 20))); reference.run(c);
        check(std::abs(db(reference.out.back()) + 15) < .002, "Auto Gain adds reference compensation to manual Makeup in DSP");
    }
    {
        CompressorInstance c; set(c, Param::Ratio, 1); c.activate({48000, 2048}); Block b(2048, .5f);
        PluginEvent event; event.frameOffset = 37; event.paramIndex = 2; event.value = 12; b.run(c, {&event, 1});
        bool exact = true; for (unsigned i = 16; i < 53; ++i) exact &= b.out[i] == .5f;
        check(exact && b.out[53] > .5f && b.out.back() > 1.9f, "Makeup automation starts at its exact sample offset before latency");
        c.reset(); set(c, Param::Makeup, 0); c.reset(); b.run(c);
        std::array<PluginEvent, 3> changes{};
        changes[0].frameOffset = 10; changes[0].paramIndex = 7; changes[0].value = 1;
        changes[1] = changes[0]; changes[1].frameOffset = 220; changes[1].value = 0;
        changes[2] = changes[0]; changes[2].frameOffset = 1000;
        b.run(c, changes); double jump = 0; for (unsigned i = 1; i < b.out.size(); ++i) jump = std::max(jump, double(std::abs(b.out[i] - b.out[i-1])));
        check(jump < .0003, "mode crossfades and mid-transition reversals introduce no discontinuity on DC");
    }
    {
        CompressorInstance c; set(c, Param::Mode, 1); set(c, Param::Ratio, 1); c.activate({48000, 50000});
        Block b(50000); std::vector<float> naive(50000);
        constexpr double drive = 1.9952623149688795;
        for (unsigned i = 0; i < 50000; ++i) { const double x = .95 * std::sin(2 * std::numbers::pi * 7000 * i / 48000); b.left[i] = b.right[i] = float(x); naive[i] = float(.75 * x + .25 * std::tanh(drive * x) / drive); }
        b.run(c);
        const double rawAlias = binAmplitude(naive, 13000, 48000, 2000), filteredAlias = binAmplitude(b.out, 13000, 48000, 2000);
        std::printf("alias at 13 kHz: raw %.8f, 2x FIR %.8f (%.1f dB)\n", rawAlias, filteredAlias, db(filteredAlias / rawAlias));
        check(filteredAlias < rawAlias * .3, "2x 33-tap FIR saturation reduces fifth-harmonic alias by more than 10 dB");
        CompressorInstance small; set(small, Param::Mode, 1); set(small, Param::Ratio, 1); small.activate({48000, 4096}); Block dc(4096, .0001f); dc.run(small);
        check(std::abs(dc.out.back() / .0001 - 1) < 1e-5, "normalized Punch saturation retains unity gain at small levels");
    }
    {
        bool stable = true, silent = true;
        for (double rate : {44100., 48000., 96000.}) {
            CompressorInstance c; for (const auto& p : parameterTable()) c.setParameterFromHost(p.index, p.maxValue); c.activate({rate, 257});
            Block b(257);
            std::array<PluginEvent, 9> events{}; for (unsigned i = 0; i < 9; ++i) { events[i].frameOffset = i * 17; events[i].paramIndex = i; events[i].value = parameterTable()[i].minValue; }
            countAllocations = true;
            for (int n = 0; n < 64; ++n) {
                for (unsigned i = 0; i < 257; ++i) {
                    b.left[i] = float(1.5 * std::sin((n * 257 + i) * .17));
                    b.right[i] = float(2 * std::cos((n * 257 + i) * .23));
                }
                b.left[3] = std::numeric_limits<float>::infinity(); b.right[5] = std::numeric_limits<float>::quiet_NaN();
                for (unsigned i = 0; i < 9; ++i) events[i].value = n % 2 ? parameterTable()[i].maxValue : parameterTable()[i].minValue;
                b.run(c, events);
                for (float v : b.out) stable &= std::isfinite(v) && std::abs(v) < 128;
                for (float v : b.other) stable &= std::isfinite(v) && std::abs(v) < 128;
            }
            std::fill(b.left.begin(), b.left.end(), 0.f); std::fill(b.right.begin(), b.right.end(), 0.f); c.reset(); b.run(c);
            countAllocations = false;
            for (float v : b.out) silent &= v == 0;
        }
        check(stable && silent && allocations == 0, "loud audio, silence, invalid samples, extreme values and automation stay finite without allocations");
    }
    return failures ? 1 : 0;
}
