#include "Internal/FluxProgram.hpp"
#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>
#include <numbers>

namespace daw::plugins::flux {
namespace {
using F = Field;
const std::array<std::vector<F>, unsigned(Kind::Count)> &definitions() {
  static const std::array<std::vector<F>, unsigned(Kind::Count)> d{
      {{},
       {{"gain", "Gain", "%", 0, 1, 1},
        {"smooth", "Smoothing", "ms", .1, 500, 2}},
       {{"threshold", "Threshold", "dB", -60, 0, -18},
        {"ratio", "Ratio", "", 1, 20, 4},
        {"attack", "Attack", "ms", .1, 500, 10, false, true},
        {"release", "Release", "ms", 5, 2000, 120, false, true},
        {"knee", "Knee", "dB", 0, 24, 6},
        {"mode", "Mode", "", 0, 2, 0, true, false,
         "Compressor|Expander|Transient"},
        {"transient", "Attack gain", "dB", -24, 24, 6},
        {"sustain", "Sustain", "dB", -24, 24, 0},
        {"external", "External detector", "", 0, 1, 0, true}},
       {{"cutoff", "Cutoff", "Hz", 20, 20000, 1000, false, true},
        {"resonance", "Resonance", "", .5, 12, .707, false, true},
        {"mode", "Mode", "", 0, 7, 0, true, false,
         "Low pass|High pass|Band pass|Notch|Peak|Low shelf|High shelf|Dual"},
        {"slope", "Slope", "", 0, 1, 1, true, false, "12 dB|24 dB"},
        {"model", "Model", "", 0, 1, 0, true, false, "Clean|Ladder"},
        {"drive", "Drive", "dB", 0, 30, 0},
        {"gain", "Gain", "dB", -24, 24, 6},
        {"upper", "Upper cutoff", "Hz", 30, 20000, 8000, false, true}},
       {{"vowel", "Vowel", "", 0, 4, 0},
        {"resonance", "Resonance", "", 1, 12, 4},
        {"width", "Width", "%", .2, 2, 1},
        {"shift", "Shift", "st", -12, 12, 0}},
       {{"drive", "Drive", "dB", 0, 36, 6},
        {"mode", "Mode", "", 0, 5, 0, true, false,
         "Soft clip|Hard clip|Tube|Tape|Wavefold|Rectifier"},
        {"bias", "Bias", "", -.5, .5, 0},
        {"tone", "Tone", "Hz", 200, 20000, 14000, false, true},
        {"compensate", "Auto gain", "", 0, 1, 1, true}},
       {{"bits", "Bits", "bit", 1, 24, 12},
        {"rate", "Sample rate", "Hz", 80, 48000, 16000, false, true},
        {"jitter", "Jitter", "%", 0, 1, 0},
        {"dither", "Dither", "%", 0, 1, 0},
        {"filter", "Filter", "Hz", 100, 20000, 16000, false, true}},
       {{"level", "Level", "dB", -60, 0, -24},
        {"color", "Color", "", 0, 4, 0, true, false,
         "White|Pink|Brown|Hiss|Crackle"},
        {"tone", "Tone", "Hz", 100, 20000, 6000, false, true},
        {"follow", "Envelope follow", "%", 0, 1, 1},
        {"gate", "Gate", "dB", -80, 0, -60}},
       {{"offset", "History offset", "%", 0, 1, 0},
        {"speed", "Speed", "x", -4, 4, .5},
        {"mode", "Mode", "", 0, 6, 0, true, false,
         "Curve|Half time|Quarter time|Reverse|Scratch|Varispeed|Stretch"},
        {"capture", "Capture", "beats", .03125, 16, 4, false, true},
        {"smooth", "De-click", "ms", .1, 50, 3},
        {"history", "History", "bars", .25, 4, 2},
        {"captureUnit", "Capture unit", "", 0, 1, 0, true, false,
         "Beats|Bars"}},
       {{"division", "Repeat length", "beats", .015625, 4, .25, false, true},
        {"capture", "Capture", "beats", .03125, 16, 1, false, true},
        {"decay", "Decay", "%", 0, 1, .15},
        {"reverse", "Reverse", "", 0, 1, 0, true},
        {"shuffle", "Shuffle", "", 0, 1, 0, true},
        {"smooth", "De-click", "ms", .1, 50, 2}},
       {{"speed", "Speed", "x", 0, 2, 1},
        {"wow", "Wow", "%", 0, 1, .1},
        {"flutter", "Flutter", "%", 0, 1, .05},
        {"drive", "Drive", "dB", 0, 24, 3},
        {"tone", "Tone", "Hz", 200, 20000, 14000, false, true},
        {"capture", "Capture", "beats", .25, 16, 4, false, true}},
       {{"transpose", "Transpose", "st", -24, 24, 0},
        {"fine", "Fine tune", "ct", -100, 100, 0},
        {"formant", "Formant", "st", -12, 12, 0},
        {"preserve", "Preserve formants", "", 0, 1, 1, true},
        {"scale", "Scale", "", 0, 3, 0, true, false,
         "Chromatic|Major|Minor|Pentatonic"},
        {"root", "Root", "", 0, 11, 0, true}},
       {{"position", "Position", "%", 0, 1, .3},
        {"size", "Grain size", "ms", 5, 500, 80, false, true},
        {"density", "Density", "Hz", 1, 200, 20, false, true},
        {"pitch", "Pitch", "st", -24, 24, 0},
        {"scatter", "Scatter", "%", 0, 1, .2},
        {"reverse", "Reverse", "%", 0, 1, 0},
        {"spread", "Spread", "%", 0, 1, .6},
        {"freeze", "Freeze", "", 0, 1, 0, true},
        {"capture", "Capture", "beats", .25, 16, 4, false, true}},
       {{"freeze", "Freeze", "%", 0, 1, 0},
        {"blur", "Blur", "%", 0, 1, .2},
        {"shift", "Shift", "Hz", -4000, 4000, 0},
        {"low", "Low cutoff", "Hz", 20, 20000, 20, false, true},
        {"high", "High cutoff", "Hz", 20, 20000, 20000, false, true},
        {"sparse", "Prune", "%", 0, 1, 0},
        {"decay", "Decay", "s", .1, 60, 10, false, true}},
       {{"note", "Root note", "MIDI", 24, 96, 48},
        {"decay", "Decay", "s", .03, 8, .8, false, true},
        {"damping", "Damping", "%", 0, 1, .4},
        {"voices", "Voices", "", 1, 8, 4, true},
        {"scale", "Scale", "", 0, 3, 1, true, false,
         "Harmonics|Major|Minor|Pentatonic"},
        {"excite", "Excitation", "%", 0, 1, .6}},
       {{"frequency", "Frequency", "Hz", .1, 4000, 100, false, true},
        {"depth", "Depth", "%", 0, 1, .8},
        {"mode", "Mode", "", 0, 1, 0, true, false,
         "Ring modulation|Frequency shift"},
        {"shape", "Carrier", "", 0, 2, 0, true, false, "Sine|Triangle|Square"},
        {"stereo", "Stereo phase", "deg", 0, 180, 0},
        {"direction", "Shift direction", "", -1, 1, 1, true}},
       {{"rate", "Rate", "Hz", .01, 10, .4, false, true},
        {"depth", "Depth", "ms", 0, 15, 5},
        {"voices", "Voices", "", 1, 4, 3, true},
        {"detune", "Detune", "ct", 0, 30, 8},
        {"spread", "Spread", "%", 0, 1, .8},
        {"tone", "Tone", "Hz", 200, 20000, 12000, false, true},
        {"sync", "Sync", "", 0, 1, 0, true}},
       {{"rate", "Rate", "Hz", .01, 10, .15, false, true},
        {"depth", "Depth", "ms", 0, 10, 3},
        {"feedback", "Feedback", "%", -.95, .95, .5},
        {"delay", "Delay", "ms", .1, 12, 2},
        {"phase", "Stereo phase", "deg", 0, 180, 90},
        {"sync", "Sync", "", 0, 1, 0, true}},
       {{"rate", "Rate", "Hz", .01, 10, .2, false, true},
        {"depth", "Depth", "%", 0, 1, .8},
        {"feedback", "Feedback", "%", -.9, .9, .3},
        {"stages", "Stages", "", 0, 3, 2, true, false, "2|4|8|12"},
        {"center", "Center", "Hz", 50, 8000, 700, false, true},
        {"spread", "Spread", "deg", 0, 180, 90},
        {"sync", "Sync", "", 0, 1, 0, true}},
       {{"pan", "Pan", "", -1, 1, 0},
        {"width", "Width", "%", 0, 2, 1},
        {"mode", "Mode", "", 0, 2, 0, true, false, "Pan|Balance|Mid pan"},
        {"rate", "Auto-pan rate", "Hz", .01, 20, .5, false, true},
        {"depth", "Auto-pan depth", "%", 0, 1, 0},
        {"sync", "Sync", "", 0, 1, 1, true}},
       {{"time", "Time", "beats", .03125, 8, .75, false, true},
        {"feedback", "Feedback", "%", 0, .98, .35},
        {"low", "Low cut", "Hz", 20, 4000, 80, false, true},
        {"high", "High cut", "Hz", 100, 20000, 8000, false, true},
        {"mode", "Mode", "", 0, 2, 1, true, false, "Stereo|Ping-pong|Multitap"},
        {"diffusion", "Diffusion", "%", 0, 1, 0},
        {"duck", "Ducking", "%", 0, 1, 0},
        {"sync", "Sync", "", 0, 1, 1, true},
        {"milliseconds", "Free time", "ms", 1, 8000, 375, false, true}},
       {{"decay", "Decay", "s", .1, 30, 2, false, true},
        {"size", "Size", "%", .1, 2, 1},
        {"damping", "Damping", "%", 0, 1, .4},
        {"predelay", "Pre-delay", "ms", 0, 250, 12},
        {"mode", "Mode", "", 0, 4, 1, true, false,
         "Room|Hall|Plate|Gated|Shimmer"},
        {"duck", "Ducking", "%", 0, 1, 0},
        {"freeze", "Freeze", "", 0, 1, 0, true},
        {"shimmer", "Shimmer", "%", 0, 1, .35}},
       {{"note", "Carrier note", "MIDI", 24, 96, 48},
        {"formant", "Formant", "st", -12, 12, 0},
        {"bandwidth", "Bandwidth", "%", .3, 2, 1},
        {"attack", "Attack", "ms", .1, 100, 5, false, true},
        {"release", "Release", "ms", 5, 1000, 80, false, true},
        {"consonants", "Consonants", "%", 0, 1, .2},
        {"carrier", "Carrier", "", 0, 2, 0, true, false, "Saw|Pulse|Sidechain"},
        {"midi", "MIDI carrier", "", 0, 1, 1, true}}}};
  return d;
}
constexpr const char *names[]{
    "Empty",     "Volume",   "Dynamics",  "Filter",
    "Vowel",     "Drive",    "Crush",     "Noise",
    "Time Warp", "Repeater", "Tape",      "Pitch & Formant",
    "Granular",  "Spectral", "Resonator", "Ring & Frequency",
    "Chorus",    "Flanger",  "Phaser",    "Stereo",
    "Delay",     "Reverb",   "Vocoder"};
constexpr const char *ids[]{
    "empty",    "volume",   "dynamics",  "filter", "vowel",  "drive",
    "crush",    "noise",    "time",      "repeat", "tape",   "pitch",
    "granular", "spectral", "resonator", "ring",   "chorus", "flanger",
    "phaser",   "stereo",   "delay",     "reverb", "vocoder"};
bool finite(double x, double lo, double hi) {
  return std::isfinite(x) && x >= lo && x <= hi;
}
} // namespace
const char *kindName(Kind k) noexcept {
  return unsigned(k) < std::size(names) ? names[unsigned(k)] : "Empty";
}
const char *kindId(Kind k) noexcept {
  return unsigned(k) < std::size(ids) ? ids[unsigned(k)] : "empty";
}
std::span<const Field> fields(Kind k) {
  return definitions()[std::min(unsigned(k), unsigned(Kind::Count) - 1)];
}
double normalized(const Field &f, double v) noexcept {
  return f.logarithmic
             ? std::log(std::max(f.min, v) / f.min) / std::log(f.max / f.min)
             : (v - f.min) / (f.max - f.min);
}
double plain(const Field &f, double v) noexcept {
  v = std::clamp(v, 0., 1.);
  double p = f.logarithmic ? f.min * std::pow(f.max / f.min, v)
                           : std::lerp(f.min, f.max, v);
  return f.stepped ? std::round(p) : p;
}
std::uint32_t hash(std::uint32_t x) noexcept {
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  return x ^ (x >> 16);
}
double randomAt(std::uint32_t seed, std::int64_t event,
                unsigned salt) noexcept {
  const auto u = std::uint64_t(event);
  return double(hash(seed ^ std::uint32_t(u) ^
                     hash(std::uint32_t(u >> 32) + salt))) /
         4294967296.;
}
std::span<const ParameterInfo> parameterTable() {
  static const auto result = [] {
    std::vector<ParameterInfo> p(parameterCount);
    auto add = [&](unsigned i, std::string id, std::string name,
                   const char *unit, double lo, double hi, double def,
                   bool step = false) {
      p[i] = {i,    std::move(id), std::move(name), unit, lo, hi, def, true,
              step, false};
    };
    add(Input, "input", "Input", "dB", -36, 24, 0);
    add(Output, "output", "Output", "dB", -36, 12, 0);
    add(Mix, "mix", "Dry / Wet", "%", 0, 1, 1);
    add(Bypass, "bypass", "Bypass", "", 0, 1, 0, true);
    p[Bypass].isBypass = true;
    add(Protect, "protect", "Output protection", "", 0, 1, 1, true);
    for (unsigned i = 0; i < 4; ++i)
      add(Macro1 + i, "macro." + std::to_string(i + 1),
          "Macro " + std::to_string(i + 1), "%", 0, 1, .5);
    add(X, "motion.x", "Motion X", "%", 0, 1, .5);
    add(Y, "motion.y", "Motion Y", "%", 0, 1, .5);
    add(SceneSelect, "scene", "Scene", "", 0, 15, 0, true);
    add(Quantize, "quantize", "Scene quantize", "", 0, 2, 2, true);
    add(ManualTrigger, "trigger", "Trigger", "", 0, 1, 0, true);
    add(Panic, "panic", "Panic", "", 0, 1, 0, true);
    add(LowSplit, "crossover.low", "Low crossover", "Hz", 30, 4000, 160);
    add(HighSplit, "crossover.high", "High crossover", "Hz", 200, 16000, 3000);
    add(Balance, "balance", "A / B balance", "%", 0, 1, .5);
    for (unsigned b = 0; b < 3; ++b) {
      auto n = std::string(b == 0 ? "low" : b == 1 ? "mid" : "high");
      add(LowGain + b, n + ".gain", n + " gain", "dB", -24, 12, 0);
      add(LowMute + b, n + ".mute", n + " mute", "", 0, 1, 0, true);
      add(LowSolo + b, n + ".solo", n + " solo", "", 0, 1, 0, true);
    }
    for (unsigned s = 0; s < slotCount; ++s) {
      std::string prefix = "slot." + std::to_string(s + 1) + ".",
                  title = "Slot " + std::to_string(s + 1) + " ";
      add(commonParam(s, Enabled), prefix + "enabled", title + "enabled", "", 0,
          1, 1, true);
      add(commonParam(s, SlotMix), prefix + "mix", title + "mix", "%", 0, 1, 1);
      add(commonParam(s, SlotOutput), prefix + "output", title + "output", "dB",
          -36, 12, 0);
      add(commonParam(s, Solo), prefix + "solo", title + "solo", "", 0, 1, 0,
          true);
      for (unsigned c = 0; c < 2; ++c) {
        add(commonParam(s, DepthA + c),
            prefix + "motion." + std::to_string(c) + ".depth",
            title + "Motion depth", "%", -1, 1, 0);
        add(commonParam(s, RateA + c),
            prefix + "motion." + std::to_string(c) + ".rate",
            title + "Motion rate", "", .0078125, 40, 1);
      }
      for (unsigned k = 1; k < unsigned(Kind::Count); ++k) {
        auto f = fields(Kind(k));
        for (unsigned v = 0; v < fieldCount; ++v) {
          const unsigned i = effectParam(s, Kind(k), v);
          if (v < f.size())
            add(i, prefix + kindId(Kind(k)) + "." + f[v].id,
                title + kindName(Kind(k)) + " " + f[v].name, f[v].unit,
                f[v].min, f[v].max, f[v].initial, f[v].stepped);
          else {
            add(i, prefix + kindId(Kind(k)) + ".reserved." + std::to_string(v),
                title + "Reserved", "", 0, 1, 0);
            p[i].isAutomatable = false;
          }
        }
      }
    }
    return p;
  }();
  return result;
}
State initialState() {
  State s;
  auto p = parameterTable();
  for (unsigned i = 0; i < GlobalCount; ++i)
    s.globals[i] = p[i].defaultValue;
  return s;
}
Scene &ensureScene(State &s, unsigned n) {
  n = std::min(n, sceneCount - 1);
  while (s.scenes.size() <= n) {
    s.scenes.push_back(s.scenes.front());
    s.scenes.back().name = "Scene " + std::to_string(s.scenes.size());
  }
  return s.scenes[n];
}
void setKind(State &s, unsigned n, Kind kind) {
  if (n >= slotCount || unsigned(kind) >= unsigned(Kind::Count))
    return;
  s.modules[n].kind = kind;
  for (auto &scene : s.scenes) {
    scene.modules[n] = SlotState{};
    auto f = fields(kind);
    for (unsigned i = 0; i < f.size(); ++i)
      scene.modules[n].values[i] = f[i].initial;
  }
}
double evaluate(const Curve &c, double at) noexcept {
  if (c.points.empty())
    return 0;
  at = std::clamp(at, 0., 1.);
  auto next =
      std::upper_bound(c.points.begin(), c.points.end(), at,
                       [](double x, const Point &p) { return x < p.x; });
  if (next == c.points.begin())
    return next->y;
  if (next == c.points.end())
    return c.points.back().y;
  auto &a = *(next - 1);
  auto &b = *next;
  double t = (at - a.x) / std::max(1.e-9, double(b.x - a.x));
  switch (a.shape) {
  case Shape::Hold:
    t = 0;
    break;
  case Shape::Smooth:
    t = t * t * (3 - 2 * t);
    break;
  case Shape::Exponential:
    t = std::pow(t, 2 + 6 * std::abs(a.tension));
    break;
  case Shape::Logarithmic:
    t = 1 - std::pow(1 - t, 2 + 6 * std::abs(a.tension));
    break;
  default:
    break;
  }
  if (a.shape == Shape::Linear && a.tension != 0)
    t = a.tension > 0 ? std::pow(t, 1 + 4 * a.tension)
                      : 1 - std::pow(1 - t, 1 - 4 * a.tension);
  return std::lerp(double(a.y), double(b.y), t);
}
void makeShape(Curve &c, unsigned shape, unsigned seed) {
  c.points.clear();
  auto point = [&](float x, float y, Shape s = Shape::Smooth) {
    c.points.push_back({x, y, 0, s});
  };
  if (shape == 0) {
    point(0, 0);
    point(.08f, 0);
    point(.55f, .85f);
    point(1, 1);
  } else if (shape == 1) {
    point(0, 0, Shape::Linear);
    point(1, 1);
  } else if (shape == 2) {
    point(0, 0, Shape::Linear);
    point(.5f, 1, Shape::Linear);
    point(1, 0);
  } else if (shape == 3)
    for (unsigned i = 0; i <= 32; ++i)
      point(float(i) / 32,
            float(.5 - .5 * std::cos(2 * std::numbers::pi * i / 32)));
  else if (shape == 4) {
    point(0, 1, Shape::Hold);
    point(.5f, 0, Shape::Hold);
    point(1, 1);
  } else
    for (unsigned i = 0; i <= 16; ++i)
      point(float(i) / 16,
            shape == 5   ? float(i % 2 == 0)
            : shape == 6 ? float(i % 4) / 3
                         : float(randomAt(seed, i)),
            Shape::Hold);
}
void transformCurve(Curve &c, unsigned op) {
  if (op == 0)
    for (auto &p : c.points)
      p.y = 1 - p.y;
  else if (op == 1) {
    for (auto &p : c.points)
      p.x = 1 - p.x;
    std::reverse(c.points.begin(), c.points.end());
  } else if (op == 2)
    for (auto &p : c.points)
      p.shape = Shape::Smooth;
  else if (op == 3) {
    auto copy = c;
    for (unsigned i = 1; i + 1 < c.points.size(); ++i)
      c.points[i].y =
          (copy.points[i - 1].y + 2 * copy.points[i].y + copy.points[i + 1].y) /
          4;
  } else if (op == 4)
    c.phase = std::fmod(c.phase + .25, 1.);
}
void euclidean(Sequence &s, unsigned pulses, unsigned rotation) {
  pulses = std::min(pulses, s.length);
  for (unsigned i = 0; i < s.length; ++i)
    s.steps[i].enabled =
        (((i + rotation) % s.length) * pulses) % s.length < pulses;
  s.enabled = true;
}

bool validState(const State &s) noexcept {
  if (s.scenes.empty() || s.scenes.size() > sceneCount ||
      s.editScene >= s.scenes.size() || unsigned(s.routing) > 2 ||
      s.sceneMode > 2 || s.midiSceneChannel > 15 || !s.seed ||
      s.path.size() > maxPoints || s.midi.size() > 128 ||
      s.parameterLocks.size() > parameterCount)
    return false;
  for (auto id : s.parameterLocks)
    if (id >= parameterCount)
      return false;
  if (s.name.size() > 256 || s.category.size() > 256 ||
      s.description.size() > 2048 || s.tags.size() > 1024 ||
      s.presetId.size() > 256)
    return false;
  auto p = parameterTable();
  for (unsigned i = 0; i < GlobalCount; ++i)
    if (!finite(s.globals[i], p[i].minValue, p[i].maxValue) ||
        (p[i].isStepped && s.globals[i] != std::round(s.globals[i])))
      return false;
  if (s.globals[LowSplit] >= s.globals[HighSplit])
    return false;
  std::array<bool, slotCount> seen{};
  for (auto i : s.order) {
    if (i >= slotCount || seen[i])
      return false;
    seen[i] = true;
  }
  for (auto &slot : s.modules)
    if (unsigned(slot.kind) >= unsigned(Kind::Count) || unsigned(slot.lane) > 4)
      return false;
  for (auto &scene : s.scenes) {
    if (scene.name.size() > 128 || scene.routes.size() > maxRoutes)
      return false;
    for (auto x : scene.macros)
      if (!finite(x, 0, 1))
        return false;
    for (unsigned n = 0; n < slotCount; ++n) {
      auto &a = scene.modules[n];
      auto f = fields(s.modules[n].kind);
      for (unsigned i = 0; i < fieldCount; ++i)
        if (!std::isfinite(a.values[i]) ||
            (i < f.size() &&
             (!finite(a.values[i], f[i].min, f[i].max) ||
              (f[i].stepped && a.values[i] != std::round(a.values[i])))))
          return false;
      if (!finite(a.mix, 0, 1) || !finite(a.output, -36, 12) ||
          a.sequence.length < 1 || a.sequence.length > 64 ||
          unsigned(a.sequence.direction) > 3 ||
          !finite(a.sequence.division, .015625, 16) ||
          !finite(a.sequence.swing, 0, .75))
        return false;
      for (auto &step : a.sequence.steps)
        if (!finite(step.amount, 0, 1) || !finite(step.probability, 0, 1) ||
            step.ratchet < 1 || step.ratchet > 8)
          return false;
      for (auto &c : a.curves) {
        if (c.points.size() < 2 || c.points.size() > maxPoints ||
            c.target >= fieldCount || !finite(c.depth, -1, 1) ||
            !finite(c.bars, .0078125, 32) || !finite(c.hz, .01, 40) ||
            !finite(c.phase, 0, 1) || unsigned(c.play) > 2 ||
            unsigned(c.trigger) > 4)
          return false;
        if (c.points.front().x != 0 || c.points.back().x != 1)
          return false;
        float last = -1;
        for (auto &point : c.points) {
          if (!finite(point.x, 0, 1) || point.x <= last ||
              !finite(point.y, 0, 1) || !finite(point.tension, -1, 1) ||
              unsigned(point.shape) > 4)
            return false;
          last = point.x;
        }
      }
    }
    for (auto &r : scene.routes)
      if (r.source > 36 || !canModulate(r.target) || !finite(r.amount, -1, 1) ||
          !finite(r.low, 0, 1) || !finite(r.high, 0, 1) || r.low > r.high)
        return false;
    for (auto &m : scene.modulators) {
      if (m.type > 3 || m.shape > 3 || !finite(m.rate, .01, 40) ||
          !finite(m.phase, 0, 1) || !finite(m.attack, .0001, 2) ||
          !finite(m.release, .001, 5) || !finite(m.smooth, 0, 1))
        return false;
      for (auto v : m.steps)
        if (!finite(v, 0, 1))
          return false;
    }
  }
  const auto &d = s.detector;
  if (!finite(d.threshold, -80, 0) || !finite(d.sensitivity, .01, 10) ||
      !finite(d.attack, .0001, 1) || !finite(d.release, .001, 5) ||
      !finite(d.holdoff, .001, 4) || !finite(d.hysteresis, 0, 24) ||
      !finite(d.highpass, 20, 10000) || !finite(d.lowpass, 100, 20000) ||
      d.highpass >= d.lowpass)
    return false;
  if (!finite(s.pathBars, .125, 8) || !finite(s.pathSmooth, 0, 1))
    return false;
  float time = -1;
  for (auto &a : s.path) {
    if (!finite(a.time, 0, 1) || a.time <= time || !finite(a.x, 0, 1) ||
        !finite(a.y, 0, 1))
      return false;
    time = a.time;
  }
  for (auto &m : s.midi)
    if (m.controller > 127 || m.parameter >= parameterCount || m.channel > 16)
      return false;
  for (auto &name : s.macroNames)
    if (name.size() > 64)
      return false;
  return true;
}
using nlohmann::json;
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Point, x, y, tension, shape)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Curve, points, target, depth, bars, hz,
                                   phase, sync, bipolar, play, trigger)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Step, amount, probability, ratchet, enabled,
                                   retrigger)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Sequence, steps, length, division, swing,
                                   enabled, direction)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SlotState, values, enabled, solo, mix,
                                   output, curves, sequence)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Modulator, type, shape, rate, phase, attack,
                                   release, smooth, sync, sidechain, steps)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ModRoute, source, target, amount, low, high,
                                   bipolar, enabled)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Scene, name, modules, modulators, routes,
                                   macros)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Slot, kind, lane, locked)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PathPoint, time, x, y)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Detector, threshold, sensitivity, attack,
                                   release, holdoff, hysteresis, highpass,
                                   lowpass)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(MidiMapping, controller, parameter, channel)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(FluxState, modules, order, globals, scenes,
                                   routing, studio, seed, editScene, sceneMode,
                                   midiSceneChannel, midiScenes, detector,
                                   macroNames, path, pathBars, pathSmooth,
                                   pathEnabled, pathReverse, midi,
                                   parameterLocks, presetId, name, category,
                                   description, tags)
bool encodeState(const State &s, std::vector<std::uint8_t> &out) {
  if (!validState(s))
    return false;
  try {
    json j = {{"format", "daw.flux"}, {"version", 1}, {"state", s}};
    auto text = j.dump();
    out.assign(text.begin(), text.end());
    return true;
  } catch (...) {
    return false;
  }
}
std::optional<State> decodeState(std::span<const std::uint8_t> bytes) {
  if (bytes.empty() || bytes.size() > 16 * 1024 * 1024)
    return {};
  try {
    auto j = json::parse(bytes.begin(), bytes.end());
    if (j.at("format") != "daw.flux" || j.at("version") != 1)
      return {};
    auto &a = j.at("state");
    if (!a.contains("parameterLocks"))
      a["parameterLocks"] = json::array();
    if (a.at("scenes").empty() || a.at("scenes").size() > 16 ||
        a.at("path").size() > maxPoints || a.at("midi").size() > 128 ||
        a.at("parameterLocks").size() > parameterCount ||
        a.at("modules").size() != slotCount ||
        a.at("order").size() != slotCount ||
        a.at("globals").size() != GlobalCount || a.at("macroNames").size() != 4)
      return {};
    for (auto &sc : a.at("scenes")) {
      if (sc.at("modules").size() != 12 || sc.at("routes").size() > maxRoutes ||
          sc.at("macros").size() != 4 || sc.at("modulators").size() != 4)
        return {};
      for (auto &mod : sc.at("modulators"))
        if (mod.at("steps").size() != 16)
          return {};
      for (auto &sl : sc.at("modules")) {
        if (sl.at("values").size() != fieldCount ||
            sl.at("curves").size() != 2 ||
            sl.at("sequence").at("steps").size() != 64)
          return {};
        for (auto &c : sl.at("curves"))
          if (c.at("points").size() > maxPoints)
            return {};
      }
    }
    auto s = a.get<State>();
    if (validState(s))
      return s;
  } catch (...) {
  }
  return {};
}
} // namespace daw::plugins::flux
