#pragma once

#include "Graph/AudioGraph.hpp"
#include "Job/JobSystem.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace daw::engine {

/// Runs a compiled graph over the job system.
///
/// The schedule is dependency-driven, not track-driven: every node whose inputs
/// are all finished becomes a job, and any idle core picks up the next one. A
/// project of a thousand tracks therefore fills every core without anybody
/// assigning tracks to threads, and a deep chain (synth → EQ → comp → limiter →
/// bus → master) still runs in the right order.
class GraphProcessor {
public:
    explicit GraphProcessor(unsigned threadCount = 0);
    ~GraphProcessor();

    /// Publish a freshly compiled graph. Control thread; the audio thread picks
    /// it up on its next block through one atomic load.
    void setGraph(std::shared_ptr<const CompiledGraph> graph);
    std::shared_ptr<const CompiledGraph> graph() const;

    /// True when publishing this graph would have to grow the job system's
    /// deques — a reallocation that must not happen while a pass is running.
    /// The caller parks the renderer for that case and only for that case.
    bool publishNeedsRenderStopped(const CompiledGraph& graph) const noexcept {
        return m_jobs.needsGrowthFor(graph.nodes.size());
    }

    /// Render one block into `output`. Realtime-safe: no allocation, no locks.
    /// On success every channel of `output` has been written, so the caller does
    /// not need to clear it first; on failure `output` is left untouched.
    ///
    /// `offline` is passed on to every node as `ProcessContext::offline`: it
    /// says the realtime deadline is lifted (mixdown, freeze, bounce), not that
    /// anything about the signal changes. The rendered samples must stay
    /// identical either way.
    ///
    /// `transport` is musical time for the block, handed to every node. It
    /// defaults to a stopped 120 BPM 4/4 so tests and tools that only care
    /// about samples need not build one.
    Status process(const AudioBlock& output, FrameCount frames,
                   SamplePos timelinePosition, bool playing,
                   bool offline = false,
                   const TransportInfo& transport = TransportInfo{});

    /// Single-threaded rendering in topological order. Used as the reference
    /// for tests and for graphs too small to be worth waking the pool.
    Status processSerial(const AudioBlock& output, FrameCount frames,
                         SamplePos timelinePosition, bool playing,
                         bool offline = false,
                         const TransportInfo& transport = TransportInfo{});

    /// Baseline node-count crossover for the pool. Smaller graphs may still run
    /// in parallel when compilation found a wide enough independent frontier;
    /// this preserves parallelism for compact graphs containing expensive DSP.
    void setParallelThreshold(std::uint32_t nodes) noexcept {
        m_parallelThreshold = nodes;
    }

    unsigned workerCount() const noexcept { return m_jobs.workerCount(); }
    FrameCount latencySamples() const;

private:
    static void executeJob(void* context, std::uint32_t nodeIndex,
                           unsigned workerIndex) noexcept;
    void runNode(const CompiledGraph& graph, std::uint32_t nodeIndex,
                 unsigned workerIndex) noexcept;
    void prepareBlockState(const CompiledGraph& graph) noexcept;
    void prepareMidiTimeline(const CompiledGraph& graph, FrameCount frames,
                             SamplePos timelinePosition, bool playing,
                             bool offline) noexcept;
    /// Resolve one node's inputs into the snapshot's scratch and build the
    /// context it will be processed with. Both the parallel and the serial path
    /// go through here: when they each built their own context the two drifted
    /// apart, and a 44.1 kHz session got the wrong sample rate on one of them.
    ProcessContext makeContext(const CompiledGraph& graph,
                               const CompiledGraph::CompiledNode& entry) const noexcept;
    /// Copy the finished mix out to the caller's block, filling every channel.
    void writeSink(const CompiledGraph& graph, const AudioBlock& output,
                   FrameCount frames) noexcept;
    const CompiledGraph* acquireGraph() noexcept;
    void releaseGraph() noexcept;
    void reclaimRetired();

    JobSystem m_jobs;
    /// Hazard-pointer publication: the renderer reads only raw atomics, while
    /// ownership and reclamation stay on the control thread. This avoids both
    /// the implementation lock used by atomic shared_ptr and a last-reference
    /// destructor running at the end of an audio block.
    std::atomic<const CompiledGraph*> m_graphRaw{nullptr};
    std::atomic<const CompiledGraph*> m_graphHazard{nullptr};
    mutable std::mutex m_graphOwnerMutex;
    std::shared_ptr<const CompiledGraph> m_graphOwner;
    std::vector<std::shared_ptr<const CompiledGraph>> m_retiredGraphs;
    const CompiledGraph* m_active = nullptr;              // valid while hazard held

    // Per-block state, written by the renderer before the pass opens and read
    // by every worker inside it.
    FrameCount m_frames = 0;
    SamplePos m_position = 0;
    bool m_playing = false;
    bool m_offline = false;
    TransportInfo m_transport;
    /// Audio-thread-owned continuity stamp. A seek or loop wrap invalidates
    /// queued PDC MIDI from the old playhead; ordinary stopped/live/offline
    /// consecutive blocks retain it so delayed releases still arrive.
    SamplePos m_lastMidiTimelinePosition = 0;
    FrameCount m_lastMidiTimelineFrames = 0;
    bool m_lastMidiTimelinePlaying = false;
    bool m_lastMidiTimelineOffline = false;
    bool m_midiTimelineValid = false;
    /// A small narrow session must not pay for the pool. Wide graphs get an
    /// adaptive lower crossover because node count is not a cost estimate: a
    /// compact graph may contain expensive third-party DSP.
    std::uint32_t m_parallelThreshold = 64;
};

} // namespace daw::engine
