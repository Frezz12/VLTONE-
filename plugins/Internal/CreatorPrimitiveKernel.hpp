#pragma once
#include <array>
#include "MiniNodeRegistry.hpp"
#include "Host/PluginTypes.hpp"
#include <memory>
#include <span>

namespace daw::plugins::mini {
struct DelayMemory;
struct PrimitiveValue {
  double left = 0, right = 0;
  const double *elements = nullptr;
  unsigned size = 0, capacity = 0;
  DelayMemory *buffer = nullptr;
  unsigned position = 0;
  std::uint64_t generation = 0;
};
struct PrimitiveInput {
  std::string node, port, fromNode, fromPort;
  PortType type = PortType::Number;
  unsigned capacity = 0;
  PrimitiveValue value;
};
struct PrimitiveOutput {
  std::string node, port;
  PortType type = PortType::Number;
  unsigned capacity = 0;
  const PrimitiveValue *value = nullptr;
};
struct PrimitiveContext {
  double sampleRate = 48000, time = 0, tempo = 120, beat = 0;
  bool playing = false;
  unsigned channels = 2;
  PrimitiveValue audio;
  std::array<double, 2> controls{};
};
// No graph traversal, allocation, strings or host calls in tick/commit/reset.
// One instance owns one convex island, including all of its feedback state.
class CreatorPrimitiveKernel {
public:
  CreatorPrimitiveKernel();
  ~CreatorPrimitiveKernel();
  bool prepare(const MiniModuleDefinition &, std::span<const unsigned> nodes,
               double sampleRate, unsigned maxFrames, unsigned channels,
               std::uint64_t seed, std::size_t &memory, std::string &error,
               unsigned depth = 0);
  std::span<PrimitiveInput> inputs();
  std::span<const PrimitiveOutput> outputs() const;
  bool tick(const PrimitiveContext &) noexcept;
  void commit() noexcept;
  void reset() noexcept;
  bool retainDelayHistory(unsigned samples, std::size_t &memory, std::string &error);
  bool tailKnown() const noexcept;
  unsigned tail() const noexcept;
  unsigned latency() const noexcept;
  std::string error() const;
private:
  struct Impl;
  std::unique_ptr<Impl> m;
};
bool reserveGraphMemory(std::size_t bytes, std::size_t &used, std::string &error);
} // namespace daw::plugins::mini
