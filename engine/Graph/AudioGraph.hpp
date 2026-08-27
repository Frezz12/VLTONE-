#pragma once

#include "Common/Types.hpp"
#include "Graph/Node.hpp"
#include "Memory/BufferPool.hpp"
#include "Midi/MidiEvent.hpp"

#include <atomic>
#include <memory>
#include <vector>

namespace daw::engine {

/// A fixed-length delay line used for automatic latency compensation on one
/// edge. Sized once at compile time; the realtime path only reads and writes.
class EdgeDelay {
public:
    void prepare(ChannelCount channels, FrameCount delaySamples,
                 FrameCount maxBlockSize);
    /// Push `input`, read the delayed signal into `output`.
    void process(const AudioBlock& input, const AudioBlock& output,
                 FrameCount frames) noexcept;
    void reset() noexcept;

    FrameCount delaySamples() const noexcept { return m_delay; }

private:
    std::vector<float> m_storage;   // channel-major ring
    std::size_t m_stride = 0;
    std::size_t m_writePos = 0;
    ChannelCount m_channels = 0;
    FrameCount m_delay = 0;
};

/// One node's outstanding-input counter, on a cache line of its own so eight
/// workers decrementing neighbouring counters do not fight over one line.
struct alignas(kCacheLine) PendingCounter {
    std::atomic<std::uint32_t> value{0};
};

/// The immutable, realtime-safe result of compiling a graph.
///
/// Nothing in here is ever mutated by the audio thread except the per-block
/// scratch at the bottom — the dependency counters, reset from
/// `pendingTemplate` at the start of each block, and the resolved input views.
/// Publication is a single atomic pointer swap, so re-routing never blocks the
/// audio thread.
struct CompiledGraph {
    struct Edge {
        std::uint32_t producer = 0;      // index into nodes
        std::uint32_t delayIndex = kInvalidNode;  // into delays, or none
        /// Compact producer MIDI buffer, or none when this edge cannot carry it.
        std::uint32_t midiBuffer = kInvalidNode;
        std::uint32_t midiDelayIndex = kInvalidNode; // into midiDelays, or none
        std::uint32_t scratchBuffer = 0; // buffer holding the delayed signal
        InputRole role = InputRole::Main;
    };

    struct CompiledNode {
        Node* node = nullptr;
        /// The snapshot co-owns its nodes: a rebuild on the control thread must
        /// not destroy DSP the audio thread is still rendering through.
        std::shared_ptr<Node> owner;
        NodeId id = kInvalidNode;   // handle in the editable graph
        std::uint32_t outputBuffer = 0;
        std::uint32_t midiOutputBuffer = kInvalidNode;
        std::uint32_t firstInput = 0;
        std::uint32_t inputCount = 0;
        std::uint32_t firstSuccessor = 0;
        std::uint32_t successorCount = 0;
        std::uint32_t dependencies = 0;  // in-degree
        FrameCount latency = 0;          // cumulative, from the sources
    };

    std::vector<CompiledNode> nodes;
    std::vector<Edge> inputEdges;          // grouped per node
    /// Index-parallel with `inputEdges`; kept contiguous because an enum member
    /// inside Edge cannot be exposed as a span across the struct stride.
    std::vector<InputRole> inputRoles;
    std::vector<std::uint32_t> successors; // grouped per node
    std::vector<std::uint32_t> order;      // topological, for serial/offline
    std::vector<std::uint32_t> roots;      // nodes with no dependencies
    std::vector<std::uint32_t> pendingTemplate;
    /// Largest dependency level: a conservative, compile-time lower bound on
    /// how many nodes can be ready together. Used to avoid treating every
    /// graph below a fixed node count as serial work.
    std::uint32_t parallelWidth = 0;
    /// Shared rather than unique so a recompile can carry a delay line over
    /// from the previous snapshot instead of restarting it from silence.
    std::vector<std::shared_ptr<EdgeDelay>> delays;
    /// Only delayed edges that actually carry MIDI get one. The edge stores its
    /// compact index; audio-only PDC paths allocate no event queue.
    std::vector<std::shared_ptr<MidiDelay>> midiDelays;

    BufferArena arena;
    std::uint32_t sinkNode = kInvalidNode;
    FrameCount totalLatency = 0;
    ChannelCount channels = 2;
    FrameCount maxBlockSize = 512;
    /// The rate the graph was compiled for, carried into every ProcessContext.
    SampleRate sampleRate = 48000.0;
    std::uint64_t generation = 0;

    // ── Per-block scratch ──
    //
    // Sized here, at compile time, so the renderer only ever indexes into it.
    // It lives in the snapshot rather than in the processor because the two
    // must never disagree: when they were separate members the audio thread
    // could load a new graph and index it with the previous graph's scratch.
    // `mutable` because the snapshot is published const — these belong to
    // whichever thread is currently rendering it, and only one may be.
    mutable std::vector<PendingCounter> pending;
    mutable std::vector<AudioBlock> inputScratch;  // resolved per-node inputs
    /// One MIDI buffer per node that can produce MIDI, not per graph node.
    /// Audio-only built-ins opt out explicitly; capable nodes retain storage even
    /// without upstream MIDI because generators may originate events themselves.
    mutable std::vector<MidiBuffer> midiBuffers;
    /// Where a delayed MIDI edge lands, index-parallel with `midiDelays`.
    mutable std::vector<MidiBuffer> midiDelayBuffers;
    mutable std::vector<const MidiBuffer*> midiInputScratch;   // per input edge
};

/// The editable graph. Nodes are owned here; edges are (producer → consumer)
/// with an explicit input slot order so summing is deterministic.
///
/// Editing happens on the control thread. `compile()` produces a snapshot that
/// the processor swaps in atomically — the audio thread never sees a partially
/// rebuilt graph, and an unaffected part of the project keeps its buffers and
/// DSP state because the same Node objects are carried over.
class AudioGraph {
public:
    AudioGraph() = default;

    /// Take ownership of a node and return its handle.
    NodeId addNode(std::unique_ptr<Node> node);
    /// Adopt a node that already exists — this is how a rebuild keeps the DSP
    /// state (filter memories, meters, loaded clips) of the parts that did not
    /// change, instead of rebuilding the world on every edit.
    NodeId adoptNode(std::shared_ptr<Node> node);
    Status removeNode(NodeId id);
    Node* node(NodeId id) noexcept;
    const Node* node(NodeId id) const noexcept;
    std::size_t nodeCount() const noexcept { return m_nodes.size(); }

    /// Route `producer` into `consumer`. Multiple producers may feed the same
    /// consumer; it receives them in connection order.
    Status connect(NodeId producer, NodeId consumer,
                   InputRole role = InputRole::Main);
    Status disconnect(NodeId producer, NodeId consumer);
    /// Drop every connection into and out of a node (used when re-routing it).
    void disconnectAll(NodeId id);

    /// The node whose output leaves the engine (the master).
    void setSink(NodeId id) noexcept { m_sink = id; }
    NodeId sink() const noexcept { return m_sink; }

    /// Compile into a realtime-safe snapshot: cycle check, topological order,
    /// buffer assignment with reuse, and automatic delay compensation.
    ///
    /// Pass the currently published snapshot as `previous` and any compensation
    /// delay line whose (producer, consumer, length) is unchanged is carried
    /// over with its contents intact. Without that every project edit restarts
    /// every compensated path from silence, which is audible as a click on a
    /// graph that has any latency in it at all.
    Result<std::shared_ptr<const CompiledGraph>> compile(
        const PrepareInfo& info, const CompiledGraph* previous = nullptr);

    /// True when the graph changed since the last compile.
    bool isDirty() const noexcept { return m_dirty; }

private:
    struct Connection {
        NodeId producer = kInvalidNode;
        InputRole role = InputRole::Main;
    };
    struct Slot {
        std::shared_ptr<Node> node;
        std::vector<Connection> inputs; // producers and bus roles, in order
        std::vector<NodeId> outputs;   // consumers
        bool alive = false;
    };

    Result<std::vector<NodeId>> topologicalOrder() const;

    std::vector<Slot> m_nodes;
    std::vector<NodeId> m_freeSlots;
    NodeId m_sink = kInvalidNode;
    bool m_dirty = true;
    std::uint64_t m_generation = 0;
};

} // namespace daw::engine
