#include "EngineController.hpp"
#include "MidiRecording.hpp"
#include "Nodes/MidiClipPlayerNode.hpp"
#include "ProjectSerializer.hpp"
#include "collaboration/MidiContentJson.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
using namespace daw;
static void check(bool value, const char *what) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}
int main() {
    MidiRecording r;
    r.event(1, 0x92, 60, 111, 0, 1);
    r.event(2, 0x92, 60, 88, .25, 2);
    r.event(1, 0x82, 60, 45, 1, 4);
    r.event(2, 0x82, 60, 67, 1.5, 5);
    r.event(1, 0xb2, 64, 127, .5, 3);
    r.event(1, 0xe2, 3, 75, .5, 6);
    r.event(1, 0xd2, 100, 0, .5, 7);
    r.event(1, 0xa2, 60, 64, .5, 8);
    r.event(1, 0xc2, 12, 0, .5, 9);
    auto data = r.finished(2);
    check(data.notes.size() == 2 && data.notes[0].channel == 2 &&
              data.notes[0].releaseVelocity == 45,
          "source/channel/release velocity");
    check(data.notes[1].lengthBeats == 1.25 && data.notes[1].velocity == 88,
          "overlapping independent notes");
    check(data.lanes.size() == 5, "all channel controller types");
    check(std::lround(data.lanes[1].points[0].value * 16383) == (3 | (75 << 7)),
          "14-bit pitch bend");
    auto second = sliceMidiPerformance(data, .75, 2);
    check(second.notes.size() == 2 && second.notes[0].startBeats == 0 &&
              second.lanes[0].points[0].beats == 0,
          "loop boundary note split and controller chase");
    MidiRecording held;
    held.event(1, 0x90, 60, 100, 0, 1);
    check(held.finished(4).notes[0].lengthBeats == 4, "stop closes held note");
    held.event(1, 0x90, 60, 99, 2, 2);
    held.event(1, 0x90, 60, 0, 3, 3);
    check(held.data.notes.size() == 2 && held.data.notes[0].lengthBeats == 2 &&
              held.data.notes[1].lengthBeats == 1,
          "retrigger and velocity-zero note off");

    engine::MidiClipPlayerNode node;
    auto notes = std::make_shared<engine::MidiClipPlayerNode::NoteList>();
    notes->push_back({0, .5, 60, 111, 2, 0, 45, 1, 4});
    node.setNotes(notes);
    auto curves = std::make_shared<engine::MidiClipPlayerNode::ControlCurves>();
    curves->push_back({0,
                       1,
                       0,
                       -2,
                       2,
                       0,
                       {{0, double(3 | (75 << 7)) / 16383, engine::curve::Shape::Hold, 0, 2}}});
    curves->push_back({0, 1, 0, 64, 2, 0, {{0, 1, engine::curve::Shape::Hold, 0, 3}}});
    node.setControllers(curves);
    node.prepare({48000, 12000, 2});
    engine::MidiBuffer output;
    output.reserve(512);
    engine::ProcessContext ctx;
    ctx.frames = 12000;
    ctx.sampleRate = 48000;
    ctx.playing = true;
    ctx.transport.tempo = 120;
    ctx.transport.ppqPosition = 0;
    ctx.midiOutput = &output;
    node.process(ctx);
    output.sort();
    check(output.size() >= 3, "playback controllers and notes");
    bool bend = false;
    for (const auto &e : output.events())
        if (e.status == 0xe2 && e.data1 == 3 && e.data2 == 75)
            bend = true;
    check(bend, "playback exact pitch bytes");
    output.clear();
    ctx.transport.ppqPosition = .5;
    node.process(ctx);
    bool release = false;
    for (const auto &e : output.events())
        if (e.status == 0x82 && e.data2 == 45)
            release = true;
    check(release, "playback release velocity");
    output.clear();
    ctx.transport.ppqPosition = 1;
    node.process(ctx);
    bool pedal = false;
    for (const auto &e : output.events())
        if (e.status == 0xb2 && e.data1 == 64 && e.data2 == 0)
            pedal = true;
    check(pedal, "pedal reset at exact clip boundary");

    EngineController c;
    check(c.initialize(48000, 512, false).isOk(), "offline initialization");
    const auto track = c.addTrack(TrackKind::Midi, "Keys");
    auto prefs = c.recordingPrefs();
    prefs.mode = RecordMode::Overwrite;
    prefs.midiOverdubMerge = false;
    c.setRecordingPrefs(prefs);
    check(c.startRecording(track), "MIDI starts without audio input");
    auto stamp = c.midiInputStamp();
    const auto builds = c.graphRebuildCountForTest();
    c.liveMidiInput(track, 0x93, 60, 101, 1, stamp);
    stamp.transportBeats += 2;
    stamp.timeNs = c.midiInputStamp().timeNs;
    c.liveMidiInput(track, 0x83, 60, 52, 1, stamp);
    check(c.graphRebuildCountForTest() == builds, "capture does not rebuild audio graph");
    c.stopRecording();
    const auto *t = c.project().findTrack(track);
    check(t->clips.size() == 1 && t->clips[0].notes.size() == 1 &&
              t->clips[0].notes[0].channel == 3,
          "editable clip landing");
    check(t->clips[0].notes[0].releaseVelocity == 52, "recording release velocity");
    c.undo();
    check(c.project().findTrack(track)->clips.empty(), "one undo removes take");
    c.redo();
    check(c.project().findTrack(track)->clips.size() == 1, "redo restores take");
    c.clearRetrospectiveMidi();
    stamp = c.midiInputStamp();
    stamp.timeNs -= 500000000;
    c.liveMidiInput(track, 0x90, 67, 90, 1, stamp);
    stamp.timeNs += 500000000;
    c.liveMidiInput(track, 0x80, 67, 20, 1, stamp);
    check(c.restoreRetrospectiveMidi(4), "retrospective restores");
    check(c.project().findTrack(track)->clips.size() == 2, "retrospective appends");
    check(!c.hasRetrospectiveMidi(), "restoration consumes its interval");
    stamp = c.midiInputStamp();
    stamp.timeNs -= 100000000;
    c.liveMidiInput(track, 0x90, 69, 91, 2, stamp);
    stamp.timeNs += 50000000;
    c.liveMidiInput(track, 0x80, 69, 31, 2, stamp);
    check(c.restoreRetrospectiveMidi(6) && c.project().findTrack(track)->clips.size() == 3,
          "second restoration contains only new performance");
    c.undo();
    check(c.project().findTrack(track)->clips.size() == 2, "restore has one undo");
    c.redo();
    c.clearRetrospectiveMidi();
    const auto recent = c.midiInputStamp();
    auto old = recent;
    if (old.timeNs > 601000000000ULL) {
        old.timeNs -= 601000000000ULL;
        c.liveMidiInput(track, 0x90, 70, 80, 3, old);
        check(c.hasRetrospectiveMidi(), "long-held note survives ten-minute trimming");
        c.liveMidiInput(track, 0x80, 70, 0, 3, recent);
        check(c.restoreRetrospectiveMidi(8), "long-held note restores");
        const auto &last = c.project().findTrack(track)->clips.back();
        check(last.durationSeconds <= 600.01 && last.notes.front().startBeats == 0,
              "retrospective is bounded to ten minutes");
    }
    c.clearRetrospectiveMidi();
    stamp = c.midiInputStamp();
    stamp.timeNs -= 100000000;
    c.liveMidiInput(track, 0x90, 71, 77, 4, stamp);
    auto cutoff = stamp.timeNs;
    stamp.timeNs += 50000000;
    c.liveMidiInput(track, 0x80, 71, 0, 4, stamp);
    c.clearRetrospectiveMidi(cutoff); // A later note must survive an older cloud acknowledgement.
    stamp.timeNs += 1000;
    c.liveMidiInput(track, 0x90, 72, 90, 4, stamp);
    stamp.timeNs += 1000;
    c.liveMidiInput(track, 0x80, 72, 0, 4, stamp);
    check(c.hasRetrospectiveMidi(), "new input survives an earlier confirmation");
    c.clearRetrospectiveMidi();

    stamp = c.midiInputStamp();
    stamp.timeNs -= 200000000;
    c.liveMidiInput(track, 0x90, 73, 90, 4, stamp);
    stamp.timeNs += 50000000;
    c.liveMidiInput(track, 0x80, 73, 0, 4, stamp);
    stamp.timeNs += 100000000;
    c.liveMidiInput(track, 0x80, 74, 0, 4, stamp);
    check(c.restoreRetrospectiveMidi(12) &&
              c.project().findTrack(track)->clips.back().durationSeconds < .051,
          "unmatched releases do not extend restored trailing silence");

    auto early = c.midiInputStamp();
    early.timeNs -= 200000000;
    auto later = early;
    later.timeNs += 50000000;
    c.liveMidiInput(track, 0x90, 67, 100, 31, later);
    c.liveMidiInput(track, 0x90, 60, 100, 32, early);
    later.timeNs += 100000000;
    early.timeNs += 100000000;
    c.liveMidiInput(track, 0x80, 67, 20, 31, later);
    c.liveMidiInput(track, 0x80, 60, 30, 32, early);
    check(c.restoreRetrospectiveMidi(14), "delayed hardware input is recoverable");
    const auto delayed = c.project().findTrack(track)->clips.back();
    check(delayed.notes.size() == 2 && delayed.notes.front().pitch == 60 &&
              delayed.notes.front().startBeats == 0 && delayed.durationSeconds < .151,
          "timestamps preserve timing when Qt delivers hardware after newer typing input");

    check(c.armCountIn({track}, 2), "MIDI count-in needs no audio input");
    stamp = c.midiInputStamp();
    c.liveMidiInput(track, 0x90, 75, 90, 7, stamp);
    check(!c.hasRetrospectiveMidi(), "recovery unavailable during count-in");
    c.cancelCountIn();
    check(c.hasRetrospectiveMidi(), "cancelled count-in keeps live input");
    check(c.armCountIn({track}, 1) && c.tickCountIn(1.0), "count-in starts the frozen MIDI target");
    stamp = c.midiInputStamp();
    stamp.transportBeats += 1;
    c.liveMidiInput(track, 0x80, 75, 10, 7, stamp);
    auto counted = c.finalizeRecordingCapture();
    check(counted.tracks[0].performance.notes.size() == 1 &&
              counted.tracks[0].performance.notes[0].startBeats == 0,
          "note held before count-in is seeded at recording start");
    c.clearRetrospectiveMidi();

    // Frozen routing, no audition recapture, controller-only and empty takes.
    const auto other = c.addTrack(TrackKind::Midi, "Other");
    check(c.startRecording(track), "start fixed-target capture");
    stamp = c.midiInputStamp();
    c.liveMidiInput(other, 0x90, 65, 100, 10, stamp, LiveMidiOrigin::Audition);
    c.liveMidiInput(other, 0x94, 66, 99, 11, stamp);
    stamp.transportBeats += 1;
    c.liveMidiInput(other, 0x84, 66, 33, 11, stamp, LiveMidiOrigin::Cleanup);
    auto frozen = c.finalizeRecordingCapture();
    check(frozen.tracks.size() == 1 && frozen.tracks[0].trackId == track &&
              frozen.tracks[0].performance.notes.size() == 1,
          "target is frozen and audition excluded");
    check(frozen.tracks[0].closedWavPath.empty(), "MIDI capture creates no WAV");
    check(c.startRecording(track), "start controller-only capture");
    stamp = c.midiInputStamp();
    c.liveMidiInput(track, 0xb4, 64, 127, 11, stamp);
    stamp.transportBeats += 1;
    c.liveMidiInput(track, 0xb4, 64, 0, 11, stamp, LiveMidiOrigin::Cleanup);
    auto controlsOnly = c.finalizeRecordingCapture();
    check(controlsOnly.tracks[0].performance.notes.empty() &&
              !controlsOnly.tracks[0].performance.lanes.empty(),
          "controllers record without notes");
    c.clearRetrospectiveMidi();
    check(c.startRecording(track), "start empty capture");
    auto empty = c.finalizeRecordingCapture();
    check(empty.tracks[0].performance.empty(),
          "initial controller state does not create an empty take");

    // Deterministic landing tests do not depend on a device callback or UI timer.
    TrackModel base;
    base.id = newUuid();
    base.kind = TrackKind::Midi;
    ClipModel original;
    original.id = newUuid();
    original.kind = ClipKind::Midi;
    original.durationSeconds = 4;
    NoteModel originalNote;
    originalNote.id = newUuid();
    originalNote.pitch = 48;
    originalNote.lengthBeats = 8;
    original.notes.push_back(originalNote);
    base.clips.push_back(original);
    EngineController::FinalizedRecordingTrack take;
    take.midi = true;
    take.midiTempo = 120;
    take.performance = data;
    take.passes = {{1, 2, 0}};
    take.semantics.mode = RecordMode::Overwrite;
    auto overwritten = c.midiRecordingLanding(base, take);
    check(overwritten.size() == 3, "overwrite keeps both sides of the replaced range");
    take.semantics.mode = RecordMode::Layers;
    take.semantics.trimTakesToRegion = true;
    auto layered = c.midiRecordingLanding(base, take);
    check(layered.size() == 1 && layered[0].takes.size() == 2, "layers preserve the original take");
    check(collab::validMidiContentIdentities(layered[0]),
          "recorded layers retain unique identities for cloud preparation");
    TrackModel rendered = base;
    rendered.clips = layered;
    check(midiPlaybackClips(rendered, 120).size() == 3, "comp renders old-new-old regions");
    check(!midiNotes(layered[0]).empty() && midiNotes(layered[0]).front().startBeats == 2,
          "piano roll exposes the recorded take at its clip position");
    auto slower = layered[0];
    retimeClipToTempo(slower, 2);
    check(slower.takes[1].lengthSeconds == layered[0].takes[1].lengthSeconds * 2 &&
              slower.comp[1].startSeconds == layered[0].comp[1].startSeconds * 2 &&
              slower.takes[1].notes == layered[0].takes[1].notes,
          "tempo changes preserve MIDI take beats and retime comp windows");
    auto sliced = layered[0];
    sliceMidiClipContent(sliced, 1, 3, 120, true);
    check(sliced.takes[0].id != layered[0].takes[0].id && sliced.comp[0].startSeconds == 0,
          "slice rebases layers and allocates identities");
    auto offsetClip = original;
    offsetClip.offsetSeconds = .5;
    sliceMidiClipContent(offsetClip, 1, 2, 120, false);
    check(offsetClip.offsetSeconds == 0 && offsetClip.notes.front().lengthBeats == 2,
          "MIDI slicing preserves clip-relative timing independently of audio offset metadata");

    {
        EngineController patternController;
        patternController.initialize(48000, 512, false);
        const auto pattern = patternController.addPattern("Recorded Pattern");
        const auto child = patternController.addTrack(TrackKind::Midi, "Pattern Keys");
        patternController.moveTrackToFolder(child, pattern);
        const auto clip = patternController.addMidiClip(child, 0, 2);
        patternController.addNote(child, clip, 48, 0, 4);
        auto preferences = patternController.recordingPrefs();
        preferences.mode = RecordMode::Layers;
        preferences.midiOverdubMerge = false;
        patternController.setRecordingPrefs(preferences);
        check(patternController.startRecording(child), "record MIDI within a Pattern");
        auto at = patternController.midiInputStamp();
        patternController.liveMidiInput(child, 0x90, 60, 100, 99, at);
        patternController.liveMidiInput(child, 0xb0, 64, 127, 99, at);
        at.transportBeats += 2;
        patternController.liveMidiInput(child, 0x80, 60, 45, 99, at);
        patternController.stopRecording();
        const auto owner = patternController.project().findTrack(pattern)->clips.front().id;
        const auto right = patternController.splitClip(pattern, owner, .5);
        const auto *children = patternController.project().findTrack(child);
        check(!right.empty() && children->clips.size() == 2 &&
                  children->clips[0].takes.size() == 2 && children->clips[1].takes.size() == 2 &&
                  !children->clips[1].takes.back().notes.empty() &&
                  children->clips[0].takes.back().id != children->clips[1].takes.back().id,
              "Pattern split preserves recorded layers and their independent identities");
        check(!midiPlaybackClips(*children, 120).empty(), "split MIDI Pattern remains playable");
    }
    take.semantics.midiOverdubMerge = true;
    auto overdub = c.midiRecordingLanding(base, take);
    check(overdub.size() == 1 && overdub[0].notes.size() == 3, "overdub merges notes");
    auto automated = base;
    ControllerLane untouched;
    untouched.id = newUuid();
    untouched.cc = 74;
    AutomationPoint existingPoint{2, .75, AutomationSegment::Hold};
    existingPoint.id = newUuid();
    untouched.points.push_back(existingPoint);
    automated.clips.front().lanes.push_back(untouched);
    auto seededTake = take;
    ControllerLane chased = untouched;
    chased.id = newUuid();
    chased.points.front().id = newUuid();
    chased.points.front().beats = 0;
    chased.points.front().value = .1;
    seededTake.performance.lanes.push_back(chased);
    const auto untouchedResult = c.midiRecordingLanding(automated, seededTake);
    const auto preserved =
        std::find_if(untouchedResult.front().lanes.begin(), untouchedResult.front().lanes.end(),
                     [](const auto &lane) { return lane.cc == 74; });
    check(preserved != untouchedResult.front().lanes.end() &&
              std::abs(automationValueAt(preserved->points, 2.5, preserved->defaultValue) - .75) <
                  1e-9,
          "overdub does not replace untouched automation with carried controller state");
    take.semantics.midiOverdubMerge = false;
    take.semantics.loopEnabled = true;
    take.semantics.loopCreatesTakes = true;
    take.passes = {{0, 1, 0}, {0, 1, 1}, {0, .5, 2}};
    auto loops = c.midiRecordingLanding(TrackModel{}, take);
    check(loops.size() == 1 && loops[0].takes.size() == 1,
          "empty final loop does not replace the previous take");

    // MIDI Learn shares the parameter lane and does not retain the raw CC.
    const auto instrument = c.addTrack(TrackKind::Instrument, "Learn");
    const auto sampler = c.pluginManager().find(plugins::Format::Internal, "daw.sampler");
    check(sampler && c.setTrackInstrumentPlugin(instrument, *sampler), "load learn fixture");
    const auto slot = c.project().findTrack(instrument)->instrument.id;
    auto params = c.insertParameters(instrument, slot);
    auto param = std::find_if(params.begin(), params.end(), [](const auto &p) {
        return p.isAutomatable && p.maxValue > p.minValue;
    });
    check(param != params.end(), "instrument exposes automatable parameter");
    c.beginMidiLearn(instrument, param->id);
    c.clearRetrospectiveMidi();
    check(c.startRecording(instrument), "learn recording starts");
    stamp = c.midiInputStamp();
    c.liveMidiInput(instrument, 0xb2, 21, 100, 20, stamp);
    stamp.transportBeats += 1;
    c.liveMidiInput(instrument, 0xb2, 21, 45, 20, stamp);
    auto learned = c.finalizeRecordingCapture();
    check(learned.tracks[0].performance.lanes.size() == 1 &&
              learned.tracks[0].performance.lanes[0].cc == -1 &&
              learned.tracks[0].performance.lanes[0].parameterId == param->id,
          "learn records parameter without duplicate CC");
    check(c.hasRetrospectiveMidi(), "learn movements are recoverable without notes");
    c.clearRetrospectiveMidi();
    check(c.startRecording(instrument), "manual automation starts");
    c.setInsertParameter(instrument, slot, param->id, param->minValue);
    auto manual = c.finalizeRecordingCapture();
    check(!manual.tracks[0].performance.lanes.empty(), "manual parameter changes create a lane");
    std::string document;
    check(ProjectSerializer::serializeDocument(c.project(), document).isOk(),
          "serialize recorded document");
    ProjectModel loaded;
    check(ProjectSerializer::deserializeDocument(loaded, document).isOk(),
          "open recorded document");
    check(loaded.findTrack(track)->clips.front().notes.front().releaseVelocity == 52,
          "save/open retains expression");
    std::puts("MIDI recording checks passed");
}
