#include "EngineController.hpp"
#include "ProjectSerializer.hpp"
#include "Internal/SamplerInstance.hpp"
#include "Recording/RecordingEngine.hpp"
#include "Core/AudioBuffer.hpp"
#include "cloud/CloudDocumentProjection.hpp"
#include "model/ClipLibrary.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
namespace {
int failures=0;
bool check(bool value, const char* label) {
    std::printf("%s %s\n",value?"PASS":"FAIL",label);
    if (!value) ++failures;
    return value;
}
const daw::ClipModel* clip(const daw::EngineController& c, const daw::EngineController::ClipAddress& at) {
    const auto* t=c.project().findTrack(at.trackId); if (!t) return nullptr;
    const auto i=std::find_if(t->clips.begin(),t->clips.end(),[&](const auto& x){return x.id==at.clipId;});
    return i==t->clips.end()?nullptr:&*i;
}
bool near(double a,double b) {return std::abs(a-b)<1e-6;}
}
int main() {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    const auto root=fs::temp_directory_path()/ ("vlt-library-"+daw::newUuid());
    fs::create_directories(root);
    const auto wav=(root/"Source.wav").string();
    audio::AudioBuffer tone(1,4800);
    for (unsigned i=0;i<4800;++i) tone.getChannel(0)[i]=.3f*std::sin(i*.06f);
    audio::AudioRecorder writer; writer.initialize(48000,1); writer.writeWAVFile(wav,tone,48000);
    daw::EngineController c{};
    if (!check(c.initialize(48000,256,false).isOk(),"headless engine")) return 1;
    const auto track=c.addTrack(daw::TrackKind::Midi,"Keys");
    const auto sampler=c.pluginManager().find(daw::plugins::Format::Internal,"daw.sampler");
    if (!check(sampler && c.setTrackInstrumentPlugin(track,*sampler),"sampler fixture")) return 1;
    const auto slot=c.project().findTrack(track)->instrument.id;
    check(c.loadSamplerSample(track,slot,wav),"sampler owns sample");
    c.setInsertParameter(track,slot,"pre.polarity",1);
    const auto midi=c.addMidiClip(track,4,2);
    daw::NoteModel note; note.id=daw::newUuid(); note.pitch=67; note.startBeats=.5; note.lengthBeats=.25;
    note.velocity=37; note.channel=9; note.releaseVelocity=73;
    c.setClipNotes(track,midi,{note},"Fixture notes");
    auto& sourceMidi=const_cast<daw::ClipModel&>(*clip(c,{track,midi}));
    daw::ControllerLane cc; cc.id=daw::newUuid(); cc.cc=64; cc.channel=9; cc.points={{0,.4},{1,.8}};
    daw::ControllerLane parameter; parameter.id=daw::newUuid(); parameter.cc=-1;
    parameter.slotId=slot; parameter.parameterId="pre.polarity"; parameter.points={{0,1}};
    sourceMidi.lanes={cc,parameter};
    std::string midiEntry;
    const auto depth=c.undoDepth();
    check(c.saveClipToLibrary({track,midi},midiEntry).isOk() && c.undoDepth()==depth+1,
        "saving a clip creates one undoable snapshot");
    check(clip(c,{track,midi}) && near(clip(c,{track,midi})->startSeconds,4),"archive drag leaves original placement intact");
    c.undo(); check(c.project().clipLibrary.empty(),"undo removes snapshot");
    c.redo(); check(c.libraryClip(midiEntry)!=nullptr,"redo restores snapshot");
    const auto& savedMidi=c.libraryClip(midiEntry)->tracks.front();
    check(!savedMidi.instrument.isLoaded() && savedMidi.inserts.empty() && savedMidi.samplerFx.inserts.empty() &&
        savedMidi.sends.empty() && savedMidi.parentId.empty(),"MIDI snapshot contains no instrument, channel FX or routing");
    check(savedMidi.clips.front().lanes.size()==1 && savedMidi.clips.front().lanes.front().cc==64 &&
        clip(c,{track,midi})->lanes.size()==2,
        "MIDI controllers survive without carrying source-plugin parameter automation");
    c.setClipNotes(track,midi,{},"Edit notes");
    check(c.libraryClip(midiEntry)->tracks.front().clips.front().notes.size()==1,"later note edits do not change snapshot");
    daw::EngineController::ClipAddress restored;
    check(c.restoreLibraryClip(midiEntry,track,8,restored).isOk() && restored.trackId==track &&
        clip(c,restored) && clip(c,restored)->notes.size()==1,"unchanged instrument reuses destination lane");
    if (const auto* back=clip(c,restored)) check(back->notes.front().id!=note.id && back->notes.front().channel==9 &&
        back->notes.front().releaseVelocity==73 && near(back->startSeconds,8),"MIDI identity, channels, release velocity and position");
    c.undo(); check(!clip(c,restored),"one undo removes restored clip");
    c.redo(); check(clip(c,restored)!=nullptr,"redo restores the same new clip id");
    c.setInsertParameter(track,slot,"pre.polarity",0);
    const auto trackCount=c.project().tracks.size();
    check(c.restoreLibraryClip(midiEntry,track,12,restored).isOk() && restored.trackId==track &&
        c.project().tracks.size()==trackCount && near(c.insertParameter(track,slot,"pre.polarity"),0),
        "changed MIDI preset stays on destination without a new track");
    const auto another=c.addTrack(daw::TrackKind::Midi,"Another instrument");
    c.setTrackInstrumentPlugin(another,*sampler);
    const auto anotherSlot=c.project().findTrack(another)->instrument.id;
    check(c.restoreLibraryClip(midiEntry,another,1,restored).isOk() && restored.trackId==another &&
        c.project().findTrack(another)->instrument.id==anotherSlot &&
        near(c.insertParameter(another,anotherSlot,"pre.polarity"),0),
        "MIDI phrase uses another track's existing instrument and preset");
    c.setInsertParameter(track,slot,"pre.polarity",1);
    auto& recorded=const_cast<daw::ClipModel&>(*clip(c,{track,midi}));
    daw::TakeModel midiTake; midiTake.id=daw::newUuid(); midiTake.lengthSeconds=2;
    midiTake.clipOffsetSeconds=.25; midiTake.notes={note};
    recorded.takes={midiTake};
    daw::CompSegment midiComp; midiComp.id=daw::newUuid(); midiComp.takeId=midiTake.id; midiComp.endSeconds=2;
    recorded.comp={midiComp};
    std::string second; check(c.saveClipToLibrary({track,midi},second).isOk(),"recorded MIDI snapshot");
    check(c.restoreLibraryClip(second,track,16,restored).isOk() && clip(c,restored)->takes.size()==1 &&
        clip(c,restored)->takes.front().notes.front().id!=note.id &&
        clip(c,restored)->comp.front().takeId==clip(c,restored)->takes.front().id &&
        near(clip(c,restored)->takes.front().clipOffsetSeconds,.25),
        "recorded MIDI take notes, comp ownership and punch-in offset survive");

    const auto audioTrack=c.addTrack(daw::TrackKind::Audio,"Audio");
    const auto audio=c.importAudio(wav,audioTrack,1);
    c.setClipSampleParameter(audioTrack,audio,"pre.reverse",1);
    c.setClipSampleParameter(audioTrack,audio,"rootnote",65);
    auto& audioModel=const_cast<daw::ClipModel&>(*clip(c,{audioTrack,audio}));
    daw::TakeModel take; take.id=daw::newUuid(); take.name="Take"; take.filePath=wav; take.lengthSeconds=.1;
    audioModel.takes.push_back(take);
    daw::CompSegment comp; comp.id=daw::newUuid(); comp.takeId=take.id; comp.endSeconds=.1;
    audioModel.comp.push_back(comp);
    // A loaded built-in processor supplies real opaque clip FX state.
    const auto effect=c.pluginManager().find(daw::plugins::Format::Internal,"daw.equalizer");
    if (effect) check(!c.addClipFxInsert(audioTrack,audio,*effect).empty(),"clip FX fixture");
    std::string audioEntry; check(c.saveClipToLibrary({audioTrack,audio},audioEntry).isOk(),"audio snapshot");
    c.removeClip(audioTrack,audio);
    check(c.restoreLibraryClip(audioEntry,audioTrack,3,restored).isOk(),"audio restored after original deletion");
    if (const auto* back=clip(c,restored)) {
        check(back->sampleEdit.reverse && back->sampleEdit.rootNote==65 && back->takes.front().filePath==wav &&
            back->takes.front().id!=take.id && back->comp.front().takeId==back->takes.front().id,
            "Sample Editor, recording takes and comp references survive");
        if (effect) check(!back->inserts.empty() && c.insertInstance(restored.trackId,back->inserts.front().id),"clip FX reinstantiated");
    }
    const auto pattern=c.addPattern("Beat");
    const auto child=c.addPatternSample(pattern,wav,0);
    c.setInsertParameter(child,c.project().findTrack(child)->instrument.id,"pre.polarity",1);
    const auto owner=c.project().findTrack(pattern)->clips.front().id;
    std::string patternEntry; check(c.saveClipToLibrary({pattern,owner},patternEntry).isOk(),"Pattern snapshot");
    check(c.libraryClip(patternEntry)->tracks.size()==2,"Pattern brings its owned child part");
    std::string patternEntry2;
    check(c.saveClipToLibrary({pattern,owner},patternEntry2).isOk(),"two Pattern snapshots share opaque sampler state");
    check(c.restoreLibraryClip(patternEntry,{},16,restored).isOk(),"Pattern restore creates independent hierarchy");
    const auto newOwner=restored.clipId;
    bool childLinked=false;
    for (const auto& t:c.project().tracks) if (t.parentId==restored.trackId)
        for (const auto& part:t.clips) childLinked |= part.patternClipId==newOwner && t.outputBusId==restored.trackId;
    check(childLinked,"restored Pattern ownership and bus routing point at copied objects");
    if (effect) {
        c.addInsert(pattern,*effect);
        check(c.restoreLibraryClip(patternEntry,pattern,20,restored).isOk() && restored.trackId!=pattern,
            "changed Pattern rack gets an independent saved channel");
    }
    daw::AutomationTarget target; target.channelId=audioTrack;
    const auto lane=c.addAutomationLane(audioTrack,target);
    const auto curve=c.addAutomationClip(lane,target,2,2);
    std::string automationEntry; check(c.saveClipToLibrary({lane,curve},automationEntry).isOk() &&
        c.restoreLibraryClip(automationEntry,lane,6,restored).isOk() && clip(c,restored)->automation.target.channelId==audioTrack,
        "automation keeps its target and curve");

    const auto moveA=c.addTrack(daw::TrackKind::Midi,"Move A"), moveB=c.addTrack(daw::TrackKind::Midi,"Move B");
    const auto first=c.addMidiClip(moveA,0,1), last=c.addMidiClip(moveA,2,1);
    const auto beforeMove=c.undoDepth();
    c.beginClipPositionEdit(); c.setClipStartSeconds(moveA,first,11); c.moveClipToTrack(moveA,first,moveB); c.cancelClipPositionEdit();
    check(c.undoDepth()==beforeMove && c.project().findTrack(moveA)->clips.front().id==first &&
        c.project().findTrack(moveA)->clips.back().id==last && near(clip(c,{moveA,first})->startSeconds,0) &&
        c.project().findTrack(moveB)->clips.empty(),"handoff to browser cancels provisional position, lane and order without undo");
    c.setTempo(60);
    check(c.restoreLibraryClip(midiEntry,{},0,restored,true).isOk() && near(clip(c,restored)->startSeconds,8) &&
        near(clip(c,restored)->durationSeconds,4),"original musical position and MIDI length adapt to current tempo");
    check(c.renameLibraryClip(midiEntry,"Alternative") && c.libraryClip(midiEntry)->name=="Alternative","rename saved clip");
    check(c.removeLibraryClip(second) && !c.libraryClip(second),"remove snapshot without touching timeline");
    c.undo(); check(c.libraryClip(second)!=nullptr,"snapshot deletion is undoable");

    // Remove every active track: all audio and sampler files must still be
    // packaged solely because archived content references them.
    std::vector<std::string> tracks;
    for (const auto& t:c.project().tracks) tracks.push_back(t.id);
    for (const auto& id:tracks) c.removeTrack(id);
    const auto package=root/"Project";
    check(c.saveProject(package.string()).isOk(),"project packages archived-only audio and sampler resources");
    const auto moved=root/"Moved"; fs::rename(package,moved); fs::remove(wav);
    daw::EngineController loaded{}; loaded.initialize(48000,256,false);
    check(loaded.openProject(moved.string()).isOk() && loaded.project().clipLibrary.size()==c.project().clipLibrary.size(),
        "project-local library survives save, relocation and reopen");
    check(loaded.restoreLibraryClip(midiEntry,{},0,restored,true).isOk(),"missing original MIDI lane is recreated");
    const auto* midiTrack=loaded.project().findTrack(restored.trackId);
    check(midiTrack && !midiTrack->instrument.isLoaded(),"restoring MIDI to a new lane leaves its instrument empty");
    check(loaded.restoreLibraryClip(patternEntry,{},0,restored).isOk(),"Pattern restores its connected instruments");
    bool samplerRestored=false;
    for (const auto& t:loaded.project().tracks) if (t.parentId==restored.trackId) {
        const auto* instance=loaded.samplerInstance(t.id,t.instrument.id);
        samplerRestored |= instance && fs::is_regular_file(instance->samplePath()) &&
            near(loaded.insertParameter(t.id,t.instrument.id,"pre.polarity"),1);
    }
    check(samplerRestored,"Pattern sampler uses packaged sample and captured state");
    check(loaded.restoreLibraryClip(audioEntry,{},0,restored,true).isOk() && fs::is_regular_file(clip(loaded,restored)->filePath),
        "archived audio remains portable without original source file");
    const auto recovery=loaded.captureIncrementalRecoverySnapshot({},true);
    check(!recovery.project.clipLibrary.empty() && !recovery.pluginStates.empty(),"incremental recovery carries archive metadata and opaque state");
    daw::ProjectModel disk;
    daw::ProjectSerializer::load(disk,moved.string());
    const auto savedPattern=std::find_if(disk.clipLibrary.begin(),disk.clipLibrary.end(),
        [&](const auto& entry){return entry.id==patternEntry;});
    if (!check(savedPattern!=disk.clipLibrary.end(),"saved Pattern metadata")) return 1;
    check(savedPattern->tracks[1].instrument.stateFile !=
        loaded.libraryClip(patternEntry)->tracks[1].instrument.stateFile,
        "sampler path normalization gives recovery state a new content identity");
    const auto projection=daw::cloud::projectForCloudSnapshotV1(loaded.project());
    check(!projection.valid() && projection.document.clipLibrary.empty() &&
        daw::cloud::containsLocalPathOrUiState(loaded.project()),
        "unsupported cloud publication cannot silently drop library or expose local paths");

    // Incomplete/corrupt state must not turn a saved sound into a default preset.
    const auto stateDir=fs::path(daw::ProjectSerializer::statePath(moved.string()));
    const auto stateFile=stateDir/savedPattern->tracks[1].instrument.stateFile;
    {
        std::ofstream stream(stateFile,std::ios::binary|std::ios::trunc); stream << "broken state";
    }
    daw::EngineController damaged{}; damaged.initialize(48000,256,false);
    check(damaged.openProject(moved.string()).isOk(),"project still opens with damaged archived state");
    const auto damagedDepth=damaged.undoDepth();
    check(!damaged.restoreLibraryClip(patternEntry,{},0,restored) && damaged.project().tracks.empty() &&
        damaged.undoDepth()==damagedDepth,"failed plugin restore rolls back without an undo entry");
    fs::remove(stateFile);
    damaged.newProject(); damaged.openProject(moved.string());
    check(!damaged.restoreLibraryClip(patternEntry,{},0,restored) && damaged.project().tracks.empty(),
        "missing archived plugin state fails before changing timeline");
    check(!damaged.saveProject((root/"Incomplete").string()),
        "saving cannot silently discard missing archived plugin state");
    loaded.newProject(); check(loaded.project().clipLibrary.empty(),"new project starts with its own empty library");
    fs::remove_all(root);
    std::printf("%d failures\n",failures);
    return failures?1:0;
}
