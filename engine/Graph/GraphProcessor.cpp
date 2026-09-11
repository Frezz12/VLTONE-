#include "Graph/GraphProcessor.hpp"
#include "DSP/Simd.hpp"
#include "ScopedNoDenormals.hpp"
#include "Memory/PcmReadCache.hpp"

#include <algorithm>

namespace daw::engine {

GraphProcessor::GraphProcessor(unsigned threadCount) : m_jobs(threadCount) {
    // The sink never changes, so it is installed once rather than per block —
    // the audio thread must not write anything the workers read concurrently.
    m_jobs.setSink(JobSink{&GraphProcessor::executeJob, this});
}

GraphProcessor::~GraphProcessor() = default;

void GraphProcessor::setGraph(std::shared_ptr<const CompiledGraph> graph) {
    // The snapshot arrives with its per-block scratch already sized by
    // `compile()`, so publishing is one atomic store. It used to be three
    // vectors and a pointer swapped under a mutex the audio thread did not
    // take, which meant a rebuild landing mid-block let the renderer index a
    // moved-from vector.
    const std::size_t nodeCount = graph ? graph->nodes.size() : 0;
    m_jobs.prepare(std::max<std::size_t>(nodeCount, 64));
    {
        std::lock_guard lock(m_graphOwnerMutex);
        if (m_graphOwner) m_retiredGraphs.push_back(std::move(m_graphOwner));
        m_graphOwner = std::move(graph);
        m_graphRaw.store(m_graphOwner.get(), std::memory_order_seq_cst);
    }
    reclaimRetired();
}

std::shared_ptr<const CompiledGraph> GraphProcessor::graph() const {
    std::lock_guard lock(m_graphOwnerMutex);
    return m_graphOwner;
}

const CompiledGraph* GraphProcessor::acquireGraph() noexcept {
    const CompiledGraph* snapshot = nullptr;
    do {
        snapshot = m_graphRaw.load(std::memory_order_seq_cst);
        m_graphHazard.store(snapshot, std::memory_order_seq_cst);
    } while (snapshot != m_graphRaw.load(std::memory_order_seq_cst));
    return snapshot;
}

void GraphProcessor::releaseGraph() noexcept {
    m_graphHazard.store(nullptr, std::memory_order_release);
}

void GraphProcessor::reclaimRetired() {
    const CompiledGraph* hazard = m_graphHazard.load(std::memory_order_seq_cst);
    std::lock_guard lock(m_graphOwnerMutex);
    std::erase_if(m_retiredGraphs, [hazard](const auto& graph) {
        return graph.get() != hazard;
    });
}

FrameCount GraphProcessor::latencySamples() const {
    auto snapshot = graph();
    return snapshot ? snapshot->totalLatency : 0;
}

void GraphProcessor::prepareBlockState(const CompiledGraph& graph) noexcept {
    for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
        if (m_fuseBlock && graph.nodes[i].inlineTask) continue;
        graph.pending[i].value.store(graph.pendingTemplate[i],
                                     std::memory_order_relaxed);
    }
}

void GraphProcessor::prepareMidiTimeline(const CompiledGraph& graph,
                                         FrameCount frames,
                                         SamplePos timelinePosition,
                                         bool playing, bool offline) noexcept {
    // A graph recompile may deliberately share these delay objects with its
    // predecessor so a harmless routing edit does not cut pending notes. Only a
    // playhead/mode discontinuity invalidates their time domain.
    bool continuous = m_midiTimelineValid &&
                      m_lastMidiTimelineOffline == offline;
    if (continuous) {
        if (offline) {
            // Offline tail blocks keep advancing their explicit sample position
            // even after sources receive playing=false.
            continuous = timelinePosition ==
                         m_lastMidiTimelinePosition + m_lastMidiTimelineFrames;
        } else if (m_lastMidiTimelinePlaying) {
            // The first stopped block begins where the final playing block ended.
            continuous = timelinePosition ==
                         m_lastMidiTimelinePosition + m_lastMidiTimelineFrames;
        } else {
            // A parked device repeats one position. Starting from that position
            // is continuous too; a different one is a locate and must flush.
            continuous = timelinePosition == m_lastMidiTimelinePosition;
        }
    }
    if (!continuous) {
        for (const auto& delay : graph.midiDelays) delay->reset();
    }
    m_lastMidiTimelinePosition = timelinePosition;
    m_lastMidiTimelineFrames = frames;
    m_lastMidiTimelinePlaying = playing;
    m_lastMidiTimelineOffline = offline;
    m_midiTimelineValid = true;
}

ProcessContext GraphProcessor::makeContext(
    const CompiledGraph& graph,
    const CompiledGraph::CompiledNode& entry) const noexcept {
    // Gather the inputs: each is the producer's output buffer, run through this
    // edge's delay line when the path needed compensating. MIDI rides the same
    // edge and takes the same shift — compensating one and not the other would
    // play an instrument early by exactly the effect's latency.
    const std::uint32_t first = entry.firstInput;
    for (std::uint32_t i = 0; i < entry.inputCount; ++i) {
        const auto& edge = graph.inputEdges[first + i];
        const AudioBlock produced =
            graph.arena.block(graph.nodes[edge.producer].outputBuffer, m_frames);
        const MidiBuffer* producedMidi =
            edge.midiBuffer != kInvalidNode
                ? &graph.midiBuffers[edge.midiBuffer]
                : nullptr;
        if (edge.delayIndex == kInvalidNode) {
            graph.inputScratch[first + i] = produced;
            graph.midiInputScratch[first + i] = producedMidi;
        } else {
            const AudioBlock delayed =
                graph.arena.block(edge.scratchBuffer, m_frames);
            graph.delays[edge.delayIndex]->process(produced, delayed, m_frames);
            graph.inputScratch[first + i] = delayed;

            if (edge.midiDelayIndex != kInvalidNode && producedMidi) {
                MidiBuffer& delayedMidi =
                    graph.midiDelayBuffers[edge.midiDelayIndex];
                graph.midiDelays[edge.midiDelayIndex]->process(
                    *producedMidi, delayedMidi, m_frames);
                graph.midiInputScratch[first + i] = &delayedMidi;
            } else {
                graph.midiInputScratch[first + i] = nullptr;
            }
        }
    }

    // The node's own MIDI output starts empty, so a node that writes nothing
    // does not hand its successors the previous block's notes.
    MidiBuffer* midiOutput = nullptr;
    if (entry.midiOutputBuffer != kInvalidNode) {
        midiOutput = &graph.midiBuffers[entry.midiOutputBuffer];
        midiOutput->clear();
    }

    ProcessContext context;
    context.output = graph.arena.block(entry.outputBuffer, m_frames);
    context.inputs = std::span<const AudioBlock>(
        graph.inputScratch.data() + first, entry.inputCount);
    context.inputRoles = std::span<const InputRole>(
        graph.inputRoles.data() + first, entry.inputCount);
    context.frames = m_frames;
    context.timelinePosition = m_position;
    context.sampleRate = graph.sampleRate;
    context.playing = m_playing;
    context.offline = m_offline;
    context.transport = m_transport;
    context.midiInputs = std::span<const MidiBuffer* const>(
        graph.midiInputScratch.data() + first, entry.inputCount);
    context.midiOutput = midiOutput;
    return context;
}

void GraphProcessor::runNode(const CompiledGraph& graph,
                             std::uint32_t nodeIndex,
                             unsigned workerIndex) noexcept {
    for (;;) {
    const auto& entry = graph.nodes[nodeIndex];

    const ProcessContext context = makeContext(graph, entry);
    const PcmReadScope pcmRead(!m_offline);
    const bool profiling = m_jobs.profiling();
    const auto started = profiling ? rt::nowNanos() : 0;
    entry.node->process(context);
    if (profiling) m_jobs.recordProfile(workerIndex,
        {graph.generation, rt::nowNanos() - started, m_position,
         entry.id, workerIndex, rt::ProfileEvent::Kind::Node});
    if (m_fuseBlock && entry.inlineSuccessor != kInvalidNode) {
        nodeIndex = entry.inlineSuccessor;
        continue;
    }

    // Release the successors this node was blocking; any that hit zero are
    // ready and go straight onto this worker's deque (their input data is warm
    // in this core's cache).
    const std::uint32_t firstSuccessor = entry.firstSuccessor;
    unsigned newlyReady = 0;
    for (std::uint32_t i = 0; i < entry.successorCount; ++i) {
        const std::uint32_t successor = graph.successors[firstSuccessor + i];
        if (graph.pending[successor].value.fetch_sub(
                1, std::memory_order_acq_rel) == 1) {
            m_jobs.submit(workerIndex, successor);
            ++newlyReady;
        }
    }
    // Helpers deliberately park after a short dependency gap. If this node was
    // that gap and just exposed a wide frontier, wake enough of them to share
    // it. The current worker accounts for one ready item itself.
    if (newlyReady > 1) m_jobs.wakeHelpers(newlyReady - 1);
    break;
    }
}

void GraphProcessor::executeJob(void* context, std::uint32_t nodeIndex,
                                unsigned workerIndex) noexcept {
    auto* self = static_cast<GraphProcessor*>(context);
    self->runNode(*self->m_active, nodeIndex, workerIndex);
}

Status GraphProcessor::process(const AudioBlock& output, FrameCount frames,
                               SamplePos timelinePosition, bool playing,
                               bool offline, const TransportInfo& transport) {
    const rt::ScopedNoDenormals noDenormals;
    const CompiledGraph* snapshot = acquireGraph();
    m_lastBlockLatency = snapshot ? snapshot->totalLatency : 0;
    if (!snapshot) return fail(EngineError::NotCompiled);
    if (frames > snapshot->maxBlockSize) {
        releaseGraph();
        return fail(EngineError::BlockTooLarge);
    }
    // Node count alone loses badly on a compact graph containing expensive DSP
    // (one slow root followed by a wide bank of plugins is a common shape).
    // No generic per-node cost hint exists at this layer, so use a conservative
    // structural signal computed at compile time: at least 16 total jobs and a
    // dependency level wide enough to occupy up to four available workers.
    constexpr std::size_t kAdaptiveMinimumNodes = 16;
    const unsigned usefulWorkers = std::min(m_jobs.workerCount(), 4u);
    const bool wideCompactGraph =
        snapshot->nodes.size() >= kAdaptiveMinimumNodes &&
        snapshot->parallelWidth >= usefulWorkers;
    if ((snapshot->nodes.size() < m_parallelThreshold && !wideCompactGraph) ||
        m_jobs.workerCount() == 1) {
        releaseGraph();
        return processSerial(output, frames, timelinePosition, playing, offline,
                             transport);
    }

    m_active = snapshot;
    m_frames = frames;
    m_position = timelinePosition;
    m_playing = playing;
    m_offline = offline;
    m_transport = transport;
    m_fuseBlock = m_taskFusion.load(std::memory_order_relaxed);

    prepareMidiTimeline(*snapshot, frames, timelinePosition, playing, offline);
    prepareBlockState(*snapshot);

    // A fused chain is one stealable task, regardless of its raw node count.
    // Wake only helpers that can take ready roots; runNode announces later
    // fan-outs when they actually become ready. Keep the established wake
    // density for wide DSP graphs: a plugin can have an expensive block at
    // any buffer size, so a low node count is not a reason to withhold workers.
    const auto tasks = m_fuseBlock ? snapshot->taskCount : std::uint32_t(snapshot->nodes.size());
    const auto ready = std::min<std::size_t>(snapshot->roots.size(), tasks);
    const unsigned available = ready > 0 ? unsigned(ready - 1) : 0;
    const unsigned helpers = std::min({available, m_jobs.workerCount() - 1,
        unsigned(std::max<std::size_t>(snapshot->nodes.size() / 16, 1))});
    m_jobs.beginPass(tasks, helpers);

    // Seed every source into worker 0's deque — this thread owns it, and a
    // work-stealing deque may only be pushed to by its owner. The other workers
    // pull the sources out from the top as they wake, which spreads the work
    // without any cross-thread pushes.
    for (std::uint32_t root : snapshot->roots) m_jobs.submit(0, root);

    m_jobs.waitForPass();

    writeSink(*snapshot, output, frames);
    m_active = nullptr;
    releaseGraph();
    return {};
}

Status GraphProcessor::processSerial(const AudioBlock& output, FrameCount frames,
                                     SamplePos timelinePosition, bool playing,
                                     bool offline,
                                     const TransportInfo& transport) {
    const rt::ScopedNoDenormals noDenormals;
    const CompiledGraph* snapshot = acquireGraph();
    m_lastBlockLatency = snapshot ? snapshot->totalLatency : 0;
    if (!snapshot) return fail(EngineError::NotCompiled);
    if (frames > snapshot->maxBlockSize) {
        releaseGraph();
        return fail(EngineError::BlockTooLarge);
    }

    m_active = snapshot;
    m_frames = frames;
    m_position = timelinePosition;
    m_playing = playing;
    m_offline = offline;
    m_transport = transport;

    prepareMidiTimeline(*snapshot, frames, timelinePosition, playing, offline);

    // Topological order guarantees every input is finished before its consumer,
    // so the serial path produces exactly the same samples as the parallel one.
    for (std::uint32_t nodeIndex : snapshot->order) {
        const auto& entry = snapshot->nodes[nodeIndex];
        const ProcessContext context = makeContext(*snapshot, entry);
        const PcmReadScope pcmRead(!offline);
        const bool profiling = m_jobs.profiling();
        const auto started = profiling ? rt::nowNanos() : 0;
        entry.node->process(context);
        if (profiling) m_jobs.recordProfile(0,
            {snapshot->generation, rt::nowNanos() - started, m_position,
             entry.id, 0, rt::ProfileEvent::Kind::Node});
    }

    writeSink(*snapshot, output, frames);
    m_active = nullptr;
    releaseGraph();
    return {};
}

void GraphProcessor::writeSink(const CompiledGraph& graph, const AudioBlock& output,
                               FrameCount frames) noexcept {
    // Every output channel is written on both branches, so a successful
    // `process` fully defines the block. That is what lets the caller skip
    // clearing it beforehand — a memset of the device buffer on every callback
    // that the very next line overwrote.
    if (graph.sinkNode == kInvalidNode) {
        for (ChannelCount ch = 0; ch < output.numChannels(); ++ch) {
            dsp::clear(output.channel(ch).first(frames));
        }
        return;
    }
    const AudioBlock mix =
        graph.arena.block(graph.nodes[graph.sinkNode].outputBuffer, frames);
    for (ChannelCount ch = 0; ch < output.numChannels(); ++ch) {
        if (ch < mix.numChannels()) {
            dsp::copy(output.channel(ch).first(frames), mix.channel(ch));
        } else {
            dsp::clear(output.channel(ch).first(frames));
        }
    }
}

} // namespace daw::engine
