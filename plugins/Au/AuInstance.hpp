#pragma once

#include "Host/PluginInstance.hpp"

#include <AudioToolbox/AudioToolbox.h>

#include <atomic>
#include <string>
#include <vector>

namespace daw::plugins {

/// An Audio Unit behind the format-agnostic interface.
///
/// AU is the odd one of the three: the host does not hand the plugin its input,
/// the plugin *pulls* it through a render callback. So `process` parks the
/// caller's pointers where the callback can find them and then asks the unit to
/// render — the buffers are only valid for the duration of that one call, which
/// is exactly as long as the callback can run.
///
/// The one genuinely nice thing about AU: parameters are already in plain units
/// and already sample-accurate (`AudioUnitSetParameter` takes a frame offset),
/// so none of VST3's normalisation dance is needed.
class AuInstance final : public PluginInstance {
public:
    AuInstance(AudioComponentInstance unit, PluginDescriptor descriptor);
    ~AuInstance() override;

    /// Second half of construction: reads the parameter list and the bus
    /// layout, which can fail on a component that registered but is broken.
    bool initialize();

    const PluginDescriptor& descriptor() const noexcept override { return m_descriptor; }
    void setListener(PluginListener* listener) noexcept override {
        m_listener.store(listener, std::memory_order_release);
    }

    bool setBusLayout(const PluginBusLayout& wanted, PluginBusLayout& accepted) override;
    PluginBusLayout busLayout() const override;

    bool activate(const PluginProcessInfo& info) override;
    void deactivate() override;
    bool isActive() const noexcept override { return m_active; }
    void startProcessing() override;
    void stopProcessing() override;

    std::span<const ParameterInfo> parameters() const noexcept override {
        return m_parameters;
    }
    std::int32_t parameterIndexForId(std::string_view id) const noexcept override;
    double parameterValue(std::uint32_t index) const noexcept override;
    std::string parameterText(std::uint32_t index, double plainValue) const override;

    bool saveState(std::vector<std::uint8_t>& out) const override;
    bool loadState(std::span<const std::uint8_t> state) override;

    bool hasEditor() const noexcept override;
    bool openEditor(void* parentHandle, PluginEditorHost* host) override;
    void closeEditor() override;
    bool isEditorOpen() const noexcept override { return m_editorView != nullptr; }
    bool editorSize(std::uint32_t& width, std::uint32_t& height) const override;
    bool editorCanResize() const override;
    bool setEditorSize(std::uint32_t& width, std::uint32_t& height) override;

    PluginProcessDisposition process(
        const PluginProcessContext& context) noexcept override;
    void reset() noexcept override;

    std::uint32_t latencySamples() const noexcept override {
        return m_latency.load(std::memory_order_relaxed);
    }
    std::uint32_t tailSamples() const noexcept override {
        return m_tail.load(std::memory_order_relaxed);
    }

private:
    void readParameters();
    void readBuses();
    void refreshLatency();
    bool setStreamFormat(AudioUnitScope scope, AudioUnitElement element,
                         std::uint32_t channels);

    /// The unit pulling its input. Reads whatever `process` parked.
    static OSStatus renderInput(void* context, AudioUnitRenderActionFlags* flags,
                                const AudioTimeStamp* timeStamp, UInt32 bus,
                                UInt32 frames, AudioBufferList* data);
    /// Fired when the unit changes something the host has to follow.
    static void propertyChanged(void* context, AudioUnit unit, AudioUnitPropertyID property,
                                AudioUnitScope scope, AudioUnitElement element);

    // ── Musical time ──
    //
    // AU is pull-shaped here too: the host does not push tempo and position
    // into the unit, it installs three callbacks the unit asks *during render*.
    // Without them a synced delay, an arpeggiator or a step sequencer has no
    // idea what the tempo is or whether the transport is rolling — it loads,
    // it opens, and it does nothing, which is exactly the complaint that sent
    // us looking. They run on the render thread, inside `AudioUnitRender`, and
    // read the transport `process` parked for this block.
    static OSStatus beatAndTempo(void* context, Float64* outBeat, Float64* outTempo);
    static OSStatus musicalTimeLocation(void* context,
                                        UInt32* outDeltaSampleOffsetToNextBeat,
                                        Float32* outTimeSigNumerator,
                                        UInt32* outTimeSigDenominator,
                                        Float64* outCurrentMeasureDownBeat);
    static OSStatus transportState(void* context, Boolean* outIsPlaying,
                                   Boolean* outTransportStateChanged,
                                   Float64* outCurrentSampleInTimeLine,
                                   Boolean* outIsCycling, Float64* outCycleStartBeat,
                                   Float64* outCycleEndBeat);
    void installHostCallbacks();

    AudioComponentInstance m_unit = nullptr;
    PluginDescriptor m_descriptor;

    std::vector<ParameterInfo> m_parameters;
    /// AU addresses parameters by an opaque id, unrelated to their order.
    std::vector<AudioUnitParameterID> m_parameterIds;

    std::atomic<PluginListener*> m_listener{nullptr};
    std::atomic<std::uint32_t> m_latency{0};
    std::atomic<std::uint32_t> m_tail{0};

    /// Only a music device or music effect has a MIDI entry point at all.
    bool m_acceptsMidi = false;
    bool m_active = false;
    bool m_processing = false;
    std::uint32_t m_maxBlockSize = 0;
    double m_sampleRate = 48000.0;
    std::uint32_t m_inputChannels = 2;
    std::uint32_t m_outputChannels = 2;
    std::vector<std::uint32_t> m_inputBusChannels;

    /// The transport for the block being rendered. Written by `process` and
    /// read by the three host callbacks above, on the same thread and inside
    /// the same call — no synchronisation needed, and none would help.
    engine::TransportInfo m_blockTransport;
    std::int64_t m_blockSampleTime = 0;

    /// The unit's own render clock, in samples, advanced by one block on every
    /// `process` call and reset only by `activate`/`reset`.
    ///
    /// **Not** the timeline position, which is what used to be handed to
    /// `AudioUnitRender`. Core Audio's `mSampleTime` describes the render
    /// stream, not the music: a unit that sees the same sample time twice is
    /// entitled to treat the second call as a repeat of the first, and several
    /// — Apple's own among them — answer it with silence. With the transport
    /// stopped the timeline position never moves, so every AU on a monitored
    /// channel muted it. Where the *music* is still reaches the unit, through
    /// the three host callbacks, which read `m_blockTransport`.
    std::int64_t m_renderTime = 0;
    bool m_blockPlaying = false;
    bool m_wasPlaying = false;

    /// Valid only inside `process`; the render callback reads them from there.
    const float* const* m_blockInputs = nullptr;
    std::uint16_t m_blockInputChannels = 0;
    std::uint64_t m_blockInputSilenceMask = 0;
    const float* const* m_blockSidechainInputs = nullptr;
    std::uint16_t m_blockSidechainInputChannels = 0;
    std::uint64_t m_blockSidechainSilenceMask = 0;
    std::uint32_t m_blockFrames = 0;

    /// The output buffer list handed to `AudioUnitRender`, plus somewhere for
    /// channels the caller has not supplied — same rule as the other formats.
    std::vector<std::uint8_t> m_outputListStorage;
    std::vector<float> m_scratch;
    std::vector<float> m_silence;

    /// `NSView*`, but this header is included from plain C++.
    void* m_editorView = nullptr;
    void* m_editorFactory = nullptr;
    PluginEditorHost* m_editorHost = nullptr;
};

} // namespace daw::plugins
