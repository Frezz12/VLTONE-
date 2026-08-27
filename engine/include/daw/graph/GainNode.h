#pragma once

#include <atomic>
#include <cmath>

#include "daw/graph/ProcessNode.h"

namespace daw::graph {

class GainNode : public ProcessNode {
public:
    void setGain(float linear) noexcept
    {
        targetGain_.store(linear, std::memory_order_relaxed);
    }

    float gain() const noexcept
    {
        return targetGain_.load(std::memory_order_relaxed);
    }

    void prepare(double sampleRate, int /*maxBlockSize*/) noexcept override
    {
        smoothing_ = static_cast<float>(
            std::exp(-1.0 / (sampleRate * 0.010)));
        currentGain_ = targetGain_.load(std::memory_order_relaxed);
    }

    void process(ProcessContext& ctx) noexcept override
    {
        if (ctx.numInputs < 1 || ctx.numOutputs < 1)
            return;

        auto& in = ctx.inputs[0];
        auto& out = ctx.outputs[0];

        const int ch = std::min(in.numChannels(), out.numChannels());
        if (ch == 0)
            return;

        const float target = targetGain_.load(std::memory_order_relaxed);

        for (int c = 0; c < ch; ++c) {
            const float* src = in.channel(c);
            float* dst = out.channel(c);
            float g = currentGain_;

            for (int i = 0; i < ctx.numFrames; ++i) {
                g = target + (g - target) * smoothing_;
                dst[i] = src[i] * g;
            }

            if (c == 0)
                currentGain_ = g;
        }
    }

    int numInputBuses() const noexcept override { return 1; }
    int numOutputBuses() const noexcept override { return 1; }
    int numChannelsForInputBus(int) const noexcept override { return 2; }
    int numChannelsForOutputBus(int) const noexcept override { return 2; }

private:
    std::atomic<float> targetGain_{1.0f};
    float currentGain_ = 1.0f;
    float smoothing_ = 0.999f;
};

} // namespace daw::graph
