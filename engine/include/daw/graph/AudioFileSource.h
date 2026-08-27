#pragma once

#include <cstdint>

#include "daw/graph/ProcessNode.h"
#include "daw/model/Clip.h"

namespace daw::graph {

class AudioFileSource : public ProcessNode {
public:
    explicit AudioFileSource(std::shared_ptr<model::Clip> clip)
        : clip_(std::move(clip)) {}

    void setClip(std::shared_ptr<model::Clip> clip) { clip_ = std::move(clip); }
    const model::Clip* clip() const noexcept { return clip_.get(); }

    void process(ProcessContext& ctx) noexcept override;

    int numInputBuses() const noexcept override { return 0; }
    int numOutputBuses() const noexcept override { return 1; }
    int numChannelsForOutputBus(int /*bus*/) const noexcept override
    {
        return clip_ && clip_->source()
             ? clip_->source()->channels()
             : 2;
    }

private:
    std::shared_ptr<model::Clip> clip_;
};

} // namespace daw::graph
