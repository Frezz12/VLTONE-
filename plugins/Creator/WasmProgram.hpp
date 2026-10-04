#pragma once
#include "Graph/Node.hpp"
#include "Host/PluginTypes.hpp"
#include "Internal/MiniModuleDefinition.hpp"
#include <memory>

namespace daw::plugins::mini {
class WasmProgram {
public:
  WasmProgram();
  ~WasmProgram();
  bool prepare(const MiniModuleDefinition &, const PluginProcessInfo &,
               unsigned channels, std::uint64_t seed, std::string &error);
  float *input(unsigned node, unsigned port) noexcept;
  const float *output(unsigned node, unsigned port) const noexcept;
  unsigned latency(unsigned node) const noexcept;
  unsigned tail(unsigned node) const noexcept;
  bool render(unsigned node, const engine::ProcessContext &) noexcept;
  void reset() noexcept;
  bool failed() const noexcept;
  std::string error() const;
  bool takeError(std::string &);
  static unsigned realtimeMemoryOperations() noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> m;
};
} // namespace daw::plugins::mini
