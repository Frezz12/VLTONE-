#pragma once

#include "AudioPluginSpec.hpp"
#include "PluginReadout.hpp"
#include "Graph/Node.hpp"

namespace daw {

struct AudioMiniModuleCompileRequest {
    struct Target { AudioPluginAddress address; AudioPluginSpec spec; };
    engine::PrepareInfo info;
    std::vector<Target> targets;
};
struct AudioMiniModuleCompileStatus {
    enum class State { Missing, Pending, Ready, Failed };
    struct Target { AudioPluginAddress address; engine::FrameCount latency = 0; };
    State state = State::Missing;
    std::string error;
    std::vector<Target> targets;
};

/// A value-only endpoint. The audio runtime owns compilation jobs and prepared
/// processors; UI workers can retain only this weak service and numeric IDs.
class AudioMiniModuleCompiler {
public:
    virtual ~AudioMiniModuleCompiler() = default;
    virtual std::uint64_t start(AudioMiniModuleCompileRequest request) = 0;
    virtual AudioMiniModuleCompileStatus poll(std::uint64_t id) = 0;
    virtual void forget(std::uint64_t id) = 0;
};

} // namespace daw
