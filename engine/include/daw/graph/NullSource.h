#pragma once

#include <cstring>

#include "daw/graph/ProcessNode.h"

namespace daw::graph {

class NullSource : public ProcessNode {
public:
    void process(ProcessContext& ctx) noexcept override
    {
        for (int b = 0; b < ctx.numOutputs; ++b) {
            ctx.outputs[b].clear();
        }
    }

    int numInputBuses() const noexcept override { return 0; }
    int numOutputBuses() const noexcept override { return 1; }
    int numChannelsForOutputBus(int) const noexcept override { return 2; }
};

} // namespace daw::graph
