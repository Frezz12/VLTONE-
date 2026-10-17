#include "Internal/FluxProgram.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace daw::plugins::flux {
namespace {
constexpr const char *categories[]{
    "01 — Sidechain & Ducking", "02 — Gates & Grooves",
    "03 — Filters & Tone",      "04 — Time & Repeat",
    "05 — Glitch & Granular",   "06 — Drive & Lo-Fi",
    "07 — Pitch & Voices",      "08 — Stereo & Modulation",
    "09 — Space & Echo",        "10 — Transitions",
    "11 — Bass Tools",          "12 — Creative Chains"};
#define P(n, c, d, t) {n, categories[c], d, t}
constexpr FactoryPreset presets[]{
    P("Four Floor", 0, "Quarter-note pumping with a smooth recovery.",
      "Bass Synth Bus Rhythmic Live Friendly"),
    P("Tight Kick", 0, "Short dips leave space for the kick attack.",
      "Bass Drums Subtle Live Friendly"),
    P("Deep Pump", 0, "Deep ducking with a long convex recovery.",
      "Bass Pads Rhythmic Live Friendly"),
    P("Broken Kick", 0, "Syncopated ducking for broken beats.",
      "Drums Bass Rhythmic Live Friendly"),
    P("Ghost Snare", 0, "Upper-mid transients trigger a short dip.",
      "Drums Bus Subtle Live Friendly"),
    P("Swing Duck", 0, "Swung sixteenth-note ducking.",
      "Bass Synth Rhythmic Live Friendly"),
    P("Double Pulse", 0, "Two different dips in every beat.",
      "Synth Pads Rhythmic Live Friendly"),
    P("Slow Breath", 0, "Gentle two-bar amplitude breathing.",
      "Pads Guitar Subtle Live Friendly"),
    P("Multiband Duck", 0, "A pumping low band with an untouched upper range.",
      "Bass Bus Rhythmic Live Friendly"),
    P("Stereo Pump", 0, "Alternating stereo motion around a stable centre.",
      "Synth Pads Rhythmic Live Friendly"),
    P("Classic Trance", 1, "Crisp sixteenth-note trance gate.",
      "Synth Pads Rhythmic Live Friendly"),
    P("Offbeat Chops", 1, "Openings between the main beats.",
      "Synth Guitar Rhythmic Live Friendly"),
    P("Dotted Gate", 1, "Dotted openings create shifting accents.",
      "Pads Vocals Rhythmic Live Friendly"),
    P("Triplet Gate", 1, "Triplet chopping for sustained material.",
      "Pads Synth Rhythmic Live Friendly"),
    P("Euclid Five", 1, "Five evenly distributed hits over sixteen steps.",
      "Synth Drums Rhythmic Live Friendly"),
    P("Euclid Seven", 1, "Seven evenly distributed hits over sixteen steps.",
      "Synth Drums Rhythmic Live Friendly"),
    P("Shuffle Gate", 1, "Swung sixteenths with varied accents.",
      "Synth Guitar Rhythmic Live Friendly"),
    P("Ratchet Spark", 1, "Bursts of two, four and eight pulses.",
      "Drums Synth Extreme Live Friendly"),
    P("Polymeter Three", 1, "A three-step accent against the main bar.",
      "Pads Synth Rhythmic Live Friendly"),
    P("Human Pulse", 1, "Repeatable skipped hits and varied strength.",
      "Drums Synth Rhythmic Live Friendly"),
    P("Acid Steps", 2, "Stepped resonant low-pass cutoff.",
      "Bass Synth Rhythmic Live Friendly"),
    P("Ladder Rise", 2, "A saturated filter opens over two bars.",
      "Synth Bus Rhythmic Live Friendly"),
    P("Talking Wah", 2, "Rhythmic movement through vowel resonances.",
      "Guitar Bass Vocals Rhythmic Live Friendly"),
    P("Dual Sweep", 2, "Opposing high-pass and low-pass motion.",
      "Pads Bus Rhythmic Live Friendly"),
    P("Comb Plucks", 2, "Short tuned resonances from input attacks.",
      "Drums Guitar Rhythmic Live Friendly"),
    P("Envelope Snap", 2, "Input attacks open the filter.",
      "Bass Guitar Subtle Live Friendly"),
    P("Notch Orbit", 2, "A slowly moving notch.",
      "Pads Guitar Subtle Live Friendly"),
    P("Three Band Motion", 2, "Stable lows, pulsing mids, moving highs.",
      "Bus Synth Rhythmic Live Friendly"),
    P("Half Pace", 3, "Half-speed playback drops pitch by an octave.",
      "Drums Synth Rhythmic Studio"),
    P("Quarter World", 3, "Deep quarter-speed buffer playback.",
      "Drums Pads Extreme Studio"),
    P("Reverse Beat", 3, "Reverse quarter-note captures.",
      "Drums Guitar Rhythmic Studio"),
    P("Reverse Bar", 3, "Reverse a captured bar.",
      "Pads Vocals Extreme Studio"),
    P("Eighth Roll", 3, "Capture and repeat eighth-note slices.",
      "Drums Vocals Rhythmic Live Friendly"),
    P("Sixteenth Rush", 3, "Sixteenth-note fills at the end of a phrase.",
      "Drums Bus Rhythmic Live Friendly"),
    P("Triplet Repeat", 3, "Decaying triplet repeats.",
      "Drums Vocals Rhythmic Live Friendly"),
    P("Tape Brake", 3, "Slow a tape to a stop within one beat.",
      "Bus Drums Extreme Live Friendly"),
    P("Scratch Turn", 3, "A drawn forward-and-back buffer gesture.",
      "Drums Vocals Extreme Studio"),
    P("Stretch Drift", 3, "Slow captures while preserving their pitch.",
      "Pads Vocals Rhythmic Studio"),
    P("Micro Cut", 4, "Short bursts of sixty-fourth-note slices.",
      "Drums Vocals Extreme Live Friendly"),
    P("Buffer Shuffle", 4, "Rearrange four captured slices.",
      "Drums Synth Rhythmic Live Friendly"),
    P("Probability Break", 4, "Repeatable probability-driven repeat fills.",
      "Drums Bus Extreme Live Friendly"),
    P("Reverse Flick", 4, "Short reversed inserts at the end of beats.",
      "Drums Vocals Rhythmic Live Friendly"),
    P("Grain Cloud", 4, "Smooth overlapping grain cloud.",
      "Pads Guitar Studio Subtle"),
    P("Grain Rain", 4, "Short grains scattered across the stereo field.",
      "Synth Vocals Studio Rhythmic"),
    P("Frozen Dust", 4, "A frozen grain capture with digital erosion.",
      "Pads Vocals Studio Extreme"),
    P("Spectral Shards", 4, "Rhythmic pruning of spectral fragments.",
      "Drums Synth Studio Extreme"),
    P("Warm Tape", 5, "Soft tape saturation with rounded treble.",
      "Bus Guitar Subtle Live Friendly"),
    P("Tube Pulse", 5, "Saturation follows the input envelope.",
      "Bass Drums Rhythmic Live Friendly"),
    P("Fold Rhythm", 5, "Stepped wavefolding in project time.",
      "Bass Synth Extreme Live Friendly"),
    P("Broken Speaker", 5, "Narrow-band hard distortion.",
      "Vocals Guitar Extreme Live Friendly"),
    P("Eight Bit Walk", 5, "A rhythm of changing bit depths.",
      "Drums Synth Rhythmic Live Friendly"),
    P("Rate Falling", 5, "A falling sample-and-hold rate.",
      "Synth Bus Extreme Live Friendly"),
    P("Dusty Cassette", 5, "Hiss, crackle, wow and muted treble.",
      "Pads Guitar Subtle Live Friendly"),
    P("Digital Rust", 5, "Ring modulation meets bit reduction.",
      "Drums Bass Extreme Live Friendly"),
    P("Octave Down", 6, "Pitch down an octave without slowing time.",
      "Vocals Guitar Studio Rhythmic"),
    P("Octave Bloom", 6, "An octave-up voice opens into space.",
      "Vocals Pads Studio Rhythmic"),
    P("Fifth Sequence", 6, "Pitch pattern: 0, +7, +12, +7 semitones.",
      "Synth Vocals Studio Rhythmic"),
    P("Minor Stair", 6, "Pitch pattern: 0, +3, +7, +10 semitones.",
      "Synth Guitar Studio Rhythmic"),
    P("Formant Talk", 6, "Moving formants with an unchanged fundamental.",
      "Vocals Synth Studio Rhythmic"),
    P("Robot Syllables", 6, "Rhythmic vocoder with an internal carrier.",
      "Vocals Drums Rhythmic Live Friendly"),
    P("Ghost Choir", 6, "Detuned voices, chorus and soft reverb.",
      "Vocals Pads Studio Subtle"),
    P("Vocal Freeze", 6, "Periodically capture and hold the vocal spectrum.",
      "Vocals Studio Extreme"),
    P("Auto Orbit", 7, "A smooth pan around a bar.",
      "Guitar Synth Subtle Live Friendly"),
    P("Ping Steps", 7, "Alternating left and right steps.",
      "Drums Synth Rhythmic Live Friendly"),
    P("Wide Breath", 7, "Slowly widen and narrow the stereo image.",
      "Pads Bus Subtle Live Friendly"),
    P("Mono Focus", 7, "Mono bass with controlled upper-band width.",
      "Bus Bass Subtle Live Friendly"),
    P("Liquid Chorus", 7, "Several chorus voices with evolving depth.",
      "Guitar Pads Subtle Live Friendly"),
    P("Jet Sweep", 7, "A pronounced long flanger sweep.",
      "Drums Guitar Extreme Live Friendly"),
    P("Phaser Spiral", 7, "A stereo multi-stage phaser orbit.",
      "Pads Guitar Rhythmic Live Friendly"),
    P("Ring Satellite", 7, "Ring modulation moving through the stereo field.",
      "Synth Guitar Extreme Live Friendly"),
    P("Dotted Echo", 8, "Dotted ping-pong repeats.",
      "Guitar Vocals Rhythmic Live Friendly"),
    P("Dub Feedback", 8, "Rhythmic bursts of filtered feedback.",
      "Drums Bus Rhythmic Live Friendly"),
    P("Reverse Room", 8, "Reversed slices feeding a short room.",
      "Guitar Vocals Studio Rhythmic"),
    P("Gated Hall", 8, "A large hall with a chopped tail.",
      "Drums Synth Rhythmic Live Friendly"),
    P("Shimmer Lift", 8, "Reverb with an octave-up tail.",
      "Pads Guitar Studio Subtle"),
    P("Frozen Chamber", 8, "Hold a space and control its decay.",
      "Pads Vocals Studio Extreme"),
    P("Spectral Mist", 8, "Blurred spectrum and diffuse echoes.",
      "Pads Guitar Studio Subtle"),
    P("Resonant Space", 8, "Tuned resonances inside a long space.",
      "Guitar Drums Studio Rhythmic"),
    P("Filter Lift", 9, "An eight-bar filter opening.",
      "Bus Synth Rhythmic Live Friendly"),
    P("Noise Riser", 9, "An eight-bar coloured-noise rise.",
      "Bus Synth Rhythmic Live Friendly"),
    P("Falling Room", 9, "Falling pitch disappears into a long tail.",
      "Bus Vocals Studio Extreme"),
    P("Tape Exit", 9, "Tape stop with a continuing echo tail.",
      "Bus Drums Rhythmic Live Friendly"),
    P("Granular Lift", 9, "Rising grain density, pitch and width.",
      "Pads Bus Studio Extreme"),
    P("Spectral Dissolve", 9, "Dissolve the input into spectral blur.",
      "Pads Bus Studio Extreme"),
    P("Build And Drop", 9, "Four-bar acceleration, then a dry return.",
      "Bus Drums Rhythmic Live Friendly"),
    P("Impact Vacuum", 9, "A short pre-impact dip and a space burst.",
      "Drums Bus Rhythmic Live Friendly"),
    P("Sub Breather", 10, "Quarter-note ducking below 120 Hz.",
      "Bass Bus Subtle Live Friendly"),
    P("Talking Bass", 10, "Vowel motion and saturated bass mids.",
      "Bass Rhythmic Live Friendly"),
    P("Reese Motion", 10, "Moving chorus mids over a stable mono sub.",
      "Bass Rhythmic Live Friendly"),
    P("Metal Edge", 10, "Frequency-shift the upper bass harmonics.",
      "Bass Extreme Live Friendly"),
    P("Bass Stutter", 10, "Repeat the mids while the sub stays continuous.",
      "Bass Rhythmic Live Friendly"),
    P("Low Punch", 10, "Shape bass attack and sustain.",
      "Bass Drums Subtle Live Friendly"),
    P("Rubber Bass", 10, "A short resonant filter envelope.",
      "Bass Rhythmic Live Friendly"),
    P("Dark Half", 10, "Dark half-time with gentle saturation.",
      "Bass Drums Studio Rhythmic"),
    P("Neon Machine", 11, "Drive, repeats, stepped filter and stereo motion.",
      "Drums Synth Extreme Live Friendly"),
    P("Clockwork Vowel", 11,
      "Euclidean gating, vowel filters and synced echoes.",
      "Synth Vocals Rhythmic Live Friendly"),
    P("Glass Garden", 11, "Grains, tuned resonators and shimmer.",
      "Pads Guitar Studio Extreme"),
    P("Broken Gravity", 11, "Reverse, tape stops and digital destruction.",
      "Drums Bus Studio Extreme"),
    P("Spectral Voyage", 11,
      "Spectral freeze, formant changes and slow panning.",
      "Vocals Pads Studio Extreme"),
    P("Flux Showcase", 11, "Four scenes: pulse, chops, grains and space.",
      "Synth Drums Pads Studio Rhythmic")};
#undef P
static_assert(std::size(presets) == 100);
} // namespace
std::span<const FactoryPreset> factoryPresets() noexcept { return presets; }
namespace {
void randomizeAll(State &s, unsigned seed) {
  auto locked = [&](unsigned id) {
    return std::find(s.parameterLocks.begin(), s.parameterLocks.end(), id) !=
           s.parameterLocks.end();
  };
  std::array<bool, slotCount> pinned{};
  for (unsigned i = 0; i < slotCount; ++i) {
    pinned[i] = s.modules[i].locked;
    for (auto id : s.parameterLocks)
      pinned[i] |= id >= GlobalCount && (id - GlobalCount) / bankSize == i;
  }
  auto r = [&](unsigned event, unsigned salt) {
    return randomAt(seed, event, salt);
  };
  const unsigned count = 2 + unsigned(r(0, 101) * 11);
  s.routing = Routing(unsigned(r(0, 102) * 3));
  for (unsigned i = 0; i < slotCount; ++i) {
    if (pinned[i]) {
      if (s.modules[i].lane == Lane::C && s.routing == Routing::Parallel)
        s.routing = Routing::Multiband;
      continue;
    }
    setKind(s, i,
            i < count
                ? Kind(1 + unsigned(r(i, 103) * (unsigned(Kind::Count) - 1)))
                : Kind::Empty);
    const unsigned lane = unsigned(r(i, 104) * 5);
    s.modules[i].lane = lane == 0                        ? Lane::Pre
                        : lane == 4                      ? Lane::Post
                        : s.routing == Routing::Serial   ? Lane::A
                        : s.routing == Routing::Parallel ? Lane(1 + lane % 2)
                                                         : Lane(lane);
  }
  // Keep explicitly locked slots at their existing positions.
  for (unsigned i = slotCount - 1; i > 0; --i) {
    const unsigned j = unsigned(r(i, 105) * (i + 1));
    if (!pinned[s.order[i]] && !pinned[s.order[j]])
      std::swap(s.order[i], s.order[j]);
  }
  while (s.scenes.size() < 4)
    s.scenes.push_back(s.scenes.front());
  const unsigned edited = s.editScene;
  for (unsigned scene = 0; scene < s.scenes.size(); ++scene) {
    s.editScene = scene;
    randomize(s, hash(seed + scene), 7);
    auto &sc = s.scenes[scene];
    auto sr = [&](unsigned event, unsigned salt) {
      return randomAt(hash(seed + scene), event, salt);
    };
    std::vector<unsigned> targets;
    for (unsigned i = 0; i < slotCount; ++i) {
      const auto kind = s.modules[i].kind;
      if (kind == Kind::Empty || s.modules[i].locked)
        continue;
      auto &slot = sc.modules[i];
      const auto defs = fields(kind);
      for (unsigned f = 0; f < defs.size(); ++f) {
        const auto id = effectParam(i, kind, f);
        if (locked(id))
          continue;
        double value = plain(defs[f], sr(i, 120 + f));
        if (std::string_view(defs[f].id) == "feedback")
          value = std::clamp(value, -.85, .85);
        if (std::string_view(defs[f].id) == "freeze")
          value = sr(i, 144 + f) < .12;
        // The generated sound must work on the current audio without external
        // sources.
        if (kind == Kind::Dynamics && f == 8)
          value = 0;
        if (kind == Kind::Vocoder && f == 6)
          value = sr(i, 145) > .5 ? 1 : 0;
        if (kind == Kind::Volume && f == 0)
          value = .3 + .7 * sr(i, 146);
        slot.values[f] = value;
        if (!defs[f].stepped)
          targets.push_back(id);
      }
      if (!locked(commonParam(i, Enabled)))
        slot.enabled = i == 0 || sr(i, 147) > .1;
      if (!locked(commonParam(i, Solo)))
        slot.solo = false;
      if (!locked(commonParam(i, SlotOutput)))
        slot.output = -1.5 + 3 * sr(i, 148);
      for (unsigned c = 0; c < 2; ++c) {
        auto &curve = slot.curves[c];
        if (!locked(commonParam(i, DepthA + c))) {
          curve.target = unsigned(sr(i, 150 + c) * defs.size());
          curve.phase = sr(i, 152 + c);
          curve.bipolar = sr(i, 154 + c) > .5;
          curve.depth *= sr(i, 156 + c) > .5 ? 1 : -1;
          curve.play =
              sr(i, 158 + c) < .15 ? PlayMode::OneShot : PlayMode::Loop;
          curve.trigger =
              sr(i, 160 + c) < .2 ? TriggerMode::Audio : TriggerMode::Host;
          for (unsigned p = 0; p < curve.points.size(); ++p) {
            curve.points[p].tension = float(sr(i + p, 162 + c) * 2 - 1);
            curve.points[p].shape = Shape(unsigned(sr(i + p, 164 + c) * 5));
          }
        }
        if (!locked(commonParam(i, RateA + c))) {
          curve.sync = sr(i, 166 + c) > .15;
          curve.hz = .05 * std::pow(400., sr(i, 168 + c));
        }
      }
      auto &seq = slot.sequence;
      constexpr unsigned lengths[]{8, 12, 16, 24, 32, 48, 64};
      seq.length = lengths[unsigned(sr(i, 172) * std::size(lengths))];
      seq.division = std::exp2(-int(sr(i, 173) * 6));
      seq.swing = .5 * sr(i, 174);
      seq.direction = Direction(unsigned(sr(i, 175) * 4));
      seq.enabled = sr(i, 176) > .2;
      euclidean(seq,
                std::max(1u, unsigned(seq.length * (.35 + .6 * sr(i, 177)))),
                unsigned(sr(i, 178) * seq.length));
      for (unsigned n = 0; n < seq.length; ++n) {
        auto &step = seq.steps[n];
        step.amount = float(.3 + .7 * sr(i * maxSteps + n, 179));
        step.probability = float(.55 + .45 * sr(i * maxSteps + n, 180));
        step.retrigger = sr(i * maxSteps + n, 181) < .25;
        step.ratchet = 1 + unsigned(sr(i * maxSteps + n, 182) * 8);
      }
    }
    for (unsigned m = 0; m < 4; ++m) {
      if (!locked(Macro1 + m))
        sc.macros[m] = sr(m, 190);
      auto &mod = sc.modulators[m];
      mod.type = unsigned(sr(m, 191) * 4);
      mod.shape = unsigned(sr(m, 192) * 4);
      mod.sync = sr(m, 193) > .2;
      mod.rate = mod.sync ? std::exp2(int(sr(m, 194) * 8) - 4)
                          : .05 * std::pow(400., sr(m, 194));
      mod.phase = sr(m, 195);
      mod.attack = .001 + .1 * sr(m, 196);
      mod.release = .02 + .8 * sr(m, 197);
      mod.smooth = .2 * sr(m, 198);
      mod.sidechain = false;
      for (unsigned n = 0; n < mod.steps.size(); ++n)
        mod.steps[n] = float(sr(m * 16 + n, 199));
    }
    std::erase_if(sc.routes, [&](const auto &route) {
      return !locked(route.target) &&
             (route.target < GlobalCount ||
              !s.modules[(route.target - GlobalCount) / bankSize].locked);
    });
    if (!targets.empty())
      for (unsigned n = 0; n < 12 && sc.routes.size() < maxRoutes; ++n) {
        const unsigned source = n < 10 ? n : 11;
        sc.routes.push_back({source,
                             targets[unsigned(sr(n, 200) * targets.size())],
                             (sr(n, 201) * 2 - 1) * .35, .15 * sr(n, 202),
                             .75 + .25 * sr(n, 203), sr(n, 204) > .5, true});
      }
  }
  s.editScene = edited;
  s.seed = seed ? seed : 1;
  auto global = [&](unsigned id, double value) {
    if (!locked(id))
      s.globals[id] = value;
  };
  global(Input, -3 + 6 * r(0, 210));
  global(Output, -3 * r(0, 211));
  global(Mix, .35 + .65 * r(0, 212));
  global(Bypass, 0);
  for (unsigned m = 0; m < 4; ++m)
    global(Macro1 + m, s.scenes[edited].macros[m]);
  global(X, r(0, 213));
  global(Y, r(0, 214));
  global(LowSplit, 40 * std::pow(20., r(0, 215)));
  global(HighSplit, 1500 * std::pow(6., r(0, 216)));
  if (s.globals[LowSplit] >= s.globals[HighSplit]) {
    if (!locked(LowSplit))
      s.globals[LowSplit] = s.globals[HighSplit] * .5;
    else if (!locked(HighSplit))
      s.globals[HighSplit] = s.globals[LowSplit] * 1.5;
  }
  global(Balance, r(0, 217));
  for (unsigned b = 0; b < 3; ++b) {
    global(LowGain + b, -4 + 6 * r(b, 218));
    global(LowMute + b, 0);
    global(LowSolo + b, 0);
  }
  global(SceneSelect, edited);
  global(Quantize, unsigned(r(0, 219) * 3));
  s.sceneMode = unsigned(r(0, 220) * 3);
  s.detector = {-48 + 36 * r(0, 221),   .4 + 2 * r(0, 222),
                .001 + .02 * r(0, 223), .03 + .5 * r(0, 224),
                .02 + .3 * r(0, 225),   2 + 10 * r(0, 226),
                30 + 300 * r(0, 227),   2000 + 14000 * r(0, 228)};
  if (!locked(X) && !locked(Y)) {
    s.path.clear();
    const unsigned points = 8 + unsigned(r(0, 229) * 25);
    for (unsigned n = 0; n < points; ++n)
      s.path.push_back(
          {float(n) / (points - 1), float(r(n, 230)), float(r(n, 231))});
    s.pathBars = std::exp2(int(r(0, 232) * 5) - 1);
    s.pathSmooth = .01 + .3 * r(0, 233);
    s.pathEnabled = r(0, 234) > .25;
    s.pathReverse = r(0, 235) > .5;
  }
  s.tags = "Random";
}
} // namespace
void randomize(State &s, unsigned seed, unsigned scope) {
  if (!validState(s))
    return;
  if (scope & 8) {
    randomizeAll(s, seed ? seed : 1);
    return;
  }
  auto locked = [&](unsigned id) {
    return std::find(s.parameterLocks.begin(), s.parameterLocks.end(), id) !=
           s.parameterLocks.end();
  };
  bool empty = std::all_of(s.modules.begin(), s.modules.end(),
                           [](auto &slot) { return slot.kind == Kind::Empty; });
  if (empty) {
    const Kind initKinds[]{Kind::Volume, Kind::Filter, Kind::Delay};
    for (unsigned i = 0; i < 3; ++i) {
      const bool pinned =
          s.modules[i].locked ||
          std::any_of(s.parameterLocks.begin(), s.parameterLocks.end(),
                      [i](unsigned id) {
                        return id >= GlobalCount &&
                               (id - GlobalCount) / bankSize == i;
                      });
      if (!pinned)
        setKind(s, i, initKinds[i]);
    }
    if (s.modules[2].kind == Kind::Delay)
      s.scenes[s.editScene].modules[2].mix = .25;
  }
  auto &sc = s.scenes[s.editScene];
  for (unsigned i = 0; i < slotCount; ++i) {
    if (s.modules[i].kind == Kind::Empty || s.modules[i].locked)
      continue;
    auto kind = s.modules[i].kind;
    auto &slot = sc.modules[i];
    auto defs = fields(kind);
    if (scope & 1)
      for (unsigned f = 0; f < defs.size(); ++f) {
        if (locked(effectParam(i, kind, f)))
          continue;
        const auto &field = defs[f];
        double r = randomAt(seed, i, f);
        double value = plain(
            field, std::clamp(normalized(field, field.initial) + (r - .5) * .85,
                              0., 1.));
        if (kind == Kind::Dynamics && f == 8)
          value = 0;
        if (kind == Kind::Vocoder && f == 6)
          value = r > .5 ? 1 : 0;
        if (std::string_view(field.id) == "freeze")
          value = 0;
        if (kind == Kind::Volume && f == 0)
          value = .25 + .75 * r;
        if (std::string_view(field.id) == "feedback")
          value = std::clamp(value, -.8, .8);
        slot.values[f] = value;
      }
    if (scope & 1) {
      if (!locked(commonParam(i, SlotMix)))
        slot.mix = .25 + .75 * randomAt(seed, i, 81);
      for (unsigned c = 0; c < 2; ++c) {
        auto &curve = slot.curves[c];
        if (curve.depth != 0 && !locked(commonParam(i, DepthA + c)))
          curve.depth =
              std::copysign(.2 + .8 * randomAt(seed, i, 82 + c), curve.depth);
        if (!locked(commonParam(i, RateA + c))) {
          if (curve.sync)
            curve.bars = std::exp2(int(randomAt(seed, i, 84 + c) * 7) - 4);
          else
            curve.hz = .1 + 8 * randomAt(seed, i, 84 + c);
        }
      }
    }
    if (scope & 2)
      for (unsigned c = 0; c < 2; ++c) {
        if (locked(commonParam(i, DepthA + c)))
          continue;
        auto &curve = slot.curves[c];
        makeShape(curve, unsigned(randomAt(seed, i, 20 + c) * 8), seed + i + c);
        curve.depth = .2 + randomAt(seed, i, 23 + c) * .8;
        if (!locked(commonParam(i, RateA + c)))
          curve.bars = std::exp2(int(randomAt(seed, i, 26 + c) * 6) - 3);
      }
    if (scope & 4) {
      auto &q = slot.sequence;
      q.enabled = true;
      q.length = 16;
      euclidean(q, 4 + unsigned(randomAt(seed, i, 33) * 11),
                unsigned(randomAt(seed, i, 34) * 16));
      for (unsigned st = 0; st < q.length; ++st) {
        q.steps[st].amount = float(.4 + randomAt(seed, st, 40 + i) * .6);
        q.steps[st].probability = float(.65 + randomAt(seed, st, 60 + i) * .35);
      }
    }
  }
  s.presetId = "random";
  s.name = "Random variation";
  s.category = "";
  s.description = "Unsaved variation. Random again for another result; Undo "
                  "restores the previous sound.";
}
State factoryState(unsigned index) {
  if (index >= 100)
    return initialState();
  State s = initialState();
  const auto &meta = presets[index];
  char id[24];
  std::snprintf(id, sizeof(id), "factory.%03u", index + 1);
  s.presetId = id;
  s.name = meta.name;
  s.category = meta.category;
  s.description = meta.description;
  s.tags = meta.tags;
  s.seed = hash(index + 0xf10);
  unsigned count = 0;
  auto add = [&](Kind k,
                 std::initializer_list<std::pair<unsigned, double>> values = {},
                 double mix = 1., Lane lane = Lane::A) -> unsigned {
    unsigned slot = count++;
    setKind(s, slot, k);
    s.modules[slot].lane = lane;
    auto &ss = s.scenes[0].modules[slot];
    ss.mix = mix;
    for (auto [p, v] : values)
      ss.values[p] = v;
    return slot;
  };
  auto curve = [&](unsigned slot, unsigned target, unsigned shape,
                   double bars = 1., double depth = 1.,
                   unsigned c = 0) -> Curve & {
    auto &cv = s.scenes[0].modules[slot].curves[c];
    cv.target = target;
    cv.bars = bars;
    cv.depth = depth;
    makeShape(cv, shape, s.seed + slot);
    return cv;
  };
  auto seq = [&](unsigned slot, unsigned pulses, unsigned length = 16,
                 double division = .25) -> Sequence & {
    auto &q = s.scenes[0].modules[slot].sequence;
    q.enabled = true;
    q.length = length;
    q.division = division;
    euclidean(q, pulses, 0);
    return q;
  };
  auto route = [&](unsigned source, unsigned target, double amount,
                   bool bipolar = true) {
    s.scenes[0].routes.push_back({source, target, amount, 0, 1, bipolar, true});
  };
  auto multi = [&](double lo = 160, double hi = 3000) {
    s.routing = Routing::Multiband;
    s.globals[LowSplit] = lo;
    s.globals[HighSplit] = hi;
  };
  if (index < 10) {
    unsigned a = add(Kind::Volume);
    auto &c = curve(a, 0, 0, .25, index == 7 ? .35 : 1.);
    switch (index) {
    case 1:
      c.points = {{0, 0, 0, Shape::Hold},
                  {.08f, 0, 0, Shape::Smooth},
                  {.24f, 1, 0, Shape::Linear},
                  {1, 1, 0, Shape::Linear}};
      break;
    case 2:
      c.points = {{0, 0, 0, Shape::Hold},
                  {.2f, 0, 0, Shape::Exponential},
                  {1, 1, .5f, Shape::Smooth}};
      break;
    case 3:
      c.bars = 1;
      seq(a, 11).steps[3].amount = .3f;
      break;
    case 4:
      c.bars = .125;
      c.trigger = TriggerMode::Audio;
      c.play = PlayMode::OneShot;
      s.detector.highpass = 1200;
      s.detector.lowpass = 6500;
      break;
    case 5:
      seq(a, 16).swing = .35;
      c.bars = .0625;
      break;
    case 6:
      c.points = {{0, 0, 0, Shape::Smooth},
                  {.3f, 1, 0, Shape::Hold},
                  {.5f, .2f, 0, Shape::Smooth},
                  {1, 1, 0, Shape::Linear}};
      break;
    case 7:
      makeShape(c, 3);
      c.bars = 2;
      break;
    case 8:
      multi();
      break;
    case 9:
      add(Kind::Stereo, {{3, 1}, {4, .65}});
      break;
    default:
      break;
    }
  } else if (index < 20) {
    unsigned a = add(Kind::Volume, {{1, 1.2}});
    curve(a, 0, 4, .0625, .95);
    auto &q = seq(a, index == 14 ? 5 : index == 15 ? 7 : 12);
    switch (index) {
    case 11:
      euclidean(q, 8, 1);
      break;
    case 12:
      q.division = .375;
      break;
    case 13:
      q.division = 1. / 3;
      break;
    case 16:
      q.swing = .4;
      for (unsigned i = 0; i < 16; ++i)
        q.steps[i].amount = i % 2 ? .55f : 1;
      break;
    case 17:
      for (unsigned i = 0; i < 16; ++i)
        q.steps[i].ratchet = 1u << (i % 4);
      break;
    case 18:
      q.length = 3;
      q.steps[0].amount = 1;
      q.steps[1].amount = .3f;
      q.steps[2].amount = .6f;
      break;
    case 19:
      for (unsigned i = 0; i < 16; ++i) {
        q.steps[i].probability = .72f;
        q.steps[i].amount = float(.55 + randomAt(s.seed, i) * .45);
      }
      break;
    default:
      break;
    }
  } else
    switch (index) {
    case 20: {
      auto a = add(Kind::Filter, {{1, 5}, {4, 1}, {5, 8}});
      curve(a, 0, 6, 1, .65);
      break;
    }
    case 21: {
      auto a = add(Kind::Filter, {{1, 2}, {4, 1}, {5, 12}});
      curve(a, 0, 1, 2);
      break;
    }
    case 22:
      curve(add(Kind::Vowel), 0, 3, .5);
      break;
    case 23: {
      auto a = add(Kind::Filter, {{2, 7}, {1, .707}});
      curve(a, 0, 3, 2, .65);
      auto &b = curve(a, 7, 3, 2, .8, 1);
      b.phase = .5;
      break;
    }
    case 24:
      add(Kind::Resonator, {{0, 55}, {1, .16}, {3, 3}}, .65);
      break;
    case 25: {
      auto a = add(Kind::Filter, {{0, 240}, {1, 2}});
      route(11, effectParam(a, Kind::Filter, 0), .8, false);
      break;
    }
    case 26:
      curve(add(Kind::Filter, {{2, 3}, {1, 4}}), 0, 3, 4, .7);
      break;
    case 27:
      multi();
      curve(add(Kind::Volume, {}, 1, Lane::B), 0, 0, .25, .6);
      curve(add(Kind::Filter, {{2, 3}}, .7, Lane::C), 0, 3, 2, .6);
      break;
    case 28:
      add(Kind::Time, {{2, 1}, {3, 4}});
      break;
    case 29:
      add(Kind::Time, {{2, 2}, {3, 8}});
      break;
    case 30:
      add(Kind::Time, {{2, 3}, {3, 1}});
      break;
    case 31:
      add(Kind::Time, {{2, 3}, {3, 1}, {6, 1}});
      break;
    case 32:
      add(Kind::Repeat, {{0, .5}, {1, 2}, {2, .08}});
      break;
    case 33: {
      auto a = add(Kind::Repeat, {{0, .25}, {1, 1}, {2, .1}});
      auto &q = seq(a, 16);
      for (unsigned i = 0; i < 16; ++i)
        q.steps[i].enabled = i >= 12;
      break;
    }
    case 34:
      add(Kind::Repeat, {{0, 1. / 3}, {1, 2}, {2, .22}});
      break;
    case 35: {
      auto &c = curve(add(Kind::Tape, {{1, 0}, {2, 0}, {5, .25}}), 0, 1, .25);
      transformCurve(c, 0);
      c.play = PlayMode::OneShot;
      break;
    }
    case 36:
      curve(add(Kind::Time, {{2, 4}, {3, 2}}), 0, 2, .5);
      break;
    case 37:
      add(Kind::Time, {{2, 6}, {1, .5}, {3, 4}});
      s.studio = true;
      break;
    case 38: {
      auto a = add(Kind::Repeat, {{0, .0625}, {1, 1}, {2, .05}});
      seq(a, 5);
      break;
    }
    case 39:
      add(Kind::Repeat, {{0, .25}, {1, 1}, {4, 1}, {2, 0}});
      break;
    case 40: {
      auto a = add(Kind::Repeat, {{0, .125}, {1, 2}, {4, 1}});
      auto &q = seq(a, 9);
      for (auto &st : q.steps)
        st.probability = .6f;
      break;
    }
    case 41: {
      auto a = add(Kind::Repeat, {{0, .25}, {1, 1}, {3, 1}, {2, 0}});
      seq(a, 4, 16).steps[3].enabled = true;
      break;
    }
    case 42:
      add(Kind::Granular, {{1, 190}, {2, 45}, {4, .15}, {6, .65}}, .7);
      s.studio = true;
      break;
    case 43:
      add(Kind::Granular, {{1, 18}, {2, 70}, {4, .75}, {6, 1}}, .75);
      break;
    case 44:
      add(Kind::Granular, {{7, 1}, {1, 120}, {5, .4}}, .8);
      add(Kind::Crush, {{0, 7}, {1, 6000}}, .6);
      s.studio = true;
      break;
    case 45:
      curve(add(Kind::Spectral, {{1, .1}, {5, .6}}), 5, 6, .5, .65);
      s.studio = true;
      break;
    case 46:
      add(Kind::Drive, {{0, 6}, {1, 3}, {3, 7000}}, .75);
      break;
    case 47: {
      auto a = add(Kind::Drive, {{0, 3}, {1, 2}}, .6);
      route(11, effectParam(a, Kind::Drive, 0), .5, false);
      break;
    }
    case 48:
      curve(add(Kind::Drive, {{1, 4}, {0, 10}}, .45), 0, 6, 1, .5);
      break;
    case 49:
      add(Kind::Filter, {{2, 7}, {0, 350}, {7, 3500}});
      add(Kind::Drive, {{1, 1}, {0, 24}, {3, 3200}}, .7);
      s.globals[Output] = -5;
      break;
    case 50:
      curve(add(Kind::Crush, {{0, 8}, {1, 14000}}), 0, 6, 1, .75);
      break;
    case 51: {
      auto &c = curve(add(Kind::Crush, {{0, 16}}), 1, 1, 2, .8);
      transformCurve(c, 0);
      break;
    }
    case 52:
      add(Kind::Tape, {{1, .65}, {2, .3}, {3, 4}, {4, 6500}}, .5);
      add(Kind::Noise, {{0, -36}, {1, 3}}, .6);
      add(Kind::Noise, {{0, -30}, {1, 4}}, .5);
      break;
    case 53:
      add(Kind::Ring, {{0, 170}, {1, .45}}, .5);
      add(Kind::Crush, {{0, 7}, {1, 8000}, {2, .2}}, .7);
      break;
    case 54:
      add(Kind::Pitch, {{0, -12}});
      s.studio = true;
      break;
    case 55:
      add(Kind::Pitch, {{0, 12}}, .6);
      curve(add(Kind::Reverb, {{0, 5}, {4, 4}}, .4), 0, 1, 2, .35);
      s.studio = true;
      break;
    case 56:
    case 57: {
      auto a = add(Kind::Pitch);
      auto &c = curve(a, 0, 6, 1);
      c.points.clear();
      double pitches[4]{0, index == 56 ? 7. : 3., index == 56 ? 12. : 7.,
                        index == 56 ? 7. : 10.};
      for (unsigned i = 0; i < 4; ++i)
        c.points.push_back(
            {i * .25f, float((pitches[i] + 24) / 48), 0, Shape::Hold});
      c.points.push_back({1, .5, 0, Shape::Hold});
      s.studio = true;
      break;
    }
    case 58:
      curve(add(Kind::Pitch, {{3, 0}}), 2, 3, 1, .65);
      s.studio = true;
      break;
    case 59: {
      auto a = add(Kind::Vocoder, {{0, 48}, {7, 0}});
      seq(a, 11);
      break;
    }
    case 60:
      add(Kind::Pitch, {{1, 12}}, .4);
      add(Kind::Chorus, {{2, 4}, {3, 18}}, .6);
      add(Kind::Reverb, {{0, 4}}, .3);
      s.studio = true;
      break;
    case 61:
      curve(add(Kind::Spectral, {{1, .2}}), 0, 4, 1);
      s.studio = true;
      break;
    case 62:
      curve(add(Kind::Stereo), 0, 3, 1, .85);
      break;
    case 63:
      curve(add(Kind::Stereo), 0, 4, .25, .8);
      break;
    case 64:
      curve(add(Kind::Stereo), 1, 3, 4, .4);
      break;
    case 65:
      multi(140, 2500);
      add(Kind::Stereo, {{1, 0}}, 1, Lane::A);
      add(Kind::Stereo, {{1, 1.35}}, 1, Lane::B);
      add(Kind::Stereo, {{1, 1.65}}, 1, Lane::C);
      break;
    case 66:
      curve(add(Kind::Chorus, {{2, 4}, {0, .18}, {3, 12}}), 1, 3, 4, .45);
      break;
    case 67:
      add(Kind::Flanger, {{0, .06}, {1, 5}, {2, .82}, {4, 120}}, .7);
      break;
    case 68:
      add(Kind::Phaser, {{0, .13}, {3, 3}, {2, .55}, {5, 120}}, .8);
      break;
    case 69:
      add(Kind::Ring, {{0, 62}, {1, .7}}, .7);
      add(Kind::Stereo, {{3, .3}, {4, .8}});
      break;
    case 70:
      add(Kind::Delay, {{0, .75}, {1, .42}}, .35);
      break;
    case 71:
      curve(add(Kind::Delay, {{0, .5}, {1, .6}, {2, 350}, {3, 3400}}, .4), 1, 5,
            1, .45);
      break;
    case 72:
      add(Kind::Time, {{2, 3}, {3, 1}}, .65);
      add(Kind::Reverb, {{0, .65}, {4, 0}, {1, .65}}, .25);
      break;
    case 73:
      add(Kind::Reverb, {{0, 5}, {4, 3}}, .5);
      curve(add(Kind::Volume), 0, 4, .125, .9);
      break;
    case 74:
      add(Kind::Reverb, {{0, 8}, {4, 4}, {7, .65}}, .5);
      s.studio = true;
      break;
    case 75:
      add(Kind::Reverb, {{0, 10}, {6, 1}, {1, 1.4}}, .65);
      s.studio = true;
      break;
    case 76:
      add(Kind::Spectral, {{1, .85}, {5, .12}}, .7);
      add(Kind::Delay, {{0, 1.5}, {1, .55}, {5, .8}}, .4);
      s.studio = true;
      break;
    case 77:
      add(Kind::Resonator, {{0, 60}, {1, 1.8}, {3, 5}}, .45);
      add(Kind::Reverb, {{0, 7}}, .5);
      s.studio = true;
      break;
    case 78: {
      auto &c = curve(add(Kind::Filter, {{1, 1.8}, {4, 1}}), 0, 1, 8);
      c.play = PlayMode::OneShot;
      break;
    }
    case 79: {
      auto &c = curve(add(Kind::Noise, {{1, 1}, {0, -18}, {3, 0}, {4, -80}}), 0,
                      1, 8, .8);
      c.play = PlayMode::OneShot;
      add(Kind::Filter, {{2, 1}, {0, 400}});
      break;
    }
    case 80: {
      auto &c = curve(add(Kind::Pitch), 0, 1, 4, .65);
      transformCurve(c, 0);
      c.play = PlayMode::OneShot;
      add(Kind::Reverb, {{0, 12}}, .6);
      s.studio = true;
      break;
    }
    case 81: {
      auto &c = curve(add(Kind::Tape, {{1, 0}, {2, 0}}), 0, 1, 1);
      transformCurve(c, 0);
      c.play = PlayMode::OneShot;
      add(Kind::Delay, {{0, .75}, {1, .65}}, .4);
      break;
    }
    case 82: {
      auto a = add(Kind::Granular, {{1, 80}, {2, 50}}, .8);
      curve(a, 2, 1, 8, .8).play = PlayMode::OneShot;
      curve(a, 3, 1, 8, .5, 1).play = PlayMode::OneShot;
      s.studio = true;
      break;
    }
    case 83:
      curve(add(Kind::Spectral), 1, 1, 8).play = PlayMode::OneShot;
      s.studio = true;
      break;
    case 84: {
      auto a = add(Kind::Repeat, {{1, 4}, {2, .04}});
      auto &c = curve(a, 0, 1, 4);
      transformCurve(c, 0);
      c.points = {{0, .6f, 0, Shape::Hold},   {.25f, .45f, 0, Shape::Hold},
                  {.5f, .3f, 0, Shape::Hold}, {.75f, .15f, 0, Shape::Hold},
                  {.96f, 0, 0, Shape::Hold},  {1, 0, 0, Shape::Hold}};
      auto &q = seq(a, 64, 64);
      for (unsigned i = 60; i < 64; ++i)
        q.steps[i].enabled = false;
      break;
    }
    case 85: {
      auto a = add(Kind::Volume);
      auto &c = curve(a, 0, 0, 1);
      c.points = {{0, 1, 0, Shape::Hold},
                  {.8f, 1, 0, Shape::Smooth},
                  {.94f, 0, 0, Shape::Hold},
                  {.99f, 0, 0, Shape::Linear},
                  {1, 1, 0, Shape::Linear}};
      add(Kind::Reverb, {{0, 3}}, .3);
      break;
    }
    case 86:
      multi(120);
      curve(add(Kind::Volume, {}, 1, Lane::A), 0, 0, .25, .75);
      break;
    case 87:
      multi(120, 4000);
      curve(add(Kind::Vowel, {}, .75, Lane::B), 0, 3, .5, .7);
      add(Kind::Drive, {{0, 10}, {1, 2}}, .5, Lane::B);
      break;
    case 88:
      multi(130);
      add(Kind::Stereo, {{1, 0}}, 1, Lane::A);
      add(Kind::Chorus, {{0, .25}, {1, 7}, {3, 15}}, .75, Lane::B);
      break;
    case 89:
      multi(120, 1400);
      add(Kind::Ring, {{0, 73}, {2, 1}}, .45, Lane::C);
      break;
    case 90:
      multi(140);
      add(Kind::Repeat, {{0, .25}, {1, 2}}, .8, Lane::B);
      break;
    case 91:
      add(Kind::Dynamics, {{5, 2}, {6, 7}, {7, -3}, {2, 2}, {3, 130}}, .8);
      break;
    case 92: {
      auto a = add(Kind::Filter, {{0, 180}, {1, 3}});
      auto &c = curve(a, 0, 0, .25, .55);
      transformCurve(c, 0);
      break;
    }
    case 93:
      add(Kind::Time, {{2, 1}, {3, 4}}, .85);
      add(Kind::Filter, {{0, 2800}});
      add(Kind::Drive, {{0, 5}, {1, 3}}, .5);
      break;
    case 94:
      add(Kind::Drive, {{0, 12}, {1, 2}}, .6);
      add(Kind::Repeat, {{0, .25}, {1, 2}, {4, 1}}, .5);
      curve(add(Kind::Filter, {{1, 3}}), 0, 6, 1, .7);
      add(Kind::Stereo, {{4, .6}, {3, 1}});
      break;
    case 95:
      seq(add(Kind::Volume), 7);
      curve(add(Kind::Vowel), 0, 6, 1, .9);
      add(Kind::Delay, {{0, .75}, {1, .45}}, .3);
      break;
    case 96:
      add(Kind::Granular, {{1, 70}, {2, 35}, {3, 12}, {6, .8}}, .7);
      add(Kind::Resonator, {{0, 67}, {3, 4}, {1, 1.5}}, .4);
      add(Kind::Reverb, {{0, 6}, {4, 4}}, .45);
      s.studio = true;
      break;
    case 97:
      add(Kind::Time, {{2, 3}, {3, 2}}, .7);
      curve(add(Kind::Tape), 0, 7, 1, .7);
      add(Kind::Crush, {{0, 6}, {1, 4400}, {2, .3}}, .5);
      s.studio = true;
      break;
    case 98:
      curve(add(Kind::Spectral, {{1, .6}}), 0, 4, 2, .95);
      curve(add(Kind::Pitch), 2, 3, 4, .55);
      curve(add(Kind::Stereo), 0, 3, 8, .8);
      s.studio = true;
      break;
    case 99:
      curve(add(Kind::Volume), 0, 0, .25, .8);
      seq(add(Kind::Repeat, {{0, .125}, {1, 1}}, 0), 9);
      add(Kind::Granular, {{1, 100}, {2, 40}}, 0);
      add(Kind::Reverb, {{0, 5}}, .15);
      s.studio = true;
      break;
    default:
      break;
    }
  // Four repeatable, centred macro assignments. No MIDI or sidechain is
  // required.
  s.globals[Mix] = .85;
  route(0, Mix, .3);
  const auto first = s.modules[0].kind;
  auto f = fields(first);
  route(1, commonParam(0, DepthA), .5);
  // Movement remains useful on static programs by providing a subtle Motion B.
  if (s.scenes[0].modules[0].curves[0].depth == 0)
    curve(0, 0, 3, 2, 0);
  route(2,
        effectParam(
            0, first,
            std::min<unsigned>(f.size() - 1, first == Kind::Volume ? 1 : 0)),
        .2);
  unsigned space = count;
  for (unsigned i = 0; i < count; ++i)
    if (s.modules[i].kind == Kind::Reverb || s.modules[i].kind == Kind::Delay)
      space = i;
  if (space == count)
    space = add(Kind::Reverb, {{0, 1.5}, {2, .55}}, .06, Lane::Post);
  route(3, commonParam(space, SlotMix), .4);
  s.scenes[0].name = "Main";
  if (index == 99) {
    ensureScene(s, 3);
    const char *names[]{"Pulse", "Chops", "Grains", "Space"};
    for (unsigned i = 0; i < 4; ++i) {
      auto &scene = s.scenes[i];
      scene.name = names[i];
      scene.modules[0].curves[0].depth = i == 0 ? .8 : 0;
      scene.modules[1].mix = i == 1 ? .9 : 0;
      scene.modules[2].mix = i == 2 ? .85 : 0;
      scene.modules[3].mix = i == 3 ? .75 : .15;
    }
  }
  return s;
}
} // namespace daw::plugins::flux
