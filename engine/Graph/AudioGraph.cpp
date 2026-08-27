#include "Graph/AudioGraph.hpp"
#include "DSP/Simd.hpp"

#include <algorithm>
#include <functional>
#include <queue>
#include <unordered_map>

namespace daw::engine {

// ── EdgeDelay ──────────────────────────────────────────────────────────────

void EdgeDelay::prepare(ChannelCount channels, FrameCount delaySamples,
                        FrameCount maxBlockSize) {
    m_channels = channels;
    m_delay = delaySamples;
    // One ring per channel, long enough for the delay plus a whole block.
    m_stride = std::size_t(delaySamples) + maxBlockSize + 1;
    m_storage.assign(m_stride * channels, 0.0f);
    m_writePos = 0;
}

void EdgeDelay::reset() noexcept {
    std::fill(m_storage.begin(), m_storage.end(), 0.0f);
    m_writePos = 0;
}

void EdgeDelay::process(const AudioBlock& input, const AudioBlock& output,
                        FrameCount frames) noexcept {
    if (m_stride == 0) return;
    for (ChannelCount ch = 0; ch < m_channels; ++ch) {
        float* ring = m_storage.data() + std::size_t(ch) * m_stride;
        const float* source = input.data(ch);
        float* destination = output.data(ch);
        std::size_t write = m_writePos;
        std::size_t read = (write + m_stride - m_delay) % m_stride;
        for (FrameCount i = 0; i < frames; ++i) {
            ring[write] = source[i];
            destination[i] = ring[read];
            if (++write >= m_stride) write = 0;
            if (++read >= m_stride) read = 0;
        }
        if (ch + 1 == m_channels) m_writePos = write;
    }
}

// ── Graph editing ──────────────────────────────────────────────────────────

NodeId AudioGraph::addNode(std::unique_ptr<Node> node) {
    return adoptNode(std::shared_ptr<Node>(std::move(node)));
}

NodeId AudioGraph::adoptNode(std::shared_ptr<Node> node) {
    m_dirty = true;
    if (!m_freeSlots.empty()) {
        const NodeId id = m_freeSlots.back();
        m_freeSlots.pop_back();
        m_nodes[id] = Slot{std::move(node), {}, {}, true};
        return id;
    }
    m_nodes.push_back(Slot{std::move(node), {}, {}, true});
    return NodeId(m_nodes.size() - 1);
}

Status AudioGraph::removeNode(NodeId id) {
    if (id >= m_nodes.size() || !m_nodes[id].alive) return fail(EngineError::UnknownNode);
    disconnectAll(id);
    m_nodes[id].node.reset();
    m_nodes[id].alive = false;
    m_freeSlots.push_back(id);
    if (m_sink == id) m_sink = kInvalidNode;
    m_dirty = true;
    return {};
}

Node* AudioGraph::node(NodeId id) noexcept {
    if (id >= m_nodes.size() || !m_nodes[id].alive) return nullptr;
    return m_nodes[id].node.get();
}

const Node* AudioGraph::node(NodeId id) const noexcept {
    if (id >= m_nodes.size() || !m_nodes[id].alive) return nullptr;
    return m_nodes[id].node.get();
}

Status AudioGraph::connect(NodeId producer, NodeId consumer, InputRole role) {
    if (producer >= m_nodes.size() || consumer >= m_nodes.size() ||
        !m_nodes[producer].alive || !m_nodes[consumer].alive) {
        return fail(EngineError::UnknownNode);
    }
    if (producer == consumer) return fail(EngineError::CycleDetected);

    auto& inputs = m_nodes[consumer].inputs;
    const auto existing = std::find_if(
        inputs.begin(), inputs.end(), [producer, role](const Connection& input) {
            return input.producer == producer && input.role == role;
        });
    if (existing == inputs.end()) {
        inputs.push_back(Connection{producer, role});
        m_nodes[producer].outputs.push_back(consumer);
        m_dirty = true;
    }
    return {};
}

Status AudioGraph::disconnect(NodeId producer, NodeId consumer) {
    if (producer >= m_nodes.size() || consumer >= m_nodes.size()) {
        return fail(EngineError::UnknownNode);
    }
    auto& inputs = m_nodes[consumer].inputs;
    auto& outputs = m_nodes[producer].outputs;
    inputs.erase(std::remove_if(inputs.begin(), inputs.end(),
                                [producer](const Connection& input) {
                                    return input.producer == producer;
                                }),
                 inputs.end());
    outputs.erase(std::remove(outputs.begin(), outputs.end(), consumer), outputs.end());
    m_dirty = true;
    return {};
}

void AudioGraph::disconnectAll(NodeId id) {
    if (id >= m_nodes.size()) return;
    for (const Connection& input : m_nodes[id].inputs) {
        const NodeId producer = input.producer;
        auto& outputs = m_nodes[producer].outputs;
        outputs.erase(std::remove(outputs.begin(), outputs.end(), id), outputs.end());
    }
    for (NodeId consumer : m_nodes[id].outputs) {
        auto& inputs = m_nodes[consumer].inputs;
        inputs.erase(std::remove_if(inputs.begin(), inputs.end(),
                                    [id](const Connection& input) {
                                        return input.producer == id;
                                    }),
                     inputs.end());
    }
    m_nodes[id].inputs.clear();
    m_nodes[id].outputs.clear();
    m_dirty = true;
}

// ── Compilation ────────────────────────────────────────────────────────────

Result<std::vector<NodeId>> AudioGraph::topologicalOrder() const {
    std::vector<std::uint32_t> inDegree(m_nodes.size(), 0);
    std::vector<NodeId> live;
    for (NodeId id = 0; id < m_nodes.size(); ++id) {
        if (!m_nodes[id].alive) continue;
        live.push_back(id);
        inDegree[id] = std::uint32_t(m_nodes[id].inputs.size());
    }

    // Kahn's algorithm, taking ready nodes in id order so the compiled order is
    // reproducible for the same graph — the offline render must match the live
    // one sample for sample.
    std::vector<NodeId> ready;
    for (NodeId id : live) {
        if (inDegree[id] == 0) ready.push_back(id);
    }
    std::sort(ready.begin(), ready.end());

    std::vector<NodeId> order;
    order.reserve(live.size());
    for (std::size_t head = 0; head < ready.size(); ++head) {
        const NodeId id = ready[head];
        order.push_back(id);
        for (NodeId consumer : m_nodes[id].outputs) {
            if (--inDegree[consumer] == 0) ready.push_back(consumer);
        }
    }

    if (order.size() != live.size()) return fail(EngineError::CycleDetected);
    return order;
}

namespace {

/// Identifies one compensation delay line across recompiles. Keyed on the node
/// *objects*, not their ids: a rebuild throws the whole AudioGraph away and
/// renumbers every NodeId, but `adoptNode` carries the same Node objects over.
struct DelayKey {
    const Node* producer;
    const Node* consumer;
    FrameCount delaySamples;

    friend bool operator==(const DelayKey&, const DelayKey&) = default;
};

struct DelayKeyHash {
    std::size_t operator()(const DelayKey& key) const noexcept {
        const std::size_t a = std::hash<const void*>{}(key.producer);
        const std::size_t b = std::hash<const void*>{}(key.consumer);
        return a ^ (b << 1) ^ (std::size_t(key.delaySamples) << 3);
    }
};

/// The pair travels together: one edge, one audio ring, one event queue.
struct DelayPair {
    std::shared_ptr<EdgeDelay> audio;
    std::shared_ptr<MidiDelay> midi;
};

using DelayCache = std::unordered_map<DelayKey, DelayPair, DelayKeyHash>;

/// Index the previous snapshot's delay lines so the new one can adopt them.
/// A ring is only interchangeable when the block size and channel count that
/// sized it are unchanged too, so a `prepare()` with new settings gets fresh
/// lines rather than mis-sized ones.
DelayCache reusableDelays(const CompiledGraph* previous, const PrepareInfo& info) {
    DelayCache cache;
    if (!previous || previous->channels != info.channels ||
        previous->maxBlockSize != info.maxBlockSize ||
        previous->sampleRate != info.sampleRate) {
        return cache;
    }
    for (std::size_t i = 0; i < previous->nodes.size(); ++i) {
        const auto& consumer = previous->nodes[i];
        for (std::uint32_t e = 0; e < consumer.inputCount; ++e) {
            const auto& edge = previous->inputEdges[consumer.firstInput + e];
            if (edge.delayIndex == kInvalidNode) continue;
            const auto& line = previous->delays[edge.delayIndex];
            if (!line) continue;
            std::shared_ptr<MidiDelay> notes;
            if (edge.midiDelayIndex != kInvalidNode &&
                edge.midiDelayIndex < previous->midiDelays.size()) {
                notes = previous->midiDelays[edge.midiDelayIndex];
            }
            cache.emplace(DelayKey{previous->nodes[edge.producer].node,
                                   consumer.node, line->delaySamples()},
                          DelayPair{line, std::move(notes)});
        }
    }
    return cache;
}

} // namespace

Result<std::shared_ptr<const CompiledGraph>> AudioGraph::compile(
    const PrepareInfo& info, const CompiledGraph* previous) {
    if (info.maxBlockSize == 0 || info.maxBlockSize > kMaxBlockSize) {
        return fail(EngineError::BlockTooLarge);
    }
    DelayCache reusable = reusableDelays(previous, info);

    auto orderResult = topologicalOrder();
    if (!orderResult) return fail(orderResult.error());
    const std::vector<NodeId>& order = *orderResult;

    auto compiled = std::make_shared<CompiledGraph>();
    compiled->channels = info.channels;
    compiled->maxBlockSize = info.maxBlockSize;
    compiled->sampleRate = info.sampleRate;
    compiled->generation = ++m_generation;

    // Dense index per live node, so the realtime path walks contiguous arrays
    // instead of chasing sparse ids.
    std::vector<std::uint32_t> denseIndex(m_nodes.size(), kInvalidNode);
    compiled->nodes.reserve(order.size());
    for (std::uint32_t i = 0; i < order.size(); ++i) {
        denseIndex[order[i]] = i;
        CompiledGraph::CompiledNode entry;
        entry.owner = m_nodes[order[i]].node;
        entry.node = entry.owner.get();
        entry.id = order[i];
        compiled->nodes.push_back(entry);
    }

    // New hosted plugins learn their real latency only after activation. Do
    // this before compiling compensation, otherwise the first graph snapshot
    // permanently bakes in the descriptor/default latency of zero.
    for (auto& entry : compiled->nodes) {
        if (entry.node->isPreparedFor(info)) continue;
        entry.node->prepare(info);
        entry.node->markPrepared(info);
    }

    // ── Latency: a node cannot start before its slowest input has caught up ──
    std::vector<FrameCount> arrival(order.size(), 0);   // latency at the input
    for (std::uint32_t i = 0; i < order.size(); ++i) {
        const Slot& slot = m_nodes[order[i]];
        FrameCount latest = 0;
        for (const Connection& input : slot.inputs) {
            latest = std::max(latest, arrival[denseIndex[input.producer]]);
        }
        arrival[i] = latest + slot.node->latencySamples();
        compiled->nodes[i].latency = arrival[i];
    }
    compiled->totalLatency =
        m_sink != kInvalidNode && denseIndex[m_sink] != kInvalidNode
            ? arrival[denseIndex[m_sink]]
            : (arrival.empty() ? 0 : *std::max_element(arrival.begin(), arrival.end()));

    // ── Edges, with a delay line wherever a path arrives early ──
    BufferAllocator allocator;
    std::vector<std::uint32_t> remainingConsumers(order.size(), 0);
    for (std::uint32_t i = 0; i < order.size(); ++i) {
        remainingConsumers[i] = std::uint32_t(m_nodes[order[i]].outputs.size());
    }

    // Reachability, one bit per node: ancestors[i] holds every node that is
    // guaranteed to have finished before i starts. It is what makes buffer
    // recycling safe under parallel execution.
    const std::size_t words = (order.size() + 63) / 64;
    std::vector<std::uint64_t> ancestors(words * order.size(), 0);
    auto ancestorsOf = [&](std::uint32_t index) {
        return std::span<std::uint64_t>(ancestors.data() + std::size_t(index) * words,
                                        words);
    };
    for (std::uint32_t i = 0; i < order.size(); ++i) {
        auto self = ancestorsOf(i);
        for (const Connection& input : m_nodes[order[i]].inputs) {
            const NodeId producer = input.producer;
            const std::uint32_t p = denseIndex[producer];
            auto parent = ancestorsOf(p);
            for (std::size_t w = 0; w < words; ++w) self[w] |= parent[w];
            self[p / 64] |= std::uint64_t(1) << (p % 64);
        }
    }

    std::uint32_t midiBufferCount = 0;
    for (std::uint32_t i = 0; i < order.size(); ++i) {
        const Slot& slot = m_nodes[order[i]];
        auto& entry = compiled->nodes[i];
        const MidiNodeRole midiRole = slot.node->midiRole();
        // Output capability is independent of upstream MIDI. A hosted MIDI
        // generator may have an ordinary audio input and still emit events of
        // its own; withholding its buffer merely because no event reached this
        // block would silently discard that output. Audio-only built-ins opt out
        // with MidiNodeRole::None, so the arena remains sparse without changing
        // the legacy contract for custom/default InputOutput nodes.
        if (producesMidi(midiRole)) {
            entry.midiOutputBuffer = midiBufferCount++;
        }
        entry.firstInput = std::uint32_t(compiled->inputEdges.size());
        entry.inputCount = std::uint32_t(slot.inputs.size());
        entry.dependencies = entry.inputCount;

        const FrameCount required = arrival[i] - slot.node->latencySamples();
        for (const Connection& input : slot.inputs) {
            const NodeId producer = input.producer;
            const std::uint32_t producerIndex = denseIndex[producer];
            CompiledGraph::Edge edge;
            edge.producer = producerIndex;
            edge.role = input.role;
            if (acceptsMidi(midiRole)) {
                edge.midiBuffer =
                    compiled->nodes[producerIndex].midiOutputBuffer;
            }

            const FrameCount produced = arrival[producerIndex];
            if (produced < required) {
                // This path is early: delay it so every input lines up. That is
                // the whole of plugin delay compensation — buses, sends,
                // sidechains and groups all fall out of this one rule.
                const FrameCount delaySamples = required - produced;
                const DelayKey key{compiled->nodes[producerIndex].node,
                                   compiled->nodes[i].node, delaySamples};
                DelayPair pair;
                // Erase on adoption: one ring holds one edge's history, so two
                // edges must never end up sharing a line.
                if (auto found = reusable.find(key); found != reusable.end()) {
                    pair = std::move(found->second);
                    reusable.erase(found);
                }
                if (!pair.audio) {
                    pair.audio = std::make_shared<EdgeDelay>();
                    pair.audio->prepare(info.channels, delaySamples, info.maxBlockSize);
                }
                edge.delayIndex = std::uint32_t(compiled->delays.size());
                compiled->delays.push_back(std::move(pair.audio));
                if (edge.midiBuffer != kInvalidNode) {
                    if (!pair.midi) {
                        pair.midi = std::make_shared<MidiDelay>();
                        pair.midi->prepare(delaySamples, kMidiEventsPerBlock);
                    }
                    edge.midiDelayIndex =
                        std::uint32_t(compiled->midiDelays.size());
                    compiled->midiDelays.push_back(std::move(pair.midi));
                }
                // Scratch for the delayed signal: written and read by this node
                // alone, so it is never shared.
                edge.scratchBuffer = std::uint32_t(allocator.acquireExclusive());
            }
            compiled->inputEdges.push_back(edge);
            compiled->inputRoles.push_back(edge.role);
        }

        // The node writes into a buffer of its own …
        entry.outputBuffer = std::uint32_t(allocator.acquire(ancestorsOf(i)));

        // … and every input buffer whose last consumer this was goes back on
        // the free list, tagged with the nodes that read it, so buffer count
        // tracks the graph's width rather than its size.
        for (const Connection& input : slot.inputs) {
            const NodeId producer = input.producer;
            const std::uint32_t producerIndex = denseIndex[producer];
            if (--remainingConsumers[producerIndex] == 0 &&
                (m_sink == kInvalidNode || producerIndex != denseIndex[m_sink])) {
                std::vector<std::uint32_t> readers;
                readers.reserve(m_nodes[producer].outputs.size());
                for (NodeId consumer : m_nodes[producer].outputs) {
                    readers.push_back(denseIndex[consumer]);
                }
                allocator.release(compiled->nodes[producerIndex].outputBuffer,
                                  std::move(readers));
            }
        }
    }

    // Successor lists, for the dependency-driven scheduler.
    std::vector<std::vector<std::uint32_t>> successorsByNode(order.size());
    for (std::uint32_t i = 0; i < order.size(); ++i) {
        for (NodeId consumer : m_nodes[order[i]].outputs) {
            successorsByNode[i].push_back(denseIndex[consumer]);
        }
    }
    for (std::uint32_t i = 0; i < order.size(); ++i) {
        compiled->nodes[i].firstSuccessor = std::uint32_t(compiled->successors.size());
        compiled->nodes[i].successorCount = std::uint32_t(successorsByNode[i].size());
        compiled->successors.insert(compiled->successors.end(),
                                    successorsByNode[i].begin(),
                                    successorsByNode[i].end());
    }

    compiled->order.reserve(order.size());
    compiled->pendingTemplate.reserve(order.size());
    // Nodes with the same dependency level never depend on one another, so a
    // level's size is a safe lower bound on exploitable parallelism. Compute it
    // once on the control thread; the callback only reads the scalar result.
    std::vector<std::uint32_t> dependencyLevel(order.size(), 0);
    std::vector<std::uint32_t> levelWidth(order.size(), 0);
    for (std::uint32_t i = 0; i < order.size(); ++i) {
        compiled->order.push_back(i);
        compiled->pendingTemplate.push_back(compiled->nodes[i].dependencies);
        if (compiled->nodes[i].dependencies == 0) compiled->roots.push_back(i);

        std::uint32_t level = 0;
        const auto& entry = compiled->nodes[i];
        for (std::uint32_t input = 0; input < entry.inputCount; ++input) {
            const std::uint32_t producer =
                compiled->inputEdges[entry.firstInput + input].producer;
            level = std::max(level, dependencyLevel[producer] + 1);
        }
        dependencyLevel[i] = level;
        compiled->parallelWidth = std::max(compiled->parallelWidth,
                                           ++levelWidth[level]);
    }

    compiled->sinkNode =
        m_sink != kInvalidNode ? denseIndex[m_sink] : kInvalidNode;

    // Every buffer the graph can possibly need, in one arena.
    compiled->arena.allocate(std::max<std::size_t>(allocator.bufferCount(), 1),
                             info.channels, info.maxBlockSize);

    // Per-block scratch travels with the snapshot, so a renderer that has
    // loaded this graph can never index it with another graph's sizes.
    compiled->pending = std::vector<PendingCounter>(order.size());
    compiled->inputScratch.assign(compiled->inputEdges.size(), AudioBlock{});

    // MIDI scratch, sized here for the same reason as the audio scratch: the
    // renderer must only ever index into what the snapshot itself carries.
    compiled->midiBuffers.resize(midiBufferCount);
    for (MidiBuffer& buffer : compiled->midiBuffers) buffer.reserve(kMidiEventsPerBlock);
    compiled->midiDelayBuffers.resize(compiled->midiDelays.size());
    for (MidiBuffer& buffer : compiled->midiDelayBuffers) {
        buffer.reserve(kMidiEventsPerBlock);
    }
    compiled->midiInputScratch.assign(compiled->inputEdges.size(), nullptr);

    m_dirty = false;
    return std::shared_ptr<const CompiledGraph>(std::move(compiled));
}

} // namespace daw::engine
