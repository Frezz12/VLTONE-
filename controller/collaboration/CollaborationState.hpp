#pragma once

#include "collaboration/ProjectCommand.hpp"
#include "model/Document.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace daw::collab {

/// Deterministic UUID-shaped id used only when migrating entities that predate
/// collaboration ids. New commands still create random UUIDs at their origin.
std::string deterministicMigrationId(std::string_view domain,
                                     std::string_view seed);

/// Assign stable ids to v5 AutomationPoint and CompSegment data. Existing ids
/// survive unchanged unless a malformed document contains a duplicate.
void ensureStableCollaborationIds(ProjectModel& project);

/// The shared materialized document plus reducer metadata. Tombstones are kept
/// outside ProjectModel so legacy engine and package code sees only live tracks.
struct TrackTombstone {
    TrackModel track;
    std::string afterId;
    std::string deleteOperationId;
    std::uint64_t deleteServerSequence = 0;
};

struct ClipTombstone {
    std::string trackId;
    ClipModel clip;
    std::string afterId;
    std::string deleteOperationId;
    std::uint64_t deleteServerSequence = 0;
};

struct MidiNoteTombstone {
    std::string trackId;
    std::string clipId;
    NoteModel note;
    std::string afterId;
    std::string deleteOperationId;
    std::uint64_t deleteServerSequence = 0;
};

struct AutomationPointTombstone {
    std::string trackId;
    std::string clipId;
    std::string laneId;
    AutomationPoint point;
    std::string afterId;
    std::string deleteOperationId;
    std::uint64_t deleteServerSequence = 0;
};

struct ControllerLaneTombstone {
    std::string trackId;
    std::string clipId;
    ControllerLane lane;
    std::string afterId;
    std::string deleteOperationId;
    std::uint64_t deleteServerSequence = 0;
};

struct TakeTombstone {
    std::string trackId;
    std::string clipId;
    TakeModel take;
    std::string afterId;
    std::string deleteOperationId;
    std::uint64_t deleteServerSequence = 0;
};

struct CompSegmentTombstone {
    std::string trackId;
    std::string clipId;
    CompSegment segment;
    std::string afterId;
    std::string deleteOperationId;
    std::uint64_t deleteServerSequence = 0;
};

struct SendTombstone {
    std::string trackId;
    SendModel send;
    std::string afterId;
    std::string deleteOperationId;
    std::uint64_t deleteServerSequence = 0;
};

struct PluginInsertTombstone {
    PluginLocation location;
    InsertModel insert;
    std::string afterId;
    std::string deleteOperationId;
    std::uint64_t deleteServerSequence = 0;
};

struct SharedProjectDocument {
    ProjectModel project;
    std::uint64_t confirmedSequence = 0;
    std::unordered_map<std::string, TrackTombstone> deletedTracks;
    std::unordered_map<std::string, ClipTombstone> deletedClips;
    std::unordered_map<std::string, MidiNoteTombstone> deletedNotes;
    std::unordered_map<std::string, AutomationPointTombstone>
        deletedAutomationPoints;
    std::unordered_map<std::string, ControllerLaneTombstone>
        deletedControllerLanes;
    std::unordered_map<std::string, TakeTombstone> deletedTakes;
    std::unordered_map<std::string, CompSegmentTombstone> deletedCompSegments;
    std::unordered_map<std::string, SendTombstone> deletedSends;
    std::unordered_map<std::string, PluginInsertTombstone>
        deletedPluginInserts;
    std::unordered_map<std::string, std::string> lastWriterByField;
    /// Positive-only materialization evidence. A live reducer remembers every
    /// operation it has replayed, while snapshot decode can reconstruct only
    /// operation ids still referenced by a field head or tombstone. Therefore
    /// membership proves durability, but absence after bootstrap never proves
    /// that an operation was not committed; recovery must query the retained
    /// server log by opId for that decision.
    std::unordered_set<std::string> appliedOperationIds;
};

/// State that affects only one user's audition/capture and must never enter a
/// ProjectCommand. These types are intentionally independent from Qt and the
/// audio-device implementation.
struct LocalTransportState {
    double positionSeconds = 0.0;
    bool playing = false;
    double loopStartSeconds = 0.0;
    double loopEndSeconds = 0.0;
    bool loopEnabled = false;
    bool metronomeEnabled = false;
};

struct LocalTrackState {
    bool soloed = false;
    bool armed = false;
    bool monitor = false;
    bool monitorAuto = false;
    TrackRecordMode recordMode = TrackRecordMode::UseGlobal;
    bool inputEnabled = false;
    std::uint32_t inputChannel = 0;
    std::uint32_t inputChannelCount = 1;
};

struct LocalPluginEditorState {
    PluginEditorChannel channel = PluginEditorChannel::Left;
    int windowX = 0;
    int windowY = 0;
    int windowWidth = 0;
    int windowHeight = 0;
    bool windowOpen = false;
};

struct LocalRecordingPreferences {
    RecordMode mode = RecordMode::Overwrite;
    bool loopTakes = true;
    int countInBars = 0;
    bool trimToRecordedRegion = true;
};

struct LocalSessionState {
    LocalTransportState transport;
    double audioDeviceSampleRate = 48000.0;
    std::uint32_t audioBufferFrames = 512;
    LocalRecordingPreferences recording;
    std::unordered_map<std::string, LocalTrackState> tracks;
};

struct LocalTrackUiState {
    double height = 72.0;
    bool expanded = true;
    bool automationExpanded = false;
};

struct LocalUiState {
    std::unordered_map<std::string, LocalPluginEditorState> pluginEditors;
    std::unordered_map<std::string, LocalTrackUiState> tracks;
    std::unordered_map<std::string, bool> expandedClips;
};

enum class SyncPhase : std::uint8_t {
    LocalOnly,
    Connecting,
    Synced,
    OfflineQueued,
    Reconnecting,
    Conflict,
    ReadOnly,
};

struct ConnectionState {
    std::string projectId;
    std::string userId;
    std::string clientId;
    std::uint64_t lastConfirmedSequence = 0;
    std::size_t pendingCommandCount = 0;
    SyncPhase phase = SyncPhase::LocalOnly;
};

} // namespace daw::collab
