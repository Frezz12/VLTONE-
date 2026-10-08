#pragma once

#include "Host/PluginInstance.hpp"
#include "Internal/SlicerParams.hpp"
#include "Internal/SlicerVoice.hpp"
#include "Common/RealtimeSnapshot.hpp"
#include "DSP/DeClick.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace daw::plugins::slicer {

struct RuntimeState {
    std::shared_ptr<const SampleData> sample;
    std::shared_ptr<const SliceTable> table;
    std::uint64_t sourceRevision = 0;
};

/// The built-in slicer, as a hosted plugin.
///
/// It implements `PluginInstance` rather than being a bespoke engine node, and
/// that is the whole design decision: the instrument slot, the parameter
/// surface, automation lanes, state chunks in the project package, bypass and
/// the delay compensation already work for anything wearing this interface. A
/// native node would have needed all of it written again, in a second shape.
///
/// Threading follows the interface's split exactly. `process` is realtime and
/// allocates nothing; the control thread decodes the sample and publishes both
/// the audio and the chop table as immutable snapshots. Unlike the sampler
/// there is no bake worker to own — a chop is an index range into the decode,
/// so nothing about it needs re-rendering when a knob moves.
class SlicerInstance final : public PluginInstance {
public:
    SlicerInstance();

    /// The descriptor the factory hands out, and what a project stores.
    static const PluginDescriptor& staticDescriptor() noexcept;
    /// `PluginDescriptor::uid` of the slicer. The app compares against this to
    /// know it can open its own editor for a slot.
    static std::string_view uid() noexcept;

    // ── PluginInstance ──
    const PluginDescriptor& descriptor() const noexcept override { return m_descriptor; }
    /// Channel bend only. A chop has one pitch for its whole life — there is no
    /// per-note expression to advertise, and claiming it would make the host
    /// send NotePitch events this implementation has nowhere to put.
    PitchCapabilities pitchCapabilities() const noexcept override { return {false, false, true, false}; }
    void setListener(PluginListener* listener) noexcept override { m_listener = listener; }

    bool setBusLayout(const PluginBusLayout& wanted, PluginBusLayout& accepted) override;
    PluginBusLayout busLayout() const override;

    bool activate(const PluginProcessInfo& info) override;
    void deactivate() override;
    bool isActive() const noexcept override { return m_active; }
    void startProcessing() override { m_processing = true; }
    void stopProcessing() override;

    std::span<const ParameterInfo> parameters() const noexcept override;
    std::int32_t parameterIndexForId(std::string_view id) const noexcept override;
    double parameterValue(std::uint32_t index) const noexcept override;
    std::string parameterText(std::uint32_t index, double plainValue) const override;

    bool saveState(std::vector<std::uint8_t>& out) const override;
    bool loadState(std::span<const std::uint8_t> state) override;

    /// Project packages keep the sample itself in Content/. These variants
    /// store only its portable basename and resolve that basename against the
    /// package on load; ordinary plugin/preset state continues to use the
    /// absolute path through saveState/loadState above.
    bool saveProjectState(std::vector<std::uint8_t>& out,
                          const std::string& packagedSampleName) const;
    bool loadProjectState(std::span<const std::uint8_t> state,
                          const std::string& contentDirectory);
    void setParameterFromHost(std::uint32_t index, double plainValue) override;

    /// False: the host draws the slicer's editor itself, in Qt, from the
    /// parameter surface below. A plugin GUI here would mean an embedded native
    /// view for a panel that is already part of the application.
    bool hasEditor() const noexcept override { return false; }
    bool openEditor(void*, PluginEditorHost*) override { return false; }
    void closeEditor() override {}
    bool isEditorOpen() const noexcept override { return false; }
    bool editorSize(std::uint32_t&, std::uint32_t&) const override { return false; }
    bool editorCanResize() const override { return false; }
    bool setEditorSize(std::uint32_t&, std::uint32_t&) override { return false; }

    PluginProcessDisposition process(
        const PluginProcessContext& context) noexcept override;
    void reset() noexcept override;
    std::uint32_t latencySamples() const noexcept override { return 0; }
    std::uint32_t tailSamples() const noexcept override;

    // ── Slicer-specific, control thread ──

    /// Decode `path` and publish the audio. Returns false when there is no
    /// decoder installed or the file will not read; the previous sample is then
    /// left alone.
    bool loadSample(const std::string& path);
    bool adoptSample(const std::string& path,
                     std::shared_ptr<const engine::SampleBuffer> decoded);
    void clearSample();
    std::string samplePath() const;
    std::string sampleName() const;
    /// What the voices are playing right now. Null when nothing is loaded.
    std::shared_ptr<const SampleData> sample() const;
    /// The raw decode, what the chop analysis reads. Null when nothing is
    /// loaded.
    std::shared_ptr<const engine::SampleBuffer> rawSample() const;

    /// Publish a new chop table. Replaces the whole thing: a note must not be
    /// able to catch a table halfway through a re-slice. The table is retained
    /// unchanged — this takes a shared pointer so the audio thread can hold the
    /// old one for the duration of a block that started reading it.
    void setSliceTable(std::shared_ptr<const SliceTable> table);
    /// The current table, for the panel to draw. Null when nothing is sliced.
    std::shared_ptr<const SliceTable> sliceTable() const;
    AnalysisSettings analysisSettings() const { return m_analysis; }
    void setAnalysisSettings(const AnalysisSettings& settings) { m_analysis = settings; }
    ControlState captureState() const;
    void restoreState(const ControlState& state);
    std::uint64_t sourceRevision() const;
    bool keyActive(int key) const noexcept;

    /// Control-thread parameter write. Same effect as an event arriving on the
    /// audio thread, and the path the slicer's own editor uses.
    void setParameter(std::uint32_t index, double plainValue);

private:
    /// Read the parameter array into the plain snapshot a block's voices use.
    SlicerSettings snapshot() const noexcept;
    void applyEvent(const PluginEvent& event, std::uint32_t frameOffset) noexcept;
    void noteOn(int key, int channel, float velocity, float pan,
                std::int32_t id = -1) noexcept;
    void noteOff(int key, int channel, std::int32_t id = -1) noexcept;
    double value(Param parameter) const noexcept {
        return m_values[indexOf(parameter)].load(std::memory_order_relaxed);
    }
    /// Render `frames` starting at `offset` into the context's outputs.
    void renderSlice(const PluginProcessContext& context,
                     const SlicerSettings& settings,
                     std::uint32_t offset, std::uint32_t frames) noexcept;
    /// Volume and Drive, applied once to the summed output.
    void applyOutputStage(float* const* out, engine::ChannelCount channels,
                          engine::FrameCount frames) noexcept;

    static constexpr std::size_t kMaxVoices = 32;

    PluginDescriptor m_descriptor;
    PluginListener* m_listener = nullptr;

    double m_sampleRate = 48000.0;
    std::uint32_t m_maxBlockSize = 512;
    bool m_active = false;
    bool m_processing = false;

    /// Plain values, indexed exactly like `parameterTable()`. Atomic because
    /// the editor reads them while the audio thread applies events; relaxed
    /// ordering is enough — a knob read one block late is not a race anybody
    /// can hear.
    std::array<std::atomic<double>, kParameterCount> m_values;

    /// Published snapshots. Loaded once per block by `process` and kept alive
    /// for its duration, so the control thread may replace either at any time.
    engine::RealtimeSnapshot<RuntimeState> m_runtime;
    const RuntimeState* m_renderRuntime = nullptr; // audio thread, one read per process
    std::uint64_t m_nextSourceRevision = 0;
    std::uint64_t m_renderSourceRevision = 0;
    engine::dsp::DeClick m_sourceTransition;
    bool m_renderOffline = false;
    void resetVoices() noexcept;
    AnalysisSettings m_analysis;
    std::array<std::atomic<std::uint64_t>, 2> m_activeKeys{};
    double m_crushHeld[engine::kMaxChannels]{};
    int m_crushCounter = 0;
    double m_outputVolume = 1.0, m_outputDrive = 0.0, m_outputCrushMix = 0.0;
    bool m_outputInitialized = false;
    std::atomic<double> m_sliceRelease{0.0};

    std::shared_ptr<const engine::SampleBuffer> m_raw;
    std::string m_samplePath;
    std::string m_sampleName;

    Voice m_voices[kMaxVoices];
    std::uint64_t m_voiceStamp = 0;
    double m_channelBend[16]{};
};

} // namespace daw::plugins::slicer
