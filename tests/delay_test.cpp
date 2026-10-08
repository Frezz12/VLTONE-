#include "Internal/DelayInstance.hpp"
#include "Internal/InternalFactory.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <numbers>
#include <tuple>
#include <chrono>
#include <string_view>

using namespace daw::plugins;
using namespace daw::plugins::delay;
namespace { thread_local bool countAllocations = false; unsigned allocations = 0; }
void* operator new(std::size_t n) { if (countAllocations) ++allocations; if (void* p = std::malloc(std::max(n, std::size_t(1)))) return p; throw std::bad_alloc(); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
namespace {
int failures = 0;
void check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); failures += !ok; }
void set(DelayInstance& d, Param p, double v) { d.setParameterFromHost(unsigned(p), v); }
struct Audio { std::vector<float> left, right; };
Audio render(double rate, unsigned block, int character = 0, bool automation = false, bool impulse = false,
             double feedback = 35, int mode = 0, double seconds = .25, double amount = 100, unsigned channels = 2) {
    DelayInstance d; set(d, Param::TimeMode, 2); set(d, Param::TimeMs, 10); set(d, Param::Mix, 100);
    set(d, Param::Feedback, feedback); set(d, Param::Mode, mode); set(d, Param::Character, character); set(d, Param::CharacterAmount, amount);
    d.activate({rate, block, true}); d.startProcessing();
    const unsigned size = unsigned(rate * seconds);
    Audio result{std::vector<float>(size), std::vector<float>(size)};
    std::vector<float> input(block), right(block); const float* inputs[]{input.data(), right.data()};
    for (unsigned at = 0; at < size; at += block) {
        const unsigned frames = std::min(block, size - at);
        for (unsigned i = 0; i < frames; ++i) {
            input[i] = impulse ? (at + i == 0 ? 1.f : 0.f) : float(.35 * std::sin(2 * std::numbers::pi * 733 * (at + i) / rate));
            right[i] = mode == 1 ? input[i] : .5f * input[i];
        }
        std::array<PluginEvent, 7> events{}; unsigned n = 0;
        if (automation) for (const auto [frame, param, v] : std::array<std::tuple<unsigned, unsigned, double>, 7>{{
            {113, 5, 63}, {811, 1, 23}, {1601, 8, 3}, {1719, 8, 5}, {2903, 4, 1}, {3027, 4, 0}, {4901, 10, 400}}}) {
            if (frame >= at && frame < at + frames) { auto& e = events[n++]; e.frameOffset = frame - at; e.paramIndex = param; e.value = v; }
        }
        float* outputs[]{result.left.data() + at, result.right.data() + at};
        PluginProcessContext c; c.inputs = inputs; c.outputs = outputs; c.inputChannels = c.outputChannels = std::uint16_t(channels);
        c.frames = frames; c.inputEvents = {events.data(), n};
        countAllocations = true; d.process(c); countAllocations = false;
    }
    check(d.latencySamples() == 0 && d.tailSamples() >= unsigned(.01 * rate), "zero dry-path latency and nonzero echo tail");
    return result;
}
double difference(const Audio& a, const Audio& b) {
    double error = 0;
    for (unsigned i = 0; i < a.left.size(); ++i) error = std::max({error, double(std::abs(a.left[i] - b.left[i])), double(std::abs(a.right[i] - b.right[i]))});
    return error;
}
}
int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--benchmark") {
        constexpr unsigned block = 128, blocks = 7500;
        std::array<float, block> input{}, left{}, right{};
        for (unsigned i = 0; i < block; ++i) input[i] = float(.3 * std::sin(i * .13));
        const float* inputs[]{input.data(), input.data()}; float* outputs[]{left.data(), right.data()};
        PluginProcessContext c; c.inputs = inputs; c.outputs = outputs; c.inputChannels = c.outputChannels = 2; c.frames = block;
        for (unsigned character = 0; character < kCharacterCount; ++character) {
            DelayInstance d; set(d, Param::Character, character); d.activate({48000, block}); d.startProcessing();
            for (int i = 0; i < 500; ++i) d.process(c);
            const auto start = std::chrono::steady_clock::now();
            for (unsigned i = 0; i < blocks; ++i) d.process(c);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            std::printf("%.*s: %.3f ms for 20 s stereo, %.3f%% of one core, %.3f us/block\n",
                int(characterName(character).size()), characterName(character).data(), ms, ms / 200, ms * 1000 / blocks);
        }
        return 0;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    InternalFactory factory; auto plugin = factory.create(DelayInstance::staticDescriptor());
    check(plugin && plugin->descriptor().name == "Classic Delay" && plugin->descriptor().uid == "daw.delay" && plugin->descriptor().category == "Effect|Delay" && plugin->parameters().size() == 14, "Classic Delay is registered with its stable UID and 14 parameters");
    bool ids = true; for (const auto& p : parameterTable()) ids &= p.isAutomatable && plugin->parameterIndexForId(p.id) == int(p.index) && plugin->parameterValue(p.index) == p.defaultValue;
    check(ids, "parameter metadata, defaults and IDs agree");
    for (const auto& p : parameterTable()) plugin->setParameterFromHost(p.index, p.maxValue);
    std::vector<std::uint8_t> saved; plugin->saveState(saved); DelayInstance restored;
    bool state = restored.loadState(saved); for (const auto& p : parameterTable()) state &= restored.parameterValue(p.index) == p.maxValue;
    check(state, "all values survive opaque state save/load");
    const std::string bad = R"({"version":1,"params":{"feedback":12,"character":"bad"}})";
    check(!restored.loadState({reinterpret_cast<const std::uint8_t*>(bad.data()), bad.size()}) && restored.parameterValue(5) == 95, "invalid state is rejected atomically");
    PluginBusLayout accepted; check(restored.setBusLayout({{1},{1}}, accepted) && !restored.setBusLayout({{2,2},{2}}, accepted), "mono/stereo effect does not advertise a sidechain");
    std::array<double, kParameterCount> p{}; for (const auto& param : parameterTable()) p[param.index] = param.defaultValue;
    daw::engine::TransportInfo transport;
    check(timing(p, transport).milliseconds == 375, "default host eighth dotted is 375 ms at 120 BPM");
    bool divisions = true;
    for (unsigned i = 0; i < 21; ++i) {
        p[2] = i; const double q = std::exp2(double(i / 3) - 4) * (i % 3 == 1 ? 1.5 : i % 3 == 2 ? 2. / 3 : 1);
        divisions &= std::abs(timing(p, transport).milliseconds - 500 * q) < 1.e-9 && !divisionName(i).empty();
    }
    check(divisions, "all straight, dotted and triplet divisions have musical lengths");
    p[2] = 22; transport.timeSigNumerator = 6; transport.timeSigDenominator = 8;
    check(timing(p, transport).milliseconds == 3000, "two bars respect 6/8 meter");
    transport.tempo = 20; transport.timeSigNumerator = transport.timeSigDenominator = 4;
    check(timing(p, transport).milliseconds == 8000 && timing(p, transport).requestedMs == 24000, "slow host tempo exposes the 8 second limit");
    p[0] = 1; p[3] = 240; p[2] = 12; check(timing(p, transport).milliseconds == 250, "local BPM is independent of host tempo");
    for (double rate : {44100.,48000.,96000.}) {
        const unsigned delay = unsigned(rate * .01);
        const auto echo = render(rate, 257, 0, false, true, 50, 0, .08);
        check(std::abs(echo.left[delay] - 1) < 1.e-6 && std::abs(echo.left[2*delay] - .5) < 1.e-6 && std::abs(echo.left[3*delay] - .25) < 1.e-6,
              "impulse repeats land on time and decay geometrically");
        const auto ping = render(rate, 128, 0, false, true, 50, 1, .08);
        check(ping.left[delay] == 1 && ping.right[delay] == 0 && ping.left[2*delay] == 0 && ping.right[2*delay] == .5f, "ping pong alternates left/right");
        const auto mono = render(rate, 128, 0, false, true, 50, 1, .08, 100, 1);
        check(mono.left[delay] == 1 && mono.left[2*delay] == .5f, "mono ping pong retains repeat level and interval");
        const auto reference = render(rate, 1, 0, true);
        for (unsigned block : {17u,128u,257u,1024u}) check(difference(reference, render(rate, block, 0, true)) < 2.e-6, "sample-offset automation is independent of block size");
    }
    const auto clean = render(48000, 257);
    std::array<Audio, 7> characters;
    for (unsigned c = 0; c < kCharacterCount; ++c) {
        characters[c] = render(48000, 257, int(c));
        check(difference(clean, render(48000, 257, int(c), false, false, 35, 0, .25, 0)) == 0, "Amount 0 removes character without changing delay");
        check(difference(characters[c], render(48000, 257, int(c))) == 0, "character renders deterministically");
        const auto tail = render(48000, 257, int(c), false, true, 95, 0, 3.5);
        bool silent = true; for (unsigned i = unsigned(tail.left.size()) - 4800; i < tail.left.size(); ++i) silent &= std::isfinite(tail.left[i]) && std::abs(tail.left[i]) < 1.e-5;
        check(silent, "95 percent feedback decays to silence for this character");
    }
    bool distinct = true; for (unsigned a = 0; a < 7; ++a) for (unsigned b = a+1; b < 7; ++b) distinct &= difference(characters[a], characters[b]) > .01;
    check(distinct, "all seven characters are audibly distinct signal paths");
    DelayInstance dry; set(dry, Param::Mix, 0); dry.activate({48000, 128});
    std::array<float,128> in{}, out{}; for (unsigned i = 0; i < in.size(); ++i) in[i] = float(std::sin(i * .3));
    const float* inputs[]{in.data()}; float* outputs[]{out.data()}; PluginProcessContext context;
    context.inputs = inputs; context.outputs = outputs; context.inputChannels = context.outputChannels = 1; context.frames = 128;
    dry.process(context); check(in == out, "Mix 0 and Output 0 reproduce the dry samples exactly");
    for (unsigned i = 0; i < kParameterCount; ++i) dry.setParameterFromHost(i, std::numeric_limits<double>::infinity());
    in.fill(std::numeric_limits<float>::quiet_NaN()); dry.process(context);
    check(std::all_of(out.begin(), out.end(), [](float x) { return std::isfinite(x); }), "nonfinite input and parameter values cannot poison output");
    check(allocations == 0, "process performs no heap allocations");
    return failures ? 1 : 0;
}
