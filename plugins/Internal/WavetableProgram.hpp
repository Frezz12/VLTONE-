#pragma once
#include "Host/PluginTypes.hpp"
#include <array>
#include <optional>
#include <string>
#include <vector>

namespace daw::plugins::wavetable {
inline constexpr std::string_view uid = "daw.wavetable";
inline constexpr unsigned oscillatorCount = 5, envelopeCount = 4, lfoCount = 10;
inline constexpr unsigned macroCount = 8, maxRoutes = 64, maxVoices = 64;
inline constexpr unsigned frameSize = 2048, maxFrames = 256, fxCount = 24;
enum class OscMode : unsigned { Wavetable, Sample, Multisample, Granular, Spectral };
enum class Warp : unsigned { Off, Sync, Bend, Pulse, Mirror, FM, RM, Fold, Quantize };
enum Global : unsigned { Output, Polyphony, Mono, Legato, Glide, BendRange, Velocity, FilterRouting,
    ArpEnabled, ArpRate, ArpGate, ArpOctaves, ArpDirection, ClipEnabled, ClipSelect, Swing,
    Macro1, GlobalCount = Macro1 + macroCount };
enum OscParam : unsigned { On, Mode, Level, Pan, Octave, Semitone, Fine, Unison, Detune, Spread,
    Phase, RandomPhase, Position, WarpMode, WarpAmount, Warp2Mode, Warp2Amount,
    FilterSend, Start, End, Loop, Rate, GrainSize, GrainDensity, GrainSpray, OscParamCount };
enum FilterParam : unsigned { FilterOn, FilterType, Cutoff, Resonance, Drive, FilterMix, FilterParamCount };
enum EnvParam : unsigned { Attack, Hold, Decay, Sustain, Release, EnvCurve, EnvParamCount };
enum LfoParam : unsigned { LfoRate, LfoSync, LfoPhase, LfoRetrigger, LfoBipolar, LfoParamCount };
inline constexpr unsigned oscBase = GlobalCount, filterBase = oscBase + oscillatorCount * OscParamCount;
inline constexpr unsigned envBase = filterBase + 2 * FilterParamCount;
inline constexpr unsigned lfoBase = envBase + envelopeCount * EnvParamCount;
inline constexpr unsigned fxBase = lfoBase + lfoCount * LfoParamCount;
inline constexpr unsigned fxParamCount = 18, parameterCount = fxBase + fxCount * fxParamCount;
constexpr unsigned osc(unsigned i, unsigned p) { return oscBase + i * OscParamCount + p; }
constexpr unsigned filter(unsigned i, unsigned p) { return filterBase + i * FilterParamCount + p; }
constexpr unsigned env(unsigned i, unsigned p) { return envBase + i * EnvParamCount + p; }
constexpr unsigned lfo(unsigned i, unsigned p) { return lfoBase + i * LfoParamCount + p; }
constexpr unsigned fx(unsigned i, unsigned p) { return fxBase + i * fxParamCount + p; }
// FX fields: 0 enabled, 1 wet mix, 2..17 the selected processor's plain values.
struct Point { float x = 0, y = 0, curve = 0; bool operator==(const Point&) const = default; };
struct Curve { std::vector<Point> points{{0, 0, 0}, {.5f, 1, 0}, {1, 0, 0}}; bool operator==(const Curve&) const = default; };
enum class Source : unsigned { Env1, Env2, Env3, Env4, Lfo1, Lfo10 = Lfo1 + 9,
    Macro1, Macro8 = Macro1 + 7, Velocity, Note, ModWheel, Pressure, Random, Pitch, Count };
struct ModRoute {
    Source source = Source::Env2;
    std::string target = "filter.1.cutoff";
    double amount = 0;
    bool bipolar = false, enabled = true;
    Curve response{{{0, 0, 0}, {1, 1, 0}}};
    bool operator==(const ModRoute&) const = default;
};
struct Resource {
    std::string id, name, sourcePath;
    unsigned channels = 1, sampleRate = 48000, frameLength = frameSize;
    std::vector<float> samples; // interleaved source data; control thread only
    bool operator==(const Resource&) const = default;
};
struct Region {
    std::string resource;
    int lowKey = 0, highKey = 127, rootKey = 60, lowVelocity = 0, highVelocity = 127;
    bool operator==(const Region&) const = default;
};
struct Oscillator {
    std::string resource; // empty selects the original built-in table bank
    unsigned bank = 0;
    std::vector<Region> regions;
    bool operator==(const Oscillator&) const = default;
};
struct Effect { unsigned kind = 0, bus = 0; bool operator==(const Effect&) const = default; };
struct ClipNote {
    double beat = 0, length = .25, velocity = .8;
    int key = 60;
    bool operator==(const ClipNote&) const = default;
};
struct Clip {
    std::string name;
    double length = 4;
    std::vector<ClipNote> notes;
    bool operator==(const Clip&) const = default;
};
struct WavetableState {
    std::array<double, parameterCount> values{};
    std::array<Oscillator, oscillatorCount> oscillators;
    std::array<Curve, lfoCount> lfos;
    std::array<Effect, fxCount> effects;
    std::array<Clip, 12> clips;
    std::vector<ModRoute> routes;
    std::vector<Resource> resources;
    std::array<std::string, macroCount> macroNames{"Tone", "Movement", "Texture", "Space", "Color", "Width", "Attack", "Release"};
    std::string name = "Init", category, description, author = "VLTONE", importReport;
    unsigned seed = 1;
    bool operator==(const WavetableState&) const = default;
};
using State = WavetableState;
std::span<const ParameterInfo> parameterTable();
int parameterIndex(std::string_view) noexcept;
State initialState();
bool validState(const State&, std::string* error = nullptr);
bool encodeState(const State&, std::vector<std::uint8_t>&);
std::optional<State> decodeState(std::span<const std::uint8_t>);
float curveValue(const Curve&, double phase) noexcept;
std::uint32_t hash(std::uint32_t) noexcept;
double unit(std::uint32_t&) noexcept;
struct FactoryPreset { std::string name, category, description; };
std::span<const FactoryPreset> factoryPresets();
State factoryState(unsigned);
} // namespace daw::plugins::wavetable
