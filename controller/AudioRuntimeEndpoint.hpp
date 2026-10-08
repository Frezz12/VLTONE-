#pragma once

#include "AudioRuntime.hpp"
#include "AudioRuntimeProcess.hpp"

#include <chrono>
#include <memory>
#include <stdexcept>

namespace daw {

/// A compound operation failed before its owned reply was acknowledged.
/// Simple commands return failure/empty readouts and publish an error notice.
class AudioEndpointError : public std::runtime_error {
public:
    explicit AudioEndpointError(audio::Result result)
        : std::runtime_error(result.message()), result(std::move(result)) {}
    audio::Result result;
};

/// UI ownership of one numeric session in daw_audio. The shared broker is the
/// sole process client; snapshots never perform IPC on a painting/UI thread.
/// Native execution is available only through an explicit worker/test factory.
class AudioRuntimeEndpoint final {
public:
    // Painting uses cached values. Editing/Undo explicitly requests an
    // acknowledged current value before deciding what to change or retain.
    enum class Readout { Cached, Current };
    explicit AudioRuntimeEndpoint(std::string executable,
        std::chrono::milliseconds timeout = std::chrono::seconds(10));
    ~AudioRuntimeEndpoint();
    AudioRuntimeEndpoint(const AudioRuntimeEndpoint&) = delete;
    AudioRuntimeEndpoint& operator=(const AudioRuntimeEndpoint&) = delete;
    static std::shared_ptr<AudioRuntimeEndpoint> forWorker(std::shared_ptr<AudioRuntime> runtime);
    static std::shared_ptr<AudioRuntimeEndpoint> forTest(std::shared_ptr<AudioRuntime> runtime);
    AudioRuntime& nativeForWorkerOrTest() const;
    bool isRemote() const noexcept;

    struct Metadata {
        double sampleRate = 48000;
        std::uint32_t blockSize = 512;
        bool prepared = false, offline = false, deviceAllowed = false;
        std::uint64_t sessionId = 0, generation = 0, revision = 0;
        bool connected = false, secondary = false;
        std::string error;
    };
    Metadata metadata() const;
    audio::Result prepare(double rate, std::uint32_t frames, bool offline = false);
    audio::Result applySession(AudioSessionSpec session, bool reconfigurePlugins = false,
        std::span<const AudioPluginStateEdit> restores = {},
        std::span<const AudioPluginCheckpoint> checkpoints = {});
    audio::Result replaceSession(AudioSessionSpec session,
        std::span<const AudioPluginStateEdit> restores = {},
        std::span<const AudioPluginCheckpoint> checkpoints = {},
        std::span<const AudioTransportCommand> transport = {});
    audio::Result replacePreparedSession(AudioSessionSpec session, double sampleRate,
        std::uint32_t blockSize, bool offline,
        std::span<const AudioPluginStateEdit> restores = {},
        std::span<const AudioPluginCheckpoint> checkpoints = {},
        std::span<const AudioTransportCommand> transport = {});
    audio::Result createSecondary(std::shared_ptr<AudioRuntimeEndpoint>& out,
        AudioSessionSpec session, std::span<const AudioPluginStateEdit> restores = {},
        std::span<const AudioPluginCheckpoint> checkpoints = {});
    /// Shares the process broker; its first apply creates the numeric session.
    std::shared_ptr<AudioRuntimeEndpoint> createSecondaryEndpoint();
    audio::Result startAudition(const std::shared_ptr<AudioRuntimeEndpoint>& secondary);
    void stopAudition();
    audio::Result closeSession();
    audio::Result restart();
    audio::Result captureRecoveryCheckpoint();
    std::uint64_t processId() const;
    audio::Result advanceForTest(std::uint32_t frames, std::uint32_t blocks = 1,
        std::span<const float> inputChannels = {});
    using TransactionId = std::uint64_t;
    TransactionId captureTransaction();
    audio::Result restoreTransaction(TransactionId token);
    void releaseTransaction(TransactionId token);
    void collectTransactionRetirements(TransactionId token);
    bool hasChannel(const std::string& channelId) const;
    std::vector<AudioPluginAddress> retiringPlugins(std::span<const AudioPluginChainSpec> wanted) const;
    bool requiresPluginPreparation(const AudioPluginAddress& address, const AudioPluginSpec& spec) const;
    std::vector<AudioPluginStateSnapshot> lastImportedPluginStates() const;

    void transportCommand(const AudioTransportCommand& command);
    AudioTransportSnapshot transportSnapshot() const;
    double presentationPositionSeconds() const;
    AudioRuntimeDiagnostics diagnostics() const;
    AudioDeviceSnapshot deviceSnapshot() const;
    AudioPluginServiceResult servicePlugins(bool externallyActive = false);
    audio::Result configureDevice(const audio::AudioDeviceConfig& requested,
        audio::AudioDeviceConfig& actual);
    audio::Result startDevice();
    audio::Result stopDevice();
    audio::Result detachDeviceCallback();
    void closeDevice();
    bool setFader(const std::string& channelId, AudioFaderTarget target,
        const AudioFaderChange& change, const std::string& clipId = {});
    bool setInput(const std::string& channelId, const AudioGraphSpec::Input& input);
    bool setSend(const std::string& channelId, const std::string& sendId, float level, bool enabled);
    bool sendLiveMidi(const std::string& channelId, const engine::MidiEvent& event);
    void previewCommand(const AudioPreviewCommand& command);
    bool openPluginEditor(const AudioPluginAddress& address, void* parent, plugins::PluginEditorHost* host);
    audio::Result showDeviceControlPanel(const std::string& id, void* nativeWindow = nullptr);
    std::weak_ptr<AudioMiniModuleCompiler> miniModuleCompiler() const;

    // BEGIN GENERATED VALUE DECLARATIONS
    double inputBeatsAt(std::uint64_t timestamp) const noexcept;
    AudioMeterSnapshot meterSnapshot(const std::string& channelId,
        const std::string& clipId = {}, bool sampler = false) const;
    void resetMeterHold(const std::string& channelId);
    AudioTimingSnapshot timingSnapshot(bool callback, bool drain);
    void setProfiling(bool enabled);
    bool popProfile(unsigned worker, rt::ProfileEvent& event);
    engine::LoudnessLevels masterLoudness() const;
    void resetMasterLoudness();
    engine::RealtimeEngine::MasterSpectrum masterSpectrum() const;
    void setMasterSpectrumConsumer(bool add);
    std::vector<audio::DeviceInfo> enumerateDevices(bool input);
    audio::DeviceInfo currentDevice(bool input) const;
    audio::AudioDeviceConfig deviceConfiguration() const;
    bool matchesDeviceConfiguration(const audio::AudioDeviceConfig& config) const;
    bool deviceNeedsRecovery() const;
    audio::Result refreshDevices();
    audio::Result probeDevice(const std::string& id, bool input, audio::DeviceInfo& info);
    float inputPeak(std::uint32_t channel) const;
    bool hasInputRoute(const std::string& channelId) const;
    void setMetronomeEnabled(bool enabled);
    void setMetronomeSample(std::shared_ptr<const engine::SampleBuffer> sample);
    void requestCountIn(int beats);
    bool startPreview(std::shared_ptr<const engine::SampleBuffer> sample, bool loop, double pitchSemitones);
    AudioPreviewSnapshot previewSnapshot() const;
    audio::Result startCapture(const AudioCaptureSpec& spec, AudioCaptureStarted& out);
    bool publishCaptures(std::span<const AudioCaptureId> ids);
    bool hasActiveCaptures() const;
    AudioCaptureStatus captureStatus(AudioCaptureId id) const;
    AudioCapturePeaks capturePeaks(AudioCaptureId id, std::uint64_t fromBucket) const;
    bool setCaptureInput(AudioCaptureId id, std::uint32_t first, std::uint32_t count, bool enabled);
    audio::Result stopCapture(AudioCaptureId id, audio::RecordingSession& closed);
    void interruptCapture(AudioCaptureId id);
    engine::PrepareInfo preparation() const;
    bool applyContent(const std::string& channelId, AudioContentSpec content);
    void suspendRecordingClipFx(std::span<const std::string> recordingTracks);
    bool hasPlugin(const AudioPluginAddress& address, std::string_view uid = {}) const;
    std::vector<AudioPluginAddress> pluginAddresses() const;
    AudioPluginStateSnapshot pluginStateSnapshot(const AudioPluginAddress& address,
        bool includeState = true, const std::optional<std::string>& packagedSample = std::nullopt,
        AudioPluginSnapshotPurpose purpose = AudioPluginSnapshotPurpose::Exact);
    std::vector<AudioPluginStateSnapshot> pluginStateSnapshots(std::span<const AudioPluginStateRequest> requests);
    audio::Result restorePluginState(const AudioPluginAddress& address,
        const AudioPluginStateRestore& state, std::vector<InsertParameter>& parameters);
    std::uint64_t pluginInstanceId(const AudioPluginAddress& address) const;
    std::vector<plugins::ParameterInfo> pluginParameters(const AudioPluginAddress& address) const;
    std::optional<plugins::ParameterInfo> pluginParameterInfo(const AudioPluginAddress& address,
        const std::string& parameterId) const;
    AudioPluginCapabilities pluginCapabilities(const AudioPluginAddress& address) const;
    bool setPluginControls(const std::string& channelId, const std::string& slotId,
        const AudioPluginControlChange& change);
    AudioPluginSlideStatus pluginSlideStatus(const AudioPluginAddress& address) const;
    bool setPluginSlide(const AudioPluginAddress& address, int mode, double range, double reserve);
    bool setPluginAutomationOverride(const AudioPluginAddress& address, const std::string& parameterId);
    double pluginParameter(const AudioPluginAddress& address, const std::string& parameterId,
        Readout readout = Readout::Cached) const;
    bool setPluginParameter(const AudioPluginAddress& address, const std::string& parameterId, double value);
    void readPluginParameters(const AudioPluginAddress& address, std::span<PluginParameterReadout> values) const;
    std::string pluginParameterText(const AudioPluginAddress& address, const std::string& parameterId,
        double value, std::int32_t indexHint) const;
    EffectMeterSnapshot effectMeterSnapshot(const AudioPluginAddress& address);
    SamplerSnapshot samplerSnapshot(const AudioPluginAddress& address, Readout readout = Readout::Cached) const;
    std::optional<SlicerSnapshot> slicerSnapshot(const AudioPluginAddress& address, bool includeActivity,
        Readout readout = Readout::Cached) const;
    std::optional<EqualizerSnapshot> equalizerSnapshot(const AudioPluginAddress& address, bool consumeMeters,
        Readout readout = Readout::Cached);
    std::optional<EqualizerResponse> equalizerResponse(const AudioPluginAddress& address) const;
    std::optional<std::array<double, 180>> modulationResponse(const AudioPluginAddress& address) const;
    std::optional<GravitySnapshot> gravitySnapshot(const AudioPluginAddress& address);
    bool setInsertPresetReference(const AudioPluginAddress& address, std::string kind, std::string name);
    bool setEqualizerAnalyzer(const AudioPluginAddress& address, const plugins::equalizer::AnalyzerConfig& config);
    bool auditionEqualizerBand(const AudioPluginAddress& address, int band);
    std::optional<std::array<double, plugins::equalizer::kParameterCount>> captureEqualizerComparison(
        const AudioPluginAddress& address, char slot);
    bool activateEqualizerComparison(const AudioPluginAddress& address, char slot);
    bool copyEqualizerComparison(const AudioPluginAddress& address);
    bool setGravityFrozen(const AudioPluginAddress& address, bool frozen);
    bool clearGravityTail(const AudioPluginAddress& address);
    std::optional<PluginEditorSnapshot> pluginEditorSnapshot(const AudioPluginAddress& address,
        Readout readout = Readout::Cached) const;
    bool closePluginEditor(const AudioPluginAddress& address, bool onlyUnattached);
    std::optional<PluginEditorSize> pluginEditorSize(const AudioPluginAddress& address) const;
    std::optional<PluginEditorSize> resizePluginEditor(const AudioPluginAddress& address, PluginEditorSize requested);
    bool pumpPluginEditor(const AudioPluginAddress& address);
    std::uint32_t pollPluginEditorShortcuts(const AudioPluginAddress& address, bool enabled);
    audio::Result capturePluginCheckpoints(std::vector<AudioPluginCheckpoint>& out,
        AudioPluginCheckpointPurpose purpose = AudioPluginCheckpointPurpose::Exact);
    audio::Result restorePluginCheckpoints(std::span<const AudioPluginCheckpoint> checkpoints);
    std::vector<InsertParameter> pluginParameterValues(const AudioPluginAddress& address) const;
    bool loadInstrumentSample(const AudioPluginAddress& address, const std::string& path,
        std::shared_ptr<const engine::SampleBuffer> decoded = {});
    bool clearInstrumentSample(const AudioPluginAddress& address);
    bool restoreSlicerState(const AudioPluginAddress& address, const plugins::slicer::ControlState& state);
    bool setSlicerSlices(const AudioPluginAddress& address,
        std::shared_ptr<const plugins::slicer::SliceTable> table, const plugins::slicer::AnalysisSettings& settings);
    bool setSlicerAnalysis(const AudioPluginAddress& address, const plugins::slicer::AnalysisSettings& settings);
    bool flushSamplerPrecompute(bool wait);
    AudioPluginRuntimeStatus pluginRuntimeStatus(const std::string& channelId, const std::string& slotId,
        Readout readout = Readout::Current) const;
    bool restartPlugin(const std::string& channelId, const std::string& slotId);
    bool advancePluginEdits();
    bool configureChannelColor(const AudioPluginAddress& address, std::uint64_t seed, std::span<const InsertParameter> parameters, bool bypassed);
    bool stageMiniModulePreparation(std::uint64_t id);
    void clearMiniModulePreparation();
    void fadeMiniModulePreparation(std::uint64_t id, bool cancel);
    bool miniModulePreparationFaded(std::uint64_t id) const;
    // END GENERATED VALUE DECLARATIONS

private:
    struct Impl;
    explicit AudioRuntimeEndpoint(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> m;
};

} // namespace daw
