#include "daw/graph/ProcessGraph.h"

namespace daw::graph {

void ProcessGraph::process(ProcessContext& ctx) const noexcept
{
    if (steps_.empty())
        return;

    for (auto& step : steps_) {
        // Собираем входные view из scratch-буферов. Маccив на cтеке — без
        // аллокаций в аудио-потоке. Чиcло входов ограничено kMaxNodeInputs.
        audio::AudioBufferView inputViews[kMaxNodeInputs];
        int numInputs = 0;
        for (int idx : step.inputBufferIndices) {
            if (numInputs >= kMaxNodeInputs)
                break;
            if (idx >= 0)
                inputViews[numInputs++] = scratch_[idx].view(ctx.numFrames);
        }

        audio::AudioBufferView nodeOutput;
        if (step.outputBufferIndex >= 0) {
            nodeOutput = scratch_[step.outputBufferIndex].view(ctx.numFrames);
        } else if (ctx.numOutputs > 0) {
            nodeOutput = ctx.outputs[0];
        } else {
            continue;
        }

        audio::AudioBufferView outputArr[] = {nodeOutput};

        ProcessContext nodeCtx;
        nodeCtx.inputs = numInputs > 0 ? inputViews : nullptr;
        nodeCtx.outputs = outputArr;
        nodeCtx.numInputs = numInputs;
        nodeCtx.numOutputs = 1;
        nodeCtx.numFrames = ctx.numFrames;
        nodeCtx.playPosition = ctx.playPosition;
        nodeCtx.playing = ctx.playing;

        if (step.node)
            step.node->process(nodeCtx);
    }
}

} // namespace daw::graph
