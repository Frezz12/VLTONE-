#pragma once
#include "Host/PluginInstance.hpp"
#include "MiniModuleDefinition.hpp"
#include <memory>

namespace daw::plugins::mini {
// A compiled typed graph. Construction/preparation/destruction belong to the
// control thread; render, controls and reset use only prepared storage.
class CreatorDspRuntime {
public:
  CreatorDspRuntime();
  ~CreatorDspRuntime();
  bool prepare(const MiniModuleDefinition &, const PluginProcessInfo &,
               unsigned channels, std::uint64_t seed, std::string &error);
  void setControl(unsigned, double) noexcept;
  bool render(const PluginProcessContext &, unsigned offset,
              unsigned frames) noexcept;
  void reset() noexcept;
  unsigned latency() const noexcept;
  unsigned tail() const noexcept;
  bool takeError(std::string &);

private:
  struct Impl;
  std::unique_ptr<Impl> m;
};
} // namespace daw::plugins::mini
