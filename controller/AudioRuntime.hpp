#pragma once

#include "AudioSessionSpec.hpp"
#include "AudioRuntimeControl.hpp"
#include "AudioRuntimeState.hpp"
#include "AudioRuntimeRecording.hpp"
#include "AudioRuntimePluginService.hpp"
#include "AudioRuntimeSession.hpp"
#include "PluginReadout.hpp"
#include "Engine/RealtimeEngine.hpp"
#include "Common/RealtimeSnapshot.hpp"
#include "Device/AudioDeviceManager.hpp"
#include "Recording/RecordingEngine.hpp"
#include "Host/PluginNode.hpp"
#include "Nodes/BasicNodes.hpp"
#include "Nodes/ChannelRoutingNodes.hpp"
#include "Nodes/PlaybackNodes.hpp"
#include "Nodes/MetronomeNode.hpp"
#include "Nodes/MidiClipPlayerNode.hpp"
#include "Nodes/PreviewPlayerNode.hpp"
#include "Nodes/TapNode.hpp"

#include <future>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace daw {

class AudioMiniModuleCompiler;

/// Owns executable audio state. No project, Undo, UI or collaboration callbacks
/// are reachable from the device callback. The controller reconciles its
/// document into this local runtime on the control thread.
class AudioRuntime final {
public:
    AudioRuntime();
    ~AudioRuntime();
    AudioRuntime(const AudioRuntime&) = delete;
    AudioRuntime& operator=(const AudioRuntime&) = delete;

    struct TrackNodes {
        engine::NodeId clips = engine::kInvalidNode;
        engine::NodeId midiClips = engine::kInvalidNode;
        engine::NodeId instrument = engine::kInvalidNode;
        engine::NodeId channelColor = engine::kInvalidNode;
        /// FX owned by the built-in sampler. These nodes only hear the
        /// instrument output; routed and monitored audio joins later.
        std::vector<engine::NodeId> samplerInserts;
        engine::NodeId samplerFader = engine::kInvalidNode;
        engine::NodeId samplerMeter = engine::kInvalidNode;
        engine::NodeId input = engine::kInvalidNode;
        /// Where audio *arriving from elsewhere* joins this channel: the output
        /// of another track routed here, or a send. Only channels that actually
        /// receive something get one, so an ordinary track carries no extra
        /// node. Everything merged here is ahead of the inserts, which is the
        /// whole point — a bus's plugins have to hear what is fed into it.
        engine::NodeId sum = engine::kInvalidNode;
        /// The insert chain, in document order, between the sources and the
        /// fader. Empty when the channel has no plugins loaded.
        std::vector<engine::NodeId> inserts;
        /// The channel merge/generator output immediately before its inserts.
        engine::NodeId sourceTap = engine::kInvalidNode;
        /// What a pre-fader send taps: the last insert, or the clips when there
        /// are none. "Pre-fader" means before the fader, *after* the inserts —
        /// tapping ahead of the plugins would send a signal nobody asked for.
        engine::NodeId preFaderTap = engine::kInvalidNode;
        engine::NodeId fader = engine::kInvalidNode;
        engine::NodeId meter = engine::kInvalidNode;
        std::vector<engine::NodeId> sends;
    };


    audio::Result prepare(double rate, std::uint32_t frames, bool offline = false);
    audio::Result startDevice();
    void closeDevice();
    void processDeviceBlock(audio::AudioCallbackContext& context);

    /// Assemble on the control thread, then publish after resource/automation
    /// updates. A failed compile leaves the published topology untouched.
    void buildGraph(const AudioGraphSpec& spec);
    /// Returns true when reconciliation applied stored plugin parameters.
    bool buildSession(AudioSessionSpec session);
    using TransactionId = std::uint64_t;
    TransactionId captureTransaction();
    audio::Result restoreTransaction(TransactionId id);
    void releaseTransaction(TransactionId id);
    /// Control thread only, after all enclosing render gates have exited.
    /// Retire abandoned graphs while keeping the reusable snapshot itself.
    void collectTransactionRetirements(TransactionId id);
    audio::Result applySession(AudioSessionSpec session, bool reconfigurePlugins = false,
        std::span<const AudioPluginStateEdit> restores = {},
        std::span<const AudioPluginCheckpoint> checkpoints = {},
        AudioSessionPublication* publication = nullptr,
        const std::function<void()>& afterCommitBeforeRetire = {});
    std::vector<AudioPluginAddress> retiringPlugins(std::span<const AudioPluginChainSpec> wanted) const;
    bool requiresPluginPreparation(const AudioPluginAddress& address, const AudioPluginSpec& spec) const;
    bool reconcilePluginChain(const AudioPluginChainSpec& chain);
    bool applyContent(const std::string& channelId, AudioContentSpec content);
    bool sendLiveMidi(const std::string& channelId, const engine::MidiEvent& event);
    /// Internal runtime ownership: the process protocol exposes numeric IDs.
    audio::Result startAudition(std::shared_ptr<AudioRuntime> secondary);
    void stopAudition();
    audio::Result commitGraph(bool reconfigurePlugins = false);
    /// After publication, retire the DSP stream of omitted recording Clip FX.
    void suspendRecordingClipFx(std::span<const std::string> recordingTracks);
    std::shared_ptr<const engine::CompiledGraph> routingGraph() const;
    const TrackNodes* trackNodes(const std::string& channelId) const;

    void transportCommand(const AudioTransportCommand& command);
    AudioTransportSnapshot transportSnapshot() const;
    double inputBeatsAt(std::uint64_t timestamp) const;
    /// Child protocol lifetime hook. The caller owns the mapped sink until
    /// bindInputClock(nullptr) drains and detaches its audio writer.
    void bindInputClock(engine::InputClock* sink, engine::AudioPresentationClock* presentation = nullptr);
    AudioMeterSnapshot meterSnapshot(const std::string& channelId,
        const std::string& clipId = {}, bool sampler = false) const;
    void resetMeterHold(const std::string& channelId);
    AudioRuntimeDiagnostics diagnostics() const;
    AudioDeviceSnapshot deviceSnapshot() const;
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
    audio::Result detachDeviceCallback();
    audio::Result stopDevice();
    audio::Result openDevice(const audio::AudioDeviceConfig& config);
    audio::Result refreshDevices();
    audio::Result probeDevice(const std::string& id, bool input, audio::DeviceInfo& info);
    audio::Result showDeviceControlPanel(const std::string& id, void* nativeWindow = nullptr);
    float inputPeak(std::uint32_t channel) const;
    unsigned configureWorkersForTest(bool realtime, unsigned maxParallelThreads);
    bool setFader(const std::string& channelId, AudioFaderTarget target,
        const AudioFaderChange& change, const std::string& clipId = {});
    bool setInput(const std::string& channelId, const AudioGraphSpec::Input& input);
    bool hasInputRoute(const std::string& channelId) const;
    bool setSend(const std::string& channelId, const std::string& sendId, float level, bool enabled);
    void setMetronomeEnabled(bool enabled);
    void setMetronomeSample(std::shared_ptr<const engine::SampleBuffer> sample);
    void requestCountIn(int beats);
    bool startPreview(std::shared_ptr<const engine::SampleBuffer> sample, bool loop, double pitchSemitones);
    void previewCommand(const AudioPreviewCommand& command);
    AudioPreviewSnapshot previewSnapshot() const;

    bool hasPlugin(const AudioPluginAddress& address, std::string_view uid = {}) const;
    std::vector<AudioPluginAddress> pluginAddresses() const;
    AudioPluginStateSnapshot pluginStateSnapshot(const AudioPluginAddress& address,
        bool includeState = true, const std::optional<std::string>& packagedSample = std::nullopt);
    std::vector<AudioPluginStateSnapshot> pluginStateSnapshots(std::span<const AudioPluginStateRequest> requests);
    audio::Result restorePluginState(const AudioPluginAddress& address,
        const AudioPluginStateRestore& state, std::vector<InsertParameter>& parameters,
        const std::function<void()>& beforeRetire = {});
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
    double pluginParameter(const AudioPluginAddress& address, const std::string& parameterId) const;
    /// Control-thread state capture includes host edits waiting for the next block.
    double pluginParameterIncludingPending(const AudioPluginAddress& address, const std::string& parameterId);
    bool setPluginParameter(const AudioPluginAddress& address, const std::string& parameterId, double value);
    void readPluginParameters(const AudioPluginAddress& address, std::span<PluginParameterReadout> values) const;
    std::string pluginParameterText(const AudioPluginAddress& address, const std::string& parameterId,
        double value, std::int32_t indexHint) const;
    EffectMeterSnapshot effectMeterSnapshot(const AudioPluginAddress& address);
    SamplerSnapshot samplerSnapshot(const AudioPluginAddress& address) const;
    std::optional<SlicerSnapshot> slicerSnapshot(const AudioPluginAddress& address, bool includeActivity) const;
    std::optional<EqualizerSnapshot> equalizerSnapshot(const AudioPluginAddress& address, bool consumeMeters);
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
    std::optional<PluginEditorSnapshot> pluginEditorSnapshot(const AudioPluginAddress& address) const;
    bool openPluginEditor(const AudioPluginAddress& address, void* parent, plugins::PluginEditorHost* host);
    bool closePluginEditor(const AudioPluginAddress& address, bool onlyUnattached);
    std::optional<PluginEditorSize> pluginEditorSize(const AudioPluginAddress& address) const;
    std::optional<PluginEditorSize> resizePluginEditor(const AudioPluginAddress& address, PluginEditorSize requested);
    bool pumpPluginEditor(const AudioPluginAddress& address);
    audio::Result capturePluginCheckpoints(std::vector<AudioPluginCheckpoint>& out);
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
    audio::Result startCapture(const AudioCaptureSpec& spec, AudioCaptureStarted& out);
    bool publishCaptures(std::span<const AudioCaptureId> ids);
    bool hasActiveCaptures() const;
    AudioCaptureStatus captureStatus(AudioCaptureId id) const;
    AudioCapturePeaks capturePeaks(AudioCaptureId id, std::uint64_t fromBucket) const;
    bool setCaptureInput(AudioCaptureId id, std::uint32_t first, std::uint32_t count, bool enabled);
    audio::Result stopCapture(AudioCaptureId id, audio::RecordingSession& closed);
    void interruptCapture(AudioCaptureId id);
    bool feedCaptureForTest(AudioCaptureId id, const audio::AudioBuffer& input, audio::BufferSize frames);
    AudioPluginRuntimeStatus pluginRuntimeStatus(const std::string& channelId, const std::string& slotId) const;
    AudioPluginServiceResult servicePlugins(bool externallyActive = false);
    std::vector<AudioPluginRuntimeStatus> pluginFaults() const;
    audio::Result recoverPlugin(const AudioPluginAddress& address,
        const std::function<void()>& beforeRetire = {});
    bool hasPluginFault(const AudioPluginAddress& address) const;
    void sharePluginCheckpoint(const AudioPluginAddress& address,
        std::shared_ptr<const std::vector<std::uint8_t>> bytes);
    bool audioSafetyStopped() const noexcept { return safetyStopped; }
    void stopForFailedRollback();
    bool advancePluginEdits();
    engine::PrepareInfo preparation() const;
    bool configureChannelColor(const AudioPluginAddress&, std::uint64_t seed,
        std::span<const InsertParameter>, bool bypassed);
    std::weak_ptr<AudioMiniModuleCompiler> miniModuleCompiler() const;
    bool stageMiniModulePreparation(std::uint64_t id);
    void clearMiniModulePreparation();
    void fadeMiniModulePreparation(std::uint64_t id, bool cancel);
    bool miniModulePreparationFaded(std::uint64_t id) const;

private:
    friend class EngineController;
    friend class AudioRuntimeOwner;
    class DeviceCallback;
    /// The node objects behind one channel. They outlive graph rebuilds, so a
    /// re-route keeps loaded clips, meter values and (later) plugin state
    /// instead of resetting the project's DSP on every edit.
    /// One loaded plugin behind an insert slot. `slotId` and `uid` record what
    /// it was built for, so a rebuild can tell "same plugin, still fine" from
    /// "the user swapped it" without reloading the world on every edit.
    struct InsertSlot {
        std::string slotId;
        std::string uid;
        AudioPluginSpec configuration;
        std::shared_ptr<plugins::PluginNode> node;
        std::shared_ptr<plugins::PluginNode> rightNode;
        std::shared_ptr<engine::ChannelSelectNode> leftSelector;
        std::shared_ptr<engine::ChannelSelectNode> rightSelector;
        std::shared_ptr<engine::StereoMergeNode> stereoMerge;
        PluginChannelMode channelMode = PluginChannelMode::Auto;
        std::array<std::optional<AudioPluginCheckpoint::Side>, 2> unavailableStates;
        std::array<std::string, 2> unavailableReasons;
        std::array<std::optional<AudioPluginStateEdit>, 2> unavailableEdits;
        /// Handle in the graph currently being assembled. Rewritten on every
        /// rebuild and used by the deferred sidechain routing pass.
        engine::NodeId nodeId = engine::kInvalidNode;
        engine::NodeId rightNodeId = engine::kInvalidNode;
        engine::NodeId leftSelectorId = engine::kInvalidNode;
        engine::NodeId rightSelectorId = engine::kInvalidNode;
    };
    struct ClipFxChannel {
        std::shared_ptr<engine::ClipPlayerNode> player;
        std::vector<InsertSlot> inserts;
        std::shared_ptr<engine::GainNode> fader;
        std::shared_ptr<engine::MeterNode> meter;
        engine::NodeId playerId = engine::kInvalidNode;
        std::vector<engine::NodeId> insertIds;
        engine::NodeId faderId = engine::kInvalidNode;
        engine::NodeId meterId = engine::kInvalidNode;
    };

    struct TrackChannel {
        std::shared_ptr<engine::ClipPlayerNode> clips;
        std::shared_ptr<engine::ClipPlayerNode> frozenPlayer;
        /// Clips with their own inserts are split out of the shared player and
        /// merged back here after their private chains.
        std::unordered_map<std::string, ClipFxChannel> clipFx;
        std::shared_ptr<engine::SumNode> clipFxSum;
        /// The notes, on tracks that carry them. Feeds the instrument.
        std::shared_ptr<engine::MidiClipPlayerNode> midiClips;
        std::unordered_set<std::uint16_t> heldMidiNotes;
        /// A list of at most one, so the same reconciliation as the inserts
        /// applies — the instrument is a plugin slot like any other, it just
        /// sits ahead of them and is the only one fed MIDI.
        std::vector<InsertSlot> instrument;
        std::vector<InsertSlot> miniModules;
        /// A private post-instrument chain used only while the instrument is
        /// the built-in sampler instance named by TrackModel::samplerFx.
        std::vector<InsertSlot> samplerInserts;
        std::shared_ptr<engine::GainNode> samplerFader;
        std::shared_ptr<engine::MeterNode> samplerMeter;
        /// Index-parallel with `TrackModel::inserts`, the same discipline
        /// `sends` already follows.
        std::vector<InsertSlot> inserts;
        std::shared_ptr<engine::GainNode> fader;
        std::shared_ptr<engine::MeterNode> meter;
        std::shared_ptr<engine::InputNode> input;
        uint32_t inputChannel = 0;
        uint32_t inputChannelCount = 1;
        /// Merge point for incoming routing; see `TrackNodes::sum`. Null on a
        /// channel nothing is routed into.
        std::shared_ptr<engine::SumNode> sum;
        std::vector<std::shared_ptr<engine::SendNode>> sends;
        std::vector<std::string> sendIds;
        std::shared_ptr<const AudioContentSpec::PluginCurves> automation;
        TrackNodes ids;
    };

    static void clearGraphIds(std::span<InsertSlot> slots);
    static std::unique_ptr<plugins::PluginInstance> createConfiguredPlugin(
        const AudioPluginSpec& spec);
    audio::Result replacePluginNodes(const std::string& channelId, InsertSlot& slot,
        std::shared_ptr<plugins::PluginNode> left, std::shared_ptr<plugins::PluginNode> right,
        const std::function<void()>& beforeRetire = {});
    void bindPluginSafety(const std::string& channelId, InsertSlot& slot);
    bool pluginAudioActivity() const;
    static bool pluginMatches(const InsertSlot& live, const AudioPluginSpec& wanted);
    static bool needsPluginReplacement(const InsertSlot* existing, const AudioPluginSpec& spec, bool right);
    InsertSlot* pluginSlot(const std::string& channelId, const std::string& slotId);
    const InsertSlot* pluginSlot(const std::string& channelId, const std::string& slotId) const;
    plugins::PluginNode* pluginNode(const AudioPluginAddress& address) const;
    plugins::PluginInstance* pluginInstance(const AudioPluginAddress& address) const;
    const std::vector<InsertSlot>* pluginChain(const AudioPluginChainSpec& chain) const;
    static bool applyStoredParameters(plugins::PluginNode& node, std::span<const InsertParameter> values);
    void preservePluginSourceForTransactions(const AudioPluginAddress& address);
    static audio::Result restorePluginNode(plugins::PluginNode& node,
        const AudioPluginStateRestore& state, std::vector<InsertParameter>& parameters,
        std::string_view slotId);
    static AudioPluginStateSnapshot snapshotPluginNode(plugins::PluginNode& node,
        const AudioPluginAddress& address, bool includeState,
        const std::optional<std::string>& packagedSample = {});
    static void restoreCheckpointNode(plugins::PluginNode& node,
        const AudioPluginCheckpoint::Side& side, const AudioPluginStateEdit* projectState = nullptr);
    static bool matchesCheckpointProjectState(const AudioPluginCheckpoint::Side& side,
        const AudioPluginStateEdit* projectState);
    static void applyPluginAutomation(TrackChannel& channel);
    static void clearGraphIds(ClipFxChannel& channel);
    static engine::NodeId connectSlots(engine::AudioGraph& graph,
        std::span<InsertSlot> slots, std::vector<engine::NodeId>& ids, engine::NodeId head);
    static engine::NodeId connectMiniModules(engine::AudioGraph& graph,
        TrackChannel& channel, const std::vector<AudioGraphSpec::MiniModuleRoute>& routes,
        bool postFx, engine::NodeId head, engine::NodeId* first = nullptr);
    // Declared first so graph workers and snapshots outlive their node owners.
    engine::RealtimeEngine engine;
    std::unique_ptr<audio::AudioDeviceManager> devices;
    using RecorderList = std::vector<std::shared_ptr<audio::AudioRecorder>>;
    engine::RealtimeSnapshot<RecorderList> activeRecorders;
    std::unique_ptr<DeviceCallback> callback;
    /// Keyed by track UUID, plus "master" for the master bus.
    std::unordered_map<std::string, TrackChannel> channels;
    struct StagedPluginPreparation;
    std::shared_ptr<StagedPluginPreparation> stageSessionPlugins(const AudioSessionSpec& session,
        std::span<const AudioPluginStateEdit> restores, std::span<const AudioPluginCheckpoint> checkpoints,
        AudioSessionPublication* publication = nullptr);
    bool stagedSessionPluginsCurrent(const std::shared_ptr<StagedPluginPreparation>& staged) const;
    StagedPluginPreparation* stagedPluginPreparation = nullptr;
    struct SessionTransaction;
    std::unordered_map<TransactionId, std::shared_ptr<SessionTransaction>> sessionTransactions;
    TransactionId nextSessionTransaction = 0;
    class MiniModuleCompiler;
    class AuditionNode;
    void configureAudioWorkers(const rt::AudioWorkerConfig& config);
    void inheritAudioWorkers(AudioRuntime& secondary) const;
    audio::Result takeDeviceFrom(AudioRuntime& previous);
    std::shared_ptr<AudioRuntime> auditionRuntime;
    bool auditionDriven = false;
    mutable std::shared_ptr<MiniModuleCompiler> miniCompiler;
    std::uint64_t pluginMainThreadGeneration = plugins::PluginMainThreadWork::generation();
    std::uint32_t pluginCompatibilitySweepTicks = 0;
    bool pendingPitchQualityChanges = false;
    // The runtime checkpoint is also the source for the document recovery
    // cache: a failed native object is never consulted by Save or recovery.
    std::unordered_map<std::uint64_t, AudioPluginStateSnapshot> lastGoodPluginStates;
    std::unordered_map<std::uint64_t, std::shared_ptr<const std::vector<std::uint8_t>>> sharedCheckpointBytes;
    std::unordered_map<std::uint64_t, std::vector<InsertParameter>> checkpointParameterEdits;
    std::atomic<bool> safetyStopped{false};
    void retainPluginSnapshot(const AudioPluginStateSnapshot& snapshot);
    void prunePluginSnapshots();
    std::unordered_map<std::uint64_t, std::shared_ptr<plugins::PluginNode>> retiringEditorNodes;
    void refreshMasterSafetyMute();
    std::uint64_t lastLiveMidiNs = 0, countInUntilNs = 0;
    audio::AudioRecorder* captureRecorder(AudioCaptureId id) const;
    struct RecordingCapture {
        std::shared_ptr<audio::AudioRecorder> recorder;
        double sampleRate = 0;
    };
    std::unordered_map<AudioCaptureId, RecordingCapture> recordings;
    AudioCaptureId nextCaptureId = 0;
    engine::AudioGraph publishedGraph;
    bool hasPublishedGraph = false;
    double sampleRate = 48000.0;
    uint32_t bufferSize = 512;
    bool deviceOpen = false;
    bool liveDeviceAllowed = false;
    bool prepared = false;
    std::shared_ptr<engine::SumNode> masterSum;
    std::shared_ptr<engine::GainNode> masterFader;
    std::shared_ptr<engine::MetronomeNode> metronome;
    std::shared_ptr<engine::PreviewPlayerNode> preview;
    engine::NodeId masterFaderId = engine::kInvalidNode;
    engine::NodeId masterSumId = engine::kInvalidNode;
    /// Retained across topology/PDC rebuilds during the same offline pass.
    std::unordered_map<std::string, std::shared_ptr<engine::TapNode>> renderTaps;
    bool renderTapsPreFader = false;
    bool renderTapsAtSource = false;
    bool renderingPass = false; // excludes the monitoring metronome from exports
    bool isRenderClone = false;
    std::unordered_map<std::string, std::shared_ptr<plugins::PluginNode>> preparedMiniModules;
};

} // namespace daw
