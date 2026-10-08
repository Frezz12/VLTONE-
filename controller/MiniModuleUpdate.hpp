#pragma once
#include "AudioMiniModuleCompiler.hpp"
#include "model/Document.hpp"
#include <memory>
#include <string>
#include <vector>

namespace daw {
// Captured on the controller thread, prepared on a worker, published back on
// the controller thread. The worker never touches the live project or DSP.
struct MiniModuleUpdate {
  MiniModuleUpdate() = default;
  MiniModuleUpdate(const MiniModuleUpdate&);
  MiniModuleUpdate& operator=(const MiniModuleUpdate&) = delete;
  ~MiniModuleUpdate();
  struct Target {
    std::string channel;
    InsertModel before, after;
    unsigned channels = 2;
    bool audioChanged = true;
    std::uint64_t instance = 0;
    bool prepared = false;
    engine::FrameCount preparedLatency = 0;
  };
  plugins::mini::MiniModuleDefinition definition;
  std::string projectId, error;
  engine::PrepareInfo info;
  std::vector<Target> targets;
  std::weak_ptr<AudioMiniModuleCompiler> compiler;
  std::uint64_t preparationId = 0;
  // A reverse transaction targets exact instances and restores their own
  // definitions (which need not all have been the same version).
  bool exactRestore = false;
  bool prepare();
};
} // namespace daw
