#include "Internal/InternalFactory.hpp"
#include "Internal/ModulationInstance.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <new>
#include <set>

using namespace daw::plugins;
using namespace daw::plugins::modulation;
namespace {
thread_local bool counting = false;
std::atomic<unsigned> allocations{0};
} // namespace
void *operator new(std::size_t n) {
    if (counting)
        ++allocations;
    if (auto *p = std::malloc(std::max<std::size_t>(1, n)))
        return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t n) {
    return ::operator new(n);
}
void operator delete(void *p) noexcept {
    std::free(p);
}
void operator delete[](void *p) noexcept {
    std::free(p);
}
void operator delete(void *p, std::size_t) noexcept {
    std::free(p);
}
void operator delete[](void *p, std::size_t) noexcept {
    std::free(p);
}

namespace {
int failures = 0;
void check(bool ok, const char *message) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", message);
    if (!ok)
        ++failures;
}
std::unique_ptr<ModulationInstance> make(Kind k) {
    switch (k) {
    case Kind::Doubler:
        return std::make_unique<DoublerInstance>();
    case Kind::Chorus:
        return std::make_unique<ChorusInstance>();
    case Kind::Flanger:
        return std::make_unique<FlangerInstance>();
    case Kind::Phaser:
        return std::make_unique<PhaserInstance>();
    }
    std::abort();
}
void preset(ModulationInstance &p, unsigned index) {
    const auto &f = factoryPresets(p.kind())[index];
    for (const auto &info : p.parameters())
        p.setParameterFromHost(info.index, f.values[info.index]);
    p.setPresetReference("factory", std::string(f.name));
}
struct Audio {
    std::vector<float> l, r;
    explicit Audio(unsigned n) : l(n), r(n) {}
    unsigned size() const { return unsigned(l.size()); }
};
void run(ModulationInstance &p, const Audio &in, Audio &out, unsigned block = 257,
         std::span<const PluginEvent> events = {}) {
    for (unsigned at = 0; at < in.size();) {
        const auto count = std::min(block, in.size() - at);
        const float *inputs[]{in.l.data() + at, in.r.data() + at};
        float *outputs[]{out.l.data() + at, out.r.data() + at};
        std::array<PluginEvent, 16> local{};
        unsigned n = 0;
        for (const auto &e : events)
            if (e.frameOffset >= at && e.frameOffset < at + count) {
                local[n] = e;
                local[n++].frameOffset -= at;
            }
        PluginProcessContext ctx;
        ctx.inputs = inputs;
        ctx.outputs = outputs;
        ctx.inputChannels = ctx.outputChannels = 2;
        ctx.frames = count;
        ctx.inputEvents = {local.data(), n};
        counting = true;
        p.process(ctx);
        counting = false;
        at += count;
    }
}
Audio fixture(double rate, double seconds, bool stereo = false) {
    Audio a(unsigned(rate * seconds));
    std::uint32_t noise = 42;
    for (unsigned i = 0; i < a.size(); ++i) {
        noise = noise * 1664525u + 1013904223u;
        const double t = i / rate;
        const double x = .25 * std::sin(2 * dsp::pi * 173 * t) +
                         .08 * std::sin(2 * dsp::pi * 431 * t) +
                         .015 * (double(noise) / 4294967295. - .5);
        a.l[i] = float(x);
        a.r[i] = stereo ? float(.2 * std::sin(2 * dsp::pi * 229 * t) + .4 * x) : float(x);
    }
    return a;
}
double difference(const Audio &a, const Audio &b) {
    double e = 0;
    for (unsigned i = 0; i < a.size(); ++i)
        e = std::max({e, std::abs(double(a.l[i]) - b.l[i]), std::abs(double(a.r[i]) - b.r[i])});
    return e;
}
double monoError(const Audio &a, const Audio &b) {
    double e = 0;
    for (unsigned i = 0; i < a.size(); ++i)
        e = std::max(e, std::abs(.5 * (double(a.l[i]) + a.r[i] - b.l[i] - b.r[i])));
    return e;
}
double correlation(const Audio &a, unsigned start) {
    double ll = 0, rr = 0, lr = 0;
    for (unsigned i = start; i < a.size(); ++i) {
        ll += double(a.l[i]) * a.l[i];
        rr += double(a.r[i]) * a.r[i];
        lr += double(a.l[i]) * a.r[i];
    }
    return lr / std::sqrt(std::max(1.e-30, ll * rr));
}
double peak(const Audio &a, unsigned start = 0) {
    double p = 0;
    for (unsigned i = start; i < a.size(); ++i) {
        if (!std::isfinite(a.l[i]) || !std::isfinite(a.r[i]))
            return 1.e99;
        p = std::max({p, std::abs(double(a.l[i])), std::abs(double(a.r[i]))});
    }
    return p;
}

void writeWav(const std::filesystem::path &path, const Audio &a, unsigned rate) {
    // IEEE float stereo WAV preserves the null-test relation without PCM dither.
    std::ofstream out(path, std::ios::binary);
    const auto u16 = [&](unsigned n) {
        char b[]{char(n), char(n >> 8)};
        out.write(b, 2);
    };
    const auto u32 = [&](unsigned n) {
        char b[]{char(n), char(n >> 8), char(n >> 16), char(n >> 24)};
        out.write(b, 4);
    };
    out.write("RIFF", 4);
    u32(36 + a.size() * 8);
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(3);
    u16(2);
    u32(rate);
    u32(rate * 8);
    u16(8);
    u16(32);
    out.write("data", 4);
    u32(a.size() * 8);
    for (unsigned i = 0; i < a.size(); ++i) {
        out.write(reinterpret_cast<const char *>(&a.l[i]), 4);
        out.write(reinterpret_cast<const char *>(&a.r[i]), 4);
    }
}
void examples(const char *directory) {
    std::filesystem::create_directories(directory);
    // Clearly labelled synthetic vowel/consonant source, not a recorded singer.
    constexpr unsigned rate = 48000;
    Audio source(rate * 8);
    double phase = 0;
    std::uint32_t noise = 91;
    for (unsigned i = 0; i < source.size(); ++i) {
        const double t = double(i) / rate, syllable = std::fmod(t, .8);
        const double envelope =
            std::clamp(syllable / .035, 0., 1.) * std::clamp((.70 - syllable) / .10, 0., 1.);
        phase += 2 * dsp::pi * (155 + 12 * std::sin(t * .7) + 1.5 * std::sin(t * 31)) / rate;
        double x = 0;
        for (int h = 1; h <= 36; ++h) {
            const double hz = h * 165;
            const double formant = std::exp(-std::pow((hz - 650) / 180, 2)) +
                                   .5 * std::exp(-std::pow((hz - 1250) / 220, 2)) +
                                   .25 * std::exp(-std::pow((hz - 2600) / 400, 2));
            x += std::sin(h * phase) * (0.10 / h + .08 * formant);
        }
        noise = noise * 1664525u + 1013904223u;
        if (syllable < .065)
            x += .06 * (double(noise) / 4294967295. - .5);
        source.l[i] = source.r[i] = float(x * envelope);
    }
    writeWav(std::filesystem::path(directory) / "01-synthetic-dry.wav", source, rate);
    for (int k = 0; k < 4; ++k) {
        auto p = make(Kind(k));
        p->activate({rate, 257, true});
        Audio result(source.size());
        run(*p, source, result);
        const auto name = descriptorFor(Kind(k)).name;
        writeWav(std::filesystem::path(directory) / (name + "-default.wav"), result, rate);
        if (k == 0) {
            for (unsigned i = 0; i < result.size(); ++i)
                result.l[i] = result.r[i] = float(.5 * (double(result.l[i]) + result.r[i]));
            writeWav(std::filesystem::path(directory) / "Doubler-mono.wav", result, rate);
        } else {
            double dryEnergy = 0, wetEnergy = 0;
            for (unsigned i = 0; i < result.size(); ++i) {
                dryEnergy += double(source.l[i]) * source.l[i];
                wetEnergy +=
                    .5 * (double(result.l[i]) * result.l[i] + double(result.r[i]) * result.r[i]);
            }
            const double gain = std::sqrt(dryEnergy / std::max(1.e-20, wetEnergy));
            for (unsigned i = 0; i < result.size(); ++i) {
                result.l[i] *= float(gain);
                result.r[i] *= float(gain);
            }
            writeWav(std::filesystem::path(directory) / (name + "-rms-matched.wav"), result, rate);
        }
    }
}
} // namespace

int main(int argc, char **argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--examples") {
        examples(argv[2]);
        return 0;
    }
    const auto start = std::chrono::steady_clock::now();
    for (int k = 0; k < 4; ++k) {
        const auto kind = Kind(k);
        std::printf("\n%s\n", descriptorFor(kind).name.c_str());
        auto p = make(kind);
        InternalFactory factory;
        check(bool(factory.create(descriptorFor(kind))), "factory instantiates registered effect");
        const auto builtins = builtinPlugins();
        check(std::any_of(builtins.begin(), builtins.end(),
                          [&](const auto &d) { return d.uid == p->descriptor().uid; }),
              "builtin catalogue publishes effect");
        check(factoryPresets(kind).size() == 10 && p->parameters().size() == (k == 0 ? 3 : 4),
              "ten presets and minimal stable parameter set");
        PluginBusLayout accepted;
        check(p->setBusLayout({{1}, {1}}, accepted) && p->setBusLayout({{2}, {2}}, accepted) &&
                  !p->setBusLayout({{3}, {3}}, accepted) &&
                  !p->setBusLayout({{2, 2}, {2}}, accepted),
              "mono/stereo layouts accepted, unsupported buses rejected");
        bool bankValid = true;
        std::set<long long> signatures;
        const auto signal = fixture(48000, .4, true);
        Audio output(signal.size());
        for (unsigned i = 0; i < 10; ++i) {
            preset(*p, i);
            std::vector<std::uint8_t> state;
            p->saveState(state);
            auto restored = make(kind);
            bankValid &=
                restored->loadState(state) && restored->presetReference() == p->presetReference();
            for (const auto &info : p->parameters())
                bankValid &= restored->parameterValue(info.index) == p->parameterValue(info.index);
            p->activate({48000, 257, true});
            run(*p, signal, output);
            bankValid &= peak(output) < 2;
            double sig = 0;
            for (unsigned n = 0; n < output.size(); ++n)
                sig += output.l[n] * double(1 + n % 71) + output.r[n] * double(n % 23);
            signatures.insert(std::llround(sig * 1.e8));
        }
        check(bankValid && signatures.size() == 10,
              "all presets restore metadata and produce distinct bounded audio");
        const auto before = p->parameterValue(0);
        const std::string bad = "{\"version\":99,\"uid\":\"daw.doubler\",\"params\":{}}";
        check(!p->loadState({reinterpret_cast<const std::uint8_t *>(bad.data()), bad.size()}) &&
                  p->parameterValue(0) == before,
              "invalid state rejected without partially changing parameters");

        bool matrix = true, deterministic = true, tail = true;
        for (double rate : {44100., 48000., 96000., 192000.}) {
            auto input = fixture(rate, .18, true);
            Audio reference(input.size());
            p = make(kind);
            for (const auto &info : p->parameters())
                p->setParameterFromHost(info.index, info.maxValue);
            p->activate({rate, 1024, true});
            run(*p, input, reference, 1);
            for (unsigned block : {64u, 257u, 1024u}) {
                p->reset();
                Audio actual(input.size());
                run(*p, input, actual, block);
                deterministic &= difference(actual, reference) < 1.e-7;
                matrix &= peak(actual) < 4;
            }
            Audio impulse(unsigned(rate * .8)), response(impulse.size());
            impulse.l[0] = impulse.r[0] = 1;
            p->reset();
            run(*p, impulse, response);
            tail &= peak(response, p->tailSamples() + 1) < 2.e-6;
            Audio silence(4096), zero(4096);
            p->reset();
            run(*p, silence, zero);
            matrix &= peak(zero) == 0;
            input.l[3] = std::numeric_limits<float>::quiet_NaN();
            input.r[4] = std::numeric_limits<float>::infinity();
            p->setParameterFromHost(0, std::numeric_limits<double>::infinity());
            p->reset();
            run(*p, input, reference);
            matrix &= peak(reference) < 4;
        }
        check(matrix,
              "sample rates, silence, impulses, extreme controls and invalid input remain finite");
        check(deterministic, "sample output is invariant across 1/64/257/1024 frame blocks");
        check(tail, "reported tail bounds impulse decay below -114 dBFS");

        auto input = fixture(48000, 1.2, true);
        Audio automated(input.size()), reference(input.size());
        std::array<PluginEvent, 5> events{};
        for (unsigned i = 0; i < events.size(); ++i) {
            events[i].frameOffset = 8191 + i * 6001;
            events[i].paramIndex = i % unsigned(p->parameters().size());
            events[i].value = (i % 2) ? .1 : .9;
        }
        p = make(kind);
        p->activate({48000, 257, true});
        run(*p, input, automated, 257, events);
        p->reset();
        // Reset parameters too: reset() intentionally preserves host values.
        preset(*p, 0);
        p->reset();
        run(*p, input, reference, 1, events);
        check(difference(automated, reference) < 1.e-7,
              "sample-offset automation is independent of block segmentation");
        p = make(kind);
        p->setParameterFromHost(0, 0);
        p->activate({48000, 257, true});
        run(*p, input, output = Audio(input.size()));
        check(difference(input, output) == 0, "zero width/amount is an exact dry pass-through");
        // A low sine exposes discontinuities which broadband audio can hide.
        Audio smoothInput(48000), transitioned(48000);
        for (unsigned n = 0; n < smoothInput.size(); ++n)
            smoothInput.l[n] = smoothInput.r[n] =
                float(.3 * std::sin(2 * dsp::pi * 110 * n / 48000.));
        p = make(kind);
        p->activate({48000, 257, true});
        std::array<PluginEvent, 4> stepEvents{};
        for (const auto &info : p->parameters()) {
            stepEvents[info.index].frameOffset = 24000;
            stepEvents[info.index].paramIndex = info.index;
            stepEvents[info.index].value = info.maxValue;
        }
        run(*p, smoothInput, transitioned, 257, {stepEvents.data(), p->parameters().size()});
        double step = 0;
        for (unsigned n = 23950; n < 26000; ++n)
            step = std::max({step, std::abs(double(transitioned.l[n]) - transitioned.l[n - 1]),
                             std::abs(double(transitioned.r[n]) - transitioned.r[n - 1])});
        check(step < .03,
              "simultaneous parameter jumps do not introduce clicks on a sustained tone");
        // Bypass/stop/reprepare lifecycle keeps memory and telemetry well-defined.
        p->startProcessing();
        check(p->isProcessing(), "processing state follows start");
        p->stopProcessing();
        p->deactivate();
        check(!p->isActive() && !p->isProcessing() && p->latencySamples() == 0,
              "deactivation and zero latency contract");
    }
    {
        DoublerInstance p;
        p.setParameterFromHost(0, 1);
        p.activate({48000, 257, true});
        auto input = fixture(48000, 3);
        Audio output(input.size());
        run(p, input, output);
        const double error = monoError(input, output), corr = correlation(output, 48000);
        std::printf("Doubler mono error %.2f dBFS, correlation %.4f\n",
                    20 * std::log10(std::max(1.e-30, error)), corr);
        check(error < 1.e-6 && corr >= .3 && difference(input, output) > .01,
              "maximum width retains mono below -120 dBFS while audibly widening with correlation "
              ">= .3");
        auto longInput = fixture(48000, 9);
        Audio longA(longInput.size()), longB(longInput.size());
        p.reset();
        run(p, longInput, longA, 1);
        p.reset();
        run(p, longInput, longB, 1024);
        check(difference(longA, longB) == 0,
              "random trajectory knot crossings are deterministic across block sizes");
        // Continuously change all controls, as rapid preset browsing does.
        bool null = true;
        for (unsigned i = 0; i < 10; ++i) {
            preset(p, i);
            run(p, input, output);
            null &= monoError(input, output) < 1.e-6;
        }
        check(null, "mono identity survives all preset transitions without resetting the wet path");
        for (unsigned i = 0; i < input.size(); ++i)
            input.r[i] = -input.l[i];
        p.reset();
        run(p, input, output);
        check(difference(input, output) == 0,
              "pre-existing antiphase stereo is retained, not repaired or further widened");
        PluginBusLayout accepted;
        p.setBusLayout({{1}, {1}}, accepted);
        p.reset();
        for (unsigned i = 0; i < input.size(); ++i)
            input.r[i] = input.l[i];
        run(p, input, output);
        check(difference(input, output) == 0,
              "forced mono disables widening without changing the source");
    }
    check(allocations.load() == 0, "processing and timestamped automation allocate no memory");
    std::printf("Completed in %.2f s; %d failures\n",
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(),
                failures);
    return failures ? 1 : 0;
}
