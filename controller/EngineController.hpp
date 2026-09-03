#pragma once

#include "MidiFile.hpp"
#include "RenderSpec.hpp"
#include "model/Document.hpp"
#include "recovery/RecoverySnapshot.hpp"
#include "UndoStack.hpp"
#include "AudioAnalysis.hpp"
#include "AudioMusicalAnalysis.hpp"
#include "WaveformCache.hpp"
#include "cloud/CloudPublicationCapture.hpp"
#include "collaboration/SharedAssetMutationSink.hpp"
#include "collaboration/SharedMutationSink.hpp"
#include "plugins/PluginManager.hpp"

#include "Host/PluginNode.hpp"
#include "Internal/SamplerInstance.hpp"
#include "Internal/SamplerPrecompute.hpp"

#include "Engine/RealtimeEngine.hpp"
#include "Common/RealtimeSnapshot.hpp"
#include "Audio/SampleBuffer.hpp"
#include "Nodes/BasicNodes.hpp"
#include "Nodes/ChannelRoutingNodes.hpp"
#include "Nodes/PlaybackNodes.hpp"
#include "Nodes/MetronomeNode.hpp"
#include "Nodes/MidiClipPlayerNode.hpp"
#include "Nodes/PreviewPlayerNode.hpp"
#include "Nodes/TapNode.hpp"

#include "Device/AudioDeviceManager.hpp"
#include "Recording/RecordingEngine.hpp"
#include "Core/Result.hpp"

#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace daw {

namespace collab {
struct ChangeImpact;
}

/// The framework-agnostic application controller: it owns the graph engine and
/// the document model, keeps them in sync, and exposes a clean C++ API that any
/// UI drives. No UI framework types leak in.
///
/// The document is the source of truth. Every edit rebuilds the affected part
/// of the engine's node graph and republishes it — the audio thread only ever
/// sees a finished graph.
/// One stretch of timeline the running take has already written. A plain
/// take makes exactly one; loop recording makes one per pass, all fed by the
/// same capture, which is why each carries its own offset into it.
struct RecordingSpan {
    double startSeconds = 0.0;        ///< where this pass began on the timeline
    double endSeconds = 0.0;          ///< the write head, or the loop end
    double captureOffsetSeconds = 0.0;///< where it begins inside the capture
};

/// The take in flight, described the way it will look once it lands.
///
/// The arrangement draws a *clip* while recording, not a red smear, so it
/// has to know what that clip is going to be: which colour, which name, and
/// whether it is a fresh clip or the next take inside one that is already
/// there. All of that is decided by the same code that lands the capture
/// (`capturePasses`, the punch-target search), so the preview and the
/// result cannot disagree.
struct RecordingPreview {
    bool active = false;
    std::string trackId;
    std::string name;                  ///< caption to draw on the clip
    std::uint32_t color = 0;           ///< colour the landed material takes
    /// True when this lands as a take *inside* `targetClipId` rather than
    /// as a clip of its own — layer recording punching into existing audio.
    bool layered = false;
    std::string targetClipId;
    int takeIndex = -1;                ///< which row of the stack it becomes
    std::vector<RecordingSpan> spans;
    /// Input peak per bucket since the punch point, oldest first, 0…1.
    /// Bucket `i` covers [i·step, (i+1)·step) of *recorded* time. A view into
    /// the live capture rather than a copy — the arrangement asks for this
    /// every frame, for every recording lane, and a long take's envelope is
    /// thousands of floats. Valid until the take stops.
    std::span<const float> envelope;
    double envelopeStepSeconds = 0.0;
    /// Seconds captured so far, from the recorder's own frame count — the
    /// clock the stripe and its waveform are both drawn against.
    double capturedSeconds = 0.0;
};

class EngineController {
public:
    EngineController();
    ~EngineController();

    EngineController(const EngineController&) = delete;
    EngineController& operator=(const EngineController&) = delete;

    /// Install the synchronous, non-owning bridge used for shared document
    /// mutations.  The owner must detach it before destroying the sink.
    void attachSharedMutationSink(collab::SharedMutationSink& sink) noexcept {
        m_sharedMutationSink = &sink;
    }
    /// Detach only if `sink` is still the active bridge.  Conditional detach
    /// prevents an older bridge's teardown from clearing a newer attachment.
    bool detachSharedMutationSink(
        const collab::SharedMutationSink& sink) noexcept {
        if (m_sharedMutationSink != &sink) return false;
        m_sharedMutationSink = nullptr;
        return true;
    }
    const collab::SharedMutationSink* sharedMutationSink() const noexcept {
        return m_sharedMutationSink;
    }
    /// Asset-producing cloud actions remain local-only requests until the app
    /// returns a verified immutable AssetRef through completeSharedAssetMutation.
    void attachSharedAssetMutationSink(
        collab::SharedAssetMutationSink& sink) noexcept {
        m_sharedAssetMutationSink = &sink;
    }
    bool detachSharedAssetMutationSink(
        const collab::SharedAssetMutationSink& sink) noexcept {
        if (m_sharedAssetMutationSink != &sink) return false;
        m_sharedAssetMutationSink = nullptr;
        return true;
    }
    collab::SharedMutationResult completeSharedAssetMutation(
        const std::string& requestId, const AssetRef& verifiedAsset);
    void cancelSharedAssetMutation(const std::string& requestId);

    /// Handles of the engine nodes that make up one channel. Exposed so tools
    /// and tests can look at what the routing actually compiled to.
    struct TrackNodes {
        engine::NodeId clips = engine::kInvalidNode;
        engine::NodeId midiClips = engine::kInvalidNode;
        engine::NodeId instrument = engine::kInvalidNode;
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
        /// What a pre-fader send taps: the last insert, or the clips when there
        /// are none. "Pre-fader" means before the fader, *after* the inserts —
        /// tapping ahead of the plugins would send a signal nobody asked for.
        engine::NodeId preFaderTap = engine::kInvalidNode;
        engine::NodeId fader = engine::kInvalidNode;
        engine::NodeId meter = engine::kInvalidNode;
        std::vector<engine::NodeId> sends;
    };

    /// The master bus addresses its inserts through a channel under this
    /// reserved id. Safe from collision because every real track id is a
    /// 36-character uuid from `newUuid()`.
    static constexpr const char* kMasterChannelId = "master";
    /// Stems are written concurrently, one open file each, so the ceiling is
    /// really the process file-descriptor limit. Refusing above this is a
    /// clearer failure than `sf_open` running out halfway through.
    static constexpr std::size_t kMaxRenderStems = 200;
    static constexpr std::size_t kSamplerFxSlots = 8;

    // ── Lifecycle ──
    /// Prepare the engine. With openDevice=true a live PortAudio stream is
    /// opened; with false only the offline path (model + export) is prepared,
    /// which needs no audio hardware.
    audio::Result initialize(double sampleRate = 48000.0,
                             uint32_t bufferSize = 512,
                             bool openDevice = true);
    audio::Result initialize(const audio::AudioDeviceConfig& config,
                             bool openDevice = true);
    void shutdown();
    bool isDeviceOpen() const { return m_deviceOpen; }
    double sampleRate() const { return m_sampleRate; }

    // ── Document ──
    void newProject();
    const ProjectModel& project() const { return m_project; }
    const std::string& projectName() const { return m_project.name; }
    void setProjectName(std::string name);

    audio::Result saveProject(const std::string& packageDir);
    audio::Result openProject(const std::string& packageDir);
    /// Freeze the current local document and every built-in plugin instance
    /// into a move-only upload staging object. This never changes the live
    /// project, transport or undo stack. The returned sources deliberately do
    /// not carry SHA-256 values yet: hashing belongs to the publisher worker,
    /// not to this control-thread/plugin seam.
    cloud::CloudPublicationCapture captureCloudPublicationV1(
        const std::string& stagingParent = {});
    /// Replace only the engine-facing materialization of a cloud document.
    ///
    /// `runtimeDocument` is a disposable edge copy: cloud AssetRef values have
    /// already been resolved to cache paths and per-user transport/UI fields
    /// have already been overlaid.  The shared reducer document is never
    /// passed here by mutable reference.  This path records no legacy undo and
    /// performs a transactional model/graph swap on the control thread; a
    /// failed graph compilation restores the previous model and graph.
    ///
    /// Initial verified snapshots set `clearLegacyUndo`.  Subsequent
    /// optimistic, acknowledged, remote and rebase projections leave the
    /// (normally already empty) legacy stack untouched; typed cloud history is
    /// authoritative for the session.
    audio::Result materializeCollaborationProject(
        ProjectModel runtimeDocument, bool clearLegacyUndo);
    /// Apply a reducer delta to the already materialized collaboration
    /// document.  Topology changes publish one graph; scalar, clip, MIDI,
    /// automation and plugin-parameter deltas update only their live nodes.
    /// The local transport, recording state and legacy undo stack are not
    /// touched.
    audio::Result projectCollaborationChange(
        ProjectModel runtimeDocument, const collab::ChangeImpact& impact);
    /// Narrow acceptance-test seam: collaboration projection must not turn a
    /// knob/note/hydration update into a full graph publication.
    std::uint64_t graphRebuildCountForTest() const noexcept {
        return m_graphRebuildCount;
    }
    /// Capture a complete crash-recovery generation. Must be called on the
    /// message/control thread: plugin formats require saveState there.
    /// Limit opaque plugin serialization when polling native editors. Cached
    /// state chunks are reused for instances outside this tick's budget; the
    /// default still captures every live instance synchronously.
    recovery::RecoverySnapshot captureRecoverySnapshot(
        std::size_t maxPluginStateCaptures =
            std::numeric_limits<std::size_t>::max());
    /// Refresh a bounded round-robin slice of opaque plugin state without
    /// copying the project. Returns true only when a content-addressed state
    /// file changed; recovery can then pay for a full document snapshot once.
    bool refreshRecoveryPluginStates(
        std::size_t maxPluginStateCaptures,
        std::span<const std::string> preferredStems = {});
    /// Activate a journal document and restore state from its State/ folder.
    /// State absent from the journal may fall back to the last manual package.
    audio::Result restoreRecoveryProject(
        ProjectModel snapshot, const std::string& recoveryDir,
        const std::string& originalPackageDir = {});
    /// Save a clip-free, portable project template without changing the live
    /// document's name, paths, state-file references or undo history.
    audio::Result saveProjectTemplate(const std::string& packageDir,
                                      const std::string& templateName);
    /// Replace the document from a template package. The UI deliberately does
    /// not adopt the template's path, so the next ordinary Save is Save As.
    audio::Result openProjectTemplate(const std::string& packageDir);
    /// Append only the template's tracks to the current document. Project-wide
    /// settings and the master channel remain untouched. All returned ids are
    /// fresh ids in the current document; the whole import is one undo entry.
    audio::Result importProjectTemplateTracks(
        const std::string& packageDir,
        std::vector<std::string>& outTrackIds);

    // ── Transport ──
    void play();
    void stop();
    void pause();
    void seekSeconds(double seconds);
    double positionSeconds() const;
    /// Smooth display clock derived from the current audio block. Never use
    /// this for edits or engine decisions; those stay on positionSeconds().
    double presentationPositionSeconds() const;
    bool isPlaying() const;
    double durationSeconds() const;

    /// Behaviour of the play/pause (Space) toggle.
    enum class PlaybackMode : std::uint8_t {
        Resume,    ///< Space resumes from the current playhead position.
        Restart,   ///< Space jumps back to the start of the current run.
    };
    void setPlaybackMode(PlaybackMode mode) { m_playbackMode = mode; }
    PlaybackMode playbackMode() const { return m_playbackMode; }

    // ── Tempo / time signature ──
    void setTempo(double bpm);
    double tempo() const { return m_project.tempo; }
    collab::SharedMutationResult setTimeSignature(int numerator,
                                                  int denominator);

    /// The key the project is in: a pitch class (0 = C) and a scale id from
    /// `miditools::scaleId`. Advisory — nothing is forced into it — but it is
    /// what the assistant writes against, so a wrong key is heard immediately.
    collab::SharedMutationResult setProjectKey(int root,
                                               const std::string& scaleId);
    /// Standing instructions the assistant works under for this project.
    /// Not undoable: it is a preference about the work, not part of it.
    collab::SharedMutationResult setAiInstructions(std::string text);
    const std::string& aiInstructions() const { return m_project.aiInstructions; }

    int keyRoot() const { return m_project.keyRoot; }
    const std::string& projectScale() const { return m_project.scale; }
    int timeSigNumerator() const { return m_project.timeSigNumerator; }
    int timeSigDenominator() const { return m_project.timeSigDenominator; }

    // ── Metronome ──
    /// Toggle the click track. It plays whenever the transport is rolling.
    void setMetronomeEnabled(bool enabled);
    bool isMetronomeEnabled() const { return m_metronomeEnabled; }
    /// Replace the built-in muted knock with a decoded audio file. Passing an
    /// empty path restores the built-in sound. The choice is a workstation
    /// preference, not project data, so the UI persists the path in settings.
    bool setMetronomeSample(const std::string& filePath);
    const std::string& metronomeSamplePath() const {
        return m_metronomeSamplePath;
    }

    // ── Loop ──
    void setLoopEnabled(bool enabled);
    bool isLoopEnabled() const;
    void setLoopRangeSeconds(double startSeconds, double endSeconds);
    double loopStartSeconds() const;
    double loopEndSeconds() const;

    // ── Tracks ──
    std::string addTrack(TrackKind kind, const std::string& name = "");
    void removeTrack(const std::string& trackId);
    collab::SharedMutationResult renameTrack(const std::string& trackId,
                                             const std::string& name);
    void setTrackVolume(const std::string& trackId, float volume);
    void setTrackPan(const std::string& trackId, float pan);
    /// Continuous mixer preview. These update the document and engine without
    /// touching history; commit the captured starts when the gesture ends.
    void setTrackVolumeLive(const std::string& trackId, float volume);
    void setTrackPanLive(const std::string& trackId, float pan);
    void commitTrackVolumeEdit(
        const std::vector<std::pair<std::string, float>>& before,
        const std::string& label = "Set Volume");
    void commitTrackPanEdit(
        const std::vector<std::pair<std::string, float>>& before,
        const std::string& label = "Set Pan");
    /// Fold the track to mono (true) or keep it stereo (false).
    void setTrackMono(const std::string& trackId, bool mono);
    collab::SharedMutationResult setTrackMuted(const std::string& trackId,
                                               bool muted);
    /// Atomically apply one mute gesture to several stable track IDs. Plain
    /// folders include their descendants; duplicate IDs are removed before
    /// the collaboration sink or local document sees the gesture.
    collab::SharedMutationResult setTracksMuted(
        std::span<const std::string> trackIds, bool muted);
    void setTrackSoloed(const std::string& trackId, bool soloed);
    /// Is anything soloed / muted right now? What a "clear all" control lights
    /// itself from, so the UI never keeps its own copy of the answer.
    bool anySoloed() const;
    bool anyMuted() const;
    /// Lift every solo / every mute in the project, in one go.
    void clearAllSolos();
    collab::SharedMutationResult clearAllMutes();
    void setTrackArmed(const std::string& trackId, bool armed);
    /// Input monitoring for one track: the graph grows a live input node feeding
    /// that channel's fader.
    void setTrackMonitor(const std::string& trackId, bool monitor);
    bool isInputMonitoringActive() const;
    /// Peak of the incoming signal on a hardware input channel (0…1).
    float inputPeak(uint32_t channel) const;
    /// Recolour a track and its clips. Colouring a *folder* colours everything
    /// filed inside it too — a folder is how a group of tracks is named and
    /// seen, so its colour is the group's colour.
    void setTrackColor(const std::string& trackId, uint32_t color);
    /// Lane height in pixels (document-only; the arrangement reads it). Live.
    void setTrackHeight(const std::string& trackId, double height);
    void commitTrackHeightEdit(
        const std::vector<std::pair<std::string, double>>& before,
        const std::string& label = "Resize Tracks");
    /// Duplicate a track (its clips, routing intent and flags). With
    /// `withInserts=false` the copy starts with empty insert slots. Undoable.
    std::string duplicateTrack(const std::string& trackId, bool withInserts = true);
    /// Duplicate a Pattern as one musical object, including every source track,
    /// instrument/sampler slot and MIDI clip. Undoable as one operation.
    std::string duplicatePattern(const std::string& patternId);
    void setTrackInputChannel(const std::string& trackId, uint32_t channel);
    /// How wide this track's input is: 1 for a mono source, 2 for a stereo pair
    /// starting at `inputChannel`. This is what a recording captures and what
    /// monitoring listens to — a mono source records a one-channel file, which
    /// is heard centred whether or not the track's mono fold is on.
    void setTrackInputChannelCount(const std::string& trackId, uint32_t count);
    void moveTrackToFolder(const std::string& trackId,
                           const std::string& parentId);

    // ── Order and folders ──
    bool moveTrack(const std::string& trackId, size_t targetIndex,
                   const std::string& newParentId);
    void setFolderExpanded(const std::string& folderId, bool expanded);
    void setAutomationExpanded(const std::string& trackId, bool expanded);
    /// Create an empty folder. A summing folder owns a bus: anything filed
    /// inside it is routed through that bus, and it has a channel strip like
    /// any other. A plain folder is a container with no signal at all.
    std::string addFolder(bool summing, const std::string& name = "");
    /// Create a summing musical container whose children remain independent
    /// instrument channels in the mixer.
    std::string addPattern(const std::string& name = "");
    /// Add one VSTi/AU/CLAP instrument lane to a Pattern, including a one-bar
    /// MIDI clip ready for the piano roll. Returns the child track id.
    std::string addPatternInstrument(
        const std::string& patternId,
        const plugins::PluginDescriptor& descriptor,
        double startSeconds = 0.0);
    /// Add one built-in Sampler lane loaded from `filePath`, named after the
    /// file stem, with a one-bar MIDI clip ready to edit.
    std::string addPatternSample(const std::string& patternId,
                                 const std::string& filePath,
                                 double startSeconds = 0.0);
    /// Turn summing on or off for an existing folder, re-routing its contents
    /// either into its new bus or back out to wherever they were headed.
    void setFolderSumming(const std::string& folderId, bool summing);
    std::string packIntoFolder(const std::vector<std::string>& trackIds,
                               const std::string& name = "",
                               bool summing = false);

    // ── Channel routing: sends and inserts ──
    /// Routing is rendered for real: every change recompiles the engine's node
    /// graph (clips → inserts → fader → meter → bus/master, with sends tapped
    /// pre or post fader).
    /// Loudest a send may be driven: +6 dB, the same ceiling a fader has.
    static constexpr float kMaxSendLevel = 1.9953f;   // 10^(6/20)

    std::string addSend(const std::string& trackId,
                        const std::string& destinationTrackId);
    void removeSend(const std::string& trackId, const std::string& sendId);
    void setSendLevel(const std::string& trackId, const std::string& sendId,
                      float level);
    /// `setSendLevel` is the live preview path; this records its final value as
    /// one undo entry when the knob is released.
    void commitSendLevelEdit(const std::string& trackId,
                             const std::string& sendId, float before,
                             const std::string& label = "Set Send Level");
    void setSendPreFader(const std::string& trackId, const std::string& sendId,
                         bool preFader);
    void setSendEnabled(const std::string& trackId, const std::string& sendId,
                        bool enabled);
    /// Route a track's main output into a bus/aux track (empty = master).
    /// Returns false and keeps the previous routing when the change would
    /// create a feedback loop.
    bool setTrackOutputBus(const std::string& trackId,
                           const std::string& busTrackId);
    void setTrackInputEnabled(const std::string& trackId, bool enabled);
    void ensureInsertSlots(const std::string& trackId, size_t count);
    void ensureMasterInsertSlots(size_t count);

    // ── Plugin inserts ──
    //
    // Every call takes a channel id: a track's uuid, or `kMasterChannelId` for
    // the master bus. The master is a channel like any other here, so none of
    // this needs a second code path.

    PluginManager& pluginManager() { return m_pluginManager; }
    const PluginManager& pluginManager() const { return m_pluginManager; }

    /// The document's insert list for a channel, or null for an unknown id.
    const std::vector<InsertModel>* channelInserts(const std::string& channelId) const;

    /// Load a plugin into a new slot at `index` (end when out of range).
    /// Returns the new slot id, or "" when the plugin could not be loaded.
    /// Undoable.
    std::string addInsert(const std::string& channelId,
                          const plugins::PluginDescriptor& descriptor,
                          size_t index = size_t(-1));
    void removeInsert(const std::string& channelId, const std::string& insertId);
    void moveInsert(const std::string& channelId, const std::string& insertId,
                    size_t targetIndex);
    /// Swap the plugin in a slot, keeping the slot id — and with it the slot's
    /// automation lanes and any editor window open on it.
    ///
    /// False when the new plugin could not be instantiated, in which case the
    /// slot is left exactly as it was. That guard is not theoretical: a stale
    /// scan cache still offers plugins whose module has since stopped
    /// advertising them, and without it the slot kept the new plugin's *name*
    /// with nothing behind it — a plugin that "does not work and will not even
    /// open".
    bool replaceInsert(const std::string& channelId, const std::string& insertId,
                       const plugins::PluginDescriptor& descriptor);
    /// Live: pushed straight to the node, no recompile, so it stays click-free.
    /// Called just before a slot's live plugin is destroyed — by a replace, a
    /// removal, an undo, or a project being closed.
    ///
    /// The plugin is **still alive** when this runs, which is the whole point:
    /// an editor window has the plugin's own view embedded in it, and the
    /// plugin must be told to let go of that view before it is destroyed. Only
    /// the UI knows a window exists, and only this layer knows when a plugin
    /// is going, so the two have to meet somewhere — here.
    using PluginRetiringFn =
        std::function<void(const std::string& channelId, const std::string& slotId)>;
    void setPluginRetiringCallback(PluginRetiringFn callback) {
        m_pluginRetiring = std::move(callback);
    }

    /// Addresses the instrument slot as readily as an insert — the instrument is
    /// a plugin slot that happens to sit ahead of them.
    void setInsertBypassed(const std::string& channelId,
                           const std::string& insertId, bool bypassed);
    /// Bypass (or enable) every insert on a channel as one undoable step. The
    /// instrument is left alone: "bypass the FX" should not silence the track.
    /// Restoring puts each slot back to the flag it had, so slots already
    /// bypassed by hand stay that way after an undo.
    void setAllInsertsBypassed(const std::string& channelId, bool bypassed);
    void setInsertMix(const std::string& channelId, const std::string& insertId,
                      float mix);
    void commitInsertMixEdit(const std::string& channelId,
                             const std::string& insertId, float beforeMix,
                             const std::string& label);

    // ── Channel strip clipboard ──
    //
    // Copying a chain has to copy the *sound*, not a row of plugin names at
    // their defaults, so each slot travels with the state its live instance
    // reports. The clipboard lives here rather than in a widget: the mixer and
    // the inspector are two views of the same console and must share it, and a
    // drag from one strip to another is the same operation with the clipboard
    // skipped.

    /// One plugin slot lifted off a channel: the document's description of it
    /// plus the plugin's own opaque state.
    struct ChainSlotSnapshot {
        InsertModel model;
        std::vector<std::uint8_t> state;
        std::vector<std::uint8_t> rightState;   ///< Dual Mono's second instance
    };

    /// A channel's FX chain, and — when it was copied whole — everything else
    /// the strip holds.
    struct ChannelSnapshot {
        std::string sourceName;       ///< what it came from, for the paste menu
        std::vector<ChainSlotSnapshot> inserts;
        bool hasSettings = false;     ///< fader, pan, flags, routing and sends
        float volume = 1.0f;
        float pan = 0.0f;
        bool muted = false;
        bool soloed = false;
        bool mono = false;
        std::string outputBusId;
        std::vector<SendModel> sends;
        bool empty() const { return inserts.empty() && !hasSettings; }
    };

    /// Lift a channel's chain, with the rest of its strip when asked for.
    ChannelSnapshot copyChannelStrip(const std::string& channelId,
                                     bool withSettings);
    /// Replace a channel's chain with a copied one. Undoable, one entry; the
    /// pasted plugins come up holding the state they were copied at.
    bool pasteChannelInserts(const std::string& channelId,
                             const ChannelSnapshot& what);
    /// The chain *and* the fader, pan, flags, routing and sends. One entry.
    bool pasteChannelStrip(const std::string& channelId,
                           const ChannelSnapshot& what);
    /// Apply a reusable Channel Strip preset: plugins plus volume and pan.
    /// Unlike clipboard paste, this deliberately preserves mute, solo, mono,
    /// routing and every send on the destination channel.
    bool pasteChannelStripPreset(const std::string& channelId,
                                 const ChannelSnapshot& what);
    /// Capture/apply the portable `.vlts` form of the preset above.
    audio::Result saveChannelStripPreset(const std::string& channelId,
                                         const std::string& filePath);
    audio::Result applyChannelStripPreset(const std::string& channelId,
                                          const std::string& filePath);
    /// What was copied last. Empty until something is.
    const ChannelSnapshot& channelClipboard() const { return m_channelClipboard; }
    void setChannelClipboard(ChannelSnapshot snapshot) {
        m_channelClipboard = std::move(snapshot);
    }
    /// Take one plugin slot off `fromChannel` and put it on `toChannel` at
    /// `index`, carrying its live state. With `copy` the source keeps its own.
    /// One undo entry covers both channels.
    bool moveInsertBetweenChannels(const std::string& fromChannel,
                                   const std::string& insertId,
                                   const std::string& toChannel, size_t index,
                                   bool copy);
    /// Copy every send of one track onto another, replacing what is there.
    /// `move` clears the source's sends, which is what dragging them does.
    bool copySendsTo(const std::string& fromTrackId, const std::string& toTrackId,
                     bool move);

    /// Saved settings shown by the host wrapper above the plugin's own UI.
    const InsertModel* insertModel(const std::string& channelId,
                                   const std::string& insertId) const;
    bool setInsertChannelMode(const std::string& channelId,
                              const std::string& insertId,
                              PluginChannelMode mode);
    void setInsertEditorChannel(const std::string& channelId,
                                const std::string& insertId,
                                PluginEditorChannel channel);
    struct SidechainSource {
        std::string id;
        std::string name;
    };
    std::vector<SidechainSource> insertSidechainSources(
        const std::string& channelId) const;
    bool insertSupportsSidechain(const std::string& channelId,
                                 const std::string& insertId) const;
    bool setInsertSidechainSource(const std::string& channelId,
                                  const std::string& insertId,
                                  const std::string& sourceTrackId);

    /// Parameters, addressed by the plugin's own stable id.
    std::vector<plugins::ParameterInfo> insertParameters(
        const std::string& channelId, const std::string& insertId) const;
    double insertParameter(const std::string& channelId,
                           const std::string& insertId,
                           const std::string& parameterId) const;
    /// Live and non-undoable, for a knob being dragged. Finish the gesture with
    /// `commitInsertParameterEdit`, the same shape `commitLaneEdit` uses.
    void setInsertParameter(const std::string& channelId,
                            const std::string& insertId,
                            const std::string& parameterId, double plainValue);
    void commitInsertParameterEdit(const std::string& channelId,
                                   const std::string& insertId,
                                   const std::string& parameterId,
                                   double beforeValue, const std::string& label);

    /// The live instance behind a slot, for an editor window. Null when the
    /// slot is empty or the plugin failed to load.
    plugins::PluginInstance* insertInstance(const std::string& channelId,
                                            const std::string& insertId);

    // ── Sampler-scoped insert FX ──
    const SamplerFxModel* samplerFx(const std::string& trackId,
                                    const std::string& samplerSlotId) const;
    std::string addSamplerFxInsert(
        const std::string& trackId, const std::string& samplerSlotId,
        const plugins::PluginDescriptor& descriptor, size_t index = size_t(-1));
    void removeSamplerFxInsert(const std::string& trackId,
                               const std::string& samplerSlotId,
                               const std::string& insertId);
    void moveSamplerFxInsert(const std::string& trackId,
                             const std::string& samplerSlotId,
                             const std::string& insertId, size_t targetIndex);
    bool replaceSamplerFxInsert(const std::string& trackId,
                                const std::string& samplerSlotId,
                                const std::string& insertId,
                                const plugins::PluginDescriptor& descriptor);
    void setAllSamplerFxBypassed(const std::string& trackId,
                                 const std::string& samplerSlotId,
                                 bool bypassed);
    void setSamplerFxVolume(const std::string& trackId,
                            const std::string& samplerSlotId, float volume);
    void setSamplerFxPan(const std::string& trackId,
                         const std::string& samplerSlotId, float pan);
    void commitSamplerFxLevelEdit(const std::string& trackId,
                                  const std::string& samplerSlotId,
                                  float beforeVolume, float beforePan,
                                  const std::string& label);
    float samplerFxPeakLeft(const std::string& trackId) const;
    float samplerFxPeakRight(const std::string& trackId) const;

    // ── Audio-clip-scoped insert FX ──
    // Same strip contract as Sampler FX, but owned by one ClipModel.  The
    // chain is inserted before that clip joins the track, so other clips and
    // monitored/routed audio never pass through it.
    const std::vector<InsertModel>* clipFx(const std::string& trackId,
                                           const std::string& clipId) const;
    std::string addClipFxInsert(
        const std::string& trackId, const std::string& clipId,
        const plugins::PluginDescriptor& descriptor, size_t index = size_t(-1));
    void removeClipFxInsert(const std::string& trackId, const std::string& clipId,
                            const std::string& insertId);
    void moveClipFxInsert(const std::string& trackId, const std::string& clipId,
                          const std::string& insertId, size_t targetIndex);
    bool replaceClipFxInsert(const std::string& trackId, const std::string& clipId,
                             const std::string& insertId,
                             const plugins::PluginDescriptor& descriptor);
    void setAllClipFxBypassed(const std::string& trackId,
                              const std::string& clipId, bool bypassed);
    void setClipFxVolume(const std::string& trackId, const std::string& clipId,
                         float volume);
    void setClipFxPan(const std::string& trackId, const std::string& clipId,
                      float pan);
    void commitClipFxLevelEdit(const std::string& trackId,
                               const std::string& clipId, float beforeVolume,
                               float beforePan, const std::string& label);
    float clipFxPeakLeft(const std::string& trackId,
                         const std::string& clipId) const;
    float clipFxPeakRight(const std::string& trackId,
                          const std::string& clipId) const;

    // ── The built-in sampler ──
    //
    // The sampler is hosted through the slot machinery like any plugin, so its
    // knobs go through `setInsertParameter` above. Only the sample itself needs
    // its own calls: it is not a parameter, it lives in the instance's state
    // chunk, and loading one is a file operation rather than a value change.

    /// The sampler in a slot, or null when the slot holds something else.
    plugins::sampler::SamplerInstance* samplerInstance(const std::string& channelId,
                                                       const std::string& slotId);
    /// Load an audio file into a sampler slot. Undoable, and false when the
    /// slot is not a sampler or the file will not decode.
    bool loadSamplerSample(const std::string& channelId, const std::string& slotId,
                           const std::string& filePath);
    /// Unload the sample, leaving the sampler in place with its knobs intact.
    void clearSamplerSample(const std::string& channelId, const std::string& slotId);
    /// The same load without an undo entry — what the undo and redo lambdas
    /// themselves call, so replaying a step cannot push another step.
    void loadSamplerSampleSilently(const std::string& channelId,
                                   const std::string& slotId,
                                   const std::string& filePath);
    /// Unload a sampler asset without manufacturing a local UndoStack entry.
    /// Used when a cloud binding is absent or not downloaded yet, so stale
    /// bytes from a previous projection cannot continue sounding.
    void clearSamplerSampleSilently(const std::string& channelId,
                                    const std::string& slotId);

    /// Put the built-in sampler in the track's instrument slot and load
    /// `filePath` into it — what dropping a sample on the slot means.
    ///
    /// One undo entry for the whole gesture: two entries would make the user
    /// press Ctrl+Z twice to undo one drag, and the intermediate state (an
    /// empty sampler) is not one anybody asked for. False when the track takes
    /// no notes, the built-in sampler is missing, or the file will not decode.
    bool loadInstrumentSampler(const std::string& trackId,
                               const std::string& filePath);

    /// Drain what the plugins have been reporting back: parameters they moved
    /// in their own editors, and latency changes (which force a rebuild so
    /// delay compensation follows). Returns true when the UI should redraw.
    /// Control thread, from the UI's existing periodic tick.
    bool pumpPluginEvents();
    /// Deterministic performance-test hook: counts full live-instance sweeps,
    /// not the O(1) no-work checks.
    std::uint64_t pluginEventScanCountForTest() const noexcept {
        return m_pluginEventScanCount;
    }

    // ── Waveforms ──
    WaveformCache& waveforms() { return m_waveforms; }
    const WaveformCache& waveforms() const { return m_waveforms; }

    /// Plain audio clip addressed for the shared Sample/Clip Editor.
    const ClipModel* audioClip(const std::string& trackId,
                               const std::string& clipId) const;
    /// Replace or clear the media referenced by one clip while keeping that
    /// clip's processing, stretch and private FX state. Empty clears it.
    bool setClipAudioFile(const std::string& trackId,
                          const std::string& clipId,
                          const std::string& filePath);
    /// Persist measured tempo/key facts on a clip. This is document metadata,
    /// so it is undoable but requires no graph rebuild.
    bool setClipMusicalAnalysis(const std::string& trackId,
                                const std::string& clipId,
                                const ClipMusicalAnalysisModel& analysis,
                                const std::string& label = "Analyze Audio Clip");
    /// Processed per-instance audio shown by the Clip editor and used by the
    /// placement renderer. Null for MIDI, layered or missing-file clips.
    std::shared_ptr<const plugins::sampler::SampleData> clipSampleData(
        const std::string& trackId, const std::string& clipId);
    double clipSampleParameter(const std::string& trackId,
                               const std::string& clipId,
                               const std::string& parameterId);
    /// Live edit; the matching commit call turns the gesture into one undo.
    void setClipSampleParameter(const std::string& trackId,
                                const std::string& clipId,
                                const std::string& parameterId, double value);
    /// The stretch factor to apply, given the one a knob is asking for and the
    /// grid the arrangement is snapping to (in seconds; 0 or less means no
    /// snapping). Near a grid line the clip's *end* is pulled onto it, so a
    /// stretch can be landed exactly on a bar without hunting for the pixel.
    ///
    /// The pull is a detent, not a quantiser: it covers part of the gap between
    /// two grid lines, so the knob still moves freely in between and simply
    /// resists a little as it passes one. Where grid lines are far apart in
    /// knob terms the band stays small instead of growing to cover the travel.
    double snappedStretchTime(const std::string& trackId,
                              const std::string& clipId, double wanted,
                              double gridSeconds) const;

    void commitClipSampleParameterEdit(const std::string& trackId,
                                       const std::string& clipId,
                                       const std::string& parameterId,
                                       double before,
                                       const std::string& label);

    // ── Engine introspection ──
    /// The compiled node graph currently driving audio.
    std::shared_ptr<const engine::CompiledGraph> routingGraph() const;
    /// The nodes that make up a channel, or nullptr for an unknown track.
    const TrackNodes* trackNodes(const std::string& trackId) const;
    /// Latency the graph compensates for, in samples.
    uint32_t latencySamples() const;
    unsigned workerCount() const;

    // ── Clips ──
    /// Create an audio track named after `trackName` (or the file), import the
    /// file at `startSeconds`, and expose the whole operation as one undo
    /// entry. Returns the new track id; on failure no track and no undo entry
    /// are left behind.
    std::string importAudioToNewTrack(const std::string& filePath,
                                      double startSeconds,
                                      const std::string& trackName = {},
                                      const ClipMusicalAnalysisModel& analysis = {});
    std::string importAudio(const std::string& filePath,
                            const std::string& trackId,
                            double startSeconds,
                            const ClipMusicalAnalysisModel& analysis = {});
    struct ClipStartChange {
        std::string trackId;
        std::string clipId;
        double startSeconds = 0.0;
    };
    void setClipStartSeconds(const std::string& trackId,
                             const std::string& clipId, double startSeconds);
    /// Move a selection in one control-thread transaction. Every affected
    /// track publishes at most one clip/note snapshot and the project duration
    /// is recomputed once, irrespective of the selection size.
    void setClipStartsSeconds(std::span<const ClipStartChange> changes);
    /// Bracket an arrangement position gesture. Geometry remains live in the
    /// document, while MIDI/plugin-automation snapshots and the transport
    /// duration are coalesced and published once by `endClipPositionEdit`.
    /// A non-empty end label records the complete move as one small position
    /// delta; the no-label overload remains a publication-only compatibility
    /// path. Calls outside this bracket keep their immediate behaviour.
    void beginClipPositionEdit();
    void endClipPositionEdit(const std::string& label = {});
    void setClipGain(const std::string& trackId,
                     const std::string& clipId, float gain);
    /// Set a clip's head/tail fade lengths in seconds (clamped so they fit
    /// inside the clip and don't overlap each other). Live, like trimming.
    void setClipFade(const std::string& trackId, const std::string& clipId,
                     double fadeInSeconds, double fadeOutSeconds);
    void commitClipFadeEdit(const std::string& trackId,
                            const std::string& clipId, double beforeIn,
                            double beforeOut, const std::string& label);
    /// Shape one fade without changing its length. Curve is -1…+1; mode adds
    /// tape-speed processing when requested. Both are live for direct UI use.
    void setClipFadeCurve(const std::string& trackId, const std::string& clipId,
                          bool fadeIn, double curve);
    void commitClipFadeCurveEdit(const std::string& trackId,
                                 const std::string& clipId, bool fadeIn,
                                 double beforeCurve,
                                 const std::string& label);
    void setClipFadeMode(const std::string& trackId, const std::string& clipId,
                         bool fadeIn, ClipFadeMode mode);
    /// Move a clip to another track, keeping its timeline position. The
    /// destination must accept the clip's kind (see `daw::trackAccepts`). Live;
    /// the arrangement commits the complete move gesture on release.
    void moveClipToTrack(const std::string& fromTrackId,
                         const std::string& clipId,
                         const std::string& toTrackId);
    /// Non-destructively resize a clip: set its timeline start, its start-in-
    /// source offset and its length together (used by edge-drag trimming). All
    /// three are clamped to the source's bounds. Live; the arrangement commits
    /// the complete trim gesture on release.
    void setClipTrim(const std::string& trackId, const std::string& clipId,
                     double startSeconds, double offsetSeconds,
                     double durationSeconds);
    /// Bracket one edge-drag. Geometry remains live, while playback snapshots
    /// and duration are published once on release. Only scalar trim state is
    /// retained, except that Automation clips retain their curve so a left-edge
    /// drag can be recomputed non-destructively from the gesture origin.
    void beginClipTrimEdit(const std::string& trackId,
                           const std::string& clipId);
    /// Bracket one edge-drag across a multi-selection. Every address is
    /// captured once and the entire mixed-kind resize becomes one undo entry.
    void beginClipTrimEdit(
        const std::vector<std::pair<std::string, std::string>>& clips);
    void endClipTrimEdit(const std::string& label = {});
    /// Split a clip in two at `atSeconds` (timeline time). Returns the id of the
    /// new right-hand clip, or empty when the cut is outside the clip. Undoable.
    std::string splitClip(const std::string& trackId, const std::string& clipId,
                          double atSeconds);
    void removeClip(const std::string& trackId, const std::string& clipId);

    /// Silence a clip without removing it. Discrete, so undoable.
    void setClipMuted(const std::string& trackId, const std::string& clipId,
                      bool muted);
    /// Rename a clip. Display-only — the source file is untouched. Undoable.
    void setClipName(const std::string& trackId, const std::string& clipId,
                     const std::string& name);
    /// Copy a clip onto the same track, placed immediately after the original.
    /// Returns the new clip's id, or empty when the source isn't found.
    /// Undoable.
    std::string duplicateClip(const std::string& trackId,
                              const std::string& clipId);
    /// Copy an existing clip to an exact timeline position. Used by repeat,
    /// where a multi-clip phrase must preserve its internal spacing.
    std::string duplicateClipAt(const std::string& trackId,
                                const std::string& clipId,
                                double startSeconds);
    /// Insert a clipboard snapshot with fresh ids at an exact position.
    std::string insertClipCopy(const std::string& trackId,
                               const ClipModel& source,
                               double startSeconds);
    /// Insert a Pattern clip and a captured set of its child MIDI clips as one
    /// independent instance. Used by arrangement copy/paste after the original
    /// may already have been cut from the document.
    using PatternClipMembers = std::vector<std::pair<std::string, ClipModel>>;
    std::string insertPatternClipCopy(const std::string& patternTrackId,
                                      const ClipModel& source,
                                      const PatternClipMembers& members,
                                      double startSeconds);

    /// Create an empty arrangement clip on a Pattern track. Child source clips
    /// can be attached later through the normal Pattern instrument/sample flow.
    std::string addPatternClip(const std::string& patternTrackId,
                               double startSeconds,
                               double lengthSeconds = 0.0);

    /// Load a plugin into the track's instrument slot, ahead of its inserts and
    /// fed by its MIDI clips. Empty descriptor uid clears the slot. Undoable.
    bool setTrackInstrumentPlugin(const std::string& trackId,
                                  const plugins::PluginDescriptor& descriptor);

    /// Set (or clear, with an empty name) the instrument at the head of a
    /// MIDI/instrument track's chain. A document-only placeholder for now —
    /// nothing in the engine reads it until plugin hosting lands. Undoable.
    void setTrackInstrument(const std::string& trackId, const std::string& name);
    /// Create an empty MIDI clip on a MIDI/instrument track. A `lengthSeconds`
    /// of 0 means one bar at the project's tempo and time signature. Returns the
    /// new clip's id, or empty when the track can't hold MIDI. Undoable.
    std::string addMidiClip(const std::string& trackId, double startSeconds,
                            double lengthSeconds = 0.0);

    // ── Takes and comping ──
    //
    // All times are seconds from the clip's own start, so a comp survives the
    // clip being moved or dropped on another lane. Swipes fire once per
    // mouse-move and are deliberately not undoable on their own; the gesture is
    // wrapped by `beginCompEdit`/`endCompEdit`, which lands one undo entry for
    // the whole stroke.

    /// Open the comp editor on a clip (the expanded, per-take view).
    void setClipExpanded(const std::string& trackId, const std::string& clipId,
                         bool expanded);
    /// Start a comp gesture: snapshots the clip's comp so the whole stroke can
    /// be undone as one edit. Nesting is a no-op, so a stroke that re-enters is
    /// still a single entry.
    void beginCompEdit(const std::string& trackId, const std::string& clipId);
    /// Finish the gesture opened by `beginCompEdit` and push it onto the undo
    /// stack. Does nothing when the comp came out unchanged.
    void endCompEdit();
    /// Paint `takeId` active over [fromSeconds, toSeconds) — one swipe.
    void setCompSegment(const std::string& trackId, const std::string& clipId,
                        const std::string& takeId, double fromSeconds,
                        double toSeconds);
    /// Make one take active for the clip's whole length (double-click a number).
    void selectTake(const std::string& trackId, const std::string& clipId,
                    const std::string& takeId);

    void setTakeName(const std::string& trackId, const std::string& clipId,
                     const std::string& takeId, const std::string& name);
    void setTakeColor(const std::string& trackId, const std::string& clipId,
                      const std::string& takeId, uint32_t color);
    void setTakeMuted(const std::string& trackId, const std::string& clipId,
                      const std::string& takeId, bool muted);
    void setTakeGain(const std::string& trackId, const std::string& clipId,
                     const std::string& takeId, float gain);
    /// Play only this take while auditioning, or an empty id to hear the comp
    /// again. Session state: it is not persisted and not undoable.
    void setSoloTake(const std::string& trackId, const std::string& clipId,
                     const std::string& takeId);
    const std::string& soloTake() const { return m_soloTakeId; }
    /// Copy a take inside its clip, placed right after the original. Returns the
    /// new take's id. The copy shares the original's audio file.
    std::string duplicateTake(const std::string& trackId,
                              const std::string& clipId,
                              const std::string& takeId);
    /// Add an audio file to a clip as another take, the way a recorded pass
    /// would land: whatever the clip already played becomes Take 1 first, so
    /// nothing is lost, and the new take is made active over its own stretch.
    /// Returns the new take's id. `clipOffsetSeconds` places it against the
    /// clip's start. This is the only way to layer a clip without a live
    /// recording — the demo project and "import as take" both go through it.
    std::string addTakeFromFile(const std::string& trackId,
                                const std::string& clipId,
                                const std::string& filePath,
                                double clipOffsetSeconds = 0.0,
                                const std::string& name = "");
    /// Remove a take. `deleteFile` also erases its audio from disk, which is
    /// only safe once nothing else references it — the call checks. Undoable in
    /// the document; a deleted file is not restored by undo.
    void removeTake(const std::string& trackId, const std::string& clipId,
                    const std::string& takeId, bool deleteFile = false);
    /// Reorder takes within a clip. Display order only — the comp decides what
    /// plays, so this changes nothing audible.
    void moveTake(const std::string& trackId, const std::string& clipId,
                  const std::string& takeId, size_t targetIndex);

    /// Render the comp — crossfades included — into a new take, keeping the
    /// sources. Returns the new take's id, or empty when there is nothing to
    /// bake. The rendered file lands in the record directory.
    ///
    /// `recordUndo` is false when a larger command (Commit) is going to push an
    /// entry of its own: two entries for one gesture would mean two undos to
    /// walk it back.
    std::string flattenComp(const std::string& trackId,
                            const std::string& clipId, bool recordUndo = true);
    /// Bake the comp down and dissolve the container: the clip keeps playing
    /// exactly what it played, as a plain single-layer clip with no takes, no
    /// comp and nothing left to expand. Destructive — the other attempts are
    /// gone from the document (their files are left alone).
    void commitComp(const std::string& trackId, const std::string& clipId);
    /// Delete every take no comp references, across the whole project. Returns
    /// how many were removed.
    size_t deleteUnusedTakes(bool deleteFiles = false);
    /// Trim each take's stored audio to the stretch the comp actually plays,
    /// rewriting the files to reclaim disk. Returns how many were rewritten.
    size_t cropToComp(const std::string& trackId, const std::string& clipId);

    // ── MIDI notes ──
    //
    // Note times are beats measured from the clip's own start, so they follow
    // the tempo. Adds and removes are discrete edits and go on the undo stack;
    // moving and resizing fire once per mouse-move and deliberately do not,
    // exactly like dragging or trimming a clip.

    /// Returns the new note's id, or empty when the clip isn't a MIDI clip.
    std::string addNote(const std::string& trackId, const std::string& clipId,
                        int pitch, double startBeats, double lengthBeats,
                        int velocity = 100);
    void removeNote(const std::string& trackId, const std::string& clipId,
                    const std::string& noteId);
    /// Move and/or resize a note in one call. Values are clamped to the legal
    /// pitch range and a minimum length, but *not* to the clip's length —
    /// shortening a clip must not destroy the notes past its new end.
    void setNote(const std::string& trackId, const std::string& clipId,
                 const std::string& noteId, int pitch, double startBeats,
                 double lengthBeats);
    /// Apply a whole live multi-note gesture. While bracketed by begin/end the
    /// owning track is published once, when the gesture ends.
    /// Entries are matched by id; fields are clamped with the same rules as
    /// `setNote` and the per-parameter setters. The surrounding begin/end edit
    /// pair remains responsible for the single undo entry.
    void setNoteStates(const std::string& trackId, const std::string& clipId,
                       std::span<const NoteModel> notes);
    /// Erase several notes in one document mutation and one engine snapshot.
    void removeNotes(const std::string& trackId, const std::string& clipId,
                     std::span<const std::string> noteIds);
    void setNoteVelocity(const std::string& trackId, const std::string& clipId,
                         const std::string& noteId, int velocity);
    /// Silence a note without removing it. Live like the velocity setter, so
    /// the Mute tool can paint across a run of notes in one gesture.
    void setNoteMuted(const std::string& trackId, const std::string& clipId,
                      const std::string& noteId, bool muted);

    /// Place a note in the stereo field, −1 … 1. The document updates live
    /// per mouse-move; a bracketed drag republishes the affected track once on
    /// mouse-up.
    void setNotePan(const std::string& trackId, const std::string& clipId,
                    const std::string& noteId, float pan);

    /// Bracket live Piano Roll mutations. The document stays immediate for
    /// drawing, while playback publication and the one history entry are
    /// deferred until the gesture ends.
    void beginNoteEdit(const std::string& trackId, const std::string& clipId);
    void endNoteEdit(const std::string& label);
    /// Monotonic invalidation token for note geometry on one track. It changes
    /// as soon as ids, pitch, timing, or membership mutate, including while
    /// realtime publication is deferred by a Piano Roll gesture. Playback-only
    /// velocity/mute samples do not force a Piano Roll geometry rebuild.
    std::uint64_t midiNotesRevision(const std::string& trackId) const;
    /// Diagnostic count of lazy id-index builds for active property gestures.
    /// Repeated mouse samples in one gesture reuse the same index.
    std::uint64_t noteEditIndexBuildCount() const noexcept {
        return m_noteEditIndexBuildCount;
    }

    /// Import a Standard MIDI File, one clip per track in the file.
    ///
    /// The first of the file's note-carrying tracks lands on `trackId`; every
    /// other one gets an instrument lane of its own, named from the file. (A
    /// format-1 file usually spends its first track on tempo and nothing else,
    /// so the ordinary two-track export still arrives as a single clip.)
    ///
    /// The file's tempo is **reported** through `outInfo`, never applied: notes
    /// are stored in beats, so they play at the project's tempo, and adopting
    /// the file's is a decision for the caller to offer. Returns the new clip
    /// ids, empty when the file will not parse or the track takes no notes.
    /// One undo entry covers the whole import, lanes included.
    std::vector<std::string> importMidiFile(const std::string& filePath,
                                            const std::string& trackId,
                                            double startSeconds,
                                            midifile::File* outInfo = nullptr);

    // ── Live notes ──
    //
    // Notes played *now* rather than written down: a key clicked on the piano
    // roll's keyboard, the typing keyboard, later a MIDI controller. They go
    // straight to the track's instrument through the same MIDI path the clips
    // use, sound whether or not the transport is rolling, and change nothing in
    // the document — nothing here is undoable, because nothing here is an edit.

    /// Start a note on the track's instrument. False when the track cannot take
    /// notes (no MIDI path), or the queue to the audio thread is full.
    bool liveNoteOn(const std::string& trackId, int pitch, int velocity = 100);
    /// End one. Always send this for every `liveNoteOn`, or the synth holds the
    /// note forever — there is no timeline here to end it.
    bool liveNoteOff(const std::string& trackId, int pitch);
    /// Send one three-byte MIDI channel-voice message to a track now. System
    /// messages and bytes outside 0…127 are rejected at this public boundary.
    bool liveMidiEvent(const std::string& trackId, int status, int data1,
                       int data2 = 0);
    /// The track a live note would sound on: `preferred` when it takes notes,
    /// otherwise the first track that does. Empty when the project has none.
    std::string liveNoteTarget(const std::string& preferred = {}) const;

    // ── Auditioning a file ──
    //
    // Listening to something that is not in the project: the browser's preview.
    // Like live notes it changes nothing in the document and is not undoable,
    // and like the metronome it comes from a node that is always in the graph.
    // It sounds with the transport parked, and it is silent in an export.

    /// Decode `filePath` and play it. The decode happens here, on the calling
    /// thread — a UI that must not stall should decode on a worker and use
    /// `previewBuffer`. False when the file will not decode.
    bool previewFile(const std::string& filePath, bool loop,
                     double pitchSemitones = 0.0);
    /// Play a buffer somebody else decoded. `sourcePath` is remembered only so
    /// the UI can ask what is playing; the buffer is owned by the preview node
    /// alone and deliberately does **not** go into the decoded-sample memo,
    /// which would retain every file the user ever clicked.
    bool previewBuffer(std::shared_ptr<const engine::SampleBuffer> audio,
                       const std::string& sourcePath, bool loop,
                       double pitchSemitones = 0.0);
    void stopPreview();
    void setPreviewLoop(bool loop);
    void setPreviewGain(float gain);
    /// Move the audition head, in seconds into the source.
    void seekPreviewSeconds(double seconds);

    bool previewPlaying() const;
    /// Where the audition head is, in seconds into the source. 0 when stopped.
    double previewPositionSeconds() const;
    double previewDurationSeconds() const { return m_previewDuration; }
    /// The file currently armed, empty when none.
    const std::string& previewPath() const { return m_previewPath; }

    /// How many decoded files are being held in memory for the project's clips
    /// and samplers. Auditioning must not add to this — a preview is listened
    /// to and forgotten — and a test asserts exactly that.
    std::size_t decodedFileCount() const { return m_samples.size(); }

    // ── Controller lanes ──
    //
    // Curves drawn under the notes: mod wheel, expression, or a plugin
    // parameter once hosting lands. Nothing in the engine reads them yet.

    /// While this is on and the transport is rolling, moving a control in a
    /// plugin's own editor writes a breakpoint into the lane that automates it.
    ///
    /// Off by default, and deliberately so: without a write-enable, opening a
    /// plugin and looking at it would rewrite the automation the user drew.
    /// Only parameters that already have a lane are recorded — creating lanes
    /// behind the user's back is the other half of that same surprise.
    void setAutomationWriteEnabled(bool enabled) { m_automationWrite = enabled; }
    bool automationWriteEnabled() const noexcept { return m_automationWrite; }

    /// Returns the new lane's id, or empty when the clip isn't a MIDI clip.
    std::string addControllerLane(const std::string& trackId,
                                  const std::string& clipId,
                                  const std::string& name, int cc);
    void removeControllerLane(const std::string& trackId,
                              const std::string& clipId,
                              const std::string& laneId);
    /// Point a lane at a plugin parameter instead of a MIDI controller.
    /// `slotId` is an insert's id, or empty for the track's instrument; an
    /// empty `parameterId` turns the lane back into a plain CC lane.
    void setLaneTarget(const std::string& trackId, const std::string& clipId,
                       const std::string& laneId, const std::string& slotId,
                       const std::string& parameterId);
    /// Replace a lane's breakpoints. Live and *not* undoable, because drawing a
    /// curve is a gesture that fires per mouse-move — `commitLaneEdit` is what
    /// turns the finished gesture into one undo entry.
    void setLanePoints(const std::string& trackId, const std::string& clipId,
                       const std::string& laneId,
                       std::vector<AutomationPoint> points);
    /// Record an already-applied lane gesture, given the points as they were
    /// before it started.
    void commitLaneEdit(const std::string& trackId, const std::string& clipId,
                        const std::string& laneId,
                        std::vector<AutomationPoint> before,
                        const std::string& label);

    // ── Automation clips ───────────────────────────────────────────────────
    //
    // A curve is a *clip* on an automation lane, and an automation lane is a
    // track — `TrackKind::Automation`, parented to the track it belongs under.
    // `visibleTracks()` applies the owner's independent automation disclosure,
    // so hiding curves never collapses real folder/pattern children. Lane
    // headers, indentation, reordering and dragging a clip to another lane all
    // still come from the ordinary track hierarchy.
    //
    // What a clip drives lives on the clip, never on the lane — so one lane can
    // carry curves for several different things, and a duplicated clip can be
    // re-pointed without its shape being redrawn.

    /// A human name for a target, used to label a lane and its clips.
    std::string automationTargetName(const AutomationTarget& target) const;
    /// Where the thing being automated stands *right now*, normalised — so a
    /// fresh curve starts at the setting the user can already see rather than
    /// yanking the control to zero the moment a lane is created.
    double defaultAutomationValue(const AutomationTarget& target) const;
    /// The target's neutral/factory value for resetting a breakpoint: unity for
    /// a channel fader, centre for pan, off for mute, the send knob's default,
    /// and the plugin-reported default for plugin parameters.
    double automationResetValue(const AutomationTarget& target) const;
    /// Normalised 0…1 → the target's own units, and back. The one place the two
    /// scales meet; plugin parameters resolve their range from the live plugin.
    double automationToPlain(const AutomationTarget& target, double normalized) const;
    double plainToAutomation(const AutomationTarget& target, double plain) const;
    /// The plain value an active automation clip supplies at the current
    /// playhead, or no value when this target has no enabled curve. UI faders
    /// use this to mirror what the audio engine is actually playing.
    std::optional<double> automationValueAtPlayhead(
        const AutomationTarget& target) const;

    /// A curve value as the user reads it: decibels for a level, L/R for a pan,
    /// on or off for a mute, and the plugin's own words for a parameter. One
    /// spelling for the editor's readout, the lane's tooltip and the context
    /// panel, so the same number never appears in two different units.
    std::string automationValueText(const AutomationTarget& target,
                                    double normalized) const;

    /// Create an automation lane under `trackId` (empty for a free-standing
    /// one). Returns the lane track's id. Expands the parent, because a lane
    /// nobody can see is not what asking for one means.
    std::string addAutomationLane(const std::string& trackId,
                                  const AutomationTarget& target);
    /// The automation lanes filed under a track, in display order.
    std::vector<std::string> automationLanesOf(const std::string& trackId) const;
    void removeAutomationLane(const std::string& laneTrackId);

    /// A curve on a lane. `lengthSeconds` of 0 means "as far as the content
    /// goes", which is what a fresh automation clip should cover.
    std::string addAutomationClip(const std::string& laneTrackId,
                                  const AutomationTarget& target,
                                  double startSeconds, double lengthSeconds = 0.0);
    /// Re-point an existing curve at something else. The shape is untouched —
    /// this is what makes copying a curve onto another parameter worth doing.
    void setAutomationTarget(const std::string& trackId, const std::string& clipId,
                             const AutomationTarget& target);

    /// The lane and clip that already drive `target`, or two empty strings.
    /// Searched project-wide rather than under one track: a curve is free to
    /// sit on any lane, and "is this already automated" is a question about the
    /// parameter, not about where somebody filed it.
    std::pair<std::string, std::string> findAutomation(
        const AutomationTarget& target) const;

    /// Automate `target` — or hand back what already does, opening the track so
    /// it can be seen. One undo entry either way, and the same answer every
    /// time it is asked, which is what makes double-clicking a knob safe to
    /// repeat and safe to do by accident.
    std::pair<std::string, std::string> ensureAutomation(const AutomationTarget& target);
    /// Replace a curve's breakpoints. Live and *not* undoable, for the same
    /// reason `setLanePoints` is not: drawing is a gesture that fires per
    /// mouse-move. `commitAutomationEdit` turns the finished gesture into one
    /// undo entry.
    void setAutomationPoints(const std::string& trackId, const std::string& clipId,
                             std::vector<AutomationPoint> points,
                             bool active = true);
    void commitAutomationEdit(const std::string& trackId, const std::string& clipId,
                              std::vector<AutomationPoint> before,
                              const std::string& label,
                              bool activeBefore);

    /// Replace a MIDI clip's whole note vector in one undoable step, labelled
    /// for the undo menu.
    ///
    /// This is the primitive every piano-roll *command* goes through — quantise,
    /// arpeggiate, glue, strum, randomise, paste, transpose. Each of those is a
    /// pure `vector<NoteModel>` transform (see MidiTools.hpp) that knows nothing
    /// about the document; this is where the result lands, gets clamped, and
    /// becomes a single undo entry instead of one per touched note.
    void setClipNotes(const std::string& trackId, const std::string& clipId,
                      std::vector<NoteModel> notes, const std::string& label);

    // ── Master / metering ──
    void setMasterVolume(float volume);
    void setMasterVolumeLive(float volume);
    void commitMasterVolumeEdit(float before,
                                const std::string& label = "Set Master Volume");
    float masterVolume() const { return m_project.masterVolume; }
    void setMasterPan(float pan);
    void setMasterPanLive(float pan);
    void commitMasterPanEdit(float before,
                             const std::string& label = "Set Master Pan");
    float masterPan() const { return m_project.masterPan; }
    float trackPeak(const std::string& trackId) const;
    float trackRms(const std::string& trackId) const;
    float masterPeak() const;
    float masterRms() const;
    float masterPeakLeft() const;
    float masterPeakRight() const;
    engine::RealtimeEngine::MasterSpectrum masterSpectrum() const;
    void addMasterSpectrumConsumer() noexcept;
    void removeMasterSpectrumConsumer() noexcept;
    float dspLoad() const;

    // ── Recording ──
    /// How a recording behaves when it lands on existing material. The mode is
    /// resolved per track: a track set to `UseGlobal` follows `recordMode()`,
    /// and holding the temporary-invert key flips whatever that resolves to.
    void setRecordMode(RecordMode mode) { m_recording.mode = mode; }
    RecordMode recordMode() const { return m_recording.mode; }
    void setTrackRecordMode(const std::string& trackId, TrackRecordMode mode);
    /// Flip the resolved mode for as long as this stays set — the Ctrl+Shift+L
    /// "just this take" override.
    void setRecordModeInverted(bool inverted) { m_recording.inverted = inverted; }
    bool isRecordModeInverted() const { return m_recording.inverted; }
    /// The mode a recording on this track would actually use, after the track
    /// override and the temporary invert.
    RecordMode effectiveRecordMode(const std::string& trackId) const;

    /// What happens to input monitoring when a recording stops.
    enum class MonitorStopPolicy : std::uint8_t {
        KeepOn,             ///< leave it on, so the take can be heard live
        ReturnToPrevious,   ///< restore whatever it was before the recording
        AutoDisable,        ///< always switch it off
    };

    /// Everything under Preferences ▸ Recording. Plain data so the UI can read
    /// and write it wholesale and the controller has one thing to consult.
    struct RecordingPrefs {
        RecordMode mode = RecordMode::Overwrite;
        bool inverted = false;              ///< temporary invert, not persisted
        /// Loop recording keeps every pass as its own take. With this off the
        /// passes overwrite each other and only the last one survives.
        bool loopCreatesTakes = true;
        /// A punch-in take covers only the stretch that was actually recorded,
        /// rather than the clip's whole length padded with silence.
        bool trimTakesToRegion = true;
        /// Open the comp editor on the clip as soon as the recording stops.
        bool autoExpandAfterRecord = false;
        /// Crossfade applied at the seams between comp segments, in
        /// milliseconds (0 … 20).
        double compCrossfadeMs = 5.0;
        /// Whether a MIDI take replaces the clip's notes or merges into them.
        bool midiOverdubMerge = false;
        /// R alone arms the selected track and starts recording, instead of
        /// requiring the transport's Record button to be engaged first.
        bool recordKeyArmsAndStarts = true;
        /// Beats of clicked count-in before recording starts. 0 is off; 3 is
        /// the familiar "three, two, one".
        int countInBeats = 3;
        /// Recording opens the target's input monitor for the take and closes it
        /// again on stop, so a performer hears themselves exactly while they are
        /// being recorded. Switched off, recording never touches monitoring at
        /// all: a track is heard live only if its own monitor button is on.
        bool autoMonitorOnRecord = true;
        MonitorStopPolicy monitorStopPolicy = MonitorStopPolicy::ReturnToPrevious;
        /// A manual click on the monitor button hands that track back to the
        /// user, so smart monitoring stops touching it.
        bool manualMonitorDisablesAuto = true;
    };

    /// Immutable recording decisions captured at the instant audio capture
    /// starts. A take must not change from Layers to Overwrite (or acquire a
    /// different loop split) because Preferences or the local loop were edited
    /// while its recorder was running.
    struct FrozenRecordingSemantics {
        RecordMode mode = RecordMode::Overwrite;
        bool loopEnabled = false;
        double loopStartSeconds = 0.0;
        double loopEndSeconds = 0.0;
        bool loopCreatesTakes = true;
        bool trimTakesToRegion = true;
        bool autoExpandAfterRecord = false;
        double compCrossfadeMs = 5.0;
        bool autoMonitorOnRecord = true;
        MonitorStopPolicy monitorStopPolicy =
            MonitorStopPolicy::ReturnToPrevious;
    };

    /// One closed local capture. The raw path and accounting survive every
    /// writer outcome. `audioReadable` means a non-empty WAV passed a metadata
    /// probe; it may still be a recoverable prefix when `fileWriteSucceeded`
    /// is false.
    struct FinalizedRecordingTrack {
        std::string trackId;
        std::string closedWavPath;
        double startSeconds = 0.0;
        double durationSeconds = 0.0;
        /// Frames described by the probed WAV, or the writer-confirmed prefix
        /// when the container could not be probed.
        std::uint64_t frames = 0;
        std::uint64_t capturedFrames = 0;
        std::uint64_t writtenFrames = 0;
        std::uint64_t droppedFrames = 0;
        double sampleRate = 0.0;
        std::uint32_t channels = 0;
        std::vector<RecordingSpan> passes;
        FrozenRecordingSemantics semantics;
        bool fileWriteSucceeded = false;
        bool audioReadable = false;
    };

    /// Capture finalization is deliberately separate from document landing.
    /// Cloud recording can retain/upload these files without touching shared
    /// clips or entering the closure-based local undo stack.
    struct FinalizedRecordingRun {
        std::vector<FinalizedRecordingTrack> tracks;

        bool empty() const noexcept { return tracks.empty(); }
    };

    const RecordingPrefs& recordingPrefs() const { return m_recording; }
    void setRecordingPrefs(const RecordingPrefs& prefs);

    /// Start recording onto one or more tracks, right now. Each target is armed
    /// if it is not already, and smart monitoring decides whether to open its
    /// monitor. For a counted-in start use `armCountIn`.
    bool startRecording(const std::string& trackId);
    bool startRecordingTracks(const std::vector<std::string>& trackIds);
    /// Strict seam for cloud recording: every id must be unique, exist, and be
    /// recordable. The start is all-or-nothing; a recorder failure closes any
    /// recorder already prepared and publishes no partial capture set.
    bool canStartRecordingTracksExactly(
        const std::vector<std::string>& trackIds) const;
    bool startRecordingTracksExactly(
        const std::vector<std::string>& trackIds);
    /// Count-in counterpart of the exact start. Its final beat preserves the
    /// same all-target rule instead of falling back to the legacy subset path.
    bool armCountInExactly(const std::vector<std::string>& trackIds,
                           int beats);
    /// Stop and close active recorders, restoring local input state and the
    /// graph, but never mutating clips/takes/comp or pushing an undo entry.
    FinalizedRecordingRun finalizeRecordingCapture();
    /// Stop and land what was captured: as a replacement clip in Overwrite mode,
    /// or as one take per loop pass in Layer mode. Returns the path of the
    /// captured file for the first track (empty when nothing was recorded).
    std::string stopRecording();
    bool isRecording() const;
    /// The tracks currently capturing, in the order recording started.
    const std::vector<std::string>& recordingTracks() const {
        return m_recordingTracks;
    }
    /// Where on the timeline the capture on `trackId` began, or −1 when that
    /// track is not recording. The timeline draws the take-to-be from here to
    /// the playhead.
    double recordingStartSeconds(const std::string& trackId) const;
    /// Count `beats` clicks off, then start recording onto `trackIds` at the
    /// playhead. The transport stays parked while it counts — rewinding into a
    /// pre-roll needs room before the punch point, which a take at bar 1 never
    /// has — so the clicks come from the metronome's own clock and the take
    /// still begins exactly where it was asked for. `beats` of 0 or less starts
    /// immediately. Returns false when nothing could be recorded onto.
    bool armCountIn(const std::vector<std::string>& trackIds, int beats);
    /// Advance a count-in by `deltaSeconds`; returns true on the tick that
    /// started the take. Driven by the UI's clock rather than an internal one,
    /// so it stays testable and there is only one timer in the program.
    bool tickCountIn(double deltaSeconds);
    /// Drop a count-in without recording (Stop, or Record pressed again).
    void cancelCountIn();
    /// True between arming a count-in and the take starting.
    bool isCountingIn() const;
    /// Seconds left of the count-in, or 0 when none is running.
    double countInRemainingSeconds() const;
    /// Beats left of the count-in: 3, 2, 1 — the number to show.
    int countInBeatsRemaining() const;
    /// Everything the arrangement needs to draw the take running on `trackId`
    /// as the clip it is about to become. Inactive when that track is not
    /// recording.
    RecordingPreview recordingPreview(const std::string& trackId);
    /// Sample the input meters into every active capture's envelope. Called
    /// from the UI's refresh tick; each sample lands in a fixed slice of
    /// *recorded* time, so the picture neither stretches nor slides when the
    /// frame rate wobbles.
    void pumpRecordingEnvelopes();
    /// Offline hook: pretend `seconds` of input have been captured on
    /// `trackId`, shaped by `level(t)` (a flat 0.6 when it is not given).
    /// An offline harness has no audio device, so the recorder's own clock
    /// never moves and a running take can otherwise be neither photographed nor
    /// asserted on past its first block. Ignored when the track is not
    /// recording.
    void seedRecordingForShot(const std::string& trackId, double seconds,
                              const std::function<float(double)>& level = {});
    /// Directory new captures are written to. Defaults to a temporary
    /// directory; saving a project points it at the package's media folder.
    void setRecordDirectory(const std::string& dir) { m_recordDir = dir; }
    const std::string& recordDirectory() const { return m_recordDir; }

    // ── Analysis ──
    /// Render one channel on its own and measure it — level, headroom, clipping
    /// and rough tonal balance. Exists because the AI assistant cannot hear:
    /// with numbers it can mix, without them it can only guess from names.
    ///
    /// The channel is soloed for the render and put back afterwards, all under
    /// one `UndoStack::Suspend`, so the measurement leaves no trace. Pass
    /// `kMasterChannelId` to measure the mix as it stands.
    audio::Result analyzeChannel(const std::string& channelId,
                                 double fromSeconds, double toSeconds,
                                 analysis::Metrics& out);

    /// Decode a file and describe it: level, attack, whether it holds a pitch,
    /// and a word for what it probably is. Does not enter the sample memo.
    audio::Result analyzeSampleFile(const std::string& filePath,
                                    analysis::SampleTraits& out);

    // ── Offline export ──
    /// Render the project to disk: mixdown, stems, or both, in one pass.
    ///
    /// One pass is the point. Stems are captured by hanging leaf taps off the
    /// channels the caller named, so they come out of the very same render as
    /// the mixdown and are guaranteed to sum back to it. Rendering each stem in
    /// its own pass — soloing a channel at a time — cannot promise that: a
    /// pre-fader send from a silenced track still feeds its bus, so a reverb
    /// return would bleed into every stem.
    ///
    /// Blocking, and it parks live audio for its whole duration (`renderOffline`
    /// holds the render gate). `onProgress` runs on the calling thread after
    /// each block; returning false from it cancels, and a cancelled render
    /// leaves no files behind.
    ///
    /// Every temporary change it makes to the project — bypassed inserts,
    /// cleared mutes, a different sample rate — is restored before it returns,
    /// including when it throws, and none of it reaches the undo stack.
    audio::Result renderProject(
        const rendering::Spec& spec,
        const std::function<bool(const rendering::Progress&)>& onProgress,
        rendering::Report& out);

    /// Stereo float WAV of the whole project. The old entry point, kept for the
    /// callers that only ever wanted that.
    audio::Result exportMixdown(const std::string& outputPath,
                                bool normalize = false);

    // ── Undo / redo ──
    // History closures publish the exact realtime state they mutate. Keeping
    // undo/redo as a pure dispatcher is important: a one-note edit must not
    // rebuild every MIDI track in a large project after its own targeted sync.
    void undo();
    void redo();
    bool canUndo() const { return m_undo.canUndo(); }
    bool canRedo() const { return m_undo.canRedo(); }
    /// What the next undo/redo would actually do, for menu items that name it
    /// ("Undo Quantize") instead of leaving the user to guess.
    std::string undoLabel() const { return m_undo.undoLabel(); }
    std::string redoLabel() const { return m_undo.redoLabel(); }

    /// Put the whole document back to a captured state, as one undo entry.
    ///
    /// The escape hatch for a long compound operation the user wants gone after
    /// they have already carried on working — the assistant's "revert this
    /// request", which cannot use `undo()` because the entry is no longer on
    /// top of the stack.
    void restoreProject(const ProjectModel& snapshot, const std::string& label);
    /// Replace any primitive entries produced during a complex live gesture
    /// with one whole-project before/after entry. Used by arrangement drags
    /// whose result may span clips or tracks.
    void commitProjectGesture(const ProjectModel& before,
                              std::size_t undoDepthBefore,
                              const std::string& label);

    /// Where the undo stack stands, for a caller that will later fold
    /// everything it did into one entry.
    std::size_t undoDepth() const { return m_undo.depth(); }
    std::uint64_t projectRevision() const { return m_undo.revision(); }
    /// How deep the stack can get. Beyond it `undoDepth` stops rising, so a
    /// caller comparing depths across an edit has to expect the ceiling.
    std::size_t undoLimit() const { return m_undo.limit(); }
    /// Fold every entry pushed since `sinceDepth` into a single one. For work
    /// that cannot hold an `UndoStack::Suspend` because it spans waits the user
    /// can edit through — see `UndoStack::collapse`.
    void collapseUndo(std::size_t sinceDepth, const std::string& label) {
        if (cloudProjectBound()) return;
        const std::size_t now = m_undo.depth();
        if (now > sinceDepth) m_undo.collapse(now - sinceDepth, label);
    }

    /// Eviction-safe compound history for UI gestures and multi-selection
    /// commands. An open group may temporarily exceed the history limit; its
    /// primitive entries are folded first and the limit is applied once, so a
    /// full stack still retains one complete undo for the user's action.
    using UndoGroup = UndoStack::Group;
    UndoGroup beginUndoGroup() {
        return cloudProjectBound() ? UndoGroup{} : m_undo.beginGroup();
    }
    void collapseUndo(UndoGroup group, const std::string& label) {
        if (cloudProjectBound()) return;
        m_undo.collapseGroup(group, label);
    }
    void releaseUndoGroup(UndoGroup group) {
        if (!cloudProjectBound()) m_undo.releaseGroup(group);
    }

    // ── Devices ──
    std::vector<audio::DeviceInfo> enumerateOutputDevices();
    std::vector<audio::DeviceInfo> enumerateInputDevices();
    std::string currentOutputDeviceUid();
    std::string currentInputDeviceUid();
    audio::DeviceInfo currentInputDeviceInfo() const;
    audio::AudioDeviceConfig audioConfiguration() const;
    uint32_t bufferSizeFrames() const { return m_bufferSize; }
    audio::Result applyAudioConfiguration(
        const audio::AudioDeviceConfig& config);
    audio::Result setOutputDevice(const std::string& uid);
    audio::Result setInputDevice(const std::string& uid);
    /// What one device will actually run at. Costs a stream negotiation per
    /// rate, so it is asked for the selected device only — see
    /// `AudioDeviceManager::probeDevice`.
    audio::Result probeDevice(const std::string& uid, bool wantInput,
                              audio::DeviceInfo& out);
    /// Open an ASIO driver's own settings panel. Fails with NotSupported on a
    /// device the operating system configures, which is all of them outside
    /// Windows.
    audio::Result showDeviceControlPanel(const std::string& uid,
                                         void* nativeWindow = nullptr);

    audio::Result setSampleRateHz(double hz);
    audio::Result setBufferSizeFrames(uint32_t frames);

    /// Local source for an audio clip that is already shared but whose bytes
    /// have not finished uploading. It exists only on the importing machine and
    /// never reaches a command or the canonical document; it is what lets the
    /// clip play here the instant it is dropped, while every other participant
    /// sees it silent until clip.setAsset lands. Empty when the clip is not
    /// awaiting an upload.
    std::string pendingLocalAudioPath(const std::string& clipId) const;

private:
    class DeviceCallback;   // bridges the PortAudio callback to the engine

    collab::SharedMutationResult submitSharedMutation(
        collab::CommandBody body, std::string undoLabel,
        std::optional<std::string> transactionId = std::nullopt);
    /// Shares an imported audio clip before its bytes exist in the cloud.
    /// `clip` must carry an empty asset: the clip.add batch goes out now, so
    /// the clip appears for every participant immediately, and the verified
    /// AssetRef follows as its own clip.setAsset once the upload lands. Until
    /// then the clip is silent for everyone except the importer, who plays it
    /// from the local cache. Returns the clip id, or empty when nothing was
    /// shared.
    std::string submitOptimisticSharedAudioClip(
        collab::SharedAssetMutationRequest request, const std::string& trackId,
        ClipModel clip, const std::string& afterId, std::string undoLabel);
    /// Same contract for a drop onto empty space, which shares a new track and
    /// its single clip together. Returns the track id.
    std::string submitOptimisticSharedAudioTrack(
        collab::SharedAssetMutationRequest request, TrackModel track,
        const std::string& afterId);
    bool cloudProjectBound();

    /// Bookkeeping for a clip shared ahead of its upload: where it plays from
    /// on this machine, and which asset request is still in flight for it.
    struct PendingLocalAudio {
        std::string sourcePath;
        std::string requestId;
    };
    /// clipId -> that bookkeeping.
    std::unordered_map<std::string, PendingLocalAudio> m_pendingLocalAudio;
    /// Cancels the upload behind any pending import whose clip has gone —
    /// undone, deleted here, or deleted by another participant. Without this an
    /// undone import would keep uploading bytes nothing will ever reference.
    void retireOrphanedPendingAudioImports();

    /// Rebuild the whole node graph from the document and publish it.
    audio::Result rebuildGraph(bool reconfigurePlugins = false);
    /// Push the document's clip list for one track into its player node.
    void syncTrackClips(const TrackModel& track);
    /// Flatten the track's MIDI clips into one timeline-ordered note list.
    void syncTrackNotes(const TrackModel& track,
                        bool geometryChanged = true);
    bool noteEditTargets(const std::string& trackId,
                         const std::string& clipId) const;
    void captureNoteEditBeforeMutation(const std::string& trackId,
                                       const std::string& clipId,
                                       const ClipModel& clip);
    NoteModel* indexedNoteForActiveEdit(const std::string& trackId,
                                        const std::string& clipId,
                                        ClipModel& clip,
                                        const std::string& noteId);
    void captureNoteDeltaBefore(const NoteModel& note);
    void publishOrDeferNotePlayback(const std::string& trackId,
                                    const std::string& clipId,
                                    const TrackModel& track,
                                    bool geometryChanged);
    void bumpMidiNotesRevision(const std::string& trackId);
    /// The same for every track that carries notes, reserved for genuinely
    /// document-wide musical-time changes such as tempo. Live/history gestures
    /// publish their affected tracks in their own paths.
    void syncAllNotes();
    /// Turn a track's plugin-parameter lanes into curves on the plugin nodes.
    void syncTrackAutomation(const TrackModel& track);
    void followPassiveAutomation(const AutomationTarget& target,
                                 double normalized);
    void syncAllAutomation();
    /// Upgrade pre-Pattern-clip projects in memory and repair unowned child MIDI
    /// clips. The migration is silent and is persisted on the next save.
    void repairPatternClips();
    audio::Result activateProject(
        ProjectModel loaded, const std::string& packageDir,
        const std::string& fallbackPackageDir = {},
        bool toleratePluginStateErrors = false);
    /// Pattern clip edits touch several tracks but are one user gesture. Store a
    /// whole-document before/after pair so undo cannot expose a half-moved group.
    void pushProjectSnapshotUndo(const ProjectModel& before,
                                 const std::string& label);
    /// Copy a Pattern clip plus explicit member snapshots, with a caller-chosen
    /// undo label (Duplicate vs Paste).
    std::string insertPatternClipCopyImpl(
        const std::string& patternTrackId, const ClipModel& source,
        const PatternClipMembers& members, double startSeconds,
        const std::string& undoLabel);
    /// Record one plugin-editor gesture as a breakpoint at the playhead.
    void writeAutomationPoint(const std::string& channelId, const std::string& slotId,
                              const std::string& parameterId, double value);
    /// The placements one clip renders as, appended to `list`. A plain clip is
    /// one placement; a layered clip is one per comp segment, with equal-power
    /// crossfades at the seams between them. Returns the index range it
    /// occupies in `list`, so the caller can crossfade whole clips against each
    /// other without reaching inside a comp.
    struct PlacementSpan {
        size_t first = 0;
        size_t count = 0;
        engine::SamplePos startSample = 0;
        engine::SamplePos endSample = 0;
    };
    PlacementSpan emitClipPlacements(const ClipModel& clip,
                                     engine::ClipPlayerNode::ClipList& list);
    /// A clip's length in seconds, falling back to its source's length when the
    /// document stores no duration.
    double effectiveClipLength(const ClipModel& clip);
    /// Recompute a fader's gain from volume/mute/solo and push it to the node.
    /// Which channels a solo leaves open: the soloed tracks, plus everything
    /// downstream of them. A bus carrying a soloed track has to stay open or
    /// soloing that track would silence it through its own destination.
    struct SoloState {
        bool any = false;
        std::unordered_set<std::string> open;
    };
    SoloState soloState() const;
    void syncTrackGain(const TrackModel& track);
    void syncTrackGain(const TrackModel& track, const SoloState& solo);
    void syncAllTrackGains();
    void updateTimelineDuration();
    /// Rebuild the control-thread copy of automation curves on demand.  UI
    /// surfaces ask for the same values many times per refresh; keeping the
    /// compiled shapes here turns those reads into map lookups instead of one
    /// project scan, allocation and sort per fader.
    struct AutomationTargetHash {
        std::size_t operator()(const AutomationTarget& target) const noexcept;
    };
    void invalidateAutomationReadoutCache() noexcept;
    void rebuildAutomationReadoutCache() const;
    /// Decode a file once and keep it; clips reference the result.
    std::shared_ptr<const engine::SampleBuffer> loadSamples(const std::string& path);
    void pruneDecodedSampleCache();
    struct PendingSharedAssetMutation;
    collab::SharedMutationResult prepareSharedAssetMutation(
        collab::SharedAssetMutationRequest request,
        PendingSharedAssetMutation pending);

    engine::SamplePos toSamples(double seconds) const;
    double toSeconds(engine::SamplePos samples) const;
    /// Apply the selected start policy shared by playback and recording.
    void applyTransportStartPolicy();

    engine::RealtimeEngine m_engine;
    std::unique_ptr<audio::AudioDeviceManager> m_devices;
    /// Utility recorder — owns nothing that is being captured; it exists for
    /// `writeWAVFile`, which the offline export and the comp flatten both use.
    std::unique_ptr<audio::AudioRecorder> m_recorder;
    std::unique_ptr<DeviceCallback> m_callback;

    ProjectModel m_project;
    collab::SharedMutationSink* m_sharedMutationSink = nullptr;
    collab::SharedAssetMutationSink* m_sharedAssetMutationSink = nullptr;
    struct PendingSharedAssetMutation {
        AssetRef expected;
        std::string cleanupPath;
        std::function<collab::SharedMutationResult(
            EngineController&, const AssetRef&)> complete;
    };
    std::unordered_map<std::string, PendingSharedAssetMutation>
        m_pendingSharedAssetMutations;
    UndoStack m_undo;
    WaveformCache m_waveforms;
    PluginManager m_pluginManager;
    std::unordered_map<std::string, recovery::RecoverySnapshot::PluginState>
        m_recoveryPluginStateCache;
    std::size_t m_recoveryPluginCaptureCursor = 0;
    bool m_automationWrite = false;
    mutable bool m_automationReadoutCacheDirty = true;
    mutable std::unordered_map<AutomationTarget, engine::LevelCurve,
                               AutomationTargetHash>
        m_automationReadoutCurves;

    /// The node objects behind one channel. They outlive graph rebuilds, so a
    /// re-route keeps loaded clips, meter values and (later) plugin state
    /// instead of resetting the project's DSP on every edit.
    /// One loaded plugin behind an insert slot. `slotId` and `uid` record what
    /// it was built for, so a rebuild can tell "same plugin, still fine" from
    /// "the user swapped it" without reloading the world on every edit.
    struct InsertSlot {
        std::string slotId;
        std::string uid;
        std::shared_ptr<plugins::PluginNode> node;
        std::shared_ptr<plugins::PluginNode> rightNode;
        std::shared_ptr<engine::ChannelSelectNode> leftSelector;
        std::shared_ptr<engine::ChannelSelectNode> rightSelector;
        std::shared_ptr<engine::StereoMergeNode> stereoMerge;
        PluginChannelMode channelMode = PluginChannelMode::Auto;
        /// Handle in the graph currently being assembled. Rewritten on every
        /// rebuild and used by the deferred sidechain routing pass.
        engine::NodeId nodeId = engine::kInvalidNode;
        engine::NodeId rightNodeId = engine::kInvalidNode;
        engine::NodeId leftSelectorId = engine::kInvalidNode;
        engine::NodeId rightSelectorId = engine::kInvalidNode;
        /// Captured immediately before a VST3-requested component reload and
        /// consumed by the replacement instance. Not project state: it exists
        /// only across one graph reconciliation.
        std::vector<std::uint8_t> reloadState;
        std::vector<std::uint8_t> rightReloadState;
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
        /// Clips with their own inserts are split out of the shared player and
        /// merged back here after their private chains.
        std::unordered_map<std::string, ClipFxChannel> clipFx;
        std::shared_ptr<engine::SumNode> clipFxSum;
        /// The notes, on tracks that carry them. Feeds the instrument.
        std::shared_ptr<engine::MidiClipPlayerNode> midiClips;
        /// A list of at most one, so the same reconciliation as the inserts
        /// applies — the instrument is a plugin slot like any other, it just
        /// sits ahead of them and is the only one fed MIDI.
        std::vector<InsertSlot> instrument;
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
        TrackNodes ids;
    };

    /// Keyed by track uuid, plus `kMasterChannelId` for the master bus.
    std::unordered_map<std::string, TrackChannel> m_channels;
    std::uint64_t m_graphRebuildCount = 0;
    std::unordered_map<std::string, std::uint64_t> m_midiNotesRevisions;
    std::uint64_t m_midiNotesRevisionCounter = 0;

    /// Stem capture points, live only for the duration of a render. Held here
    /// rather than patched into the graph once because `rebuildGraph` discards
    /// the whole topology: anything that rebuilds mid-render — a plugin
    /// reporting new latency, say — would otherwise silently drop the taps and
    /// write silent stems. Keyed the same way as `m_channels`.
    std::unordered_map<std::string, std::shared_ptr<engine::TapNode>> m_renderTaps;
    /// Whether those taps hang off `preFaderTap` instead of `meter`.
    bool m_renderTapsPreFader = false;
    /// True for the duration of an offline render. `rebuildGraph` leaves the
    /// metronome out while it is set: the click is a monitoring aid, and it is
    /// gated on `context.playing`, which an offline pass asserts — so without
    /// this an enabled click lands in the exported file.
    bool m_renderingPass = false;
    /// Move the whole session to another sample rate, dropping the decoded-clip
    /// caches that were converted for the old one. Used by a render that writes
    /// at a rate the project does not run at, in both directions.
    void applyRenderSampleRate(double rate);

    // ── Insert plumbing (declared here: it needs TrackChannel above) ──
    /// Bring a channel's loaded plugins in line with its document slots,
    /// keeping instances that did not change.
    void syncChannelInserts(const std::string& channelId, TrackChannel& channel,
                            const std::vector<InsertModel>& slots);
    /// The same reconciliation against any list of slots — the insert chain has
    /// one, the instrument slot has a list of exactly one.
    void syncSlots(const std::string& channelId, std::vector<InsertSlot>& live,
                   const std::vector<InsertModel>& slots);
    /// Tell whoever is listening that these slots' plugins are about to go, and
    /// do it while they are still alive. See `setPluginRetiringCallback`.
    void announceRetiring(const std::string& channelId,
                          const std::vector<InsertSlot>& going);
    /// The same, for every loaded plugin in the project — closing or replacing
    /// the whole document takes them all at once.
    void announceAllRetiring();
    /// Wire `head` through the channel's inserts and return the last node in
    /// the chain — which is `head` itself when there are none.
    engine::NodeId connectInsertChain(engine::AudioGraph& graph,
                                      TrackChannel& channel, engine::NodeId head);
    engine::NodeId connectSlots(engine::AudioGraph& graph,
                                std::vector<InsertSlot>& live,
                                std::vector<engine::NodeId>& ids,
                                engine::NodeId head);
    TrackChannel* findChannel(const std::string& channelId);
    /// Write each loaded plugin's state chunk into the package's `State/`
    /// folder and record the filename in the document. Called from
    /// `saveProject`, because only this class holds the live instances.
    audio::Result writePluginState(ProjectModel& document,
                                   const std::string& packageDir);
    void cleanupPluginState(const ProjectModel& document,
                            const std::string& packageDir);
    /// Restore those chunks after a load, falling back to the stored parameter
    /// values when a blob will not apply.
    audio::Result loadPluginState(
        const std::string& packageDir,
        const std::unordered_set<std::string>* channelFilter = nullptr,
        bool includeMaster = true,
        const std::string& fallbackPackageDir = {},
        bool tolerateStateErrors = false);
    PluginRetiringFn m_pluginRetiring;

    std::vector<InsertModel>* mutableChannelInserts(const std::string& channelId);
    std::vector<InsertModel>* mutableSamplerFxInserts(
        const std::string& trackId, const std::string& samplerSlotId);
    std::vector<InsertModel>* mutableClipFxInserts(
        const std::string& trackId, const std::string& clipId);
    /// One slot by id, searching the instrument as well as the inserts — the
    /// same addressing `insertNode` uses, so the document and the graph agree on
    /// what a slot id means.
    InsertModel* mutableInsertSlot(const std::string& channelId,
                                   const std::string& insertId);
    InsertSlot* liveInsertSlot(const std::string& channelId,
                               const std::string& insertId);
    plugins::PluginNode* insertNode(const std::string& channelId,
                                    const std::string& insertId);
    plugins::PluginNode* editorInsertNode(const std::string& channelId,
                                          const std::string& insertId);
    bool sidechainWouldFeedback(const std::string& destinationChannelId,
                                const std::string& sourceTrackId) const;
    ChannelSnapshot m_channelClipboard;
    std::unordered_map<std::string, std::shared_ptr<const engine::SampleBuffer>> m_samples;
    /// One baked clip sample, and the two things that decide whether it is
    /// still valid: the file it came from and the precomputed settings it was
    /// rendered through. Keyed by clip id, so a clip that is deleted or given a
    /// different file drops its entry instead of leaving one behind.
    struct ClipSampleCacheEntry {
        std::string path;
        plugins::sampler::PrecomputeSettings settings;
        std::shared_ptr<const plugins::sampler::SampleData> data;
    };
    std::unordered_map<std::string, ClipSampleCacheEntry> m_clipSampleCache;
    /// Content/settings-level reuse behind the clip-id lookup above. Identical
    /// clips share one full baked buffer, while weak ownership lets stale
    /// combinations disappear once no clip or graph snapshot references them.
    struct SharedClipSampleCacheEntry {
        std::string path;
        plugins::sampler::PrecomputeSettings settings;
        std::weak_ptr<const plugins::sampler::SampleData> data;
    };
    std::vector<SharedClipSampleCacheEntry> m_sharedClipSampleCache;
    std::shared_ptr<const plugins::sampler::SampleData> processedClipSample(
        const ClipModel& clip, const std::string& path,
        std::shared_ptr<const engine::SampleBuffer> raw);

    /// Tracks whose clip list is waiting to be handed to the engine because
    /// producing it means re-rendering a sample.
    ///
    /// Baking a precomputed effect is O(length of the file), and a knob drag
    /// asks for it once per mouse move — on a long clip that is a frozen
    /// interface. The built-in sampler uses a cancellable worker, but clip
    /// bakes feed a shared cache and graph publication path and are still
    /// synchronized here. Coalescing costs at most one tick of latency.
    /// Anything
    /// that must be exact *now* — an export, an offline analysis, hitting play
    /// — flushes first.
    std::vector<std::string> m_deferredClipSync;
    void deferClipSync(const std::string& trackId);
    void flushDeferredClipSync();
    static constexpr std::uint32_t kPluginCompatibilitySweepTicks = 64;
    std::uint64_t m_pluginMainThreadGeneration =
        plugins::PluginMainThreadWork::generation();
    std::uint32_t m_pluginCompatibilitySweepTicks = 0;
    std::uint64_t m_pluginEventScanCount = 0;
    /// Wait for every built-in sampler's latest background bake. Playback and
    /// offline/export paths call this before consuming the graph so a GUI-tick
    /// race can never render the previous generation.
    void flushSamplerPrecompute();
    std::shared_ptr<engine::SumNode> m_masterSum;
    std::shared_ptr<engine::GainNode> m_masterFader;
    std::shared_ptr<engine::MetronomeNode> m_metronome;
    /// Auditions files from the browser. Always in the graph, like the
    /// metronome, so a preview survives every rebuild.
    std::shared_ptr<engine::PreviewPlayerNode> m_preview;
    std::string m_previewPath;
    double m_previewDuration = 0.0;
    engine::NodeId m_masterFaderId = engine::kInvalidNode;
    engine::NodeId m_masterSumId = engine::kInvalidNode;
    bool m_metronomeEnabled = false;
    std::string m_metronomeSamplePath;

    double m_sampleRate = 48000.0;
    uint32_t m_bufferSize = 512;
    bool m_deviceOpen = false;
    bool m_liveDeviceAllowed = false;
    bool m_prepared = false;
    PlaybackMode m_playbackMode{PlaybackMode::Resume};
    /// Where the current playback run started; Restart mode returns here.
    double m_playAnchorSeconds = 0.0;
    std::string m_recordDir;
    RecordingPrefs m_recording;

    /// One track's capture for the duration of a recording run.
    struct Capture {
        std::string trackId;
        std::shared_ptr<audio::AudioRecorder> recorder;
        std::string path;
        double startSeconds = 0.0;      ///< timeline position recording began
        bool monitorBefore = false;     ///< monitor state to restore on stop
        bool armedBefore = false;
        bool monitorAutoBefore = false; ///< exact-start rollback only
        bool monitorManaged = false;
        FrozenRecordingSemantics semantics;
        /// Input peak per bucket since the capture began, for the growing
        /// waveform the arrangement draws inside the take-to-be. A bucket is a
        /// fixed slice of *recorded* time rather than one UI frame: the frame
        /// rate wobbles (and changes outright when the refresh timer speeds up
        /// for the take), and an evenly-drawn frame-per-sample envelope slides
        /// backwards under the playhead every time it does.
        std::vector<float> envelope;
        /// Set by `seedRecordingForShot` in an offline harness, where the
        /// recorder's own clock never advances. Negative in a real take.
        double seededSeconds = -1.0;
        /// Seconds one bucket covers. Doubles each time the envelope hits its
        /// cap and is halved, so index times stay exact on a long take.
        double envelopeStepSeconds = 0.025;
    };
    std::vector<Capture> m_captures;
    std::vector<std::string> m_recordingTracks;
    /// The count-in in flight: what it will record onto, how many beats are
    /// still to be counted, and how long until the next one lands.
    std::vector<std::string> m_countInTracks;
    int m_countInBeatsLeft = 0;
    double m_countInToNextBeat = 0.0;
    bool m_countInRequiresExactTargets = false;
    /// Each count-in target's monitor state before the count-in opened it, so
    /// cancelling puts it back and the take that follows knows what "before"
    /// really was.
    std::vector<std::pair<std::string, bool>> m_countInMonitorBefore;

    /// The recorders the audio thread taps, published as an immutable snapshot
    /// exactly like a track's clip list. Starting or stopping a capture swaps
    /// the whole list, so the render callback never sees a half-built set and a
    /// recorder being retired stays alive until the block using it is done.
    using RecorderList = std::vector<std::shared_ptr<audio::AudioRecorder>>;
    engine::RealtimeSnapshot<RecorderList> m_activeRecorders;
    void publishRecorders();

    /// Open or leave closed the track's monitor per the smart-monitoring rule:
    /// only listen when no other track is already carrying the same input.
    /// Marks the track as auto-managed either way.
    void applySmartMonitoring(TrackModel& track);
    bool startRecordingTracksImpl(const std::vector<std::string>& trackIds,
                                  bool requireEveryTarget);
    bool armCountInImpl(const std::vector<std::string>& trackIds, int beats,
                        bool requireEveryTarget);
    FrozenRecordingSemantics frozenRecordingSemantics(
        const std::string& trackId) const;
    /// The passes a capture of `capturedLength` seconds, punched in at
    /// `startSeconds`, has made: one span per stretch of timeline it wrote,
    /// each carrying its own offset into the capture. Loop recording makes
    /// several, everything else exactly one. Used both to land a finished take
    /// and to draw a running one, so what is drawn is what will land.
    static std::vector<RecordingSpan> capturePasses(
        const FrozenRecordingSemantics& semantics, double startSeconds,
        double capturedLength);
    /// Delete everything on the track between two timeline times, trimming the
    /// clips that straddle the edges rather than dropping them whole.
    void clearTrackRange(TrackModel& track, double from, double to);
    /// Turn one already-finalized capture into clips or takes on its track.
    /// Every decision comes from the frozen result; current preferences are
    /// deliberately irrelevant here.
    void landCapture(TrackModel& track,
                     const FinalizedRecordingTrack& recording);

    /// Put every track's output back where its folders say it belongs, after
    /// anything that changed the tree: a track inside a summing folder feeds
    /// that folder's bus. A track routed somewhere by hand keeps that routing
    /// — only a route that came from a folder (or no route at all) is
    /// rewritten, so pulling a track out of a folder does not strand it on a
    /// bus it can no longer see.
    void syncFolderRouting();

    /// The live plugin's description of the parameter a target names, or null
    /// when the slot or the parameter is gone. Where the plain-unit range comes
    /// from, and the reason a stale target degrades quietly instead of driving
    /// the wrong knob.
    /// Push this track's volume/pan/mute curves — and its sends' — into the
    /// nodes that play them. Companion to `syncTrackAutomation`, which does the
    /// same for the plugins on the channel.
    void syncTrackLevelAutomation(const TrackModel& track);
    void syncAutomationTarget(const AutomationTarget& target);
    void syncAllLevelAutomation();

    const plugins::ParameterInfo* automationParameterInfo(
        const AutomationTarget& target) const;

    /// Point a running capture at the track's current input — a different
    /// channel, or a different width. Live: the input can be changed while the
    /// take is rolling.
    void retargetCaptureInput(const TrackModel& track);

    /// Push a fully-formed track onto the project, rebuild and make it
    /// undoable. The one place a track is born, so `addTrack` and `addFolder`
    /// cannot drift apart in what they register with the undo stack.
    std::string appendTrack(TrackModel model);

    /// Move everything musical onto the new tempo after a BPM change: clips
    /// keep the bar they start on, MIDI keeps its length in beats, audio keeps
    /// its length in seconds. `from` and `to` are the old and new tempo.
    void retimeToTempo(double from, double to);

    /// Push the document's stored parameter values into a live plugin — the
    /// fallback for anything its own state blob did not carry, and the
    /// correction for a blob captured before the last edits reached it.
    void applyStoredParameters(plugins::PluginNode& node,
                               const std::vector<InsertParameter>& values);
    /// Put `chain` on a channel, replacing whatever is there, and hand each
    /// new instance the state it was copied with. The one path a paste, an
    /// undo and a redo all go through, so the three cannot disagree.
    void applyChain(const std::string& channelId,
                    const std::vector<ChainSlotSnapshot>& chain);

    /// The clip a (track, clip) pair names, or nullptr.
    ClipModel* findClip(const std::string& trackId, const std::string& clipId);
    /// Push a clip's takes/comp back into the engine after a comp edit.
    void syncClipOwner(const std::string& trackId);
    /// Apply one discrete change to a take and push it as a single undo entry.
    template <typename Mutate>
    void editTake(const char* label, const std::string& trackId,
                  const std::string& clipId, const std::string& takeId,
                  Mutate mutate);

    /// The take soloed for auditioning, or empty. Session state.
    std::string m_soloTakeId;
    std::string m_soloClipId;

    /// A comp gesture in flight: the clip it targets and the comp it started
    /// from, so the whole stroke undoes as one edit.
    struct CompEdit {
        bool active = false;
        std::string trackId;
        std::string clipId;
        std::vector<CompSegment> before;
        std::vector<CompSegment> after;
    };
    CompEdit m_compEdit;

    struct NoteEdit {
        bool active = false;
        bool structural = false;
        bool indexBuilt = false;
        bool playbackDirty = false;
        std::string trackId;
        std::string clipId;
        // Property gestures keep just the first state of each touched note.
        // Views point into the stable note vector and are discarded before a
        // structural mutation can reallocate it.
        std::unordered_map<std::string_view, std::size_t> noteIndices;
        std::unordered_map<std::string, NoteModel> beforeById;
        // Add/remove/whole-vector transforms intentionally fall back to the
        // complete snapshot required to preserve order and membership.
        std::vector<NoteModel> structuralBefore;
    };
    NoteEdit m_noteEdit;
    std::uint64_t m_noteEditIndexBuildCount = 0;

    struct ClipPositionOrigin {
        std::string beforeTrackId;
        double beforeStartSeconds = 0.0;
        std::size_t beforeIndex = 0;
        // Kept current as the live gesture crosses lanes. End therefore scans
        // only these affected owners to resolve final vector order, never every
        // clip in the project.
        std::string afterTrackId;
        double afterStartSeconds = 0.0;
        std::size_t afterIndex = 0;
    };

    struct ClipPositionEdit {
        bool active = false;
        // While a private-FX clip is temporarily between owners, keep both
        // live graph channels untouched until the final delta is known.
        bool graphDirty = false;
        // Captured lazily on the first mutation. Notes, lanes, samples and all
        // other ClipModel payload stay in place and are never copied for drag
        // history, even for very dense MIDI clips.
        std::unordered_map<std::string, ClipPositionOrigin> origins;
    };
    ClipPositionEdit m_clipPositionEdit;

    struct ClipTrimOrigin {
        bool dirty = false;
        std::string trackId;
        std::string clipId;
        ClipKind kind = ClipKind::Audio;
        double beforeStartSeconds = 0.0;
        double beforeOffsetSeconds = 0.0;
        double beforeDurationSeconds = 0.0;
        ClipMusicalAnalysisModel beforeMusicalAnalysis;
        // Populated only for an Automation clip. MIDI notes/lanes, takes,
        // inserts and sample payloads are deliberately absent from this state.
        ClipAutomationModel beforeAutomation;
        double sourceDurationSeconds = 0.0;
        std::vector<std::string> patternMemberTrackIds;
    };
    struct ClipTrimEdit {
        bool active = false;
        std::vector<ClipTrimOrigin> origins;
    };
    ClipTrimEdit m_clipTrimEdit;
};

} // namespace daw
