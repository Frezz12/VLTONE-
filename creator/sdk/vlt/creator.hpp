#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#define VLT_NODE __attribute__((annotate("vlt.node")))
#define VLT_PORT(id) __attribute__((annotate("vlt.port:" id)))

namespace vlt {
struct AudioFrame {
  float left = 0, right = 0;
};
inline AudioFrame operator*(AudioFrame a, float b) {
  return {a.left * b, a.right * b};
}
inline AudioFrame operator+(AudioFrame a, AudioFrame b) {
  return {a.left + b.left, a.right + b.right};
}
inline AudioFrame operator-(AudioFrame a, AudioFrame b) {
  return {a.left - b.left, a.right - b.right};
}
struct Context {
  double sampleRate = 48000, tempo = 120, beat = 0;
  std::int64_t sampleTime = 0;
  std::uint64_t seed = 0;
  unsigned channels = 2;
  bool playing = false;
};
struct PrepareContext {
  double sampleRate = 48000;
  unsigned channels = 2, maxBlockSize = 512;
  std::uint64_t seed = 0;
  unsigned latency = 0, tail = 0;
};
template <class Signature> class Function;
template <class Signature> struct FunctionTraits;
template <class R, class... A> struct FunctionTraits<R(A...)> {
  using Result = R;
};
template <class R, class... A>
struct FunctionTraits<R(A...) noexcept> : FunctionTraits<R(A...)> {};
template <unsigned Index, class Signature> struct FunctionArgument;
template <unsigned Index, class R, class A, class... Rest>
struct FunctionArgument<Index, R(A, Rest...)>
    : FunctionArgument<Index - 1, R(Rest...)> {};
template <class R, class A, class... Rest>
struct FunctionArgument<0, R(A, Rest...)> {
  using Type = A;
};
template <unsigned Index, class R, class... A>
struct FunctionArgument<Index, R(A...) noexcept>
    : FunctionArgument<Index, R(A...)> {};
template <class R, class... A> class Function<R(A...)> {
public:
  using Result = R;
  using Thunk = R (*)(void *, const Context &, A...);
  Function() = default;
  Function(void *state, const Context *context, Thunk thunk)
      : state_(state), context_(context), thunk_(thunk) {}
  R operator()(A... args) const { return thunk_(state_, *context_, args...); }

private:
  void *state_ = nullptr;
  const Context *context_ = nullptr;
  Thunk thunk_ = nullptr;
};
namespace detail {
extern "C"
    __attribute__((import_module("vlt"), import_name("reserve"))) unsigned
    reserve(unsigned bytes, unsigned alignment);
}
// Storage comes from the instance's preparation arena. reserve traps during
// DSP.
template <class T> class Buffer {
public:
  void prepare(unsigned count) {
    if (count > 16 * 1024 * 1024 / sizeof(T))
      __builtin_trap();
    data_ =
        reinterpret_cast<T *>(detail::reserve(count * sizeof(T), alignof(T)));
    count_ = count;
    clear();
  }
  unsigned size() const { return count_; }
  void clear() {
    for (unsigned i = 0; i < count_; ++i)
      data_[i] = T{};
  }
  T &operator[](unsigned i) {
    if (i >= count_)
      __builtin_trap();
    return data_[i];
  }
  const T &operator[](unsigned i) const {
    if (i >= count_)
      __builtin_trap();
    return data_[i];
  }

private:
  T *data_ = nullptr;
  unsigned count_ = 0;
};
inline float clamp(float x, float lo, float hi) {
  return std::clamp(x, lo, hi);
}
inline float tanh(float x) { return std::tanh(x); }
inline AudioFrame tanh(AudioFrame x) { return {tanh(x.left), tanh(x.right)}; }
inline float sin(float x) { return std::sin(x); }
inline float cos(float x) { return std::cos(x); }
inline float exp(float x) { return std::exp(x); }
inline float pow(float x, float y) { return std::pow(x, y); }
inline float mix(float a, float b, float t) { return a + (b - a) * t; }
inline AudioFrame mix(AudioFrame a, AudioFrame b, float t) {
  return a + (b - a) * t;
}
class Random {
public:
  void seed(std::uint64_t value) { value_ = value ? value : 1; }
  float next() {
    value_ ^= value_ << 13;
    value_ ^= value_ >> 7;
    value_ ^= value_ << 17;
    return float(value_ >> 40) * (1.f / 16777216.f);
  }

private:
  std::uint64_t value_ = 1;
};
} // namespace vlt
