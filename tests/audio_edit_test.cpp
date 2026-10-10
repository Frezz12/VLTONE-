#include "AudioEdit.hpp"
#include "EngineController.hpp"
#include "ProjectSerializer.hpp"
#include "model/ProjectMemory.hpp"
#include "Recording/RecordingEngine.hpp"
#include "Core/AudioBuffer.hpp"
#include "platform/AudioFileDecoder.hpp"
#include "Internal/SamplerInstance.hpp"
#include <filesystem>
#include <nlohmann/json.hpp>
#include <cstdio>
#include <cmath>
#include <chrono>

using namespace daw;
static int failures=0;
static bool check(bool ok,const char* text){std::printf("%s %s\n",ok?"PASS":"FAIL",text);if(!ok)++failures;return ok;}
int main(){
    std::setvbuf(stdout,nullptr,_IONBF,0);
    auto raw=std::make_shared<engine::SampleBuffer>(2,16,8000);
    for(int i=0;i<16;++i){raw->writableChannel(0)[i]=float(i+1)/32;raw->writableChannel(1)[i]=-float(i+1)/64;}
    auto decode=[raw](const AudioEditSource&){return raw;};
    auto original=audioedit::fromSource({"source","/test.wav",8000,16,2});
    auto doc=original;
    auto enormous=original;enormous.regions.clear();enormous.frames=AudioEditFrame(1)<<32;
    check(!audioedit::render(enormous,decode),"oversize render cannot truncate frame allocation");
    auto adjusted=original;audioedit::gain(adjusted,4,8,-6.020599913279624);
    auto quieter=audioedit::render(adjusted,decode);
    check(std::abs(quieter->channel(0)[4]-raw->channel(0)[4]*.5)<1e-7&&
        quieter->channel(1)[8]==raw->channel(1)[8],"range gain changes both channels and preserves boundaries");
    check(std::abs(audioedit::peak(*raw,0,16)-.5)<1e-7,"normalization uses one peak across stereo channels");
    check(audioedit::silence(doc,4,8),"delete middle");
    auto rendered=audioedit::render(doc,decode);
    check(rendered&&rendered->frames()==16&&rendered->channel(0)[4]==0&&rendered->channel(0)[8]==raw->channel(0)[8],"delete leaves exact silence without ripple");
    check(raw->channel(0)[4]!=0,"source bytes stay unchanged");
    check(audioedit::silence(doc,0,16)&&doc.frames==16&&doc.regions.empty(),"delete all preserves duration");
    rendered=audioedit::render(doc,decode);
    check(rendered&&audioedit::peak(*rendered,0,16)==0,"silent document renders safely");
    doc=original;audioedit::fade(doc,0,16,true,8,.7);auto faded=audioedit::render(doc,decode);
    audioedit::split(doc,5);auto split=audioedit::render(doc,decode);
    bool identical=true;for(int ch=0;ch<2;++ch)for(int i=0;i<16;++i)identical&=std::abs(faded->channel(ch)[i]-split->channel(ch)[i])<1e-7;
    check(identical,"splitting a curved fade preserves every sample");
    audioedit::reverse(doc,2,13);audioedit::reverse(doc,2,13);auto twice=audioedit::render(doc,decode);
    identical=true;for(int ch=0;ch<2;++ch)for(int i=0;i<16;++i)identical&=std::abs(faded->channel(ch)[i]-twice->channel(ch)[i])<1e-7;
    check(identical,"reverse twice restores audio and curved envelopes");
    doc=original;audioedit::split(doc,4);auto first=doc.regions.front().id;
    check(audioedit::move(doc,first,8)&&doc.frames==16,"move without time stretch or duration change");
    rendered=audioedit::render(doc,decode);
    check(rendered->channel(0)[0]==0&&rendered->channel(0)[8]==raw->channel(0)[0]&&rendered->channel(0)[12]==raw->channel(0)[12],"move replaces destination and keeps neighbours");
    const auto copied=audioedit::copy(original,0,4);
    doc=original;audioedit::paste(doc,copied,8,14);rendered=audioedit::render(doc,decode);
    check(rendered->channel(0)[8]==raw->channel(0)[0]&&rendered->channel(0)[12]==0&&rendered->channel(0)[14]==raw->channel(0)[14],"paste clears longer selection without ripple");
    check(audioedit::paste(doc,copied,16,16)&&doc.frames==20,"paste extends source content explicitly");
    AudioEditDocument roundtrip;
    check(audioedit::fromJson(audioedit::toJson(doc),roundtrip)&&roundtrip==doc,"edit document roundtrip");
    auto bad=audioedit::toJson(doc);bad["regions"][0]["length"]=-1;
    check(!audioedit::fromJson(bad,roundtrip),"invalid regions rejected atomically");
    auto differentRate=copied;differentRate.sampleRate=16000;differentRate.frames=8;differentRate.regions[0].length=8;
    doc=original;check(audioedit::paste(doc,differentRate,8,8)&&audioedit::valid(doc),"cross-rate paste retains source timing");
    auto reverseCopy=copied;audioedit::reverse(reverseCopy,0,reverseCopy.frames);
    auto highRate=original;highRate.sampleRate=48000;highRate.frames=96;highRate.regions.front().length=96;
    check(audioedit::paste(highRate,reverseCopy,48,48)&&audioedit::valid(highRate),"reversed paste at a higher rate retains valid source bounds");
    bool rateBounds=true;
    for(double from:{8000.,44100.,48000.,192000.})for(double to:{8000.,44100.,48000.,192000.})for(int length:{1,3,11,31}) {
        auto shortSource=audioedit::fromSource({"rate-source","rate.wav",from,length,2});
        audioedit::reverse(shortSource,0,length);
        auto destination=audioedit::fromSource({"rate-dest","dest.wav",to,64,2});
        if(audioedit::paste(destination,shortSource,4,4))rateBounds&=audioedit::valid(destination);
    }
    check(rateBounds,"short reversed pastes preserve valid bounds across sample rates");

    ProjectModel project;TrackModel track;track.id="track";track.kind=TrackKind::Midi;
    ClipModel a;a.id="a";a.contentId="phrase";a.kind=ClipKind::Midi;a.notes.push_back({"note",60,0,1,100});
    ClipModel b=a;b.id="b";b.startSeconds=4;track.clips.push_back(a);track.clips.push_back(b);project.tracks.push_back(track);project.resolveClipContents();
    auto& left=project.tracks[0].clips[0];auto& right=project.tracks[0].clips[1];
    left.notes[0].pitch=72;
    check(right.notes[0].pitch==72&&left.contentStorage()==right.contentStorage(),"placements use one canonical content object");
    auto snapshot=project;snapshot.tracks[0].clips[0].notes[0].pitch=48;
    check(left.notes[0].pitch==72&&snapshot.tracks[0].clips[1].notes[0].pitch==48,"project snapshot isolates content but preserves internal links");
    right.detachContent();right.notes[0].pitch=65;check(left.notes[0].pitch==72,"detach preserves independent editing");
    right.contentId=left.contentId;right.bindContent(left.contentStorage());
    std::string json;check(ProjectSerializer::serializeDocument(project,json).isOk(),"serialize shared content registry");
    ProjectModel loaded;check(ProjectSerializer::deserializeDocument(loaded,json).isOk(),"load shared content registry");
    loaded.tracks[0].clips[0].notes[0].pitch=80;
    check(loaded.tracks[0].clips[1].notes[0].pitch==80,"links survive save and reload");
    {
        ProjectModel many;TrackModel parents,parts;
        parents.id="parents";parents.kind=TrackKind::Pattern;
        parts.id="parts";parts.kind=TrackKind::Midi;
        ClipModel root;root.id="root";root.kind=ClipKind::Pattern;root.contentId="composition";
        root.durationSeconds=4;root.contentStorage()->patternInitialized=true;
        for(int i=0;i<8;++i) {
            ClipModel child;child.id="part-"+std::to_string(i);child.kind=ClipKind::Midi;
            child.patternClipId=root.id;child.patternPartId=child.id;child.contentId=child.id;
            child.durationSeconds=1;child.notes={{newUuid(),60+i,0,1,100}};
            root.contentStorage()->patternParts.push_back({child.id,parts.id,child.contentId,double(i),2,0});
            parts.clips.push_back(std::move(child));
        }
        parents.clips.push_back(root);
        for(int i=1;i<200;++i){auto peer=root;peer.id="root-"+std::to_string(i);peer.startSeconds=i*4.;parents.clips.push_back(std::move(peer));}
        many.tracks.push_back(std::move(parents));many.tracks.push_back(std::move(parts));
        const auto start=std::chrono::steady_clock::now();
        many.resolveClipContents();many.resolveClipContents();
        const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        check(many.tracks.back().clips.size()==1600&&many.clipContents.size()==9,
            "many linked Patterns retain one composition and one source per part");
        std::printf("Pattern resolver: 1600 views, %.2f ms for creation and refresh\n",elapsed);
        const auto beforeMemory=estimatedProjectBytes(many);
        auto& sharedNotes=many.tracks.back().clips.front().notes;
        sharedNotes.resize(2000);
        const auto noteGrowth=sharedNotes.capacity()*sizeof(NoteModel);
        const auto afterMemory=estimatedProjectBytes(many);
        check(afterMemory>beforeMemory && afterMemory-beforeMemory<noteGrowth*3,
            "undo memory accounts shared Pattern notes once across 200 views");
    }

    EngineController controller;check(controller.initialize(48000,512,false).isOk(),"offline controller initializes");
    auto t=controller.addTrack(TrackKind::Midi,"Notes");auto c=controller.addMidiClip(t,0,2);
    auto n=controller.addNote(t,c,60,0,1);auto copies=controller.duplicateLinkedClips({{t,c}});
    check(copies.size()==1&&controller.linkedClips({t,c}).size()==2,"linked MIDI duplication");
    controller.setClipNotes(t,c,{{n,67,0,1,100}},"Transpose");
    const auto read=[&](const std::string& id)->const ClipModel*{for(const auto& clip:controller.project().findTrack(t)->clips)if(clip.id==id)return &clip;return nullptr;};
    check(!copies.empty()&&read(copies[0].clipId)->notes[0].pitch==67,"controller edit propagates through canonical data");
    controller.undo();check(read(c)->notes[0].pitch==60&&read(copies[0].clipId)->notes[0].pitch==60,"undo changes every linked placement");
    auto independent=controller.duplicateClip(t,c);check(read(independent)->contentId!=read(c)->contentId,"ordinary duplication remains independent");
    check(controller.makeClipsIndependent(copies),"detach command");
    check(controller.linkedClips({t,c}).size()==1,"detach removes link");controller.undo();
    check(controller.linkedClips({t,c}).size()==2,"undo restores link");
    const auto splitId=controller.splitClip(t,c,1);
    check(!splitId.empty()&&read(splitId)->contentId==read(c)->contentId&&
        read(c)->notes==read(copies[0].clipId)->notes&&read(splitId)->contentOffsetBeats==2,
        "linked split creates content windows without truncating notes");
    controller.undo();check(!read(splitId)&&read(c)->durationSeconds==2,"undo linked split restores placement");
    controller.removeClip(t,c);controller.undo();
    check(read(c)&&read(c)->contentStorage()==read(copies[0].clipId)->contentStorage(),
        "undo deletion rejoins the canonical content rather than a detached snapshot");
    controller.makeClipsIndependent({{t,c},copies[0]});
    check(read(c)->contentId!=read(copies[0].clipId)->contentId,
        "detaching several selected peers makes each one independent");
    controller.undo();

    {
        const auto laneId=controller.addTrack(TrackKind::Midi,"Linked glue");
        const auto phrase=controller.addMidiClip(laneId,0,2);
        controller.setClipNotes(laneId,phrase,{{newUuid(),60,0,.5,100},{newUuid(),72,2,.5,100}},"Glue fixture");
        const auto linked=controller.duplicateLinkedClips({{laneId,phrase}});
        const auto tail=controller.splitClip(laneId,phrase,1);
        std::string glued;
        check(controller.glueClips({{laneId,phrase},{laneId,tail}},glued).isOk(),"glue linked MIDI windows");
        const auto* track=controller.project().findTrack(laneId);
        const auto result=std::find_if(track->clips.begin(),track->clips.end(),[&](const auto& clip){return clip.id==glued;});
        check(result!=track->clips.end()&&result->notes.size()==2&&result->notes.back().startBeats==2&&
            result->contentOffsetBeats==0&&controller.linkedClips({laneId,glued}).size()==1,
            "MIDI glue bakes only audible windows into independent content");
        check(controller.linkedClips(linked.front()).size()==1,"MIDI glue leaves the unselected linked peer intact");
        controller.undo();
        check(controller.linkedClips({laneId,phrase}).size()==3,"undo MIDI glue restores both linked windows");
        EngineController::FinalizedRecordingTrack recording;
        recording.midi=true;recording.midiTempo=120;recording.passes={{1,1.25,0}};
        recording.performance.notes={{newUuid(),84,0,.5,100}};
        recording.semantics.mode=RecordMode::Overwrite;
        recording.semantics.midiOverdubMerge=true;
        const auto overdub=controller.midiRecordingLanding(*controller.project().findTrack(laneId),recording);
        bool sharedRecording=overdub.size()==3;
        for(const auto& clip:overdub)sharedRecording&=clip.notes.size()==3&&
            std::any_of(clip.notes.begin(),clip.notes.end(),[](const auto& n){return n.pitch==84&&n.startBeats==2;})&&
            std::any_of(clip.notes.begin(),clip.notes.end(),[](const auto& n){return n.pitch==60&&n.startBeats==0;});
        check(sharedRecording,"recording into a linked window preserves hidden notes and updates all views");
        recording.semantics.midiOverdubMerge=false;
        const auto overwrite=controller.midiRecordingLanding(*controller.project().findTrack(laneId),recording);
        check(overwrite.size()==3&&overwrite.front().notes.size()==2&&
            overwrite.front().notes.front().pitch==60,
            "overwrite changes the shared source range without splitting instance placements");
        TrackModel delayed;delayed.id=laneId;
        for(const auto& clip:controller.project().findTrack(laneId)->clips)
            if(clip.id==linked.front().clipId)delayed.clips.push_back(clip);
        recording.semantics.midiOverdubMerge=true;
        recording.passes={{1,4.5,0}};recording.performance.notes={{newUuid(),84,0,7,100}};
        const auto extended=controller.midiRecordingLanding(delayed,recording);
        check(extended.size()==2&&extended.front().startSeconds==2&&extended.front().durationSeconds==2.5&&
            extended.back().startSeconds==1&&extended.back().durationSeconds==1&&
            !extended.back().notes.empty(),"recording across a shared source boundary retains the leading performance and new tail");
    }

    auto lane=controller.addTrack(TrackKind::Automation,"Curve");
    AutomationTarget target;target.kind=AutomationTargetKind::TrackVolume;target.channelId=t;
    auto curve=controller.addAutomationClip(lane,target,0,2);
    auto curveCopies=controller.duplicateLinkedClips({{lane,curve}});
    const auto curveAt=[&](const std::string& id)->const ClipModel*{for(const auto& clip:controller.project().findTrack(lane)->clips)if(clip.id==id)return &clip;return nullptr;};
    check(curveCopies.size()==1,"linked automation duplication");
    if(!curveCopies.empty()) {
        auto pan=target;pan.kind=AutomationTargetKind::TrackPan;
        const auto initialPoints=curveAt(curve)->automation.points;
        controller.setAutomationTarget(lane,curveCopies[0].clipId,pan);
        check(curveAt(curve)->automation.points==initialPoints,"retargeting an inactive linked curve preserves the shared shape");
        controller.setAutomationPoints(lane,curve,{{0,.1},{4,.9}},true);
        check(curveAt(curveCopies[0].clipId)->automation.points==curveAt(curve)->automation.points&&
            curveAt(curve)->automation.target==target&&curveAt(curveCopies[0].clipId)->automation.target==pan,
            "automation shares shape while destinations stay independent");
        const auto points=curveAt(curve)->automation.points;
        controller.beginClipTrimEdit(lane,curve);
        controller.setClipTrim(lane,curve,.5,0,1.5);
        controller.endClipTrimEdit("Trim curve");
        check(curveAt(curve)->automation.points==points&&curveAt(curveCopies[0].clipId)->startSeconds==2,
            "linked automation trim leaves shared shape and other placement intact");
        controller.undo();check(curveAt(curve)->startSeconds==0&&curveAt(curve)->contentOffsetBeats==0,
            "automation window undo restores content offset");
        const auto otherLane=controller.addTrack(TrackKind::Automation,"Linked pan curve");
        controller.beginClipPositionEdit();
        controller.moveClipToTrack(lane,curveCopies.front().clipId,otherLane);
        controller.endClipPositionEdit("Move curve instance");
        controller.captureIncrementalRecoverySnapshot({},true);
        controller.setAutomationPoints(lane,curve,{{0,.2},{4,.8}},true);
        const auto recovered=controller.captureIncrementalRecoverySnapshot({},false);
        bool current=false;
        for(const auto& track:recovered.trackParts)if(track->id==otherLane)
            current=!track->clips.empty()&&track->clips.front().automation.points.front().value==.2;
        check(current,"automation recovery refreshes linked shapes with different destinations on other lanes");
        const auto tail=controller.splitClip(lane,curve,.5);
        std::string joined;
        check(controller.glueClips({{lane,curve},{lane,tail}},joined).isOk()&&curveAt(joined)&&
            curveAt(joined)->contentOffsetBeats==0&&controller.linkedClips({lane,joined}).size()==1&&
            std::abs(automationValueAt(curveAt(joined)->automation.points,2,.2)-.5)<1e-9,
            "automation glue resolves source windows into an independent curve");
    }
    const auto sampler=controller.pluginManager().find(plugins::Format::Internal,"daw.sampler");
    if(check(bool(sampler),"pattern sampler fixture")) {
        const auto pattern=controller.addPattern("Pattern");
        const auto child=controller.addPatternInstrument(pattern,*sampler);
        const auto parentClip=controller.project().findTrack(pattern)->clips.front().id;
        const auto childClip=controller.project().findTrack(child)->clips.front().id;
        auto patternCopies=controller.duplicateLinkedClips({{pattern,parentClip}});
        check(patternCopies.size()==1,"linked pattern duplication");
        const auto peers=controller.linkedClips({child,childClip});
        check(peers.size()==2,"pattern child content is linked");
        controller.setClipNotes(child,childClip,{{newUuid(),74,0,1,91}},"Pattern notes");
        bool all=true;for(const auto& clip:controller.project().findTrack(child)->clips)all&=clip.notes.size()==1&&clip.notes[0].pitch==74;
        check(all,"pattern edit reaches linked child performances");
        const auto rootAt=[&](const std::string& id)->const ClipModel* {
            for(const auto& root:controller.project().findTrack(pattern)->clips)if(root.id==id)return &root;
            return nullptr;
        };
        const auto partAt=[&](const std::string& id)->const ClipModel* {
            for(const auto& part:controller.project().findTrack(child)->clips)if(part.id==id)return &part;
            return nullptr;
        };
        controller.beginClipPositionEdit();controller.setClipStartSeconds(child,childClip,.25);
        controller.endClipPositionEdit("Move Pattern part");
        check(partAt(peers.back().clipId)&&std::abs(partAt(peers.back().clipId)->startSeconds-rootAt(patternCopies[0].clipId)->startSeconds-.25)<1e-9,
            "pattern part positions belong to the common composition");
        controller.undo();check(partAt(childClip)->startSeconds==0,"pattern part move undo restores composition");
        controller.beginClipPositionEdit();controller.setClipStartSeconds(child,childClip,.5);controller.cancelClipPositionEdit();
        check(partAt(childClip)->startSeconds==0,"cancel Pattern part gesture restores common composition");
        const auto patternRecoveryBefore=controller.captureIncrementalRecoverySnapshot({},true);
        const auto extra=controller.addMidiClip(child,.5,.5,parentClip);
        check(!extra.empty()&&controller.project().findTrack(child)->clips.size()==4,
            "adding a Pattern part materializes every linked instance");
        const auto patternRecoveryAfter=controller.captureIncrementalRecoverySnapshot({},false);
        bool oldPart=false,newParts=false;
        for(const auto& lane:patternRecoveryBefore.trackParts)if(lane->id==pattern)
            oldPart=lane->clips.front().contentStorage()->patternParts.size()==1;
        for(const auto& lane:patternRecoveryAfter.trackParts)if(lane->id==pattern)
            newParts=lane->clips.front().contentStorage()->patternParts.size()==2;
        check(oldPart&&newParts,"Pattern composition participates in incremental recovery without changing old snapshots");
        controller.removeClip(child,extra);
        check(controller.project().findTrack(child)->clips.size()==2,"removing a Pattern part removes peer views without deleting instruments");
        controller.undo();check(controller.project().findTrack(child)->clips.size()==4,"undo restores removed Pattern part in every instance");
        controller.undo();check(controller.project().findTrack(child)->clips.size()==2,"undo creation removes a whole Pattern part");
        controller.redo();check(controller.project().findTrack(child)->clips.size()==4,"redo creation reuses the canonical part identity");
        controller.undo();
        const auto duplicate=controller.duplicateClip(child,childClip);
        check(!duplicate.empty()&&controller.project().findTrack(child)->clips.size()==4&&partAt(duplicate)->contentId!=partAt(childClip)->contentId,
            "ordinary Pattern part duplicate adds independent content to the composition");
        controller.undo();check(controller.project().findTrack(child)->clips.size()==2,"undo duplicated part removes all generated views");
        const auto splitPart=controller.splitClip(child,childClip,.5);
        check(!splitPart.empty()&&controller.project().findTrack(child)->clips.size()==4&&partAt(splitPart)->contentOffsetBeats==1,
            "splitting a Pattern part updates every composition view without changing notes");
        controller.undo();check(controller.project().findTrack(child)->clips.size()==2,"undo Pattern split restores one part per instance");
        const auto copiedTrack=controller.duplicateTrack(child,true);
        check(!copiedTrack.empty()&&controller.project().findTrack(copiedTrack)->clips.size()==2,
            "duplicating an instrument lane adds one shared composition part per source part");
        controller.undo();check(!controller.project().findTrack(copiedTrack)&&rootAt(parentClip)->contentStorage()->patternParts.size()==1,
            "undo duplicated lane removes its canonical Pattern membership");
        controller.removeTrack(child);controller.undo();
        check(controller.project().findTrack(child)&&controller.project().findTrack(child)->clips.size()==2,
            "undo instrument removal restores canonical Pattern membership");
        const auto splitRoot=controller.splitClip(pattern,parentClip,.25);
        std::string gluedPattern;
        check(controller.glueClips({{pattern,parentClip},{pattern,splitRoot}},gluedPattern).isOk(),
            "glue split Pattern windows");
        bool bounded=true;std::size_t gluedParts=0;
        for(const auto& part:controller.project().findTrack(child)->clips)if(part.patternClipId==gluedPattern) {
            ++gluedParts;
            for(const auto& note:part.notes)bounded&=note.startBeats>=0&&
                note.startBeats+note.lengthBeats<=secondsToBeats(part.durationSeconds,controller.tempo())+1e-9;
            bounded&=part.offsetSeconds==0&&part.contentOffsetBeats==0&&controller.linkedClips({child,part.id}).size()==1;
        }
        check(gluedParts==2&&bounded,"Pattern glue crops child windows and removes hidden links");
        controller.undo();controller.undo();
        std::string savedPattern;ProjectModel loadedPattern;
        check(ProjectSerializer::serializeDocument(controller.project(),savedPattern).isOk()&&
            ProjectSerializer::deserializeDocument(loadedPattern,savedPattern).isOk()&&
            loadedPattern.findTrack(child)->clips.size()==2,
            "Pattern composition and child content survive project roundtrip");
        auto damagedPattern=nlohmann::json::parse(savedPattern);
        damagedPattern["clipContents"][rootAt(parentClip)->contentId]["patternParts"][0]["trackId"]="missing-lane";
        ProjectModel rejectedPattern;
        check(!ProjectSerializer::deserializeDocument(rejectedPattern,damagedPattern.dump()).isOk(),
            "dangling Pattern parts fail to load without silently losing music");
        check(controller.makeClipsIndependent({{child,childClip}})&&
            controller.linkedClips({child,childClip}).size()==1&&controller.linkedClips({pattern,parentClip}).size()==1,
            "detaching a nested part also separates its containing Pattern composition");
        controller.undo();
        check(controller.linkedClips({pattern,parentClip}).size()==2&&controller.linkedClips({child,childClip}).size()==2,
            "undo nested detachment restores the complete shared tree");
        controller.makeClipsIndependent(patternCopies);
        check(controller.linkedClips({child,childClip}).size()==1,"pattern detachment separates the child content tree");
    }
    namespace fs=std::filesystem;
    const auto root=fs::temp_directory_path()/("vlt-audio-edit-"+newUuid());fs::create_directories(root);
    const auto file=(root/"source.wav").string();
    audio::AudioBuffer tone(2,4800);
    for(unsigned ch=0;ch<2;++ch)for(unsigned i=0;i<4800;++i)tone.getChannel(ch)[i]=float(ch+1)*.2f*std::sin(i*.03f);
    check(audio::AudioRecorder::writeWAVFile(file,tone,48000).isOk(),"audio fixture");
    const auto audioTrack=controller.addTrack(TrackKind::Audio,"Audio");
    const auto audioClip=controller.importAudio(file,audioTrack,0);
    const EngineController::AudioEditTarget audioTarget{audioTrack,audioClip,false};
    auto audioCopies=controller.duplicateLinkedClips({{audioTrack,audioClip}});
    check(audioCopies.size()==1,"linked audio duplication");
    auto edited=controller.audioEditDocument(audioTarget);
    check(edited.initialized,"editor resolves original audio source");
    audioedit::silence(edited,1200,2400);
    auto prepared=EngineController::renderAudioEdit(edited);
    check(prepared&&controller.setAudioEdit(audioTarget,edited,prepared,"Erase middle"),"prepared audio edit commits");
    if(!audioCopies.empty()) {
        const auto other=controller.audioEditDocument({audioTrack,audioCopies[0].clipId,false});
        check(other==edited,"audio edits share a document across linked instances");
        const auto length=controller.audioClip(audioTrack,audioCopies[0].clipId)->durationSeconds;
        controller.setClipGain(audioTrack,audioClip,.3f);
        check(controller.audioClip(audioTrack,audioCopies[0].clipId)->gain==1&&
            controller.audioClip(audioTrack,audioCopies[0].clipId)->durationSeconds==length,
            "audio instance gain and placement remain independent");
    }
    {
        // Warp coordinates belong to each placement, including after a knife
        // cut and an editor selection trim. Shared audio must remain intact.
        auto& mutableProject=const_cast<ProjectModel&>(controller.project());
        auto& instance=*std::find_if(mutableProject.findTrack(audioTrack)->clips.begin(),
            mutableProject.findTrack(audioTrack)->clips.end(),[&](const auto& item){return item.id==audioClip;});
        instance.warp.enabled=true;instance.warp.baselineDurationSeconds=.1;
        instance.warp.markers={{newUuid(),0,0,true},{newUuid(),.025,.1,false},{newUuid(),.1,.2,true}};
        const auto warpBefore=instance.warp;
        const auto rightId=controller.splitClip(audioTrack,audioClip,.05);
        const auto* rightPart=controller.audioClip(audioTrack,rightId);
        check(rightPart&&!rightPart->warp.empty()&&std::abs(rightPart->warp.markers.front().sourceSeconds-.025)<1e-9,
            "linked warp split preserves the source time at the cut");
        controller.undo();
        check(controller.audioClip(audioTrack,audioClip)->warp==warpBefore,"undo restores unsplit warp map");
        check(controller.setAudioEditWindow(audioTarget,.025,.1,"Trim warped selection"),"editor trims a warped source range");
        const auto* trimmed=controller.audioClip(audioTrack,audioClip);
        check(trimmed&&trimmed->startSeconds==0&&std::abs(trimmed->offsetSeconds-.025)<1e-9&&
            std::abs(trimmed->durationSeconds-.05)<1e-9,"warped editor trim preserves placement and maps source duration");
        controller.undo();
        auto& restored=*std::find_if(mutableProject.findTrack(audioTrack)->clips.begin(),
            mutableProject.findTrack(audioTrack)->clips.end(),[&](const auto& item){return item.id==audioClip;});
        restored.warp={};
    }
    check(controller.setAudioEditWindow(audioTarget,120./48000,121./48000,"Trim one sample")&&
        std::abs(controller.audioClip(audioTrack,audioClip)->durationSeconds-1./48000)<1e-12,
        "editor trim retains a single sample without timeline minimum rounding");
    controller.undo();
    check(controller.beginAudioEdit(audioTarget),"audio transaction begins");
    auto reversed=edited;audioedit::reverse(reversed,0,4800);
    auto reversedAudio=EngineController::renderAudioEdit(reversed);
    check(controller.updateAudioEdit(reversed,reversedAudio),"audio transaction previews");
    controller.cancelAudioEdit();check(controller.audioEditDocument(audioTarget)==edited,"Escape restores audio transaction baseline");
    {
        const auto snapshot=controller.captureIncrementalRecoverySnapshot({},true);
        auto quieter=edited;audioedit::gain(quieter,0,1200,-6);
        check(controller.setAudioEdit(audioTarget,quieter,EngineController::renderAudioEdit(quieter),"Recovery edit"),
            "recovery fixture edits canonical audio");
        const auto changed=controller.captureIncrementalRecoverySnapshot({},false);
        bool immutable=false,current=false;
        for(const auto& track:snapshot.trackParts)for(const auto& clip:track->clips)
            if(clip.id==audioClip)immutable=clip.audioEdit==edited;
        for(const auto& track:changed.trackParts)for(const auto& clip:track->clips)
            if(clip.id==audioClip)current=clip.audioEdit==quieter;
        check(immutable&&current,"incremental recovery invalidates edited content and keeps old snapshots immutable");
        controller.undo();
        std::string entry;EngineController::ClipAddress restored;
        check(controller.saveClipToLibrary({audioTrack,audioClip},entry).isOk()&&
            controller.restoreLibraryClip(entry,audioTrack,1,restored).isOk()&&
            controller.audioEditDocument({restored.trackId,restored.clipId,false})==edited&&
            controller.linkedClips(restored).size()==1,
            "clip library restores edited audio as independent content");
    }
    {
        auto* instance=const_cast<ClipModel*>(controller.audioClip(audioTrack,audioClip));
        const auto originalVersion=newUuid(),printedVersion=newUuid();
        instance->offlineHistory.push_back({originalVersion,{},"Source",captureClipAudioVersion(*instance)});
        ClipAudioVersionSource printed;printed.filePath=file;printed.durationSeconds=.1;printed.channels=2;
        instance->offlineHistory.push_back({printedVersion,originalVersion,"Printed",printed});
        instance->offlineVersionId=originalVersion;
        check(controller.selectOfflineRenderVersion({audioTrack,audioClip},printedVersion).isOk(),
            "linked instance can select an offline version");
        check(controller.audioEditDocument(audioTarget)==edited&&controller.offlineProcessCacheValid({audioTrack,audioClip})&&
            controller.audioClip(audioTrack,audioCopies.front().clipId)->offlineProcess.empty(),
            "offline version leaves canonical audio and linked peer processing unchanged");
        check(controller.restoreOfflineRenderOriginal({audioTrack,audioClip}).isOk()&&
            controller.audioEditDocument(audioTarget)==edited&&controller.audioClip(audioTrack,audioClip)->offlineProcess.empty(),
            "restoring the instance source retains shared editor content");
    }
    if(sampler) {
        auto instrument=controller.addTrack(TrackKind::Midi,"Edited sampler");
        controller.setTrackInstrumentPlugin(instrument,*sampler);
        const auto slot=controller.project().findTrack(instrument)->instrument.id;
        controller.loadSamplerSample(instrument,slot,file);
        const EngineController::AudioEditTarget slotTarget{instrument,slot,true};
        auto sampleDoc=controller.audioEditDocument(slotTarget);audioedit::reverse(sampleDoc,0,sampleDoc.frames);
        auto samples=EngineController::renderAudioEdit(sampleDoc);
        check(controller.setAudioEdit(slotTarget,sampleDoc,samples,"Reverse instrument"),"instrument uses the same audio document mechanism");
        check(controller.audioEditDocument(audioTarget)==edited,"loading the same file into an instrument does not create a link");
    }
    {
        EngineController sound;
        sound.initialize(48000,256,false);
        const auto lane=sound.addTrack(TrackKind::Audio,"Rendered edits");
        const auto clip=sound.importAudio(file,lane,0);
        const EngineController::AudioEditTarget target{lane,clip,false};
        const auto peers=sound.duplicateLinkedClips({{lane,clip}});
        auto document=sound.audioEditDocument(target);
        audioedit::silence(document,1200,2400);
        check(peers.size()==1&&sound.setAudioEdit(target,document,EngineController::renderAudioEdit(document),"Erase"),
            "playback fixture publishes one edited revision to both copies");
        audio::AudioBuffer input(2,256),output(2,256);input.clear();
        std::vector<float> live;
        sound.seekSeconds(0);sound.play();bool rendered=true;
        for(int block=0;block<40;++block){
            rendered&=sound.processDeviceBlockForTest(input,output,256);
            live.insert(live.end(),output.getChannel(0),output.getChannel(0)+256);
        }
        sound.stop();
        const auto rangePeak=[](const auto& samples,std::size_t first,std::size_t last,std::size_t stride=1){
            float peak=0;for(auto i=first;i<last&&i*stride<samples.size();++i)
                peak=std::max(peak,std::abs(samples[i*stride]));return peak;
        };
        check(rendered&&rangePeak(live,500,1100)>.15f&&rangePeak(live,1500,2100)<1e-6f&&
            rangePeak(live,5300,5900)>.15f&&rangePeak(live,6300,6900)<1e-6f,
            "realtime timeline plays the same edited holes in both linked copies");
        const auto outputPath=(root/"edited-export.wav").string();
        audio::platform::DecodedAudio decoded;
        check(sound.exportMixdown(outputPath,false).isOk()&&
            audio::platform::decodeAudioFile(outputPath,decoded).isOk()&&decoded.channels==2&&
            rangePeak(decoded.interleaved,500,1100,2)>.15f&&rangePeak(decoded.interleaved,1500,2100,2)<1e-6f&&
            rangePeak(decoded.interleaved,5300,5900,2)>.15f&&rangePeak(decoded.interleaved,6300,6900,2)<1e-6f,
            "offline export consumes the same revision as realtime linked playback");
    }
    const auto package=root/"saved.vlt";
    {
        ProjectModel compProject;TrackModel compTrack;compTrack.id=newUuid();compTrack.kind=TrackKind::Audio;
        ClipModel compClip;compClip.id=newUuid();compClip.kind=ClipKind::Audio;
        compClip.filePath=file;compClip.durationSeconds=.1;compClip.gain=.4;
        TakeModel take;take.id=newUuid();take.filePath=file;take.lengthSeconds=.1;
        compClip.takes.push_back(take);compClip.comp.push_back({take.id,0,.1,newUuid()});
        const auto compTrackId=compTrack.id,compClipId=compClip.id;
        compTrack.clips.push_back(compClip);compProject.tracks.push_back(compTrack);
        auto flattened=EngineController::prepareCompAudioEdit(compClip,48000,root.string());
        check(flattened.audio&&flattened.document.initialized,"comp renders to an editor source in background preparation");
        if(flattened.audio)check(std::abs(flattened.audio->channel(0)[100]-tone.getChannel(0)[100])<1e-5,
            "comp source does not bake instance gain twice");
        EngineController compController;compController.initialize(48000,512,false);
        check(compController.restoreRecoveryProject(compProject,root.string()).isOk(),"comp fixture activates");
        const EngineController::AudioEditTarget compTarget{compTrackId,compClipId,false};
        check(compController.adoptCompAudioEdit(compTarget,flattened),"explicit editable comp version commits");
        const auto* active=compController.audioClip(compTrackId,compClipId);
        check(active&&active->takes.empty()&&active->offlineHistory.size()==2&&
            active->offlineHistory.front().source.takes.size()==1&&active->gain==.4f,
            "comp keeps original takes in durable version history and preserves gain");
        compController.undo();active=compController.audioClip(compTrackId,compClipId);
        check(active&&active->takes.size()==1&&!active->audioEdit.initialized,"undo restores editable comp takes");
    }
    check(ProjectSerializer::save(controller.project(),package.string()).isOk(),"package stores editor source resources");
    fs::rename(package,root/"moved.vlt");fs::remove(file);
    ProjectModel moved;check(ProjectSerializer::load(moved,(root/"moved.vlt").string()).isOk(),"relocated package opens");
    for(const auto& tr:moved.tracks) {
        for(const auto& cl:tr.clips)if(cl.audioEdit.initialized)
            check(bool(EngineController::renderAudioEdit(cl.audioEdit)),"relocated clip edit sources resolve");
        if(tr.instrument.audioEdit.initialized)
            check(bool(EngineController::renderAudioEdit(tr.instrument.audioEdit)),"relocated sampler edit sources resolve");
    }
    EngineController reopened;reopened.initialize(48000,512,false);
    check(reopened.openProject((root/"moved.vlt").string()).isOk(),"relocated package activates in the audio engine");
    for(const auto& tr:reopened.project().tracks)if(tr.instrument.audioEdit.initialized) {
        const auto* sampler=reopened.samplerInstance(tr.id,tr.instrument.id);
        auto actual=sampler?sampler->rawSample():nullptr;
        const auto expected=EngineController::renderAudioEdit(tr.instrument.audioEdit);
        check(actual&&expected&&actual->frames()==std::uint64_t(tr.instrument.audioEdit.frames)&&
            std::abs(actual->channel(0)[100]-expected->channel(0)[100])<1e-7,
            "reopened sampler playback receives the exact edited audio revision");
    }
    fs::remove_all(root);
    return failures?1:0;
}
