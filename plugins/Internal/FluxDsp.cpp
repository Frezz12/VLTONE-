#include "Internal/FluxDsp.hpp"
#include "signalsmith-stretch.h"
#include <complex>
#include <numbers>

namespace daw::plugins::flux::dsp {
namespace {
constexpr double pi = std::numbers::pi, tau = 2 * pi;
constexpr std::array<double, 8> reverbTimes{.0297, .0371, .0411, .0437,
                                            .0531, .0617, .0719, .0893};
double frac(double x) noexcept { return x - std::floor(x); }
double noteHz(double n) noexcept { return 440 * std::exp2((n - 69) / 12); }
Stereo mix(Stereo a, Stereo b, double t) noexcept {
  // Finite audio and bounded interpolation factors do not need std::lerp's
  // overflow/extrapolation branches in every delay tap.
  return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t};
}
double follow(double x, double &state, double attack, double release) noexcept {
  state = clean(x + (state - x) * (x > state ? attack : release));
  return state;
}
} // namespace
double gain(double db) noexcept {
  return db == 0 ? 1 : std::pow(10., db * .05);
}
double pole(double seconds, double rate) noexcept {
  return std::exp(-1 / std::max(1., seconds * rate));
}
double sine(double cycles) noexcept {
  // Modulation oscillators share a table, as in Morph. Linear interpolation
  // bounds the sine error below 7.4e-8; audio-rate waveshapers retain libm.
  static const auto table = [] {
    std::array<double, 8193> values{};
    for (unsigned i = 0; i < values.size(); ++i)
      values[i] = std::sin(tau * i / 8192);
    return values;
  }();
  const double at = frac(cycles) * 8192;
  const auto i = std::min(8191u, unsigned(at));
  return table[i] + (table[i + 1] - table[i]) * (at - i);
}
void Biquad::tune(unsigned mode, double hz, double q, double db,
                  double rate) noexcept {
  const double w = tau * std::clamp(hz, 5., rate * .475) / rate,
               c = std::cos(w), s = std::sin(w);
  const double alpha = s / (2 * std::clamp(q, .1, 20.)),
               A = db == 0 ? 1 : std::pow(10., db / 40);
  double a0 = 1 + alpha;
  a1 = -2 * c;
  a2 = 1 - alpha;
  switch (mode) {
  case 1:
    b0 = (1 + c) * .5;
    b1 = -1 - c;
    b2 = b0;
    break;
  case 2:
    b0 = alpha;
    b1 = 0;
    b2 = -alpha;
    break;
  case 3:
    b0 = 1;
    b1 = -2 * c;
    b2 = 1;
    break;
  case 4:
    b0 = 1 + alpha * A;
    b1 = -2 * c;
    b2 = 1 - alpha * A;
    a0 = 1 + alpha / A;
    a2 = 1 - alpha / A;
    break;
  case 5:
  case 6: {
    const double k = 2 * std::sqrt(A) * alpha, sgn = mode == 5 ? 1. : -1.;
    b0 = A * ((A + 1) - (A - 1) * c * sgn + k);
    b1 = 2 * A * ((A - 1) - (A + 1) * c * sgn) * sgn;
    b2 = A * ((A + 1) - (A - 1) * c * sgn - k);
    a0 = (A + 1) + (A - 1) * c * sgn + k;
    a1 = -2 * ((A - 1) + (A + 1) * c * sgn) * sgn;
    a2 = (A + 1) + (A - 1) * c * sgn - k;
    break;
  }
  default:
    b0 = (1 - c) * .5;
    b1 = 1 - c;
    b2 = b0;
    break;
  }
  b0 /= a0;
  b1 /= a0;
  b2 /= a0;
  a1 /= a0;
  a2 /= a0;
}
double Biquad::tick(double x) noexcept {
  const double y = clean(b0 * x + z1);
  z1 = b1 * x - a1 * y + z2;
  z2 = b2 * x - a2 * y;
  if (std::abs(z1) < 1e-24)
    z1 = 0;
  if (std::abs(z2) < 1e-24)
    z2 = 0;
  return y;
}
void History::prepare(unsigned capacity) {
  samples.assign(std::max(4u, capacity), {});
  reset();
}
void History::push(Stereo x) noexcept {
  if (samples.empty())
    return;
  samples[cursor] = {float(x[0]), float(x[1])};
  if (++cursor == samples.size())
    cursor = 0;
  ++total;
  available = std::min<std::uint64_t>(available + 1, samples.size());
}
Stereo History::absolute(double at) const noexcept {
  if (!available || !std::isfinite(at) || at < double(total - available) ||
      at > double(total - 1))
    return {};
  const auto a = std::uint64_t(at);
  // The validity check bounds the distance to one ring. Avoid two integer
  // divisions for every delay tap (and every active grain).
  const auto distance = std::size_t(total - a);
  const auto first = cursor >= distance ? cursor - distance
                                        : cursor + samples.size() - distance;
  const auto second = a == total - 1 ? first
                      : first + 1 == samples.size() ? 0 : first + 1;
  auto x = samples[first], y = samples[second];
  return mix({x[0], x[1]}, {y[0], y[1]}, at - double(a));
}
Stereo History::ago(double delay) const noexcept {
  return absolute(double(total) - 1 - std::max(0., delay));
}
void Crossover::tune(double lo, double hi, double rate) noexcept {
  hi = std::max(hi, lo * 1.05);
  for (auto &f : filters)
    for (unsigned i = 0; i < 12; ++i)
      f[i].tune((i / 2) % 2, i < 4 ? lo : hi, .7071067811865476, 0, rate);
}
std::array<Stereo, 3> Crossover::split(Stereo x) noexcept {
  std::array<Stereo, 3> y{};
  for (unsigned c = 0; c < 2; ++c) {
    auto &f = filters[c];
    const double lo = f[1].tick(f[0].tick(x[c])),
                 hi = f[3].tick(f[2].tick(x[c]));
    y[0][c] = f[5].tick(f[4].tick(lo)) + f[7].tick(f[6].tick(lo));
    y[1][c] = f[9].tick(f[8].tick(hi));
    y[2][c] = f[11].tick(f[10].tick(hi));
  }
  return y;
}
void Crossover::reset() noexcept {
  for (auto &f : filters)
    for (auto &b : f)
      b.reset();
}

struct Processor::Impl {
  Kind kind;
  double rate;
  bool studio;
  unsigned seed, latency = 0, control = 0;
  Values p{}, tuned{};
  bool tunedReady = false;
  std::array<double, 8> reverbFeedback{};
  Values coefficientInputs{}, coefficients{};
  double followerAttack = 0, followerRelease = 0, slowAttack = 0,
         slowRelease = 0, dcPole = 0;
  std::array<double, 4> resonatorInputs{};
  std::array<double, 8> resonatorDelay{}, resonatorFeedback{};
  double resonatorNorm = 1;
  History history, dry, predelay;
  std::array<History, 8> delays;
  std::array<std::array<Biquad, 40>, 2> filters;
  std::array<std::array<double, 12>, 2> allpass{};
  Stereo tone{}, previous{}, held{}, feedback{}, dcIn{}, dcOut{};
  std::array<double, 16> envelopes{};
  double env = 0, slow = 0, phase = 0, hold = 0, read = 0, anchor = 0, age = 0,
         fill = 0, freezePosition = 0;
  std::int64_t cycle = std::numeric_limits<std::int64_t>::min();
  bool frozen = false, ready = false, panicFreeze = false;
  std::uint64_t ticks = 0, grainCounter = 0;
  struct Grain {
    double position = 0, speed = 1, age = 0, length = 0, pan = 0;
    double windowCos = 1, windowSin = 0, deltaCos = 1, deltaSin = 0;
    Stereo panGain{1, 1};
  };
  std::array<Grain, 64> grains{};
  std::unique_ptr<signalsmith::stretch::SignalsmithStretch<float>> stretch;
  using STFT = signalsmith::linear::DynamicSTFT<
      float, false, signalsmith::linear::STFT_SPECTRUM_UNPACKED>;
  std::unique_ptr<STFT> stft;
  std::array<std::vector<double>, 2> magnitudes, phases, increments, lastPhase;
  std::array<std::vector<std::complex<float>>, 2> scratch;
  unsigned hop = 0;

  Impl(Kind k, double r, bool s, unsigned sd)
      : kind(k), rate(r), studio(s), seed(sd) {
    (void)sine(0); // Initialize the shared table on the control thread.
    coefficientInputs.fill(std::numeric_limits<double>::quiet_NaN());
    resonatorInputs.fill(std::numeric_limits<double>::quiet_NaN());
    followerAttack = pole(k == Kind::Reverb ? .002 : .003, rate);
    followerRelease = pole(k == Kind::Reverb ? .15 : k == Kind::Noise ? .08 : .1, rate);
    slowAttack = pole(.025, rate);
    slowRelease = pole(.3, rate);
    dcPole = std::exp(-2 * pi * 5 / rate);
    auto defs = fields(k);
    for (unsigned i = 0; i < defs.size(); ++i)
      p[i] = defs[i].initial;
    const bool buffer = k == Kind::Time || k == Kind::Repeat ||
                        k == Kind::Tape || k == Kind::Granular;
    if (buffer)
      history.prepare(unsigned(rate * 32) + 8);
    if (k == Kind::Delay)
      history.prepare(unsigned(rate * 8) + 8);
    if (k == Kind::Chorus || k == Kind::Flanger)
      history.prepare(unsigned(rate * .12) + 8);
    if (k == Kind::Resonator)
      for (auto &d : delays)
        d.prepare(unsigned(rate * .15) + 8);
    if (k == Kind::Reverb) {
      for (auto &d : delays)
        d.prepare(unsigned(rate * .3) + 8);
      predelay.prepare(unsigned(rate * .25) + 8);
      history.prepare(unsigned(rate * .12) + 8);
    }
    const unsigned window =
        unsigned((studio ? 4096 : 1024) * rate / 48000 + 31) & ~31u;
    if (k == Kind::Pitch || k == Kind::Time) {
      stretch =
          std::make_unique<signalsmith::stretch::SignalsmithStretch<float>>(
              seed);
      stretch->configure(2, int(window), int(window / 4), true);
      latency = unsigned(stretch->inputLatency() + stretch->outputLatency());
    }
    if (k == Kind::Spectral) {
      stft = std::make_unique<STFT>();
      stft->configure(2, 2, window, 0, window / 4);
      latency = unsigned(stft->latency());
      for (unsigned c = 0; c < 2; ++c) {
        const auto n = stft->bands();
        magnitudes[c].resize(n);
        phases[c].resize(n);
        increments[c].resize(n);
        lastPhase[c].resize(n);
        scratch[c].resize(n);
      }
    }
    if (latency)
      dry.prepare(latency + 8);
    tune();
  }
  void reset() noexcept {
    history.reset();
    dry.reset();
    predelay.reset();
    for (auto &d : delays)
      d.reset();
    for (auto &f : filters)
      for (auto &b : f)
        b.reset();
    allpass = {};
    tone = previous = held = feedback = dcIn = dcOut = {};
    envelopes = {};
    env = slow = phase = hold = read = anchor = age = fill = freezePosition = 0;
    cycle = std::numeric_limits<std::int64_t>::min();
    frozen = ready = false;
    panicFreeze = true;
    ticks = grainCounter = 0;
    grains = {};
    hop = control = 0;
    tunedReady = false;
    if (stretch)
      stretch->reset();
    if (stft)
      stft->reset();
    for (unsigned c = 0; c < 2; ++c) {
      std::fill(magnitudes[c].begin(), magnitudes[c].end(), 0);
      std::fill(lastPhase[c].begin(), lastPhase[c].end(), 0);
    }
  }
  void tune() noexcept {
    if (kind == Kind::Reverb) {
      const double modeSize = p[4] < .5                  ? .55
                              : p[4] > 1.5 && p[4] < 2.5 ? .8
                                                         : 1;
      for (unsigned i = 0; i < reverbFeedback.size(); ++i)
        reverbFeedback[i] = std::min(
            .9995, std::pow(.001, reverbTimes[i] * p[1] * modeSize / p[0]));
    }
    for (unsigned c = 0; c < 2; ++c) {
      auto &f = filters[c];
      switch (kind) {
      case Kind::Filter:
        for (unsigned i = 0; i < 4; ++i)
          f[i].tune(unsigned(p[2]) == 7 ? (i < 2 ? 1 : 0) : unsigned(p[2]),
                    unsigned(p[2]) == 7 && i >= 2 ? p[7] : p[0], p[1], p[6],
                    rate * (studio ? 4 : 2));
        for (unsigned i = 34; i < 36; ++i)
          f[i].tune(0, rate * .43, .707, 0, rate * (studio ? 4 : 2));
        break;
      case Kind::Vowel: {
        constexpr double vowel[5][3] = {{800, 1150, 2900},
                                        {400, 1700, 2600},
                                        {300, 2200, 3000},
                                        {450, 800, 2830},
                                        {325, 700, 2530}};
        unsigned a = std::min(3u, unsigned(p[0]));
        double t = p[0] - a;
        for (unsigned i = 0; i < 3; ++i)
          f[i].tune(2,
                    std::lerp(vowel[a][i], vowel[a + 1][i], t) *
                        std::exp2(p[3] / 12),
                    p[1] / p[2], 0, rate);
        break;
      }
      case Kind::Drive:
        f[0].tune(0, p[3], .707, 0, rate);
        for (unsigned i = 1; i < 3; ++i)
          f[i].tune(0, rate * .43, .707, 0, rate * (studio ? 4 : 2));
        break;
      case Kind::Crush:
        f[0].tune(0, p[4], .707, 0, rate);
        break;
      case Kind::Noise:
        f[0].tune(0, p[2], .707, 0, rate);
        f[1].tune(1, 2000, .707, 0, rate);
        break;
      case Kind::Tape:
        f[0].tune(0, p[4], .707, 0, rate);
        for (unsigned i = 1; i < 3; ++i)
          f[i].tune(0, rate * .43, .707, 0, rate * (studio ? 4 : 2));
        break;
      case Kind::Chorus:
        f[0].tune(0, p[5], .707, 0, rate);
        break;
      case Kind::Delay:
        f[0].tune(1, p[2], .707, 0, rate);
        f[1].tune(0, p[3], .707, 0, rate);
        break;
      case Kind::Vocoder:
        for (unsigned i = 0; i < 16; ++i) {
          double freq = 100 * std::pow(80., i / 15.);
          f[i].tune(2, freq, 3 / p[2], 0, rate);
          f[16 + i].tune(2, freq * std::exp2(p[1] / 12), 3 / p[2], 0, rate);
        }
        f[32].tune(1, 5000, .707, 0, rate);
        break;
      default:
        break;
      }
    }
    if (stretch && kind == Kind::Pitch) {
      double n = p[0];
      if (p[4] >= 1) {
        constexpr int scales[3][7] = {{0, 2, 4, 5, 7, 9, 11},
                                      {0, 2, 3, 5, 7, 8, 10},
                                      {0, 2, 4, 7, 9, 12, 14}};
        double best = 1e9, chosen = n;
        for (int octave = -3; octave <= 3; ++octave)
          for (int degree : scales[std::min(2, int(p[4]) - 1)]) {
            double candidate = octave * 12 + degree + p[5];
            if (std::abs(n - candidate) < best) {
              best = std::abs(n - candidate);
              chosen = candidate;
            }
          }
        n = chosen;
      }
      stretch->setTransposeSemitones(float(n + p[1] / 100));
      stretch->setFormantSemitones(float(p[2]), p[3] > .5);
    }
  }
  // Each field has one coefficient type for its processor kind. Recompute at
  // the exact sample where a smoothed/automated value changes, not every sample.
  double cachedGain(unsigned field) noexcept {
    if (coefficientInputs[field] != p[field]) {
      coefficientInputs[field] = p[field];
      coefficients[field] = gain(p[field]);
    }
    return coefficients[field];
  }
  double cachedPole(unsigned field) noexcept {
    if (coefficientInputs[field] != p[field]) {
      coefficientInputs[field] = p[field];
      coefficients[field] = pole(p[field] * .001, rate);
    }
    return coefficients[field];
  }
  Stereo pitch(Stereo x) noexcept {
    float in[2]{float(x[0]), float(x[1])}, out[2]{};
    const float *ip[]{in, in + 1};
    float *op[]{out, out + 1};
    stretch->process(ip, 1, op, 1);
    return {out[0], out[1]};
  }
  Stereo spectral(Stereo x, const Clock &clock) noexcept {
    for (unsigned c = 0; c < 2; ++c) {
      float v = float(x[c]);
      stft->writeInput(c, 1, &v);
    }
    stft->moveInput(1);
    if (++hop >= stft->defaultInterval()) {
      hop = 0;
      stft->analyse();
      const auto n = stft->bands();
      const double shift = p[2] * stft->fftSamples() / rate,
                   decay = std::exp(-double(stft->defaultInterval()) /
                                    (rate * p[6]));
      for (unsigned c = 0; c < 2; ++c) {
        auto *spectrum = stft->spectrum(c);
        auto &mag = magnitudes[c];
        auto &ph = phases[c];
        auto &inc = increments[c];
        auto &prev = lastPhase[c];
        for (unsigned b = 0; b < n; ++b) {
          const double angle = std::arg(spectrum[b]),
                       expected = tau * b * stft->defaultInterval() /
                                  stft->fftSamples();
          const double delta =
              expected + std::remainder(angle - prev[b] - expected, tau);
          prev[b] = angle;
          if (!frozen || p[0] < .01) {
            inc[b] = delta;
            ph[b] = angle;
            mag[b] = std::abs(spectrum[b]);
          } else {
            ph[b] += inc[b];
            mag[b] =
                std::lerp(double(std::abs(spectrum[b])), mag[b] * decay, p[0]);
          }
        }
        for (unsigned b = 0; b < n; ++b) {
          double a = mag[b];
          if (b > 0 && b + 1 < n)
            a = std::lerp(a, (mag[b - 1] + a + mag[b + 1]) / 3, p[1]);
          // Temporal smoothing and neighbouring bins make blur independent of
          // block size.
          double old = std::abs(scratch[c][b]);
          a = std::lerp(a, old, p[1] * .85);
          scratch[c][b] = std::polar(float(a), float(ph[b]));
        }
        for (unsigned b = 0; b < n; ++b) {
          const double hz = b * rate / stft->fftSamples(), source = b - shift;
          if (hz < p[3] || hz > p[4] || source < 0 || source >= n - 1 ||
              randomAt(clock.seed, std::int64_t(std::floor(clock.beat * 4)),
                       b + 31 * c) < p[5])
            spectrum[b] = 0;
          else {
            auto index = unsigned(source);
            float t = float(source - index);
            spectrum[b] =
                scratch[c][index] * (1 - t) + scratch[c][index + 1] * t;
          }
        }
      }
      frozen = p[0] > .01 && !panicFreeze;
      if (p[0] < .01)
        panicFreeze = false;
      // Restoring a frozen state captures the first complete new analysis
      // window.
      if (panicFreeze && ticks >= stft->blockSamples()) {
        panicFreeze = false;
        frozen = p[0] > .01;
      }
      stft->synthesise();
    }
    Stereo out{};
    for (unsigned c = 0; c < 2; ++c) {
      float v = 0;
      stft->readOutput(c, 1, &v);
      out[c] = v;
    }
    stft->moveOutput(1);
    return out;
  }
  Stereo time(Stereo x, const Clock &clock) noexcept {
    history.push(x);
    const double beatSamples = rate * 60 / clock.tempo;
    const double beats = kind == Kind::Time
                             ? ((p[2] < .5 || std::abs(p[2] - 4) < .5)
                                    ? p[5] * clock.barBeats
                                    : p[3] * (p[6] > .5 ? clock.barBeats : 1))
                         : kind == Kind::Repeat ? p[1]
                                                : p[5];
    const double length =
        std::clamp(beats * beatSamples, 8.,
                   std::min(rate * 32 - 8, clock.barBeats * 4 * beatSamples));
    fill = std::min(1., history.available / length);
    const auto event = std::int64_t(std::floor(clock.beat / beats + 1e-9));
    if (fill < 1) {
      if (latency) {
        dry.push(x);
        return dry.ago(latency);
      }
      return x;
    }
    if (!ready || event != cycle || clock.retrigger) {
      anchor = double(history.total) - 1;
      read = kind == Kind::Time && (p[2] == 3 || p[1] < 0) ? length - 1 : 0;
      cycle = event;
      ready = true;
      age = 0;
    }
    Stereo y = x;
    double relative = 0, fade = kind == Kind::Time     ? p[4]
                                : kind == Kind::Repeat ? p[5]
                                                       : 3;
    if (kind == Kind::Repeat) {
      const double slice = std::clamp(p[0] * beatSamples, 8., length),
                   pos = std::fmod(read, slice);
      unsigned section = 0;
      if (p[4] > .5)
        section = unsigned(randomAt(seed, event, unsigned(read / slice)) *
                           std::max(1., std::floor(length / slice)));
      relative = std::min(length - 1, section * slice +
                                          (p[3] > .5 ? slice - 1 - pos : pos));
      y = history.absolute(anchor - length + relative);
      const double repeats = std::floor(read / slice);
      for (auto &v : y)
        v *= std::pow(1 - p[2], repeats);
      const double edge = std::min(pos, slice - pos);
      for (auto &v : y)
        v *= std::min(
            1., edge / std::max(1., std::min(slice * .2, fade * .001 * rate)));
      ++read;
    } else {
      double speed = kind == Kind::Tape ? p[0] : p[1];
      const unsigned mode = kind == Kind::Time ? unsigned(p[2]) : 5;
      if (mode == 1)
        speed = .5;
      if (mode == 2)
        speed = .25;
      if (mode == 3)
        speed = -1;
      if (kind == Kind::Tape)
        speed *= 1 + p[1] * .015 * sine(clock.sample / rate * .7) +
                 p[2] * .005 * sine(clock.sample / rate * 11.7);
      if (mode == 0)
        relative = length - 1 - p[0] * (length - 1);
      else if (mode == 4)
        relative = (length - 1) * (1 - p[0]);
      else
        relative = std::fmod(read + length * 8, length);
      y = (mode == 0 || mode == 4)
              ? history.ago(p[0] * (length - 1))
              : history.absolute(anchor - length + relative);
      // Overlap the two ends of the captured region, avoiding discontinuities.
      const double overlap = std::min(length * .1, fade * .001 * rate),
                   edge = length - relative;
      if (mode != 0 && mode != 4 && edge < overlap)
        y = mix(
            y,
            history.absolute(anchor - length + (relative - length + overlap)),
            1 - edge / overlap);
      read += speed;
      age += 1;
      if (kind == Kind::Tape)
        for (unsigned c = 0; c < 2; ++c) {
          unsigned os = studio ? 4 : 2;
          double result = 0;
          for (unsigned i = 1; i <= os; ++i) {
            double v = std::tanh(std::lerp(previous[c], y[c], double(i) / os) *
                                 cachedGain(3)) /
                       std::max(1., cachedGain(3) * .6);
            result = filters[c][2].tick(filters[c][1].tick(v));
          }
          previous[c] = y[c];
          y[c] =
              filters[c][0].tick(result) * std::sqrt(std::clamp(speed, 0., 1.));
        }
      if (kind == Kind::Time) {
        if (mode == 6) {
          stretch->setTransposeFactor(float(1 / std::max(.1, std::abs(speed))));
          y = pitch(y);
        } else {
          dry.push(y);
          y = dry.ago(latency);
        }
      }
    }
    return y;
  }
  Stereo granular(Stereo x, const Clock &clock) noexcept {
    const double length =
        std::min({p[8] * rate * 60 / clock.tempo, rate * 32 - 8,
                  clock.barBeats * 4 * rate * 60 / clock.tempo});
    if (!frozen)
      history.push(x);
    fill = std::min(1., history.available / length);
    if (fill < 1)
      return x;
    if (p[7] < .5) {
      frozen = false;
      panicFreeze = false;
    } else if (!panicFreeze)
      frozen = true;
    else if (fill >= 1) {
      panicFreeze = false;
      frozen = true;
    }
    const unsigned count = studio ? 64 : 16;
    hold -= p[2] / rate;
    if (hold <= 0 || clock.retrigger) {
      hold += 1;
      for (unsigned i = 0; i < count; ++i)
        if (grains[i].age >= grains[i].length) {
          auto &g = grains[i];
          auto id = grainCounter++;
          g.age = 0;
          g.length = p[1] * rate * .001;
          g.speed =
              std::exp2(p[3] / 12) * (randomAt(seed, id, 2) < p[5] ? -1 : 1);
          g.pan = (randomAt(seed, id, 3) * 2 - 1) * p[6];
          g.panGain = {std::sqrt(1 - g.pan), std::sqrt(1 + g.pan)};
          g.windowCos = 1;
          g.windowSin = 0;
          g.deltaCos = std::cos(tau / g.length);
          g.deltaSin = std::sin(tau / g.length);
          double offset = std::clamp(
              p[0] + (randomAt(seed, id, 1) * 2 - 1) * p[4] * .5, 0., 1.);
          g.position = double(history.total) - 1 -
                       offset * std::max(8., length - g.length) -
                       std::max(g.length, 8.);
          break;
        }
    }
    Stereo y{};
    double weight = 0;
    for (unsigned i = 0; i < count; ++i) {
      auto &g = grains[i];
      if (g.age >= g.length)
        continue;
      // Rotate the Hann window's oscillator instead of recalculating a
      // cosine and two fixed pan gains for every grain on every sample.
      const double w = std::clamp(.5 - .5 * g.windowCos, 0., 1.);
      const double nextCos =
          g.windowCos * g.deltaCos - g.windowSin * g.deltaSin;
      g.windowSin = g.windowSin * g.deltaCos + g.windowCos * g.deltaSin;
      g.windowCos = nextCos;
      auto v = history.absolute(g.position);
      y[0] += v[0] * w * g.panGain[0];
      y[1] += v[1] * w * g.panGain[1];
      weight += w;
      g.position += g.speed;
      ++g.age;
    }
    for (auto &v : y)
      v /= std::max(1., std::sqrt(weight));
    return y;
  }
  Stereo run(Stereo x, Stereo sc, const Values &values,
             const Clock &clock) noexcept {
    p = values;
    if ((control++ & 31u) == 0 && (!tunedReady || tuned != p)) {
      tune();
      tuned = p;
      tunedReady = true;
    }
    ++ticks;
    Stereo y = x;
    const double peak = std::max(std::abs(x[0]), std::abs(x[1]));
    switch (kind) {
    case Kind::Volume:
      follow(p[0], env, cachedPole(1), cachedPole(1));
      for (auto &v : y)
        v *= env;
      break;
    case Kind::Dynamics: {
      double detector =
          p[8] > .5 ? std::max(std::abs(sc[0]), std::abs(sc[1])) : peak;
      follow(detector, env, cachedPole(2), cachedPole(3));
      follow(detector, slow, slowAttack, slowRelease);
      double db = 20 * std::log10(std::max(1e-9, env)), over = db - p[0],
             reduction = 0;
      if (p[5] < .5) {
        if (over > p[4] * .5)
          reduction = over * (1 / p[1] - 1);
        else if (over > -p[4] * .5 && p[4] > 0)
          reduction =
              (1 / p[1] - 1) * std::pow(over + p[4] * .5, 2) / (2 * p[4]);
      } else if (p[5] < 1.5)
        reduction = std::min(0., over) * (p[1] - 1);
      else {
        double transient = std::clamp((env - slow) / (slow + .01), 0., 1.);
        reduction = std::lerp(p[7], p[6], transient);
      }
      for (auto &v : y)
        v *= gain(std::clamp(reduction, -90., 24.));
      break;
    }
    case Kind::Filter: {
      const unsigned os = studio ? 4 : 2;
      const double inputGain = cachedGain(5), bandGain = cachedGain(6) - 1;
      const double g =
          p[4] > .5
              ? 1 - std::exp(-tau * std::min(p[0], rate * .45) / (rate * os))
              : 0;
      const double k = std::clamp((p[1] - .5) * .33, 0., 3.8);
      for (unsigned c = 0; c < 2; ++c) {
        auto &f = filters[c];
        double result = 0;
        for (unsigned i = 1; i <= os; ++i) {
          double input = std::lerp(previous[c], x[c], double(i) / os),
                 v = input;
          if (p[4] > .5) {
            // Four saturated integrators with local feedback form the ladder.
            // The integrators and the decimation filter run at the quality
            // rate.
            auto &z = allpass[c];
            double drive =
                std::tanh(input * inputGain - k * z[p[3] > .5 ? 3 : 1]);
            for (unsigned stage = 0; stage < 4; ++stage) {
              z[stage] += g * (std::tanh(drive) - std::tanh(z[stage]));
              drive = z[stage];
            }
            double low = p[3] > .5 ? z[3] : z[1],
                   high = p[3] > .5
                              ? input - 4 * z[0] + 6 * z[1] - 4 * z[2] + z[3]
                              : input - 2 * z[0] + z[1],
                   band = 2 * (z[1] - z[3]);
            switch (unsigned(p[2])) {
            case 1:
            case 7:
              v = high;
              break;
            case 2:
              v = band;
              break;
            case 3:
              v = input - band;
              break;
            case 4:
              v = input + bandGain * band;
              break;
            case 5:
              v = input + bandGain * low;
              break;
            case 6:
              v = input + bandGain * high;
              break;
            default:
              v = low;
              break;
            }
          } else {
            if (p[5] > .01)
              v = std::tanh(v * inputGain);
            v = f[0].tick(v);
            if (p[3] > .5)
              v = f[1].tick(v);
          }
          if (p[2] > 6.5) {
            v = f[2].tick(v);
            if (p[3] > .5)
              v = f[3].tick(v);
          }
          result = f[35].tick(f[34].tick(v));
        }
        previous[c] = x[c];
        y[c] = result;
      }
      break;
    }
    case Kind::Vowel:
      for (unsigned c = 0; c < 2; ++c)
        y[c] = 2.5 * (filters[c][0].tick(x[c]) + .7 * filters[c][1].tick(x[c]) +
                      .3 * filters[c][2].tick(x[c]));
      break;
    case Kind::Drive: {
      const unsigned os = studio ? 4 : 2;
      const double inputGain = cachedGain(0);
      const double compensation = p[4] > .5 ? 1 / std::sqrt(inputGain) : 1;
      for (unsigned c = 0; c < 2; ++c) {
        double sum = 0;
        for (unsigned i = 1; i <= os; ++i) {
          double v =
              std::lerp(previous[c], x[c], double(i) / os) * inputGain + p[2];
          switch (unsigned(p[1])) {
          case 1:
            v = std::clamp(v, -1., 1.);
            break;
          case 2:
            v = std::tanh(v + .25) - std::tanh(.25);
            break;
          case 3:
            v = v / (1 + std::abs(v));
            break;
          case 4:
            v = 2 / std::numbers::pi * std::asin(std::sin(v * pi * .5));
            break;
          case 5:
            v = std::abs(v);
            break;
          default:
            v = std::tanh(v);
            break;
          }
          sum = filters[c][2].tick(filters[c][1].tick(v));
        }
        previous[c] = x[c];
        double v = sum - dcIn[c] + dcPole * dcOut[c];
        dcIn[c] = sum;
        dcOut[c] = v;
        y[c] = filters[c][0].tick(v) * compensation;
      }
      break;
    }
    case Kind::Crush: {
      hold -= p[1] / rate;
      if (hold <= 0) {
        hold += 1 + (randomAt(seed, clock.sample, 8) - .5) * p[2];
        double steps = std::exp2(p[0] - 1);
        for (unsigned c = 0; c < 2; ++c)
          held[c] = std::round(x[c] * steps +
                               (randomAt(seed, clock.sample, c) - .5) * p[3]) /
                    steps;
      }
      for (unsigned c = 0; c < 2; ++c)
        y[c] = filters[c][0].tick(held[c]);
      break;
    }
    case Kind::Noise: {
      follow(peak, env, followerAttack, followerRelease);
      double level = cachedGain(0) * std::lerp(1., std::min(1., env * 3), p[3]) *
                     (env >= cachedGain(4) ? 1 : 0);
      for (unsigned c = 0; c < 2; ++c) {
        double white = randomAt(seed, clock.sample, c) * 2 - 1, v = white;
        switch (unsigned(p[1])) {
        case 1:
          tone[c] = tone[c] * .96 + white * .04;
          v = (white * .18 + tone[c] * 2);
          break;
        case 2:
          tone[c] = std::clamp(tone[c] * .998 + white * .025, -1., 1.);
          v = tone[c] * 3;
          break;
        case 3:
          v = filters[c][1].tick(white);
          break;
        case 4:
          v = randomAt(seed, clock.sample, 22 + c) < .001 ? white * 10 : 0;
          break;
        default:
          break;
        }
        y[c] = x[c] + filters[c][0].tick(v) * level;
      }
      break;
    }
    case Kind::Time:
    case Kind::Repeat:
    case Kind::Tape:
      y = time(x, clock);
      break;
    case Kind::Pitch:
      y = pitch(x);
      break;
    case Kind::Granular:
      y = granular(x, clock);
      break;
    case Kind::Spectral:
      y = spectral(x, clock);
      break;
    case Kind::Resonator: {
      y = {};
      constexpr int scales[3][8] = {{0, 4, 7, 12, 16, 19, 24, 28},
                                    {0, 3, 7, 10, 12, 15, 19, 22},
                                    {0, 2, 4, 7, 9, 12, 14, 16}};
      unsigned voices = unsigned(p[3]);
      const std::array<double, 4> settings{p[0], p[1], p[3], p[4]};
      if (settings != resonatorInputs) {
        resonatorInputs = settings;
        resonatorNorm = std::sqrt(double(voices));
        for (unsigned i = 0; i < voices; ++i) {
          const double hz = p[4] < .5
              ? noteHz(p[0]) * (i + 1)
              : noteHz(p[0] + scales[std::min(2, int(p[4]) - 1)][i]);
          resonatorDelay[i] = rate / hz;
          resonatorFeedback[i] = std::min(
              .9995, std::pow(.001, resonatorDelay[i] / (p[1] * rate)));
        }
      }
      for (unsigned i = 0; i < voices; ++i) {
        auto v = delays[i].ago(resonatorDelay[i] - 1);
        const double feedbackGain = resonatorFeedback[i];
        Stereo next{};
        for (unsigned c = 0; c < 2; ++c) {
          double &damp = allpass[c][i];
          damp = std::lerp(v[c], damp, p[2] * .95);
          next[c] = clean(x[c] * p[5] + damp * feedbackGain);
          y[c] += v[c] / resonatorNorm;
        }
        delays[i].push(next);
      }
      break;
    }
    case Kind::Ring: {
      phase = frac(phase + p[0] / rate);
      for (unsigned c = 0; c < 2; ++c) {
        double a = tau * phase + c * p[4] * pi / 180, v = 0;
        if (p[2] < .5) {
          double carrier = std::sin(a);
          if (p[3] > .5)
            carrier = p[3] > 1.5 ? (carrier >= 0 ? 1 : -1)
                                 : std::asin(carrier) * 2 / pi;
          v = x[c] * carrier;
        } else { // Two all-pass paths approximate a quadrature Hilbert pair.
          constexpr double ca[]{.161758, .733029, .94535, .990598},
              cb[]{.479401, .876218, .976597, .9975};
          double re = x[c], im = previous[c];
          previous[c] = x[c];
          for (unsigned i = 0; i < 4; ++i) {
            double z = re;
            re = ca[i] * re + allpass[c][i];
            allpass[c][i] = z - ca[i] * re;
            z = im;
            im = cb[i] * im + allpass[c][4 + i];
            allpass[c][4 + i] = z - cb[i] * im;
          }
          v = re * std::cos(a) - im * std::sin(a) * (p[5] < 0 ? -1 : 1);
        }
        y[c] = std::lerp(x[c], v, p[1]);
      }
      break;
    }
    case Kind::Chorus:
    case Kind::Flanger: {
      bool chorus = kind == Kind::Chorus;
      double speed = p[0] * (p[chorus ? 6 : 5] > .5 ? clock.tempo / 60 : 1);
      phase = frac(phase + speed / rate);
      if (chorus) {
        history.push(x);
        for (unsigned c = 0; c < 2; ++c) {
          double sum = 0;
          unsigned voices = unsigned(p[2]);
          for (unsigned i = 0; i < voices; ++i) {
            double a = phase + double(i) / voices + c * p[4] * .25;
            double delay =
                (12 + p[1] * (1 + sine(a)) +
                 .1 * p[3] * sine(phase * .37 + i * .31)) *
                rate * .001;
            sum += history.ago(delay)[c];
          }
          y[c] = .5 * x[c] + .5 * filters[c][0].tick(sum / voices);
        }
      } else {
        Stereo next{};
        for (unsigned c = 0; c < 2; ++c) {
          double delay =
              (p[3] +
               p[1] * (1 + sine(phase + c * p[4] / 360))) *
              rate * .001;
          double v = history.ago(delay)[c];
          next[c] = clean(x[c] + v * p[2]);
          y[c] = .5 * (x[c] + v);
        }
        history.push(next);
      }
      break;
    }
    case Kind::Phaser: {
      phase = frac(phase + p[0] * (p[6] > .5 ? clock.tempo / 60 : 1) / rate);
      constexpr unsigned counts[]{2, 4, 8, 12};
      for (unsigned c = 0; c < 2; ++c) {
        double hz = std::clamp(p[4] * std::exp2(3 * p[1] *
                                                sine(phase + c * p[5] / 360)),
                               20., rate * .4),
               w = std::tan(pi * hz / rate), a = (w - 1) / (w + 1),
               v = x[c] + feedback[c] * p[2];
        for (unsigned i = 0; i < counts[unsigned(p[3])]; ++i) {
          double out = a * v + allpass[c][i];
          allpass[c][i] = v - a * out;
          v = out;
        }
        feedback[c] = clean(v);
        y[c] = .5 * (x[c] + v);
      }
      break;
    }
    case Kind::Stereo: {
      phase = frac(phase + p[3] * (p[5] > .5 ? clock.tempo / 60 : 1) / rate);
      double pan = std::clamp(p[0] + p[4] * sine(phase), -1., 1.);
      double mid = (x[0] + x[1]) * .5, side = (x[0] - x[1]) * .5 * p[1];
      if (p[2] > 1.5)
        y = {mid * std::sqrt(1 - pan) + side, mid * std::sqrt(1 + pan) - side};
      else {
        y = {mid + side, mid - side};
        if (p[2] > .5) {
          y[0] *= std::min(1., 1 - pan);
          y[1] *= std::min(1., 1 + pan);
        } else {
          y[0] *= std::sqrt(1 - pan);
          y[1] *= std::sqrt(1 + pan);
        }
      }
      break;
    }
    case Kind::Delay: {
      double length = std::clamp(p[7] > .5 ? p[0] * 60 / clock.tempo * rate
                                           : p[8] * .001 * rate,
                                 1., double(history.samples.size() - 4));
      // A moving tap is smoothed to avoid a click when time is automated.
      if (read == 0)
        read = length;
      read += (length - read) * .001;
      y = history.ago(read);
      if (p[4] > 1.5) {
        auto b = history.ago(read * .75), c = history.ago(read * .5),
             d = history.ago(read * .25);
        for (unsigned k = 0; k < 2; ++k)
          y[k] = (y[k] + b[1 - k] * .75 + c[k] * .5 + d[1 - k] * .25) * .6;
      }
      Stereo next{};
      follow(peak, env, followerAttack, followerRelease);
      for (unsigned c = 0; c < 2; ++c) {
        double v = filters[c][1].tick(filters[c][0].tick(y[c]));
        double diff = -.5 * v + allpass[c][0];
        allpass[c][0] = v + .5 * diff;
        v = std::lerp(v, diff, p[5]);
        feedback[c] = v;
      }
      for (unsigned c = 0; c < 2; ++c) {
        next[c] = std::tanh(
            x[c] + feedback[p[4] > .5 && p[4] < 1.5 ? 1 - c : c] * p[1]);
        y[c] /= 1 + env * p[6] * 8;
      }
      history.push(next);
      break;
    }
    case Kind::Reverb: {
      predelay.push(x);
      auto input = predelay.ago(p[3] * rate * .001);
      std::array<double, 8> v{};
      double sum = 0;
      if (p[6] < .5) {
        frozen = false;
        panicFreeze = false;
      } else if (ticks > rate * .1) {
        frozen = true;
        panicFreeze = false;
      }
      double modeSize = p[4] < .5 ? .55 : p[4] > 1.5 && p[4] < 2.5 ? .8 : 1;
      for (unsigned i = 0; i < 8; ++i) {
        double len = reverbTimes[i] * p[1] * modeSize * rate;
        auto d = delays[i].ago(len);
        double value = (d[0] + d[1]) * .5;
        double &damp = allpass[0][i];
        damp = std::lerp(value, damp, p[2] * .98);
        v[i] = damp;
        sum += v[i];
      }
      Stereo shimmer{};
      if (p[4] > 3.5 && p[7] != 0) {
        const double second = frac(phase + .5),
                     window = .5 - .5 * sine(phase + .25);
        shimmer = mix(history.ago((.003 + .06 * (1 - second)) * rate),
                      history.ago((.003 + .06 * (1 - phase)) * rate), window);
      }
      y = {};
      for (unsigned i = 0; i < 8; ++i) {
        double fb = frozen ? .99998 : reverbFeedback[i];
        double out = (v[i] - sum * .25) * fb +
                     (frozen ? 0 : (input[0] + input[1]) * .18);
        if (p[4] > 3.5)
          out += shimmer[i % 2] * p[7] * .12;
        delays[i].push({clean(out), clean(out)});
        y[i % 2] += v[i] * .5;
      }
      phase = frac(phase + 1 / (rate * .06));
      history.push(y);
      follow(peak, env, followerAttack, followerRelease);
      for (auto &vout : y)
        vout /= 1 + env * p[5] * 8;
      if (p[4] > 2.5 && p[4] < 3.5 && env < .005)
        for (auto &vout : y)
          vout *= env / .005;
      break;
    }
    case Kind::Vocoder: {
      double note = clock.note >= 0 && p[7] > .5 ? clock.note : p[0];
      phase = frac(phase + noteHz(note) / rate);
      Stereo carrier{2 * phase - 1, 2 * phase - 1};
      if (p[6] > .5 && p[6] < 1.5)
        carrier = {phase < .5 ? 1. : -1., phase < .5 ? 1. : -1.};
      if (p[6] > 1.5)
        carrier = sc;
      y = {};
      const double attack = cachedPole(3), release = cachedPole(4);
      for (unsigned i = 0; i < 16; ++i) {
        double detection = 0;
        for (unsigned c = 0; c < 2; ++c)
          detection += std::abs(filters[c][i].tick(x[c])) * .5;
        follow(detection, envelopes[i], attack, release);
        for (unsigned c = 0; c < 2; ++c)
          y[c] += filters[c][16 + i].tick(carrier[c]) * envelopes[i] * 8;
      }
      for (unsigned c = 0; c < 2; ++c)
        y[c] += filters[c][32].tick(x[c]) * p[5];
      break;
    }
    default:
      break;
    }
    for (auto &v : y)
      v = clean(v);
    return y;
  }
};
Processor::Processor(Kind k, double r, bool s, unsigned seed)
    : m(std::make_unique<Impl>(k, r, s, seed)) {}
Processor::~Processor() = default;
Stereo Processor::process(Stereo x, Stereo sc, const Values &p,
                          const Clock &clock) noexcept {
  return m->run(x, sc, p, clock);
}
void Processor::reset() noexcept { m->reset(); }
unsigned Processor::latency() const noexcept { return m->latency; }
float Processor::fill() const noexcept { return float(m->fill); }
Kind Processor::kind() const noexcept { return m->kind; }
std::size_t Processor::memoryBytes() const noexcept {
  std::size_t bytes =
      sizeof(Impl) + (m->history.samples.size() + m->dry.samples.size() +
                      m->predelay.samples.size()) *
                         sizeof(std::array<float, 2>);
  for (auto &d : m->delays)
    bytes += d.samples.size() * sizeof(std::array<float, 2>);
  if (m->stretch)
    bytes += m->stretch->blockSamples() * 256;
  if (m->stft)
    bytes += m->stft->blockSamples() * 128;
  return bytes;
}
} // namespace daw::plugins::flux::dsp
