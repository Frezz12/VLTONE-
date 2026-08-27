#pragma once

#include "Host/PluginInstance.hpp"
#include "Internal/SamplerParams.hpp"
#include "Internal/SamplerPrecompute.hpp"
#include "Internal/SamplerVoice.hpp"
#include "Common/RealtimeSnapshot.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace daw::plugins::sampler {

/// The built-in sampler, as a hosted plugin.
///
/// It implements `PluginInstance` rather than being a bespoke engine node, and
/// that is the whole design decision: the instrument slot, the parameter
/// surface, automation lanes, state chunks in the project package, bypass and
/// the delay compensation already work for anything wearing this interface. A
/// native node would have needed all of it written again, in a second shape.
///
/// Threading follows the interface's split exactly. `process` is realtime and
/// allocates nothing; the control thread decodes and snapshots requests, while
/// a private worker bakes the O(sample length) effects. Results are published
/// to the audio thread as immutable `SampleData` snapshots.
class SamplerInstance final : public PluginInstance {
public:
    SamplerInstance();
    ~SamplerInstance() override;

    /// The descriptor the factory hands out, and what a project stores.
    static const PluginDescriptor& staticDescriptor() noexcept;
    /// `PluginDescriptor::uid` of the sampler. The app compares against this to
    /// know it can open its own editor for a slot.
    static std::string_view uid() noexcept;

    // ── PluginInstance ──
    const PluginDescriptor& descriptor() const noexcept override { return m_descriptor; }
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
    void pumpMainThread() override;

    /// False: the host draws the sampler's editor itself, in Qt, from the
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

    // ── Sampler-specific, control thread ──

    /// Decode `path`, publish a raw fallback, and queue the precomputed effects.
    /// Returns false when there is no decoder installed or the file will not
    /// read; the previous sample is then left alone.
    bool loadSample(const std::string& path);
    void clearSample();
    std::string samplePath() const;
    std::string sampleName() const;
    /// What the voices are playing right now — the editor draws this, tail and
    /// precomputed effects included, because that is what is heard.
    std::shared_ptr<const SampleData> sample() const;
    /// The file as decoded, before the precomputed stage. Null when nothing is
    /// loaded.
    std::shared_ptr<const engine::SampleBuffer> rawSample() const;

    /// Control-thread parameter write. Same effect as an event arriving on the
    /// audio thread, and the path the sampler's own editor uses.
    void setParameter(std::uint32_t index, double plainValue);

    /// Re-bake the precomputed effects now, instead of on the next
    /// `pumpMainThread`, and wait for the latest generation. The actual DSP
    /// still runs on the bake worker; offline/play/export callers use this as a
    /// synchronous flush without moving O(sample length) work onto themselves.
    void rebuildProcessedSample();
    void flushPendingPrecompute();
    bool precomputePending() const noexcept;

private:
    /// Read the parameter array into the plain snapshot a block's voices use.
    SamplerSettings snapshot() const noexcept;
    void applyEvent(const PluginEvent& event, std::uint32_t frameOffset) noexcept;
    void noteOn(int key, int channel, float velocity) noexcept;
    void noteOff(int key, int channel) noexcept;
    /// Render `frames` starting at `offset` into the context's outputs.
    void renderSlice(const PluginProcessContext& context, const SamplerSettings& settings,
                     std::uint32_t offset, std::uint32_t frames) noexcept;

    struct BakeRequest {
        std::uint64_t generation = 0;
        std::shared_ptr<const engine::SampleBuffer> raw;
        std::string path;
        std::string name;
        PrecomputeSettings settings;
        bool keepOnDisk = false;
    };
    void markPrecomputeDirty() noexcept;
    PrecomputeSettings precomputeSettings() const noexcept;
    BakeRequest bakeRequest(std::uint64_t generation) const;
    void schedulePendingPrecompute();
    void bakeWorkerLoop();
    void publishRawSample();

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

    /// Published snapshot. Loaded once per block by `process` and kept alive
    /// for its duration, so the control thread may replace it at any time.
    engine::RealtimeSnapshot<SampleData> m_sample;
    /// The decode, before the precomputed stage — kept so changing a
    /// precomputed knob re-bakes from the original instead of compounding.
    std::shared_ptr<const engine::SampleBuffer> m_raw;
    std::string m_samplePath;
    std::string m_sampleName;

    // An audio-thread parameter event only advances these atomics. Queueing,
    // allocation and SampleData publication happen on the control/bake threads.
    std::atomic<bool> m_precomputeDirty{false};
    std::atomic<std::uint64_t> m_precomputeGeneration{0};
    std::atomic<std::uint64_t> m_completedPrecomputeGeneration{0};

    mutable std::mutex m_bakeMutex;
    std::condition_variable m_bakeChanged;
    std::optional<BakeRequest> m_pendingBake;
    bool m_stopBakeWorker = false;
    std::thread m_bakeWorker;

    Voice m_voices[kMaxVoices];
    std::uint64_t m_voiceStamp = 0;
    /// Free-running LFO phase per target, in cycles, for the Global switch.
    double m_globalPhase[kModTargetCount] = {};
};

} // namespace daw::plugins::sampler
