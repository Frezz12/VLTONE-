#include "collaboration/SharedProjectSnapshot.hpp"

#include "ProjectSerializer.hpp"
#include "cloud/CloudDocumentProjection.hpp"
#include "collaboration/ProjectCommand.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <type_traits>
#include <utility>
#include <vector>

namespace daw::collab {
namespace {

using json = nlohmann::json;

constexpr std::size_t kMaximumTombstonesPerKind = 100000;
constexpr std::size_t kMaximumFieldWriters = 1000000;
constexpr std::size_t kMaximumFieldKeyBytes = 512;
constexpr char kWrapperTrackId[] =
    "00000000-0000-4000-8000-000000000001";
constexpr char kWrapperClipId[] =
    "00000000-0000-4000-8000-000000000002";

audio::Result invalid(std::string message) {
    return audio::Result::fail(audio::EngineError::UnsupportedFormat,
                               std::move(message));
}

bool exactKeys(const json& value,
               std::initializer_list<const char*> keys) {
    if (!value.is_object() || value.size() != keys.size()) return false;
    return std::all_of(keys.begin(), keys.end(), [&](const char* key) {
        return value.contains(key);
    });
}

bool sequenceValue(const json& value, std::uint64_t& result) {
    if (value.is_number_unsigned()) {
        result = value.get<std::uint64_t>();
        return result <= std::uint64_t(std::numeric_limits<std::int64_t>::max());
    }
    if (!value.is_number_integer()) return false;
    const std::int64_t signedValue = value.get<std::int64_t>();
    if (signedValue < 0) return false;
    result = std::uint64_t(signedValue);
    return true;
}

bool validFieldKey(const std::string& value) {
    if (value.empty() || value.size() > kMaximumFieldKeyBytes) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return character >= 0x20 && character != 0x7f;
    });
}

bool projectContainsLocalState(const ProjectModel& project) {
    return cloud::containsLocalPathOrUiState(project, nullptr);
}

bool trackContainsLocalState(const TrackModel& track) {
    ProjectModel project;
    project.tracks.push_back(track);
    return projectContainsLocalState(project);
}

bool clipContainsLocalState(const ClipModel& clip) {
    TrackModel track;
    track.id = kWrapperTrackId;
    track.clips.push_back(clip);
    return trackContainsLocalState(track);
}

bool pluginContainsLocalState(const InsertModel& insert) {
    ProjectModel project;
    project.masterInserts.push_back(insert);
    return projectContainsLocalState(project);
}

audio::Result projectJson(const ProjectModel& project, json& output) {
    std::string bytes;
    const audio::Result encoded = ProjectSerializer::serializeDocument(
        project, bytes, MediaPaths::Absolute);
    if (!encoded) return encoded;
    try {
        output = json::parse(bytes);
    } catch (const std::exception& error) {
        return invalid(std::string("cannot parse encoded project: ") +
                       error.what());
    }
    return audio::Result::ok();
}

audio::Result projectFromJson(const json& input, ProjectModel& project) {
    return ProjectSerializer::deserializeDocument(project, input.dump());
}

json trackEntity(const TrackModel& track) {
    ProjectModel wrapper;
    wrapper.tracks.push_back(track);
    json root;
    if (!projectJson(wrapper, root)) return nullptr;
    return root.at("tracks").at(0);
}

json clipEntity(const ClipModel& clip) {
    TrackModel track;
    track.id = kWrapperTrackId;
    track.clips.push_back(clip);
    return trackEntity(track).at("clips").at(0);
}

json noteEntity(const NoteModel& note) {
    ClipModel clip;
    clip.id = kWrapperClipId;
    clip.kind = ClipKind::Midi;
    clip.notes.push_back(note);
    return clipEntity(clip).at("notes").at(0);
}

json pointEntity(const AutomationPoint& point) {
    ClipModel clip;
    clip.id = kWrapperClipId;
    clip.kind = ClipKind::Automation;
    clip.automation.points.push_back(point);
    return clipEntity(clip).at("automation").at("points").at(0);
}

json laneEntity(const ControllerLane& lane) {
    ClipModel clip;
    clip.id = kWrapperClipId;
    clip.kind = ClipKind::Midi;
    clip.lanes.push_back(lane);
    return clipEntity(clip).at("lanes").at(0);
}

json takeEntity(const TakeModel& take) {
    ClipModel clip;
    clip.id = kWrapperClipId;
    clip.kind = ClipKind::Audio;
    clip.takes.push_back(take);
    return clipEntity(clip).at("takes").at(0);
}

json compEntity(const CompSegment& segment) {
    ClipModel clip;
    clip.id = kWrapperClipId;
    clip.kind = ClipKind::Audio;
    clip.durationSeconds = std::max(1.0, segment.endSeconds);
    TakeModel take;
    take.id = segment.takeId;
    take.lengthSeconds = clip.durationSeconds;
    clip.takes.push_back(std::move(take));
    clip.comp.push_back(segment);
    return clipEntity(clip).at("comp").at(0);
}

json sendEntity(const SendModel& send) {
    TrackModel track;
    track.id = kWrapperTrackId;
    track.sends.push_back(send);
    return trackEntity(track).at("sends").at(0);
}

json pluginEntity(const InsertModel& insert) {
    ProjectModel wrapper;
    wrapper.masterInserts.push_back(insert);
    json root;
    if (!projectJson(wrapper, root)) return nullptr;
    return root.at("masterInserts").at(0);
}

json pluginLocationEntity(const PluginLocation& location) {
    return json{{"chain", pluginChainName(location.chain)},
                {"trackId", location.trackId},
                {"clipId", location.clipId}};
}

bool decodeTrackEntity(const json& entity, TrackModel& result) {
    ProjectModel wrapper;
    json root;
    if (!projectJson(wrapper, root)) return false;
    root["tracks"] = json::array({entity});
    return projectFromJson(root, wrapper).isOk() &&
           wrapper.tracks.size() == 1 &&
           (result = std::move(wrapper.tracks.front()), true);
}

bool decodeClipEntity(const json& entity, ClipModel& result) {
    TrackModel track;
    track.id = kWrapperTrackId;
    json encodedTrack = trackEntity(track);
    if (!encodedTrack.is_object()) return false;
    encodedTrack["clips"] = json::array({entity});
    return decodeTrackEntity(encodedTrack, track) && track.clips.size() == 1 &&
           (result = std::move(track.clips.front()), true);
}

bool decodeNoteEntity(const json& entity, NoteModel& result) {
    ClipModel clip;
    clip.id = kWrapperClipId;
    clip.kind = ClipKind::Midi;
    json encodedClip = clipEntity(clip);
    if (!encodedClip.is_object()) return false;
    encodedClip["notes"] = json::array({entity});
    return decodeClipEntity(encodedClip, clip) && clip.notes.size() == 1 &&
           (result = std::move(clip.notes.front()), true);
}

bool decodePointEntity(const json& entity, AutomationPoint& result) {
    ClipModel clip;
    clip.id = kWrapperClipId;
    clip.kind = ClipKind::Automation;
    json encodedClip = clipEntity(clip);
    if (!encodedClip.is_object() ||
        !encodedClip.at("automation").is_object()) {
        return false;
    }
    encodedClip["automation"]["points"] = json::array({entity});
    return decodeClipEntity(encodedClip, clip) &&
           clip.automation.points.size() == 1 &&
           (result = std::move(clip.automation.points.front()), true);
}

bool decodeLaneEntity(const json& entity, ControllerLane& result) {
    ClipModel clip;
    clip.id = kWrapperClipId;
    clip.kind = ClipKind::Midi;
    json encodedClip = clipEntity(clip);
    if (!encodedClip.is_object()) return false;
    encodedClip["lanes"] = json::array({entity});
    return decodeClipEntity(encodedClip, clip) && clip.lanes.size() == 1 &&
           (result = std::move(clip.lanes.front()), true);
}

bool decodeTakeEntity(const json& entity, TakeModel& result) {
    ClipModel clip;
    clip.id = kWrapperClipId;
    clip.kind = ClipKind::Audio;
    json encodedClip = clipEntity(clip);
    if (!encodedClip.is_object()) return false;
    encodedClip["takes"] = json::array({entity});
    return decodeClipEntity(encodedClip, clip) && clip.takes.size() == 1 &&
           (result = std::move(clip.takes.front()), true);
}

bool decodeCompEntity(const json& entity, CompSegment& result) {
    if (!entity.is_object()) return false;
    const std::string takeId = entity.value("takeId", std::string());
    if (!isUuid(takeId)) return false;
    ClipModel clip;
    clip.id = kWrapperClipId;
    clip.kind = ClipKind::Audio;
    clip.durationSeconds = std::max(1.0, entity.value("endSeconds", 0.0));
    TakeModel take;
    take.id = takeId;
    take.lengthSeconds = clip.durationSeconds;
    clip.takes.push_back(take);
    json encodedClip = clipEntity(clip);
    if (!encodedClip.is_object()) return false;
    encodedClip["comp"] = json::array({entity});
    return decodeClipEntity(encodedClip, clip) && clip.comp.size() == 1 &&
           (result = std::move(clip.comp.front()), true);
}

bool decodeSendEntity(const json& entity, SendModel& result) {
    TrackModel track;
    track.id = kWrapperTrackId;
    json encodedTrack = trackEntity(track);
    if (!encodedTrack.is_object()) return false;
    encodedTrack["sends"] = json::array({entity});
    return decodeTrackEntity(encodedTrack, track) && track.sends.size() == 1 &&
           (result = std::move(track.sends.front()), true);
}

bool decodePluginEntity(const json& entity, InsertModel& result) {
    ProjectModel wrapper;
    json root;
    if (!projectJson(wrapper, root)) return false;
    root["masterInserts"] = json::array({entity});
    return projectFromJson(root, wrapper).isOk() &&
           wrapper.masterInserts.size() == 1 &&
           (result = std::move(wrapper.masterInserts.front()), true);
}

bool decodePluginLocation(const json& value, PluginLocation& result) {
    if (!exactKeys(value, {"chain", "trackId", "clipId"})) return false;
    if (!pluginChainFromName(value.value("chain", std::string()), result.chain))
        return false;
    result.trackId = value.value("trackId", std::string());
    result.clipId = value.value("clipId", std::string());
    switch (result.chain) {
        case PluginChain::Master:
            return result.trackId.empty() && result.clipId.empty();
        case PluginChain::Track:
        case PluginChain::Instrument:
        case PluginChain::SamplerFx:
            return isUuid(result.trackId) && result.clipId.empty();
        case PluginChain::Clip:
            return isUuid(result.trackId) && isUuid(result.clipId);
    }
    return false;
}

template <typename Map, typename EntityEncoder>
json encodeTombstones(const Map& values, EntityEncoder encode) {
    std::vector<typename Map::const_iterator> ordered;
    ordered.reserve(values.size());
    for (auto iterator = values.begin(); iterator != values.end(); ++iterator)
        ordered.push_back(iterator);
    std::sort(ordered.begin(), ordered.end(), [](auto left, auto right) {
        return left->first < right->first;
    });
    json result = json::array();
    for (auto iterator : ordered) result.push_back(encode(iterator->second));
    return result;
}

bool commonTombstone(const json& value, std::string& id,
                     std::string& deleteOperationId,
                     std::uint64_t& deleteSequence,
                     std::initializer_list<const char*> exactShape) {
    if (!exactKeys(value, exactShape)) return false;
    id = value.value("id", std::string());
    deleteOperationId = value.value("deleteOperationId", std::string());
    return isUuid(id) && isUuid(deleteOperationId) &&
           sequenceValue(value.at("deleteServerSequence"), deleteSequence) &&
           deleteSequence > 0 && value.at("entity").is_object();
}

template <typename Map, typename Decoder>
bool decodeTombstoneArray(const json& values, Map& output, Decoder decode) {
    if (!values.is_array() || values.size() > kMaximumTombstonesPerKind)
        return false;
    for (const json& value : values) {
        typename Map::mapped_type tombstone;
        std::string id;
        if (!decode(value, id, tombstone) || output.contains(id)) return false;
        output.emplace(std::move(id), std::move(tombstone));
    }
    return true;
}

template <typename Tombstone>
bool validDeleteSequence(const Tombstone& tombstone,
                         std::uint64_t snapshotSequence) {
    return tombstone.deleteServerSequence > 0 &&
           tombstone.deleteServerSequence <= snapshotSequence &&
           isUuid(tombstone.deleteOperationId);
}

} // namespace

audio::Result serializeSharedProjectSnapshot(
    const SharedProjectDocument& document, std::string& bytes) {
    bytes.clear();
    if (document.confirmedSequence >
        std::uint64_t(std::numeric_limits<std::int64_t>::max())) {
        return invalid("snapshot sequence is outside the server range");
    }
    if (projectContainsLocalState(document.project))
        return invalid("shared snapshot contains local path or UI/session state");
    json project;
    const audio::Result encodedProject = projectJson(document.project, project);
    if (!encodedProject) return encodedProject;

    for (const auto& [id, value] : document.deletedTracks)
        if (id != value.track.id || !isUuid(id) ||
            !validDeleteSequence(value, document.confirmedSequence) ||
            trackContainsLocalState(value.track))
            return invalid("invalid deleted track snapshot state");
    for (const auto& [id, value] : document.deletedClips)
        if (id != value.clip.id || !isUuid(id) || !isUuid(value.trackId) ||
            !validDeleteSequence(value, document.confirmedSequence) ||
            clipContainsLocalState(value.clip))
            return invalid("invalid deleted clip snapshot state");
    for (const auto& [id, value] : document.deletedNotes)
        if (id != value.note.id || !isUuid(id) || !isUuid(value.trackId) ||
            !isUuid(value.clipId) ||
            !validDeleteSequence(value, document.confirmedSequence))
            return invalid("invalid deleted note snapshot state");
    for (const auto& [id, value] : document.deletedAutomationPoints)
        if (id != value.point.id || !isUuid(id) || !isUuid(value.trackId) ||
            !isUuid(value.clipId) ||
            (!value.laneId.empty() && !isUuid(value.laneId)) ||
            !validDeleteSequence(value, document.confirmedSequence))
            return invalid("invalid deleted automation point snapshot state");
    for (const auto& [id, value] : document.deletedControllerLanes)
        if (id != value.lane.id || !isUuid(id) || !isUuid(value.trackId) ||
            !isUuid(value.clipId) ||
            !validDeleteSequence(value, document.confirmedSequence))
            return invalid("invalid deleted controller lane snapshot state");
    for (const auto& [id, value] : document.deletedTakes)
        if (id != value.take.id || !isUuid(id) || !isUuid(value.trackId) ||
            !isUuid(value.clipId) ||
            !validDeleteSequence(value, document.confirmedSequence) ||
            !value.take.filePath.empty())
            return invalid("invalid deleted take snapshot state");
    for (const auto& [id, value] : document.deletedCompSegments)
        if (id != value.segment.id || !isUuid(id) || !isUuid(value.trackId) ||
            !isUuid(value.clipId) ||
            !validDeleteSequence(value, document.confirmedSequence))
            return invalid("invalid deleted comp segment snapshot state");
    for (const auto& [id, value] : document.deletedSends)
        if (id != value.send.id || !isUuid(id) || !isUuid(value.trackId) ||
            !isUuid(value.send.destinationTrackId) ||
            !validDeleteSequence(value, document.confirmedSequence))
            return invalid("invalid deleted send snapshot state");
    for (const auto& [id, value] : document.deletedPluginInserts) {
        const bool validLocation = [&] {
            switch (value.location.chain) {
                case PluginChain::Master:
                    return value.location.trackId.empty() &&
                           value.location.clipId.empty();
                case PluginChain::Track:
                case PluginChain::Instrument:
                case PluginChain::SamplerFx:
                    return isUuid(value.location.trackId) &&
                           value.location.clipId.empty();
                case PluginChain::Clip:
                    return isUuid(value.location.trackId) &&
                           isUuid(value.location.clipId);
            }
            return false;
        }();
        if (id != value.insert.id || !isUuid(id) || !validLocation ||
            !validDeleteSequence(value, document.confirmedSequence) ||
            pluginContainsLocalState(value.insert)) {
            return invalid("invalid deleted plugin snapshot state");
        }
    }

    json writers = json::array();
    std::vector<std::pair<std::string, std::string>> orderedWriters(
        document.lastWriterByField.begin(), document.lastWriterByField.end());
    std::sort(orderedWriters.begin(), orderedWriters.end());
    if (orderedWriters.size() > kMaximumFieldWriters)
        return invalid("snapshot has too many field writers");
    for (const auto& [field, operation] : orderedWriters) {
        if (!validFieldKey(field) || !isUuid(operation))
            return invalid("snapshot has an invalid field writer");
        writers.push_back(json{{"fieldKey", field},
                               {"operationId", operation}});
    }

    const auto trackTombstone = [](const TrackTombstone& value) {
        return json{{"id", value.track.id},
                    {"afterId", value.afterId},
                    {"deleteOperationId", value.deleteOperationId},
                    {"deleteServerSequence", value.deleteServerSequence},
                    {"entity", trackEntity(value.track)}};
    };
    const auto clipTombstone = [](const ClipTombstone& value) {
        return json{{"id", value.clip.id},
                    {"trackId", value.trackId},
                    {"afterId", value.afterId},
                    {"deleteOperationId", value.deleteOperationId},
                    {"deleteServerSequence", value.deleteServerSequence},
                    {"entity", clipEntity(value.clip)}};
    };
    const auto noteTombstone = [](const MidiNoteTombstone& value) {
        return json{{"id", value.note.id},
                    {"trackId", value.trackId},
                    {"clipId", value.clipId},
                    {"afterId", value.afterId},
                    {"deleteOperationId", value.deleteOperationId},
                    {"deleteServerSequence", value.deleteServerSequence},
                    {"entity", noteEntity(value.note)}};
    };
    const auto pointTombstone = [](const AutomationPointTombstone& value) {
        return json{{"id", value.point.id},
                    {"trackId", value.trackId},
                    {"clipId", value.clipId},
                    {"laneId", value.laneId},
                    {"afterId", value.afterId},
                    {"deleteOperationId", value.deleteOperationId},
                    {"deleteServerSequence", value.deleteServerSequence},
                    {"entity", pointEntity(value.point)}};
    };
    const auto laneTombstone = [](const ControllerLaneTombstone& value) {
        return json{{"id", value.lane.id},
                    {"trackId", value.trackId},
                    {"clipId", value.clipId},
                    {"afterId", value.afterId},
                    {"deleteOperationId", value.deleteOperationId},
                    {"deleteServerSequence", value.deleteServerSequence},
                    {"entity", laneEntity(value.lane)}};
    };
    const auto takeTombstone = [](const TakeTombstone& value) {
        return json{{"id", value.take.id},
                    {"trackId", value.trackId},
                    {"clipId", value.clipId},
                    {"afterId", value.afterId},
                    {"deleteOperationId", value.deleteOperationId},
                    {"deleteServerSequence", value.deleteServerSequence},
                    {"entity", takeEntity(value.take)}};
    };
    const auto compTombstone = [](const CompSegmentTombstone& value) {
        return json{{"id", value.segment.id},
                    {"trackId", value.trackId},
                    {"clipId", value.clipId},
                    {"afterId", value.afterId},
                    {"deleteOperationId", value.deleteOperationId},
                    {"deleteServerSequence", value.deleteServerSequence},
                    {"entity", compEntity(value.segment)}};
    };
    const auto sendTombstone = [](const SendTombstone& value) {
        return json{{"id", value.send.id},
                    {"trackId", value.trackId},
                    {"afterId", value.afterId},
                    {"deleteOperationId", value.deleteOperationId},
                    {"deleteServerSequence", value.deleteServerSequence},
                    {"entity", sendEntity(value.send)}};
    };
    const auto pluginTombstone = [](const PluginInsertTombstone& value) {
        return json{{"id", value.insert.id},
                    {"location", pluginLocationEntity(value.location)},
                    {"afterId", value.afterId},
                    {"deleteOperationId", value.deleteOperationId},
                    {"deleteServerSequence", value.deleteServerSequence},
                    {"entity", pluginEntity(value.insert)}};
    };

    json root{
        {"format", "vlt-shared-project-snapshot"},
        {"schemaVersion", kSharedProjectSnapshotSchemaVersion},
        {"projectFormatVersion", kSharedProjectFormatVersion},
        {"serverSequence", document.confirmedSequence},
        {"project", std::move(project)},
        {"deletedTracks", encodeTombstones(document.deletedTracks,
                                             trackTombstone)},
        {"deletedClips", encodeTombstones(document.deletedClips,
                                            clipTombstone)},
        {"deletedNotes", encodeTombstones(document.deletedNotes,
                                            noteTombstone)},
        {"deletedAutomationPoints",
         encodeTombstones(document.deletedAutomationPoints, pointTombstone)},
        {"deletedControllerLanes",
         encodeTombstones(document.deletedControllerLanes, laneTombstone)},
        {"deletedTakes", encodeTombstones(document.deletedTakes,
                                            takeTombstone)},
        {"deletedCompSegments",
         encodeTombstones(document.deletedCompSegments, compTombstone)},
        {"deletedSends", encodeTombstones(document.deletedSends,
                                           sendTombstone)},
        {"deletedPluginInserts",
         encodeTombstones(document.deletedPluginInserts, pluginTombstone)},
        {"fieldWriters", std::move(writers)},
    };
    try {
        bytes = root.dump();
    } catch (const std::exception& error) {
        return audio::Result::fail(
            audio::EngineError::FileWriteError,
            std::string("cannot encode shared snapshot: ") + error.what());
    }
    if (bytes.size() > kMaximumSharedProjectSnapshotBytes) {
        bytes.clear();
        return audio::Result::fail(audio::EngineError::FileWriteError,
                                   "shared snapshot exceeds the client limit");
    }
    return audio::Result::ok();
}

audio::Result deserializeSharedProjectSnapshot(
    SharedProjectDocument& document, std::string_view bytes) {
    if (bytes.empty() || bytes.size() > kMaximumSharedProjectSnapshotBytes)
        return invalid("shared snapshot size is invalid");
    json root;
    try {
        root = json::parse(bytes.begin(), bytes.end());
    } catch (const std::exception& error) {
        return invalid(std::string("invalid shared snapshot JSON: ") +
                       error.what());
    }
    if (!exactKeys(root,
                   {"format", "schemaVersion", "projectFormatVersion",
                    "serverSequence", "project", "deletedTracks",
                    "deletedClips", "deletedNotes",
                    "deletedAutomationPoints", "deletedControllerLanes",
                    "deletedTakes", "deletedCompSegments", "deletedSends",
                    "deletedPluginInserts", "fieldWriters"}) ||
        root.value("format", std::string()) !=
            "vlt-shared-project-snapshot" ||
        root.value("schemaVersion", 0) !=
            kSharedProjectSnapshotSchemaVersion ||
        root.value("projectFormatVersion", 0) !=
            kSharedProjectFormatVersion ||
        !root.at("project").is_object()) {
        return invalid("shared snapshot envelope is invalid");
    }
    SharedProjectDocument parsed;
    if (!sequenceValue(root.at("serverSequence"), parsed.confirmedSequence))
        return invalid("shared snapshot sequence is invalid");
    const audio::Result decodedProject =
        projectFromJson(root.at("project"), parsed.project);
    if (!decodedProject) return decodedProject;
    if (projectContainsLocalState(parsed.project))
        return invalid("shared snapshot contains local project state");

    if (!decodeTombstoneArray(
            root.at("deletedTracks"), parsed.deletedTracks,
            [&](const json& value, std::string& id, TrackTombstone& result) {
                if (!commonTombstone(
                        value, id, result.deleteOperationId,
                        result.deleteServerSequence,
                        {"id", "afterId", "deleteOperationId",
                         "deleteServerSequence", "entity"}) ||
                    !decodeTrackEntity(value.at("entity"), result.track) ||
                    result.track.id != id)
                    return false;
                result.afterId = value.value("afterId", std::string());
                return (result.afterId.empty() || isUuid(result.afterId)) &&
                       result.deleteServerSequence <= parsed.confirmedSequence;
            }) ||
        !decodeTombstoneArray(
            root.at("deletedClips"), parsed.deletedClips,
            [&](const json& value, std::string& id, ClipTombstone& result) {
                if (!commonTombstone(
                        value, id, result.deleteOperationId,
                        result.deleteServerSequence,
                        {"id", "trackId", "afterId", "deleteOperationId",
                         "deleteServerSequence", "entity"}) ||
                    !decodeClipEntity(value.at("entity"), result.clip) ||
                    result.clip.id != id)
                    return false;
                result.trackId = value.value("trackId", std::string());
                result.afterId = value.value("afterId", std::string());
                return isUuid(result.trackId) &&
                       (result.afterId.empty() || isUuid(result.afterId)) &&
                       result.deleteServerSequence <= parsed.confirmedSequence;
            }) ||
        !decodeTombstoneArray(
            root.at("deletedNotes"), parsed.deletedNotes,
            [&](const json& value, std::string& id,
                MidiNoteTombstone& result) {
                if (!commonTombstone(
                        value, id, result.deleteOperationId,
                        result.deleteServerSequence,
                        {"id", "trackId", "clipId", "afterId",
                         "deleteOperationId", "deleteServerSequence",
                         "entity"}) ||
                    !decodeNoteEntity(value.at("entity"), result.note) ||
                    result.note.id != id)
                    return false;
                result.trackId = value.value("trackId", std::string());
                result.clipId = value.value("clipId", std::string());
                result.afterId = value.value("afterId", std::string());
                return isUuid(result.trackId) && isUuid(result.clipId) &&
                       (result.afterId.empty() || isUuid(result.afterId)) &&
                       result.deleteServerSequence <= parsed.confirmedSequence;
            }) ||
        !decodeTombstoneArray(
            root.at("deletedAutomationPoints"),
            parsed.deletedAutomationPoints,
            [&](const json& value, std::string& id,
                AutomationPointTombstone& result) {
                if (!commonTombstone(
                        value, id, result.deleteOperationId,
                        result.deleteServerSequence,
                        {"id", "trackId", "clipId", "laneId", "afterId",
                         "deleteOperationId", "deleteServerSequence",
                         "entity"}) ||
                    !decodePointEntity(value.at("entity"), result.point) ||
                    result.point.id != id)
                    return false;
                result.trackId = value.value("trackId", std::string());
                result.clipId = value.value("clipId", std::string());
                result.laneId = value.value("laneId", std::string());
                result.afterId = value.value("afterId", std::string());
                return isUuid(result.trackId) && isUuid(result.clipId) &&
                       (result.laneId.empty() || isUuid(result.laneId)) &&
                       (result.afterId.empty() || isUuid(result.afterId)) &&
                       result.deleteServerSequence <= parsed.confirmedSequence;
            }) ||
        !decodeTombstoneArray(
            root.at("deletedControllerLanes"),
            parsed.deletedControllerLanes,
            [&](const json& value, std::string& id,
                ControllerLaneTombstone& result) {
                if (!commonTombstone(
                        value, id, result.deleteOperationId,
                        result.deleteServerSequence,
                        {"id", "trackId", "clipId", "afterId",
                         "deleteOperationId", "deleteServerSequence",
                         "entity"}) ||
                    !decodeLaneEntity(value.at("entity"), result.lane) ||
                    result.lane.id != id)
                    return false;
                result.trackId = value.value("trackId", std::string());
                result.clipId = value.value("clipId", std::string());
                result.afterId = value.value("afterId", std::string());
                return isUuid(result.trackId) && isUuid(result.clipId) &&
                       (result.afterId.empty() || isUuid(result.afterId)) &&
                       result.deleteServerSequence <= parsed.confirmedSequence;
            }) ||
        !decodeTombstoneArray(
            root.at("deletedTakes"), parsed.deletedTakes,
            [&](const json& value, std::string& id, TakeTombstone& result) {
                if (!commonTombstone(
                        value, id, result.deleteOperationId,
                        result.deleteServerSequence,
                        {"id", "trackId", "clipId", "afterId",
                         "deleteOperationId", "deleteServerSequence",
                         "entity"}) ||
                    !decodeTakeEntity(value.at("entity"), result.take) ||
                    result.take.id != id)
                    return false;
                result.trackId = value.value("trackId", std::string());
                result.clipId = value.value("clipId", std::string());
                result.afterId = value.value("afterId", std::string());
                return isUuid(result.trackId) && isUuid(result.clipId) &&
                       (result.afterId.empty() || isUuid(result.afterId)) &&
                       result.deleteServerSequence <= parsed.confirmedSequence;
            }) ||
        !decodeTombstoneArray(
            root.at("deletedCompSegments"), parsed.deletedCompSegments,
            [&](const json& value, std::string& id,
                CompSegmentTombstone& result) {
                if (!commonTombstone(
                        value, id, result.deleteOperationId,
                        result.deleteServerSequence,
                        {"id", "trackId", "clipId", "afterId",
                         "deleteOperationId", "deleteServerSequence",
                         "entity"}) ||
                    !decodeCompEntity(value.at("entity"), result.segment) ||
                    result.segment.id != id)
                    return false;
                result.trackId = value.value("trackId", std::string());
                result.clipId = value.value("clipId", std::string());
                result.afterId = value.value("afterId", std::string());
                return isUuid(result.trackId) && isUuid(result.clipId) &&
                       (result.afterId.empty() || isUuid(result.afterId)) &&
                       result.deleteServerSequence <= parsed.confirmedSequence;
            }) ||
        !decodeTombstoneArray(
            root.at("deletedSends"), parsed.deletedSends,
            [&](const json& value, std::string& id, SendTombstone& result) {
                if (!commonTombstone(
                        value, id, result.deleteOperationId,
                        result.deleteServerSequence,
                        {"id", "trackId", "afterId", "deleteOperationId",
                         "deleteServerSequence", "entity"}) ||
                    !decodeSendEntity(value.at("entity"), result.send) ||
                    result.send.id != id) {
                    return false;
                }
                result.trackId = value.value("trackId", std::string());
                result.afterId = value.value("afterId", std::string());
                return isUuid(result.trackId) &&
                       isUuid(result.send.destinationTrackId) &&
                       (result.afterId.empty() || isUuid(result.afterId)) &&
                       result.deleteServerSequence <= parsed.confirmedSequence;
            }) ||
        !decodeTombstoneArray(
            root.at("deletedPluginInserts"), parsed.deletedPluginInserts,
            [&](const json& value, std::string& id,
                PluginInsertTombstone& result) {
                if (!commonTombstone(
                        value, id, result.deleteOperationId,
                        result.deleteServerSequence,
                        {"id", "location", "afterId", "deleteOperationId",
                         "deleteServerSequence", "entity"}) ||
                    !decodePluginLocation(value.at("location"),
                                          result.location) ||
                    !decodePluginEntity(value.at("entity"), result.insert) ||
                    result.insert.id != id) {
                    return false;
                }
                result.afterId = value.value("afterId", std::string());
                return (result.afterId.empty() || isUuid(result.afterId)) &&
                       result.deleteServerSequence <= parsed.confirmedSequence;
            })) {
        return invalid("shared snapshot tombstones are invalid");
    }
    for (const auto& [ignored, value] : parsed.deletedTracks)
        if (trackContainsLocalState(value.track))
            return invalid("deleted track contains local state");
    for (const auto& [ignored, value] : parsed.deletedClips)
        if (clipContainsLocalState(value.clip))
            return invalid("deleted clip contains local state");
    for (const auto& [ignored, value] : parsed.deletedTakes)
        if (!value.take.filePath.empty())
            return invalid("deleted take contains a local path");
    for (const auto& [ignored, value] : parsed.deletedPluginInserts)
        if (pluginContainsLocalState(value.insert))
            return invalid("deleted plugin contains local state");

    const json& writers = root.at("fieldWriters");
    if (!writers.is_array() || writers.size() > kMaximumFieldWriters)
        return invalid("shared snapshot field writers are invalid");
    std::string previousField;
    for (const json& writer : writers) {
        if (!exactKeys(writer, {"fieldKey", "operationId"}))
            return invalid("shared snapshot field writer shape is invalid");
        const std::string field = writer.value("fieldKey", std::string());
        const std::string operation =
            writer.value("operationId", std::string());
        if (!validFieldKey(field) || !isUuid(operation) ||
            (!previousField.empty() && field <= previousField))
            return invalid("shared snapshot field writer order is invalid");
        previousField = field;
        parsed.lastWriterByField.emplace(field, operation);
        parsed.appliedOperationIds.insert(operation);
    }
    // This rebuild is deliberately positive-only: snapshots do not carry an
    // unbounded historical operation-id set. IDs no longer referenced by a
    // current writer/tombstone remain durable in the server log, so callers
    // must never interpret their absence here as an authoritative negative.
    const auto rememberDelete = [&](const auto& values) {
        for (const auto& [ignored, tombstone] : values)
            parsed.appliedOperationIds.insert(tombstone.deleteOperationId);
    };
    rememberDelete(parsed.deletedTracks);
    rememberDelete(parsed.deletedClips);
    rememberDelete(parsed.deletedNotes);
    rememberDelete(parsed.deletedAutomationPoints);
    rememberDelete(parsed.deletedControllerLanes);
    rememberDelete(parsed.deletedTakes);
    rememberDelete(parsed.deletedCompSegments);
    rememberDelete(parsed.deletedSends);
    rememberDelete(parsed.deletedPluginInserts);
    document = std::move(parsed);
    return audio::Result::ok();
}

} // namespace daw::collab
