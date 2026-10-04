#include "Graph/OfflineGraphRenderer.hpp"
#include "Memory/PcmReadCache.hpp"
#include "ScopedNoDenormals.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <numeric>
#include <queue>
#include <thread>
#include <unordered_set>
#include <vector>

namespace daw::engine {
namespace {

constexpr std::size_t kWorkspaceLimit = 64 * 1024 * 1024;
constexpr std::size_t kMaxWindow = 8;

struct Block {
    SamplePos position = 0;
    FrameCount frames = 0;
    TransportInfo transport;
    bool playing = false;
    std::size_t remaining = 0;
};

struct Stage {
    std::vector<std::uint32_t> nodes, inputs, successors;
    std::uint64_t next = 0;
    double cost = 1.0, criticalCost = 1.0;
    bool busy = false, capture = false;
};

struct Task {
    std::uint32_t stage = 0;
    std::uint64_t sequence = 0;
    double priority = 0.0;
};

struct EarlierDemand {
    bool operator()(const Task& a, const Task& b) const noexcept {
        if (a.sequence != b.sequence) return a.sequence > b.sequence;
        if (a.priority != b.priority) return a.priority < b.priority;
        return a.stage > b.stage;
    }
};

struct Completion {
    Task task;
    double cost = 0.0;
    std::exception_ptr error;
};

// Offline only: a mutex protects dispatch, never a device callback. Each stage
// has at most one outstanding job. The coordinator publishes it only AFTER
// servicing that instance, so process/service/reset never race on a plugin.
class Renderer {
public:
    Renderer(const CompiledGraph& graph, std::size_t window, std::span<const double> nodeCosts)
        : m_graph(graph), m_window(window), m_blocks(window),
          m_inputs(graph.inputEdges.size()), m_midiInputs(graph.inputEdges.size()) {
        std::vector<std::uint32_t> owner(graph.nodes.size(), kInvalidNode);
        for (const auto first : graph.order) {
            if (graph.nodes[first].inlineTask) continue;
            const auto stageIndex = std::uint32_t(m_stages.size());
            auto& stage = m_stages.emplace_back();
            for (auto node = first; node != kInvalidNode;
                 node = graph.nodes[node].inlineSuccessor) {
                owner[node] = stageIndex;
                stage.nodes.push_back(node);
                if (!nodeCosts.empty()) stage.cost += nodeCosts[node];
                stage.capture |= graph.nodes[node].node->offlineNodePolicy() == OfflineNodePolicy::Capture;
            }
        }
        for (std::uint32_t i = 0; i < m_stages.size(); ++i) {
            auto& stage = m_stages[i];
            for (const auto node : stage.nodes) {
                const auto& entry = graph.nodes[node];
                for (std::uint32_t j = 0; j < entry.inputCount; ++j) {
                    const auto producer = owner[graph.inputEdges[entry.firstInput + j].producer];
                    if (producer != i) stage.inputs.push_back(producer);
                }
            }
            std::sort(stage.inputs.begin(), stage.inputs.end());
            stage.inputs.erase(std::unique(stage.inputs.begin(), stage.inputs.end()), stage.inputs.end());
            for (const auto producer : stage.inputs) m_stages[producer].successors.push_back(i);
        }
        // The realtime arena recycles buffers across nodes. Reusing its indices
        // across overlapping blocks would corrupt readers even within a slot.
        // This arena gives every node/slot its own output and every edge scratch.
        m_arena.allocate(window * graph.nodes.size() + graph.inputEdges.size(),
                         graph.channels, graph.maxBlockSize);
        m_midi.resize(window * graph.midiBuffers.size());
        m_finished.reserve(m_stages.size());
        updateCosts();
    }

    ~Renderer() {
        {
            std::lock_guard lock(m_mutex);
            m_stopping = true;
        }
        m_work.notify_all();
        // Joining precedes graph restoration, including sink exceptions,
        // cancellation, failures and latency/restart requests.
        for (auto& thread : m_threads) if (thread.joinable()) thread.join();
    }

    Status run(unsigned workers, SamplePos start, SamplePos end,
               FrameCount blockSize, SamplePos sourcesEnd, const Transport& transport,
               const std::function<bool(const AudioBlock&, FrameCount)>& sink,
               const std::function<Status(std::uint32_t, SamplePos)>& service) {
        const auto count = std::min<std::size_t>({workers, m_stages.size(), 16});
        for (std::size_t i = 0; i < count; ++i)
            m_threads.emplace_back([this] { worker(); });

        SamplePos position = start;
        auto openBlocks = [&] {
            while (position < end && m_opened - m_committed < m_window) {
                auto& block = m_blocks[m_opened % m_window];
                block.position = position;
                block.frames = FrameCount(std::min<SamplePos>(blockSize, end - position));
                if (position < sourcesEnd)
                    block.frames = FrameCount(std::min<SamplePos>(block.frames, sourcesEnd - position));
                block.transport = transport.infoAt(position);
                block.playing = position < sourcesEnd;
                block.remaining = m_stages.size();
                position += block.frames;
                ++m_opened;
            }
        };
        openBlocks();
        queueReady();
        std::vector<Completion> completed;
        completed.reserve(m_stages.size());
        std::size_t observations = 0;
        while (m_committed < m_opened) {
            {
                std::unique_lock lock(m_mutex);
                m_done.wait(lock, [&] { return !m_finished.empty(); });
                completed.swap(m_finished);
            }
            for (const auto& completion : completed) {
                if (completion.error) std::rethrow_exception(completion.error);
                auto& stage = m_stages[completion.task.stage];
                const auto& block = m_blocks[completion.task.sequence % m_window];
                for (const auto node : stage.nodes) {
                    if (auto status = service(node, block.position); !status) return status;
                    const auto midi = m_graph.nodes[node].midiOutputBuffer;
                    if (midi != kInvalidNode)
                        m_midi[(completion.task.sequence % m_window) * m_graph.midiBuffers.size() + midi]
                            .copyFrom(m_graph.midiBuffers[midi]);
                }
                stage.cost = stage.next == 0 ? completion.cost
                    : stage.cost * 0.8 + completion.cost * 0.2;
                ++stage.next;
                stage.busy = false;
                --m_blocks[completion.task.sequence % m_window].remaining;
                ++observations;
            }
            completed.clear();
            if (observations >= 32) { updateCosts(); observations = 0; }

            // Capture nodes remain pinned to this frontier until sink returns.
            // Other stages can continue while files are written, but no buffer
            // slot is reused until ALL its readers and captures have finished.
            while (m_committed < m_opened && m_blocks[m_committed % m_window].remaining == 0) {
                const auto& block = m_blocks[m_committed % m_window];
                const auto output = nodeOutput(m_graph.sinkNode, m_committed, block.frames);
                if (!sink(output, block.frames)) return {};
                ++m_committed;
                openBlocks();
            }
            queueReady();
        }
        return {};
    }

private:
    AudioBlock nodeOutput(std::uint32_t node, std::uint64_t sequence, FrameCount frames) const noexcept {
        return m_arena.block((sequence % m_window) * m_graph.nodes.size() + node, frames);
    }

    void updateCosts() {
        // Downstream critical work breaks ties among equally urgent tasks.
        // Cost is measured on this project, not inferred from raw node count.
        for (auto i = m_stages.size(); i-- > 0;) {
            double downstream = 0.0;
            for (const auto successor : m_stages[i].successors)
                downstream = std::max(downstream, m_stages[successor].criticalCost);
            m_stages[i].criticalCost = m_stages[i].cost + downstream;
        }
    }

    void queueReady() {
        std::lock_guard lock(m_mutex);
        for (std::uint32_t i = 0; i < m_stages.size(); ++i) {
            auto& stage = m_stages[i];
            if (stage.busy || stage.next >= m_opened ||
                (stage.capture && stage.next != m_committed)) continue;
            if (std::any_of(stage.inputs.begin(), stage.inputs.end(),
                [&](auto producer) { return m_stages[producer].next <= stage.next; })) continue;
            stage.busy = true;
            m_ready.push({i, stage.next, stage.criticalCost});
        }
        m_work.notify_all();
    }

    void process(const Task& task) {
        const rt::ScopedNoDenormals noDenormals;
        const PcmReadScope pcmRead(false);
        const auto& block = m_blocks[task.sequence % m_window];
        const auto midiOffset = (task.sequence % m_window) * m_graph.midiBuffers.size();
        for (const auto node : m_stages[task.stage].nodes) {
            const auto& entry = m_graph.nodes[node];
            for (std::uint32_t i = 0; i < entry.inputCount; ++i) {
                const auto input = entry.firstInput + i;
                const auto& edge = m_graph.inputEdges[input];
                auto audio = nodeOutput(edge.producer, task.sequence, block.frames);
                const MidiBuffer* midi = edge.midiBuffer != kInvalidNode
                    ? &m_midi[midiOffset + edge.midiBuffer] : nullptr;
                if (edge.delayIndex != kInvalidNode) {
                    const auto delayed = m_arena.block(m_window * m_graph.nodes.size() + input, block.frames);
                    m_graph.delays[edge.delayIndex]->process(audio, delayed, block.frames);
                    audio = delayed;
                    if (edge.midiDelayIndex != kInvalidNode && midi) {
                        auto& delayedMidi = m_graph.midiDelayBuffers[edge.midiDelayIndex];
                        m_graph.midiDelays[edge.midiDelayIndex]->process(*midi, delayedMidi, block.frames);
                        midi = &delayedMidi;
                    } else midi = nullptr;
                }
                m_inputs[input] = audio;
                m_midiInputs[input] = midi;
            }
            MidiBuffer* midiOutput = entry.midiOutputBuffer != kInvalidNode
                ? &m_graph.midiBuffers[entry.midiOutputBuffer] : nullptr;
            if (midiOutput) midiOutput->clear();
            ProcessContext context;
            context.output = nodeOutput(node, task.sequence, block.frames);
            context.inputs = std::span<const AudioBlock>(m_inputs).subspan(entry.firstInput, entry.inputCount);
            context.inputRoles = std::span<const InputRole>(m_graph.inputRoles).subspan(entry.firstInput, entry.inputCount);
            context.midiInputs = std::span<const MidiBuffer* const>(m_midiInputs).subspan(entry.firstInput, entry.inputCount);
            context.midiOutput = midiOutput;
            context.frames = block.frames;
            context.timelinePosition = block.position;
            context.sampleRate = m_graph.sampleRate;
            context.playing = block.playing;
            context.offline = true;
            context.transport = block.transport;
            context.compensateInputLatency(entry.inputLatency);
            entry.node->process(context);
        }
    }

    void worker() noexcept {
        for (;;) {
            Task task;
            {
                std::unique_lock lock(m_mutex);
                m_work.wait(lock, [&] { return m_stopping || !m_ready.empty(); });
                if (m_stopping) return;
                task = m_ready.top(); m_ready.pop();
            }
            const auto start = std::chrono::steady_clock::now();
            Completion result; result.task = task;
            try { process(task); } catch (...) { result.error = std::current_exception(); }
            result.cost = std::chrono::duration<double, std::nano>(
                std::chrono::steady_clock::now() - start).count();
            {
                std::lock_guard lock(m_mutex);
                m_finished.push_back(std::move(result));
            }
            m_done.notify_one();
        }
    }

    const CompiledGraph& m_graph;
    std::size_t m_window;
    BufferArena m_arena;
    std::vector<Block> m_blocks;
    std::vector<Stage> m_stages;
    std::vector<AudioBlock> m_inputs;
    std::vector<const MidiBuffer*> m_midiInputs;
    std::vector<MidiBuffer> m_midi;
    std::uint64_t m_opened = 0, m_committed = 0;
    std::mutex m_mutex;
    std::condition_variable m_work, m_done;
    std::priority_queue<Task, std::vector<Task>, EarlierDemand> m_ready;
    std::vector<Completion> m_finished;
    bool m_stopping = false;
    std::vector<std::thread> m_threads;
};

} // namespace

std::size_t offlinePipelineWindow(const CompiledGraph& graph, unsigned workers) {
    if (workers < 2 || graph.taskCount < 2 || graph.sinkNode == kInvalidNode ||
        std::any_of(graph.nodes.begin(), graph.nodes.end(), [](const auto& entry) {
            return entry.node->offlineNodePolicy() == OfflineNodePolicy::Barrier;
        })) return 0;
    std::unordered_set<const Node*> instances;
    for (const auto& entry : graph.nodes)
        if (!instances.insert(entry.node).second) return 0;
    // Bound the aligned PCM/control workspace. MIDI slots copy actual events
    // after DSP completes; the large worst-case pitch reserve remains one per
    // node, as in the reference renderer. Both caches hold at most eight blocks.
    const auto stride = (std::size_t(graph.maxBlockSize) + kCacheLine / sizeof(float) - 1)
        / (kCacheLine / sizeof(float)) * (kCacheLine / sizeof(float));
    const auto pcmBytes = std::size_t(graph.channels) * stride * sizeof(float);
    const auto perSlot = graph.nodes.size() * pcmBytes + graph.midiBuffers.size()
        * sizeof(MidiBuffer);
    const auto fixed = graph.inputEdges.size() * (pcmBytes + sizeof(AudioBlock) + sizeof(MidiBuffer*))
        + graph.nodes.size() * 256;
    if (perSlot == 0 || fixed >= kWorkspaceLimit) return 0;
    const auto window = std::min(kMaxWindow, (kWorkspaceLimit - fixed) / perSlot);
    return window < 2 ? 0 : window;
}

std::optional<Status> renderOfflinePipelined(
    const CompiledGraph& graph, unsigned workers, SamplePos start, SamplePos end,
    FrameCount blockSize, SamplePos sourcesEnd, const Transport& transport,
    const std::function<bool(const AudioBlock&, FrameCount)>& sink,
    const std::function<Status(std::uint32_t, SamplePos)>& service,
    std::span<const double> nodeCosts) {
    const auto window = offlinePipelineWindow(graph, workers);
    if (window == 0 || end - start <= blockSize) return std::nullopt;
    // Dispatch/service handoffs cost more than tiny copy/gain stages. Measure
    // the actual session before allocating the temporal workspace or threads.
    // ponytail: fixed 10 us/job crossover; tune from cross-platform benchmarks.
    if (!nodeCosts.empty() &&
        std::accumulate(nodeCosts.begin(), nodeCosts.end(), 0.0) < graph.taskCount * 10000.0)
        return std::nullopt;
    Renderer renderer(graph, window, nodeCosts);
    return renderer.run(workers, start, end, blockSize, sourcesEnd, transport, sink, service);
}

} // namespace daw::engine
