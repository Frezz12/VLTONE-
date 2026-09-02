#include "ProjectSerializer.hpp"
#include "collaboration/CommandGateway.hpp"
#include "collaboration/CommandJson.hpp"
#include "collaboration/ConditionalUndo.hpp"
#include "collaboration/SharedProjectSnapshot.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace daw;
using namespace daw::collab;

namespace {

int failures = 0;

bool check(bool condition, const char* label) {
    std::printf("%s  %s\n", condition ? "PASS" : "FAIL", label);
    if (!condition) ++failures;
    return condition;
}

std::string testUuid(std::string_view domain, std::string_view label) {
    return deterministicMigrationId(domain, label);
}

std::string operationId(std::string_view label) {
    return testUuid("collaboration-test-operation", label);
}

std::string trackId(std::string_view label) {
    return testUuid("collaboration-test-track", label);
}

CommandMeta meta(std::string label, std::uint64_t clientSequence = 1) {
    CommandMeta value;
    value.projectId = testUuid("collaboration-test", "project");
    value.operationId = label.empty() ? std::string() : operationId(label);
    value.actorId = testUuid("collaboration-test", "actor");
    value.clientId = testUuid("collaboration-test", "client");
    value.clientSequence = clientSequence;
    return value;
}

template <typename Body>
ProjectCommand command(std::string id, Body body,
                       std::uint64_t clientSequence = 1) {
    ProjectCommand value;
    value.meta = meta(std::move(id), clientSequence);
    value.body = std::move(body);
    return value;
}

NoteModel midiNote(std::string id, int pitch, double startBeats,
                   double lengthBeats = 1.0, int velocity = 100) {
    NoteModel note;
    note.id = std::move(id);
    note.pitch = pitch;
    note.startBeats = startBeats;
    note.lengthBeats = lengthBeats;
    note.velocity = velocity;
    return note;
}

AutomationPoint automationPoint(std::string id, double beats, double value,
                                AutomationSegment shape =
                                    AutomationSegment::Linear) {
    AutomationPoint point;
    point.id = std::move(id);
    point.beats = beats;
    point.value = value;
    point.shape = shape;
    return point;
}

AssetRef testAsset(std::string_view label, AssetKind kind, char digest = 'a') {
    AssetRef asset;
    asset.assetId = testUuid("collaboration-test-asset", label);
    asset.sha256.assign(64, digest);
    asset.kind = kind;
    asset.byteSize = 4096;
    asset.originalName = kind == AssetKind::Audio ? "sample.wav" : "state.bin";
    if (kind == AssetKind::Audio) {
        asset.mimeType = "audio/wav";
        asset.codec = "pcm_f32le";
        asset.sampleRate = 48000.0;
        asset.channels = 2;
        asset.frames = 1024;
    }
    return asset;
}

InsertModel builtinInsert(std::string_view label, std::string uid) {
    InsertModel insert;
    insert.id = testUuid("collaboration-test-plugin", label);
    insert.name = uid;
    insert.format = PluginFormat::Internal;
    insert.uid = std::move(uid);
    insert.vendor = "VLT";
    insert.pluginVersion = "1.0.0";
    insert.stateSchemaVersion = 1;
    if (insert.uid == "daw.sampler") {
        insert.assetBindings.push_back(
            PluginAssetBinding{"sample", testAsset("sampler-source",
                                                     AssetKind::Audio, 'b'),
                               true});
    }
    return insert;
}

std::string stateFingerprint(const SharedProjectDocument& state) {
    json tracks = json::array();
    for (const TrackModel& track : state.project.tracks) {
        json clips = json::array();
        for (const ClipModel& clip : track.clips) {
            json notes = json::array();
            for (const NoteModel& note : clip.notes) {
                notes.push_back(json{{"id", note.id},
                                     {"pitch", note.pitch},
                                     {"start", note.startBeats},
                                     {"length", note.lengthBeats},
                                     {"velocity", note.velocity},
                                     {"muted", note.muted},
                                     {"color", note.color},
                                     {"pan", note.pan}});
            }
            const auto pointsJson = [](const auto& points) {
                json result = json::array();
                for (const AutomationPoint& point : points) {
                    result.push_back(json{{"id", point.id},
                                          {"beats", point.beats},
                                          {"value", point.value},
                                          {"shape", toString(point.shape)},
                                          {"curve", point.curve}});
                }
                return result;
            };
            json lanes = json::array();
            for (const ControllerLane& lane : clip.lanes) {
                lanes.push_back(json{{"id", lane.id},
                                     {"name", lane.name},
                                     {"cc", lane.cc},
                                     {"parameterId", lane.parameterId},
                                     {"slotId", lane.slotId},
                                     {"defaultValue", lane.defaultValue},
                                     {"points", pointsJson(lane.points)}});
            }
            json takes = json::array();
            for (const TakeModel& take : clip.takes) {
                takes.push_back(json{{"id", take.id},
                                     {"name", take.name},
                                     {"filePath", take.filePath},
                                     {"offsetSeconds", take.offsetSeconds},
                                     {"lengthSeconds", take.lengthSeconds},
                                     {"clipOffsetSeconds",
                                      take.clipOffsetSeconds},
                                     {"gain", take.gain},
                                     {"muted", take.muted},
                                     {"channels", take.channels},
                                     {"color", take.color},
                                     {"assetId", take.asset.assetId},
                                     {"sha256", take.asset.sha256},
                                     {"byteSize", take.asset.byteSize}});
            }
            json comp = json::array();
            for (const CompSegment& segment : clip.comp) {
                comp.push_back(json{{"id", segment.id},
                                    {"takeId", segment.takeId},
                                    {"startSeconds", segment.startSeconds},
                                    {"endSeconds", segment.endSeconds}});
            }
            clips.push_back(json{{"id", clip.id},
                                 {"kind", toString(clip.kind)},
                                 {"name", clip.name},
                                 {"start", clip.startSeconds},
                                 {"duration", clip.durationSeconds},
                                 {"offset", clip.offsetSeconds},
                                 {"gain", clip.gain},
                                 {"pan", clip.pan},
                                 {"muted", clip.muted},
                                 {"color", clip.color},
                                 {"compCrossfadeMs", clip.compCrossfadeMs},
                                 {"notes", std::move(notes)},
                                 {"points", pointsJson(clip.automation.points)},
                                 {"automationTarget",
                                  json::array({
                                      toString(clip.automation.target.kind),
                                      clip.automation.target.channelId,
                                      clip.automation.target.slotId,
                                      clip.automation.target.parameterId,
                                      clip.automation.target.sendId})},
                                 {"automationDefault",
                                  clip.automation.defaultValue},
                                 {"automationActive", clip.automation.active},
                                 {"lanes", std::move(lanes)},
                                 {"takes", std::move(takes)},
                                 {"comp", std::move(comp)}});
        }
        tracks.push_back(json{{"id", track.id},
                              {"kind", toString(track.kind)},
                              {"name", track.name},
                              {"color", track.color},
                              {"volume", track.volume},
                              {"pan", track.pan},
                              {"muted", track.muted},
                              {"mono", track.mono},
                              {"parentId", track.parentId},
                              {"outputBusId", track.outputBusId},
                              {"clips", std::move(clips)}});
    }
    std::vector<std::pair<std::string, std::string>> writers(
        state.lastWriterByField.begin(), state.lastWriterByField.end());
    std::sort(writers.begin(), writers.end());
    json writerJson = json::array();
    for (const auto& [key, value] : writers)
        writerJson.push_back(json::array({key, value}));
    std::vector<std::string> tombstones;
    for (const auto& [id, ignored] : state.deletedTracks) tombstones.push_back(id);
    std::sort(tombstones.begin(), tombstones.end());
    std::vector<std::string> clipTombstones;
    for (const auto& [id, ignored] : state.deletedClips)
        clipTombstones.push_back(id);
    std::sort(clipTombstones.begin(), clipTombstones.end());
    std::vector<std::string> noteTombstones;
    for (const auto& [id, ignored] : state.deletedNotes)
        noteTombstones.push_back(id);
    std::sort(noteTombstones.begin(), noteTombstones.end());
    std::vector<std::string> pointTombstones;
    for (const auto& [id, ignored] : state.deletedAutomationPoints)
        pointTombstones.push_back(id);
    std::sort(pointTombstones.begin(), pointTombstones.end());
    std::vector<std::string> laneTombstones;
    for (const auto& [id, ignored] : state.deletedControllerLanes)
        laneTombstones.push_back(id);
    std::sort(laneTombstones.begin(), laneTombstones.end());
    std::vector<std::string> takeTombstones;
    for (const auto& [id, ignored] : state.deletedTakes)
        takeTombstones.push_back(id);
    std::sort(takeTombstones.begin(), takeTombstones.end());
    std::vector<std::string> compTombstones;
    for (const auto& [id, ignored] : state.deletedCompSegments)
        compTombstones.push_back(id);
    std::sort(compTombstones.begin(), compTombstones.end());
    return json{{"name", state.project.name},
                {"tempo", state.project.tempo},
                {"timeSig", json::array({state.project.timeSigNumerator,
                                          state.project.timeSigDenominator})},
                {"key", json::array({state.project.keyRoot, state.project.scale})},
                {"master", json::array({state.project.masterVolume,
                                         state.project.masterPan})},
                {"tracks", std::move(tracks)},
                {"writers", std::move(writerJson)},
                {"tombstones", tombstones},
                {"clipTombstones", clipTombstones},
                {"noteTombstones", noteTombstones},
                {"pointTombstones", pointTombstones},
                {"laneTombstones", laneTombstones},
                {"takeTombstones", takeTombstones},
                {"compTombstones", compTombstones}}
        .dump();
}

json readJson(const fs::path& file) {
    std::ifstream stream(file);
    json value;
    stream >> value;
    return value;
}

void serializerV6AndLegacyMigration(const fs::path& dir) {
    const fs::path legacyFile = dir / "legacy-v5.json";
    const std::string legacyTrackId = trackId("legacy");
    json compClip{{"id", "clip-comp"},
                  {"kind", "audio"},
                  {"durationSeconds", 4.0}};
    compClip["takes"] =
        json::array({json{{"id", "take-1"}, {"name", "Take 1"}}});
    compClip["comp"] = json::array({json{{"takeId", "take-1"},
                                           {"startSeconds", 0.0},
                                           {"endSeconds", 4.0}}});

    json automationClip{{"id", "clip-automation"},
                        {"kind", "automation"}};
    automationClip["automation"] =
        json{{"kind", "volume"},
             {"channelId", legacyTrackId},
             {"points", json::array({
                 json{{"beats", 0.0}, {"value", 0.25}},
                 json{{"beats", 4.0}, {"value", 0.75}},
             })}};

    json laneClip{{"id", "clip-lane"}, {"kind", "midi"}};
    laneClip["lanes"] = json::array({
        json{{"id", "lane-1"},
             {"points", json::array({
                 json{{"beats", 1.0}, {"value", 0.5}},
             })}},
    });

    json legacyTrack{{"id", legacyTrackId}, {"kind", "audio"}};
    legacyTrack["clips"] = json::array(
        {std::move(compClip), std::move(automationClip), std::move(laneClip)});
    const json legacy{{"format", "vlt-project"},
                      {"version", 5},
                      {"name", "Legacy"},
                      {"tracks", json::array({std::move(legacyTrack)})}};
    {
        std::ofstream stream(legacyFile);
        stream << legacy.dump(2);
    }

    ProjectModel first;
    ProjectModel second;
    check(ProjectSerializer::loadDocument(first, legacyFile.string(), dir.string())
              .isOk(),
          "loads legacy v5 document");
    check(ProjectSerializer::loadDocument(second, legacyFile.string(), dir.string())
              .isOk(),
          "loads legacy v5 document twice");
    const std::string compId = first.tracks[0].clips[0].comp[0].id;
    const std::string automationId =
        first.tracks[0].clips[1].automation.points[0].id;
    const std::string laneId = first.tracks[0].clips[2].lanes[0].points[0].id;
    check(!compId.empty() && !automationId.empty() && !laneId.empty(),
          "v5 migration assigns point and comp ids");
    check(compId == second.tracks[0].clips[0].comp[0].id &&
              automationId == second.tracks[0].clips[1].automation.points[0].id &&
              laneId == second.tracks[0].clips[2].lanes[0].points[0].id,
          "v5 migration ids are deterministic across clients");

    ProjectModel project;
    project.name = "Assets";
    TrackModel track;
    track.id = trackId("assets");
    ClipModel clip;
    clip.id = "clip-assets";
    clip.kind = ClipKind::Audio;
    clip.asset = AssetRef{"asset-audio", std::string(64, 'a'), AssetKind::Audio,
                          4096, "voice.wav", "audio/wav", "pcm_s16le", 48000.0,
                          2, 12000};
    TakeModel take;
    take.id = "take-assets";
    take.asset = clip.asset;
    clip.takes.push_back(take);
    track.clips.push_back(clip);

    InsertModel insert;
    insert.id = "insert-1";
    insert.format = PluginFormat::Internal;
    insert.uid = "daw.sampler";
    insert.name = "Sampler";
    insert.vendor = "VLT Studio Pro";
    insert.pluginVersion = "1.0";
    insert.stateSchemaVersion = 1;
    insert.stateAsset = AssetRef{"asset-state", std::string(64, 'b'),
                                 AssetKind::PluginState, 128, "state.bin"};
    insert.assetBindings.push_back(
        PluginAssetBinding{"sample", clip.asset, true});
    track.inserts.push_back(insert);
    project.tracks.push_back(track);

    const fs::path v6File = dir / "project-v6.json";
    check(ProjectSerializer::saveDocument(project, v6File.string(),
                                          MediaPaths::Absolute)
              .isOk(),
          "writes v6 project document");
    const json saved = readJson(v6File);
    check(saved.value("version", 0) == 6, "v6 writer publishes format version 6");
    const json& savedInsert = saved["tracks"][0]["inserts"][0];
    check(savedInsert.value("pluginVersion", "") == "1.0" &&
              savedInsert.value("stateSchemaVersion", 0) == 1,
          "v6 writes plugin product and state schema versions");
    check(savedInsert["stateAsset"].value("assetId", "") == "asset-state" &&
              !savedInsert["stateAsset"].contains("audioMetadata"),
          "v6 writes content-addressed plugin state");
    check(saved["tracks"][0]["clips"][0]["asset"]["audioMetadata"]
                  .value("sampleRate", 0.0) == 48000.0 &&
              savedInsert["assetBindings"][0].value("key", "") == "sample",
          "v6 nests audio metadata and writes plugin binding keys");

    ProjectModel loaded;
    check(ProjectSerializer::loadDocument(loaded, v6File.string(), dir.string())
              .isOk(),
          "loads v6 project document");
    const InsertModel& loadedInsert = loaded.tracks[0].inserts[0];
    check(loaded.tracks[0].clips[0].asset == clip.asset &&
              loadedInsert.pluginVersion == "1.0" &&
              loadedInsert.stateSchemaVersion == 1 &&
              loadedInsert.stateAsset == insert.stateAsset &&
              loadedInsert.assetBindings.size() == 1 &&
              loadedInsert.assetBindings[0].key == "sample",
          "v6 asset and plugin compatibility fields round-trip");

    std::string memoryBytes;
    ProjectModel memoryLoaded;
    check(ProjectSerializer::serializeDocument(project, memoryBytes)
                  .isOk() &&
              !memoryBytes.empty() &&
              ProjectSerializer::deserializeDocument(memoryLoaded,
                                                     memoryBytes)
                  .isOk() &&
              memoryLoaded.tracks.size() == 1 &&
              memoryLoaded.tracks[0].clips[0].asset == clip.asset,
          "in-memory v6 codec round-trips without filesystem access");
    std::string memoryBytesAgain;
    check(ProjectSerializer::serializeDocument(memoryLoaded,
                                               memoryBytesAgain)
                  .isOk() &&
              memoryBytesAgain == memoryBytes,
          "in-memory v6 codec is canonical across replay");
}

void commandWireRoundTrip() {
    ProjectCommand setTempo = command(
        "tempo", SetProjectScalar{ProjectScalar::Tempo, 132.0}, 7);
    setTempo.meta.transactionId = operationId("gesture");
    setTempo.meta.baseServerSequence = 41;
    setTempo.conditions.push_back(
        FieldWriterIs{"project:tempo", operationId("before-tempo")});
    const std::string wire = serializeProjectCommand(setTempo);
    const json encoded = json::parse(wire);
    check(!encoded.contains("meta") && encoded.value("schemaVersion", 0) == 1 &&
              encoded.value("opId", "") == operationId("tempo") &&
              encoded.value("baseServerSeq", 0) == 41 &&
              encoded.contains("preconditions") &&
              encoded["touchedFields"] == json::array({"project:tempo"}),
          "wire command uses the flat locked envelope");
    std::string error;
    auto parsed = deserializeProjectCommand(wire, &error);
    check(parsed.has_value() && error.empty() &&
              serializeProjectCommand(*parsed) == wire,
          "command JSON round-trip is canonical");

    const std::string wireTrack = trackId("wire-a");
    auto batchBody = std::make_shared<BatchCommand>();
    batchBody->commands.push_back(
        command("ignored-child", AddTrack{wireTrack, TrackKind::Audio,
                                           "A", 1, {}, {}}));
    batchBody->commands.push_back(
        command("ignored-child-2", SetTrackProperty{
            wireTrack, TrackProperty::Muted, true}));
    ProjectCommand batch = command("batch", batchBody);
    const json batchJson = projectCommandToJson(batch);
    const json& child = batchJson["payload"]["commands"][0];
    check(child.contains("kind") && child.contains("payload") &&
              child.contains("preconditions") && !child.contains("opId"),
          "batch children contain bodies, not nested envelopes");
    check(projectCommandFromJson(batchJson, &error).has_value(),
          "batch command parses from wire JSON");
    json mismatchedTouched = encoded;
    mismatchedTouched["touchedFields"] = json::array({"project:name"});
    check(!projectCommandFromJson(mismatchedTouched, &error).has_value(),
          "wire parser rejects forged touchedFields");
    json malformedOperation = encoded;
    malformedOperation["opId"] = "not-a-uuid";
    check(!projectCommandFromJson(malformedOperation, &error).has_value(),
          "wire parser rejects a malformed operation UUID");
    json nonCanonicalOperation = encoded;
    std::string upperOperation = nonCanonicalOperation.value("opId", "");
    std::transform(upperOperation.begin(), upperOperation.end(),
                   upperOperation.begin(), [](unsigned char c) {
                       return static_cast<char>(std::toupper(c));
                   });
    nonCanonicalOperation["opId"] = upperOperation;
    check(!projectCommandFromJson(nonCanonicalOperation, &error).has_value(),
          "wire parser rejects a non-canonical uppercase UUID");

#ifdef DAW_PROJECT_COMMAND_SCHEMA
    const json schema = readJson(DAW_PROJECT_COMMAND_SCHEMA);
    const auto& required = schema.at("required");
    const auto hasRequired = [&](const char* key) {
        return std::find(required.begin(), required.end(), key) != required.end();
    };
    check(hasRequired("schemaVersion") && hasRequired("opId") &&
              hasRequired("transactionId") && hasRequired("baseServerSeq") &&
              hasRequired("kind") && hasRequired("payload") &&
              hasRequired("preconditions") && hasRequired("touchedFields") &&
              schema["$defs"]["id"].value("format", "") == "uuid" &&
              schema["$defs"]["id"].value("pattern", "") ==
                  "^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$" &&
              schema["$defs"]["precondition"]["properties"]["kind"]
                      .value("const", "") == "fieldWriterIs" &&
              schema["properties"]["preconditions"].value("maxItems", 0) ==
                  1024 &&
              schema["properties"]["touchedFields"].value("maxItems", 0) ==
                  8192 &&
              schema["$defs"]["batchPayload"]["properties"]["commands"]
                      .value("maxItems", 0) == 1024 &&
              schema["$defs"]["recordingCommitPayload"]["properties"]
                      ["leases"]
                          .value("maxItems", 0) == 1024 &&
              schema["$defs"]["recordingCommitPayload"]["properties"]
                      ["commands"]["items"]
                          .value("$ref", "") ==
                  "#/$defs/recordingCommitItem" &&
              schema["$defs"]["recordingCommitItem"]["properties"]["kind"]
                      ["enum"] ==
                  json::array({"clip.add", "clip.setProperty",
                               "clip.setAsset", "take.add",
                               "compSegment.upsert"}) &&
              schema["$defs"]["recordingCommitItem"]["oneOf"].size() == 5 &&
              schema["$defs"]["recordingLease"]["properties"]["leaseId"]
                      .value("$ref", "") == "#/$defs/id" &&
              schema["$defs"]["audioAssetRef"]["properties"]["sha256"]
                      .value("pattern", "") == "^[0-9a-f]{64}$" &&
              schema["$defs"]["takeAddPayload"]["required"].size() == 4 &&
              schema["$defs"]["controllerLaneAddPayload"]["required"]
                      .size() == 7,
          "checked-in schema locks the command envelope");
#endif
}

void reducerReplayBatchDeleteAndUndo() {
    const std::string batchTrack = trackId("batch");
    const std::string goodBatchOp = operationId("good-batch");
    SharedProjectDocument batchState;
    auto goodBatch = std::make_shared<BatchCommand>();
    goodBatch->commands.push_back(
        command({}, AddTrack{batchTrack, TrackKind::Audio,
                              "Before", 4, {}, {}}));
    goodBatch->commands.push_back(command({}, SetTrackProperty{
        batchTrack, TrackProperty::Name, std::string("After")}));
    ApplyResult goodBatchResult = ProjectReducer::apply(
        batchState, command("good-batch", goodBatch));
    const bool writersUseOuter = std::all_of(
        batchState.lastWriterByField.begin(),
        batchState.lastWriterByField.end(), [&](const auto& writer) {
            return writer.second == goodBatchOp;
        });
    check(goodBatchResult.changed() && writersUseOuter &&
              batchState.appliedOperationIds.size() == 1 &&
              batchState.appliedOperationIds.contains(goodBatchOp),
          "batch children use only the durable outer operation id");
    const json inverseWire = projectCommandToJson(*goodBatchResult.inverse);
    bool inverseGuardsUseOuter = true;
    for (const json& child : inverseWire["payload"]["commands"]) {
        for (const json& guard : child["preconditions"]) {
            inverseGuardsUseOuter = inverseGuardsUseOuter &&
                guard.value("kind", "") == "fieldWriterIs" &&
                guard.value("operationId", "") == goodBatchOp;
        }
    }
    const std::string batchLifecycle =
        ProjectReducer::trackLifecycleKey(batchTrack);
    const bool addInverseTouchesLifecycle = std::find(
        inverseWire["touchedFields"].begin(),
        inverseWire["touchedFields"].end(), batchLifecycle) !=
        inverseWire["touchedFields"].end();
    ProjectCommand undoBatch = *goodBatchResult.inverse;
    undoBatch.meta = meta("undo-good-batch");
    ApplyResult undoneBatch = ProjectReducer::apply(batchState, undoBatch);
    check(inverseGuardsUseOuter && addInverseTouchesLifecycle &&
              undoneBatch.changed() &&
              batchState.project.findTrack(batchTrack) == nullptr,
          "batch add inverse uses fieldWriterIs lifecycle guards atomically");

    const std::string trackA = trackId("a");
    const std::string trackBId = trackId("b");
    const std::string trackC = trackId("c");
    const std::string missingTrack = trackId("missing");
    std::vector<ProjectCommand> log;
    log.push_back(command("add-a", AddTrack{trackA, TrackKind::Audio,
                                             "A", 0x11, {}, {}}));
    log.push_back(command("add-b", AddTrack{trackBId, TrackKind::Bus,
                                             "B", 0x22, {}, trackA}));
    log.push_back(command("tempo", SetProjectScalar{
        ProjectScalar::Tempo, 128.0}));
    log.push_back(command("name-a", SetTrackProperty{
        trackA, TrackProperty::Name, std::string("Lead")}));
    log.push_back(command("move-b", MoveTrack{trackBId, {}}));

    SharedProjectDocument left;
    SharedProjectDocument right;
    for (const ProjectCommand& item : log) {
        check(ProjectReducer::apply(left, item).accepted(),
              "left reducer accepts replay command");
        check(ProjectReducer::apply(right, item).accepted(),
              "right reducer accepts replay command");
    }
    check(stateFingerprint(left) == stateFingerprint(right),
          "independent reducer replay converges deterministically");
    check(ProjectReducer::apply(left, log.front()).code == ApplyCode::Duplicate,
          "duplicate operation id is idempotent");

    auto badBatch = std::make_shared<BatchCommand>();
    badBatch->commands.push_back(
        command({}, AddTrack{trackC, TrackKind::Audio, "C", 3, {}, trackA}));
    badBatch->commands.push_back(command({}, SetTrackProperty{
        missingTrack, TrackProperty::Muted, true}));
    const std::string beforeBatch = stateFingerprint(left);
    ApplyResult batchResult =
        ProjectReducer::apply(left, command("bad-batch", badBatch));
    check(!batchResult.accepted() && stateFingerprint(left) == beforeBatch &&
              left.project.findTrack(trackC) == nullptr,
          "rejected batch is atomic");

    TrackModel* trackB = left.project.findTrack(trackBId);
    trackB->outputBusId = trackA;
    trackB->sends.push_back(SendModel{
        testUuid("collaboration-test-send", "a"), trackA, 0.5f, false, true});
    ApplyResult deleted = ProjectReducer::apply(
        left, command("delete-a", DeleteTrack{trackA}));
    check(deleted.changed() && left.project.findTrack(trackA) == nullptr &&
              left.deletedTracks.contains(trackA) &&
              left.project.findTrack(trackBId)->outputBusId == trackA &&
              left.project.findTrack(trackBId)->sends.size() == 1,
          "track delete tombstones the target and retains stable routing refs");
    const ProjectCommand deleteShape =
        command("delete-a-shape", DeleteTrack{trackA});
    const std::vector<std::string> deleteTouchedVector =
        commandTouchedFields(deleteShape);
    const std::set<std::string> deleteTouched(deleteTouchedVector.begin(),
                                              deleteTouchedVector.end());
    check(deleted.impact.fieldKeys == deleteTouched &&
              json(deleteTouchedVector) ==
                  projectCommandToJson(deleteShape)["touchedFields"],
          "delete ApplyResult and wire declare the exact same field heads");
    const json deleteInverseWire = projectCommandToJson(*deleted.inverse);
    const std::string lifecycleKey = ProjectReducer::trackLifecycleKey(trackA);
    const bool deleteInverseIsGeneric =
        deleteInverseWire["preconditions"].size() == 1 &&
        deleteInverseWire["preconditions"][0].value("kind", "") ==
            "fieldWriterIs" &&
        deleteInverseWire["preconditions"][0].value("fieldKey", "") ==
            lifecycleKey &&
        std::find(deleteInverseWire["touchedFields"].begin(),
                  deleteInverseWire["touchedFields"].end(), lifecycleKey) !=
            deleteInverseWire["touchedFields"].end();
    check(deleteInverseIsGeneric,
          "delete inverse uses only the lifecycle fieldWriterIs contract");
    ApplyResult late = ProjectReducer::apply(
        left, command("late-name", SetTrackProperty{
            trackA, TrackProperty::Name, std::string("Too late")}));
    check(late.code == ApplyCode::DeletedEntity &&
              left.project.findTrack(trackA) == nullptr,
          "delete wins and rejects a late track mutation");

    ProjectCommand restore = *deleted.inverse;
    restore.meta = meta("restore-a");
    ApplyResult restored = ProjectReducer::apply(left, restore);
    check(restored.changed() && left.project.findTrack(trackA) &&
              left.project.findTrack(trackBId)->outputBusId == trackA &&
              left.project.findTrack(trackBId)->sends.size() == 1,
          "typed restore revives the target behind retained stable refs");

    SharedProjectDocument undoState;
    ConditionalUndoHistory history;
    ProjectCommand setName = command("set-name", SetProjectScalar{
        ProjectScalar::Name, std::string("First")});
    ApplyResult setNameResult = ProjectReducer::apply(undoState, setName);
    history.record(setName, setNameResult, "Rename Project");
    auto undo = history.prepareUndo(meta("undo-name"));
    check(undo.has_value(), "conditional undo prepares a typed inverse");
    ApplyResult undone = ProjectReducer::apply(undoState, undo->command);
    check(undone.changed() && undoState.project.name == "Untitled" &&
              history.complete(undo->token, undone) && history.canRedo(),
          "successful conditional undo creates redo history");
    auto redo = history.prepareRedo(meta("redo-name"));
    ApplyResult redone = ProjectReducer::apply(undoState, redo->command);
    check(redone.changed() && undoState.project.name == "First" &&
              history.complete(redo->token, redone),
          "redo is a new conditional command");

    SharedProjectDocument conflictState;
    ConditionalUndoHistory conflictHistory;
    ProjectCommand local = command("local", SetProjectScalar{
        ProjectScalar::Name, std::string("Local")});
    ApplyResult localResult = ProjectReducer::apply(conflictState, local);
    conflictHistory.record(local, localResult, "Local rename");
    ProjectReducer::apply(conflictState, command("remote", SetProjectScalar{
        ProjectScalar::Name, std::string("Remote")}));
    auto conflictingUndo = conflictHistory.prepareUndo(meta("conflict-undo"));
    ApplyResult conflict =
        ProjectReducer::apply(conflictState, conflictingUndo->command);
    check(conflict.code == ApplyCode::PreconditionsFailed &&
              !conflictHistory.complete(conflictingUndo->token, conflict) &&
              conflictHistory.canUndo() && conflictState.project.name == "Remote",
          "conditional undo cannot clobber a newer remote writer");

    SharedProjectDocument granularState;
    const std::string granularTrack = trackId("granular-undo");
    ProjectReducer::apply(
        granularState,
        command("granular-track",
                AddTrack{granularTrack, TrackKind::Audio, "Before", 1, {}, {}}));
    ApplyResult localTrackName = ProjectReducer::apply(
        granularState, command("local-track-name", SetTrackProperty{
                           granularTrack, TrackProperty::Name,
                           std::string("Local Name")}));
    ProjectReducer::apply(
        granularState, command("remote-track-volume", SetTrackProperty{
                           granularTrack, TrackProperty::Volume, 0.5}));
    ProjectCommand granularUndo = *localTrackName.inverse;
    granularUndo.meta = meta("undo-local-track-name");
    check(ProjectReducer::apply(granularState, granularUndo).changed() &&
              granularState.project.findTrack(granularTrack)->name == "Before" &&
              granularState.project.findTrack(granularTrack)->volume == 0.5f,
          "unrelated remote volume edit does not block undo of track name");
}

void clipNoteAutomationReducerAndWire() {
    const std::string midiTrack = trackId("command-midi");
    const std::string automationTrack = trackId("command-automation");
    const std::string midiClip =
        testUuid("collaboration-test-clip", "midi-a");
    const std::string secondClip =
        testUuid("collaboration-test-clip", "midi-b");
    const std::string automationClip =
        testUuid("collaboration-test-clip", "automation");
    const std::string firstNote =
        testUuid("collaboration-test-note", "first");
    const std::string secondNote =
        testUuid("collaboration-test-note", "second");
    const std::string firstPoint =
        testUuid("collaboration-test-point", "first");
    const std::string secondPoint =
        testUuid("collaboration-test-point", "second");

    std::vector<ProjectCommand> setup{
        command("clip-note-track", AddTrack{midiTrack, TrackKind::Midi,
                                             "MIDI", 1, {}, {}}),
        command("automation-track",
                AddTrack{automationTrack, TrackKind::Automation,
                         "Automation", 2, {}, midiTrack}),
        command("midi-clip",
                AddClip{midiTrack, midiClip, ClipKind::Midi, "Phrase", 0.0,
                        8.0, 3, {}}),
        command("second-midi-clip",
                AddClip{midiTrack, secondClip, ClipKind::Midi, "Second", 8.0,
                        4.0, 4, midiClip}),
        command("automation-clip",
                AddClip{automationTrack, automationClip, ClipKind::Automation,
                        "Volume", 0.0, 8.0, 5, {}}),
    };

    SharedProjectDocument state;
    for (const ProjectCommand& item : setup)
        check(ProjectReducer::apply(state, item).changed(),
              "clip/note/automation setup command applies");

    const json clipWire = projectCommandToJson(setup[2]);
    std::string wireError;
    const std::string clipPrefix = "clip:" + midiClip + ":";
    const std::vector<std::string> completeClipAddFields{
        clipPrefix + "asset",
        clipPrefix + "automationActive",
        clipPrefix + "automationDefaultValue",
        clipPrefix + "automationTarget",
        clipPrefix + "color",
        clipPrefix + "compCrossfadeMs",
        clipPrefix + "descendants",
        clipPrefix + "durationSeconds",
        clipPrefix + "gain",
        clipPrefix + "lifecycle",
        clipPrefix + "muted",
        clipPrefix + "name",
        clipPrefix + "offsetSeconds",
        clipPrefix + "pan",
        clipPrefix + "position",
        clipPrefix + "sampleEdit",
        clipPrefix + "startSeconds",
        "track:" + midiTrack + ":clipLanding",
    };
    check(commandTouchedFields(setup[2]) == completeClipAddFields &&
              clipWire["touchedFields"] == json(completeClipAddFields) &&
              projectCommandFromJson(clipWire, &wireError).has_value(),
          "clip.add declares initialized mutable fields and round-trips");
    const std::string midiLandingHead =
        "track:" + midiTrack + ":clipLanding";
    check(commandTouchedFields(command(
              "delete-clip-shape", DeleteClip{midiTrack, midiClip})) ==
              std::vector<std::string>{
                  clipPrefix + "descendants", clipPrefix + "lifecycle",
                  midiLandingHead} &&
              commandTouchedFields(command(
                  "restore-clip-shape",
                  RestoreClip{midiTrack, midiClip,
                              operationId("delete-clip-shape")})) ==
                  std::vector<std::string>{
                      clipPrefix + "descendants", clipPrefix + "lifecycle",
                      clipPrefix + "position", midiLandingHead} &&
              commandTouchedFields(command(
                  "retime-clip-shape",
                  SetClipProperty{midiTrack, midiClip,
                                  ClipProperty::StartSeconds, 1.0})) ==
                  std::vector<std::string>{clipPrefix + "startSeconds",
                                           midiLandingHead} &&
              commandTouchedFields(command(
                  "asset-clip-shape",
                  SetClipAsset{midiTrack, midiClip, {}})) ==
                  std::vector<std::string>{clipPrefix + "asset",
                                           midiLandingHead} &&
              commandTouchedFields(command(
                  "sample-edit-clip-shape",
                  SetClipSampleEdit{midiTrack, midiClip, {}})) ==
                  std::vector<std::string>{clipPrefix + "sampleEdit",
                                           midiLandingHead},
          "clip landing-sensitive mutations advance the owning track head");
    json extraMetadata = clipWire;
    extraMetadata["actorId"] = testUuid("collaboration-test", "extra-actor");
    check(!projectCommandFromJson(extraMetadata, &wireError).has_value(),
          "locked project command rejects committed-envelope metadata");

    ProjectCommand move =
        command("move-second-clip", MoveClip{secondClip, midiTrack, {}});
    const std::vector<std::string> moveFields = commandTouchedFields(move);
    ApplyResult moved = ProjectReducer::apply(state, move);
    TrackModel* midi = state.project.findTrack(midiTrack);
    check(moved.changed() && midi->clips.front().id == secondClip &&
              moveFields == std::vector<std::string>{
                                "clip:" + secondClip + ":position",
                                "project:clipTrackAssignments",
                                "track:" + midiTrack + ":clipLanding"} &&
              moved.inverse &&
              moved.inverse->conditions.front().fieldKey ==
                  ProjectReducer::clipPositionKey(secondClip),
          "clip order uses a stable afterId anchor and conditional inverse");
    ProjectCommand undoMove = *moved.inverse;
    undoMove.meta = meta("undo-move-second-clip");
    check(ProjectReducer::apply(state, undoMove).changed() &&
              midi->clips[0].id == midiClip && midi->clips[1].id == secondClip,
          "clip move inverse restores its prior anchor");

    ApplyResult renamed = ProjectReducer::apply(
        state, command("rename-midi-clip",
                       SetClipProperty{midiTrack, midiClip,
                                       ClipProperty::Name,
                                       std::string("Edited Phrase")}));
    check(renamed.changed() && midi->clips[0].name == "Edited Phrase",
          "clip scalar property applies");
    ProjectCommand undoRename = *renamed.inverse;
    undoRename.meta = meta("undo-rename-midi-clip");
    check(ProjectReducer::apply(state, undoRename).changed() &&
              midi->clips[0].name == "Phrase",
          "clip property inverse is field-writer guarded");

    ProjectCommand setCrossfade = command(
        "set-clip-comp-crossfade",
        SetClipProperty{midiTrack, midiClip,
                        ClipProperty::CompCrossfadeMs, 7.25});
    const ApplyResult crossfadeChanged =
        ProjectReducer::apply(state, setCrossfade);
    const json crossfadeWire = projectCommandToJson(setCrossfade);
    check(crossfadeChanged.changed() &&
              std::fabs(midi->clips[0].compCrossfadeMs - 7.25) < 1e-9 &&
              commandTouchedFields(setCrossfade) ==
                  std::vector<std::string>{clipPrefix + "compCrossfadeMs"} &&
              crossfadeWire["payload"]["property"] == "compCrossfadeMs" &&
              projectCommandFromJson(crossfadeWire, &wireError).has_value(),
          "comp crossfade is a typed shared clip property with stable wire metadata");
    const std::string beforeBadCrossfade = stateFingerprint(state);
    check(!ProjectReducer::apply(
               state,
               command("reject-clip-comp-crossfade",
                       SetClipProperty{midiTrack, midiClip,
                                       ClipProperty::CompCrossfadeMs, 20.01}))
               .accepted() &&
              stateFingerprint(state) == beforeBadCrossfade,
          "comp crossfade outside zero to twenty milliseconds fails atomically");

    SharedProjectDocument guardedClipUndo;
    const std::string guardedTrack = trackId("guarded-clip-undo");
    const std::string guardedClip =
        testUuid("collaboration-test-clip", "guarded-undo");
    check(ProjectReducer::apply(
              guardedClipUndo,
              command("guarded-clip-track",
                      AddTrack{guardedTrack, TrackKind::Audio, "Track", 1,
                               {}, {}}))
              .changed(),
          "guarded clip undo fixture creates its track");
    ApplyResult guardedAdd = ProjectReducer::apply(
        guardedClipUndo,
        command("guarded-clip-add",
                AddClip{guardedTrack, guardedClip, ClipKind::Audio,
                        "Recorded", 0.0, 2.0, 2, {}}));
    check(guardedAdd.changed() &&
              ProjectReducer::apply(
                  guardedClipUndo,
                  command("guarded-clip-remote-name",
                          SetClipProperty{guardedTrack, guardedClip,
                                          ClipProperty::Name,
                                          std::string("Remote name")}))
                  .changed(),
          "newer clip field writer applies after clip.add");
    ProjectCommand guardedUndo = *guardedAdd.inverse;
    guardedUndo.meta = meta("guarded-clip-undo");
    check(ProjectReducer::apply(guardedClipUndo, guardedUndo).code ==
                  ApplyCode::PreconditionsFailed &&
              guardedClipUndo.project.findTrack(guardedTrack)
                      ->clips.front()
                      .name == "Remote name",
          "clip.add undo cannot delete a clip changed by another writer");

    SharedProjectDocument descendantClipUndo;
    const std::string descendantTrack = trackId("descendant-clip-undo");
    const std::string descendantClip =
        testUuid("collaboration-test-clip", "descendant-undo");
    const std::string descendantNote =
        testUuid("collaboration-test-note", "descendant-undo");
    check(ProjectReducer::apply(
              descendantClipUndo,
              command("descendant-clip-track",
                      AddTrack{descendantTrack, TrackKind::Midi, "MIDI", 1,
                               {}, {}}))
              .changed(),
          "descendant clip undo fixture creates its track");
    ApplyResult descendantAdd = ProjectReducer::apply(
        descendantClipUndo,
        command("descendant-clip-add",
                AddClip{descendantTrack, descendantClip, ClipKind::Midi,
                        "Phrase", 0.0, 2.0, 2, {}}));
    const ProjectCommand addDescendant = command(
        "descendant-note-add",
        UpsertMidiNote{descendantTrack, descendantClip,
                       midiNote(descendantNote, 60, 0.0), {}});
    check(descendantAdd.changed() &&
              ProjectReducer::apply(descendantClipUndo, addDescendant)
                  .changed() &&
              descendantClipUndo.lastWriterByField.at(
                  ProjectReducer::clipDescendantsKey(descendantClip)) ==
                  addDescendant.meta.operationId,
          "descendant mutation advances the owning clip collection head");
    ProjectCommand undoDescendantClip = *descendantAdd.inverse;
    undoDescendantClip.meta = meta("undo-descendant-clip");
    check(ProjectReducer::apply(descendantClipUndo, undoDescendantClip).code ==
                  ApplyCode::PreconditionsFailed &&
              descendantClipUndo.project.findTrack(descendantTrack)
                      ->clips.front()
                      .notes.size() == 1,
          "clip.add undo cannot delete a clip with a newer descendant");

    ApplyResult descendantDelete = ProjectReducer::apply(
        descendantClipUndo,
        command("descendant-clip-delete",
                DeleteClip{descendantTrack, descendantClip}));
    const std::string descendantKey =
        ProjectReducer::clipDescendantsKey(descendantClip);
    const bool deleteUndoGuardsDescendants =
        descendantDelete.changed() && descendantDelete.inverse &&
        std::any_of(descendantDelete.inverse->conditions.begin(),
                    descendantDelete.inverse->conditions.end(),
                    [&](const FieldWriterIs& condition) {
                        return condition.fieldKey == descendantKey &&
                               condition.operationId ==
                                   command("descendant-clip-delete", DeleteClip{
                                       descendantTrack, descendantClip})
                                       .meta.operationId;
                    });
    check(deleteUndoGuardsDescendants,
          "clip.delete undo guards the complete descendant collection");

    ProjectCommand restoreDescendantClip = *descendantDelete.inverse;
    restoreDescendantClip.meta = meta("descendant-clip-restore");
    ApplyResult descendantRestored =
        ProjectReducer::apply(descendantClipUndo, restoreDescendantClip);
    const std::string lateDescendantNote =
        testUuid("collaboration-test-note", "late-descendant-after-restore");
    check(descendantRestored.changed() && descendantRestored.inverse &&
              ProjectReducer::apply(
                  descendantClipUndo,
                  command("late-descendant-after-restore",
                          UpsertMidiNote{
                              descendantTrack, descendantClip,
                              midiNote(lateDescendantNote, 67, 1.0),
                              descendantNote}))
                  .changed(),
          "restored clip accepts a later descendant edit");
    ProjectCommand undoDescendantRestore = *descendantRestored.inverse;
    undoDescendantRestore.meta = meta("undo-descendant-clip-restore");
    check(ProjectReducer::apply(descendantClipUndo, undoDescendantRestore).code ==
                  ApplyCode::PreconditionsFailed &&
              descendantClipUndo.project.findTrack(descendantTrack)
                      ->clips.front()
                      .notes.size() == 2,
          "clip.restore undo cannot delete a later descendant edit");

    const NoteModel noteA = midiNote(firstNote, 60, 0.0);
    const NoteModel noteB = midiNote(secondNote, 64, 2.0);
    ProjectCommand insertNoteA = command(
        "insert-note-a", UpsertMidiNote{midiTrack, midiClip, noteA, {}});
    ProjectCommand insertNoteB = command(
        "insert-note-b",
        UpsertMidiNote{midiTrack, midiClip, noteB, firstNote});
    ApplyResult insertedA = ProjectReducer::apply(state, insertNoteA);
    ApplyResult insertedB = ProjectReducer::apply(state, insertNoteB);
    const std::vector<std::string> noteFields =
        commandTouchedFields(insertNoteA);
    check(insertedA.changed() && insertedB.changed() &&
              midi->clips[0].notes.size() == 2 && noteFields.size() == 9 &&
              std::find(noteFields.begin(), noteFields.end(),
                        ProjectReducer::notePositionKey(firstNote)) !=
                  noteFields.end() &&
              std::find(noteFields.begin(), noteFields.end(),
                        ProjectReducer::clipDescendantsKey(midiClip)) !=
                  noteFields.end() &&
              std::find(noteFields.begin(), noteFields.end(),
                        ProjectReducer::noteLifecycleKey(firstNote)) ==
                  noteFields.end(),
          "MIDI note upsert advances granular content and position heads");

    NoteModel changedNote = noteA;
    changedNote.pitch = 67;
    ApplyResult noteUpdate = ProjectReducer::apply(
        state, command("update-note-a",
                       UpsertMidiNote{midiTrack, midiClip, changedNote,
                                      secondNote}));
    check(noteUpdate.changed() && midi->clips[0].notes.back().pitch == 67,
          "MIDI note upsert updates data and order by afterId");
    NoteModel changedSibling = noteB;
    changedSibling.pitch = 72;
    check(ProjectReducer::apply(
              state, command("update-note-b-sibling",
                             UpsertMidiNote{midiTrack, midiClip,
                                            changedSibling, {}}))
              .changed(),
          "unrelated sibling note advances the coarse clip revision");
    ProjectCommand undoNoteUpdate = *noteUpdate.inverse;
    undoNoteUpdate.meta = meta("undo-update-note-a");
    check(ProjectReducer::apply(state, undoNoteUpdate).changed() &&
              midi->clips[0].notes.front() == noteA &&
              midi->clips[0].notes.back() == changedSibling,
          "MIDI note undo ignores unrelated sibling descendant revisions");

    ApplyResult deletedNote = ProjectReducer::apply(
        state, command("delete-note-a",
                       DeleteMidiNote{midiTrack, midiClip, firstNote}));
    const json noteRestoreWire = projectCommandToJson(*deletedNote.inverse);
    check(deletedNote.changed() && state.deletedNotes.contains(firstNote) &&
              noteRestoreWire["preconditions"][0].value("kind", "") ==
                  "fieldWriterIs" &&
              noteRestoreWire["preconditions"][0].value("fieldKey", "") ==
                  ProjectReducer::noteLifecycleKey(firstNote),
          "MIDI note delete produces a generic lifecycle-guarded restore");
    check(ProjectReducer::apply(
              state, command("late-note-a",
                             UpsertMidiNote{midiTrack, midiClip, changedNote,
                                            secondNote}))
              .code == ApplyCode::DeletedEntity,
          "MIDI note delete wins and rejects a late upsert");
    ProjectCommand restoreNote = *deletedNote.inverse;
    restoreNote.meta = meta("restore-note-a");
    check(ProjectReducer::apply(state, restoreNote).changed() &&
              midi->clips[0].notes.front() == noteA,
          "MIDI note conditional restore resurrects the tombstone");

    const AutomationPoint pointA =
        automationPoint(firstPoint, 0.0, 0.25);
    const AutomationPoint pointB = automationPoint(
        secondPoint, 4.0, 0.75, AutomationSegment::SCurve);
    ProjectCommand insertPointA = command(
        "insert-point-a",
        UpsertAutomationPoint{automationTrack, automationClip, {}, pointA, {}});
    ProjectCommand insertPointB = command(
        "insert-point-b", UpsertAutomationPoint{
                              automationTrack, automationClip, {}, pointB,
                              firstPoint});
    check(ProjectReducer::apply(state, insertPointA).changed() &&
              ProjectReducer::apply(state, insertPointB).changed(),
          "automation point upserts preserve chronological afterId order");
    TrackModel* automation = state.project.findTrack(automationTrack);
    ClipModel& curve = automation->clips.front();
    check(curve.automation.points.size() == 2 &&
              curve.automation.points[1] == pointB &&
              state.lastWriterByField.at(
                  ProjectReducer::clipDescendantsKey(automationClip)) ==
                  insertPointB.meta.operationId &&
              projectCommandFromJson(projectCommandToJson(insertPointB),
                                     &wireError)
                  .has_value(),
          "automation point command round-trips with stable point identity");

    AutomationPoint changedPoint = pointB;
    changedPoint.value = 0.5;
    ApplyResult pointUpdate = ProjectReducer::apply(
        state, command("update-point-b", UpsertAutomationPoint{
                   automationTrack, automationClip, {}, changedPoint,
                   firstPoint}));
    AutomationPoint remotePoint = changedPoint;
    remotePoint.value = 0.9;
    check(ProjectReducer::apply(
              state, command("remote-point-b", UpsertAutomationPoint{
                         automationTrack, automationClip, {}, remotePoint,
                         firstPoint}))
              .changed(),
          "newer automation writer applies");
    ProjectCommand conflictingPointUndo = *pointUpdate.inverse;
    conflictingPointUndo.meta = meta("conflicting-point-undo");
    check(ProjectReducer::apply(state, conflictingPointUndo).code ==
              ApplyCode::PreconditionsFailed &&
              curve.automation.points[1].value == 0.9,
          "automation conditional inverse cannot clobber a newer writer");

    ApplyResult deletedPoint = ProjectReducer::apply(
        state, command("delete-point-a", DeleteAutomationPoint{
                   automationTrack, automationClip, {}, firstPoint}));
    check(deletedPoint.changed() &&
              state.deletedAutomationPoints.contains(firstPoint),
          "automation point delete stores a tombstone");
    check(ProjectReducer::apply(
              state, command("late-point-a", UpsertAutomationPoint{
                         automationTrack, automationClip, {}, pointA, {}}))
              .code == ApplyCode::DeletedEntity,
          "automation point delete wins and rejects a late upsert");
    ProjectCommand restorePoint = *deletedPoint.inverse;
    restorePoint.meta = meta("restore-point-a");
    check(ProjectReducer::apply(state, restorePoint).changed() &&
              curve.automation.points.front() == pointA,
          "automation point restore preserves chronological ordering");

    const std::string laneId =
        testUuid("collaboration-test-controller-lane", "modulation");
    const std::string lanePointId =
        testUuid("collaboration-test-point", "lane");
    ControllerLane lane;
    lane.id = laneId;
    midi->clips[0].lanes.push_back(lane);
    check(ProjectReducer::apply(
              state, command("lane-point", UpsertAutomationPoint{
                         midiTrack, midiClip, laneId,
                         automationPoint(lanePointId, 1.0, 0.4), {}}))
              .changed() &&
              midi->clips[0].lanes.front().points.front().id == lanePointId,
          "automation point commands address stable MIDI controller lane ids");

    ApplyResult deletedClip = ProjectReducer::apply(
        state,
        command("delete-midi-clip", DeleteClip{midiTrack, midiClip}));
    check(deletedClip.changed() && state.deletedClips.contains(midiClip),
          "clip delete retains the full clip in a tombstone");
    check(ProjectReducer::apply(
              state, command("late-clip-name",
                             SetClipProperty{midiTrack, midiClip,
                                             ClipProperty::Name,
                                             std::string("Too late")}))
                  .code == ApplyCode::DeletedEntity &&
              ProjectReducer::apply(
                  state, command("late-note-in-deleted-clip",
                                 UpsertMidiNote{midiTrack, midiClip, noteA, {}}))
                  .code == ApplyCode::DeletedEntity,
          "clip delete wins and rejects late clip and child mutations");
    ProjectCommand restoreClip = *deletedClip.inverse;
    restoreClip.meta = meta("restore-midi-clip");
    check(ProjectReducer::apply(state, restoreClip).changed() &&
              midi->clips.front().id == midiClip &&
              midi->clips.front().notes.front() == noteA,
          "clip restore resurrects its notes and automation intact");

    SharedProjectDocument replayLeft;
    SharedProjectDocument replayRight;
    std::vector<ProjectCommand> replay = setup;
    replay.push_back(insertNoteA);
    replay.push_back(insertNoteB);
    replay.push_back(insertPointA);
    replay.push_back(insertPointB);
    for (const ProjectCommand& item : replay) {
        check(ProjectReducer::apply(replayLeft, item).accepted() &&
                  ProjectReducer::apply(replayRight, item).accepted(),
              "extended reducer accepts deterministic replay item");
    }
    check(stateFingerprint(replayLeft) == stateFingerprint(replayRight),
          "clip/note/automation replay converges byte-for-byte");

    constexpr int kBatchNoteCount = 32;
    const std::string batchTrack = trackId("multi-note-batch");
    const std::string batchClip =
        testUuid("collaboration-test-clip", "multi-note-batch");
    SharedProjectDocument batchLeft;
    SharedProjectDocument batchRight;
    const std::vector<ProjectCommand> batchSetup{
        command("multi-note-track",
                AddTrack{batchTrack, TrackKind::Midi, "Batch", 1, {}, {}}),
        command("multi-note-clip",
                AddClip{batchTrack, batchClip, ClipKind::Midi, "Batch", 0.0,
                        16.0, 1, {}}),
    };
    for (const ProjectCommand& item : batchSetup) {
        ProjectReducer::apply(batchLeft, item);
        ProjectReducer::apply(batchRight, item);
    }
    auto multiNoteBody = std::make_shared<BatchCommand>();
    std::string afterNote;
    for (int index = 0; index < kBatchNoteCount; ++index) {
        const std::string id = testUuid("collaboration-test-batch-note",
                                        std::to_string(index));
        multiNoteBody->commands.push_back(command(
            {}, UpsertMidiNote{batchTrack, batchClip,
                               midiNote(id, 48 + (index % 24), index * 0.25,
                                        0.2, 80 + (index % 40)),
                               afterNote}));
        afterNote = id;
    }
    ProjectCommand multiNote = command("multi-note-batch", multiNoteBody);
    const std::vector<std::string> multiNoteFields =
        commandTouchedFields(multiNote);
    const json multiNoteWire = projectCommandToJson(multiNote);
    ApplyResult multiLeft = ProjectReducer::apply(batchLeft, multiNote);
    ApplyResult multiRight = ProjectReducer::apply(batchRight, multiNote);
    const std::string multiNoteOp = operationId("multi-note-batch");
    const bool allWritersUseOuter = std::all_of(
        multiNoteFields.begin(), multiNoteFields.end(),
        [&](const std::string& field) {
            const auto found = batchLeft.lastWriterByField.find(field);
            return found != batchLeft.lastWriterByField.end() &&
                   found->second == multiNoteOp;
        });
    check(multiLeft.changed() && multiRight.changed() &&
              multiNoteFields.size() == kBatchNoteCount * 8 + 1 &&
              multiNoteWire["touchedFields"].size() == multiNoteFields.size() &&
              projectCommandFromJson(multiNoteWire, &wireError).has_value() &&
              allWritersUseOuter &&
              stateFingerprint(batchLeft) == stateFingerprint(batchRight),
          "moderate atomic MIDI batch converges beyond the legacy 64-field cap");
    ProjectCommand undoMultiNote = *multiLeft.inverse;
    undoMultiNote.meta = meta("undo-multi-note-batch");
    check(ProjectReducer::apply(batchLeft, undoMultiNote).changed() &&
              batchLeft.project.findTrack(batchTrack)->clips.front().notes.empty(),
          "multi-note batch conditional inverse applies atomically");
}

void laneTakeCompReducerAndRecordingBatch() {
    const std::string midiTrack = trackId("lane-phase-midi");
    const std::string automationTrack = trackId("lane-phase-automation");
    const std::string audioTrack = trackId("recording-phase-audio");
    const std::string midiClip =
        testUuid("collaboration-test-clip", "lane-phase-midi");
    const std::string automationClip =
        testUuid("collaboration-test-clip", "lane-phase-automation");
    const std::string audioClip =
        testUuid("collaboration-test-clip", "recording-phase-audio");
    const std::string laneId =
        testUuid("collaboration-test-controller-lane", "phase-lane");
    const std::string secondLaneId =
        testUuid("collaboration-test-controller-lane", "phase-lane-2");

    const std::vector<ProjectCommand> setup{
        command("lane-phase-midi-track",
                AddTrack{midiTrack, TrackKind::Midi, "MIDI", 1, {}, {}}),
        command("lane-phase-automation-track",
                AddTrack{automationTrack, TrackKind::Automation,
                         "Automation", 2, {}, midiTrack}),
        command("recording-phase-audio-track",
                AddTrack{audioTrack, TrackKind::Audio, "Audio", 3, {},
                         automationTrack}),
        command("lane-phase-midi-clip",
                AddClip{midiTrack, midiClip, ClipKind::Midi, "MIDI", 0.0,
                        8.0, 1, {}}),
        command("lane-phase-automation-clip",
                AddClip{automationTrack, automationClip,
                        ClipKind::Automation, "Automation", 0.0, 8.0, 2,
                        {}}),
        command("recording-phase-audio-clip",
                AddClip{audioTrack, audioClip, ClipKind::Audio, "Recorded",
                        0.0, 8.0, 3, {}}),
    };
    SharedProjectDocument state;
    for (const ProjectCommand& item : setup)
        check(ProjectReducer::apply(state, item).changed(),
              "lane/take/comp setup command applies");

    ProjectCommand addLane = command(
        "controller-lane-add",
        AddControllerLane{midiTrack, midiClip, laneId, "Mod Wheel",
                          ControllerLaneTarget{1, {}, {}}, 0.25, {}});
    ApplyResult laneAdded = ProjectReducer::apply(state, addLane);
    ProjectCommand addSecondLane = command(
        "controller-lane-add-second",
        AddControllerLane{midiTrack, midiClip, secondLaneId, "Cutoff",
                          ControllerLaneTarget{-1, "cutoff", {}}, 0.5,
                          laneId});
    check(laneAdded.changed() &&
              ProjectReducer::apply(state, addSecondLane).changed() &&
              state.project.findTrack(midiTrack)->clips.front().lanes[1].id ==
                  secondLaneId &&
              state.lastWriterByField.at(
                  ProjectReducer::clipDescendantsKey(midiClip)) ==
                  addSecondLane.meta.operationId &&
              commandTouchedFields(addLane) ==
                  std::vector<std::string>({
                      ProjectReducer::clipDescendantsKey(midiClip),
                      ProjectReducer::controllerLaneLifecycleKey(laneId),
                      ProjectReducer::controllerLanePositionKey(laneId)}),
          "controller lane add uses stable afterId and lifecycle fields");
    std::string wireError;
    const json laneWire = projectCommandToJson(addSecondLane);
    check(projectCommandFromJson(laneWire, &wireError).has_value() &&
              laneWire["payload"]["target"].value("parameterId", "") ==
                  "cutoff",
          "controller lane command round-trips through locked JSON");

    ApplyResult laneTarget = ProjectReducer::apply(
        state, command("controller-lane-target",
                       SetControllerLaneTarget{
                           midiTrack, midiClip, laneId,
                           ControllerLaneTarget{-1, "resonance", {}}}));
    ApplyResult laneDefault = ProjectReducer::apply(
        state, command("controller-lane-default",
                       SetControllerLaneDefault{midiTrack, midiClip, laneId,
                                                0.75}));
    ProjectCommand undoLaneTarget = *laneTarget.inverse;
    undoLaneTarget.meta = meta("undo-controller-lane-target");
    check(laneTarget.changed() && laneDefault.changed() &&
              ProjectReducer::apply(state, undoLaneTarget).changed() &&
              state.project.findTrack(midiTrack)->clips.front().lanes.front()
                      .cc == 1 &&
              state.project.findTrack(midiTrack)->clips.front().lanes.front()
                      .defaultValue == 0.75,
          "lane target undo is independent from a newer default writer");

    const std::string lanePointId =
        testUuid("collaboration-test-point", "phase-lane-point");
    const AutomationPoint lanePoint =
        automationPoint(lanePointId, 1.0, 0.6);
    check(ProjectReducer::apply(
              state, command("controller-lane-point",
                             UpsertAutomationPoint{midiTrack, midiClip, laneId,
                                                   lanePoint, {}}))
              .changed(),
          "controller lane accepts stable-id automation points");

    ApplyResult laneDeleted = ProjectReducer::apply(
        state, command("controller-lane-delete",
                       DeleteControllerLane{midiTrack, midiClip, laneId}));
    check(laneDeleted.changed() &&
              ProjectReducer::apply(
                  state, command("controller-lane-late-default",
                                 SetControllerLaneDefault{midiTrack, midiClip,
                                                          laneId, 0.1}))
                      .code == ApplyCode::DeletedEntity &&
              ProjectReducer::apply(
                  state, command("controller-lane-late-point",
                                 UpsertAutomationPoint{
                                     midiTrack, midiClip, laneId, lanePoint,
                                     {}}))
                      .code == ApplyCode::DeletedEntity,
          "controller lane delete wins over late property and point edits");
    ProjectCommand restoreLane = *laneDeleted.inverse;
    restoreLane.meta = meta("controller-lane-restore");
    check(ProjectReducer::apply(state, restoreLane).changed() &&
              state.project.findTrack(midiTrack)->clips.front().lanes.front()
                      .id == laneId &&
              state.project.findTrack(midiTrack)
                      ->clips.front()
                      .lanes.front()
                      .points.front() == lanePoint,
          "controller lane restore recovers the full lane and anchor");

    AutomationTarget primaryTarget;
    primaryTarget.kind = AutomationTargetKind::TrackVolume;
    primaryTarget.channelId = audioTrack;
    ApplyResult automationTarget = ProjectReducer::apply(
        state, command("automation-target",
                       SetAutomationTarget{automationTrack, automationClip,
                                           primaryTarget}));
    ApplyResult automationDefault = ProjectReducer::apply(
        state, command("automation-default",
                       SetAutomationDefault{automationTrack, automationClip,
                                            0.4}));
    ApplyResult automationActive = ProjectReducer::apply(
        state, command("automation-active",
                       SetAutomationActive{automationTrack, automationClip,
                                           true}));
    check(automationTarget.changed() && automationDefault.changed() &&
              automationActive.changed() &&
              state.project.findTrack(automationTrack)
                  ->clips.front()
                  .automation.active &&
              commandTouchedFields(command(
                  "automation-active-shape",
                  SetAutomationActive{automationTrack, automationClip, true})) ==
                  std::vector<std::string>({
                      "clip:" + automationClip + ":automationActive"}),
          "primary automation target/default/active use separate field heads");

    TakeModel take;
    take.id = testUuid("collaboration-test-take", "recording");
    take.name = "Take 1";
    take.lengthSeconds = 4.0;
    take.gain = 1.0f;
    take.channels = 2;
    take.asset.assetId =
        testUuid("collaboration-test-asset", "recording");
    take.asset.sha256 = std::string(64, 'a');
    take.asset.kind = AssetKind::Audio;
    take.asset.byteSize = 384000;
    take.asset.originalName = "take-1.wav";
    take.asset.mimeType = "audio/wav";
    take.asset.codec = "pcm_s24le";
    take.asset.sampleRate = 48000.0;
    take.asset.channels = 2;
    take.asset.frames = 192000;
    CompSegment segment;
    segment.id =
        testUuid("collaboration-test-comp-segment", "recording");
    segment.takeId = take.id;
    segment.startSeconds = 0.0;
    segment.endSeconds = 4.0;

    auto recordingBody = std::make_shared<BatchCommand>();
    recordingBody->commands.push_back(
        command({}, AddTake{audioTrack, audioClip, take, {}}));
    recordingBody->commands.push_back(command(
        {}, UpsertCompSegment{audioTrack, audioClip, segment, {}}));
    const std::string recordingLease =
        testUuid("collaboration-test-recording-lease", "recording");
    ProjectCommand recordingCommit =
        command("recording-commit",
                RecordingCommit{{RecordingLeaseClaim{audioTrack,
                                                      recordingLease}},
                                recordingBody});
    const json recordingWire = projectCommandToJson(recordingCommit);
    const json& takeWire =
        recordingWire["payload"]["commands"][0]["payload"]["take"];
    auto parsedRecording = projectCommandFromJson(recordingWire, &wireError);
    check(recordingWire.value("kind", "") == "recording.commit" &&
              recordingWire["payload"]["leases"].size() == 1 &&
              recordingWire["payload"]["leases"][0].value("trackId", "") ==
                  audioTrack &&
              recordingWire["payload"]["leases"][0].value("leaseId", "") ==
                  recordingLease &&
              !takeWire.contains("filePath") && !takeWire.contains("file") &&
              takeWire["asset"].value("assetId", "") == take.asset.assetId &&
              takeWire["asset"].value("sha256", "") == take.asset.sha256 &&
              takeWire["asset"].value("byteSize", 0) > 0 &&
              takeWire["asset"]["audioMetadata"].value("sampleRate", 0.0) ==
                  48000.0 &&
              parsedRecording.has_value() &&
              serializeProjectCommand(*parsedRecording) ==
                  recordingWire.dump(),
          "recording.commit has canonical leases, commands and AssetRef wire");
    json forgedRecordingWire = recordingWire;
    forgedRecordingWire["payload"]["commands"][0]["payload"]["take"]
                       ["filePath"] = "/tmp/not-wire-data.wav";
    check(!projectCommandFromJson(forgedRecordingWire, &wireError).has_value(),
          "locked recording parser rejects a forged local file path");

    json mismatchedRecordingTrack = recordingWire;
    mismatchedRecordingTrack["payload"]["leases"][0]["trackId"] =
        trackId("recording-mismatch");
    check(!projectCommandFromJson(mismatchedRecordingTrack, &wireError)
               .has_value(),
          "recording.commit rejects a lease/command track mismatch");
    json duplicateRecordingTrack = recordingWire;
    duplicateRecordingTrack["payload"]["leases"].push_back(
        json{{"trackId", audioTrack},
             {"leaseId", testUuid("collaboration-test-recording-lease",
                                  "duplicate-track")}});
    check(!projectCommandFromJson(duplicateRecordingTrack, &wireError)
               .has_value(),
          "recording.commit rejects duplicate leased tracks");
    json duplicateRecordingLease = recordingWire;
    duplicateRecordingLease["payload"]["leases"].push_back(
        json{{"trackId", trackId("duplicate-recording-lease")},
             {"leaseId", recordingLease}});
    check(!projectCommandFromJson(duplicateRecordingLease, &wireError)
               .has_value(),
          "recording.commit rejects duplicate lease ids");
    json emptyRecordingCommands = recordingWire;
    emptyRecordingCommands["payload"]["commands"] = json::array();
    emptyRecordingCommands["touchedFields"] = json::array();
    check(!projectCommandFromJson(emptyRecordingCommands, &wireError)
               .has_value(),
          "recording.commit rejects an empty document transaction");
    json nestedRecordingCommit = recordingWire;
    nestedRecordingCommit["payload"]["commands"][0]["kind"] =
        "recording.commit";
    nestedRecordingCommit["payload"]["commands"][0]["payload"] =
        recordingWire["payload"];
    check(!projectCommandFromJson(nestedRecordingCommit, &wireError)
               .has_value(),
          "recording.commit rejects a nested recording.commit");
    auto wrappingBatchBody = std::make_shared<BatchCommand>();
    wrappingBatchBody->commands.push_back(recordingCommit);
    ProjectCommand wrappingBatch =
        command("batch-wrapping-recording", wrappingBatchBody);
    SharedProjectDocument wrappingBatchState = state;
    check(ProjectReducer::apply(wrappingBatchState, wrappingBatch).code ==
                  ApplyCode::InvalidCommand &&
              stateFingerprint(wrappingBatchState) == stateFingerprint(state),
          "ordinary batch rejects a nested recording.commit");

    auto smuggledRecordingBody = std::make_shared<BatchCommand>();
    smuggledRecordingBody->commands.push_back(
        command({}, AddTake{audioTrack, audioClip, take, {}}));
    smuggledRecordingBody->commands.push_back(command(
        {}, SetTrackProperty{audioTrack, TrackProperty::Muted, true}));
    ProjectCommand smuggledRecording = command(
        "recording-smuggled-track-edit",
        RecordingCommit{
            {RecordingLeaseClaim{
                audioTrack,
                testUuid("collaboration-test-recording-lease", "smuggled")}},
            smuggledRecordingBody});
    SharedProjectDocument smuggledRecordingState = state;
    const json smuggledRecordingWire =
        projectCommandToJson(smuggledRecording);
    check(ProjectReducer::apply(smuggledRecordingState,
                                smuggledRecording)
                  .code == ApplyCode::InvalidCommand &&
              !projectCommandFromJson(smuggledRecordingWire, &wireError)
                   .has_value() &&
              stateFingerprint(smuggledRecordingState) ==
                  stateFingerprint(state),
          "recording.commit rejects an unrelated track edit smuggled in its batch");

    const std::string missingRecordingTrack =
        trackId("recording-atomic-missing");
    const std::string missingRecordingClip =
        testUuid("collaboration-test-clip", "recording-atomic-missing");
    auto rejectedRecordingBody = std::make_shared<BatchCommand>();
    rejectedRecordingBody->commands.push_back(
        command({}, AddTake{audioTrack, audioClip, take, {}}));
    rejectedRecordingBody->commands.push_back(command(
        {}, AddClip{missingRecordingTrack, missingRecordingClip,
                    ClipKind::Audio, "Missing track", 0.0, 1.0, 1, {}}));
    ProjectCommand rejectedRecording = command(
        "recording-atomic-reject",
        RecordingCommit{
            {RecordingLeaseClaim{
                 audioTrack,
                 testUuid("collaboration-test-recording-lease", "atomic-a")},
             RecordingLeaseClaim{
                 missingRecordingTrack,
                 testUuid("collaboration-test-recording-lease", "atomic-b")}},
            rejectedRecordingBody});
    SharedProjectDocument rejectedRecordingState = state;
    const std::string beforeRejectedRecording =
        stateFingerprint(rejectedRecordingState);
    check(!ProjectReducer::apply(rejectedRecordingState, rejectedRecording)
               .accepted() &&
              stateFingerprint(rejectedRecordingState) ==
                  beforeRejectedRecording,
          "rejected recording.commit leaves the document atomically unchanged");

    const std::string simpleRecordingClip =
        testUuid("collaboration-test-clip", "simple-recording-commit");
    const AssetRef simpleRecordingAsset =
        testAsset("simple-recording-commit", AssetKind::Audio, 'd');
    auto simpleRecordingBody = std::make_shared<BatchCommand>();
    simpleRecordingBody->commands.push_back(command(
        {}, AddClip{audioTrack, simpleRecordingClip, ClipKind::Audio,
                    "Simple recording", 8.0, 2.0, 3, audioClip}));
    simpleRecordingBody->commands.push_back(command(
        {}, SetClipProperty{audioTrack, simpleRecordingClip,
                            ClipProperty::Gain, 0.9}));
    simpleRecordingBody->commands.push_back(command(
        {}, SetClipAsset{audioTrack, simpleRecordingClip,
                         simpleRecordingAsset}));
    ProjectCommand simpleRecordingCommit = command(
        "simple-recording-commit",
        RecordingCommit{
            {RecordingLeaseClaim{
                audioTrack,
                testUuid("collaboration-test-recording-lease", "simple")}},
            simpleRecordingBody});
    SharedProjectDocument simpleRecordingState = state;
    ApplyResult simpleRecorded =
        ProjectReducer::apply(simpleRecordingState, simpleRecordingCommit);
    const ClipModel& simpleRecordedClip =
        simpleRecordingState.project.findTrack(audioTrack)->clips.back();
    check(simpleRecorded.changed() && simpleRecorded.inverse &&
              simpleRecordedClip.id == simpleRecordingClip &&
              simpleRecordedClip.asset == simpleRecordingAsset &&
              simpleRecordedClip.gain == 0.9f &&
              projectCommandFromJson(
                  projectCommandToJson(simpleRecordingCommit), &wireError)
                  .has_value(),
          "recording.commit accepts the v1 clip.add/property/asset landing path");
    ProjectCommand undoSimpleRecording = *simpleRecorded.inverse;
    undoSimpleRecording.meta = meta("undo-simple-recording-commit");
    check(ProjectReducer::apply(simpleRecordingState, undoSimpleRecording)
                  .changed() &&
              simpleRecordingState.project.findTrack(audioTrack)
                      ->clips.size() == 1,
          "simple recording.commit has one atomic lease-free inverse");

    SharedProjectDocument recordingLeft = state;
    SharedProjectDocument recordingRight = state;
    ApplyResult recordedLeft =
        ProjectReducer::apply(recordingLeft, recordingCommit);
    ApplyResult recordedRight =
        ProjectReducer::apply(recordingRight, recordingCommit);
    const std::vector<std::string> recordingFields =
        commandTouchedFields(recordingCommit);
    const bool recordingWritersUseOuter = std::all_of(
        recordingFields.begin(), recordingFields.end(),
        [&](const std::string& field) {
            return recordingLeft.lastWriterByField.at(field) ==
                   operationId("recording-commit");
        });
    check(recordedLeft.changed() && recordedRight.changed() &&
              recordingFields.size() == 8 && recordingWritersUseOuter &&
              recordedLeft.inverse &&
              commandKind(*recordedLeft.inverse) == "batch" &&
              recordingLeft.project.findTrack(audioTrack)
                      ->clips.front()
                      .takes.size() == 1 &&
              recordingLeft.project.findTrack(audioTrack)
                      ->clips.front()
                      .comp.size() == 1 &&
              stateFingerprint(recordingLeft) ==
                  stateFingerprint(recordingRight),
          "atomic recording.commit converges and produces a lease-free inverse");

    SharedProjectDocument undoRecording = recordingLeft;
    ProjectCommand recordingInverse = *recordedLeft.inverse;
    recordingInverse.meta = meta("undo-recording-commit");
    check(ProjectReducer::apply(undoRecording, recordingInverse).changed() &&
              undoRecording.project.findTrack(audioTrack)
                  ->clips.front()
                  .takes.empty() &&
              undoRecording.project.findTrack(audioTrack)
                  ->clips.front()
                  .comp.empty(),
          "recording batch inverse deletes comp before its referenced take");

    CompSegment remoteSegment = segment;
    remoteSegment.endSeconds = 5.0;
    check(ProjectReducer::apply(
              recordingLeft,
              command("remote-comp-edit",
                      UpsertCompSegment{audioTrack, audioClip, remoteSegment,
                                        {}}))
              .changed(),
          "newer comp segment writer applies");
    ProjectCommand conflictingRecordingUndo = *recordedLeft.inverse;
    conflictingRecordingUndo.meta = meta("conflicting-recording-undo");
    check(ProjectReducer::apply(recordingLeft, conflictingRecordingUndo).code ==
              ApplyCode::PreconditionsFailed &&
              recordingLeft.project.findTrack(audioTrack)
                      ->clips.front()
                      .comp.front()
                      .endSeconds == 5.0,
          "recording conditional undo cannot clobber a newer comp writer");

    SharedProjectDocument lifecycle = recordingRight;
    ApplyResult danglingTakeDelete = ProjectReducer::apply(
        lifecycle,
        command("take-delete-dangling",
                DeleteTake{audioTrack, audioClip, take.id}));
    const ClipModel& danglingClip =
        lifecycle.project.findTrack(audioTrack)->clips.front();
    check(danglingTakeDelete.changed() && danglingClip.takes.empty() &&
              danglingClip.comp.size() == 1 &&
              danglingClip.comp.front().takeId == take.id &&
              ProjectReducer::apply(
                  lifecycle,
                  command("late-comp-with-deleted-take",
                          UpsertCompSegment{audioTrack, audioClip, segment,
                                            {}}))
                      .code == ApplyCode::DeletedEntity,
          "take delete retains a dangling comp ref and blocks later targeting");
    ProjectCommand reviveDanglingTake = *danglingTakeDelete.inverse;
    reviveDanglingTake.meta = meta("take-restore-dangling");
    check(ProjectReducer::apply(lifecycle, reviveDanglingTake).changed() &&
              lifecycle.project.findTrack(audioTrack)
                      ->clips.front()
                      .comp.front()
                      .takeId == take.id,
          "take restore revives the unchanged dangling comp reference");
    ApplyResult compDeleted = ProjectReducer::apply(
        lifecycle,
        command("comp-delete", DeleteCompSegment{audioTrack, audioClip,
                                                  segment.id}));
    check(compDeleted.changed() &&
              ProjectReducer::apply(
                  lifecycle,
                  command("late-comp-upsert",
                          UpsertCompSegment{audioTrack, audioClip, segment,
                                            {}}))
                      .code == ApplyCode::DeletedEntity,
          "comp segment delete wins over a late upsert");
    ProjectCommand restoreComp = *compDeleted.inverse;
    restoreComp.meta = meta("comp-restore");
    check(ProjectReducer::apply(lifecycle, restoreComp).changed(),
          "comp segment restore recovers its stable anchor");

    ApplyResult compDeletedForTake = ProjectReducer::apply(
        lifecycle,
        command("comp-delete-for-take",
                DeleteCompSegment{audioTrack, audioClip, segment.id}));
    ApplyResult takeDeleted = ProjectReducer::apply(
        lifecycle,
        command("take-delete", DeleteTake{audioTrack, audioClip, take.id}));
    check(compDeletedForTake.changed() && takeDeleted.changed() &&
              ProjectReducer::apply(
                  lifecycle,
                  command("late-take-add",
                          AddTake{audioTrack, audioClip, take, {}}))
                      .code == ApplyCode::DeletedEntity,
          "take delete wins and does not mutate undeclared comp fields");
    ProjectCommand restoreTake = *takeDeleted.inverse;
    restoreTake.meta = meta("take-restore");
    ProjectCommand restoreCompAfterTake = *compDeletedForTake.inverse;
    restoreCompAfterTake.meta = meta("comp-restore-after-take");
    check(ProjectReducer::apply(lifecycle, restoreTake).changed() &&
              ProjectReducer::apply(lifecycle, restoreCompAfterTake).changed() &&
              lifecycle.project.findTrack(audioTrack)
                      ->clips.front()
                      .takes.front()
                      .asset == take.asset,
          "take and comp conditional restores preserve the full AssetRef");

    TakeModel unsafeTake = take;
    unsafeTake.id = testUuid("collaboration-test-take", "unsafe-local-path");
    unsafeTake.filePath = "/tmp/local-only.wav";
    check(ProjectReducer::apply(
              state, command("unsafe-local-take",
                             AddTake{audioTrack, audioClip, unsafeTake, {}}))
                  .code == ApplyCode::InvalidCommand,
          "pure reducer rejects local paths in collaborative take commands");
    TakeModel uppercaseHashTake = take;
    uppercaseHashTake.id =
        testUuid("collaboration-test-take", "uppercase-hash");
    uppercaseHashTake.asset.assetId =
        testUuid("collaboration-test-asset", "uppercase-hash");
    uppercaseHashTake.asset.sha256 = std::string(64, 'A');
    check(ProjectReducer::apply(
              state, command("uppercase-hash-take",
                             AddTake{audioTrack, audioClip,
                                     uppercaseHashTake, {}}))
                  .code == ApplyCode::InvalidCommand,
          "recording AssetRef requires a lowercase SHA-256 digest");
}

void sharedSnapshotMetadataRoundTrip() {
    SharedProjectDocument source;
    source.confirmedSequence = 17;
    source.project.name = "Snapshot";

    TrackModel liveTrack;
    liveTrack.id = trackId("snapshot-live");
    liveTrack.name = "Live";
    source.project.tracks.push_back(liveTrack);

    TrackTombstone deletedTrack;
    deletedTrack.track.id = trackId("snapshot-deleted-track");
    deletedTrack.track.name = "Deleted track";
    deletedTrack.deleteOperationId = operationId("snapshot-delete-track");
    deletedTrack.deleteServerSequence = 3;
    source.deletedTracks.emplace(deletedTrack.track.id, deletedTrack);

    ClipTombstone deletedClip;
    deletedClip.trackId = liveTrack.id;
    deletedClip.clip.id = testUuid("snapshot-clip", "deleted");
    deletedClip.clip.kind = ClipKind::Audio;
    deletedClip.clip.name = "Deleted clip";
    deletedClip.deleteOperationId = operationId("snapshot-delete-clip");
    deletedClip.deleteServerSequence = 5;
    source.deletedClips.emplace(deletedClip.clip.id, deletedClip);

    MidiNoteTombstone deletedNote;
    deletedNote.trackId = liveTrack.id;
    deletedNote.clipId = testUuid("snapshot-clip", "midi");
    deletedNote.note = midiNote(
        testUuid("snapshot-note", "deleted"), 72, 2.0, 0.5, 111);
    deletedNote.deleteOperationId = operationId("snapshot-delete-note");
    deletedNote.deleteServerSequence = 7;
    source.deletedNotes.emplace(deletedNote.note.id, deletedNote);

    AutomationPointTombstone deletedPoint;
    deletedPoint.trackId = liveTrack.id;
    deletedPoint.clipId = testUuid("snapshot-clip", "automation");
    deletedPoint.point = automationPoint(
        testUuid("snapshot-point", "deleted"), 4.0, 0.75,
        AutomationSegment::SCurve);
    deletedPoint.deleteOperationId = operationId("snapshot-delete-point");
    deletedPoint.deleteServerSequence = 9;
    source.deletedAutomationPoints.emplace(deletedPoint.point.id,
                                            deletedPoint);

    ControllerLaneTombstone deletedLane;
    deletedLane.trackId = liveTrack.id;
    deletedLane.clipId = testUuid("snapshot-clip", "lane");
    deletedLane.lane.id = testUuid("snapshot-lane", "deleted");
    deletedLane.lane.name = "Expression";
    deletedLane.lane.cc = 11;
    deletedLane.lane.points.push_back(automationPoint(
        testUuid("snapshot-point", "lane"), 1.0, 0.5));
    deletedLane.deleteOperationId = operationId("snapshot-delete-lane");
    deletedLane.deleteServerSequence = 11;
    source.deletedControllerLanes.emplace(deletedLane.lane.id, deletedLane);

    TakeTombstone deletedTake;
    deletedTake.trackId = liveTrack.id;
    deletedTake.clipId = testUuid("snapshot-clip", "takes");
    deletedTake.take.id = testUuid("snapshot-take", "deleted");
    deletedTake.take.name = "Take 2";
    deletedTake.take.asset.assetId =
        testUuid("snapshot-asset", "recording");
    deletedTake.take.asset.sha256.assign(64, 'c');
    deletedTake.take.asset.kind = AssetKind::Audio;
    deletedTake.take.asset.byteSize = 8192;
    deletedTake.deleteOperationId = operationId("snapshot-delete-take");
    deletedTake.deleteServerSequence = 13;
    source.deletedTakes.emplace(deletedTake.take.id, deletedTake);

    CompSegmentTombstone deletedSegment;
    deletedSegment.trackId = liveTrack.id;
    deletedSegment.clipId = deletedTake.clipId;
    deletedSegment.segment.id = testUuid("snapshot-segment", "deleted");
    deletedSegment.segment.takeId = deletedTake.take.id;
    deletedSegment.segment.startSeconds = 0.25;
    deletedSegment.segment.endSeconds = 1.75;
    deletedSegment.deleteOperationId =
        operationId("snapshot-delete-segment");
    deletedSegment.deleteServerSequence = 15;
    source.deletedCompSegments.emplace(deletedSegment.segment.id,
                                       deletedSegment);

    source.lastWriterByField.emplace("project:tempo",
                                     operationId("snapshot-writer-tempo"));
    source.lastWriterByField.emplace(
        ProjectReducer::trackFieldKey(liveTrack.id, TrackProperty::Name),
        operationId("snapshot-writer-track"));

    std::string encoded;
    SharedProjectDocument decoded;
    const audio::Result written =
        serializeSharedProjectSnapshot(source, encoded);
    const audio::Result read =
        written ? deserializeSharedProjectSnapshot(decoded, encoded)
                : audio::Result::fail(audio::EngineError::Unknown,
                                      written.message());
    check(written.isOk() && read.isOk() &&
              decoded.confirmedSequence == source.confirmedSequence &&
              decoded.project.tracks.size() == 1 &&
              decoded.deletedTracks.size() == 1 &&
              decoded.deletedClips.size() == 1 &&
              decoded.deletedNotes.size() == 1 &&
              decoded.deletedAutomationPoints.size() == 1 &&
              decoded.deletedControllerLanes.size() == 1 &&
              decoded.deletedTakes.size() == 1 &&
              decoded.deletedCompSegments.size() == 1 &&
              decoded.deletedTakes.at(deletedTake.take.id).take.asset ==
                  deletedTake.take.asset &&
              decoded.deletedCompSegments.at(deletedSegment.segment.id)
                      .segment.takeId == deletedSegment.segment.takeId &&
              decoded.deletedCompSegments.at(deletedSegment.segment.id)
                      .segment.startSeconds ==
                  deletedSegment.segment.startSeconds &&
              decoded.deletedCompSegments.at(deletedSegment.segment.id)
                      .segment.endSeconds ==
                  deletedSegment.segment.endSeconds &&
              decoded.lastWriterByField == source.lastWriterByField,
          "shared snapshot preserves reducer tombstones and field writers");

    std::string encodedAgain;
    check(read.isOk() &&
              serializeSharedProjectSnapshot(decoded, encodedAgain).isOk() &&
              encodedAgain == encoded,
          "shared snapshot bytes are canonical after decode");

    json corrupted = json::parse(encoded);
    corrupted["fieldWriters"][0]["operationId"] = "not-a-uuid";
    SharedProjectDocument rejected;
    check(deserializeSharedProjectSnapshot(rejected, corrupted.dump()).isError(),
          "shared snapshot rejects forged reducer metadata atomically");

    source.project.tracks.front().armed = true;
    std::string leaked;
    check(serializeSharedProjectSnapshot(source, leaked).isError() &&
              leaked.empty(),
          "shared snapshot rejects local capture and UI state");
}

void routingPluginAssetReducerAndWire() {
    SharedProjectDocument state;
    const std::string sourceId = trackId("routing-source");
    const std::string busId = trackId("routing-bus");
    const std::string folderId = trackId("routing-folder");
    const std::string instrumentTrackId = trackId("routing-instrument");
    const std::string clipId = testUuid("routing-clip", "audio");
    const std::string sendId = testUuid("routing-send", "reverb");

    const auto apply = [&](std::string label, auto body,
                           std::uint64_t serverSequence = 0) {
        ProjectCommand value = command(std::move(label), std::move(body));
        value.meta.serverSequence = serverSequence;
        const std::string wire = serializeProjectCommand(value);
        std::string error;
        const auto decoded = deserializeProjectCommand(wire, &error);
        check(decoded.has_value() && error.empty() &&
                  serializeProjectCommand(*decoded) == wire,
              "extended command round-trips through locked wire JSON");
        if (!decoded) return ApplyResult{};
        ProjectCommand committed = *decoded;
        committed.meta.serverSequence = serverSequence;
        return ProjectReducer::apply(state, committed);
    };

    check(apply("routing-folder-add",
                AddTrack{folderId, TrackKind::Folder, "Folder", 1, {}, {}})
                  .changed() &&
              apply("routing-source-add",
                    AddTrack{sourceId, TrackKind::Audio, "Source", 2, {},
                             folderId})
                  .changed() &&
              apply("routing-bus-add",
                    AddTrack{busId, TrackKind::Bus, "Bus", 3, {}, sourceId})
                  .changed() &&
              apply("routing-instrument-add",
                    AddTrack{instrumentTrackId, TrackKind::Instrument,
                             "Instrument", 4, {}, busId})
                  .changed() &&
              apply("routing-clip-add",
                    AddClip{sourceId, clipId, ClipKind::Audio, "Audio", 0.0,
                            4.0, 5, {}})
                  .changed(),
          "routing/plugin fixture uses only typed creation commands");

    check(apply("routing-parent", SetTrackParent{sourceId, folderId}).changed() &&
              apply("routing-output", SetTrackOutput{sourceId, busId}).changed(),
          "track parent and output routing use independent scalar heads");
    check(ProjectReducer::apply(
              state,
              command("routing-cycle", SetTrackOutput{busId, sourceId}))
                  .code == ApplyCode::InvalidCommand,
          "routing reducer rejects output cycles");

    SendModel send;
    send.id = sendId;
    send.destinationTrackId = busId;
    send.level = 0.25f;
    check(apply("send-add", AddSend{sourceId, send, {}}).changed() &&
              apply("send-level",
                    SetSendProperty{sourceId, sendId, SendProperty::Level,
                                    0.75})
                  .changed() &&
              state.project.findTrack(sourceId)->sends.front().level == 0.75f,
          "send lifecycle and mixer property are reduced deterministically");

    const AssetRef clipAsset = testAsset("clip-source", AssetKind::Audio, 'c');
    ClipSampleEditModel edit;
    edit.loopMode = 1;
    edit.loopStart = 0.1;
    edit.loopEnd = 0.9;
    edit.reverse = true;
    check(apply("clip-asset", SetClipAsset{sourceId, clipId, clipAsset})
                  .changed() &&
              apply("clip-sample-edit",
                    SetClipSampleEdit{sourceId, clipId, edit})
                  .changed() &&
              state.project.findTrack(sourceId)->clips.front().asset ==
                  clipAsset &&
              state.project.findTrack(sourceId)->clips.front().sampleEdit.reverse,
          "clip asset identity and final sample gesture are durable commands");

    const PluginLocation clipChain{PluginChain::Clip, sourceId, clipId};
    InsertModel clipEqualizer =
        builtinInsert("clip-equalizer", "daw.equalizer");
    InsertModel clipGravity = builtinInsert("clip-gravity", "daw.gravity");
    ApplyResult addClipEqualizer = apply(
        "plugin-clip-equalizer-add",
        AddPluginInsert{clipChain, clipEqualizer, {}});
    ApplyResult bypassClipEqualizer = apply(
        "plugin-clip-equalizer-bypass",
        SetPluginProperty{clipChain, clipEqualizer.id,
                          PluginProperty::Bypassed, true});
    ApplyResult addClipGravity = apply(
        "plugin-clip-gravity-add",
        AddPluginInsert{clipChain, clipGravity, clipEqualizer.id});
    ProjectCommand undoClipBypass = *bypassClipEqualizer.inverse;
    undoClipBypass.meta = meta("undo-plugin-clip-equalizer-bypass");
    check(addClipEqualizer.changed() && bypassClipEqualizer.changed() &&
              addClipGravity.changed() &&
              addClipEqualizer.inverse->conditions.size() == 1 &&
              state.lastWriterByField.at(
                  ProjectReducer::clipDescendantsKey(clipId)) ==
                  operationId("plugin-clip-gravity-add") &&
              ProjectReducer::apply(state, undoClipBypass).changed() &&
              !state.project.findTrack(sourceId)
                   ->clips.front()
                   .inserts.front()
                   .bypassed &&
              state.project.findTrack(sourceId)
                      ->clips.front()
                      .inserts.size() == 2,
          "clip plugin descendants advance the clip head without coupling a granular undo to sibling edits");

    InsertModel equalizer = builtinInsert("equalizer", "daw.equalizer");
    const PluginLocation trackChain{PluginChain::Track, sourceId, {}};
    ApplyResult addEqualizer =
        apply("plugin-eq-add", AddPluginInsert{trackChain, equalizer, {}});
    const AssetRef resource =
        testAsset("equalizer-resource", AssetKind::PluginResource, 'd');
    ApplyResult parameter = apply(
        "plugin-eq-param",
        SetPluginParameter{trackChain, equalizer.id, "band.1.gain", 2.5,
                           false});
    ApplyResult binding = apply(
        "plugin-eq-binding",
        SetPluginAssetBinding{trackChain, equalizer.id,
                              PluginAssetBinding{"curve", resource, true}});
    ApplyResult bypass = apply(
        "plugin-eq-bypass",
        SetPluginProperty{trackChain, equalizer.id,
                          PluginProperty::Bypassed, true});
    check(addEqualizer.changed() && parameter.changed() && binding.changed() &&
              bypass.changed() && bypass.inverse &&
              bypass.inverse->conditions.size() == 1 &&
              state.project.findTrack(sourceId)->inserts.front().parameters
                      .front()
                      .id == "band.1.gain" &&
              state.project.findTrack(sourceId)->inserts.front()
                      .assetBindings.front().asset == resource,
          "built-in plugin properties, parameters and assets have typed safe inverses");

    const InsertModel& equalizerState =
        state.project.findTrack(sourceId)->inserts.front();
    ApplyResult fullState = apply(
        "plugin-eq-full-state",
        SetPluginState{trackChain,
                       equalizer.id,
                       "2.0.0",
                       equalizerState.stateSchemaVersion,
                       equalizerState.stateAsset,
                       equalizerState.rightStateAsset,
                       equalizerState.parameters,
                       equalizerState.rightParameters,
                       equalizerState.assetBindings});
    ApplyResult newerParameter = apply(
        "plugin-eq-newer-param",
        SetPluginParameter{trackChain, equalizer.id, "band.1.gain", 3.0,
                           false});
    ProjectCommand staleStateUndo = *fullState.inverse;
    staleStateUndo.meta = meta("plugin-eq-stale-state-undo");
    check(fullState.changed() && newerParameter.changed() &&
              ProjectReducer::apply(state, staleStateUndo).code ==
                  ApplyCode::PreconditionsFailed,
          "full plugin-state undo cannot clobber a newer parameter writer");

    InsertModel gravity = builtinInsert("gravity", "daw.gravity");
    check(apply("plugin-gravity-add",
                AddPluginInsert{trackChain, gravity, equalizer.id})
                  .changed() &&
              apply("plugin-gravity-move",
                    MovePluginInsert{trackChain, gravity.id, {}})
                  .changed() &&
              state.project.findTrack(sourceId)->inserts.front().id ==
                  gravity.id,
          "plugin chain ordering uses stable afterId anchors");

    InsertModel sampler = builtinInsert("sampler", "daw.sampler");
    const PluginLocation instrumentChain{PluginChain::Instrument,
                                         instrumentTrackId, {}};
    check(apply("plugin-sampler-add",
                AddPluginInsert{instrumentChain, sampler, {}})
                  .changed() &&
              state.project.findTrack(instrumentTrackId)->instrument.id ==
                  sampler.id,
          "Sampler is available as a built-in instrument with explicit sample binding");
    PluginAssetBinding weakenedSample = sampler.assetBindings.front();
    weakenedSample.required = false;
    check(ProjectReducer::apply(
              state,
              command("plugin-sampler-weaken-binding",
                      SetPluginAssetBinding{instrumentChain, sampler.id,
                                            weakenedSample}))
                  .code == ApplyCode::InvalidCommand,
          "Sampler sample binding remains required after targeted updates");

    ProjectCommand oversizedParameter = command(
        "plugin-oversized-parameter",
        SetPluginParameter{trackChain, gravity.id,
                           std::string(kMaxPluginParameterIdBytes + 1, 'x'),
                           0.5, false});
    std::string oversizedError;
    check(!commandHasValidIds(oversizedParameter, &oversizedError) &&
              !oversizedError.empty(),
          "plugin parameter ids cannot overflow the 512-character field head");

    InsertModel unsafe = builtinInsert("unsafe", "daw.gravity");
    unsafe.path = "/Library/Audio/Plug-Ins/unsafe.vst3";
    check(ProjectReducer::apply(
              state, command("plugin-unsafe",
                             AddPluginInsert{trackChain, unsafe, gravity.id}))
                  .code == ApplyCode::InvalidCommand,
          "plugin command rejects local paths and non-shared UI state");
    InsertModel external = builtinInsert("external", "vendor.external");
    external.format = PluginFormat::Vst3;
    check(ProjectReducer::apply(
              state, command("plugin-external",
                             AddPluginInsert{trackChain, external, gravity.id}))
                  .code == ApplyCode::InvalidCommand,
          "v1 reducer rejects third-party plugin insertion");

    ApplyResult deleteSend =
        apply("send-delete", DeleteSend{sourceId, sendId}, 21);
    ApplyResult deletePlugin = apply(
        "plugin-eq-delete", DeletePluginInsert{trackChain, equalizer.id}, 22);
    check(deleteSend.changed() && deletePlugin.changed() &&
              state.deletedSends.contains(sendId) &&
              state.deletedPluginInserts.contains(equalizer.id) &&
              ProjectReducer::apply(
                  state,
                  command("plugin-eq-late",
                          SetPluginParameter{trackChain, equalizer.id,
                                             "band.1.gain", 0.0, false}))
                      .code == ApplyCode::DeletedEntity,
          "send/plugin delete-wins tombstones reject late mutations");

    state.confirmedSequence = 22;
    std::string snapshot;
    SharedProjectDocument decoded;
    const audio::Result snapshotWritten =
        serializeSharedProjectSnapshot(state, snapshot);
    const audio::Result snapshotRead = snapshotWritten
        ? deserializeSharedProjectSnapshot(decoded, snapshot)
        : snapshotWritten;
    if (!snapshotWritten || !snapshotRead)
        std::printf("snapshot diagnostic: write=%s read=%s\n",
                    snapshotWritten.message().c_str(),
                    snapshotRead.message().c_str());
    check(snapshotWritten.isOk() && snapshotRead.isOk() &&
              decoded.deletedSends.at(sendId).send.destinationTrackId == busId &&
              decoded.deletedPluginInserts.at(equalizer.id)
                      .insert.assetBindings.front().asset == resource,
          "shared snapshot preserves send and plugin tombstones without local paths");
}

struct RecordingAdapter final : ProjectProjectionAdapter {
    int calls = 0;
    bool sawFullProjection = false;
    std::vector<ProjectionOrigin> origins;
    void projectChanged(const SharedProjectDocument&, const ChangeImpact& impact,
                        ProjectionOrigin origin) override {
        ++calls;
        sawFullProjection = sawFullProjection || impact.fullProjection;
        origins.push_back(origin);
    }
};

void gatewayOptimisticConfirmedReplay() {
    RecordingAdapter adapter;
    CommandGateway gateway({}, &adapter);
    ProjectCommand malformed = command("malformed", SetProjectScalar{
        ProjectScalar::Tempo, 121.0});
    malformed.meta.operationId = "not-a-uuid";
    check(gateway.submit(malformed).code == GatewayCode::Rejected &&
              gateway.pending().empty() &&
              gateway.optimistic().project.tempo == 120.0,
          "gateway rejects malformed operation UUIDs before optimistic apply");

    ProjectCommand localTempo = command("local-tempo", SetProjectScalar{
        ProjectScalar::Tempo, 135.0});
    check(gateway.submit(localTempo).accepted() &&
              gateway.optimistic().project.tempo == 135.0 &&
              gateway.confirmed().project.tempo == 120.0 &&
              gateway.pending().size() == 1,
          "gateway applies local command optimistically only");

    ProjectCommand remoteName = command("remote-name", SetProjectScalar{
        ProjectScalar::Name, std::string("Remote")});
    remoteName.meta.serverSequence = 1;
    GatewayUpdate remote = gateway.receiveConfirmed(remoteName);
    check(remote.accepted() && gateway.confirmed().project.name == "Remote" &&
              gateway.confirmed().project.tempo == 120.0 &&
              gateway.optimistic().project.name == "Remote" &&
              gateway.optimistic().project.tempo == 135.0 &&
              gateway.pending().size() == 1 && adapter.calls == 2,
          "gateway replays pending locals over a remote confirmation");

    localTempo.meta.serverSequence = 2;
    GatewayUpdate acknowledged = gateway.receiveConfirmed(localTempo);
    check(acknowledged.accepted() && gateway.pending().empty() &&
              gateway.confirmed().project.tempo == 135.0 &&
              gateway.optimistic().project.tempo == 135.0 &&
              gateway.confirmed().confirmedSequence == 2 &&
              adapter.calls == 2,
          "gateway advances a matching own ack without rematerializing it");

    RecordingAdapter changedAckAdapter;
    CommandGateway changedAckGateway({}, &changedAckAdapter);
    ProjectCommand optimisticTempo = command(
        "changed-own-ack",
        SetProjectScalar{ProjectScalar::Tempo, 136.0});
    check(changedAckGateway.submit(optimisticTempo).accepted() &&
              changedAckAdapter.calls == 1,
          "changed-ack fixture materializes its optimistic value");
    ProjectCommand changedAcknowledgement = optimisticTempo;
    changedAcknowledgement.body =
        SetProjectScalar{ProjectScalar::Tempo, 137.0};
    changedAcknowledgement.meta.serverSequence = 1;
    check(changedAckGateway.receiveConfirmed(changedAcknowledgement)
                  .accepted() &&
              changedAckGateway.pending().empty() &&
              changedAckGateway.optimistic().project.tempo == 137.0 &&
              changedAckAdapter.calls == 2 &&
              changedAckAdapter.origins.back() ==
                  ProjectionOrigin::ConfirmedLocalAck,
          "matching opId cannot suppress a materially changed acknowledgement");

    ProjectCommand remotePan = command("remote-pan", SetProjectScalar{
        ProjectScalar::MasterPan, 0.25});
    remotePan.meta.serverSequence = 3;
    check(gateway.receiveConfirmed(remotePan).accepted() &&
              adapter.origins.back() == ProjectionOrigin::ConfirmedRemote,
          "adapter can keep remote confirmation out of local undo and dirty paths");

    ProjectCommand gap = command("gap", SetProjectScalar{
        ProjectScalar::Tempo, 140.0});
    gap.meta.serverSequence = 5;
    check(gateway.receiveConfirmed(gap).code == GatewayCode::SequenceGap &&
              gateway.confirmed().confirmedSequence == 3,
          "gateway refuses a confirmed sequence gap");
    check(adapter.calls >= 3 && adapter.sawFullProjection,
          "gateway reports projection impacts through the adapter seam");

    RecordingAdapter staleAckAdapter;
    CommandGateway staleAckGateway({}, &staleAckAdapter);
    ProjectCommand pending = command(
        "stale-own-ack",
        SetProjectScalar{ProjectScalar::Tempo, 155.0});
    check(staleAckGateway.submit(pending).accepted() &&
              staleAckGateway.pending().size() == 1,
          "gateway queues pending operation before snapshot replacement");
    SharedProjectDocument newerSnapshot;
    newerSnapshot.project.tempo = 148.0;
    check(staleAckGateway.replaceConfirmed(std::move(newerSnapshot), 9)
              .accepted() &&
              staleAckGateway.optimistic().project.tempo == 155.0,
          "snapshot replacement replays a pending local operation");
    pending.meta.serverSequence = 8;
    GatewayUpdate staleOwnAck = staleAckGateway.receiveConfirmed(pending);
    check(staleOwnAck.code == GatewayCode::Duplicate &&
              staleAckGateway.pending().empty() &&
              staleAckGateway.confirmed().confirmedSequence == 9 &&
              staleAckGateway.confirmed().project.tempo == 148.0 &&
              staleAckGateway.optimistic().project.tempo == 148.0 &&
              staleAckAdapter.calls == 3 &&
              staleAckAdapter.origins.back() == ProjectionOrigin::Rebase,
          "stale own ack projects a pending drop after a newer snapshot");
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const fs::path dir = fs::temp_directory_path() /
                         "vlt-collaboration-protocol-test";
    std::error_code error;
    fs::remove_all(dir, error);
    fs::create_directories(dir, error);

    serializerV6AndLegacyMigration(dir);
    commandWireRoundTrip();
    reducerReplayBatchDeleteAndUndo();
    clipNoteAutomationReducerAndWire();
    laneTakeCompReducerAndRecordingBatch();
    sharedSnapshotMetadataRoundTrip();
    routingPluginAssetReducerAndWire();
    gatewayOptimisticConfirmedReplay();

    fs::remove_all(dir, error);
    if (failures) std::printf("\nFAILURES PRESENT: %d\n", failures);
    return failures == 0 ? 0 : 1;
}
