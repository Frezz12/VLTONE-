#pragma once

#include "daw/graph/ProcessNode.h"

namespace daw::graph {

class AudioOutputNode : public ProcessNode {
public:
    void process(ProcessContext& ctx) noexcept override
    {
        if (ctx.numInputs < 1 || ctx.numOutputs < 1)
            return;

        auto& in = ctx.inputs[0];
        auto& out = ctx.outputs[0];

        const int ch = std::min(in.numChannels(), out.numChannels());
        if (ch == 0)
            return;

        for (int c = 0; c < ch; ++c) {
            const float* src = in.channel(c);
            float* dst = out.channel(c);
            for (int i = 0; i < ctx.numFrames; ++i)
                dst[i] += src[i];
        }
    }

    int numInputBuses() const noexcept override { return 1; }
    int numOutputBuses() const noexcept override { return 1; }
    int numChannelsForInputBus(int) const noexcept override { return 2; }
    int numChannelsForOutputBus(int) const noexcept override { return 2; }
};

} // namespace daw::graph
