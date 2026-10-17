#pragma once

#include "Graph/AudioGraph.hpp"

namespace daw::engine {

/// Live playback and pipelined export use different arenas, but the same
/// edge, MIDI, PDC and transport rules. Views refer to preallocated storage;
/// each scheduled node owns a disjoint range of the scratch arrays.
struct GraphProcessBuffers {
    std::span<AudioBlock> inputs;
    std::span<const MidiBuffer*> midiInputs;
    std::span<const MidiBuffer> midi;
};

template <typename InputBuffer, typename DelayBuffer>
void prepareGraphProcessContext(
    const CompiledGraph& graph, const CompiledGraph::CompiledNode& entry,
    ProcessContext& context, GraphProcessBuffers buffers,
    InputBuffer inputBuffer, DelayBuffer delayBuffer, SamplePos sourcesEnd) noexcept {
    for (std::uint32_t i = 0; i < entry.inputCount; ++i) {
        const auto input = entry.firstInput + i;
        const auto& edge = graph.inputEdges[input];
        auto audio = inputBuffer(edge.producer);
        const MidiBuffer* midi = edge.midiBuffer != kInvalidNode
            ? &buffers.midi[edge.midiBuffer] : nullptr;
        if (edge.delayIndex != kInvalidNode) {
            const auto delayed = delayBuffer(input);
            graph.delays[edge.delayIndex]->process(audio, delayed, context.frames);
            audio = delayed;
            if (edge.midiDelayIndex != kInvalidNode && midi) {
                auto& delayedMidi = graph.midiDelayBuffers[edge.midiDelayIndex];
                graph.midiDelays[edge.midiDelayIndex]->process(*midi, delayedMidi, context.frames);
                midi = &delayedMidi;
            } else midi = nullptr;
        }
        buffers.inputs[input] = audio;
        buffers.midiInputs[input] = midi;
    }
    context.inputs = buffers.inputs.subspan(entry.firstInput, entry.inputCount);
    context.inputRoles = std::span(graph.inputRoles).subspan(entry.firstInput, entry.inputCount);
    context.midiInputs = buffers.midiInputs.subspan(entry.firstInput, entry.inputCount);
    context.midiOutput = entry.midiOutputBuffer != kInvalidNode
        ? &graph.midiBuffers[entry.midiOutputBuffer] : nullptr;
    if (context.midiOutput) context.midiOutput->clear();
    context.sampleRate = graph.sampleRate;
    context.compensateInputLatency(entry.inputLatency, sourcesEnd);
}

} // namespace daw::engine
