#pragma once

#include "Host/PluginTypes.hpp"
#include <array>
#include <optional>
#include <string>
#include <vector>

namespace daw::plugins::flux {
inline constexpr unsigned slotCount = 12, sceneCount = 16, fieldCount = 16;
inline constexpr unsigned maxPoints = 256, maxSteps = 64, maxRoutes = 64;
enum class Kind : unsigned {
  Empty,
  Volume,
  Dynamics,
  Filter,
  Vowel,
  Drive,
  Crush,
  Noise,
  Time,
  Repeat,
  Tape,
  Pitch,
  Granular,
  Spectral,
  Resonator,
  Ring,
  Chorus,
  Flanger,
  Phaser,
  Stereo,
  Delay,
  Reverb,
  Vocoder,
  Count
};
enum class Routing : unsigned { Serial, Parallel, Multiband };
enum class Lane : unsigned { Pre, A, B, C, Post };
enum class Shape : unsigned { Hold, Linear, Smooth, Exponential, Logarithmic };
enum class TriggerMode : unsigned { Host, Midi, Audio, Sidechain, Manual };
enum class PlayMode : unsigned { Loop, OneShot, Held };
enum class Direction : unsigned { Forward, Reverse, PingPong, Random };
enum Global : unsigned {
  Input,
  Output,
  Mix,
  Bypass,
  Protect,
  Macro1,
  Macro2,
  Macro3,
  Macro4,
  X,
  Y,
  SceneSelect,
  Quantize,
  ManualTrigger,
  Panic,
  LowSplit,
  HighSplit,
  Balance,
  LowGain,
  MidGain,
  HighGain,
  LowMute,
  MidMute,
  HighMute,
  LowSolo,
  MidSolo,
  HighSolo,
  GlobalCount
};
enum Common : unsigned {
  Enabled,
  SlotMix,
  SlotOutput,
  Solo,
  DepthA,
  DepthB,
  RateA,
  RateB,
  CommonCount
};
inline constexpr unsigned bankSize =
    CommonCount + (unsigned(Kind::Count) - 1) * fieldCount;
inline constexpr unsigned parameterCount = GlobalCount + slotCount * bankSize;
constexpr bool canModulate(unsigned id) {
  return id < parameterCount && (id < SceneSelect || id > Panic);
}
constexpr unsigned commonParam(unsigned slot, unsigned field) {
  return GlobalCount + slot * bankSize + field;
}
constexpr unsigned effectParam(unsigned slot, Kind kind, unsigned field) {
  return commonParam(slot, CommonCount) + (unsigned(kind) - 1) * fieldCount +
         field;
}
struct Field {
  const char *id;
  const char *name;
  const char *unit;
  double min, max, initial;
  bool stepped = false, logarithmic = false;
  const char *choices = "";
};
std::span<const Field> fields(Kind);
const char *kindName(Kind) noexcept;
const char *kindId(Kind) noexcept;
std::span<const ParameterInfo> parameterTable();
double normalized(const Field &, double) noexcept;
double plain(const Field &, double) noexcept;

struct Point {
  bool operator==(const Point &) const = default;
  float x = 0, y = 1, tension = 0;
  Shape shape = Shape::Smooth;
};
struct Curve {
  bool operator==(const Curve &) const = default;
  std::vector<Point> points{{0, 1, 0, Shape::Linear}, {1, 1, 0, Shape::Linear}};
  unsigned target = 0;
  double depth = 0, bars = 1, hz = 1, phase = 0;
  bool sync = true, bipolar = false;
  PlayMode play = PlayMode::Loop;
  TriggerMode trigger = TriggerMode::Host;
};
double evaluate(const Curve &, double phase) noexcept;
void makeShape(Curve &, unsigned shape, unsigned seed = 1);
void transformCurve(Curve &, unsigned operation);
struct Step {
  bool operator==(const Step &) const = default;
  float amount = 1, probability = 1;
  unsigned ratchet = 1;
  bool enabled = true, retrigger = false;
};
struct Sequence {
  bool operator==(const Sequence &) const = default;
  std::array<Step, maxSteps> steps{};
  unsigned length = 16;
  double division = .25, swing = 0;
  bool enabled = false;
  Direction direction = Direction::Forward;
};
void euclidean(Sequence &, unsigned pulses, unsigned rotation);
struct SlotState {
  bool operator==(const SlotState &) const = default;
  std::array<double, fieldCount> values{};
  bool enabled = true, solo = false;
  double mix = 1, output = 0;
  std::array<Curve, 2> curves;
  Sequence sequence;
};
struct Modulator {
  bool operator==(const Modulator &) const = default;
  unsigned type = 0, shape = 0; // LFO, steps, random, follower
  double rate = 1, phase = 0, attack = .01, release = .15, smooth = .02;
  bool sync = true, sidechain = false;
  std::array<float, 16> steps{};
};
// Sources: macros 0..3, XY 4..5, global modulators 6..9, velocity 10,
// input envelope 11, sidechain envelope 12, slot Motion A/B 13..36.
struct ModRoute {
  bool operator==(const ModRoute &) const = default;
  unsigned source = 0, target = Mix;
  double amount = 0, low = 0, high = 1;
  bool bipolar = false, enabled = true;
};
struct Scene {
  bool operator==(const Scene &) const = default;
  std::string name = "Scene 1";
  std::array<SlotState, slotCount> modules;
  std::array<Modulator, 4> modulators;
  std::vector<ModRoute> routes;
  std::array<double, 4> macros{.5, .5, .5, .5};
};
struct Slot {
  bool operator==(const Slot &) const = default;
  Kind kind = Kind::Empty;
  Lane lane = Lane::A;
  bool locked = false;
};
struct PathPoint {
  bool operator==(const PathPoint &) const = default;
  float time = 0, x = .5f, y = .5f;
};
struct Detector {
  bool operator==(const Detector &) const = default;
  double threshold = -24, sensitivity = 1, attack = .002, release = .12,
         holdoff = .08, hysteresis = 6, highpass = 30, lowpass = 12000;
};
struct MidiMapping {
  bool operator==(const MidiMapping &) const = default;
  unsigned controller = 1, parameter = Macro1, channel = 0;
};
struct FluxState {
  bool operator==(const FluxState &) const = default;
  std::array<Slot, slotCount> modules{};
  std::array<unsigned, slotCount> order{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
  std::array<double, GlobalCount> globals{};
  std::vector<Scene> scenes{Scene{}};
  Routing routing = Routing::Serial;
  bool studio = false;
  unsigned seed = 1, editScene = 0, sceneMode = 0, midiSceneChannel = 15;
  bool midiScenes = false;
  Detector detector;
  std::array<std::string, 4> macroNames{"Intensity", "Movement", "Texture",
                                        "Space"};
  std::vector<PathPoint> path;
  double pathBars = 1, pathSmooth = .01;
  bool pathEnabled = false, pathReverse = false;
  std::vector<MidiMapping> midi;
  std::vector<unsigned> parameterLocks;
  std::string presetId = "init", name = "Init", category, description, tags;
};
using State = FluxState;
State initialState();
void setKind(State &, unsigned slot, Kind);
Scene &ensureScene(State &, unsigned);
bool validState(const State &) noexcept;
bool encodeState(const State &, std::vector<std::uint8_t> &);
std::optional<State> decodeState(std::span<const std::uint8_t>);
struct FactoryPreset {
  const char *name;
  const char *category;
  const char *description;
  const char *tags;
};
std::span<const FactoryPreset> factoryPresets() noexcept;
State factoryState(unsigned);
// Scope bits: parameter values (1), curves (2), sequences (4), complete sound
// including topology, scenes and modulation (8). No preset file is written.
inline constexpr unsigned randomAll = 15;
void randomize(State &, unsigned seed, unsigned scope = 1);
std::uint32_t hash(std::uint32_t) noexcept;
double randomAt(std::uint32_t seed, std::int64_t event,
                unsigned salt = 0) noexcept;
} // namespace daw::plugins::flux
