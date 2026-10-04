#pragma once

#include "Graph/AudioGraph.hpp"
#include "Transport/Transport.hpp"

#include <functional>
#include <optional>

namespace daw::engine {

/// Zero means ineligible; otherwise the number of retained temporal slots.
std::size_t offlinePipelineWindow(const CompiledGraph& graph, unsigned workers);

/// Runs an exclusively owned offline graph through a bounded temporal window.
/// nullopt means the graph is ineligible and no DSP or sink was called.
/// The service callback runs on the caller, with that node's DSP parked.
std::optional<Status> renderOfflinePipelined(
    const CompiledGraph& graph, unsigned workers,
    SamplePos start, SamplePos end, FrameCount blockSize, SamplePos sourcesEnd,
    const Transport& transport,
    const std::function<bool(const AudioBlock&, FrameCount)>& sink,
    const std::function<Status(std::uint32_t, SamplePos)>& service,
    std::span<const double> nodeCosts = {});

} // namespace daw::engine
