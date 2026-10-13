#include "Internal/MorphProgram.hpp"
#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>
#include <numbers>

namespace daw::plugins::morph {
namespace {
constexpr double pi = std::numbers::pi_v<double>;
double clamp01(double x) { return std::clamp(x, 0., 1.); }
} // namespace
std::span<const ParameterInfo> parameterTable() {
  static const auto table = [] {
    std::array<ParameterInfo, Count> t;
    auto p = [&](unsigned i, const char *id, const char *name, const char *unit,
                 double lo, double hi, double value, bool step = false) {
      t[i] = {i, id, name, unit, lo, hi, value, true, step, false};
    };
    p(ChorusOn, "chorus.enabled", "Chorus enabled", "", 0, 1, 1, true);
    p(ChorusRate, "chorus.rate", "Chorus rate", "Hz", .01, 10, .3);
    p(ChorusDepth, "chorus.depth", "Chorus depth", "%", 0, 1, .45);
    p(ChorusVoices, "chorus.voices", "Chorus voices", "", 1, 4, 3, true);
    p(ChorusSpread, "chorus.spread", "Chorus spread", "%", 0, 1, .78);
    p(ChorusDetune, "chorus.detune", "Chorus detune", "ct", 0, 25, 8);
    p(ChorusTone, "chorus.tone", "Chorus tone", "%", 0, 1, .65);
    p(ChorusLink, "chorus.link", "Chorus link", "", 0, 1, 1, true);
    p(ChorusSync, "chorus.sync", "Chorus sync", "", 0, 8, 0, true);
    p(FlangerOn, "flanger.enabled", "Flanger enabled", "", 0, 1, 1, true);
    p(FlangerRate, "flanger.rate", "Flanger rate", "Hz", .01, 10, .08);
    p(FlangerDepth, "flanger.depth", "Flanger depth", "%", 0, 1, .4);
    p(FlangerFeedback, "flanger.feedback", "Flanger feedback", "%", -.85, .85,
      .25);
    p(FlangerDelay, "flanger.delay", "Flanger delay", "ms", .1, 12, 2.8);
    p(FlangerColor, "flanger.color", "Flanger color", "%", 0, 1, .5);
    p(FlangerStereo, "flanger.stereo", "Flanger stereo", "%", 0, 1, .65);
    p(FlangerLink, "flanger.link", "Flanger link", "", 0, 1, 1, true);
    p(FlangerSync, "flanger.sync", "Flanger sync", "", 0, 8, 0, true);
    p(PhaserOn, "phaser.enabled", "Phaser enabled", "", 0, 1, 1, true);
    p(PhaserRate, "phaser.rate", "Phaser rate", "Hz", .01, 10, .2);
    p(PhaserDepth, "phaser.depth", "Phaser depth", "%", 0, 1, .5);
    p(PhaserStages, "phaser.stages", "Phaser stages (pairs)", "", 1, 4, 4,
      true);
    p(PhaserResonance, "phaser.resonance", "Phaser resonance", "%", 0, .85,
      .35);
    p(PhaserSpread, "phaser.spread", "Phaser spread", "%", 0, 1, .7);
    p(PhaserMix, "phaser.mix", "Phaser mix", "%", 0, 1, 1);
    p(PhaserLink, "phaser.link", "Phaser link", "", 0, 1, 1, true);
    p(PhaserSync, "phaser.sync", "Phaser sync", "", 0, 8, 0, true);
    p(DoublerOn, "doubler.enabled", "Doubler enabled", "", 0, 1, 1, true);
    p(DoublerTime, "doubler.time", "Doubler time", "ms", 3, 50, 18);
    p(DoublerDetune, "doubler.detune", "Doubler detune", "ct", 0, 25, 9);
    p(DoublerVoices, "doubler.voices", "Doubler voices", "", 1, 4, 2, true);
    p(DoublerSpread, "doubler.spread", "Doubler spread", "%", 0, 1, .85);
    p(DoublerHumanize, "doubler.humanize", "Doubler humanize", "%", 0, 1, .37);
    p(DoublerLevel, "doubler.level", "Doubler level", "dB", -24, 6, 0);
    p(DoublerLink, "doubler.link", "Doubler link", "", 0, 1, 1, true);
    p(MorphX, "morph.x", "Morph X", "", -1, 1, 0);
    p(MorphY, "morph.y", "Morph Y", "", -1, 1, 0);
    p(Routing, "morph.routing", "Routing", "", 0, 1, 1, true);
    p(Interaction, "morph.interaction", "Elastic Weave", "%", 0, 1, .45);
    for (unsigned i = 0; i < 2; ++i) {
      const unsigned o = i ? Lfo2Rate : Lfo1Rate;
      const std::string prefix = i ? "lfo2." : "lfo1.";
      const char *suffix[]{"rate", "shape", "phase", "smooth", "sync"};
      const char *names[]{"Rate", "Shape", "Phase", "Smooth", "Sync"};
      const double lo[]{.01, 0, 0, 0, 0}, hi[]{20, 3, 360, 1, 8},
          def[]{i ? .17 : .5, 0, 0, .12, i ? 0. : 4.};
      for (unsigned j = 0; j < 5; ++j)
        t[o + j] = {o + j,
                    prefix + suffix[j],
                    std::string(i ? "LFO 2 " : "LFO 1 ") + names[j],
                    j == 0   ? "Hz"
                    : j == 2 ? "deg"
                    : j == 3 ? "%"
                             : "",
                    lo[j],
                    hi[j],
                    def[j],
                    true,
                    j == 1 || j == 4,
                    false};
    }
    p(EnvSensitivity, "env.sensitivity", "Envelope sensitivity", "%", 0, 1,
      .42);
    p(EnvAttack, "env.attack", "Envelope attack", "ms", .1, 200, 10);
    p(EnvRelease, "env.release", "Envelope release", "ms", 10, 2000, 320);
    p(EnvSource, "env.source", "Envelope source", "", 0, 2, 0, true);
    p(Breathing, "env.breathing", "Breathing Bloom", "%", 0, 1, .35);
    p(Movement, "macro.movement", "Movement", "%", 0, 1, .5);
    p(Texture, "macro.texture", "Texture", "%", 0, 1, .5);
    p(Character, "macro.character", "Character", "%", 0, 1, .5);
    p(Space, "macro.space", "Space", "%", 0, 1, .5);
    p(HighPass, "output.hpf", "High pass", "Hz", 20, 2000, 20);
    p(LowPass, "output.lpf", "Low pass", "Hz", 500, 20000, 16000);
    p(Drive, "drive.amount", "Drive", "%", 0, 1, 0);
    p(DriveMode, "drive.mode", "Drive mode", "", 0, 2, 0, true);
    p(DrivePosition, "drive.position", "Drive position", "", 0, 1, 0, true);
    p(EchoLevel, "echo.level", "Echo level", "%", 0, 1, 0);
    p(EchoTime, "echo.time", "Echo time", "ms", 5, 2000, 250);
    p(EchoFeedback, "echo.feedback", "Echo feedback", "%", 0, .88, .3);
    p(EchoDamping, "echo.damping", "Echo damping", "%", 0, 1, .5);
    p(EchoPingPong, "echo.pingpong", "Echo ping-pong", "%", 0, 1, .5);
    p(EchoSync, "echo.sync", "Echo sync", "", 0, 8, 0, true);
    p(BloomLevel, "bloom.level", "Bloom level", "%", 0, 1, 0);
    p(BloomDecay, "bloom.decay", "Bloom decay", "s", .15, 6, 2);
    p(BloomDensity, "bloom.density", "Bloom density", "%", 0, 1, .65);
    p(BloomTone, "bloom.tone", "Bloom tone", "%", 0, 1, .4);
    p(Width, "output.width", "Width", "%", 0, 2, 1);
    p(Mix, "output.mix", "Dry / wet", "%", 0, 1, 1);
    p(Output, "output.gain", "Output", "dB", -24, 12, 0);
    p(AutoGain, "output.autogain", "Auto gain", "", 0, 1, 1, true);
    p(SoftClip, "output.softclip", "Soft clip", "", 0, 1, 0, true);
    p(MotionOn, "motion.enabled", "Motion playback", "", 0, 1, 0, true);
    p(MotionBars, "motion.bars", "Motion length (powers of two)", "", 0, 3, 1,
      true);
    p(MotionDepth, "motion.depth", "Motion depth", "%", 0, 1, 1);
    p(MotionReverse, "motion.reverse", "Reverse motion", "", 0, 1, 0, true);
    p(MotionSmooth, "motion.smooth", "Motion smooth", "%", 0, 1, .2);
    p(CanvasOn, "canvas.enabled", "Canvas", "", 0, 1, 0, true);
    p(CanvasInfluence, "canvas.influence", "Canvas influence", "%", 0, 1, .5);
    p(CanvasLocks, "canvas.locks", "Generation locks", "", 0, 127, 16, true);
    for (unsigned s = 0; s < 16; ++s) {
      const auto prefix = "mod." + std::to_string(s + 1) + ".";
      const unsigned i = matrixSource(s);
      t[i] = {i,
              prefix + "source",
              "Modulation source",
              "",
              0,
              15,
              0,
              true,
              true,
              false};
      t[i + 1] = {i + 1,
                  prefix + "target",
                  "Modulation destination",
                  "",
                  0,
                  MatrixStart - 1,
                  ChorusDepth,
                  true,
                  true,
                  false};
      t[i + 2] = {i + 2,
                  prefix + "amount",
                  "Modulation amount",
                  "%",
                  -1,
                  1,
                  0,
                  true,
                  false,
                  false};
    }
    return t;
  }();
  return table;
}
Values defaults() {
  Values v{};
  for (const auto &p : parameterTable())
    v[p.index] = p.defaultValue;
  return v;
}
bool modulationTarget(unsigned i) noexcept {
  return i < MatrixStart && !parameterTable()[i].isStepped &&
         i != CanvasInfluence && i != Lfo1Rate && i != Lfo2Rate &&
         !(i >= Lfo1Shape && i <= Lfo2Sync) && !(i >= Movement && i <= Space) &&
         i != MotionDepth && i != MotionSmooth;
}
bool validProgram(const MorphCanvasProgram &p) noexcept {
  if (p.pointCount > p.maxPoints || p.previewWidth > p.previewSide ||
      p.previewHeight > p.previewSide ||
      ((p.previewWidth == 0) != (p.previewHeight == 0)))
    return false;
  float last = -1;
  for (unsigned i = 0; i < p.pointCount; ++i) {
    const auto &a = p.path[i];
    if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(a.time) ||
        std::abs(a.x) > 1 || std::abs(a.y) > 1 || a.time < 0 || a.time > 1 ||
        a.time < last)
      return false;
    last = a.time;
  }
  for (const auto &c : p.cells) {
    float sum = 0;
    for (float w : c.weights) {
      if (!std::isfinite(w) || w < 0 || w > 1)
        return false;
      sum += w;
    }
    if (std::abs(sum - 1) > 1.e-3f)
      return false;
    for (float f : {c.light, c.saturation, c.edge})
      if (!std::isfinite(f) || f < 0 || f > 1)
        return false;
  }
  return true;
}
CanvasCell sampleCanvas(const MorphCanvasProgram &p, double x,
                        double y) noexcept {
  x = clamp01((x + 1) * .5) * (p.side - 1);
  y = clamp01((y + 1) * .5) * (p.side - 1);
  const auto ix = unsigned(x), iy = unsigned(y),
             jx = std::min(ix + 1, p.side - 1),
             jy = std::min(iy + 1, p.side - 1);
  CanvasCell out;
  out.weights.fill(0);
  out.light = out.saturation = out.edge = 0;
  const double fx = x - ix, fy = y - iy;
  const std::array<unsigned, 4> ids{iy * p.side + ix, iy * p.side + jx,
                                    jy * p.side + ix, jy * p.side + jx};
  const std::array<double, 4> weights{(1 - fx) * (1 - fy), fx * (1 - fy),
                                      (1 - fx) * fy, fx * fy};
  for (unsigned k = 0; k < 4; ++k) {
    const auto &c = p.cells[ids[k]];
    const auto w = float(weights[k]);
    for (unsigned j = 0; j < 4; ++j)
      out.weights[j] += w * c.weights[j];
    out.light += w * c.light;
    out.saturation += w * c.saturation;
    out.edge += w * c.edge;
  }
  return out;
}
void analyzeCanvas(MorphCanvasProgram &p) noexcept {
  if (!p.previewWidth || !p.previewHeight || p.previewWidth > p.previewSide ||
      p.previewHeight > p.previewSide) {
    p.cells.fill(CanvasCell{});
    p.painted = false;
    return;
  }
  constexpr std::array<double, 4> hues{.54, .77, .075, .15};
  bool painted = false;
  for (unsigned y = 0; y < p.side; ++y)
    for (unsigned x = 0; x < p.side; ++x) {
      auto &c = p.cells[y * p.side + x];
      const unsigned px = std::min(p.previewWidth - 1,
                                   x * p.previewWidth / p.side),
                     py = std::min(p.previewHeight - 1,
                                   y * p.previewHeight / p.side);
      const unsigned endX = std::max(px + 1, (x + 1) * p.previewWidth / p.side),
                     endY =
                         std::max(py + 1, (y + 1) * p.previewHeight / p.side);
      double r = 0, g = 0, b = 0, alpha = 0;
      // Area averages retain small details instead of sampling one pixel per
      // cell. All decoding/analysis remains on the control side of the engine.
      for (unsigned row = py; row < endY; ++row)
        for (unsigned col = px; col < endX; ++col) {
          const auto pixel = p.rgba[row * p.previewSide + col];
          const double a = (pixel >> 24) / 255.;
          r += ((pixel >> 16) & 255) / 255. * a;
          g += ((pixel >> 8) & 255) / 255. * a;
          b += (pixel & 255) / 255. * a;
          alpha += a;
        }
      if (alpha == 0) {
        c = CanvasCell{};
        continue;
      }
      painted = true;
      r /= alpha;
      g /= alpha;
      b /= alpha;
      alpha /= (endX - px) * (endY - py);
      const auto hi = std::max({r, g, b}), lo = std::min({r, g, b}),
                 d = hi - lo;
      double h = 0;
      if (d > 1.e-6) {
        h = hi == r ? (g - b) / d : hi == g ? 2 + (b - r) / d : 4 + (r - g) / d;
        h /= 6;
        if (h < 0)
          h += 1;
      }
      c.light = float(.5 + alpha * (.2126 * r + .7152 * g + .0722 * b - .5));
      c.saturation = float(hi > 1.e-6 ? alpha * d / hi : 0);
      c.edge = 0;
      double sum = 0;
      for (unsigned i = 0; i < 4; ++i) {
        double dist = std::abs(h - hues[i]);
        dist = std::min(dist, 1 - dist);
        c.weights[i] = float(std::exp(-dist * dist / 0.015));
        sum += c.weights[i];
      }
      for (auto &w : c.weights)
        w = float((1 - c.saturation) * .25 + c.saturation * w / sum);
    }
  for (unsigned y = 0; y < p.side; ++y)
    for (unsigned x = 0; x < p.side; ++x) {
      const float dx = p.cells[y * p.side + std::min(x + 1, p.side - 1)].light -
                       p.cells[y * p.side + (x ? x - 1 : 0)].light;
      const float dy = p.cells[std::min(y + 1, p.side - 1) * p.side + x].light -
                       p.cells[(y ? y - 1 : 0) * p.side + x].light;
      p.cells[y * p.side + x].edge =
          std::clamp(2 * std::hypot(dx, dy), 0.f, 1.f);
    }
  p.painted = painted;
}
std::uint32_t canvasFingerprint(const MorphCanvasProgram &p) noexcept {
  // Hash decoded pixels, not file paths, timestamps, EXIF or compression bytes.
  // Padding and invisible RGB do not affect the result. This is a variation
  // seed, not a cryptographic identifier.
  std::uint32_t hash = 2166136261u;
  auto add = [&](std::uint32_t word) {
    for (unsigned byte = 0; byte < 4; ++byte) {
      hash = (hash ^ (word & 255u)) * 16777619u;
      word >>= 8;
    }
  };
  add(p.previewWidth);
  add(p.previewHeight);
  for (unsigned y = 0; y < std::min(p.previewHeight, p.previewSide); ++y)
    for (unsigned x = 0; x < std::min(p.previewWidth, p.previewSide); ++x) {
      const auto pixel = p.rgba[y * p.previewSide + x];
      add(pixel >> 24 ? pixel : 0);
    }
  return hash ? hash : 1;
}
void makeOrbit(MorphCanvasProgram &p, unsigned variant) noexcept {
  p.pointCount = 128;
  p.recorded = false;
  for (unsigned i = 0; i < p.pointCount; ++i) {
    const double t = double(i) / (p.pointCount - 1);
    const bool spiral = variant % 3 == 2;
    const double a = (spiral ? 6 : 2) * pi * t;
    const double radius = spiral             ? .12 + .7 * t
                          : variant % 3 == 1 ? .3 + .5 * std::sin(pi * t)
                                             : .76;
    p.path[i] = {float(radius * std::sin(a)), float(radius * std::cos(a)),
                 float(t)};
  }
}
void smoothPath(MorphCanvasProgram &p) noexcept {
  if (p.pointCount < 3)
    return;
  auto old = p.path;
  for (unsigned i = 1; i + 1 < p.pointCount; ++i) {
    p.path[i].x = (old[i - 1].x + 2 * old[i].x + old[i + 1].x) * .25f;
    p.path[i].y = (old[i - 1].y + 2 * old[i].y + old[i + 1].y) * .25f;
  }
}
Values interpretCanvas(const MorphCanvasProgram &p, Values v) {
  if (!p.painted)
    return v;
  double light = 0, lightSquared = 0, sat = 0, edge = 0;
  std::array<double, 4> w{};
  for (const auto &c : p.cells) {
    light += c.light;
    lightSquared += c.light * c.light;
    sat += c.saturation;
    edge += c.edge;
    for (unsigned i = 0; i < 4; ++i)
      w[i] += c.weights[i];
  }
  light /= p.cells.size();
  sat /= p.cells.size();
  edge /= p.cells.size();
  for (auto &a : w)
    a /= p.cells.size();
  const double contrast = clamp01(
      2.5 *
      std::sqrt(std::max(0., lightSquared / p.cells.size() - light * light)));
  const double detail = clamp01(edge * 1.5 + contrast * .35);
  const double landscape =
      clamp01(double(p.previewWidth) / std::max(1u, p.previewHeight) * .5);

  // Start from the same complete, bounded generator as the Random button.
  // The image seed varies timings, voices, stages, routing and assignments;
  // its measured visual features then give those choices a sonic direction.
  // Using defaults avoids accumulating changes on every repeated import.
  State base;
  base.values = defaults();
  auto generated = randomState(base, p.seed).values;
  auto &g = generated;
  for (auto id : moduleOffsets)
    g[id] = 1; // An earlier bypass must not make a new photo inaudible.
  g[ChorusDepth] = .23 + .61 * sat;
  g[ChorusDetune] = 3 + 18 * sat;
  g[ChorusTone] = .12 + .84 * light;
  g[ChorusRate] *= .6 + detail;
  g[FlangerDepth] = .22 + .6 * sat;
  g[FlangerFeedback] = (g[FlangerFeedback] < 0 ? -1 : 1) * (.12 + .52 * detail);
  g[FlangerColor] = .12 + .8 * light;
  g[PhaserDepth] = .3 + .55 * sat;
  g[PhaserResonance] = .1 + .58 * detail;
  g[DoublerDetune] = 3 + 18 * sat;
  for (auto id : {ChorusSync, FlangerSync, PhaserSync})
    g[id] = 0;
  g[Interaction] = .2 + .65 * sat;
  g[MorphX] = w[2] - w[1];
  g[MorphY] = w[3] - w[0];
  g[Texture] = .35 + .45 * sat;
  g[Character] = .35 + .3 * contrast;
  g[Space] = .4 + .35 * landscape;
  g[Drive] = .06 + .32 * contrast + .2 * detail;
  g[EchoLevel] = .14 + .22 * (1 - light);
  g[EchoTime] *= .85 + .3 * landscape;
  g[EchoFeedback] = .18 + .42 * contrast;
  g[EchoDamping] = .1 + .75 * (1 - light);
  g[EchoSync] = 0;
  g[BloomLevel] = .14 + .26 * (1 - detail);
  g[BloomDecay] = .65 + g[BloomDecay] * (1 - .65 * detail);
  g[BloomDensity] = .4 + .6 * detail;
  g[BloomTone] = .15 + .7 * light;
  g[Breathing] = .25 + .4 * detail;
  g[HighPass] = 20;
  g[LowPass] = 2000 + 18000 * light;
  g[Width] = .85 + .65 * landscape;
  g[MotionOn] = 1;
  g[MotionBars] = 1 + p.seed % 3;
  g[MotionDepth] = .3 + .55 * sat;
  g[MotionSmooth] = .2 + .6 * (1 - detail);
  g[MotionReverse] = (p.seed >> 8) & 1;

  // The existing seven generation locks also protect associated macros and
  // routes. Output gain/compensation stay user-controlled in every case.
  const auto group = [](unsigned id) -> unsigned {
    if (id <= DoublerLink || (id >= Lfo1Rate && id <= EnvSource) ||
        id == Movement || id == Texture)
      return 1;
    if (id == Character || (id >= Drive && id <= DrivePosition))
      return 2;
    if (id >= EchoLevel && id <= EchoSync)
      return 4;
    if (id == Space || id == Breathing || (id >= BloomLevel && id <= BloomTone))
      return 8;
    if (id == HighPass || id == LowPass || (id >= Width && id <= SoftClip))
      return 16;
    if (id >= MorphX && id <= Interaction)
      return 32;
    if (id >= MotionOn && id <= MotionSmooth)
      return 64;
    return 0;
  };
  const unsigned locks = unsigned(v[CanvasLocks]);
  for (unsigned id = 0; id < MatrixStart; ++id) {
    if (!group(id) || (locks & group(id)) || id == Output || id == AutoGain ||
        id == SoftClip)
      continue;
    const auto &info = parameterTable()[id];
    v[id] = std::clamp(g[id], info.minValue, info.maxValue);
  }
  for (unsigned slot = 0; slot < 16; ++slot) {
    const auto oldTarget = unsigned(v[matrixTarget(slot)]);
    if (oldTarget == Mix || oldTarget == Output) {
      v[matrixSource(slot)] = v[matrixAmount(slot)] = 0;
      continue;
    }
    if (locks & group(oldTarget))
      continue;
    const auto target = unsigned(g[matrixTarget(slot)]);
    v[matrixTarget(slot)] = target;
    const bool enabled = !(locks & group(target));
    v[matrixSource(slot)] = enabled ? g[matrixSource(slot)] : 0;
    v[matrixAmount(slot)] = enabled ? g[matrixAmount(slot)] : 0;
  }
  v[Mix] = 1;
  v[CanvasOn] = 1;
  return v;
}

std::span<const FactoryPreset> factoryPresets() noexcept {
  static constexpr FactoryPreset p[]{{"Velvet Portrait", "Vocals"},
                                     {"Silk Contour", "Vocals"},
                                     {"Breathing Choir", "Vocals"},
                                     {"Neon Whisper", "Vocals"},
                                     {"Vocal Ink", "Vocals"},
                                     {"Liquid Strings", "Guitars"},
                                     {"Copper Motion", "Guitars"},
                                     {"Soft Jet", "Guitars"},
                                     {"Prism Arpeggio", "Guitars"},
                                     {"Bent Velvet", "Guitars"},
                                     {"Felt Halo", "Piano / Keys"},
                                     {"Glass Ribbon", "Piano / Keys"},
                                     {"Dusty Keys", "Piano / Keys"},
                                     {"Water Sketch", "Piano / Keys"},
                                     {"Orbit Nocturne", "Piano / Keys"},
                                     {"Pocket Weave", "Drums"},
                                     {"Ghost Snare", "Drums"},
                                     {"Chrome Hats", "Drums"},
                                     {"Ink Break", "Drums"},
                                     {"Elastic Room", "Drums"},
                                     {"Mono Velvet", "Bass"},
                                     {"Rubber Current", "Bass"},
                                     {"Carbon Double", "Bass"},
                                     {"Subtle Orbit", "Bass"},
                                     {"Molten Contour", "Bass"},
                                     {"Evolving Dream", "Synth / FX"},
                                     {"Aurora Canvas", "Synth / FX"},
                                     {"Ink Spiral", "Synth / FX"},
                                     {"Photo Drift", "Synth / FX"},
                                     {"Ember Bloom", "Synth / FX"}};
  return p;
}
State factoryState(unsigned index) {
  State s;
  s.values = defaults();
  makeOrbit(s.canvas);
  if (index >= factoryPresets().size())
    return s;
  const auto p = factoryPresets()[index];
  s.presetName = p.name;
  s.category = p.category;
  // Individual voicings: chorus depth, flanger depth, phaser depth, detune,
  // drive, echo, bloom. Factory sound is data, independent of UI state.
  static constexpr double voicing[30][7]{{.32, .10, .15, 7, .06, 0, .08},
                                         {.15, .06, .48, 4, .03, .06, .22},
                                         {.55, .12, .25, 11, .08, .04, .28},
                                         {.25, .62, .18, 6, .10, .24, .12},
                                         {.42, .35, .38, 10, .12, .15, .22},
                                         {.48, .12, .22, 5, .12, .17, .09},
                                         {.15, .16, .72, 3, .28, .12, .08},
                                         {.12, .80, .22, 4, .09, .06, .04},
                                         {.34, .20, .32, 9, .10, .38, .18},
                                         {.65, .60, .68, 13, .42, .28, .16},
                                         {.25, .05, .16, 4, .04, .04, .30},
                                         {.50, .12, .25, 12, .03, .18, .15},
                                         {.43, .08, .25, 5, .24, .09, .12},
                                         {.45, .30, .42, 9, .08, .22, .30},
                                         {.30, .14, .42, 8, .06, .25, .48},
                                         {.18, .12, .24, 3, .14, 0, .08},
                                         {.10, .10, .15, 2, .08, .06, .35},
                                         {.08, .55, .14, 3, .12, .04, .02},
                                         {.30, .48, .50, 8, .25, .25, .12},
                                         {.12, .16, .22, 4, .10, .08, .42},
                                         {0, 0, 0, 0, .32, 0, 0},
                                         {.08, .25, .10, 1, .18, 0, 0},
                                         {.12, .05, .12, 4, .22, .04, .02},
                                         {.10, .08, .20, 2, .10, 0, .02},
                                         {.10, .22, .50, 3, .48, .08, .04},
                                         {.62, .38, .58, 12, .12, .28, .40},
                                         {.58, .42, .52, 15, .14, .30, .38},
                                         {.55, .65, .70, 18, .30, .32, .28},
                                         {.48, .35, .62, 14, .20, .34, .42},
                                         {.40, .45, .62, 10, .58, .45, .50}};
  auto &v = s.values;
  const auto &a = voicing[index];
  for (auto [i, x] : {std::pair{ChorusDepth, a[0]},
                      {FlangerDepth, a[1]},
                      {PhaserDepth, a[2]},
                      {DoublerDetune, a[3]},
                      {Drive, a[4]},
                      {EchoLevel, a[5]},
                      {BloomLevel, a[6]}})
    v[i] = x;
  const unsigned category = index / 5, variant = index % 5;
  v[Movement] = .32 + .07 * variant;
  v[Texture] = .38 + .05 * variant;
  v[Character] = .45 + a[4] * .28;
  v[Space] = category == 4 ? .25 : .38 + a[6] * .5;
  v[ChorusRate] = .12 + .063 * variant;
  v[FlangerRate] = .035 + .021 * variant;
  v[PhaserRate] = .09 + .035 * variant;
  v[Interaction] = .18 + .16 * variant;
  v[Routing] = variant % 2;
  v[EchoSync] = 4 + (variant % 3);
  v[EchoFeedback] = .22 + .065 * variant;
  v[BloomDecay] = category == 3 ? .35 + .2 * variant : 1.1 + .7 * variant;
  v[Breathing] = category == 0 ? .65 : category == 3 ? .85 : .25;
  v[MorphX] = (variant == 2 ? -.55 : variant == 1 ? .4 : 0);
  v[MorphY] = (variant == 0 ? -.3 : variant == 3 ? .25 : 0);
  v[LowPass] = index == 12 ? 5200 : category == 4 ? 8000 : 15500;
  if (category == 4) {
    v[Width] = index == 20 ? 0 : .65;
    v[HighPass] = index == 22 ? 140 : 20;
  }
  if (index == 20) {
    v[ChorusOn] = v[FlangerOn] = v[PhaserOn] = v[DoublerOn] = 0;
  }
  if (index == 7) {
    v[FlangerFeedback] = .58;
    v[FlangerColor] = .35;
  }
  if (index == 17) {
    v[HighPass] = 1800;
    v[FlangerFeedback] = .48;
  }
  if (index == 9 || index == 29) {
    v[DrivePosition] = 1;
    v[DriveMode] = 1;
    v[EchoFeedback] = .64;
  }
  if (index == 24 || index == 27)
    v[DriveMode] = 2;
  v[matrixSource(0)] = 1;
  v[matrixTarget(0)] = ChorusDepth;
  v[matrixAmount(0)] = .05 + .02 * variant;
  v[matrixSource(1)] = 3;
  v[matrixTarget(1)] = BloomLevel;
  v[matrixAmount(1)] = -.08;
  if (index == 4 || index == 8 || index == 13 || index == 18 || index >= 25) {
    s.canvas.seed = 0x4d4f5250u + index * 7919;
    s.canvas.previewWidth = s.canvas.previewHeight = s.canvas.previewSide;
    for (unsigned y = 0; y < s.canvas.previewSide; ++y)
      for (unsigned x = 0; x < s.canvas.previewSide; ++x) {
        const double u = double(x) / s.canvas.previewSide,
                     w = double(y) / s.canvas.previewSide;
        auto c = [&](double phase) {
          return unsigned(
              255 *
              clamp01(.48 +
                      .43 * std::sin(2 * pi * (u * (1 + variant) + w * .7) +
                                     phase)));
        };
        s.canvas.rgba[y * s.canvas.previewSide + x] =
            0xff000000u | (c(index * .4) << 16) | (c(2.1 + index * .2) << 8) |
            c(4.2);
      }
    analyzeCanvas(s.canvas);
    makeOrbit(s.canvas, variant);
    v[CanvasOn] = v[MotionOn] = 1;
    v[CanvasInfluence] = .25 + .08 * variant;
    v[MotionBars] = category == 5 ? 3 : 1;
  }
  return s;
}

State randomState(const State &base, std::uint32_t seed) {
  State s = base;
  s.presetKind = "custom";
  s.presetName = "Randomized";
  s.category.clear();
  // Local, deterministic generator: no shared RNG or audio-thread work.
  auto next = [&]() {
    seed += 0x9e3779b9u;
    auto x = seed;
    x = (x ^ (x >> 16)) * 0x21f0aaadu;
    x = (x ^ (x >> 15)) * 0x735a2d97u;
    return double(x ^ (x >> 15)) / 4294967296.;
  };
  auto &v = s.values;
  auto range = [&](unsigned p, double low, double high) {
    v[p] = low + (high - low) * next();
  };
  auto rate = [&](unsigned p, double low, double high) {
    v[p] = low * std::pow(high / low, next());
  };
  auto choice = [&](unsigned p, unsigned count, unsigned first = 0) {
    v[p] = first + unsigned(next() * count);
  };
  for (unsigned m = 0; m < moduleOffsets.size(); ++m) {
    v[moduleOffsets[m]] = next() < .8;
    v[linkParameters[m]] = next() < .7;
  }
  // Every result has an audible modulation section.
  v[moduleOffsets[unsigned(next() * moduleOffsets.size())]] = 1;
  rate(ChorusRate, .04, 2);
  range(ChorusDepth, .15, .9);
  choice(ChorusVoices, 3, 2);
  range(ChorusSpread, .3, 1);
  range(ChorusDetune, 2, 22);
  range(ChorusTone, .2, .9);
  rate(FlangerRate, .015, 1);
  range(FlangerDepth, .08, .8);
  range(FlangerFeedback, -.55, .65);
  range(FlangerDelay, 1, 7);
  range(FlangerColor, .15, .95);
  range(FlangerStereo, .3, 1);
  rate(PhaserRate, .03, 1.8);
  range(PhaserDepth, .15, .9);
  choice(PhaserStages, 4, 1);
  range(PhaserResonance, .05, .7);
  range(PhaserSpread, .2, 1);
  range(DoublerTime, 8, 35);
  range(DoublerDetune, 3, 22);
  choice(DoublerVoices, 4, 1);
  range(DoublerSpread, .35, 1);
  range(DoublerHumanize, .1, .85);
  range(DoublerLevel, -4, 0);
  for (auto p : {ChorusSync, FlangerSync, PhaserSync, EchoSync})
    v[p] = next() < .7 ? 0 : 2 + unsigned(next() * 6);
  range(MorphX, -1, 1);
  range(MorphY, -1, 1);
  const auto distance = std::max(1., std::abs(v[MorphX]) + std::abs(v[MorphY]));
  v[MorphX] /= distance;
  v[MorphY] /= distance;
  choice(Routing, 2);
  range(Interaction, .15, 1);
  for (auto p : {Movement, Texture, Character, Space})
    range(p, .2, .8);
  for (auto offset : {Lfo1Rate, Lfo2Rate}) {
    rate(offset, .03, 2);
    choice(offset + 1, 3);
    range(offset + 2, 0, 360);
    range(offset + 3, .15, .7);
    v[offset + 4] = next() < .6 ? 0 : 2 + unsigned(next() * 6);
  }
  range(EnvSensitivity, .25, .75);
  range(EnvAttack, 3, 25);
  range(EnvRelease, 80, 800);
  v[EnvSource] = 0;
  range(Breathing, .15, .8);
  range(Drive, 0, .65);
  choice(DriveMode, 3);
  choice(DrivePosition, 2);
  range(EchoLevel, 0, .4);
  rate(EchoTime, 80, 650);
  range(EchoFeedback, .1, .65);
  range(EchoDamping, .15, .85);
  range(EchoPingPong, 0, 1);
  range(BloomLevel, .08, .5);
  range(BloomDecay, .6, 4.8);
  range(BloomDensity, .3, 1);
  range(BloomTone, .2, .85);
  v[HighPass] = next() < .7 ? 20 : 20 + 330 * next();
  rate(LowPass, 6000, 20000);
  range(Width, .8, 1.6);
  // Replace old assignments too: a former Mix route must not dilute 100% wet.
  constexpr unsigned targets[]{ChorusDepth,   FlangerFeedback, PhaserResonance,
                               DoublerDetune, ChorusTone,      BloomLevel,
                               Drive,         EchoDamping};
  for (unsigned slot = 0; slot < 16; ++slot) {
    v[matrixSource(slot)] = slot < 4 ? 1 + unsigned(next() * 4) : 0;
    v[matrixTarget(slot)] = targets[unsigned(next() * std::size(targets))];
    v[matrixAmount(slot)] = slot < 4 ? -.15 + .3 * next() : 0;
  }
  v[Mix] = v[PhaserMix] = 1;
  // The current picture, trajectory and output level remain user-controlled.
  return s;
}

bool encodeState(const State &s, std::vector<std::uint8_t> &out) {
  if (!validProgram(s.canvas))
    return false;
  nlohmann::json j{{"version", 1},
                   {"uid", "daw.morph"},
                   {"presetKind", s.presetKind},
                   {"presetName", s.presetName},
                   {"category", s.category}};
  auto &params = j["params"];
  for (const auto &p : parameterTable())
    params[p.id] = s.values[p.index];
  const auto &p = s.canvas;
  auto &c = j["canvas"];
  c = {{"seed", p.seed},
       {"painted", p.painted},
       {"recorded", p.recorded},
       {"width", p.previewWidth},
       {"height", p.previewHeight}};
  c["cells"] = nlohmann::json::array();
  for (const auto &a : p.cells)
    c["cells"].push_back({a.weights[0], a.weights[1], a.weights[2],
                          a.weights[3], a.light, a.saturation, a.edge});
  c["path"] = nlohmann::json::array();
  for (unsigned i = 0; i < p.pointCount; ++i)
    c["path"].push_back({p.path[i].x, p.path[i].y, p.path[i].time});
  c["pixels"] = nlohmann::json::array();
  for (unsigned y = 0; y < p.previewHeight; ++y)
    for (unsigned x = 0; x < p.previewWidth; ++x)
      c["pixels"].push_back(p.rgba[y * p.previewSide + x]);
  const auto text = j.dump();
  out.assign(text.begin(), text.end());
  return true;
}
std::optional<State> decodeState(std::span<const std::uint8_t> bytes) {
  if (bytes.empty() || bytes.size() > 2 * 1024 * 1024)
    return {};
  try {
    auto j = nlohmann::json::parse(bytes.begin(), bytes.end());
    if (j.at("uid") != "daw.morph" || j.at("version") != 1 ||
        !j.at("params").is_object())
      return {};
    State s;
    s.values = defaults();
    s.presetKind = j.value("presetKind", "user");
    s.presetName = j.value("presetName", "Custom");
    s.category = j.value("category", "");
    if (s.presetKind.size() > 32 || s.presetName.size() > 256 ||
        s.category.size() > 128)
      return {};
    for (const auto &p : parameterTable())
      if (j["params"].contains(p.id)) {
        const auto &value = j["params"][p.id];
        if (!value.is_number())
          return {};
        double v = value.get<double>();
        if (!std::isfinite(v) || v < p.minValue || v > p.maxValue)
          return {};
        s.values[p.index] = p.isStepped ? std::round(v) : v;
      }
    auto &p = s.canvas;
    const auto &c = j.at("canvas");
    p.seed = c.at("seed").get<std::uint32_t>();
    p.painted = c.at("painted").get<bool>();
    p.recorded = c.at("recorded").get<bool>();
    p.previewWidth = c.at("width").get<unsigned>();
    p.previewHeight = c.at("height").get<unsigned>();
    if (p.previewWidth > p.previewSide || p.previewHeight > p.previewSide)
      return {};
    const auto &cells = c.at("cells");
    if (!cells.is_array() || cells.size() != p.cells.size())
      return {};
    for (unsigned i = 0; i < cells.size(); ++i) {
      if (!cells[i].is_array() || cells[i].size() != 7)
        return {};
      auto &a = p.cells[i];
      for (unsigned k = 0; k < 4; ++k)
        a.weights[k] = cells[i][k].get<float>();
      a.light = cells[i][4].get<float>();
      a.saturation = cells[i][5].get<float>();
      a.edge = cells[i][6].get<float>();
    }
    const auto &path = c.at("path");
    if (!path.is_array() || path.size() > p.maxPoints)
      return {};
    p.pointCount = unsigned(path.size());
    for (unsigned i = 0; i < p.pointCount; ++i) {
      if (!path[i].is_array() || path[i].size() != 3)
        return {};
      p.path[i] = {path[i][0].get<float>(), path[i][1].get<float>(),
                   path[i][2].get<float>()};
    }
    const auto &pixels = c.at("pixels");
    if (!pixels.is_array() || pixels.size() != p.previewWidth * p.previewHeight)
      return {};
    for (unsigned y = 0; y < p.previewHeight; ++y)
      for (unsigned x = 0; x < p.previewWidth; ++x)
        p.rgba[y * p.previewSide + x] =
            pixels[y * p.previewWidth + x].get<std::uint32_t>();
    if (!validProgram(p))
      return {};
    return s;
  } catch (const nlohmann::json::exception &) {
    return {};
  }
}
} // namespace daw::plugins::morph
