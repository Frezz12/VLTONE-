#pragma once

#include "daw/graph/ProcessContext.h"

namespace daw::graph {

class ProcessNode {
public:
    virtual ~ProcessNode() = default;

    virtual void process(ProcessContext& ctx) noexcept = 0;

    virtual void prepare(double /*sampleRate*/, int /*maxBlockSize*/) {}

    virtual void reset() noexcept {}

    virtual int latencySamples() const noexcept { return 0; }

    virtual int tailSamples() const noexcept { return 0; }

    virtual int numInputBuses() const noexcept { return 1; }

    virtual int numOutputBuses() const noexcept { return 1; }

    virtual int numChannelsForInputBus(int /*bus*/) const noexcept { return 2; }

    virtual int numChannelsForOutputBus(int /*bus*/) const noexcept { return 2; }
};

} // namespace daw::graph
