#pragma once

#include "model/Document.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace daw::collab {

inline constexpr std::uint32_t kProjectCommandSchemaVersion = 1;
inline constexpr std::size_t kMaxProjectCommandPreconditions = 1024;
inline constexpr std::size_t kMaxProjectCommandTouchedFields = 8192;
inline constexpr std::size_t kMaxProjectCommandBatchSize = 1024;
inline constexpr std::size_t kMaxPluginParameterIdBytes = 400;

using ScalarValue = std::variant<std::string, double, std::int64_t, bool>;

enum class ProjectScalar : std::uint8_t {
    Name,
    Tempo,
    AiInstructions,
    MasterVolume,
    MasterPan,
};

enum class TrackProperty : std::uint8_t {
    Name,
    Color,
    Volume,
    Pan,
    Muted,
    Mono,
};

enum class ClipProperty : std::uint8_t {
    Name,
    StartSeconds,
    DurationSeconds,
    OffsetSeconds,
    Gain,
    Pan,
    Muted,
    Color,
    CompCrossfadeMs,
};

enum class SendProperty : std::uint8_t {
    DestinationTrackId,
    Level,
    PreFader,
    Enabled,
};

/// A plugin can live in one of the document-owned processing chains. The
/// location deliberately carries stable entity ids, never vector indices.
enum class PluginChain : std::uint8_t {
    Master,
    Track,
    Instrument,
    SamplerFx,
    Clip,
};

struct PluginLocation {
    PluginChain chain = PluginChain::Track;
    std::string trackId;
    std::string clipId;

    friend bool operator==(const PluginLocation&,
                           const PluginLocation&) = default;
};

enum class PluginProperty : std::uint8_t {
    Name,
    Bypassed,
    Mix,
    ChannelMode,
    SidechainTrackId,
};

struct CommandMeta {
    std::uint32_t schemaVersion = kProjectCommandSchemaVersion;
    std::string projectId;
    std::string operationId;
    std::string actorId;
    std::string clientId;
    std::uint64_t clientSequence = 0;
    std::uint64_t baseServerSequence = 0;
    std::uint64_t serverSequence = 0; // zero until confirmed by the server
    std::string transactionId;
};

struct FieldWriterIs {
    std::string fieldKey;
    std::string operationId;
};

using CommandCondition = FieldWriterIs;

struct SetProjectScalar {
    ProjectScalar field = ProjectScalar::Name;
    ScalarValue value = std::string();
};

struct SetTimeSignature {
    int numerator = 4;
    int denominator = 4;
};

struct SetProjectKey {
    int root = 0;
    std::string scale = "major";
};

struct AddTrack {
    std::string trackId;
    TrackKind kind = TrackKind::Audio;
    std::string name;
    std::uint32_t color = 0x4A90D9;
    std::string parentId;
    /// Empty means insert first; otherwise insert immediately after this id.
    std::string afterId;
};

struct DeleteTrack {
    std::string trackId;
};

/// Explicit resurrection used by conditional undo. Ordinary AddTrack is never
/// allowed to reuse a tombstoned id, which is the delete-wins rule.
struct RestoreTrack {
    std::string trackId;
    std::string deleteOperationId;
};

struct MoveTrack {
    std::string trackId;
    /// Empty means move first; otherwise move immediately after this id.
    std::string afterId;
};

struct SetTrackProperty {
    std::string trackId;
    TrackProperty property = TrackProperty::Name;
    ScalarValue value = std::string();
};

struct SetTrackParent {
    std::string trackId;
    std::string parentId;
};

struct SetTrackOutput {
    std::string trackId;
    /// Empty routes to master; otherwise a stable Bus/Aux/Group track id.
    std::string outputTrackId;
};

struct AddSend {
    std::string trackId;
    SendModel send;
    std::string afterId;
};

struct DeleteSend {
    std::string trackId;
    std::string sendId;
};

struct RestoreSend {
    std::string trackId;
    std::string sendId;
    std::string deleteOperationId;
};

struct MoveSend {
    std::string trackId;
    std::string sendId;
    std::string afterId;
};

struct SetSendProperty {
    std::string trackId;
    std::string sendId;
    SendProperty property = SendProperty::Level;
    ScalarValue value = 0.0;
};

struct AddClip {
    std::string trackId;
    std::string clipId;
    ClipKind kind = ClipKind::Audio;
    std::string name;
    double startSeconds = 0.0;
    double durationSeconds = 0.0;
    std::uint32_t color = 0x4A90D9;
    std::string afterId;
};

struct DeleteClip {
    std::string trackId;
    std::string clipId;
};

struct RestoreClip {
    std::string trackId;
    std::string clipId;
    std::string deleteOperationId;
};

/// Move a clip to `trackId` and place it immediately after `afterId`. The
/// destination may be its current track; empty afterId means first.
struct MoveClip {
    std::string clipId;
    std::string trackId;
    std::string afterId;
};

struct SetClipProperty {
    std::string trackId;
    std::string clipId;
    ClipProperty property = ClipProperty::Name;
    ScalarValue value = std::string();
};

/// Replace a cloud audio identity only after the blob was completed. An empty
/// AssetRef explicitly clears the source identity (for conditional undo).
struct SetClipAsset {
    std::string trackId;
    std::string clipId;
    AssetRef asset;
};

/// Sample-editor gestures may preview ephemerally, then commit one atomic
/// immutable value object through this command.
struct SetClipSampleEdit {
    std::string trackId;
    std::string clipId;
    ClipSampleEditModel sampleEdit;
};

struct AddPluginInsert {
    PluginLocation location;
    InsertModel insert;
    std::string afterId;
};

struct DeletePluginInsert {
    PluginLocation location;
    std::string insertId;
};

struct RestorePluginInsert {
    PluginLocation location;
    std::string insertId;
    std::string deleteOperationId;
};

struct MovePluginInsert {
    PluginLocation location;
    std::string insertId;
    std::string afterId;
};

struct SetPluginProperty {
    PluginLocation location;
    std::string insertId;
    PluginProperty property = PluginProperty::Name;
    ScalarValue value = std::string();
};

/// Atomically updates compatibility metadata, fallback parameters and all
/// immutable state/resource assets without copying machine-local paths/UI.
struct SetPluginState {
    PluginLocation location;
    std::string insertId;
    std::string pluginVersion;
    int stateSchemaVersion = 0;
    AssetRef stateAsset;
    AssetRef rightStateAsset;
    std::vector<InsertParameter> parameters;
    std::vector<InsertParameter> rightParameters;
    std::vector<PluginAssetBinding> assetBindings;
};

struct SetPluginParameter {
    PluginLocation location;
    std::string insertId;
    std::string parameterId;
    double value = 0.0;
    bool rightChannel = false;
};

struct RemovePluginParameter {
    PluginLocation location;
    std::string insertId;
    std::string parameterId;
    bool rightChannel = false;
};

struct SetPluginAssetBinding {
    PluginLocation location;
    std::string insertId;
    PluginAssetBinding binding;
};

struct RemovePluginAssetBinding {
    PluginLocation location;
    std::string insertId;
    std::string key;
};

struct UpsertMidiNote {
    std::string trackId;
    std::string clipId;
    NoteModel note;
    std::string afterId;
};

struct DeleteMidiNote {
    std::string trackId;
    std::string clipId;
    std::string noteId;
};

struct RestoreMidiNote {
    std::string trackId;
    std::string clipId;
    std::string noteId;
    std::string deleteOperationId;
};

/// `laneId` is empty for an Automation clip's primary curve. A non-empty id
/// addresses a ControllerLane inside a MIDI clip.
struct UpsertAutomationPoint {
    std::string trackId;
    std::string clipId;
    std::string laneId;
    AutomationPoint point;
    std::string afterId;
};

struct DeleteAutomationPoint {
    std::string trackId;
    std::string clipId;
    std::string laneId;
    std::string pointId;
};

struct RestoreAutomationPoint {
    std::string trackId;
    std::string clipId;
    std::string laneId;
    std::string pointId;
    std::string deleteOperationId;
};

/// The target fields carried by a MIDI clip controller lane. `cc == -1`
/// addresses a plugin parameter; otherwise it is a MIDI controller number.
struct ControllerLaneTarget {
    int cc = 1;
    std::string parameterId;
    std::string slotId;

    friend bool operator==(const ControllerLaneTarget&,
                           const ControllerLaneTarget&) = default;
};

struct AddControllerLane {
    std::string trackId;
    std::string clipId;
    std::string laneId;
    std::string name;
    ControllerLaneTarget target;
    double defaultValue = 0.0;
    std::string afterId;
};

struct DeleteControllerLane {
    std::string trackId;
    std::string clipId;
    std::string laneId;
};

struct RestoreControllerLane {
    std::string trackId;
    std::string clipId;
    std::string laneId;
    std::string deleteOperationId;
};

struct SetControllerLaneTarget {
    std::string trackId;
    std::string clipId;
    std::string laneId;
    ControllerLaneTarget target;
};

struct SetControllerLaneDefault {
    std::string trackId;
    std::string clipId;
    std::string laneId;
    double defaultValue = 0.0;
};

/// These settings belong to the primary curve of an Automation clip. A MIDI
/// ControllerLane has no `active` property in the document model.
struct SetAutomationTarget {
    std::string trackId;
    std::string clipId;
    AutomationTarget target;
};

struct SetAutomationDefault {
    std::string trackId;
    std::string clipId;
    double defaultValue = 0.0;
};

struct SetAutomationActive {
    std::string trackId;
    std::string clipId;
    bool active = false;
};

/// Collaborative take creation is intentionally cloud-native: `take.filePath`
/// and MIDI `take.notes` must be empty, while `take.asset` must be a complete
/// immutable audio AssetRef. Local paths are resolved by the cache/projector.
struct AddTake {
    std::string trackId;
    std::string clipId;
    TakeModel take;
    std::string afterId;
};

struct DeleteTake {
    std::string trackId;
    std::string clipId;
    std::string takeId;
};

struct RestoreTake {
    std::string trackId;
    std::string clipId;
    std::string takeId;
    std::string deleteOperationId;
};

struct UpsertCompSegment {
    std::string trackId;
    std::string clipId;
    CompSegment segment;
    std::string afterId;
};

struct DeleteCompSegment {
    std::string trackId;
    std::string clipId;
    std::string segmentId;
};

struct RestoreCompSegment {
    std::string trackId;
    std::string clipId;
    std::string segmentId;
    std::string deleteOperationId;
};

struct BatchCommand;

/// A recording commit proves that every document mutation targeting a recorded
/// track belongs to the exact server-issued lease. The backend consumes these
/// claims atomically with the outer operation; the pure reducer only validates
/// their canonical shape and applies the nested document transaction.
struct RecordingLeaseClaim {
    std::string trackId;
    std::string leaseId;

    friend bool operator==(const RecordingLeaseClaim&,
                           const RecordingLeaseClaim&) = default;
};

struct RecordingCommit {
    std::vector<RecordingLeaseClaim> leases;
    std::shared_ptr<BatchCommand> batch;
};

using CommandBody = std::variant<SetProjectScalar, SetTimeSignature,
                                 SetProjectKey, AddTrack, DeleteTrack,
                                 RestoreTrack, MoveTrack, SetTrackProperty,
                                 SetTrackParent, SetTrackOutput, AddSend,
                                 DeleteSend, RestoreSend, MoveSend,
                                 SetSendProperty,
                                 AddClip, DeleteClip, RestoreClip, MoveClip,
                                 SetClipProperty, SetClipAsset,
                                 SetClipSampleEdit, AddPluginInsert,
                                 DeletePluginInsert, RestorePluginInsert,
                                 MovePluginInsert, SetPluginProperty,
                                 SetPluginState, SetPluginParameter,
                                 RemovePluginParameter,
                                 SetPluginAssetBinding,
                                 RemovePluginAssetBinding, UpsertMidiNote,
                                 DeleteMidiNote, RestoreMidiNote,
                                 UpsertAutomationPoint,
                                 DeleteAutomationPoint,
                                 RestoreAutomationPoint,
                                 AddControllerLane, DeleteControllerLane,
                                 RestoreControllerLane,
                                 SetControllerLaneTarget,
                                 SetControllerLaneDefault,
                                 SetAutomationTarget, SetAutomationDefault,
                                 SetAutomationActive, AddTake, DeleteTake,
                                 RestoreTake, UpsertCompSegment,
                                 DeleteCompSegment, RestoreCompSegment,
                                 RecordingCommit,
                                 std::shared_ptr<BatchCommand>>;

struct ProjectCommand {
    CommandMeta meta;
    std::vector<CommandCondition> conditions;
    CommandBody body;
};

struct BatchCommand {
    /// Children inherit the durable outer operation id. Their preconditions are
    /// checked against the pre-batch state, then all bodies apply atomically.
    /// Nested batches are rejected so a transaction has one clear boundary.
    std::vector<ProjectCommand> commands;
};

std::string projectScalarName(ProjectScalar field);
bool projectScalarFromName(const std::string& name, ProjectScalar& out);
std::string trackPropertyName(TrackProperty property);
bool trackPropertyFromName(const std::string& name, TrackProperty& out);
std::string clipPropertyName(ClipProperty property);
bool clipPropertyFromName(const std::string& name, ClipProperty& out);
std::string sendPropertyName(SendProperty property);
bool sendPropertyFromName(const std::string& name, SendProperty& out);
std::string pluginChainName(PluginChain chain);
bool pluginChainFromName(const std::string& name, PluginChain& out);
std::string pluginPropertyName(PluginProperty property);
bool pluginPropertyFromName(const std::string& name, PluginProperty& out);
std::string commandKind(const ProjectCommand& command);
/// True for the canonical 8-4-4-4-12 hexadecimal UUID wire representation.
bool isUuid(std::string_view value) noexcept;
/// Validate all operation/entity identifiers carried by a typed command.
/// Batch child envelopes are intentionally ignored because only the outer
/// command has a durable operation id.
bool commandHasValidIds(const ProjectCommand& command,
                        std::string* error = nullptr);
/// Canonical sorted field-head keys the backend must advance for this command.
/// Batch children contribute to one union written under the outer opId.
std::vector<std::string> commandTouchedFields(const ProjectCommand& command);

} // namespace daw::collab
