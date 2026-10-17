#include "Internal/FluxInstance.hpp"
#include "Internal/FluxDsp.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace daw::plugins::flux {
namespace {
using dsp::Stereo;
static_assert(std::atomic<double>::is_always_lock_free &&
              std::atomic<float>::is_always_lock_free);
double frac(double x) noexcept { return x - std::floor(x); }
Stereo blend(Stereo a, Stereo b, double t) noexcept {
  return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t};
}
int freezeField(Kind k) noexcept {
  return k == Kind::Granular   ? 7
         : k == Kind::Spectral ? 0
         : k == Kind::Reverb   ? 6
                               : -1;
}
struct SlotKernel {
  dsp::Processor processor;
  dsp::History dry;
  std::span<const Field> definitions;
  unsigned latency = 0;
  double outputDb = 0, outputGain = 1;
  SlotKernel(Kind k, double r, bool studio, unsigned seed)
      : processor(k, r, studio, seed), definitions(fields(k)),
        latency(processor.latency()) {
    dry.prepare(latency + 8);
  }
  void reset() noexcept {
    processor.reset();
    dry.reset();
  }
};
struct RoutingKernel {
  dsp::Crossover wet, dry;
  std::array<dsp::History, 3> align;
  dsp::History totalDry;
  std::array<unsigned, 3> padding{};
  double lowSplit = -1, highSplit = -1;
  unsigned latency = 0;
  void reset() noexcept {
    wet.reset();
    dry.reset();
    totalDry.reset();
    for (auto &h : align)
      h.reset();
  }
};
// Ownership follows the SPSC triple-buffer handshake. Destructors, allocation
// and preparation belong exclusively to the producer/control thread.
struct FluxProgram {
  State state;
  std::array<std::shared_ptr<SlotKernel>, slotCount> kernels;
  std::vector<unsigned> active;
  std::array<std::vector<unsigned>, 5> lanes;
  std::shared_ptr<RoutingKernel> routing;
  unsigned revision = 0;
  bool restart = false;
};
void copySceneValues(const State &state, unsigned scene,
                     std::array<double, parameterCount> &v) noexcept {
  const auto &sc = state.scenes[scene < state.scenes.size() ? scene : 0];
  for (unsigned j = 0; j < 4; ++j)
    v[Macro1 + j] = sc.macros[j];
  for (unsigned i = 0; i < slotCount; ++i) {
    const auto &ss = sc.modules[i];
    v[commonParam(i, Enabled)] = ss.enabled;
    v[commonParam(i, SlotMix)] = ss.mix;
    v[commonParam(i, SlotOutput)] = ss.output;
    v[commonParam(i, Solo)] = ss.solo;
    for (unsigned c = 0; c < 2; ++c) {
      v[commonParam(i, DepthA + c)] = ss.curves[c].depth;
      v[commonParam(i, RateA + c)] =
          ss.curves[c].sync ? ss.curves[c].bars : ss.curves[c].hz;
    }
    if (state.modules[i].kind != Kind::Empty)
      for (unsigned p = 0; p < fields(state.modules[i].kind).size(); ++p)
        v[effectParam(i, state.modules[i].kind, p)] = ss.values[p];
  }
}
} // namespace
struct FluxInstance::Impl {
  std::array<std::atomic<double>, parameterCount> values{};
  std::array<double, parameterCount> targets{};
  State controlState = initialState();
  std::array<FluxProgram, 3> programs;
  std::atomic<unsigned> middle{1}, reportedLatency{0}, revision{0},
      liveRevision{0}, midiRevision{0};
  std::atomic<double> elapsedBeats{0};
  unsigned front = 0, back = 2;
  std::array<std::shared_ptr<SlotKernel>, slotCount> controlKernels;
  std::shared_ptr<RoutingKernel> controlRouting;
  double rate = 48000, preparedRate = 0;
  double activityStep = 1 - dsp::pole(.002, 48000),
         parameterStep = 1 - dsp::pole(.0035, 48000),
         volumeStep = 1 - dsp::pole(.00008, 48000);
  bool preparedStudio = false;
  bool modulationDirty = true;
  unsigned observedParameterRevision = 0;
  bool parametersDirty = true;
  std::array<std::array<double, 2>, slotCount> triggerBeat{}, triggerSample{},
      curvePhase{};
  std::array<std::array<bool, 2>, slotCount> triggerActive{};
  std::array<dsp::Values, slotCount> effective{}, smoothed{};
  std::array<dsp::Values, slotCount> normalizedTargets{};
  std::array<double, slotCount> activity{};
  std::array<double, slotCount> sequenceStrength{};
  std::array<bool, slotCount> sequenceTrigger{}, sequenceOrigin{},
      slotTriggered{}, freezeSuppressed{};
  std::array<bool, parameterCount> timestamped{};
  std::array<std::int64_t, slotCount> previousStep{};
  std::array<double, 37> sources{};
  std::array<double, GlobalCount> globals{};
  std::array<std::array<double, CommonCount>, slotCount> common{};
  std::array<double, 2> xy{.5, .5};
  std::array<double, 4> modSmooth{}, modFollower{};
  std::array<double, 4> modAttack{}, modRelease{}, modPole{};
  double xyPole = 0, detectorAttack = 0, detectorRelease = 0,
         detectorThreshold = 0, detectorRearm = 0;
  std::array<std::array<dsp::Biquad, 2>, 2> detectorFilters;
  double detectorLow = -1, detectorHigh = -1;
  std::array<double, 2> detectorEnvelope{}, detectorHold{};
  std::array<bool, 2> armed{true, true};
  std::array<bool, 2048> notes{};
  dsp::Clock clock;
  double velocity = 0, freeBeat = 0, expectedBeat = 0, queueBeat = 0,
         fillRemaining = -1, transition = 1, continuousBeats = 0;
  double inputEnv = 0, sideEnv = 0, corLL = 0, corRR = 0, corLR = 0,
         globalMix = 1, outGain = 1, inGain = 1;
  unsigned playingScene = 0, queuedScene = sceneCount, returnScene = 0,
           controlTick = 0;
  int lastNote = -1;
  bool hadBlock = false, held = false, needsStart = true;
  std::int64_t expectedSample = 0, sampleCounter = 0;
  Stereo lastOutput{}, transitionFrom{};
  // Scalars and histories are independent relaxed atomics. A torn visual
  // frame is harmless; there is no seqlock retry loop on either thread.
  std::array<std::atomic<float>, 1100> meters{};
  std::atomic<int> lastCC{-1}, lastChannel{0};
  std::atomic<unsigned> parameterRevision{0};
  unsigned wavePosition = 0, waveTick = 0;
  Impl() {
    (void)dsp::sine(0);
    auto table = parameterTable();
    for (unsigned i = 0; i < parameterCount; ++i) {
      targets[i] = table[i].defaultValue;
      values[i].store(targets[i]);
    }
    for (auto &p : programs)
      p.state = controlState;
    previousStep.fill(std::numeric_limits<std::int64_t>::min());
  }
  bool publish(const State &s, bool force = false) {
    if (!validState(s))
      return false;
    try {
      FluxProgram next;
      next.state = s;
      bool structural =
          force || preparedRate != rate || s.studio != preparedStudio ||
          s.routing != controlState.routing || s.order != controlState.order;
      bool newPreset = s.presetId != controlState.presetId;
      next.restart = force || newPreset;
      for (unsigned i = 0; i < slotCount; ++i) {
        structural |= s.modules[i].kind != controlState.modules[i].kind ||
                      s.modules[i].lane != controlState.modules[i].lane;
        if (s.modules[i].kind == Kind::Empty)
          continue;
        if (!force && !newPreset && preparedRate == rate &&
            preparedStudio == s.studio && controlKernels[i] &&
            controlKernels[i]->processor.kind() == s.modules[i].kind)
          next.kernels[i] = controlKernels[i];
        else {
          next.kernels[i] = std::make_shared<SlotKernel>(
              s.modules[i].kind, rate, s.studio, s.seed + i);
          structural = true;
        }
      }
      // Topology is immutable on the audio thread. Visit only populated slots
      // in each lane instead of rescanning all twelve slots for every sample.
      for (unsigned i = 0; i < slotCount; ++i)
        if (next.kernels[i])
          next.active.push_back(i);
      for (unsigned i : s.order)
        if (next.kernels[i]) {
          auto lane = s.modules[i].lane;
          if (s.routing == Routing::Serial && lane != Lane::Pre &&
              lane != Lane::Post)
            lane = Lane::A;
          next.lanes[unsigned(lane)].push_back(i);
        }
      if (structural || !controlRouting) {
        auto r = std::make_shared<RoutingKernel>();
        std::array<unsigned, 3> branches{};
        unsigned pre = 0, post = 0;
        for (unsigned i : s.order)
          if (next.kernels[i]) {
            unsigned latency = next.kernels[i]->processor.latency();
            auto lane = s.modules[i].lane;
            if (lane == Lane::Pre)
              pre += latency;
            else if (lane == Lane::Post)
              post += latency;
            else
              branches[s.routing == Routing::Serial
                           ? 0
                           : std::min(2u, unsigned(lane) - 1)] += latency;
          }
        unsigned branchLatency =
            *std::max_element(branches.begin(), branches.end());
        r->latency = pre + branchLatency + post;
        for (unsigned b = 0; b < 3; ++b) {
          r->padding[b] = branchLatency - branches[b];
          r->align[b].prepare(r->padding[b] + 8);
        }
        r->totalDry.prepare(r->latency + 8);
        r->wet.tune(s.globals[LowSplit], s.globals[HighSplit], rate);
        r->dry.tune(s.globals[LowSplit], s.globals[HighSplit], rate);
        next.routing = std::move(r);
      } else
        next.routing = controlRouting;
      next.revision = revision.load() + 1;
      // Build succeeds before changing any observable state.
      std::array<double, parameterCount> updated{};
      for (unsigned i = 0; i < parameterCount; ++i)
        updated[i] = values[i].load(std::memory_order_relaxed);
      for (unsigned i = 0; i < GlobalCount; ++i)
        updated[i] = s.globals[i];
      copySceneValues(s, unsigned(s.globals[SceneSelect]), updated);
      for (unsigned i = 0; i < GlobalCount; ++i)
        updated[i] = s.globals[i];
      controlState = s;
      controlKernels = next.kernels;
      controlRouting = next.routing;
      preparedRate = rate;
      preparedStudio = s.studio;
      for (unsigned i = 0; i < parameterCount; ++i)
        values[i].store(updated[i], std::memory_order_relaxed);
      reportedLatency.store(next.routing->latency, std::memory_order_release);
      programs[back] = std::move(next);
      revision.store(programs[back].revision, std::memory_order_release);
      back = middle.exchange(back | 4u, std::memory_order_acq_rel) & 3u;
      return true;
    } catch (...) {
      return false;
    }
  }
  const FluxProgram &program() const noexcept { return programs[front]; }
  const Scene &scene() const noexcept {
    return program().state.scenes[playingScene < program().state.scenes.size()
                                      ? playingScene
                                      : 0];
  }
  void updateCoefficients() noexcept {
    modulationDirty = true;
    parametersDirty = true;
    const auto &s = program().state;
    const auto &d = s.detector;
    detectorAttack = dsp::pole(d.attack, rate);
    detectorRelease = dsp::pole(d.release, rate);
    detectorThreshold = dsp::gain(d.threshold);
    detectorRearm = dsp::gain(d.threshold - d.hysteresis);
    xyPole = dsp::pole(s.pathSmooth, rate);
    for (unsigned i = 0; i < 4; ++i) {
      const auto &mod = scene().modulators[i];
      modAttack[i] = dsp::pole(mod.attack, rate);
      modRelease[i] = dsp::pole(mod.release, rate);
      modPole[i] = dsp::pole(mod.smooth, rate);
    }
  }
  void adopt() noexcept {
    if (!(middle.load(std::memory_order_acquire) & 4u))
      return;
    front = middle.exchange(front, std::memory_order_acq_rel) & 3u;
    const auto &s = program().state;
    for (unsigned i = 0; i < parameterCount; ++i)
      targets[i] = values[i].load(std::memory_order_relaxed);
    playingScene =
        std::min<unsigned>(unsigned(s.globals[SceneSelect]), sceneCount - 1);
    queuedScene = sceneCount;
    copySceneValues(s, playingScene, targets);
    for (unsigned i = 0; i < GlobalCount; ++i)
      targets[i] = s.globals[i];
    for (unsigned i = 0; i < slotCount; ++i)
      smoothed[i] = scene().modules[i].values;
    updateCoefficients();
    transitionFrom = lastOutput;
    transition = 0;
    controlTick = 0;
    needsStart |= program().restart;
    liveRevision.store(program().revision, std::memory_order_release);
  }
  void reset() noexcept {
    adopt();
    for (auto &k : program().kernels)
      if (k)
        k->reset();
    if (program().routing)
      program().routing->reset();
    for (auto &f : detectorFilters)
      for (auto &b : f)
        b.reset();
    detectorEnvelope = {};
    detectorLow = detectorHigh = -1;
    detectorHold = {};
    armed = {true, true};
    notes = {};
    triggerBeat = {};
    triggerSample = {};
    curvePhase = {};
    modSmooth = {};
    modFollower = {};
    sources = {};
    xy = {targets[X], targets[Y]};
    activity = {};
    sequenceOrigin = {};
    slotTriggered = {};
    freezeSuppressed = {};
    previousStep.fill(std::numeric_limits<std::int64_t>::min());
    held = false;
    lastNote = -1;
    velocity = inputEnv = sideEnv = 0;
    sampleCounter = 0;
    hadBlock = false;
    transition = 0;
    transitionFrom = lastOutput = {};
    globalMix = targets[Mix];
    inGain = dsp::gain(targets[Input]);
    outGain = dsp::gain(targets[Output]);
    queuedScene = sceneCount;
    fillRemaining = -1;
    needsStart = true;
    continuousBeats = 0;
  }
  void startCurves() noexcept {
    const auto &sc = scene();
    for (unsigned s = 0; s < slotCount; ++s)
      for (unsigned c = 0; c < 2; ++c) {
        triggerBeat[s][c] = clock.beat;
        triggerSample[s][c] = double(sampleCounter);
        triggerActive[s][c] =
            sc.modules[s].curves[c].trigger == TriggerMode::Host ||
            (held && sc.modules[s].curves[c].trigger == TriggerMode::Midi);
      }
    needsStart = false;
  }
  void requestScene(unsigned scene, double beat) noexcept {
    scene = std::min<unsigned>(scene, sceneCount - 1);
    if (scene == playingScene && queuedScene == sceneCount)
      return;
    queuedScene = scene;
    double grid = targets[Quantize] < .5    ? 0
                  : targets[Quantize] < 1.5 ? 1
                                            : clock.barBeats;
    queueBeat = grid > 0 ? std::ceil((beat - 1e-8) / grid) * grid : beat;
  }
  void selectScene(unsigned scene) noexcept {
    auto previous = playingScene;
    playingScene = scene;
    updateCoefficients();
    queuedScene = sceneCount;
    freezeSuppressed = {};
    sequenceOrigin = {};
    copySceneValues(program().state, scene, targets);
    targets[SceneSelect] = scene;
    const auto &s = program().state;
    for (unsigned j = 0; j < 4; ++j)
      values[Macro1 + j].store(targets[Macro1 + j], std::memory_order_relaxed);
    for (unsigned i = 0; i < slotCount; ++i) {
      for (unsigned c = 0; c < CommonCount; ++c)
        values[commonParam(i, c)].store(targets[commonParam(i, c)],
                                        std::memory_order_relaxed);
      if (s.modules[i].kind != Kind::Empty)
        for (unsigned p = 0; p < fields(s.modules[i].kind).size(); ++p)
          values[effectParam(i, s.modules[i].kind, p)].store(
              targets[effectParam(i, s.modules[i].kind, p)],
              std::memory_order_relaxed);
    }
    values[SceneSelect].store(scene, std::memory_order_relaxed);
    parameterRevision.fetch_add(1, std::memory_order_release);
    transition = 0;
    transitionFrom = lastOutput;
    startCurves();
    if (s.sceneMode == 2 && fillRemaining < 0) {
      returnScene = previous;
      fillRemaining = clock.barBeats;
    }
  }
  void trigger(TriggerMode type) noexcept {
    const auto &sc = scene();
    for (unsigned s = 0; s < slotCount; ++s)
      for (unsigned c = 0; c < 2; ++c)
        if (sc.modules[s].curves[c].trigger == type) {
          triggerBeat[s][c] = clock.beat;
          triggerSample[s][c] = double(sampleCounter);
          triggerActive[s][c] = true;
          slotTriggered[s] = true;
          freezeSuppressed[s] = false;
        }
  }
  void parameter(unsigned index, double value) noexcept {
    auto table = parameterTable();
    if (index >= table.size() || !std::isfinite(value))
      return;
    const auto &info = table[index];
    value = std::clamp(value, info.minValue, info.maxValue);
    if (info.isStepped)
      value = std::round(value);
    double previous = targets[index];
    targets[index] = value;
    modulationDirty |= value != previous;
    bool changed =
        values[index].exchange(value, std::memory_order_relaxed) != value;
    if (index == SceneSelect && value != previous)
      requestScene(unsigned(value), clock.beat);
    if (index == ManualTrigger && value > .5 && previous < .5) {
      trigger(TriggerMode::Manual);
      clock.retrigger = true;
    }
    if (index >= GlobalCount) {
      unsigned slot = (index - GlobalCount) / bankSize;
      auto kind = program().state.modules[slot].kind;
      int field = freezeField(kind);
      if (field >= 0 && index == effectParam(slot, kind, unsigned(field)) &&
          value > 0)
        freezeSuppressed[slot] = false;
    }
    if (index == Panic && value > .5 && previous < .5) {
      for (unsigned s = 0; s < slotCount; ++s)
        if (program().kernels[s]) {
          program().kernels[s]->reset();
          freezeSuppressed[s] = true;
          int field = freezeField(program().state.modules[s].kind);
          if (field >= 0) {
            auto id = effectParam(s, program().state.modules[s].kind,
                                  unsigned(field));
            targets[id] = 0;
            changed |= values[id].exchange(0, std::memory_order_relaxed) != 0;
          }
        }
      if (program().routing)
        program().routing->reset();
      transition = 0;
      transitionFrom = lastOutput;
    }
    if (changed)
      parameterRevision.fetch_add(1, std::memory_order_release);
  }
  void event(const PluginEvent &e) noexcept {
    const auto &s = program().state;
    switch (e.kind) {
    case PluginEvent::Kind::ParamValue:
      parameter(e.paramIndex, e.value);
      break;
    case PluginEvent::Kind::NoteOn:
      if (e.key < 0 || e.key > 127 || e.channel < 0 || e.channel > 15)
        break;
      notes[unsigned(e.channel) * 128 + e.key] = true;
      held = true;
      lastNote = e.key;
      velocity = std::clamp(e.value, 0., 1.);
      trigger(TriggerMode::Midi);
      clock.retrigger = true;
      if (s.midiScenes && unsigned(e.channel) == s.midiSceneChannel &&
          e.key >= 36 && e.key < 52) {
        returnScene = playingScene;
        requestScene(unsigned(e.key - 36), clock.beat);
      }
      break;
    case PluginEvent::Kind::NoteOff:
    case PluginEvent::Kind::NoteChoke:
      if (e.key < 0 || e.key > 127 || e.channel < 0 || e.channel > 15)
        break;
      notes[unsigned(e.channel) * 128 + e.key] = false;
      held = std::any_of(notes.begin(), notes.end(), [](bool v) { return v; });
      if (!held)
        lastNote = -1;
      if (s.midiScenes && s.sceneMode == 1 &&
          unsigned(e.channel) == s.midiSceneChannel && e.key >= 36 &&
          e.key < 52)
        requestScene(returnScene, clock.beat);
      break;
    case PluginEvent::Kind::MidiController:
      lastCC.store(int(e.paramIndex));
      lastChannel.store(e.channel);
      midiRevision.fetch_add(1, std::memory_order_release);
      for (const auto &mapping : s.midi)
        if (mapping.controller == e.paramIndex &&
            (!mapping.channel || mapping.channel == unsigned(e.channel + 1))) {
          const auto &p = parameterTable()[mapping.parameter];
          parameter(mapping.parameter, std::lerp(p.minValue, p.maxValue,
                                                 std::clamp(e.value, 0., 1.)));
        }
      break;
    default:
      break;
    }
  }
  void detect(Stereo in, Stereo sc) noexcept {
    const auto &d = program().state.detector;
    const double attack = detectorAttack, release = detectorRelease;
    double peak = std::max(std::abs(in[0]), std::abs(in[1])),
           side = std::max(std::abs(sc[0]), std::abs(sc[1]));
    inputEnv = peak + (inputEnv - peak) * (peak > inputEnv ? attack : release);
    sideEnv = side + (sideEnv - side) * (side > sideEnv ? attack : release);
    for (unsigned b = 0; b < 2; ++b) {
      auto sig = b ? sc : in;
      double v = std::abs(detectorFilters[b][1].tick(
                     detectorFilters[b][0].tick((sig[0] + sig[1]) * .5))) *
                 d.sensitivity;
      auto &env = detectorEnvelope[b];
      env = v + (env - v) * (v > env ? attack : release);
      detectorHold[b] = std::max(0., detectorHold[b] - 1 / rate);
      if (env < detectorRearm)
        armed[b] = true;
      if (armed[b] && detectorHold[b] == 0 && env > detectorThreshold) {
        armed[b] = false;
        detectorHold[b] = d.holdoff;
        trigger(b ? TriggerMode::Sidechain : TriggerMode::Audio);
      }
    }
  }
  double sequence(unsigned slot, bool &retrigger) noexcept {
    const auto &q = scene().modules[slot].sequence;
    if (!q.enabled) {
      sequenceOrigin[slot] = false;
      return 1;
    }
    double position = clock.beat / q.division;
    double pair = std::floor(position / 2), within = position - pair * 2;
    double swung = within < 1 + q.swing
                       ? within / (1 + q.swing)
                       : 1 + (within - 1 - q.swing) / (1 - q.swing);
    auto absolute = std::int64_t(pair * 2 + std::floor(swung));
    auto positive = [&](std::int64_t v, unsigned n) {
      return unsigned((v % n + n) % n);
    };
    unsigned step = positive(absolute, q.length);
    if (q.direction == Direction::Reverse)
      step = q.length - 1 - step;
    else if (q.direction == Direction::PingPong && q.length > 1) {
      unsigned n = positive(absolute, q.length * 2 - 2);
      step = n < q.length ? n : q.length * 2 - 2 - n;
    } else if (q.direction == Direction::Random)
      step = std::min(q.length - 1, unsigned(randomAt(program().state.seed,
                                                      absolute, slot + 88) *
                                             q.length));
    const auto &st = q.steps[step];
    double sub = frac(swung) * st.ratchet;
    bool on = st.enabled &&
              randomAt(program().state.seed, absolute, slot) < st.probability;
    auto key = absolute * 8 + std::int64_t(sub);
    retrigger =
        on && key != previousStep[slot] && (st.retrigger || st.ratchet > 1);
    previousStep[slot] = key;
    meters[40 + slot].store(float(step), std::memory_order_relaxed);
    return on && (st.ratchet == 1 || frac(sub) < .65) ? st.amount : 0;
  }
  void modulation() noexcept {
    const auto &s = program().state;
    const auto &sc = scene();
    for (unsigned i = 0; i < 4; ++i)
      sources[i] = targets[Macro1 + i];
    sources[4] = targets[X];
    sources[5] = targets[Y];
    if (s.pathEnabled && s.path.size() > 1) {
      double t = frac(clock.beat / (clock.barBeats * s.pathBars));
      if (s.pathReverse)
        t = 1 - t;
      auto next = std::upper_bound(
          s.path.begin(), s.path.end(), t,
          [](double v, const PathPoint &p) { return v < p.time; });
      auto &b = next == s.path.end() ? s.path.back() : *next;
      auto &a = next == s.path.begin() ? s.path.front() : *(next - 1);
      double u = b.time > a.time ? (t - a.time) / (b.time - a.time) : 0;
      sources[4] = std::lerp(a.x, b.x, u);
      sources[5] = std::lerp(a.y, b.y, u);
    }
    sources[10] = velocity;
    sources[11] = std::clamp(inputEnv * 2, 0., 1.);
    sources[12] = std::clamp(sideEnv * 2, 0., 1.);
    for (unsigned i = 0; i < 4; ++i) {
      const auto &mod = sc.modulators[i];
      double at = (mod.sync ? clock.beat : double(sampleCounter) / rate) *
                      mod.rate +
                  mod.phase,
             t = frac(at), v = 0;
      if (mod.type == 0) {
        switch (mod.shape) {
        case 1:
          v = t;
          break;
        case 2:
          v = 1 - std::abs(t * 2 - 1);
          break;
        case 3:
          v = t < .5 ? 1 : 0;
          break;
        default:
          v = .5 - .5 * dsp::sine(t + .25);
          break;
        }
      } else if (mod.type == 1)
        v = mod.steps[std::min(15u, unsigned(t * 16))];
      else if (mod.type == 2)
        v = randomAt(s.seed, std::int64_t(std::floor(at)), i + 80);
      else {
        double input = mod.sidechain ? sideEnv : inputEnv;
        double p = input > modFollower[i] ? modAttack[i] : modRelease[i];
        modFollower[i] = input + (modFollower[i] - input) * p;
        v = std::clamp(modFollower[i] * 2, 0., 1.);
      }
      double p = modPole[i];
      modSmooth[i] = v + (modSmooth[i] - v) * p;
      sources[6 + i] = modSmooth[i];
    }
    for (unsigned c = 0; c < 2; ++c) {
      xy[c] = sources[4 + c] + (xy[c] - sources[4 + c]) * xyPole;
      sources[4 + c] = xy[c];
    }
    // Unrouted controls change only on a parameter event or scene/program
    // adoption. Keep sample-accurate modulation without recopying idle banks.
    const bool routed = !sc.routes.empty();
    if (modulationDirty || routed) {
      for (unsigned i = 0; i < GlobalCount; ++i)
        globals[i] = targets[i];
      for (unsigned slot : program().active)
        for (unsigned c = 0; c < CommonCount; ++c)
          common[slot][c] = targets[commonParam(slot, c)];
    }
    for (unsigned stage = 0; stage < 2; ++stage) {
      for (const auto &route : sc.routes)
        if (route.enabled) {
          double source = sources[route.source];
          if (route.bipolar)
            source = source * 2 - 1;
          const auto &info = parameterTable()[route.target];
          double *destination = nullptr;
          if (route.target < GlobalCount) {
            if (stage == 0)
              destination = &globals[route.target];
          } else if (stage == 1) {
            unsigned slot = (route.target - GlobalCount) / bankSize,
                     local = (route.target - GlobalCount) % bankSize;
            if (local < CommonCount)
              destination = &common[slot][local];
          }
          if (destination) {
            double norm = (*destination - info.minValue) /
                          (info.maxValue - info.minValue);
            *destination = std::lerp(info.minValue, info.maxValue,
                                     std::clamp(norm + source * route.amount,
                                                route.low, route.high));
            if (info.isStepped)
              *destination = std::round(*destination);
          }
        }
      if (stage == 0) {
        for (unsigned i = 0; i < 4; ++i)
          sources[i] = globals[Macro1 + i];
        for (unsigned i = 0; i < 2; ++i)
          sources[4 + i] = std::clamp(
              sources[4 + i] + globals[X + i] - targets[X + i], 0., 1.);
      }
    }
    for (unsigned slot : program().active) {
      const auto &ss = sc.modules[slot];
      const auto defs = program().kernels[slot]->definitions;
      if (modulationDirty)
        for (unsigned f = 0; f < defs.size(); ++f)
          normalizedTargets[slot][f] = normalized(
              defs[f], targets[effectParam(slot, s.modules[slot].kind, f)]);
      if (modulationDirty || routed || common[slot][DepthA] != 0 ||
          common[slot][DepthB] != 0)
        for (unsigned p = 0; p < defs.size(); ++p)
          effective[slot][p] =
              targets[effectParam(slot, s.modules[slot].kind, p)];
      for (unsigned c = 0; c < 2; ++c) {
        const auto &curve = ss.curves[c];
        double length =
            std::clamp(common[slot][RateA + c], curve.sync ? .0078125 : .01,
                       curve.sync ? 32. : 40.);
        bool host = curve.trigger == TriggerMode::Host &&
                    curve.play == PlayMode::Loop && !sequenceOrigin[slot];
        double elapsed =
            curve.sync
                ? (clock.beat - (host ? 0 : triggerBeat[slot][c])) /
                      ((length < 1 ? 4 : clock.barBeats) * length)
                : (sampleCounter - (host ? 0 : triggerSample[slot][c])) /
                      rate * length;
        double at = curve.play == PlayMode::Loop
                        ? frac(elapsed + curve.phase)
                        : std::clamp(elapsed + curve.phase, 0., 1.);
        if (!triggerActive[slot][c] ||
            (curve.play == PlayMode::Held && !held))
          at = 1;
        curvePhase[slot][c] = at;
        double v = curve.points.size() == 2 &&
                           curve.points[0].y == curve.points[1].y
                       ? curve.points[0].y : evaluate(curve, at);
        sources[13 + slot * 2 + c] = v;
        if (curve.target < defs.size() && common[slot][DepthA + c] != 0) {
          double depth = common[slot][DepthA + c];
          // The second curve can follow the first on the same parameter.
          // Otherwise its base is the unchanged host value, whose logarithm
          // was already calculated when that value arrived.
          const bool followsFirst = c == 1 && common[slot][DepthA] != 0 &&
                                    ss.curves[0].target == curve.target;
          double base = followsFirst
              ? normalized(defs[curve.target], effective[slot][curve.target])
              : normalizedTargets[slot][curve.target];
          effective[slot][curve.target] =
              plain(defs[curve.target],
                    curve.bipolar ? base + depth * (v - .5)
                                  : std::lerp(base, depth < 0 ? 1 - v : v,
                                              std::abs(depth)));
        }
      }
    }
    for (const auto &route : sc.routes)
      if (route.enabled && route.target >= GlobalCount) {
        unsigned slot = (route.target - GlobalCount) / bankSize,
                 local = (route.target - GlobalCount) % bankSize;
        if (local < CommonCount)
          continue;
        auto kind = Kind(1 + (local - CommonCount) / fieldCount);
        unsigned field = (local - CommonCount) % fieldCount;
        if (s.modules[slot].kind != kind)
          continue;
        auto defs = fields(kind);
        if (field >= defs.size())
          continue;
        double v = sources[route.source];
        if (route.bipolar)
          v = v * 2 - 1;
        effective[slot][field] =
            plain(defs[field],
                  std::clamp(normalized(defs[field], effective[slot][field]) +
                                 v * route.amount,
                             route.low, route.high));
      }
    modulationDirty = false;
  }
  Stereo slot(unsigned index, Stereo x, Stereo sidechain,
              bool anySolo) noexcept {
    const auto &p = program();
    auto &kernel = *p.kernels[index];
    auto kind = p.state.modules[index].kind;
    const auto defs = kernel.definitions;
    double strength = sequenceStrength[index];
    auto processorClock = clock;
    processorClock.retrigger |= sequenceTrigger[index] || slotTriggered[index];
    double amount = common[index][SlotMix], level = common[index][SlotOutput];
    if (kind != Kind::Volume)
      amount *= strength;
    double enabled =
        common[index][Enabled] > .5 && (!anySolo || common[index][Solo] > .5)
            ? amount
            : 0;
    activity[index] += (enabled - activity[index]) * activityStep;
    int freeze = freezeField(kind);
    const double smoothStep = kind == Kind::Volume ? volumeStep : parameterStep;
    for (unsigned f = 0; f < defs.size(); ++f) {
      double v = effective[index][f];
      if (kind == Kind::Volume && f == 0)
        v *= strength;
      if (int(f) == freeze && freezeSuppressed[index])
        v = 0;
      if (defs[f].stepped)
        smoothed[index][f] = v;
      else
        smoothed[index][f] += (v - smoothed[index][f]) * smoothStep;
    }
    auto wet =
        kernel.processor.process(x, sidechain, smoothed[index], processorClock);
    // History stores floats. Preserve that rounding even when no delay line
    // is needed, so zero-latency wet/dry mixing stays numerically identical.
    Stereo dry{float(x[0]), float(x[1])};
    if (kernel.latency) {
      kernel.dry.push(x);
      dry = kernel.dry.ago(kernel.latency);
    }
    if (level != kernel.outputDb) {
      kernel.outputDb = level;
      kernel.outputGain = dsp::gain(level);
    }
    for (auto &v : wet)
      v *= kernel.outputGain;
    return blend(dry, wet, activity[index]);
  }
  Stereo lane(Lane lane, Stereo x, Stereo sc, bool solo) noexcept {
    for (unsigned i : program().lanes[unsigned(lane)])
      x = slot(i, x, sc, solo);
    return x;
  }
  Stereo audio(Stereo x, Stereo sidechain) noexcept {
    const auto &p = program();
    if (!p.routing)
      return x;
    auto &r = *p.routing;
    detect(x, sidechain);
    for (unsigned i : p.active) {
      sequenceTrigger[i] = false;
      sequenceStrength[i] = sequence(i, sequenceTrigger[i]);
      if (sequenceTrigger[i]) {
        sequenceOrigin[i] = true;
        for (unsigned c = 0; c < 2; ++c) {
          triggerBeat[i][c] = clock.beat;
          triggerSample[i][c] = double(sampleCounter);
          triggerActive[i][c] = true;
        }
      }
    }
    modulation();
    const bool solo = [&] {
      for (unsigned i : p.active)
        if (common[i][Solo] > .5)
          return true;
      return false;
    }();
    inGain += (dsp::gain(globals[Input]) - inGain) * .002;
    outGain += (dsp::gain(globals[Output]) - outGain) * .002;
    for (auto &v : x)
      v *= inGain;
    if ((controlTick++ & 31u) == 0) {
      if (p.state.routing == Routing::Multiband &&
          (r.lowSplit != globals[LowSplit] ||
           r.highSplit != globals[HighSplit])) {
        r.lowSplit = globals[LowSplit];
        r.highSplit = globals[HighSplit];
        r.wet.tune(r.lowSplit, r.highSplit, rate);
        r.dry.tune(r.lowSplit, r.highSplit, rate);
      }
      if (detectorLow != p.state.detector.highpass ||
          detectorHigh != p.state.detector.lowpass) {
        detectorLow = p.state.detector.highpass;
        detectorHigh = p.state.detector.lowpass;
        for (auto &f : detectorFilters) {
          f[0].tune(1, detectorLow, .707, 0, rate);
          f[1].tune(0, detectorHigh, .707, 0, rate);
        }
      }
    }
    Stereo original = x;
    if (p.state.routing == Routing::Multiband) {
      auto bands = r.dry.split(x);
      original = {};
      for (auto b : bands)
        for (unsigned c = 0; c < 2; ++c)
          original[c] += b[c];
    }
    Stereo dry{float(original[0]), float(original[1])};
    if (r.latency) {
      r.totalDry.push(original);
      dry = r.totalDry.ago(r.latency);
    }
    Stereo y = lane(Lane::Pre, x, sidechain, solo);
    if (p.state.routing == Routing::Serial) {
      y = lane(Lane::A, y, sidechain, solo);
    } else {
      std::array<Stereo, 3> branch{y, y, y};
      if (p.state.routing == Routing::Multiband)
        branch = r.wet.split(y);
      bool anyBandSolo =
          globals[LowSolo] > .5 || globals[MidSolo] > .5 ||
          (p.state.routing == Routing::Multiband && globals[HighSolo] > .5);
      y = {};
      for (unsigned b = 0; b < (p.state.routing == Routing::Parallel ? 2u : 3u);
           ++b) {
        auto v = lane(Lane(unsigned(Lane::A) + b), branch[b], sidechain, solo);
        if (r.padding[b]) {
          r.align[b].push(v);
          v = r.align[b].ago(r.padding[b]);
        } else {
          v = {float(v[0]), float(v[1])};
        }
        double level = dsp::gain(globals[LowGain + b]);
        if (p.state.routing == Routing::Parallel)
          level *= b ? globals[Balance] : 1 - globals[Balance];
        if (globals[LowMute + b] > .5 ||
            (anyBandSolo && globals[LowSolo + b] < .5))
          level = 0;
        for (unsigned c = 0; c < 2; ++c)
          y[c] += v[c] * level;
      }
    }
    y = lane(Lane::Post, y, sidechain, solo);
    double mix = globals[Bypass] > .5 ? 0 : globals[Mix];
    globalMix += (mix - globalMix) * .004;
    y = blend(dry, y, globalMix);
    for (auto &v : y) {
      v *= outGain;
      if (globals[Protect] > .5 && std::abs(v) > .95)
        v = std::copysign(.95 + .05 * std::tanh((std::abs(v) - .95) / .05), v);
      v = dsp::clean(v);
    }
    transition = std::min(1., transition + 1 / (rate * .005));
    y = blend(transitionFrom, y, transition);
    lastOutput = y;
    return y;
  }
};

FluxInstance::FluxInstance() : m(std::make_unique<Impl>()) {}
FluxInstance::~FluxInstance() = default;
const PluginDescriptor &FluxInstance::staticDescriptor() {
  static const auto d = [] {
    PluginDescriptor d;
    d.format = Format::Internal;
    d.uid = d.path = "daw.flux";
    d.name = "Flux";
    d.vendor = "VLTONE";
    d.version = "1.0.0";
    d.category = "Effect|Multi-effect|Rhythmic";
    d.wantsMidi = true;
    d.stateSchemaVersion = 1;
    return d;
  }();
  return d;
}
bool FluxInstance::setBusLayout(const PluginBusLayout &wanted,
                                PluginBusLayout &accepted) {
  if (wanted.inputs.empty() || wanted.outputs.empty() || wanted.inputs[0] < 1 ||
      wanted.inputs[0] > 2 || wanted.outputs[0] < 1 || wanted.outputs[0] > 2 ||
      wanted.inputs.size() > 2 ||
      (wanted.inputs.size() > 1 && wanted.inputs[1] > 2) ||
      wanted.outputs.size() != 1)
    return false;
  m_layout = {{wanted.inputs[0], 2}, {wanted.outputs[0]}};
  if (wanted.inputs.size() > 1 && wanted.inputs[1] <= 2)
    m_layout.inputs[1] = wanted.inputs[1];
  accepted = m_layout;
  return true;
}
bool FluxInstance::activate(const PluginProcessInfo &info) {
  if (!std::isfinite(info.sampleRate) || info.sampleRate < 8000 ||
      info.sampleRate > 384000)
    return false;
  m->rate = info.sampleRate;
  m->activityStep = 1 - dsp::pole(.002, m->rate);
  m->parameterStep = 1 - dsp::pole(.0035, m->rate);
  m->volumeStep = 1 - dsp::pole(.00008, m->rate);
  if (!m->publish(m->controlState, true))
    return false;
  m->adopt();
  m->reset();
  m_active = true;
  return true;
}
std::int32_t
FluxInstance::parameterIndexForId(std::string_view id) const noexcept {
  auto p = parameterTable();
  for (unsigned i = 0; i < p.size(); ++i)
    if (p[i].id == id)
      return int(i);
  return -1;
}
double FluxInstance::parameterValue(std::uint32_t index) const noexcept {
  return index < parameterCount
             ? m->values[index].load(std::memory_order_relaxed)
             : 0;
}
std::string FluxInstance::parameterText(std::uint32_t index,
                                        double value) const {
  if (index >= parameterCount)
    return {};
  auto p = parameterTable()[index];
  char b[64];
  std::snprintf(b, sizeof(b), p.isStepped ? "%.0f" : "%.2f", value);
  return std::string(b) + (p.unit.empty() ? "" : " " + p.unit);
}
void FluxInstance::setParameterFromHost(std::uint32_t index, double value) {
  if (index >= parameterCount || !std::isfinite(value))
    return;
  const auto &p = parameterTable()[index];
  value = std::clamp(value, p.minValue, p.maxValue);
  if (p.isStepped)
    value = std::round(value);
  if (m->values[index].exchange(value, std::memory_order_relaxed) != value)
    m->parameterRevision.fetch_add(1, std::memory_order_release);
}
State FluxInstance::state() const {
  State s = m->controlState;
  for (unsigned i = 0; i < GlobalCount; ++i)
    s.globals[i] = parameterValue(i);
  // Independent automation lanes can cross; persist the same ordered split
  // that the crossover actually uses, keeping the saved program valid.
  s.globals[HighSplit] =
      std::max(s.globals[HighSplit], s.globals[LowSplit] * 1.05);
  unsigned playing = m->liveRevision.load(std::memory_order_acquire) ==
                             m->revision.load(std::memory_order_acquire)
                         ? unsigned(m->meters[12].load())
                         : unsigned(s.globals[SceneSelect]);
  playing = std::min(playing, sceneCount - 1);
  auto &sc = ensureScene(s, playing);
  for (unsigned i = 0; i < 4; ++i)
    sc.macros[i] = s.globals[Macro1 + i];
  for (unsigned i = 0; i < slotCount; ++i) {
    auto &ss = sc.modules[i];
    ss.enabled = parameterValue(commonParam(i, Enabled)) > .5;
    ss.solo = parameterValue(commonParam(i, Solo)) > .5;
    ss.mix = parameterValue(commonParam(i, SlotMix));
    ss.output = parameterValue(commonParam(i, SlotOutput));
    for (unsigned c = 0; c < 2; ++c) {
      auto &cv = ss.curves[c];
      cv.depth = parameterValue(commonParam(i, DepthA + c));
      double v = parameterValue(commonParam(i, RateA + c));
      if (cv.sync)
        cv.bars = std::clamp(v, .0078125, 32.);
      else
        cv.hz = std::clamp(v, .01, 40.);
    }
    if (s.modules[i].kind != Kind::Empty)
      for (unsigned p = 0; p < fields(s.modules[i].kind).size(); ++p)
        ss.values[p] = parameterValue(effectParam(i, s.modules[i].kind, p));
  }
  return s;
}
bool FluxInstance::setState(const State &state) {
  unsigned latency = latencySamples();
  if (!m->publish(state))
    return false;
  if (m_listener) {
    if (latency != latencySamples())
      m_listener->onLatencyChanged();
    m_listener->onStateChanged();
  }
  return true;
}
bool FluxInstance::saveState(std::vector<std::uint8_t> &out) const {
  return encodeState(state(), out);
}
bool FluxInstance::loadState(std::span<const std::uint8_t> bytes) {
  auto s = decodeState(bytes);
  return s && setState(*s);
}
void FluxInstance::setPresetReference(std::string kind, std::string name) {
  m->controlState.presetId = std::move(kind);
  m->controlState.name = std::move(name);
  m->parameterRevision.fetch_add(1, std::memory_order_release);
}
void FluxInstance::reset() noexcept { m->reset(); }
std::uint32_t FluxInstance::latencySamples() const noexcept {
  return m->reportedLatency.load(std::memory_order_acquire);
}
std::uint32_t FluxInstance::tailSamples() const noexcept {
  return unsigned(
      std::min(m->rate * 60, double(std::numeric_limits<unsigned>::max())));
}
std::size_t FluxInstance::dspMemoryBytes() const noexcept {
  std::size_t size = 0;
  for (auto &k : m->controlKernels)
    if (k)
      size += k->processor.memoryBytes() +
              k->dry.samples.size() * sizeof(std::array<float, 2>);
  if (m->controlRouting) {
    size += m->controlRouting->totalDry.samples.size() *
            sizeof(std::array<float, 2>);
    for (auto &h : m->controlRouting->align)
      size += h.samples.size() * sizeof(std::array<float, 2>);
  }
  return size;
}
FluxTelemetry FluxInstance::telemetry() const noexcept {
  FluxTelemetry t;
  auto get = [&](unsigned n) {
    return m->meters[n].load(std::memory_order_relaxed);
  };
  t.elapsedBeats = m->elapsedBeats.load(std::memory_order_relaxed);
  for (unsigned c = 0; c < 2; ++c) {
    t.input[c] = get(c);
    t.output[c] = get(2 + c);
  }
  t.envelope = get(4);
  t.sidechain = get(5);
  t.correlation = get(6);
  t.x = get(7);
  t.y = get(8);
  t.beat = get(9);
  t.playingScene = unsigned(get(12));
  t.queuedScene = unsigned(get(13));
  t.latency = latencySamples();
  t.revision = m->revision.load();
  t.parameterRevision = m->parameterRevision.load(std::memory_order_acquire);
  t.midiRevision = m->midiRevision.load(std::memory_order_acquire);
  t.lastCC = m->lastCC.load();
  t.lastChannel = m->lastChannel.load();
  t.barBeats = get(10);
  for (unsigned s = 0; s < slotCount; ++s) {
    t.fill[s] = get(20 + s);
    t.step[s] = get(40 + s);
    for (unsigned c = 0; c < 2; ++c)
      t.phases[s][c] = get(60 + s * 2 + c);
    for (unsigned f = 0; f < fieldCount; ++f)
      t.effective[s][f] = get(90 + s * fieldCount + f);
  }
  unsigned start = unsigned(get(14));
  for (unsigned c = 0; c < 3; ++c)
    for (unsigned i = 0; i < 256; ++i)
      t.waveform[c][i] = get(300 + c * 256 + (start + i) % 256);
  return t;
}
PluginProcessDisposition
FluxInstance::process(const PluginProcessContext &ctx) noexcept {
  if (!m_active || !ctx.outputs)
    return PluginProcessDisposition::Error;
  m->adopt();
  auto &m = *this->m;
  m.clock.rate = m.rate;
  m.clock.tempo = std::isfinite(ctx.transport.tempo)
                      ? std::clamp(ctx.transport.tempo, 1., 1000.)
                      : 120.;
  m.clock.barBeats = 4. * std::max(1, ctx.transport.timeSigNumerator) /
                     std::max(1, ctx.transport.timeSigDenominator);
  m.clock.seed = m.program().state.seed;
  double startBeat = ctx.playing && std::isfinite(ctx.transport.ppqPosition)
                         ? ctx.transport.ppqPosition
                         : m.freeBeat;
  bool loop = ctx.transport.looping &&
              ctx.transport.loopEndPpq > ctx.transport.loopStartPpq &&
              m.expectedBeat >= ctx.transport.loopEndPpq -
                                    2 * m.clock.tempo / (60 * m.rate) &&
              std::abs(startBeat - ctx.transport.loopStartPpq) <
                  2 * m.clock.tempo / (60 * m.rate);
  if (m.hadBlock && ctx.playing && ctx.sampleTime != m.expectedSample && !loop)
    m.reset();
  m.clock.beat = startBeat;
  if (m.needsStart)
    m.startCurves();
  const auto parameterRevision =
      m.parameterRevision.load(std::memory_order_acquire);
  if (m.parametersDirty || parameterRevision != m.observedParameterRevision) {
    m.parametersDirty = false;
    m.observedParameterRevision = parameterRevision;
    m.timestamped.fill(false);
    for (const auto &event : ctx.inputEvents)
      if (event.kind == PluginEvent::Kind::ParamValue &&
          event.paramIndex < parameterCount && event.frameOffset < ctx.frames)
        m.timestamped[event.paramIndex] = true;
    for (unsigned i = 0; i < parameterCount; ++i)
      if (!m.timestamped[i]) {
        double v = m.values[i].load(std::memory_order_relaxed);
        if (v != m.targets[i])
          m.parameter(i, v);
      }
  }
  std::size_t event = 0;
  Stereo inPeak{}, outPeak{};
  for (unsigned f = 0; f < ctx.frames; ++f) {
    m.clock.beat = startBeat + double(f) * m.clock.tempo / (60 * m.rate);
    m.clock.sample = ctx.playing ? ctx.sampleTime + f : m.sampleCounter;
    m.clock.retrigger = false;
    m.slotTriggered.fill(false);
    while (event < ctx.inputEvents.size() &&
           ctx.inputEvents[event].frameOffset <= f)
      m.event(ctx.inputEvents[event++]);
    if (m.queuedScene < sceneCount && m.clock.beat + 1e-9 >= m.queueBeat)
      m.selectScene(m.queuedScene);
    if (m.fillRemaining >= 0 && m.fillRemaining < 1e-9) {
      m.selectScene(m.returnScene);
      m.fillRemaining = -1;
    }
    m.clock.note = m.lastNote;
    m.clock.velocity = m.velocity;
    Stereo x{}, sc{};
    for (unsigned c = 0; c < 2; ++c) {
      if (ctx.inputs && ctx.inputChannels &&
          ctx.inputs[std::min<unsigned>(c, ctx.inputChannels - 1)])
        x[c] = dsp::clean(
            ctx.inputs[std::min<unsigned>(c, ctx.inputChannels - 1)][f]);
      if (ctx.sidechainInputs && ctx.sidechainInputChannels &&
          ctx.sidechainInputs[std::min<unsigned>(c, ctx.sidechainInputChannels -
                                                        1)])
        sc[c] = dsp::clean(ctx.sidechainInputs[std::min<unsigned>(
            c, ctx.sidechainInputChannels - 1)][f]);
    }
    auto y = m.audio(x, sc);
    for (unsigned c = 0; c < ctx.outputChannels; ++c)
      if (ctx.outputs[c])
        ctx.outputs[c][f] = float(ctx.outputChannels == 1 ? (y[0] + y[1]) * .5
                                                          : y[std::min(1u, c)]);
    for (unsigned c = 0; c < 2; ++c) {
      inPeak[c] = std::max(inPeak[c], std::abs(x[c]));
      outPeak[c] = std::max(outPeak[c], std::abs(y[c]));
    }
    m.corLL = m.corLL * .999 + y[0] * y[0] * .001;
    m.corRR = m.corRR * .999 + y[1] * y[1] * .001;
    m.corLR = m.corLR * .999 + y[0] * y[1] * .001;
    if (++m.waveTick >= unsigned(std::max(1., m.rate / 12000))) {
      m.waveTick = 0;
      m.meters[300 + m.wavePosition].store(float((x[0] + x[1]) * .5));
      m.meters[556 + m.wavePosition].store(float((y[0] + y[1]) * .5));
      m.meters[812 + m.wavePosition].store(float((sc[0] + sc[1]) * .5));
      m.wavePosition = (m.wavePosition + 1) % 256;
    }
    ++m.sampleCounter;
    m.continuousBeats += m.clock.tempo / (60 * m.rate);
    if (m.fillRemaining > 0)
      m.fillRemaining =
          std::max(0., m.fillRemaining - m.clock.tempo / (60 * m.rate));
  }
  for (unsigned c = 0; c < 2; ++c) {
    m.meters[c].store(float(inPeak[c]));
    m.meters[2 + c].store(float(outPeak[c]));
  }
  m.meters[4].store(float(m.inputEnv));
  m.meters[5].store(float(m.sideEnv));
  m.meters[6].store(
      float(m.corLR / std::sqrt(std::max(1e-20, m.corLL * m.corRR))));
  m.meters[7].store(float(m.sources[4]));
  m.meters[8].store(float(m.sources[5]));
  m.meters[9].store(float(m.clock.beat));
  m.meters[10].store(float(m.clock.barBeats));
  m.meters[12].store(float(m.playingScene));
  m.meters[13].store(float(m.queuedScene));
  m.meters[14].store(float(m.wavePosition));
  for (unsigned s = 0; s < slotCount; ++s) {
    if (m.program().kernels[s])
      m.meters[20 + s].store(m.program().kernels[s]->processor.fill());
    for (unsigned c = 0; c < 2; ++c)
      m.meters[60 + s * 2 + c].store(float(m.curvePhase[s][c]));
    const auto kind = m.program().state.modules[s].kind;
    for (unsigned p = 0; p < fieldCount; ++p) {
      double v = m.effective[s][p];
      if (kind == Kind::Volume && p == 0)
        v *= m.sequenceStrength[s];
      if (int(p) == freezeField(kind) && m.freezeSuppressed[s])
        v = 0;
      m.meters[90 + s * fieldCount + p].store(float(v));
    }
  }
  m.elapsedBeats.store(m.continuousBeats, std::memory_order_relaxed);
  m.expectedSample = ctx.sampleTime + ctx.frames;
  m.expectedBeat = startBeat + ctx.frames * m.clock.tempo / (60 * m.rate);
  m.freeBeat = m.expectedBeat;
  m.hadBlock = true;
  return PluginProcessDisposition::Continue;
}
} // namespace daw::plugins::flux
