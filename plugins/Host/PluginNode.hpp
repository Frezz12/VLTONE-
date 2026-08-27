#pragma once

#include "Common/LockFreeQueue.hpp"
#include "Common/RealtimeSnapshot.hpp"
#include "Graph/Node.hpp"
#include "Host/PluginInstance.hpp"

#include <atomic>
#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace daw::plugins {

/// One hosted plugin as a graph node.
///
/// Three engine facts shape everything here:
///
///  * **The output buffer arrives dirty.** The arena is zeroed once and its
///    buffers are recycled, so whatever the previous owner wrote is still in
///    there. The node must fully write or clear every channel of every block.
///  * **Channels are cache-line padded**, so `data(ch+1) - data(ch)` is not
///    `frames`. Channel pointers are collected one at a time, never derived
///    from a base pointer and a stride.
///  * **Output never aliases an input** — `AudioGraph::compile` guarantees it —
///    so the plugin gets distinct in and out pointers with no bounce buffer.
///
/// Bypass deliberately does **not** change the reported latency. Changing it
/// would dirty the graph and force a recompile on a control users toggle
/// rhythmically; reporting a constant latency costs a few samples of needless
/// delay and buys a click-free toggle.
class PluginNode final : public engine::Node, public PluginListener {
public:
    PluginNode(std::string name, std::unique_ptr<PluginInstance> instance);
    ~PluginNode() override;

    std::string_view name() const noexcept override { return m_name; }
    bool isSource() const noexcept override { return m_isSource; }
    engine::FrameCount latencySamples() const noexcept override {
        return m_latency.load(std::memory_order_relaxed);
    }
    engine::FrameCount tailSamples() const noexcept override {
        return m_instance ? m_instance->tailSamples() : 0;
    }

    void prepare(const engine::PrepareInfo& info) override;
    void reset() override;
    void suspend() override;
    void resume() override;
    void process(const engine::ProcessContext& context) override;

    // ── Control thread ──

    PluginInstance* instance() noexcept { return m_instance.get(); }
    const PluginInstance* instance() const noexcept { return m_instance.get(); }

    void setBypassed(bool bypassed) noexcept {
        m_bypassed.store(bypassed, std::memory_order_relaxed);
    }
    void setMix(float mix) noexcept {
        m_mix.store(std::clamp(mix, 0.0f, 1.0f), std::memory_order_relaxed);
    }
    bool isBypassed() const noexcept {
        return m_bypassed.load(std::memory_order_relaxed);
    }
    bool isReady() const noexcept { return m_ready.load(std::memory_order_acquire); }

    /// Main-bus width requested from the plugin when this node is prepared.
    /// The graph arena remains stereo; this selects the plugin's mono/stereo
    /// processing variant and PluginNode adapts around a refusal.
    void setPreferredChannelCount(std::uint16_t channels) noexcept {
        channels = std::clamp<std::uint16_t>(channels, 1, 2);
        if (m_preferredChannelCount.exchange(channels, std::memory_order_acq_rel) !=
            channels) {
            invalidatePrepare();
        }
    }
    std::uint16_t preferredChannelCount() const noexcept {
        return m_preferredChannelCount.load(std::memory_order_acquire);
    }

    /// Queue a parameter change for the next block. Lock-free; returns false
    /// when the ring is full, which means the control thread is producing
    /// faster than the audio thread consumes and the change is dropped rather
    /// than blocking the caller.
    bool pushEvent(const PluginEvent& event) noexcept { return m_inbound.push(event); }

    /// One automated parameter: breakpoints in **beats** from the start of the
    /// timeline, kept sorted.
    ///
    /// Beats, like notes, so a tempo change carries the curve with it. And
    /// evaluated on the audio thread rather than pushed from the control
    /// thread: a curve is a function of the playhead, and the only place that
    /// knows the playhead to the sample is `process`. Pushing it through the
    /// event ring would quantise every automated parameter to the UI's tick.
    struct AutomationCurve {
        std::uint32_t parameterIndex = 0;
        /// What the parameter holds before the first breakpoint.
        double defaultValue = 0.0;
        std::vector<std::pair<double, double>> points;   // (beats, plain value)
    };
    using AutomationCurves = std::vector<AutomationCurve>;

    /// Control thread: publish a new set of curves. Immutable snapshot, same
    /// discipline as the clip and note lists.
    void setAutomation(std::shared_ptr<const AutomationCurves> curves) {
        m_automation.publish(std::move(curves));
    }
    std::shared_ptr<const AutomationCurves> automation() const {
        return m_automation.controlCopy();
    }

    /// Drain what the plugin reported back — parameters it moved in its own
    /// editor, gestures. Control thread.
    bool popNotification(PluginEvent& out) noexcept { return m_outbound.pop(out); }

    /// True once after the plugin's latency changed; the caller must rebuild
    /// the graph for delay compensation to follow.
    bool takeLatencyChanged() noexcept {
        return m_latencyChanged.exchange(false, std::memory_order_acq_rel);
    }
    /// True once after the plugin asked to be restarted wholesale.
    bool takeRestartRequested() noexcept {
        return m_restartRequested.exchange(false, std::memory_order_acq_rel);
    }
    bool takeReloadRequested() noexcept {
        return m_reloadRequested.exchange(false, std::memory_order_acq_rel);
    }
    /// Start one controller-side drain. Clearing before the drain means a
    /// notification racing with it publishes a fresh global generation and is
    /// guaranteed another turn; a burst before it is coalesced into this one.
    void beginMainThreadPump() noexcept {
        m_mainThreadWorkPending.store(false, std::memory_order_release);
    }

    // ── PluginListener, called from the plugin on any thread ──
    void onParameterChanged(std::uint32_t index, double plainValue) noexcept override;
    void onParameterGesture(std::uint32_t index, bool begin) noexcept override;
    void onLatencyChanged() noexcept override;
    void onRestartRequested() noexcept override;
    void onReloadRequested() noexcept override;

private:
    void requestMainThreadPump() noexcept;
    void rememberMidiOutput(const engine::MidiEvent& event) noexcept;
    void releaseHeldMidi(engine::MidiBuffer* output) noexcept;

    /// Adapter handing the plugin's own output events to `m_outbound`.
    class Sink final : public EventSink {
    public:
        explicit Sink(PluginNode& owner) : m_owner(owner) {}
        void push(const PluginEvent& event) noexcept override;

    private:
        PluginNode& m_owner;
    };

    std::string m_name;
    std::unique_ptr<PluginInstance> m_instance;
    Sink m_sink;
    bool m_isSource = false;
    bool m_wantsMidi = false;
    /// Notes that this MIDI-owning plugin successfully emitted downstream. When
    /// bypass switches the route back to the dry stream, releases for the
    /// transformed notes must precede that stream or transposed/arpeggiated
    /// voices can remain held forever. Fixed storage keeps the callback bounded.
    std::array<std::uint16_t, 16 * 128> m_heldMidiOutput{};
    std::uint32_t m_heldMidiOutputCount = 0;

    // Channel pointer arrays and the dry copy, all sized in prepare().
    std::vector<const float*> m_inputPointers;
    std::vector<const float*> m_sidechainPointers;
    std::vector<float*> m_outputPointers;
    std::vector<float> m_inputStorage;    // summed main-bus input
    std::vector<float> m_sidechainStorage; // summed first auxiliary input
    /// Somewhere for the plugin to write output channels the arena does not
    /// have. A 5.1 plugin on a stereo arena writes six channels whatever the
    /// host wants, and the four it has nowhere to put still have to land in
    /// memory this node owns.
    std::vector<float> m_outputStorage;
    std::vector<float> m_dryStorage;      // for the bypass crossfade
    std::vector<float> m_dryDelayStorage; // aligns dry with the plugin latency
    engine::FrameCount m_dryDelaySamples = 0;
    engine::FrameCount m_dryDelayPosition = 0;
    engine::FrameCount m_maxBlockSize = 0;
    engine::ChannelCount m_arenaChannels = 0;
    std::uint16_t m_pluginInputChannels = 0;
    std::uint16_t m_pluginSidechainChannels = 0;
    std::uint16_t m_pluginOutputChannels = 0;
    std::atomic<std::uint16_t> m_preferredChannelCount{2};

    /// Events for one block, gathered from the ring and sorted. Reserved in
    /// prepare(); process() never allocates.
    std::vector<PluginEvent> m_blockEvents;

    engine::RealtimeSnapshot<AutomationCurves> m_automation;

    /// Per-curve scan cursors for the automation pass, so a curve with many
    /// breakpoints is not re-scanned from the start every block. `m_curveCursor[i]`
    /// is the index of the first point of curve `i` whose beat position is past
    /// the last block's start; it only ever moves forward, which makes the whole
    /// pass amortised O(1) per curve per block. The cursors are valid only for
    /// the snapshot they were built against, and only while the playhead moves
    /// forward — both are checked in `process` and reset when they no longer hold.
    std::vector<std::size_t> m_curveCursor;
    const AutomationCurves* m_curveCursorFor = nullptr;
    double m_lastBlockStartBeats = 0.0;

    engine::LockFreeSPSCQueue<PluginEvent, 2048> m_inbound;
    engine::LockFreeMPSCQueue<PluginEvent, 512> m_outbound;

    std::atomic<engine::FrameCount> m_latency{0};
    std::atomic<bool> m_bypassed{false};
    std::atomic<float> m_mix{1.0f};
    std::atomic<bool> m_latencyChanged{false};
    std::atomic<bool> m_restartRequested{false};
    std::atomic<bool> m_reloadRequested{false};
    std::atomic<bool> m_mainThreadWorkPending{false};
    std::atomic<bool> m_ready{false};
    /// Ramps 1 → 0 when bypass engages and back when it lifts, so the switch
    /// is a short crossfade instead of a discontinuity.
    float m_wet = 1.0f;
    /// A sleeping bypassed processor is reset exactly once before its process
    /// callback is skipped. That prevents a frozen reverb/delay tail from
    /// reappearing when bypass is later released.
    bool m_bypassProcessorReset = false;
    /// Set only from an explicit format disposition (currently CLAP). The
    /// audio thread keeps enough transport/tail state to wake without polling
    /// the plugin or allocating.
    bool m_pluginSleeping = false;
    std::uint64_t m_tailFramesRemaining = 0;
    PluginProcessDisposition m_lastProcessDisposition =
        PluginProcessDisposition::Continue;
    bool m_sleepTransportValid = false;
    engine::SamplePos m_sleepTimelinePosition = 0;
    engine::FrameCount m_sleepBlockFrames = 0;
    bool m_sleepPlaying = false;
    bool m_sleepOffline = false;
    engine::TransportInfo m_sleepTransport;
    engine::MidiBuffer* m_currentMidiOutput = nullptr;
};

} // namespace daw::plugins
