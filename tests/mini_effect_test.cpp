#include "Internal/MiniModuleInstance.hpp"
#include "Internal/MiniEffectInstance.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <numbers>
#include <nlohmann/json.hpp>

namespace {
std::atomic<bool> counting{false};
std::atomic<unsigned> allocations{0};
}
void* operator new(std::size_t n) {
    if (counting) ++allocations;
    if (auto* p = std::malloc(std::max(n, std::size_t(1)))) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
namespace {
using namespace daw::plugins;
using namespace daw::plugins::mini;
constexpr double pi = std::numbers::pi, rate = 48000;
int failures = 0;
void check(bool ok, const char* label) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label); failures += !ok;
}
struct Audio {
    std::vector<float> l, r, ol, oright;
    explicit Audio(unsigned size) : l(size), r(size), ol(size), oright(size) {}
    bool run(PluginInstance& p, unsigned block = 128, unsigned channels = 2) {
        bool ok = true;
        for (unsigned at = 0; at < l.size(); at += block) {
            const float* in[]{l.data() + at, r.data() + at};
            float* out[]{ol.data() + at, oright.data() + at};
            PluginProcessContext c; c.inputs = in; c.outputs = out;
            c.inputChannels = c.outputChannels = std::uint16_t(channels);
            c.frames = std::min(block, unsigned(l.size()) - at); c.sampleTime = at;
            ok &= p.process(c) != PluginProcessDisposition::Error;
        }
        return ok;
    }
    void sine(double hz, double amplitude = .2) {
        for (unsigned i = 0; i < l.size(); ++i) l[i] = r[i] = float(amplitude * std::sin(2 * pi * hz * i / rate));
    }
};
bool prepare(MiniModuleInstance& p, std::string_view id, std::string_view mode = {},
             double first = NAN, double second = NAN, double sr = rate, unsigned ch = 2) {
    if (!p.configure(builtin(id), 0, mode)) return false;
    if (std::isfinite(first)) p.setParameterFromHost(0, first);
    if (std::isfinite(second)) p.setParameterFromHost(1, second);
    PluginBusLayout accepted;
    return p.setBusLayout({{std::uint16_t(ch)}, {std::uint16_t(ch)}}, accepted) && p.activate({sr, 256});
}
double energy(const std::vector<float>& v, unsigned start) {
    double sum = 0; for (unsigned i = start; i < v.size(); ++i) sum += v[i] * double(v[i]);
    return sum / (v.size() - start);
}
double component(const std::vector<float>& v, double hz, unsigned begin = 24000) {
    std::complex<double> sum{};
    for (unsigned i = begin; i < v.size(); ++i) sum += double(v[i]) * std::polar(1., -2 * pi * hz * i / rate);
    return 2 * std::abs(sum) / (v.size() - begin);
}
double toneFrequency(const std::vector<float>& v) {
    double first = 0, last = 0; unsigned crossings = 0;
    for (unsigned i = 24001; i < v.size(); ++i) if (v[i-1] <= 0 && v[i] > 0) {
        last = i - 1 + double(-v[i-1]) / (v[i] - v[i-1]);
        if (!crossings++) first = last;
    }
    return crossings > 1 ? (crossings - 1) * rate / (last - first) : 0;
}
void matrix() {
    bool valid = true, finite = true, allocationFree = true, latency = true, typed = true;
    for (const auto& type : effectTypes()) {
        auto definition = builtin(type.id);
        valid &= validate(definition).empty() && fromJson(toJson(definition)) == definition;
        auto modes = definition.modes;
        if (modes.empty()) modes.push_back({});
        for (const auto& mode : modes) {
            for (double sr : {8000., 44100., 48000., 96000., 192000., 384000.}) for (unsigned ch : {1u, 2u}) {
                MiniModuleInstance p;
                valid &= prepare(p, type.id, mode.id, NAN, NAN, sr, ch);
                latency &= type.kind == EffectKind::Pitch ? p.latencySamples() > 0 : p.latencySamples() == 0;
                Audio audio(513); audio.sine(440);
                allocations = 0; counting = true;
                finite &= audio.run(p, 37, ch);
                p.reset();
                counting = false; allocationFree &= allocations == 0;
                for (float v : audio.ol) finite &= std::isfinite(v) && std::abs(v) < 10;
            }
            MiniModuleInstance saved, restored;
            valid &= prepare(saved, type.id, mode.id);
            std::vector<std::uint8_t> state;
            valid &= saved.saveState(state) && restored.loadState(state) && restored.mode() == mode.id;
        }
        auto graph = resolved(definition); graph.version = 3;
        graph.controls.clear();
        for (auto& edge : graph.connections) { edge.fromPort = "out"; edge.toPort = "in"; }
        MiniModuleInstance creator;
        if (!validate(graph).empty()) std::printf("CREATOR %s: %s\n", type.id.data(), validate(graph).c_str());
        typed &= validate(graph).empty() && creator.configure(graph) && creator.activate({rate, 128});
        Audio audio(1024); audio.sine(1000);
        typed &= audio.run(creator);
    }
    check(valid, "all new modules/modes prepare, validate and serialize at 8–384 kHz, mono/stereo");
    check(finite && allocationFree, "all new DSP and resets are finite and allocate nothing on the audio thread");
    check(latency, "only Pitch/Formant reports nonzero latency");
    check(typed, "new effects also execute as typed Creator nodes");
}
void stereoAndTremolo() {
    Audio a(48000); a.sine(317);
    for (unsigned i = 0; i < a.r.size(); ++i) a.r[i] = float(.1 * std::sin(2 * pi * 731 * i / rate));
    MiniModuleInstance stereo; prepare(stereo, "stereo", {}, 200, 0); a.run(stereo);
    double error = 0;
    for (unsigned i = 0; i < a.l.size(); ++i) error = std::max(error, std::abs(double(a.ol[i]) + a.oright[i] - a.l[i] - a.r[i]));
    check(error < 1e-7, "Stereo preserves the mono sum at 200% width without time/phase offsets");
    for (unsigned i = 0; i < a.l.size(); ++i) a.r[i] = -.8f * a.l[i];
    prepare(stereo, "stereo", {}, 200, 100); a.run(stereo);
    double sideIn = 0, sideOut = 0;
    for (unsigned i = 24000; i < a.l.size(); ++i) { sideIn += std::pow(a.l[i] - a.r[i], 2); sideOut += std::pow(a.ol[i] - a.oright[i], 2); }
    check(sideOut <= sideIn * 1.001, "Stereo Safe avoids increasing an already anti-correlated side signal");
    a.sine(401); prepare(stereo, "stereo", {}, 200, 0); a.run(stereo);
    check(a.ol == a.l && a.oright == a.r, "Stereo never invents side information for a mono source");
    MiniModuleInstance tremolo; prepare(tremolo, "tremolo", "sine", 4, 100);
    std::fill(a.l.begin(), a.l.end(), 1.f); a.r = a.l; a.run(tremolo);
    const auto [lo, hi] = std::minmax_element(a.ol.begin(), a.ol.end());
    check(*lo < 1e-6 && *hi == 1 && a.ol == a.oright && std::abs(component(a.ol, 4) - .5) < .001,
          "Tremolo has the requested rate/depth and linked stereo phase");
}
void filterAndClip() {
    bool response = true;
    for (auto mode : {"6db", "12db", "24db", "48db"}) for (bool high : {false, true}) {
        MiniModuleInstance filter; prepare(filter, "filter", mode, high ? 1000 : 20, high ? 20000 : 1000);
        Audio a(48000); a.sine(1000); a.run(filter);
        const double db = 10 * std::log10(energy(a.ol, 24000) / energy(a.l, 24000));
        response &= std::abs(db + 3.0103) < .03;
        filter.reset(); a.sine(high ? 500 : 2000); a.run(filter);
        const unsigned order = std::string_view(mode) == "6db" ? 1 : std::string_view(mode) == "12db" ? 2 : std::string_view(mode) == "24db" ? 4 : 8;
        const double ratio = high ? std::tan(pi*1000/rate)/std::tan(pi*500/rate)
                                  : std::tan(pi*2000/rate)/std::tan(pi*1000/rate);
        const double expected = -10*std::log10(1+std::pow(ratio, 2*order));
        const double actual = 10*std::log10(energy(a.ol, 24000)/energy(a.l, 24000));
        response &= std::abs(actual-expected) < .05;
    }
    check(response, "all four Butterworth slopes have a -3 dB cutoff on both filter sides");
    MiniModuleInstance filter; prepare(filter, "filter"); Audio a(48000); a.sine(997); a.run(filter);
    check(a.ol == a.l, "Filter endpoints are an exact dry path");
    MiniModuleInstance clipper; prepare(clipper, "soft-clip");
    for (unsigned i = 0; i < a.l.size(); ++i) a.l[i] = float(-4. + 8. * i / (a.l.size() - 1));
    a.r = a.l; a.run(clipper);
    bool monotonic = true, linear = true;
    double oddError = 0;
    for (unsigned i = 0; i < a.l.size(); ++i) {
        if (i) monotonic &= a.ol[i] >= a.ol[i-1];
        if (std::abs(a.l[i]) < .49) linear &= a.ol[i] == a.l[i];
        oddError = std::max(oddError, std::abs(double(a.ol[i]) + a.ol[a.ol.size()-1-i]));
    }
    check(monotonic && linear && oddError < 1e-6 && a.ol.front() == -1 && a.ol.back() == 1,
          "Soft Clip is monotonic, symmetric, bounded and linear below threshold");
}
void pitch() {
    unsigned previous = 0;
    for (auto mode : {"live", "pro"}) {
        MiniModuleInstance p; prepare(p, "pitch-formant", mode);
        const unsigned latency = p.latencySamples();
        std::printf("LATENCY %s: %u samples, %.3f ms at 48 kHz\n", mode, latency, latency / 48.);
        check(latency > previous, "Pro has a larger analysis window than Live"); previous = latency;
        Audio a(96000); a.sine(220); a.run(p, 37);
        bool exact = true;
        for (unsigned i = 0; i < a.l.size(); ++i) exact &= a.ol[i] == (i < latency ? 0 : a.l[i-latency]);
        check(exact, "neutral pitch/formant preserves the waveform at the reported latency");
        prepare(p, "pitch-formant", mode, 12, 0); a.run(p);
        const double shifted = component(a.ol, 440), original = component(a.ol, 220);
        std::printf("PITCH %s: 440 Hz %.5f, residual 220 Hz %.5f\n", mode, shifted, original);
        const double hz = toneFrequency(a.ol), cents = 1200*std::log2(hz/440);
        std::printf("PITCH ACCURACY %s: %.4f Hz, %.3f cents\n", mode, hz, cents);
        check(std::abs(cents) < (std::string_view(mode) == "pro" ? 3 : 15),
              "pitch frequency stays within the measured Live/Pro tolerance");
        check(energy(a.ol, 24000) > .01 && shifted > 10 * original && a.ol == a.oright,
              "octave shift changes pitch and keeps identical stereo channels aligned");
        MiniModuleInstance split; prepare(split, "pitch-formant", mode, 12, 0);
        Audio b = a; b.run(split, 37);
        double difference = 0;
        for (unsigned i = 0; i < a.ol.size(); ++i) difference = std::max(difference, std::abs(double(a.ol[i]) - b.ol[i]));
        check(difference < 1e-5, "pitch rendering is independent of host block partition");
        // A harmonic voice with a known broad formant near 900 Hz. Moving only
        // the formant must move the envelope, while preserving the 100 Hz grid.
        for (unsigned i = 0; i < a.l.size(); ++i) {
            double sample = 0;
            for (unsigned h = 1; h <= 40; ++h) {
                const double envelope = .15 + std::exp(-.5 * std::pow((h * 100. - 900) / 230, 2));
                sample += .035 * envelope * std::sin(2 * pi * h * 100 * i / rate);
            }
            a.l[i] = a.r[i] = float(sample);
        }
        prepare(p, "pitch-formant", mode, 0, 7); a.run(p);
        double peakHz = 0, peakAmplitude = 0;
        for (unsigned h = 1; h <= 35; ++h) {
            const double amplitude = component(a.ol, h * 100.);
            if (amplitude > peakAmplitude) { peakAmplitude = amplitude; peakHz = h * 100.; }
        }
        std::printf("FORMANT %s: envelope peak %.1f Hz (source peak 900 Hz, target 1348 Hz)\n", mode, peakHz);
        check(peakHz >= 1200 && peakHz <= 1500 && component(a.ol, peakHz + 50) < .05 * peakAmplitude,
              "formant knob moves the vowel resonance while preserving the harmonic pitch grid");
    }
}
void automationAndAir() {
    bool continuous = true, finite = true;
    for (const auto& type : effectTypes()) {
        MiniModuleInstance whole, split;
        prepare(whole, type.id); prepare(split, type.id);
        Audio a(256); a.sine(997); Audio b = a;
        const auto params = whole.parameters();
        std::array<PluginEvent, 2> edits{};
        for (unsigned p = 0; p < 2; ++p) {
            edits[p].kind = PluginEvent::Kind::ParamValue;
            edits[p].frameOffset = 73; edits[p].paramIndex = p;
            edits[p].value = std::lerp(params[p].minValue, params[p].maxValue, .7);
        }
        const float* input[]{a.l.data(), a.r.data()}; float* output[]{a.ol.data(), a.oright.data()};
        PluginProcessContext c; c.inputs = input; c.outputs = output;
        c.inputChannels = c.outputChannels = 2; c.frames = 256; c.inputEvents = edits;
        whole.process(c);
        const float* first[]{b.l.data(), b.r.data()}; float* firstOut[]{b.ol.data(), b.oright.data()};
        c.inputs = first; c.outputs = firstOut; c.frames = 73; c.inputEvents = {}; split.process(c);
        for (const auto& e : edits) split.setParameterFromHost(e.paramIndex, e.value);
        const float* rest[]{b.l.data()+73, b.r.data()+73}; float* restOut[]{b.ol.data()+73,b.oright.data()+73};
        c.inputs = rest; c.outputs = restOut; c.frames = 183; split.process(c);
        for (unsigned i = 0; i < 256; ++i) {
            continuous &= std::abs(a.ol[i]-b.ol[i]) < 1e-6;
            finite &= std::isfinite(a.ol[i]);
        }
    }
    check(continuous && finite, "sample-offset automation matches split rendering for every new effect");
    MiniModuleInstance air; prepare(air, "air"); Audio a(48000); a.sine(8000); a.run(air);
    check(a.ol == a.l, "Air at zero is bit-exact dry audio");
    prepare(air, "air", {}, 50, 50); a.run(air);
    check(energy(a.ol, 24000) > energy(a.l, 24000) * 1.1 && a.ol == a.oright,
          "Air adds high-frequency energy without unlinking stereo dynamics");
}
}
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    matrix(); stereoAndTremolo(); filterAndClip(); pitch(); automationAndAir();
    return failures ? 1 : 0;
}
