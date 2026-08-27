#pragma once

#include <cstring>

#include "daw/graph/ProcessNode.h"

namespace daw::graph {

class SummingBus : public ProcessNode {
public:
    void process(ProcessContext& ctx) noexcept override
    {
        if (ctx.numOutputs < 1)
            return;

        auto& out = ctx.outputs[0];
        const int channels = out.numChannels();

        out.clear();

        for (int b = 0; b < ctx.numInputs; ++b) {
            auto& in = ctx.inputs[b];
            const int ch = std::min(in.numChannels(), channels);
            for (int c = 0; c < ch; ++c) {
                const float* src = in.channel(c);
                float* dst = out.channel(c);
                for (int i = 0; i < ctx.numFrames; ++i)
                    dst[i] += src[i];
            }
        }
    }

    int numInputBuses() const noexcept override
    {
        return maxInputs_;
    }

    int numOutputBuses() const noexcept override { return 1; }

    int numChannelsForInputBus(int) const noexcept override { return 2; }

    int numChannelsForOutputBus(int) const noexcept override { return 2; }

    void setMaxInputs(int n) noexcept { maxInputs_ = n; }

private:
    int maxInputs_ = 32;
};

} // namespace daw::graph
