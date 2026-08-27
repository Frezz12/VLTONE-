#pragma once

#include "Common/Types.hpp"
#include "DSP/Simd.hpp"
#include "Graph/Node.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace daw::engine {

/// A leaf that copies whatever reaches it and passes nothing on — the point a
/// stem is taken from during an offline render.
///
/// It is a leaf, not a link inserted into the chain, and that is the whole
/// design. Splicing a node into an existing edge would change the topology the
/// compiler sees, which moves latency and therefore PDC, so the stems would be
/// taken from a graph that is not quite the mix. Hanging a leaf off a node that
/// already has consumers adds an edge and nothing else: the signal path, its
/// latency and the compiled order of everything else are untouched, so the mix
/// rendered in the same pass is bit-identical to the mix rendered without taps.
/// `AudioGraph::topologicalOrder` schedules every live node rather than only
/// those reachable from the sink, so a leaf really does get processed.
///
/// The captured block stays in the node. The controller drains it after each
/// block completes, which keeps file writing off the worker threads — a `sink`
/// called from `process` would have several stems allocating and blocking on
/// disk inside the scheduler pass, in whatever order the stealing happened to
/// produce.
class TapNode : public Node {
public:
    explicit TapNode(std::string name = "Tap") : m_name(std::move(name)) {}

    std::string_view name() const noexcept override { return m_name; }
    MidiNodeRole midiRole() const noexcept override { return MidiNodeRole::None; }

    void prepare(const PrepareInfo& info) override {
        m_channels = info.channels;
        m_capacity = info.maxBlockSize;
        m_storage.assign(std::size_t(m_channels) * m_capacity, 0.0f);
        m_pointers.resize(m_channels);
        for (ChannelCount channel = 0; channel < m_channels; ++channel) {
            m_pointers[channel] = m_storage.data() + std::size_t(channel) * m_capacity;
        }
        m_frames = 0;
    }

    void reset() override {
        std::fill(m_storage.begin(), m_storage.end(), 0.0f);
        m_frames = 0;
    }

    void process(const ProcessContext& context) override {
        m_frames = 0;
        if (m_channels == 0 || context.frames > m_capacity) return;

        // Sum rather than copy: a tap point is an ordinary node output, and a
        // node with several producers presents them as separate input blocks.
        AudioBlock capture(m_pointers.data(), m_channels, context.frames);
        dsp::sumInto(capture, context.inputs);
        m_frames = context.frames;

        // The node's own output buffer is never read by anyone — nothing
        // consumes a leaf — but the arena hands one out regardless, and a
        // successor-less buffer left undefined would still be recycled into
        // another node later. Clearing it costs one block and keeps the arena's
        // contents defined.
        for (ChannelCount channel = 0; channel < context.output.numChannels(); ++channel) {
            dsp::clear(context.output.channel(channel).first(context.frames));
        }
    }

    /// Planar pointers to the last block this tap saw. Valid between the end of
    /// one graph pass and the start of the next.
    const float* const* captured() const noexcept { return m_pointers.data(); }
    FrameCount capturedFrames() const noexcept { return m_frames; }
    ChannelCount capturedChannels() const noexcept { return m_channels; }

private:
    std::string m_name;
    std::vector<float> m_storage;
    std::vector<float*> m_pointers;
    ChannelCount m_channels = 0;
    FrameCount m_capacity = 0;
    FrameCount m_frames = 0;
};

} // namespace daw::engine
