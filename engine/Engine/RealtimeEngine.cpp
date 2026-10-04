#include "Engine/RealtimeEngine.hpp"
#include "Graph/OfflineGraphRenderer.hpp"
#include "DSP/Simd.hpp"
#include "ScopedNoDenormals.hpp"

#include <chrono>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <thread>

namespace daw::engine {

RealtimeEngine::RenderGate::RenderGate(RealtimeEngine& engine)
    : m_engine(engine), m_lock(engine.m_controlMutex) {
    if (m_engine.m_gateDepth++ != 0) return;
    m_engine.m_gateRequested.store(true);
    // Spin rather than sleep: this closes in at most one block period, and the
    // control thread has nothing useful to do with the interval anyway.
    while (m_engine.m_rendering.load()) std::this_thread::yield();
}

RealtimeEngine::RenderGate::~RenderGate() {
    if (--m_engine.m_gateDepth == 0) m_engine.m_gateRequested.store(false);
}

RealtimeEngine::RealtimeEngine(unsigned threadCount) : m_processor(threadCount) {}

RealtimeEngine::~RealtimeEngine() = default;

Status RealtimeEngine::prepare(SampleRate sampleRate, FrameCount maxBlockSize,
                               ChannelCount channels, bool offline) {
    if (maxBlockSize == 0 || maxBlockSize > kMaxBlockSize) {
        return fail(EngineError::BlockTooLarge);
    }

    // Every node is about to be re-prepared with new settings, which reallocates
    // the scratch `process()` reads. Park the renderer first — the device
    // callback may well be mid-block.
    const RenderGate gate(*this);

    if (m_offlineActive) return fail(EngineError::NotCompiled);

    m_prepareInfo.sampleRate = sampleRate;
    m_prepareInfo.maxBlockSize = maxBlockSize;
    m_prepareInfo.channels = channels;
    m_prepareInfo.offline = offline;
    m_transport.setSampleRate(sampleRate);
    prepareMasterSpectrum(sampleRate);
    m_masterLoudness.prepare(sampleRate);

    m_offlineStorage.assign(std::size_t(channels) * maxBlockSize, 0.0f);
    m_offlinePointers.resize(channels);
    for (ChannelCount ch = 0; ch < channels; ++ch) {
        m_offlinePointers[ch] = m_offlineStorage.data() + std::size_t(ch) * maxBlockSize;
    }
    m_offlineError.clear();
    return offline ? prepareOfflineGraph() : commitGraph();
}

double RealtimeEngine::SpectrumBandFilter::process(float input,
                                                   std::size_t channel) noexcept {
    SpectrumFilterState& state = channels[channel];
    const double x = input;
    const double y = b0 * x + b2 * state.x2 - a1 * state.y1 - a2 * state.y2;
    state.x2 = state.x1;
    state.x1 = x;
    state.y2 = state.y1;
    state.y1 = y;
    return y;
}

void RealtimeEngine::SpectrumBandFilter::reset() noexcept {
    channels = {};
}

void RealtimeEngine::prepareMasterSpectrum(SampleRate sampleRate) noexcept {
    // ISO-like log spacing: enough resolution to read bass/mids/air in an
    // 88-pixel transport display without turning each bar into a sub-pixel.
    static constexpr std::array<double, kMasterSpectrumBandCount> centres{
        63.0, 100.0, 160.0, 250.0, 400.0, 630.0,
        1000.0, 1600.0, 2500.0, 4000.0, 6300.0, 10000.0,
    };
    constexpr double q = 1.15;

    for (std::size_t band = 0; band < centres.size(); ++band) {
        SpectrumBandFilter& filter = m_spectrumFilters[band];
        filter = {};
        if (sampleRate > 0.0) {
            const double frequency = std::min(centres[band], sampleRate * 0.45);
            const double omega = 2.0 * std::numbers::pi * frequency / sampleRate;
            const double alpha = std::sin(omega) / (2.0 * q);
            const double a0 = 1.0 + alpha;
            // RBJ constant-peak-gain band-pass. State persists across device
            // blocks, which is essential for bass periods longer than a block.
            filter.b0 = alpha / a0;
            filter.b2 = -filter.b0;
            filter.a1 = -2.0 * std::cos(omega) / a0;
            filter.a2 = (1.0 - alpha) / a0;
        }
        m_masterSpectrum[band].store(0.0f, std::memory_order_relaxed);
    }
    m_masterSpectrumActive = false;
}

RealtimeEngine::MasterSpectrum RealtimeEngine::masterSpectrum() const noexcept {
    MasterSpectrum result{};
    for (std::size_t band = 0; band < result.size(); ++band) {
        result[band] = m_masterSpectrum[band].load(std::memory_order_relaxed);
    }
    return result;
}

void RealtimeEngine::removeMasterSpectrumConsumer() noexcept {
    unsigned current = m_masterSpectrumConsumers.load(std::memory_order_relaxed);
    while (current != 0 &&
           !m_masterSpectrumConsumers.compare_exchange_weak(
               current, current - 1, std::memory_order_release,
               std::memory_order_relaxed)) {
    }
}

Status RealtimeEngine::commitGraph(bool reconfigureNodes) {
    const std::lock_guard controlLock(m_controlMutex);
    if (m_offlineActive) return fail(EngineError::NotCompiled);
    // Invalidated nodes may be deactivated and have their scratch reallocated
    // during compile. In that case the complete compile must be inside the
    // gate, not merely the final atomic publication.
    // Hand the published snapshot over so compensation delay lines that did not
    // change survive the rebuild with their contents.
    const std::shared_ptr<const CompiledGraph> previous = m_processor.graph();
    std::unique_ptr<RenderGate> reconfigurationGate;
    if (reconfigureNodes ||
        (previous && m_graph.requiresRenderStopped(m_prepareInfo, *previous)))
        reconfigurationGate = std::make_unique<RenderGate>(*this);
    auto compiled = m_graph.compile(m_prepareInfo, previous.get());
    if (!compiled) return fail(compiled.error());

    // Publishing is a single atomic store, so a routing edit lands without
    // disturbing the renderer. The one exception is a graph that has outgrown
    // the job system's deques: growing them swaps out the slot arrays the
    // workers are indexing, which loses items and hangs the pass in flight.
    // That is rare — capacity only grows — so pay for the gate only then.
    if (m_processor.publishNeedsRenderStopped(**compiled)) {
        const RenderGate gate(*this);
        m_processor.setGraph(*compiled);
    } else {
        m_processor.setGraph(*compiled);
    }
    return {};
}

void RealtimeEngine::updateMasterMeters(const AudioBlock& output,
                                        FrameCount frames) noexcept {
    const float left = output.numChannels() > 0
        ? dsp::peak(output.channel(0).first(frames)) : 0.0f;
    const float right = output.numChannels() > 1
        ? dsp::peak(output.channel(1).first(frames)) : 0.0f;
    m_masterPeakL.store(left, std::memory_order_relaxed);
    m_masterPeakR.store(right, std::memory_order_relaxed);
    m_masterPeakHold.observe(std::max(left, right));
    // Read the final master output, after its inserts/fader. M/S remain live
    // for monitoring; I accumulates only while the transport is running.
    m_masterLoudness.process(output, frames, m_transport.isPlaying());

    if (m_masterSpectrumConsumers.load(std::memory_order_acquire) == 0) {
        if (m_masterSpectrumActive) {
            for (std::size_t band = 0; band < m_spectrumFilters.size(); ++band) {
                m_spectrumFilters[band].reset();
                m_masterSpectrum[band].store(0.0f, std::memory_order_relaxed);
            }
            m_masterSpectrumActive = false;
        }
        return;
    }
    m_masterSpectrumActive = true;

    const std::size_t channels = std::min<std::size_t>(output.numChannels(), 2);
    const double seconds = m_prepareInfo.sampleRate > 0.0
                               ? double(frames) / m_prepareInfo.sampleRate
                               : 0.0;
    for (std::size_t band = 0; band < m_spectrumFilters.size(); ++band) {
        double energy = 0.0;
        SpectrumBandFilter& filter = m_spectrumFilters[band];
        for (std::size_t channel = 0; channel < channels; ++channel) {
            const float* input = output.data(ChannelCount(channel));
            for (FrameCount frame = 0; frame < frames; ++frame) {
                const double value = filter.process(input[frame], channel);
                energy += value * value;
            }
        }

        const double sampleCount = double(frames) * double(channels);
        float target = sampleCount > 0.0
                           ? float(std::sqrt(energy / sampleCount))
                           : 0.0f;
        if (!std::isfinite(target)) target = 0.0f;
        const float previous =
            m_masterSpectrum[band].load(std::memory_order_relaxed);
        // Fast attack preserves drum hits; the slower release keeps adjacent
        // UI refreshes visually connected without smearing frequency content.
        const double timeConstant = target > previous ? 0.018 : 0.180;
        const float amount = seconds > 0.0
                                 ? float(1.0 - std::exp(-seconds / timeConstant))
                                 : 1.0f;
        m_masterSpectrum[band].store(
            std::clamp(previous + (target - previous) * amount, 0.0f, 2.0f),
            std::memory_order_relaxed);
    }
}

void RealtimeEngine::renderBlock(const AudioBlock& output,
                                 const float* const* input,
                                 ChannelCount inputChannels, FrameCount frames) {
    const rt::ScopedNoDenormals noDenormals;
    // Gate check, then claim the render. The claim must be published before the
    // re-check, or a gate opening in between would see `rendering` clear and
    // let the control thread reconfigure nodes underneath this block.
    if (m_gateRequested.load()) {
        m_lastBlockResult.store(BlockResult::Gated, std::memory_order_relaxed);
        m_gatedBlocks.fetch_add(1, std::memory_order_relaxed);
        for (ChannelCount ch = 0; ch < output.numChannels(); ++ch) {
            dsp::clear(output.channel(ch).first(frames));
        }
        if (!m_outputGated) m_outputTransition.begin(m_transport.sampleRate());
        m_outputTransition.process(output, frames);
        m_outputGated = true;
        return;
    }
    m_rendering.store(true);
    if (m_gateRequested.load()) {
        m_lastBlockResult.store(BlockResult::Gated, std::memory_order_relaxed);
        m_gatedBlocks.fetch_add(1, std::memory_order_relaxed);
        m_rendering.store(false);
        for (ChannelCount ch = 0; ch < output.numChannels(); ++ch) {
            dsp::clear(output.channel(ch).first(frames));
        }
        if (!m_outputGated) m_outputTransition.begin(m_transport.sampleRate());
        m_outputTransition.process(output, frames);
        m_outputGated = true;
        return;
    }

    const auto start = std::chrono::steady_clock::now();

    m_inputBus.channels = input;
    m_inputBus.channelCount = inputChannels;
    m_inputBus.frames = frames;

    const bool playing = m_transport.isPlaying();
    const auto transportGeneration = m_transport.presentationGeneration();
    // Read the playhead for this block, then advance — every node in the graph
    // sees exactly the same timeline position.
    const SamplePos position = playing ? m_transport.advance(frames, /*deferPresentation=*/true)
                                       : m_transport.position();

    m_lastBlockPosition = position;

    // Musical time is read once, for the position this block starts at, so the
    // whole graph agrees on the beat even though nodes run on several threads.
    const TransportInfo transport = m_transport.infoAt(position);

    // `process` writes every channel when it succeeds; only a graph that failed
    // to render leaves the device buffer undefined, and that is the one case
    // that needs silencing.
    const auto graphStarted = rt::nowNanos();
    m_lastBlockResult.store(BlockResult::Complete, std::memory_order_relaxed);
    Status processed;
    bool dspFailed = false;
    FrameCount offset = 0;
    SamplePos segmentPosition = position;
    std::array<float*, kMaxChannels> outputPointers{};
    std::array<const float*, kMaxChannels> inputPointers{};
    if (frames > m_prepareInfo.maxBlockSize) processed = fail(EngineError::BlockTooLarge);
    else if (output.numChannels() > kMaxChannels || inputChannels > kMaxChannels)
        processed = fail(EngineError::ChannelMismatch);
    while (processed && offset < frames) {
        FrameCount count = frames - offset;
        if (playing && transport.looping) {
            const auto begin = m_transport.loopStart(), end = m_transport.loopEnd();
            if (end > begin) {
                if (segmentPosition >= end)
                    segmentPosition = begin + (segmentPosition - begin) % (end - begin);
                if (segmentPosition < end)
                    count = FrameCount(std::min<SamplePos>(count, end - segmentPosition));
            }
        }
        for (ChannelCount ch = 0; ch < output.numChannels(); ++ch)
            outputPointers[ch] = output.data(ch) + offset;
        for (ChannelCount ch = 0; ch < inputChannels; ++ch)
            inputPointers[ch] = input && input[ch] ? input[ch] + offset : nullptr;
        m_inputBus.channels = input ? inputPointers.data() : nullptr;
        m_inputBus.frames = count;
        auto segmentTransport = transport;
        const double beatsPerSample = transport.tempo / (60.0 * m_prepareInfo.sampleRate);
        segmentTransport.ppqPosition = segmentPosition * beatsPerSample;
        const double bar = 4.0 * transport.timeSigNumerator / transport.timeSigDenominator;
        segmentTransport.barStartPpq = bar > 0.0
            ? std::floor(segmentTransport.ppqPosition / bar) * bar : 0.0;
        processed = m_processor.process(AudioBlock(outputPointers.data(), output.numChannels(), count),
            count, segmentPosition, playing, false, segmentTransport);
        if (!processed && processed.error() == EngineError::ProcessingFailed) {
            dspFailed = true; processed = {};
        }
        if (processed) {
            FrameCount fadeLimit = kMaxBlockSize;
            if (playing && transport.looping &&
                m_transport.loopEnd() > m_transport.loopStart())
                fadeLimit = FrameCount(std::clamp<SamplePos>(
                    (m_transport.loopEnd() - m_transport.loopStart()) / 2,
                    1, kMaxBlockSize));
            if (m_outputGated || (m_outputValid &&
                (playing != m_outputPlaying ||
                 transportGeneration != m_outputTransportGeneration ||
                 m_processor.lastBlockGraphGeneration() != m_outputGraphGeneration ||
                 (playing && segmentPosition != m_expectedOutputPosition))))
                m_outputTransition.begin(m_transport.sampleRate(), fadeLimit);
            m_outputTransition.process(output, count, offset);
            m_outputValid = true;
            m_outputGated = false;
            m_outputPlaying = playing;
            m_outputTransportGeneration = transportGeneration;
            m_outputGraphGeneration = m_processor.lastBlockGraphGeneration();
            m_expectedOutputPosition = segmentPosition + (playing ? count : 0);
        }
        offset += count;
        segmentPosition += count;
    }
    if (processed && dspFailed) processed = fail(EngineError::ProcessingFailed);
    m_lastRenderError.store(processed ? -1 : int(processed.error()), std::memory_order_relaxed);
    if (!processed) {
        m_lastBlockResult.store(BlockResult::Failed, std::memory_order_relaxed);
        m_failedBlocks.fetch_add(1, std::memory_order_relaxed);
        // DSP failures already contain safe fallback audio; retain healthy tracks.
        if (processed.error() != EngineError::ProcessingFailed) {
            for (ChannelCount ch = 0; ch < output.numChannels(); ++ch)
                dsp::clear(output.channel(ch).first(frames));
            m_outputTransition.begin(m_transport.sampleRate());
            m_outputTransition.process(output, frames);
            m_outputGated = true;
        }
    }

    if (playing) m_transport.finishPresentationBlock(m_processor.lastBlockLatencySamples());
    m_graphMetrics.record(rt::nowNanos() - graphStarted, frames, m_prepareInfo.sampleRate);

    updateMasterMeters(output, frames);
    if (m_blockObserver) m_blockObserver(output, frames);

    m_inputBus.channels = nullptr;
    m_inputBus.frames = 0;

    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const double budget = m_prepareInfo.sampleRate > 0.0
                              ? double(frames) / m_prepareInfo.sampleRate
                              : 0.0;
    if (budget > 0.0) {
        // Smoothed, so the read-out is stable rather than per-block noise.
        const float previous = m_dspLoad.load(std::memory_order_relaxed);
        m_dspLoad.store(previous * 0.9f + float(elapsed / budget) * 0.1f,
                        std::memory_order_relaxed);
    }

    m_rendering.store(false);
}

Status RealtimeEngine::offlineFailure(const CompiledGraph::CompiledNode& entry,
                                      EngineError error, SamplePos position) {
    m_offlineError = std::string(describe(error)) + ": " +
        std::string(entry.node->name()) + " (node " + std::to_string(entry.id) +
        ", sample " + std::to_string(position) + ")";
    return fail(error);
}

Status RealtimeEngine::prepareOfflineGraph() {
    PrepareInfo info = m_prepareInfo;
    info.offline = true;
    auto snapshot = m_processor.graph();
    // Activation and deferred main-thread work can change latency or layout.
    // Stabilise before any file window is computed, with a bounded restart
    // budget so a broken plugin cannot hang preparation forever.
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        if (!snapshot || m_graph.isDirty() ||
            std::any_of(snapshot->nodes.begin(), snapshot->nodes.end(),
                [&](const auto& entry) { return !entry.node->isPreparedFor(info); })) {
            auto compiled = m_graph.compile(info, snapshot.get());
            if (!compiled) return fail(compiled.error());
            snapshot = *compiled;
            m_processor.setGraph(snapshot);
        }
        bool stable = true;
        for (const auto& entry : snapshot->nodes) {
            if (const auto status = entry.node->serviceOffline(); !status)
                return offlineFailure(entry, status.error(), 0);
            stable &= entry.node->isPreparedFor(info);
        }
        if (!stable) continue;
        for (const auto& entry : snapshot->nodes)
            if (const auto status = entry.node->offlineStatus(); !status)
                return offlineFailure(entry, status.error(), 0);
        return {};
    }
    m_offlineError = "audio processor did not finish preparing for export";
    return fail(EngineError::RenderConfigurationChanged);
}

Status RealtimeEngine::renderOffline(
    SamplePos startSample, SamplePos endSample, FrameCount blockSize,
    const std::function<bool(const AudioBlock&, FrameCount)>& sink,
    OfflineOptions options) {
    const RenderGate renderGate(*this);
    if (endSample <= startSample) return fail(EngineError::InvalidArgument);
    const FrameCount block = std::min(blockSize, m_prepareInfo.maxBlockSize);
    if (block == 0) return fail(EngineError::BlockTooLarge);
    if (m_offlineActive) return fail(EngineError::NotCompiled);
    const auto original = m_processor.graph();
    if (!original) return fail(EngineError::NotCompiled);
    struct OfflineScope {
        bool& active;
        explicit OfflineScope(bool& flag) : active(flag) { active = true; }
        ~OfflineScope() { active = false; }
    } offlineScope(m_offlineActive);
    m_offlineError.clear();

    PrepareInfo offlineInfo = m_prepareInfo;
    m_lastOfflinePipeline = false;
    offlineInfo.offline = true;
    const auto clearDelays = [](const CompiledGraph& graph) {
        for (const auto& delay : graph.delays) delay->reset();
        for (const auto& delay : graph.midiDelays) delay->reset();
    };
    const auto restore = [&] {
        // A live engine needs a newly compiled realtime graph, not just nodes
        // switched back underneath the offline delay lines. An isolated
        // controller remains offline for its whole lifetime.
        if (!m_prepareInfo.offline) {
            auto compiled = m_graph.compile(m_prepareInfo);
            if (!compiled) throw std::runtime_error(std::string(describe(compiled.error())));
            m_processor.setGraph(*compiled);
        }
        if (const auto graph = m_processor.graph()) clearDelays(*graph);
    };
    AudioBlock output(m_offlinePointers.data(), m_prepareInfo.channels, block);
    Status result;
    try {
        // Also clears a previous pass's failure before retrying this engine.
        for (const auto& entry : original->nodes) entry.node->reset();
        result = prepareOfflineGraph();
        if (result) {
            const auto snapshot = m_processor.graph();
            for (const auto& entry : snapshot->nodes) entry.node->reset();
            clearDelays(*snapshot);
            const auto serviceNode = [&](std::uint32_t node, SamplePos position) -> Status {
                const auto& entry = snapshot->nodes[node];
                if (const auto status = entry.node->serviceOffline(); !status)
                    return offlineFailure(entry, status.error(), position);
                if (!entry.node->isPreparedFor(offlineInfo))
                    return offlineFailure(entry, EngineError::RenderRestartRequired, position);
                if (const auto status = entry.node->offlineStatus(); !status)
                    return offlineFailure(entry, status.error(), position);
                return {};
            };
            const auto service = [&](SamplePos position) -> Status {
                for (std::uint32_t node = 0; node < snapshot->nodes.size(); ++node)
                    if (auto status = serviceNode(node, position); !status) return status;
                return {};
            };
            result = service(startSample);
            SamplePos position = startSample;
            const auto framesAt = [&](SamplePos at) {
                auto frames = FrameCount(std::min<SamplePos>(block, endSample - at));
                if (at < options.sourcesEndSample)
                    frames = FrameCount(std::min<SamplePos>(frames, options.sourcesEndSample - at));
                return frames;
            };
            bool stopped = false;
            std::vector<double> costs;
            if (result && options.pipeline && !options.forcePipeline &&
                offlinePipelineWindow(*snapshot, m_processor.workerCount()) != 0) {
                costs.assign(snapshot->nodes.size(), 0.0);
                std::vector<double> observation(costs.size());
                unsigned measured = 0;
                // These are real output blocks, with the same boundaries and
                // events as the reference, not a speculative/reset trial pass.
                for (; measured < 3 && result && position < endSample; ++measured) {
                    const auto frames = framesAt(position);
                    result = m_processor.processSerial(output, frames, position,
                        position < options.sourcesEndSample, true, m_transport.infoAt(position), observation);
                    if (!result) break;
                    result = service(position);
                    if (!result) break;
                    if (!sink(output, frames)) { stopped = true; break; }
                    for (std::size_t node = 0; node < costs.size(); ++node) costs[node] += observation[node];
                    position += frames;
                }
                if (measured != 0) for (auto& cost : costs) cost /= measured;
            }
            if (result && !stopped && position < endSample && options.pipeline) {
                if (auto pipelined = renderOfflinePipelined(*snapshot, m_processor.workerCount(),
                    position, endSample, block, options.sourcesEndSample, m_transport, sink, serviceNode, costs)) {
                    m_lastOfflinePipeline = true;
                    result = *pipelined;
                }
            }
            for (; !m_lastOfflinePipeline && !stopped && result && position < endSample;) {
                const auto frames = framesAt(position);
                const TransportInfo transport = m_transport.infoAt(position);
                result = m_processor.process(output, frames, position,
                    position < options.sourcesEndSample, true, transport);
                if (!result) break;
                // Workers are finished. Both error inspection and format
                // main-thread callbacks happen here, before a sink can write
                // this block; nothing is added to the realtime device path.
                result = service(position);
                if (!result || !sink(output, frames)) break;
                position += frames;
            }
        }
    } catch (...) {
        try { restore(); } catch (...) { m_processor.setGraph({}); }
        throw;
    }
    try { restore(); } catch (...) { m_processor.setGraph({}); throw; }
    return result;
}

} // namespace daw::engine
