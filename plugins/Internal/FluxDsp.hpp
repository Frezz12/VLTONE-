#pragma once
#include "Internal/FluxProgram.hpp"
#include <algorithm>
#include <cmath>
#include <memory>

namespace daw::plugins::flux::dsp {
using Stereo = std::array<double, 2>;
using Values = std::array<double, fieldCount>;
inline double clean(double x) noexcept {
  return std::isfinite(x) ? std::clamp(x, -32., 32.) : 0.;
}
double gain(double db) noexcept;
double pole(double seconds, double rate) noexcept;
double sine(double cycles) noexcept;
struct Biquad {
  double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
  void tune(unsigned mode, double hz, double q, double db,
            double rate) noexcept;
  double tick(double x) noexcept;
  void reset() noexcept { z1 = z2 = 0; }
};
// Invalidation is O(1): old samples cannot be read after a transport reset.
struct History {
  std::vector<std::array<float, 2>> samples;
  std::uint64_t total = 0, available = 0;
  std::size_t cursor = 0;
  void prepare(unsigned capacity);
  void reset() noexcept { total = available = cursor = 0; }
  void push(Stereo) noexcept;
  Stereo absolute(double position) const noexcept;
  Stereo ago(double delay) const noexcept;
};
struct Crossover {
  // Cascaded Butterworth pairs make LR4; low branch receives the second
  // crossover's all-pass, so all three bands have the same summed phase.
  std::array<std::array<Biquad, 12>, 2> filters;
  void tune(double low, double high, double rate) noexcept;
  std::array<Stereo, 3> split(Stereo) noexcept;
  void reset() noexcept;
};
struct Clock {
  double rate = 48000, tempo = 120, beat = 0, barBeats = 4;
  std::int64_t sample = 0;
  unsigned seed = 1;
  int note = -1;
  double velocity = 1;
  bool retrigger = false;
};
class Processor {
public:
  Processor(Kind, double rate, bool studio, unsigned seed);
  ~Processor();
  Processor(const Processor &) = delete;
  Processor &operator=(const Processor &) = delete;
  Stereo process(Stereo input, Stereo sidechain, const Values &,
                 const Clock &) noexcept;
  void reset() noexcept;
  unsigned latency() const noexcept;
  float fill() const noexcept;
  std::size_t memoryBytes() const noexcept;
  Kind kind() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> m;
};
} // namespace daw::plugins::flux::dsp
