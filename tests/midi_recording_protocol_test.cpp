#include "collaboration/CommandJson.hpp"
#include "collaboration/MidiContentJson.hpp"
#include "collaboration/ProjectReducer.hpp"
#include "collaboration/SharedProjectSnapshot.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
using namespace daw;
using namespace daw::collab;
void check(bool value, const char *what) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}
ProjectCommand command(CommandBody body) {
    ProjectCommand c;
    c.meta.operationId = newUuid();
    c.meta.projectId = "00000000-0000-4000-8000-000000000001";
    c.body = std::move(body);
    return c;
}
int main() {
    std::ifstream fixture(std::filesystem::path(__FILE__).parent_path() /
                          "fixtures/midi_content_v4.json");
    check(bool(fixture), "shared C++/Go MIDI wire fixture is present");
    const auto wire = nlohmann::json::parse(fixture);
    ClipModel wireClip;
    check(midiContentFromJson(wire, wireClip) && validMidiContentIdentities(wireClip),
          "shared MIDI wire fixture validates");
    check(midiContentToJson(wireClip) == wire, "shared MIDI wire fixture round-trips exactly");
    SharedProjectDocument state;
    TrackModel track;
    track.id = newUuid();
    track.kind = TrackKind::Midi;
    state.project.tracks.push_back(track);
    ClipModel clip;
    clip.id = newUuid();
    clip.kind = ClipKind::Midi;
    clip.durationSeconds = 5000;
    for (int i = 0; i < 7000; ++i) {
        NoteModel n;
        n.id = newUuid();
        n.startBeats = i * .25;
        n.channel = i % 16;
        n.releaseVelocity = i % 128;
        n.startOrder = i * 2 + 1;
        n.endOrder = i * 2 + 2;
        clip.notes.push_back(n);
    }
    ControllerLane lane;
    lane.id = newUuid();
    lane.cc = -2;
    lane.channel = 9;
    lane.name = "Bend";
    for (int i = 0; i < 7000; ++i) {
        AutomationPoint p;
        p.id = newUuid();
        p.beats = i * .25;
        p.value = (i % 16384) / 16383.0;
        p.eventOrder = i + 1;
        p.shape = AutomationSegment::Hold;
        lane.points.push_back(p);
    }
    clip.lanes.push_back(lane);
    const auto run = newUuid(), content = newUuid();
    auto parts = prepareMidiContent(run, content, clip);
    std::size_t bytes = 0;
    for (const auto &part : parts) {
        auto c = command(part);
        bytes += serializedProjectCommandPayloadSize(c);
        check(serializedProjectCommandPayloadSize(c) < kMaxProjectCommandBatchBytes,
              "part fits message limit");
        auto decoded = deserializeProjectCommand(serializeProjectCommand(c));
        check(bool(decoded), "part wire decode");
        check(ProjectReducer::apply(state, *decoded).accepted(), "stage part");
    }
    check(bytes > 1024 * 1024, "performance larger than 1MB");
    check(state.project.tracks[0].clips.empty(), "parts invisible on timeline");
    std::string saved;
    check(serializeSharedProjectSnapshot(state, saved).isOk(), "persist prepared parts");
    SharedProjectDocument restarted;
    auto read = deserializeSharedProjectSnapshot(restarted, saved);
    if (!read.isOk())
        std::fprintf(stderr, "snapshot decode failed\n");
    check(read.isOk(), "restart from snapshot");
    auto batch = std::make_shared<BatchCommand>();
    batch->commands.push_back(
        command(AddClip{track.id, clip.id, ClipKind::Midi, "Take", 0, 5000, clip.color, {}}));
    batch->commands.push_back(
        command(ApplyMidiContent{track.id, clip.id, run, content, std::uint32_t(parts.size())}));
    auto commit = command(RecordingCommit{{}, batch});
    auto applied = ProjectReducer::apply(restarted, commit);
    if (!applied.accepted())
        std::fprintf(stderr, "%s\n", applied.message.c_str());
    check(applied.accepted(), "atomic MIDI commit");
    check(restarted.project.tracks[0].clips[0].notes == clip.notes,
          "all note properties survive chunking");
    check(restarted.project.tracks[0].clips[0].lanes == clip.lanes,
          "all controller properties survive chunking");
    check(ProjectReducer::apply(restarted, commit).code == ApplyCode::Duplicate,
          "idempotent commit");
    check(bool(applied.inverse), "one inverse for whole take");
    auto undo = *applied.inverse;
    undo.meta.operationId = newUuid();
    auto undone = ProjectReducer::apply(restarted, undo);
    if (!undone.accepted())
        std::fprintf(stderr, "%s\n", undone.message.c_str());
    check(undone.accepted() && restarted.project.tracks[0].clips.empty(),
          "one undo removes whole take");
    auto redo = *undone.inverse;
    redo.meta.operationId = newUuid();
    check(ProjectReducer::apply(restarted, redo).accepted(), "whole take redo");
    check(restarted.project.tracks[0].clips[0].notes.size() == 7000, "redo retains full content");
    // A second client, initialized from the same retained preparation, reaches
    // exactly the same document. Foreign note edits block a whole-take undo.
    SharedProjectDocument secondClient;
    check(deserializeSharedProjectSnapshot(secondClient, saved).isOk(), "second client snapshot");
    check(ProjectReducer::apply(secondClient, commit).accepted(), "second client commit");
    check(secondClient.project.tracks[0].clips[0].notes ==
              restarted.project.tracks[0].clips[0].notes,
          "two reducers converge");
    auto foreign = secondClient.project.tracks[0].clips[0].notes[0];
    foreign.velocity = 22;
    check(
        ProjectReducer::apply(secondClient, command(UpsertMidiNote{track.id, clip.id, foreign, {}}))
            .accepted(),
        "foreign edit after recording");
    auto conflictUndo = *applied.inverse;
    conflictUndo.meta.operationId = newUuid();
    check(!ProjectReducer::apply(secondClient, conflictUndo).accepted() &&
              secondClient.project.tracks[0].clips[0].notes[0].velocity == 22,
          "undo preserves a later foreign edit");
    ClipModel layered;
    layered.kind = ClipKind::Midi;
    layered.durationSeconds = 2;
    TakeModel take;
    take.id = newUuid();
    take.lengthSeconds = 2;
    take.notes = {clip.notes.front()};
    take.lanes = {lane};
    layered.takes = {take};
    selectWholeTake(layered, take.id);
    ClipModel decodedLayer;
    check(midiContentFromJson(midiContentToJson(layered), decodedLayer), "layer codec");
    check(decodedLayer.comp.size() == 1 && decodedLayer.takes[0].lanes == take.lanes,
          "comp and take automation survive wire parsing");
    auto takeCommand = command(AddTake{track.id, clip.id, take, {}});
    check(bool(deserializeProjectCommand(serializeProjectCommand(takeCommand))),
          "ordinary MIDI take editing wire format");
    auto bad = midiContentToJson(clip);
    bad["notes"][0]["channel"] = 16;
    ClipModel rejected;
    check(!midiContentFromJson(bad, rejected),
          "invalid note channel is rejected instead of clamped");
    bad = midiContentToJson(clip);
    bad["lanes"][0]["points"][0]["value"] = 1.1;
    check(!midiContentFromJson(bad, rejected), "invalid automation value is rejected");
    // No part of an incomplete commit may escape the transaction.
    auto incomplete = command(RecordingCommit{{}, std::make_shared<BatchCommand>(*batch)});
    std::get<ApplyMidiContent>(std::get<RecordingCommit>(incomplete.body).batch->commands[1].body)
        .contentId = newUuid();
    SharedProjectDocument fresh = state;
    check(!ProjectReducer::apply(fresh, incomplete).accepted() &&
              fresh.project.tracks[0].clips.empty(),
          "incomplete preparation commits nothing");
    std::puts("MIDI protocol checks passed");
}
