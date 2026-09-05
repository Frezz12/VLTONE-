#pragma once

#include "Graph/AudioGraph.hpp"
#include "Graph/GraphProcessor.hpp"
#include "Nodes/PlaybackNodes.hpp"
#include "Transport/Transport.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>

namespace daw::engine {

/// Knobs for one offline pass. At namespace scope because a default argument
/// cannot use a nested type's default member initializer while the enclosing
/// class is still being defined.
struct OfflineOptions {
    /// From this position on, blocks are rendered with `playing = false`. Clip
    /// and MIDI players return immediately on that flag while effects keep
    /// processing, so this is how a render captures a reverb tail: the
    /// arrangement stops, the decay does not. The default keeps sources playing
    /// for the whole pass.
    SamplePos sourcesEndSample = std::numeric_limits<SamplePos>::max();
};

/// The engine's public face: owns the graph, the scheduler and the transport,
/// and turns one device callback into one rendered block.
///
/// Everything the audio thread touches is either preallocated or published
/// atomically, so the control thread can rebuild routing, load clips or change
/// tempo while audio keeps running.
class RealtimeEngine {
public:
    static constexpr std::size_t kMasterSpectrumBandCount = 12;
    using MasterSpectrum = std::array<float, kMasterSpectrumBandCount>;

    /// Parks the renderer at a block boundary. While one of these is held
    /// `renderBlock` returns silence without touching the graph, and the
    /// constructor does not return until the block already in flight has
    /// finished — so the control thread may then do things a live node cannot
    /// survive: reallocate its scratch in `prepare()`, activate or destroy a
    /// hosted plugin, change the sample rate.
    ///
    /// Control thread only, and it blocks for up to one block period. Routine
    /// routing edits must NOT take one: they do not re-prepare anything (see
    /// `Node::isPreparedFor`), and parking the renderer for them would put a
    /// hole in the audio on every click in the mixer.
    class RenderGate {
    public:
        explicit RenderGate(RealtimeEngine& engine);
        ~RenderGate();
        RenderGate(const RenderGate&) = delete;
        RenderGate& operator=(const RenderGate&) = delete;

    private:
        RealtimeEngine& m_engine;
        std::unique_lock<std::recursive_mutex> m_lock;
    };

    explicit RealtimeEngine(unsigned threadCount = 0);
    ~RealtimeEngine();

    /// Control thread. Sizes buffers and re-prepares every node, so it parks
    /// the renderer for the duration.
    Status prepare(SampleRate sampleRate, FrameCount maxBlockSize,
                   ChannelCount channels = 2);

    SampleRate sampleRate() const noexcept { return m_prepareInfo.sampleRate; }
    FrameCount maxBlockSize() const noexcept { return m_prepareInfo.maxBlockSize; }
    ChannelCount channels() const noexcept { return m_prepareInfo.channels; }

    Transport& transport() noexcept { return m_transport; }
    const Transport& transport() const noexcept { return m_transport; }

    /// The editable graph. After changing it call `commitGraph()`.
    AudioGraph& graph() noexcept { return m_graph; }

    /// Compile and publish. The audio thread picks the new graph up on its next
    /// block; the old one stays alive until that block is finished.
    Status commitGraph(bool reconfigureNodes = false);

    /// The graph currently driving audio — for diagnostics and tests.
    std::shared_ptr<const CompiledGraph> compiledGraph() const {
        return m_processor.graph();
    }

    /// Where live input arrives from. Valid only during `renderBlock`.
    const InputBus* inputBus() const noexcept { return &m_inputBus; }

    /// Audio thread: render one block. `input` may be null.
    void renderBlock(const AudioBlock& output, const float* const* input,
                     ChannelCount inputChannels, FrameCount frames);

    /// Control thread: render `frames` starting at `startSample` as fast as the
    /// machine allows, with the realtime constraints lifted. Used for mixdown,
    /// freeze and bounce. `sink` receives each block.
    ///
    /// The sink returns false to end the pass early — the one mechanism behind
    /// both cancelling an export and stopping a tail once it has decayed. A
    /// pass stopped that way still reports success; the caller knows it asked.
    Status renderOffline(SamplePos startSample, SamplePos endSample,
                         FrameCount blockSize,
                         const std::function<bool(const AudioBlock&, FrameCount)>& sink,
                         OfflineOptions options = {});

    /// Master output level of the last block (0…1), for the UI.
    float masterPeakLeft() const noexcept { return m_masterPeakL.load(std::memory_order_relaxed); }
    float masterPeakRight() const noexcept { return m_masterPeakR.load(std::memory_order_relaxed); }
    /// Log-spaced master-bus energy from 63 Hz to 10 kHz. Each value is a
    /// smoothed linear RMS level; the UI is responsible only for its dB-to-pixel
    /// mapping, never for inventing frequency motion from a broadband peak.
    MasterSpectrum masterSpectrum() const noexcept;
    /// Register a live reader of the spectrum display. Peak meters remain
    /// active unconditionally, but the twelve band-pass filters are skipped
    /// while this count is zero.
    void addMasterSpectrumConsumer() noexcept {
        m_masterSpectrumConsumers.fetch_add(1, std::memory_order_release);
    }
    void removeMasterSpectrumConsumer() noexcept;

    /// Fraction of the block budget the last render consumed (0…1+).
    float dspLoad() const noexcept { return m_dspLoad.load(std::memory_order_relaxed); }

    /// Compensation latency the published graph introduces.
    FrameCount latencySamples() const { return m_processor.latencySamples(); }
    unsigned workerCount() const noexcept { return m_processor.workerCount(); }

    /// Called on the audio thread after every block; used for recording taps.
    void setBlockObserver(std::function<void(const AudioBlock&, FrameCount)> observer) {
        m_blockObserver = std::move(observer);
    }

private:
    struct SpectrumFilterState {
        double x1 = 0.0;
        double x2 = 0.0;
        double y1 = 0.0;
        double y2 = 0.0;
    };
    struct SpectrumBandFilter {
        double b0 = 0.0;
        double b2 = 0.0;
        double a1 = 0.0;
        double a2 = 0.0;
        std::array<SpectrumFilterState, 2> channels;

        double process(float input, std::size_t channel) noexcept;
        void reset() noexcept;
    };

    void prepareMasterSpectrum(SampleRate sampleRate) noexcept;
    void updateMasterMeters(const AudioBlock& output, FrameCount frames) noexcept;

    AudioGraph m_graph;
    GraphProcessor m_processor;
    Transport m_transport;
    PrepareInfo m_prepareInfo;

    InputBus m_inputBus;
    std::function<void(const AudioBlock&, FrameCount)> m_blockObserver;

    std::atomic<float> m_masterPeakL{0.0f};
    std::atomic<float> m_masterPeakR{0.0f};
    std::array<SpectrumBandFilter, kMasterSpectrumBandCount> m_spectrumFilters;
    std::array<std::atomic<float>, kMasterSpectrumBandCount> m_masterSpectrum{};
    std::atomic<unsigned> m_masterSpectrumConsumers{0};
    /// Audio-thread-owned transition flag. It makes disabling the display pay
    /// for filter/value reset once, rather than on every subsequent block.
    bool m_masterSpectrumActive = false;
    std::atomic<float> m_dspLoad{0.0f};

    // RenderGate handshake. Both sides are seq_cst on purpose: the control
    // thread stores `gateRequested` then loads `rendering`, the audio thread
    // stores `rendering` then loads `gateRequested`, and anything weaker lets
    // the store-load pair reorder so that both read "clear" and the gate
    // returns while a block is still running.
    std::atomic<bool> m_gateRequested{false};
    std::atomic<bool> m_rendering{false};
    // Only control threads lock this mutex. Nested gates retain the outer
    // owner's exclusion, and different control threads cannot overlap.
    std::recursive_mutex m_controlMutex;
    unsigned m_gateDepth = 0;
    bool m_offlineActive = false;

    // Offline rendering borrows this instead of the device's output buffer.
    std::vector<float> m_offlineStorage;
    std::vector<float*> m_offlinePointers;
};

} // namespace daw::engine
