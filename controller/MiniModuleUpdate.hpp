#pragma once
#include "Host/PluginNode.hpp"
#include "model/Document.hpp"
#include <memory>
#include <string>
#include <vector>

namespace daw {
// Captured on the controller thread, prepared on a worker, published back on
// the controller thread. The worker never touches the live project or DSP.
struct MiniModuleUpdate {
  struct Target {
    std::string channel;
    InsertModel before, after;
    unsigned channels = 2;
    bool audioChanged = true;
    std::shared_ptr<plugins::PluginNode> prepared;
  };
  plugins::mini::MiniModuleDefinition definition;
  std::string projectId, error;
  engine::PrepareInfo info;
  std::vector<Target> targets;
  bool prepare();
};
} // namespace daw
