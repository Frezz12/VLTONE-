#include "Internal/MorphInstance.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace daw::plugins::morph {
namespace {
constexpr double pi = modulation::dsp::pi;
// All inputs and interpolation factors here are finite and bounded. Avoid
// std::lerp's extrapolation/overflow branches in the inner audio loop.
inline double blend(double a, double b, double t) noexcept {
  return a + (b - a) * t;
}
double clean(double x) noexcept {
  return modulation::dsp::flush(
      std::clamp(modulation::dsp::clean(x), -16., 16.));
}
double cycle(double x) noexcept { return x - std::floor(x); }
// Oscillators advance by less than one cycle. Avoid a general floor operation
// for every sample; absolute host PPQ still uses cycle().
void advancePhase(double &phase, double step) noexcept {
  phase += step;
  if (phase >= 1)
    phase -= 1;
  else if (phase < 0)
    phase += 1;
}
double sine(double phase) noexcept {
  static const auto lut = [] {
    std::array<double, 2049> a{};
    for (unsigned i = 0; i < a.size(); ++i)
      a[i] = std::sin(2 * pi * i / 2048);
    return a;
  }();
  const double at = cycle(phase) * 2048;
  const auto i = unsigned(at);
  return blend(lut[i], lut[i + 1], at - i);
}
double syncBeats(double setting) noexcept {
  return std::exp2(std::clamp(std::round(setting), 1., 8.) - 4.);
}
double warm(double x) noexcept {
  x = std::clamp(x, -1., 1.);
  return x - x * x * x / 3;
}
double integral(double x) noexcept {
  const double a = std::abs(x);
  return a < 1 ? x * x * .5 - x * x * x * x / 12 : a * (2. / 3) - .25;
}
double transfer(double x, unsigned mode) noexcept {
  if (mode == 1)
    return warm(x + .25) - warm(.25);
  if (mode == 2)
    return sine(x / (2 * pi));
  return warm(x);
}
double primitive(double x, unsigned mode) noexcept {
  if (mode == 1)
    return integral(x + .25) - warm(.25) * x;
  if (mode == 2)
    return -std::cos(x);
  return integral(x);
}
constexpr std::array<double, 4> bloomSeconds{.0311, .0437, .0593, .0719};
} // namespace
const PluginDescriptor &MorphInstance::staticDescriptor() {
  static const auto descriptor = [] {
    PluginDescriptor d;
    d.format = Format::Internal;
    d.uid = d.path = "daw.morph";
    d.name = "Morph";
    d.vendor = "VLTONE";
    d.version = "1.0";
    d.category = "Effect|Modulation";
    d.stateSchemaVersion = 1;
    return d;
  }();
  return descriptor;
}
MorphInstance::MorphInstance() {
  (void)sine(0);
  m_state.values = defaults();
  makeOrbit(m_state.canvas);
  for (unsigned i = 0; i < Count; ++i)
    m_values[i].store(m_state.values[i], std::memory_order_relaxed);
  for (auto &p : m_programs)
    p = m_state.canvas;
  m_targets = m_current = m_state.values;
}
bool MorphInstance::setBusLayout(const PluginBusLayout &wanted,
                                 PluginBusLayout &accepted) {
  if (m_active || wanted.inputs.empty() || wanted.inputs.size() > 2 ||
      wanted.outputs.size() != 1 || wanted.inputs[0] < 1 ||
      wanted.inputs[0] > 2 || wanted.outputs[0] < 1 || wanted.outputs[0] > 2)
    return false;
  if (wanted.inputs.size() == 2 && wanted.inputs[1] > 2)
    return false;
  accepted = m_layout = wanted;
  return true;
}
void MorphInstance::Core::prepare(double sr) {
  for (auto &d : chorus)
    d.prepare(sr, .085);
  for (auto &d : flanger)
    d.prepare(sr, .03);
  for (auto &d : doubler)
    d.prepare(sr, .095);
  reset();
}
void MorphInstance::Core::reset() noexcept {
  for (auto &d : chorus)
    d.reset();
  for (auto &d : flanger)
    d.reset();
  for (auto &d : doubler)
    d.reset();
  phaser = {};
  chorusTone = {};
  flangerTone = {};
  flangerFeedback = {};
  phaserFeedback = {};
  pitchPhase = {0, .25, .5, .75};
}
bool MorphInstance::activate(const PluginProcessInfo &info) {
  if (!std::isfinite(info.sampleRate) || info.sampleRate < 8000 ||
      info.sampleRate > 192000)
    return false;
  m_rate = info.sampleRate;
  m_gainPole = std::exp(-1 / (m_rate * .18));
  m_gainDownPole = std::exp(-1 / (m_rate * .08));
  m_gainUpPole = std::exp(-1 / (m_rate * .18));
  for (auto &c : m_cores)
    c.prepare(m_rate);
  for (auto &d : m_echo)
    d.prepare(m_rate, 2.01);
  for (unsigned i = 0; i < 4; ++i)
    m_bloom[i].prepare(m_rate, bloomSeconds[i] + .003);
  m_active = true;
  reset();
  return true;
}
std::int32_t
MorphInstance::parameterIndexForId(std::string_view id) const noexcept {
  for (const auto &p : parameterTable())
    if (p.id == id)
      return std::int32_t(p.index);
  return -1;
}
double MorphInstance::parameterValue(std::uint32_t i) const noexcept {
  return i < Count ? m_values[i].load(std::memory_order_relaxed) : 0;
}
void MorphInstance::setParameterFromHost(std::uint32_t i, double value) {
  if (i >= Count || !std::isfinite(value))
    return;
  const auto &p = parameterTable()[i];
  value = std::clamp(value, p.minValue, p.maxValue);
  if (p.isStepped)
    value = std::round(value);
  m_values[i].store(value, std::memory_order_relaxed);
}
std::string MorphInstance::parameterText(std::uint32_t i, double v) const {
  if (i >= Count)
    return {};
  char b[80]{};
  const auto &p = parameterTable()[i];
  if (i == Routing)
    return v < .5 ? "Parallel" : "Serial";
  if (i == DriveMode)
    return v < .5 ? "Warm" : v < 1.5 ? "Tube" : "Fold";
  if (i == DrivePosition)
    return v < .5 ? "Wet input" : "Echo feedback";
  if (i == EnvSource)
    return v < .5 ? "Input L+R" : v < 1.5 ? "Side" : "Sidechain";
  if (i == Lfo1Shape || i == Lfo2Shape) {
    constexpr const char *shapes[]{"Sine", "Triangle", "Saw", "Square"};
    return shapes[unsigned(std::clamp(std::round(v), 0., 3.))];
  }
  if (i == PhaserStages) {
    std::snprintf(b, sizeof(b), "%d", 2 * int(std::lround(v)));
    return b;
  }
  if (i == MotionBars) {
    std::snprintf(b, sizeof(b), "%d bars", 1 << int(std::clamp(v, 0., 3.)));
    return b;
  }
  if (i == ChorusSync || i == FlangerSync || i == PhaserSync || i == Lfo1Sync ||
      i == Lfo2Sync || i == EchoSync) {
    constexpr const char *labels[]{"Free", "1/32",  "1/16",   "1/8",   "1/4",
                                   "1/2",  "1 bar", "2 bars", "4 bars"};
    return labels[unsigned(std::clamp(std::round(v), 0., 8.))];
  }
  if (i >= MatrixStart && (i - MatrixStart) % 3 == 0)
    return sourceNames[unsigned(std::clamp(std::round(v), 0., 15.))];
  if (i >= MatrixStart && (i - MatrixStart) % 3 == 1)
    return parameterTable()[unsigned(std::clamp(std::round(v), 0.,
                                                double(MatrixStart - 1)))]
        .name;
  if (p.unit == "%")
    std::snprintf(b, sizeof(b), "%.0f%%", v * 100);
  else if (p.isStepped)
    std::snprintf(b, sizeof(b), "%.0f", v);
  else if (p.unit == "Hz" && v >= 1000)
    std::snprintf(b, sizeof(b), "%.1f kHz", v / 1000);
  else
    std::snprintf(b, sizeof(b), v < 1 ? "%.2f %s" : "%.1f %s", v,
                  p.unit.c_str());
  return b;
}
State MorphInstance::state() const {
  auto s = m_state;
  for (unsigned i = 0; i < Count; ++i)
    s.values[i] = parameterValue(i);
  return s;
}
bool MorphInstance::saveState(std::vector<std::uint8_t> &out) const {
  return encodeState(state(), out);
}
bool MorphInstance::loadState(std::span<const std::uint8_t> data) {
  const auto next = decodeState(data);
  if (!next)
    return false;
  for (unsigned i = 0; i < Count; ++i)
    setParameterFromHost(i, next->values[i]);
  m_state = *next;
  setCanvas(next->canvas);
  return true;
}
bool MorphInstance::setCanvas(const MorphCanvasProgram &p,
                              const std::string *category) {
  if (!validProgram(p))
    return false;
  if (category)
    m_state.category = *category;
  m_state.canvas = p;
  m_programs[m_back] = p;
  m_back = m_middle.exchange(m_back | 4, std::memory_order_acq_rel) & 3;
  if (m_listener)
    m_listener->onStateChanged();
  return true;
}
void MorphInstance::adoptProgram() noexcept {
  if (m_middle.load(std::memory_order_acquire) & 4)
    m_front = m_middle.exchange(m_front, std::memory_order_acq_rel) & 3;
}
void MorphInstance::setPresetReference(std::string kind, std::string name) {
  m_state.presetKind = std::move(kind);
  m_state.presetName = std::move(name);
  if (m_listener)
    m_listener->onStateChanged();
}
void MorphInstance::reset() noexcept {
  adoptProgram();
  for (auto &c : m_cores)
    c.reset();
  for (auto &d : m_echo)
    d.reset();
  for (auto &d : m_bloom)
    d.reset();
  for (unsigned i = 0; i < Count; ++i)
    m_targets[i] = m_current[i] = parameterValue(i);
  m_steps = {};
  m_controlPhase = 0;
  m_phases = {0, .17, .39, .61};
  m_lfoPhase = {};
  m_lfo = {};
  double enabled = 0;
  for (unsigned i = 0; i < 4; ++i)
    enabled += (m_weights[i] = m_targets[moduleOffsets[i]]);
  if (enabled > 0)
    for (auto &weight : m_weights)
      weight /= enabled;
  m_weightSteps = {};
  m_controlInitialized = false;
  m_allpassCoefficients = {};
  m_allpassSteps = {};
  m_chorusDelays = {};
  m_chorusDelaySteps = {};
  m_outputGain = std::pow(10., m_current[Output] / 20);
  m_doublerGain = std::pow(10., m_current[DoublerLevel] / 20);
  m_outputGainStep = m_doublerGainStep = 0;
  m_hp = {};
  m_lp = {};
  m_echoTone = {};
  m_bloomTone = {};
  m_bloomDamping = {};
  m_previousDrive = {};
  m_previousFeedbackDrive = {};
  m_feedbackDc = {};
  m_fast = m_envelope = m_inputEnergy = m_wetEnergy = m_freeBeat = 0;
  m_dryEnergy = m_crossEnergy = 0;
  m_duck = m_autoGain = m_autoGainTarget = 1;
  m_gainTelemetry.store(1, std::memory_order_relaxed);
  m_routeBlend = m_targets[Routing];
  m_echoFrom = m_echoTo = m_targets[EchoTime] * m_rate * .001;
  m_echoFade = 1;
  m_random.reset(m_programs[m_front].seed ? m_programs[m_front].seed : 1,
                 m_rate / 16);
  for (auto &t : m_telemetry)
    t.store(0, std::memory_order_relaxed);
}
void MorphInstance::control(const PluginProcessContext &ctx,
                            unsigned offset) noexcept {
  const auto &p = m_programs[m_front];
  auto desired = m_targets;
  const double samplesPerBeat = m_rate * 60 / m_tempo;
  const double beatsPerBar = 4. * std::max(1, ctx.transport.timeSigNumerator) /
                             std::max(1, ctx.transport.timeSigDenominator);
  const double beats = ctx.playing && std::isfinite(ctx.transport.ppqPosition)
                           ? ctx.transport.ppqPosition + offset / samplesPerBeat
                           : m_freeBeat;
  double phase =
      cycle(beats / (beatsPerBar * std::exp2(m_targets[MotionBars])));
  if (m_targets[MotionReverse] > .5)
    phase = 1 - phase;
  m_scanPhase = phase;
  double x = m_targets[MorphX], y = m_targets[MorphY];
  if (m_targets[MotionOn] > .5 && p.pointCount > 1) {
    unsigned i = 0;
    while (i + 1 < p.pointCount && p.path[i + 1].time < phase)
      ++i;
    const auto &a = p.path[i];
    const auto &b = p.path[std::min(i + 1, p.pointCount - 1)];
    double t = std::clamp(
        (phase - a.time) / std::max(1.e-6, double(b.time - a.time)), 0., 1.);
    t = blend(t, t * t * (3 - 2 * t), m_targets[MotionSmooth]);
    const double px = blend(double(a.x), double(b.x), t),
                 py = blend(double(a.y), double(b.y), t);
    // Close a freehand stroke gently over the last 4% of its loop.
    const double seam = std::clamp((phase - .96) / .04, 0., 1.);
    x = blend(x, blend(px, double(p.path[0].x), seam), m_targets[MotionDepth]);
    y = blend(y, blend(py, double(p.path[0].y), seam), m_targets[MotionDepth]);
  }
  m_scanX = x;
  m_scanY = y;
  // Read the preceding smoothed control position. Matrix routes may themselves
  // move the scanner; one control tick of delay prevents an algebraic cycle.
  const auto c = sampleCanvas(p, m_current[MorphX], m_current[MorphY]);
  for (unsigned i = 0; i < 2; ++i) {
    const unsigned o = i ? Lfo2Rate : Lfo1Rate;
    double ph = m_targets[o + 4] > .5
                    ? cycle(beats / syncBeats(m_targets[o + 4]))
                    : m_lfoPhase[i];
    ph += m_targets[o + 2] / 360;
    const double f = cycle(ph);
    double raw = sine(ph);
    switch (int(m_targets[o + 1])) {
    case 1:
      raw = 1 - 4 * std::abs(f - .5);
      break;
    case 2:
      raw = 2 * f - 1;
      break;
    case 3:
      raw = f < .5 ? 1 : -1;
      break;
    default:
      break;
    }
    m_lfo[i] +=
        std::clamp(16 / (m_rate * (.0001 + .06 * m_targets[o + 3])), 0., 1.) *
        (raw - m_lfo[i]);
  }
  const double random = m_random.next(m_rate / 16);
  std::array<double, 16> sources{0,
                                 m_lfo[0],
                                 m_lfo[1],
                                 m_envelope,
                                 random,
                                 m_targets[Movement],
                                 m_targets[Texture],
                                 m_targets[Character],
                                 m_targets[Space],
                                 x,
                                 y,
                                 c.light,
                                 c.saturation,
                                 c.edge,
                                 x,
                                 y};
  desired[MorphX] = x;
  desired[MorphY] = y;
  for (unsigned slot = 0; slot < 16; ++slot) {
    const unsigned src = unsigned(m_targets[matrixSource(slot)]),
                   dest = unsigned(m_targets[matrixTarget(slot)]);
    if (src && modulationTarget(dest)) {
      const auto &info = parameterTable()[dest];
      desired[dest] += sources[src] * m_targets[matrixAmount(slot)] *
                       (info.maxValue - info.minValue);
    }
  }
  const double movement = std::exp2((m_targets[Movement] - .5) * 2),
               texture = .5 + m_targets[Texture];
  for (auto i : {ChorusRate, FlangerRate, PhaserRate})
    desired[i] *= movement;
  for (auto i : {ChorusDepth, FlangerDepth, PhaserDepth})
    desired[i] *= texture;
  desired[Drive] += (m_targets[Character] - .5) * .55;
  desired[FlangerFeedback] += (m_targets[Character] - .5) * .3;
  desired[PhaserResonance] += (m_targets[Character] - .5) * .25;
  desired[Width] *= .5 + m_targets[Space];
  desired[BloomLevel] *= .5 + m_targets[Space];
  const double influence =
      m_targets[CanvasOn] > .5 && p.painted ? m_targets[CanvasInfluence] : 0;
  desired[LowPass] *= std::exp2(influence * (c.light - .5) * 2);
  desired[ChorusDepth] += influence * (c.saturation - .5) * .4;
  desired[DoublerDetune] += influence * (c.saturation - .5) * 8;
  desired[Drive] += influence * c.edge * .3;
  desired[FlangerFeedback] += influence * c.edge * .22;
  desired[PhaserResonance] += influence * c.edge * .2;
  for (unsigned i = 0; i < MatrixStart; ++i) {
    const auto &info = parameterTable()[i];
    desired[i] = std::clamp(desired[i], info.minValue, info.maxValue);
    const bool immediate = i == Routing || i == ChorusSync ||
                           i == FlangerSync || i == PhaserSync ||
                           i >= MotionOn || i == EnvSource || i == Lfo1Shape ||
                           i == Lfo2Shape || i == Lfo1Sync || i == Lfo2Sync;
    if (immediate) {
      m_current[i] = desired[i];
      m_steps[i] = 0;
    } else {
      const double duration = (i == Mix || i == Output) ? .005 : .02;
      m_steps[i] = (desired[i] - m_current[i]) *
                   std::min(1., 16 / (m_rate * duration)) / 16;
    }
  }
  x = std::clamp(desired[MorphX], -1., 1.);
  y = std::clamp(desired[MorphY], -1., 1.);
  const double distance = std::abs(x) + std::abs(y);
  if (distance > 1) {
    x /= distance;
    y /= distance;
  }
  const double center = std::max(0., 1 - std::abs(x) - std::abs(y)) * .25;
  std::array<double, 4> w{center + std::max(0., -y), center + std::max(0., -x),
                          center + std::max(0., x), center + std::max(0., y)};
  double sum = 0;
  for (unsigned i = 0; i < 4; ++i) {
    w[i] = blend(w[i], double(c.weights[i]), influence);
    w[i] *= m_targets[moduleOffsets[i]];
    sum += w[i];
  }
  for (unsigned i = 0; i < 4; ++i) {
    const double target = sum > 1.e-8 ? w[i] / sum : 0;
    m_weightSteps[i] =
        (target - m_weights[i]) * std::min(1., 16 / (.01 * m_rate)) / 16;
  }
  for (unsigned i = 0; i < 4; ++i) {
    double ph = m_phases[i];
    const unsigned sync = i == 0   ? ChorusSync
                          : i == 1 ? FlangerSync
                                   : PhaserSync;
    if (i < 3 && m_targets[sync] > .5)
      ph = cycle(beats / syncBeats(m_targets[sync]));
    const double own = sine(ph),
                 shared = sine((m_targets[Lfo1Sync] > .5
                                    ? beats / syncBeats(m_targets[Lfo1Sync])
                                    : m_lfoPhase[0]) +
                               i * .17);
    m_motion[i] = blend(own, shared,
                        m_current[Interaction] * m_current[linkParameters[i]]);
  }
  for (unsigned ch = 0; ch < 2; ++ch)
    for (unsigned voice = 0; voice < 4; ++voice) {
      const double ownPhase = m_targets[ChorusSync] > .5
                                  ? beats / syncBeats(m_targets[ChorusSync])
                                  : m_phases[0];
      const double sharedPhase = m_targets[Lfo1Sync] > .5
                                     ? beats / syncBeats(m_targets[Lfo1Sync])
                                     : m_lfoPhase[0];
      const double spread = voice * .23 + ch * m_current[ChorusSpread] * .3;
      const double wave =
          blend(sine(ownPhase + spread), sine(sharedPhase + spread),
                m_current[Interaction] * m_current[ChorusLink]);
      const double delay =
          (14 + voice * 3 +
           (2 + m_current[ChorusDetune] * .2) * m_current[ChorusDepth] * wave) *
          m_rate * .001;
      if (!m_controlInitialized)
        m_chorusDelays[ch][voice] = delay;
      m_chorusDelaySteps[ch][voice] = (delay - m_chorusDelays[ch][voice]) / 16;
    }
  auto pole = [&](double hz) {
    return std::exp(-2 * pi * std::clamp(hz, 1., m_rate * .45) / m_rate);
  };
  m_chorusPole = pole(1200 + 16000 * m_current[ChorusTone]);
  m_flangerPole = pole(800 + 15000 * m_current[FlangerColor]);
  m_echoPole = pole(16000 - 15000 * m_current[EchoDamping]);
  m_bloomPole = pole(700 + 11000 * m_current[BloomTone]);
  m_hpPole = pole(m_current[HighPass]);
  m_lpPole = pole(m_current[LowPass]);
  m_dcPole = pole(10);
  m_attackPole = std::exp(-1 / (m_rate * m_current[EnvAttack] * .001));
  m_releasePole = std::exp(-1 / (m_rate * m_current[EnvRelease] * .001));
  for (unsigned ch = 0; ch < 2; ++ch)
    for (unsigned stage = 0; stage < 8; ++stage) {
      const double sweep =
          m_motion[2] * (1 - ch * .3 * m_current[PhaserSpread]);
      const double hz = std::clamp(
          350 * std::exp2((sweep * m_current[PhaserDepth] * 2.5) + stage * .37),
          30., m_rate * .42);
      const double t = std::tan(pi * hz / m_rate),
                   coefficient = (t - 1) / (t + 1);
      if (!m_controlInitialized)
        m_allpassCoefficients[ch][stage] = coefficient;
      m_allpassSteps[ch][stage] =
          (coefficient - m_allpassCoefficients[ch][stage]) / 16;
    }
  for (unsigned i = 0; i < 4; ++i) {
    const double cents =
        m_current[DoublerDetune] * (i % 2 ? -1 : 1) * (i < 2 ? 1 : .67);
    m_pitchSteps[i] = (1 - std::exp2(cents / 1200)) / (m_rate * .018);
    m_bloomGain[i] = std::pow(.001, bloomSeconds[i] / m_current[BloomDecay]);
  }
  const double wanted = std::clamp(
      m_targets[EchoSync] > .5 ? syncBeats(m_targets[EchoSync]) * samplesPerBeat
                               : m_current[EchoTime] * m_rate * .001,
      3., m_rate * 2);
  if (m_echoFade >= 1 && std::abs(wanted - m_echoTo) > .5) {
    m_echoFrom = m_echoTo;
    m_echoTo = wanted;
    m_echoFade = 0;
  }
  m_doublerGainStep =
      (std::pow(10., m_current[DoublerLevel] / 20) - m_doublerGain) / 16;
  m_outputGainStep =
      (std::pow(10., m_current[Output] / 20) - m_outputGain) / 16;
  m_driveGain = 1 + m_current[Drive] * 7;
  m_driveNorm = 1 / std::sqrt(m_driveGain);
  m_controlInitialized = true;
}
std::array<double, 2> MorphInstance::modules(Core &core,
                                             std::array<double, 2> in,
                                             bool serial) noexcept {
  const auto &v = m_current;
  auto chain = in;
  std::array<double, 2> parallel{};
  double total = 0;
  for (unsigned module = 0; module < 4; ++module) {
    const double weight = m_weights[module];
    total += weight;
    const auto source = serial ? chain : in;
    auto out = source;
    if (module == 0) {
      for (unsigned ch = 0; ch < 2; ++ch) {
        auto &d = core.chorus[ch];
        d.write(source[ch]);
        double result = 0, norm = 0;
        for (unsigned voice = 0; voice < 4; ++voice) {
          const double gain = std::clamp(v[ChorusVoices] - voice, 0., 1.);
          if (gain <= 0)
            continue;
          result += gain * d.read(m_chorusDelays[ch][voice]);
          norm += gain;
        }
        d.advance();
        core.chorusTone[ch] =
            clean(m_chorusPole * core.chorusTone[ch] +
                  (1 - m_chorusPole) * result / std::max(1., norm));
        out[ch] = core.chorusTone[ch];
      }
    } else if (module == 1) {
      for (unsigned ch = 0; ch < 2; ++ch) {
        auto &d = core.flanger[ch];
        d.write(
            clean(source[ch] + core.flangerFeedback[ch] * v[FlangerFeedback]));
        const double wave = m_motion[1] * (1 - ch * v[FlangerStereo] * .7);
        const double samples =
            std::max(3., v[FlangerDelay] * m_rate * .001 *
                             (1 + .85 * v[FlangerDepth] * wave));
        const double delayed = d.readLinear(samples);
        d.advance();
        core.flangerTone[ch] = clean(m_flangerPole * core.flangerTone[ch] +
                                     (1 - m_flangerPole) * delayed);
        core.flangerFeedback[ch] = core.flangerTone[ch];
        out[ch] =
            core.flangerTone[ch] * (1 - std::abs(v[FlangerFeedback]) * .35);
      }
    } else if (module == 2) {
      for (unsigned ch = 0; ch < 2; ++ch) {
        double result =
            source[ch] + core.phaserFeedback[ch] * v[PhaserResonance];
        double selected = 0;
        for (unsigned stage = 0; stage < 8; ++stage) {
          result = core.phaser[ch][stage].process(
              result, m_allpassCoefficients[ch][stage]);
          if (stage % 2) {
            const double gain =
                std::max(0., 1 - std::abs(v[PhaserStages] - (stage + 1) / 2.));
            selected += gain * result;
          }
        }
        core.phaserFeedback[ch] = clean(selected);
        out[ch] = blend(source[ch], selected * (1 - v[PhaserResonance] * .4),
                        v[PhaserMix]);
      }
    } else {
      std::array<double, 2> result{}, norm{};
      for (unsigned ch = 0; ch < 2; ++ch)
        core.doubler[ch].write(source[ch]);
      for (unsigned voice = 0; voice < 4; ++voice) {
        const double voiceGain = std::clamp(v[DoublerVoices] - voice, 0., 1.);
        if (voiceGain <= 0)
          continue;
        const double phase = core.pitchPhase[voice],
                     other = phase >= .5 ? phase - .5 : phase + .5;
        const double a = .5 - .5 * sine(phase + .25);
        const double base = v[DoublerTime] + v[DoublerHumanize] * 2 *
                                                 m_motion[3] *
                                                 (voice % 2 ? -1 : 1);
        const double first = (base + 18 * phase) * m_rate * .001,
                     second = (base + 18 * other) * m_rate * .001;
        for (unsigned ch = 0; ch < 2; ++ch) {
          const double gain = voiceGain * (1 + (ch == (voice % 2) ? 1 : -1) *
                                                   v[DoublerSpread] * .65);
          const auto &d = core.doubler[ch];
          result[ch] += gain * (a * d.read(first) + (1 - a) * d.read(second));
          norm[ch] += gain;
        }
      }
      for (unsigned ch = 0; ch < 2; ++ch) {
        core.doubler[ch].advance();
        out[ch] = result[ch] / std::max(1., norm[ch]) * m_doublerGain;
      }
      for (unsigned voice = 0; voice < 4; ++voice)
        advancePhase(core.pitchPhase[voice], m_pitchSteps[voice]);
    }
    for (unsigned ch = 0; ch < 2; ++ch) {
      chain[ch] = blend(chain[ch], out[ch], weight);
      parallel[ch] += weight * out[ch];
    }
  }
  if (total < 1.e-8)
    return in;
  return serial ? chain : parallel;
}
double MorphInstance::saturate(double x, unsigned ch, bool feedback) noexcept {
  const double amount = m_current[Drive];
  auto &previous = (feedback ? m_previousFeedbackDrive : m_previousDrive)[ch];
  const double gain = m_driveGain, driven = std::clamp(x * gain, -12., 12.);
  const double delta = driven - previous;
  if (amount < 1.e-8) {
    previous = driven;
    return x;
  }
  double result = 0;
  const double mode = m_current[DriveMode];
  for (unsigned k = 0; k < 3; ++k) {
    const double w = std::max(0., 1 - std::abs(mode - k));
    if (w <= 0)
      continue;
    result +=
        w * (std::abs(delta) < 1.e-5
                 ? transfer((driven + previous) * .5, k)
                 : (primitive(driven, k) - primitive(previous, k)) / delta);
  }
  const double aligned = (driven + previous) * .5 / gain;
  previous = driven;
  if (amount < 1.e-8)
    return x;
  // Both terms of the ADAA parallel blend have the same half-sample phase.
  // A feedback shaper must remain non-expanding. Input Drive may add gain;
  // feedback Drive divides by its input gain to keep the loop below unity.
  const double presence = std::min(1., amount * 3);
  const double shaped =
      blend(aligned, result * (feedback ? 1 / gain : m_driveNorm), presence);
  // Fade in the ADAA branch's half-sample phase along with its character.
  // At zero Drive this must approach the original sample continuously,
  // otherwise the first nonzero automation value abruptly engages averaging.
  return blend(x, shaped, presence);
}
std::array<double, 2> MorphInstance::sample(std::array<double, 2> in) noexcept {
  const auto &v = m_current;
  const double detector = std::max(std::abs(in[0]), std::abs(in[1]));
  m_inputEnergy = m_gainPole * m_inputEnergy +
                  (1 - m_gainPole) * (in[0] * in[0] + in[1] * in[1]) * .5;
  std::array<double, 2> driven;
  for (unsigned ch = 0; ch < 2; ++ch)
    driven[ch] = v[DrivePosition] >= 1 ? in[ch]
                                       : blend(saturate(in[ch], ch, false),
                                               in[ch], v[DrivePosition]);
  for (auto &value : driven)
    value = clean(value);
  m_routeBlend += std::clamp(m_targets[Routing] - m_routeBlend,
                             -1 / (.02 * m_rate), 1 / (.02 * m_rate));
  std::array<double, 2> wet{}, a{}, b{};
  if (m_routeBlend < 1)
    a = modules(m_cores[0], driven, false);
  if (m_routeBlend > 0)
    b = modules(m_cores[1], driven, true);
  for (unsigned ch = 0; ch < 2; ++ch)
    wet[ch] = blend(a[ch], b[ch], m_routeBlend);
  std::array<double, 2> echo{};
  m_echoFade = std::min(1., m_echoFade + 1 / (.03 * m_rate));
  for (unsigned ch = 0; ch < 2; ++ch) {
    const double read = blend(m_echo[ch].readLinear(m_echoFrom),
                              m_echo[ch].readLinear(m_echoTo), m_echoFade);
    m_echoTone[ch] =
        clean(m_echoPole * m_echoTone[ch] + (1 - m_echoPole) * read);
    echo[ch] = m_echoTone[ch];
  }
  for (unsigned ch = 0; ch < 2; ++ch) {
    double feedback = blend(echo[ch], echo[1 - ch], v[EchoPingPong]);
    if (v[DrivePosition] > 0)
      feedback =
          blend(feedback, saturate(feedback, ch, true), v[DrivePosition]);
    m_feedbackDc[ch] =
        clean(m_dcPole * m_feedbackDc[ch] + (1 - m_dcPole) * feedback);
    feedback -= m_feedbackDc[ch];
    m_echo[ch].write(clean(wet[ch] + v[EchoFeedback] * feedback));
    m_echo[ch].advance();
    wet[ch] += v[EchoLevel] * echo[ch];
  }
  std::array<double, 4> tail{};
  double sum = 0;
  for (unsigned i = 0; i < 4; ++i) {
    const double t = m_bloom[i].readLinear(bloomSeconds[i] * m_rate);
    m_bloomDamping[i] =
        clean(m_bloomPole * m_bloomDamping[i] + (1 - m_bloomPole) * t);
    tail[i] = m_bloomDamping[i];
    sum += tail[i];
  }
  const double bloomDuck = 1 - v[Breathing] * (1 - m_duck);
  for (unsigned i = 0; i < 4; ++i) {
    const double diffused = sum * .5 - tail[i];
    const double feedback = blend(tail[i], diffused, v[BloomDensity]);
    m_bloom[i].write(
        clean((wet[i % 2] * .35 * bloomDuck) + feedback * m_bloomGain[i]));
    m_bloom[i].advance();
  }
  wet[0] += v[BloomLevel] * (tail[0] + tail[2]) * .65 * bloomDuck;
  wet[1] += v[BloomLevel] * (tail[1] + tail[3]) * .65 * bloomDuck;
  for (unsigned ch = 0; ch < 2; ++ch) {
    m_hp[ch] = clean(m_hpPole * m_hp[ch] + (1 - m_hpPole) * wet[ch]);
    double high = wet[ch] - m_hp[ch];
    // End stops are exact bypasses, rather than a hidden 20 Hz / 20 kHz EQ.
    if (v[HighPass] <= 20.001)
      high = wet[ch];
    m_lp[ch] = clean(m_lpPole * m_lp[ch] + (1 - m_lpPole) * high);
    wet[ch] = v[LowPass] >= 19999 ? high : m_lp[ch];
  }
  const double mid = (wet[0] + wet[1]) * .5,
               side = (wet[0] - wet[1]) * .5 * v[Width];
  wet = {mid + side, mid - side};
  // Measure the final stereo wet branch, including Width. Voice averaging
  // and phase cancellation can lose more than 6 dB at fully wet settings.
  double wetEnergy = 0, dryEnergy = 0, crossEnergy = 0;
  for (unsigned ch = 0; ch < 2; ++ch) {
    const double dryPart = in[ch] * (1 - bloomDuck);
    const double wetPart = wet[ch] * bloomDuck;
    wetEnergy += wetPart * wetPart * .5;
    dryEnergy += dryPart * dryPart * .5;
    crossEnergy += dryPart * wetPart * .5;
  }
  m_wetEnergy = m_gainPole * m_wetEnergy + (1 - m_gainPole) * wetEnergy;
  m_dryEnergy = m_gainPole * m_dryEnergy + (1 - m_gainPole) * dryEnergy;
  m_crossEnergy = m_gainPole * m_crossEnergy + (1 - m_gainPole) * crossEnergy;
  if (m_controlPhase == 0) {
    if (v[AutoGain] <= .5)
      m_autoGainTarget = 1;
    else if (detector > 1.e-4 && m_inputEnergy > 1.e-8 && m_wetEnergy > 1.e-8) {
      // Breathing restores unamplified dry attacks. Include its correlation
      // with wet in the energy match: wet*g*g + 2*cross*g + dry = input.
      const double discriminant =
          m_crossEnergy * m_crossEnergy +
          m_wetEnergy * std::max(0., m_inputEnergy - m_dryEnergy);
      m_autoGainTarget = std::clamp(
          (std::sqrt(discriminant) - m_crossEnergy) / m_wetEnergy, .25, 4.);
    }
    // Hold through silence: do not chase delayed tails or amplify a noise
    // floor. Gain smoothing has the same time constants at every sample rate.
  }
  const double gainPole =
      m_autoGainTarget < m_autoGain ? m_gainDownPole : m_gainUpPole;
  m_autoGain += (1 - gainPole) * (m_autoGainTarget - m_autoGain);
  const double gain = m_outputGain;
  bool allOff = true;
  for (auto i : moduleOffsets)
    allOff &= v[i] < 1.e-8;
  allOff &= v[Drive] < 1.e-8 && v[EchoLevel] < 1.e-8 && v[BloomLevel] < 1.e-8;
  for (unsigned ch = 0; ch < 2; ++ch) {
    double output = blend(in[ch], wet[ch] * m_autoGain, v[Mix] * bloomDuck);
    if (v[Mix] < 1.e-12 || allOff)
      output = in[ch];
    output *= gain;
    if (v[SoftClip] > 0 && std::abs(output) > .9)
      output = blend(
          output,
          std::copysign(.9 + .1 * (1 - std::exp(-10 * (std::abs(output) - .9))),
                        output),
          v[SoftClip]);
    wet[ch] = (v[Mix] < 1.e-12 && v[SoftClip] < .5)
                  ? modulation::dsp::clean(output)
                  : clean(output);
  }
  return wet;
}
PluginProcessDisposition
MorphInstance::process(const PluginProcessContext &ctx) noexcept {
  if (!ctx.outputs)
    return PluginProcessDisposition::Continue;
  adoptProgram();
  for (unsigned i = 0; i < Count; ++i)
    m_targets[i] = parameterValue(i);
  if (std::isfinite(ctx.transport.tempo) && ctx.transport.tempo >= 1 &&
      ctx.transport.tempo <= 1000)
    m_tempo = ctx.transport.tempo;
  unsigned event = 0;
  double peak[2]{};
  for (unsigned f = 0; f < ctx.frames; ++f) {
    while (event < ctx.inputEvents.size() &&
           ctx.inputEvents[event].frameOffset <= f) {
      const auto &e = ctx.inputEvents[event++];
      if (e.kind == PluginEvent::Kind::ParamValue && e.paramIndex < Count &&
          std::isfinite(e.value)) {
        const auto &info = parameterTable()[e.paramIndex];
        const double value = std::clamp(e.value, info.minValue, info.maxValue);
        m_targets[e.paramIndex] = info.isStepped ? std::round(value) : value;
        m_values[e.paramIndex].store(m_targets[e.paramIndex],
                                     std::memory_order_relaxed);
        m_controlPhase = 0;
      }
    }
    std::array<double, 2> in{};
    for (unsigned ch = 0; ch < 2; ++ch) {
      const unsigned src =
          ctx.inputChannels ? std::min<unsigned>(ch, ctx.inputChannels - 1) : 0;
      if (ctx.inputs && ctx.inputChannels && ctx.inputs[src])
        in[ch] = modulation::dsp::clean(ctx.inputs[src][f]);
    }
    if (!m_active) {
      for (unsigned ch = 0; ch < ctx.outputChannels; ++ch)
        if (ctx.outputs[ch])
          ctx.outputs[ch][f] = float(in[std::min(ch, 1u)]);
      continue;
    }
    double detector = std::max(std::abs(in[0]), std::abs(in[1]));
    if (m_targets[EnvSource] == 1)
      detector = std::abs(in[0] - in[1]) * .5;
    if (m_targets[EnvSource] == 2) {
      detector = 0;
      for (unsigned ch = 0; ch < ctx.sidechainInputChannels; ++ch)
        if (ctx.sidechainInputs && ctx.sidechainInputs[ch])
          detector =
              std::max(detector, std::abs(clean(ctx.sidechainInputs[ch][f])));
    }
    detector =
        std::clamp(detector * (.5 + 6 * m_current[EnvSensitivity]), 0., 1.);
    const double envPole = detector > m_envelope ? m_attackPole : m_releasePole;
    m_envelope = envPole * m_envelope + (1 - envPole) * detector;
    m_fast += .04 * (detector - m_fast);
    const double duckTarget = 1 - std::clamp((m_fast - m_envelope) * 3, 0., .9);
    m_duck += std::min(1., duckTarget < m_duck
                               ? .08
                               : 1 / (m_rate * .001 *
                                      std::max(10., m_current[EnvRelease]))) *
              (duckTarget - m_duck);
    if (!m_controlPhase)
      control(ctx, f);
    for (unsigned i = 0; i < MatrixStart; ++i)
      m_current[i] += m_steps[i];
    for (unsigned ch = 0; ch < 2; ++ch) {
      for (unsigned s = 0; s < 8; ++s)
        m_allpassCoefficients[ch][s] += m_allpassSteps[ch][s];
      for (unsigned v = 0; v < 4; ++v)
        m_chorusDelays[ch][v] += m_chorusDelaySteps[ch][v];
    }
    m_outputGain += m_outputGainStep;
    m_doublerGain += m_doublerGainStep;
    for (unsigned i = 0; i < 4; ++i) {
      m_weights[i] += m_weightSteps[i];
      const auto r = i == 0 ? ChorusRate : i == 1 ? FlangerRate : PhaserRate;
      advancePhase(m_phases[i], (i == 3 ? .13 : m_current[r]) / m_rate);
    }
    for (unsigned i = 0; i < 2; ++i)
      advancePhase(m_lfoPhase[i], m_current[i ? Lfo2Rate : Lfo1Rate] / m_rate);
    const auto out = sample(in);
    for (unsigned ch = 0; ch < ctx.outputChannels; ++ch)
      if (ctx.outputs[ch]) {
        const double value = ctx.outputChannels == 1 ? (out[0] + out[1]) * .5
                                                     : out[std::min(ch, 1u)];
        ctx.outputs[ch][f] = float(value);
        peak[std::min(ch, 1u)] =
            std::max(peak[std::min(ch, 1u)], std::abs(value));
      }
    m_freeBeat += m_tempo / (60 * m_rate);
    m_controlPhase = (m_controlPhase + 1) % 16;
  }
  const float head[]{float(peak[0]),           float(peak[1]),
                     float(m_envelope),        float(m_current[MorphX]),
                     float(m_current[MorphY]), float(m_scanPhase)};
  for (unsigned i = 0; i < 6; ++i)
    m_telemetry[i].store(head[i], std::memory_order_relaxed);
  for (unsigned i = 0; i < 4; ++i) {
    m_telemetry[6 + i].store(float(m_weights[i]), std::memory_order_relaxed);
    m_telemetry[10 + i].store(float(m_motion[i]), std::memory_order_relaxed);
  }
  for (unsigned i = 0; i < MatrixStart; ++i)
    m_telemetry[14 + i].store(float(m_current[i]), std::memory_order_relaxed);
  m_gainTelemetry.store(float(m_autoGain), std::memory_order_relaxed);
  return PluginProcessDisposition::Continue;
}
Telemetry MorphInstance::telemetry() const noexcept {
  Telemetry t;
  t.autoGain = m_gainTelemetry.load(std::memory_order_relaxed);
  t.left = m_telemetry[0].load(std::memory_order_relaxed);
  t.right = m_telemetry[1].load(std::memory_order_relaxed);
  t.envelope = m_telemetry[2].load(std::memory_order_relaxed);
  t.x = m_telemetry[3].load(std::memory_order_relaxed);
  t.y = m_telemetry[4].load(std::memory_order_relaxed);
  t.phase = m_telemetry[5].load(std::memory_order_relaxed);
  for (unsigned i = 0; i < 4; ++i) {
    t.weights[i] = m_telemetry[6 + i].load(std::memory_order_relaxed);
    t.motion[i] = m_telemetry[10 + i].load(std::memory_order_relaxed);
  }
  for (unsigned i = 0; i < MatrixStart; ++i)
    t.effective[i] = m_telemetry[14 + i].load(std::memory_order_relaxed);
  return t;
}
std::uint32_t MorphInstance::tailSamples() const noexcept {
  const auto bound = [this](unsigned id) {
    double value = parameterValue(id);
    for (unsigned s = 0; s < 16; ++s)
      if (parameterValue(matrixSource(s)) > 0 &&
          unsigned(parameterValue(matrixTarget(s))) == id)
        value = parameterTable()[id].maxValue;
    return value;
  };
  double seconds = .25;
  if (bound(EchoLevel) > 0) {
    const double delay =
        parameterValue(EchoSync) > 0 ? 2 : bound(EchoTime) * .001;
    const double feedback = std::clamp(bound(EchoFeedback), .001, .88);
    seconds += delay * (1 + std::log(1.e-5) / std::log(feedback));
  }
  if (bound(BloomLevel) > 0)
    seconds += bound(BloomDecay) * 2;
  return std::uint32_t(std::min(
      m_rate * seconds, double(std::numeric_limits<std::uint32_t>::max())));
}
std::size_t MorphInstance::dspMemoryBytes() const noexcept {
  return sizeof(*this) +
         std::size_t(m_rate * (2 * 2 * (.085 + .03 + .095) + 2 * 2.01 + .0311 +
                               .0437 + .0593 + .0719 + .012) +
                     160) *
             sizeof(double);
}
} // namespace daw::plugins::morph
