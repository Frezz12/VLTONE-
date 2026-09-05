#pragma once

#include "DSP/Curve.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// The document model — the UI-facing project data, independent of the engine.
// It carries no engine handles at all: the controller compiles this model into
// a node graph, so the document stays pure data that serialises as-is.
namespace daw {

struct InsertModel;

/// Generate a random RFC-4122-ish UUID string (8-4-4-4-12 hex).
std::string newUuid();

/// Content-addressed project data. `assetId` is the server/catalog identity;
/// `sha256` pins the bytes independently of a filename or a particular cache.
/// Legacy local projects may keep both fields empty and continue to use their
/// `filePath`/`stateFile` compatibility fields.
enum class AssetKind : uint8_t {
    Unknown = 0,
    Audio = 1,
    PluginState = 2,
    PluginResource = 3,
    Freeze = 4,
};

std::string toString(AssetKind kind);
AssetKind assetKindFromString(const std::string& name);

struct AssetRef {
    std::string assetId;
    std::string sha256;
    AssetKind kind = AssetKind::Unknown;
    std::uint64_t byteSize = 0;
    std::string originalName;
    std::string mimeType;
    std::string codec;
    double sampleRate = 0.0;
    std::uint32_t channels = 0;
    std::uint64_t frames = 0;

    bool empty() const noexcept { return assetId.empty() && sha256.empty(); }
    friend bool operator==(const AssetRef&, const AssetRef&) = default;
};

/// A non-state file a plugin needs (a Sampler source, impulse response, etc.).
/// The binding key is plugin-defined but stable within its state schema.
struct PluginAssetBinding {
    std::string key;
    AssetRef asset;
    bool required = true;

    friend bool operator==(const PluginAssetBinding&,
                           const PluginAssetBinding&) = default;
};

enum class TrackKind {
    Audio,
    Instrument,
    Midi,
    /// A compact musical container. Its children are ordinary instrument/MIDI
    /// tracks with their own mixer channels; the pattern itself is their bus
    /// and collapses them into one arrangement lane.
    Pattern,
    /// A lane that holds automation clips and nothing else. Usually a child of
    /// the track it drives — expanding that track reveals its automation lanes,
    /// which is `expanded` and `visibleTracks()` doing the work already — but a
    /// parentless one is a free-standing automation track that can drive
    /// anything in the project.
    ///
    /// It carries no signal: no fader, no pan, no inserts, no mixer strip. See
    /// `carriesAudio` below.
    Automation,
    Bus,
    Aux,
    Group,
    Master,
    Folder
};

std::string toString(TrackKind kind);
TrackKind trackKindFromString(const std::string& s);

/// What a clip carries. Stored explicitly rather than inferred: "has notes"
/// misreads an empty MIDI clip, and "has no file" misreads an audio clip whose
/// media went missing.
enum class ClipKind {
    Audio,
    Midi,
    /// An arrangement-level container on a Pattern track. It carries no audio
    /// itself: child MIDI clips point back to it through `patternClipId`, so the
    /// whole musical phrase can be cut, moved, copied and removed as one clip.
    Pattern,
    /// A curve, on an Automation track. What it drives is stored on the *clip*
    /// (`ClipModel::automation`), not on the lane, so one lane can carry several
    /// clips driving different things and a copied clip can be re-pointed at
    /// another parameter without its shape being redrawn.
    Automation,
};

std::string toString(ClipKind kind);
ClipKind clipKindFromString(const std::string& s);

/// The processing performed across a user-created clip fade. A regular fade
/// changes level only; Tape also ramps playback speed so the head winds up or
/// the tail winds down like a stopped reel.
enum class ClipFadeMode : uint8_t { Gain, Tape };

/// What happens when a recording lands on top of existing material.
///   Overwrite — the new material replaces what was there; one clip, one layer.
///   Layers    — the new material is kept as another take inside the clip, and
///               nothing is lost; the clip becomes a comp container.
enum class RecordMode { Overwrite, Layers };

std::string toString(RecordMode mode);
RecordMode recordModeFromString(const std::string& s);

/// A track's own choice, which may defer to the application-wide default.
enum class TrackRecordMode { UseGlobal, Overwrite, Layers };

std::string toString(TrackRecordMode mode);
TrackRecordMode trackRecordModeFromString(const std::string& s);

/// One note inside a MIDI clip.
///
/// Timed in beats *relative to the clip's start*, so the music follows the
/// tempo, while the clip itself stays in seconds like every other clip and all
/// the existing arrangement maths keeps working untouched.
///
/// The uuid is what the piano roll holds on to. Indices would not survive an
/// undo restoring the whole `notes` vector by value, nor the reallocation an
/// insert causes.
struct NoteModel {
    std::string id;
    int pitch = 60;               // 0 … 127, 60 = middle C (labelled C5)
    double startBeats = 0.0;      // from the clip's start
    double lengthBeats = 1.0;
    int velocity = 100;           // 1 … 127
    /// Silenced but kept in place — the Mute tool's product. A muted note is
    /// still edited, moved and quantised like any other; only playback and its
    /// drawn fill differ, which is why this is a flag and not a deletion.
    bool muted = false;
    /// Per-note colour override, 0xRRGGBB. Zero means "inherit" — the note is
    /// painted from the clip's colour, or from whatever colouring mode the
    /// piano roll is in. Zero rather than an optional so the JSON stays flat and
    /// an old project loads as "inherit" for free.
    uint32_t color = 0;
    /// Where this note sits in the stereo field, −1 … 1. Per note rather than
    /// per track: a hi-hat pattern wants its strokes spread, and the piano
    /// roll's parameter lane edits this exactly like velocity.
    float pan = 0.0f;

    friend bool operator==(const NoteModel&, const NoteModel&) = default;
};

/// How a curve travels from one breakpoint to the next.
enum class AutomationSegment : uint8_t {
    /// A straight line, bent by the point's `curve`.
    Linear = 0,
    /// The value is held until the next breakpoint and then jumps. What a
    /// switch, a mute or a stepped parameter wants — a ramp between two
    /// settings of a two-state control is a ramp through a value it does not
    /// have.
    Hold = 1,
    /// Eased at both ends rather than at one. `curve` biases which end gets
    /// more of the easing.
    SCurve = 2,
};

std::string toString(AutomationSegment segment);
AutomationSegment automationSegmentFromString(const std::string& s);

/// The engine's own spelling of the same three shapes. The values are paired
/// one-to-one and asserted where the two meet, so a cast is safe and the engine
/// keeps not depending on the document model.
engine::curve::Shape toCurveShape(AutomationSegment segment);

/// One breakpoint of a controller curve. `value` is normalised 0 … 1; what that
/// means is the lane's business (a CC is 0–127 of it, a plugin knob is its own
/// range), which keeps the curve editor from having to know about either.
struct AutomationPoint {
    double beats = 0.0;    // from the clip's start, like a note
    double value = 0.0;
    /// The segment running from this point to the *next* one — so the last
    /// point's shape is never read, and inserting a point never changes the
    /// shape of the run before it.
    AutomationSegment shape = AutomationSegment::Linear;
    /// How far that segment is bent: −1 convex … 0 straight … +1 concave. The
    /// same scale and the same sign convention as `ClipModel::fadeInCurve`, so
    /// the one curve-shaping gesture in the application means one thing.
    double curve = 0.0;

    /// Stable collaboration identity. Kept last so existing aggregate
    /// initializers remain source-compatible; v5 files receive deterministic
    /// migration ids when they are loaded.
    std::string id;

    /// Exact comparison on purpose: this answers "did the gesture change
    /// anything", where the two sides are copies of the same doubles, not the
    /// results of two different calculations.
    friend bool operator==(const AutomationPoint&, const AutomationPoint&) = default;
};

/// Where a curve is sent.
enum class AutomationTargetKind : uint8_t {
    TrackVolume = 0,
    TrackPan = 1,
    TrackMute = 2,
    SendLevel = 3,
    PluginParameter = 4,
};

std::string toString(AutomationTargetKind kind);
AutomationTargetKind automationTargetKindFromString(const std::string& s);

/// What one automation clip drives.
///
/// Held by the clip rather than by the lane it sits on: that is what lets one
/// lane carry curves for several different things, and what lets a duplicated
/// clip be re-pointed at another parameter while keeping the shape that was
/// drawn on it.
struct AutomationTarget {
    AutomationTargetKind kind = AutomationTargetKind::TrackVolume;
    /// The track being driven — *not* the lane the clip sits on. A lane under
    /// one track can perfectly well drive another, and a free-standing
    /// automation track always does.
    std::string channelId;
    /// Plugin parameters: the insert's slot id, or **empty for the track's
    /// instrument**. Empty rather than the instrument's real uuid because that
    /// is the spelling `ControllerLane::slotId` already uses and the one
    /// `syncTrackAutomation` and `writeAutomationPoint` already reconcile.
    std::string slotId;
    std::string parameterId;   // plugin parameter, format-native stable id
    std::string sendId;        // SendLevel only

    friend bool operator==(const AutomationTarget&, const AutomationTarget&) = default;
};

/// The curve an automation clip carries.
///
/// A nested object like `ClipSampleEditModel`, absent on every other kind of
/// clip and written to disk only when it has something to say.
struct ClipAutomationModel {
    AutomationTarget target;
    /// The value held before the first breakpoint, and the whole curve while
    /// there are none — normalised, like every point.
    double defaultValue = 0.0;
    /// A fresh curve follows its target without driving it. The first real
    /// point edit activates playback; undoing that edit makes it passive again.
    bool active = false;
    std::vector<AutomationPoint> points;   // kept sorted by `beats`
};

/// A controller lane inside a MIDI clip: mod wheel, expression, pitch bend, or
/// — once plugin hosting lands — a parameter of the track's instrument.
///
/// Lanes live on the clip rather than on the track because they are part of the
/// phrase: copying a clip has to copy its modulation with it, and two clips on
/// one track routinely want different shapes.
struct ControllerLane {
    std::string id;
    std::string name;          // "Mod Wheel", "Cutoff"
    /// MIDI controller number, or −1 for a lane that addresses a plugin
    /// parameter by `parameterId` instead.
    int cc = 1;
    std::string parameterId;   // empty unless this drives a plugin parameter
    /// Which plugin the parameter belongs to: an insert's slot id, or empty for
    /// the track's instrument. Empty is the default because automating the
    /// instrument is the common case and it keeps old projects loading as what
    /// they meant.
    std::string slotId;
    /// Value when the curve has no points yet, and the level held before the
    /// first breakpoint.
    double defaultValue = 0.0;
    std::vector<AutomationPoint> points;   // kept sorted by `beats`
};

/// One recorded attempt stored inside a clip — a "take" or "layer".
///
/// A take is a *view* of a file, not a file: loop recording captures one
/// continuous WAV and gives each pass its own take pointing at the same path
/// with a different `offsetSeconds`. Nothing about a take is timeline-relative;
/// it describes source material, and the comp map below decides when it plays.
struct TakeModel {
    std::string id;
    std::string name;              // "Take 1"
    std::string filePath;          // absolute, or a bare name resolved in Content/
    double offsetSeconds = 0.0;    // where in the file this take begins
    double lengthSeconds = 0.0;    // 0 = to the end of the file
    /// Where this take sits against the clip's start, in seconds. Non-zero when
    /// a punch-in take covers only part of the clip and "trim to recorded
    /// region" is on, so the take is not padded with silence.
    double clipOffsetSeconds = 0.0;
    float gain = 1.0f;
    bool muted = false;
    int channels = 0;
    uint32_t color = 0x4A90D9;
    std::vector<NoteModel> notes;  // MIDI takes
    AssetRef asset;               // v6 cloud identity; filePath is legacy/cache
};

/// One stretch of the finished comp: play `takeId` from `startSeconds` to
/// `endSeconds`, both measured from the clip's start.
///
/// Segments are kept sorted and non-overlapping (see `normalizeComp`), so at any
/// instant exactly one take is active — which is what makes the comp lane a
/// single unambiguous waveform.
struct CompSegment {
    std::string takeId;
    double startSeconds = 0.0;
    double endSeconds = 0.0;
    /// Stable collaboration identity, assigned deterministically to v5 data.
    std::string id;
};

/// Time-stretch priority for one audio clip. These are strategies, not UI
/// presets: the realtime player selects different grain/time-domain geometry
/// for transient, looping, monophonic and dense material.
enum class ClipStretchMode : uint8_t {
    Resample = 0,
    Drums = 1,
    Loop = 2,
    Vocal = 3,
    Complex = 4,
};

/// Destructive-looking sample operations kept non-destructively on one clip.
/// Two ClipModels may point at the same WAV and still render differently.
/// Start/end and fades deliberately remain in ClipModel itself: those values
/// already drive timeline trim geometry, so keeping a second copy here would
/// let the editor and arrangement disagree.
struct ClipSampleEditModel {
    int loopMode = 0;              ///< 0 off, 1 forward, 2 ping-pong
    double loopStart = 0.0;        ///< normalized inside the trimmed source
    double loopEnd = 1.0;
    ClipStretchMode stretchMode = ClipStretchMode::Resample;
    double stretchTime = 1.0;      ///< output duration multiplier
    double stretchPitch = 0.0;     ///< semitones, independent of duration
    double formant = 0.0;          ///< spectral-envelope tilt, semitones
    int rootNote = 60;              ///< audition keyboard reference (C5)

    double boost = 0.0;
    double eqLow = 0.0;
    double eqMid = 0.0;
    double eqHigh = 0.0;
    double ringMix = 0.0;
    double ringFreq = 0.5;
    double cut = 1.0;
    double res = 0.0;
    int reverbType = 0;
    double reverb = 0.0;
    double stereoDelay = 0.0;
    double pogo = 0.0;
    bool removeDc = false;
    bool reversePolarity = false;
    bool normalize = false;
    bool fadeStereo = false;
    bool reverse = false;
    bool swapStereo = false;
};

enum class MusicalAnalysisStatus : uint8_t {
    Unavailable = 0,
    Ambiguous = 1,
    Available = 2,
};

/// Musical facts measured from this particular clip range. They belong to the
/// clip rather than the source file: two trims of one WAV can contain different
/// sections and a pitch/stretch edit changes the answer of only one instance.
struct ClipTempoAnalysisModel {
    MusicalAnalysisStatus status = MusicalAnalysisStatus::Unavailable;
    double bpm = 0.0;
    double confidence = 0.0;
    double stability = 0.0;
    std::vector<double> alternatives;
    bool variable = false;
};

struct ClipKeyAnalysisModel {
    MusicalAnalysisStatus status = MusicalAnalysisStatus::Unavailable;
    int root = -1;
    std::string scale;
    double confidence = 0.0;
    int alternateRoot = -1;
    std::string alternateScale;
    double tuningCents = 0.0;
};

struct ClipMusicalAnalysisModel {
    /// Zero means no analysis. Positive versions let a newer detector retain
    /// old results while offering an explicit re-analysis instead of silently
    /// pretending they were produced by the current algorithm.
    int algorithmVersion = 0;
    ClipTempoAnalysisModel tempo;
    ClipKeyAnalysisModel key;
    double analyzedOffsetSeconds = 0.0;
    double analyzedDurationSeconds = 0.0;

    bool empty() const noexcept { return algorithmVersion <= 0; }
};

/// A rendered clip may enter the graph after processing which is already
/// printed into its file. The anchor is the original channel whose remaining
/// downstream controls stay live; an empty/missing anchor falls back to the
/// clip's owning audio track.
enum class PlaybackInjectionStage : uint8_t {
    None = 0,
    TrackSource,
    BeforeTrackFader,
    BeforeFolderFader,
    BeforeMasterFx,
    BeforeMasterFader,
};

std::string toString(PlaybackInjectionStage stage);
PlaybackInjectionStage playbackInjectionStageFromString(const std::string& name);

struct PlaybackInjection {
    PlaybackInjectionStage stage = PlaybackInjectionStage::None;
    std::string anchorChannelId;

    bool active() const noexcept {
        return stage != PlaybackInjectionStage::None;
    }
};

/// Non-destructive direct/offline processing. `renderedFilePath` is a cache;
/// the clip's ordinary source/takes remain authoritative and are used whenever
/// `sourceFingerprint` no longer matches them.
struct OfflineProcessModel {
    std::vector<InsertModel> chain;
    std::string renderedFilePath;
    AssetRef renderedAsset;
    std::string sourceFingerprint;
    double sourceDurationSeconds = 0.0;
    double renderedDurationSeconds = 0.0;
    bool includeTail = false;
    double tailSilenceDb = -96.0;
    double tailHoldSeconds = 0.3;
    double tailMaxSeconds = 30.0;

    bool empty() const noexcept {
        return chain.empty() && renderedFilePath.empty();
    }
};

struct ClipModel {
    std::string id;
    std::string name;
    std::string filePath;         // absolute, or a bare name resolved in Content/
    double startSeconds = 0.0;
    double durationSeconds = 0.0;
    double offsetSeconds = 0.0;
    double fadeInSeconds = 0.0;    // user fade at the clip's head
    double fadeOutSeconds = 0.0;   // user fade at the clip's tail
    double fadeInCurve = 0.0;      // -1 convex … 0 linear … +1 concave
    double fadeOutCurve = 0.0;
    ClipFadeMode fadeInMode = ClipFadeMode::Gain;
    ClipFadeMode fadeOutMode = ClipFadeMode::Gain;
    float gain = 1.0f;
    float pan = 0.0f;
    bool muted = false;
    int channels = 0;              // source channel count (mono/stereo dots)
    uint32_t color = 0x4A90D9;
    ClipKind kind = ClipKind::Audio;
    /// The Pattern clip this child MIDI clip belongs to. Empty for ordinary
    /// MIDI/audio clips and for the Pattern clip itself. Keeping the relation on
    /// the child makes independent Pattern instances possible on the same set of
    /// source tracks without hiding ownership in timeline geometry.
    std::string patternClipId;
    std::vector<NoteModel> notes;  // MIDI clips only
    /// Controller/automation curves drawn under the notes. MIDI clips only.
    std::vector<ControllerLane> lanes;

    /// Every attempt recorded into this clip. Empty means a plain single-layer
    /// clip that plays `filePath`/`notes` directly — the shape every clip had
    /// before layer recording, and still the common case.
    ///
    /// Once this is non-empty the clip is a *container*: `comp` decides what
    /// plays, and `filePath`/`notes` are no longer read for playback.
    std::vector<TakeModel> takes;
    /// The assembled result. Empty while `takes` is empty.
    std::vector<CompSegment> comp;
    /// Equal-power crossfade used at comp seams. This belongs to the clip so a
    /// frozen recording sounds the same for every collaborator; recording
    /// preferences are only the default for the next capture.
    double compCrossfadeMs = 5.0;
    /// The curve, on an Automation clip. Meaningless on every other kind.
    ClipAutomationModel automation;
    /// Per-instance Sample/Clip Editor state. Optional on disk for backward
    /// compatibility; its defaults reproduce the pre-v4 playback path.
    ClipSampleEditModel sampleEdit;
    ClipMusicalAnalysisModel musicalAnalysis;
    /// Plugin effects owned by this clip alone. They are routed after this
    /// clip's player and before it joins the track, so neighbouring clips,
    /// monitored input and audio routed into the track bypass them.
    std::vector<InsertModel> inserts;
    /// Cached direct-offline processing, applied before the ordinary realtime
    /// clip inserts above.
    OfflineProcessModel offlineProcess;
    /// Semantic graph entry for Bounce in Place audio. Ordinary clips leave it
    /// at None and follow the standard track path.
    PlaybackInjection playbackInjection;
    /// Whether the comp editor is open on this clip in the arrangement.
    bool expanded = false;
    AssetRef asset;               // v6 cloud identity; filePath is legacy/cache
};

/// An aux send from a track to a bus/aux track. Rendered by the engine as a
/// SendNode tapped before or after the fader.
struct SendModel {
    std::string id;
    std::string destinationTrackId;   // bus/aux track uuid
    float level = 0.5f;               // 0 … 1
    bool preFader = false;
    bool enabled = true;
};

/// Which plugin API a slot refers to.
///
/// Deliberately a separate enum from `plugins::Format` rather than an include:
/// this header is the pure-data layer and must not depend on which formats a
/// given build compiled in. `controller/plugins/PluginConvert.hpp` maps the two
/// and static_asserts every pairing so they cannot drift apart in silence.
/// `Internal` is the build's own instruments and effects — the sampler — which
/// are hosted through the same slot machinery as a third-party plugin.
enum class PluginFormat : uint8_t {
    None = 0,
    Clap = 1,
    Vst3 = 2,
    AudioUnit = 3,
    Internal = 4,
    Vst = 5
};

std::string toString(PluginFormat format);
PluginFormat pluginFormatFromString(const std::string& name);

/// Host-side channel topology for one plugin slot. `Auto` follows the owning
/// track, while the explicit modes are chosen from the plugin wrapper.
enum class PluginChannelMode : uint8_t {
    Auto = 0,
    Mono = 1,
    Stereo = 2,
    DualMono = 3
};

std::string toString(PluginChannelMode mode);
PluginChannelMode pluginChannelModeFromString(const std::string& name);

/// Which half of a dual-mono slot is shown by the editor wrapper.
enum class PluginEditorChannel : uint8_t { Left = 0, Right = 1 };

std::string toString(PluginEditorChannel channel);
PluginEditorChannel pluginEditorChannelFromString(const std::string& name);

/// One parameter's value at save time, in the plugin's own plain units.
struct InsertParameter {
    std::string id;      ///< format-native, stable across versions
    double value = 0.0;
};

/// An insert (plugin) slot on a channel. `format == None` means a free slot —
/// which is exactly the shape every project written before plugin hosting has,
/// so old files load with no migration step.
struct InsertModel {
    /// Stable across a plugin swap in the same slot, which is what lets the
    /// stored state, the automation lanes and an open editor window keep
    /// pointing at the right thing.
    std::string id;
    std::string name;              ///< display name; survives even a free slot
    bool bypassed = false;

    PluginFormat format = PluginFormat::None;
    std::string uid;               ///< resolved before `path`, so a moved plugin still loads
    std::string path;              ///< last known module location
    std::string vendor;

    /// Basename of the opaque state chunk inside the package's `State/` folder.
    /// Empty until the slot has been saved once. Same "store the basename,
    /// resolve against packageDir" rule the clip media follows.
    std::string stateFile;
    /// Independent state for the right instance in Dual Mono mode.
    std::string rightStateFile;

    /// A fallback for when the state blob will not apply — the plugin was
    /// upgraded, or the project moved to a machine with a different build.
    std::vector<InsertParameter> parameters;
    std::vector<InsertParameter> rightParameters;

    /// Host-provided dry/wet, 0…1. Every plugin gets one for free.
    float mix = 1.0f;

    /// Host wrapper state. The sidechain source is a track id; an empty id is
    /// the explicit "Off" value. Missing sources remain silent on load.
    PluginChannelMode channelMode = PluginChannelMode::Auto;
    PluginEditorChannel editorChannel = PluginEditorChannel::Left;
    std::string sidechainTrackId;

    /// Editor placement, so reopening a project puts the window back.
    int windowX = 0, windowY = 0, windowWidth = 0, windowHeight = 0;
    bool windowOpen = false;

    /// Exact product/state compatibility. `path` remains a per-machine lookup
    /// hint; these values are the durable shared requirements.
    std::string pluginVersion;
    int stateSchemaVersion = 0;
    AssetRef stateAsset;
    AssetRef rightStateAsset;
    std::vector<PluginAssetBinding> assetBindings;

    /// True when this slot actually refers to a plugin.
    bool isLoaded() const { return format != PluginFormat::None && !uid.empty(); }
};

/// The processing lane owned by the built-in sampler in an instrument slot.
///
/// It deliberately does not reuse `TrackModel::inserts`: these plugins sit
/// immediately after the sampler and before routed audio joins the channel, so
/// they affect the sample alone. `ownerInstrumentId` ties the lane to the
/// concrete sampler slot and prevents a chain from silently following a synth
/// that replaces it.
struct SamplerFxModel {
    std::string ownerInstrumentId;
    float volume = 1.0f;           ///< post-FX gain, 0 … 2
    float pan = 0.0f;              ///< post-FX stereo position, −1 … 1
    std::vector<InsertModel> inserts;

    bool isOwnedBy(const InsertModel& instrument) const {
        return instrument.uid == "daw.sampler" && !ownerInstrumentId.empty() &&
               ownerInstrumentId == instrument.id;
    }
};

struct TrackModel {
    std::string id;
    TrackKind kind = TrackKind::Audio;
    std::string name;
    uint32_t color = 0x4A90D9;
    float volume = 1.0f;
    float pan = 0.0f;
    bool muted = false;
    bool soloed = false;
    bool armed = false;
    bool monitor = false;
    /// This track's override of the global recording mode.
    TrackRecordMode recordMode = TrackRecordMode::UseGlobal;
    /// True while `monitor` is being driven by smart monitoring rather than by
    /// the user. Session state, not persisted: a manual click hands control
    /// back to the user, and a reload starts fresh.
    bool monitorAuto = false;
    bool mono = false;                // true = fold to mono; false = keep stereo
    double height = 72.0;             // lane height in px (resizable)
    bool expanded = true;             // folders
    /// Automation disclosure is independent from folder disclosure. A Pattern
    /// or summing folder can keep its musical children open while its curve
    /// lanes are hidden, and toggling automation never collapses real tracks.
    bool automationExpanded = false;
    /// Folder tracks only: this folder owns a bus, and everything filed inside
    /// it is routed through that bus rather than straight to the master. A
    /// plain folder (false) is a container and nothing else — no fader, no
    /// pan, no inserts, no signal of its own. See `carriesAudio` below, which
    /// is the single answer to "does this track have a channel at all".
    bool summing = false;
    std::string parentId;             // folder parent uuid; empty = root
    uint32_t inputChannel = 0;
    /// How many hardware channels this track captures and monitors, starting at
    /// `inputChannel`: 1 for a mono source, 2 for a stereo pair.
    ///
    /// One by default, and that is the important half. Recording always took a
    /// *pair* from the chosen input — a mono microphone on input 1 produced a
    /// stereo file with a silent right channel, which sounded fine only for as
    /// long as the track's mono fold was hiding it, and collapsed into one ear
    /// the moment the track was switched to stereo for a stereo effect.
    uint32_t inputChannelCount = 1;
    std::string outputBusId;          // routing target track uuid; empty = master
    bool inputEnabled = false;        // false = "No Input"
    /// What plays this track's notes — a sampler or a synth at the head of the
    /// chain, before the inserts. MIDI/instrument tracks only, and a placeholder
    /// until plugin hosting lands: an empty `name` means "no instrument", and
    /// nothing in the engine reads it yet. Persisted so a project keeps the
    /// choice across the change.
    InsertModel instrument;
    /// A sampler-only post-instrument lane. It is silent/inactive unless the
    /// current instrument is the owning built-in sampler.
    SamplerFxModel samplerFx;
    std::vector<SendModel> sends;
    std::vector<InsertModel> inserts;
    std::vector<ClipModel> clips;
};

/// A track together with how deep it sits in the folder hierarchy.
struct TrackRow {
    size_t index = 0;     // into ProjectModel::tracks
    int depth = 0;        // 0 = root
};

struct ProjectModel {
    std::string name = "Untitled";
    std::string author;
    /// Project artwork. A saved package owns its copy in Content/; an unsaved
    /// project may still point at the image the user selected.
    std::string coverImagePath;
    double tempo = 120.0;
    int timeSigNumerator = 4;
    int timeSigDenominator = 4;
    /// The key the project is in: a pitch class (0 = C) and a scale id from
    /// `miditools::scaleId`. Nothing is forced to obey it — it is what the
    /// assistant uses to keep what it writes in key.
    int keyRoot = 0;
    std::string scale = "major";
    /// Standing instructions for the AI assistant, in the user's own words —
    /// "always sidechain the bass", "keep it under 100 BPM". Part of the
    /// project rather than the application settings, because they describe
    /// *this* music and should travel with it.
    std::string aiInstructions;
    /// The cycle region, in seconds, and whether the playhead is going round
    /// it. Part of the document because it is part of the arrangement: a
    /// reopened project should still be looping the eight bars that were being
    /// worked on, not start from a blank ruler.
    double loopStartSeconds = 0.0;
    double loopEndSeconds = 0.0;
    bool loopEnabled = false;
    double sampleRate = 48000.0;
    float masterVolume = 1.0f;
    float masterPan = 0.0f;
    std::vector<InsertModel> masterInserts;
    std::vector<TrackModel> tracks;

    TrackModel* findTrack(const std::string& id);
    const TrackModel* findTrack(const std::string& id) const;

    /// Index of a track in `tracks`, or npos.
    size_t indexOf(const std::string& id) const;

private:
    void rebuildTrackIndex() const;
    mutable std::unordered_map<std::string, size_t> m_trackIndex;
    mutable std::vector<TrackRow> m_visibleRows;
    mutable std::uint64_t m_visibleRowsSignature = 0;
    friend const std::vector<TrackRow>& visibleTracks(const ProjectModel& project);
};

// ── Takes and comping ──────────────────────────────────────────────────────
//
// All times here are seconds measured from the clip's own start, never timeline
// time, so a comp survives the clip being moved or dragged to another lane.

/// True when the clip holds takes, i.e. the comp map drives playback.
bool isLayered(const ClipModel& clip);

TakeModel* findTake(ClipModel& clip, const std::string& takeId);
const TakeModel* findTake(const ClipModel& clip, const std::string& takeId);

/// Sort the comp map, drop empty and unknown-take segments, clamp it to
/// [0, clip length] and merge neighbours that name the same take. Every comp
/// edit ends with this, so the rest of the code can assume a clean map.
void normalizeComp(ClipModel& clip);

/// The take playing at `seconds` from the clip's start, or an empty string.
std::string activeTakeAt(const ClipModel& clip, double seconds);

/// Make `takeId` the active take over [fromSeconds, toSeconds) — one swipe of
/// the comp brush. Existing segments are trimmed or split around the new one.
void setCompRange(ClipModel& clip, const std::string& takeId, double fromSeconds,
                  double toSeconds);

/// Make `takeId` the only active take for the clip's whole length.
void selectWholeTake(ClipModel& clip, const std::string& takeId);

/// Turn a plain clip into a one-take container, so the first recorded layer
/// lands beside the material that was already there instead of replacing it.
/// Does nothing to a clip that already has takes, or to an empty MIDI clip with
/// no source at all.
void promoteToTake(ClipModel& clip);

// ── The level taper ────────────────────────────────────────────────────────
//
// One definition of "how far up is this level", shared by the channel fader and
// by a volume automation curve. They have to agree: a curve drawn level with
// the fader's cap must be the level the fader is showing, and two tapers would
// make that true only by coincidence.

/// Normalised fader travel 0 … 1 → linear gain. −60 dB at the bottom, +6 dB at
/// the top, with unity at about 80 % of the throw.
double gainFromNormalized(double position);
/// The inverse. A gain of zero answers zero rather than −∞.
double normalizedFromGain(double gain);

/// Put a curve in the shape every reader is entitled to assume: points sorted
/// by time, one value per instant, and every value inside 0…1.
///
/// Two points at the same beat would make "the value here" ambiguous, and each
/// reader would need a tie-breaking rule of its own. Every path that produces
/// points — an edit, a generator, a load — ends here.
void normalizeAutomation(std::vector<AutomationPoint>& points);

/// The value of a curve at `beats`, honouring each segment's shape and bend.
/// `fallback` is held before the first point, which is what a clip's
/// `defaultValue` is for. The one answer the engine, the compiler and the
/// arrangement's drawing all use.
double automationValueAt(const std::vector<AutomationPoint>& points, double beats,
                         double fallback);

/// The colour take `index` of a stack wears: the track's colour nudged in
/// brightness, so a pile of layers reads as a family rather than a wall of one
/// flat colour. Shared with the arrangement, which paints the take *being*
/// recorded in the colour it will have once it lands.
uint32_t takeColor(uint32_t base, size_t index);

/// Whether a lane of `kind` can hold a clip of `clipKind`. The single place
/// that decides which clips belong on which track, so drag-and-drop, the
/// context menus and the controller all agree.
bool trackAccepts(TrackKind kind, ClipKind clipKind);

/// Whether a live recording can land on this track. Bus-style channels
/// (including Aux, surfaced in the UI as Send) process routed signal but do
/// not own clips or hardware inputs.
inline bool acceptsRecording(const TrackModel& track) {
    return trackAccepts(track.kind, ClipKind::Audio) ||
           trackAccepts(track.kind, ClipKind::Midi);
}

/// True for any folder, summing or not — i.e. a track that exists to hold
/// other tracks rather than to play anything of its own.
inline bool isFolder(const TrackModel& track) {
    return track.kind == TrackKind::Folder || track.kind == TrackKind::Pattern;
}

/// True when the folder owns a bus that its children sum into.
inline bool isSummingFolder(const TrackModel& track) {
    return (track.kind == TrackKind::Folder && track.summing) ||
           track.kind == TrackKind::Pattern;
}

/// Does this track have a channel — a fader, a pan, inserts, a place in the
/// mixer? Everything but the master (which has its own) and a plain folder
/// (which is a container and carries no signal). The engine builds a strip for
/// exactly these tracks, and the UI shows controls for exactly these tracks,
/// which is why both go through this one predicate.
inline bool carriesAudio(const TrackModel& track) {
    return track.kind != TrackKind::Master &&
           track.kind != TrackKind::Automation &&
           (track.kind != TrackKind::Folder || track.summing);
}

/// A lane that holds automation clips. Never has a channel; see `carriesAudio`.
inline bool isAutomationLane(const TrackModel& track) {
    return track.kind == TrackKind::Automation;
}

/// The nearest ancestor folder that sums, or an empty string. This is where a
/// track's output belongs unless the user has routed it somewhere by hand.
std::string summingParent(const ProjectModel& project,
                          const std::string& trackId);

/// Musical time ↔ wall time. `tempo` is beats per minute; a non-positive tempo
/// is treated as 1 so a corrupt project can't divide by zero.
double beatsToSeconds(double beats, double tempo);
double secondsToBeats(double seconds, double tempo);

/// Nesting depth of a track (0 when it has no folder parent).
int trackDepth(const ProjectModel& project, const std::string& trackId);

/// Tracks in display order, skipping anything inside a collapsed folder. Both
/// the header column and the timeline lanes iterate this, so they always agree
/// on which lane is which.
const std::vector<TrackRow>& visibleTracks(const ProjectModel& project);

/// True when `candidateId` is `folderId` itself or lives somewhere beneath it.
/// Used to stop a folder being dropped into its own subtree.
bool isDescendantOf(const ProjectModel& project, const std::string& candidateId,
                    const std::string& folderId);

/// A folder's direct and indirect children, in document order.
std::vector<std::string> subtreeOf(const ProjectModel& project,
                                   const std::string& folderId);

} // namespace daw
