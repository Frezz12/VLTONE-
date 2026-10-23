#pragma once

#include "AudioRuntimeState.hpp"
#include "Graph/NodeDiagnostics.hpp"

namespace daw {

struct AudioPerformanceRow {
    enum class Kind { Processing, Instrument, Insert, SamplerFx, ClipFx, MiniModule };
    Kind kind = Kind::Processing;
    std::uint64_t id = 0;
    AudioPluginAddress address;
    std::string name, clipId;
    plugins::Format format = plugins::Format::Unknown;
    AudioPluginRuntimeStatus health;
    engine::NodeTimingSnapshot timing;
    std::uint32_t latencySamples = 0, pathLatencySamples = 0;
    bool inGraph = false, bypassed = false;
};

struct AudioPerformanceSnapshot {
    std::uint64_t timestampNs = 0, generation = 0;
    std::uint32_t totalLatencySamples = 0, graphNodes = 0, graphEdges = 0;
    std::vector<AudioPerformanceRow> rows;
};

inline bool processingUnresponsive(const engine::NodeTimingSnapshot& timing, std::uint64_t now) {
    // A heuristic, not a claim that a vendor call can safely be killed in-process.
    return timing.activeSinceNs > timing.completedAtNs && now > timing.activeSinceNs &&
        now - timing.activeSinceNs > 2'000'000'000ull;
}

} // namespace daw
