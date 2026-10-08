#include "EngineController.hpp"
#include "ChannelStripPreset.hpp"
#include "model/ChannelColor.hpp"
#include "model/MiniModules.hpp"
#include "Internal/ChannelColorInstance.hpp"
#include "Internal/InternalFactory.hpp"
#include "Internal/SamplerInstance.hpp"
#include "Recording/RecordingEngine.hpp"
#include "platform/AudioFileDecoder.hpp"
#include "collaboration/CommandJson.hpp"
#include "collaboration/ProjectReducer.hpp"
#include "collaboration/SharedProjectSnapshot.hpp"
#include "cloud/CloudDocumentProjection.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <numbers>

namespace {
namespace fs=std::filesystem;
using namespace daw;
using namespace daw::collab;
int failures=0;
bool check(bool ok,const char* label) { std::printf("%s %s\n",ok?"PASS":"FAIL",label); failures+=!ok; return ok; }
double difference(const std::vector<float>& a,const std::vector<float>& b,unsigned skipFrames=0) {
    if(a.size()!=b.size() || a.empty()) return 1e9;
    double e=0; for(size_t i=skipFrames*2;i<a.size();++i) e=std::max(e,std::abs(double(a[i])-b[i])); return e;
}
const InsertModel& color(const EngineController& c,const std::string& id) { return c.miniModules(id).front(); }
void enable(EngineController& c,const std::string& id,bool enabled) {
    if(c.miniModules(id).empty()) c.addMiniModule(id,plugins::mini::builtin("color"));
    else c.setInsertBypassed(id,color(c,id).id,!enabled);
}
void edit(EngineController& c,const std::string& id,const char* parameter,double value) {
    const auto slot=color(c,id).id;const auto before=c.insertParameter(id,slot,parameter);
    c.setInsertParameter(id,slot,parameter,value);c.commitInsertParameterEdit(id,slot,parameter,before,"Change Color");
}
void cold(EngineController& c) {
    c.stop(); c.seekSeconds(0); c.pumpPluginEvents();
    for(const auto& node:c.routingGraph()->nodes) node.node->reset();
}
std::vector<float> playback(EngineController& c,unsigned frames) {
    cold(c); c.play(); audio::AudioBuffer in(2,256),out(2,256); in.clear(); std::vector<float> result(frames*2);
    bool ok=true;
    for(unsigned at=0;at<frames;at+=256) {
        const auto n=std::min(256u,frames-at); ok&=c.processDeviceBlockForTest(in,out,n);
        for(unsigned i=0;i<n;++i) for(unsigned ch=0;ch<2;++ch) result[(at+i)*2+ch]=out.getChannel(ch)[i];
    }
    c.stop(); check(ok,"device renders each comparison block"); return result;
}
bool path(const engine::CompiledGraph& graph,engine::NodeId from,engine::NodeId to) {
    for(const auto& node:graph.nodes) if(node.id==to) {
        if(from==to) return true;
        for(unsigned i=0;i<node.inputCount;++i) {
            const auto producer=graph.nodes[graph.inputEdges[node.firstInput+i].producer].id;
            if(path(graph,from,producer)) return true;
        }
    }
    return false;
}
ProjectCommand command(CommandBody body) {
    ProjectCommand c; c.meta.projectId=deterministicMigrationId("color-test","project");
    c.meta.operationId=newUuid(); c.meta.actorId=deterministicMigrationId("color-test","actor");
    c.meta.clientId=deterministicMigrationId("color-test","client"); c.meta.clientSequence=1; c.body=std::move(body); return c;
}
}

int main() {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    const auto temporary=fs::temp_directory_path()/("vlt-color-test-"+newUuid()); fs::create_directories(temporary);
    audio::AudioBuffer source(2,48000);
    for(unsigned i=0;i<48000;++i) {
        source.getChannel(0)[i]=float(.251*std::sin(2*std::numbers::pi*631*i/48000)+.067*std::sin(2*std::numbers::pi*97*i/48000));
        source.getChannel(1)[i]=float(.17*std::sin(2*std::numbers::pi*421*i/48000));
        // Finish at zero so the live clip-boundary declick does not feed a
        // different tail into Color's oversampling filter than offline input.
        const auto endFade = float(std::clamp((47999. - i) / 240., 0., 1.));
        source.getChannel(0)[i] *= endFade;
        source.getChannel(1)[i] *= endFade;
    }
    const auto wav=(temporary/"source.wav").string();
    audio::AudioRecorder writer; writer.initialize(48000,2); check(bool(writer.writeWAVFile(wav,source,48000)),"synthetic stereo source written");
    {
        EngineController c{EngineController::TestRuntime{}}; c.initialize(48000,256,false);
        const auto id=c.importAudioToNewTrack(wav,0);
        // Do not reset the graph: a fresh old project must be transparent from
        // its first block, before any user gesture or transport restart.
        c.play(); audio::AudioBuffer in(2,256),out(2,256); in.clear(); bool transparent=true;
        for(unsigned at=0;at<1024;at+=256) {
            transparent&=c.processDeviceBlockForTest(in,out,256);
            for(unsigned i=0;i<256;++i) for(unsigned ch=0;ch<2;++ch) {
                const auto position=at+i;
                const auto expected=source.getChannel(ch)[position];
                transparent&=std::abs(out.getChannel(ch)[i]-expected)<1e-7;
            }
        }
        c.stop(); check(transparent && !c.project().findTrack(id)->channelColor,"empty rack is exactly dry from the first device block");
        check(c.routingGraph()->totalLatency==0,"empty rack has no DSP latency");
        const auto copied=c.duplicateTrack(id,true,false);
        check(!copied.empty() && c.miniModules(copied).empty(),"duplicate of empty rack stays empty");
    }
    for(bool master:{false,true}) {
        EngineController c{EngineController::TestRuntime{}};c.initialize(48000,256,false);const auto sourceId=c.importAudioToNewTrack(wav,0);
        const auto id=master?std::string("master"):sourceId;
        const auto mini=c.addMiniModule(id,plugins::mini::builtin("color"));
        const auto fx=c.addInsert(id,plugins::channel_color::ChannelColorInstance::staticDescriptor());
        c.setInsertParameter(id,mini,"drive",75);c.setInsertParameter(id,mini,"tone",60);
        c.setInsertParameter(id,fx,"drive",-90);c.setInsertParameter(id,fx,"tone",-60);
        const auto nodeFor=[&](const std::string& slot){
            const auto* instance=c.insertInstance(id,slot);
            for(const auto& n:c.routingGraph()->nodes) if(auto* host=dynamic_cast<plugins::PluginNode*>(n.node);host&&host->instance()==instance)return n.id;
            return engine::kInvalidNode;
        };
        const auto before=playback(c,12288);
        check(!fx.empty() && path(*c.routingGraph(),nodeFor(mini),nodeFor(fx)),"pre FX mini precedes Audio FX on track and Master");
        const auto undo=c.undoDepth();check(c.setMiniModulePostFx(id,mini,true) && c.undoDepth()==undo+1,"post FX choice is one Undo action");
        check(path(*c.routingGraph(),nodeFor(fx),nodeFor(mini)),"post FX mini follows Audio FX on track and Master");
        const auto after=playback(c,12288);check(difference(before,after)>1e-5,"pre/post selection changes actual nonlinear processing order");
        // Device transport changes crossfade the previous output for 5 ms.
        c.undo();check(difference(before,playback(c,12288),256)<2e-5,"route Undo restores identical rendered sound after the transport fade");
    }
    {
        EngineController c{EngineController::TestRuntime{}};c.initialize(48000,256,false);
        for(const auto kind:{TrackKind::Audio,TrackKind::Midi,TrackKind::Instrument,TrackKind::Pattern,TrackKind::Group,TrackKind::Bus,TrackKind::Aux}) {
            const auto id=c.addTrack(kind,"Channel");check(c.miniModules(id).empty(),"new channels start with empty racks");
            check(!c.addMiniModule(id,plugins::mini::builtin("color")).empty(),"audio channel accepts Color");
        }
        const auto id=c.project().tracks.front().id,slot=color(c,id).id;
        check(c.insertInstance(id,slot) && c.insertInstance(id,slot)->latencySamples()==48 && c.insertParameters(id,slot).size()==2,"Color exposes two parameters and 48 samples latency");
        auto depth=c.undoDepth();for(double value:{5.,19.,35.}) c.setInsertParameter(id,slot,"drive",value);
        c.commitInsertParameterEdit(id,slot,"drive",-20,"Change Color Drive");
        check(c.undoDepth()==depth+1 && c.insertParameter(id,slot,"drive")==35,"continuous gesture commits one Undo");
        c.undo();check(c.insertParameter(id,slot,"drive")==-20,"gesture Undo restores prior value");c.redo();edit(c,id,"tone",-27);
        const auto seed=color(c,id).profileSeed;
        const auto copy=c.duplicateTrack(id,true,false);
        check(!copy.empty() && color(c,copy).profileSeed==seed && color(c,copy).id!=slot,"duplicate preserves profile with new module ID");
        const auto target=c.addTrack(TrackKind::Audio,"Paste");depth=c.undoDepth();
        check(c.pasteChannelStrip(target,c.copyChannelStrip(id,true)) && c.undoDepth()==depth+1 && color(c,target).profileSeed==seed && c.insertParameter(target,color(c,target).id,"tone")==-27,"full strip copy includes module and profile");
        c.undo();check(c.miniModules(target).empty(),"strip paste Undo restores empty rack");c.redo();
        const auto preset=(temporary/"color.vlts").string();check(bool(c.saveChannelStripPreset(id,preset)) && bool(c.applyChannelStripPreset(target,preset)) && color(c,target).profileSeed==seed,"portable strip preset restores module");
        const auto project=(temporary/"color.vlt").string();EngineController opened{EngineController::TestRuntime{}};opened.initialize(48000,256,false);
        check(bool(c.saveProject(project)) && bool(opened.openProject(project)) && color(opened,id).profileSeed==seed && opened.insertParameter(id,slot,"drive")==35 && opened.insertParameter(id,slot,"tone")==-27,"project restores ID values and component profile");
        const auto templ=(temporary/"color.vltt").string();EngineController fromTemplate{EngineController::TestRuntime{}};fromTemplate.initialize(48000,256,false);
        check(bool(c.saveProjectTemplate(templ,"Color template")) && bool(fromTemplate.openProjectTemplate(templ)) && color(fromTemplate,fromTemplate.project().tracks.front().id).profileSeed==seed && color(fromTemplate,fromTemplate.project().tracks.front().id).id!=slot,"template remaps module IDs while retaining profile");
    }
    {
        EngineController c{EngineController::TestRuntime{}}; c.initialize(48000,256,false); const auto id=c.importAudioToNewTrack(wav,0); c.setRecordDirectory(temporary.string());
        enable(c,id,true); edit(c,id,"drive",75); edit(c,id,"tone",32);
        const auto latency=c.routingGraph()->totalLatency;
        check(latency==48,"COLOR declares exactly 48 frames throughout the channel graph");
        const auto live=playback(c,48256);
        rendering::Spec spec; spec.outputDir=temporary.string(); spec.file.container=audio::platform::Container::Wav;
        spec.file.encoding=audio::platform::Encoding::Float32; spec.range=rendering::Range::Custom; spec.customEndSeconds=1;
        spec.stemChannelIds={id}; spec.blockSize=256;
        const auto render=[&](const char* name) {
            spec.baseName=name; rendering::Report report;
            check(bool(c.renderProject(spec,{},report)) && !report.files.empty(),"COLOR export succeeds");
            std::vector<std::vector<float>> data;
            for(const auto& file:report.files) { audio::platform::DecodedAudio decoded; check(bool(audio::platform::decodeAudioFile(file,decoded)),"exported audio decodes"); data.push_back(std::move(decoded.interleaved)); }
            return data;
        };
        const auto wet=render("color-wet"); double error=1e9; unsigned worstFrame=0;
        if(!wet.empty() && wet.front().size()==96000) {
            error=0; for(unsigned i=256;i<48000;++i) for(unsigned ch=0;ch<2;++ch) { const auto delta=std::abs(double(live[(i+latency)*2+ch])-wet.front()[i*2+ch]); if(delta>error){error=delta;worstFrame=i;} }
        }
        std::printf("MEASURE COLOR device/export maximum error %.9g at frame %u\n",error,worstFrame);
        check(error<2e-5 && wet.size()==2 && difference(wet.front(),wet[1])<2e-5,"playback and exported stereo stems agree from the same initial state");
        spec.bypassTrackInserts=true; const auto dry=render("color-dry"); spec.bypassTrackInserts=false;
        std::vector<float> original(96000); for(unsigned i=0;i<48000;++i) for(unsigned ch=0;ch<2;++ch) original[i*2+ch]=source.getChannel(ch)[i];
        check(!dry.empty() && difference(dry.front(),original)<1e-6 && difference(wet.front(),original)>.001,"dry track export excludes COLOR while ordinary export includes it");
        spec.stemsAtSource=true; const auto atSource=render("color-source"); spec.stemsAtSource=false;
        check(atSource.size()==2 && difference(atSource[1],original)<1e-6,"source export taps immediately before COLOR");
        rendering::Report frozen; check(bool(c.freezeTrack(id,{},frozen)) && c.isTrackFrozen(id),"freeze bakes enabled COLOR");
        const auto baked=render("color-frozen");
        check(!baked.empty() && difference(baked.front(),wet.front())<2e-5,"frozen export applies COLOR exactly once");
        auto look=color(c,id).miniModule->appearance;look.theme="copper";
        check(c.setMiniModuleAppearance(id,color(c,id).id,look) && c.isTrackFrozen(id),"appearance changes preserve frozen audio");
        edit(c,id,"drive",65); check(!c.isTrackFrozen(id),"changing COLOR invalidates the track freeze");
        // Automation is evaluated independently of panel timers, with the same
        // offsets at the device and during offline export.
        std::vector<std::string> lanes;
        for(const auto* parameter:{"drive","tone"}) {
            AutomationTarget target; target.kind=AutomationTargetKind::PluginParameter; target.channelId=id;
            target.slotId=color(c,id).id; target.parameterId=parameter;
            const auto lane=c.addAutomationLane(id,target); const auto clip=c.addAutomationClip(lane,target,0,1); lanes.push_back(lane);
            c.setAutomationPoints(lane,clip,{{0,.25},{.67,.8},{1.51,.4},{2,.6}});
            check(!lane.empty() && !clip.empty(),"COLOR parameter automation lane binds");
        }
        const auto automatedLive=playback(c,48256); const auto automated=render("color-automation"); error=1e9;
        if(!automated.empty() && automated.front().size()==96000) {
            error=0; for(unsigned i=256;i<48000;++i) for(unsigned ch=0;ch<2;++ch) { const auto delta=std::abs(double(automatedLive[(i+48)*2+ch])-automated.front()[i*2+ch]); if(delta>error){error=delta;worstFrame=i;} }
        }
        std::printf("MEASURE COLOR automated device/export maximum error %.9g at frame %u\n",error,worstFrame);
        check(error<2e-5 && difference(wet.front(),automated.front())>.001,"Drive and Tone automation produce identical playback and export");
        const auto automatedProject=(temporary/"automated-color.vlt").string();
        EngineController automatedReopen{EngineController::TestRuntime{}}; automatedReopen.initialize(48000,256,false);
        check(bool(c.saveProject(automatedProject)) && bool(automatedReopen.openProject(automatedProject)) &&
            automatedReopen.insertParameter(id,color(c,id).id,"drive")==65 && automatedReopen.insertParameter(id,color(c,id).id,"tone")==32,
            "saving after automation preserves the static COLOR controls on reopening");
        spec.baseName="color-reopened-automation"; rendering::Report reopenedReport; audio::platform::DecodedAudio reopenedAudio;
        const bool reopenedRender=bool(automatedReopen.renderProject(spec,{},reopenedReport)) && !reopenedReport.files.empty() &&
            bool(audio::platform::decodeAudioFile(reopenedReport.files.front(),reopenedAudio));
        check(reopenedRender && difference(automated.front(),reopenedAudio.interleaved)<2e-5,"COLOR automation renders identically after save and reopen");
        const auto bus=c.addTrack(TrackKind::Bus,"COLOR sum"); c.setTrackOutputBus(id,bus);
        check(c.trackNodes(bus)->channelColor==engine::kInvalidNode,"summation does not add a second COLOR stage");
    }
    {
        EngineController c{EngineController::TestRuntime{}}; c.initialize(48000,256,false); c.setRecordDirectory(temporary.string());
        const auto id=c.addTrack(TrackKind::Audio,"Monitor"); c.setTrackInputRouting(id,0,2,true); c.setTrackMonitor(id,true);
        enable(c,id,true); edit(c,id,"drive",85); cold(c);
        const auto* nodes=c.trackNodes(id); const auto graph=c.routingGraph();
        check(nodes->sum!=engine::kInvalidNode && path(*graph,nodes->input,nodes->channelColor),"physical input is merged before COLOR");
        audio::AudioBuffer in(2,256),out(2,256); double changed=0;
        plugins::channel_color::ChannelColorInstance reference;
        reference.setProfileSeed(parseChannelColorSeed(color(c,id).profileSeed)); reference.setParameterFromHost(0,85); reference.activate({48000,256});
        std::array<float,256> left{},right{}; const float* ins[]{in.getChannel(0),in.getChannel(1)}; float* outs[]{left.data(),right.data()};
        bool monitoring=true;
        for(unsigned at=0;at<8192;at+=256) {
            for(unsigned i=0;i<256;++i) for(unsigned ch=0;ch<2;++ch) in.getChannel(ch)[i]=source.getChannel(ch)[at+i];
            monitoring&=c.processDeviceBlockForTest(in,out,256);
            plugins::PluginProcessContext context; context.inputs=ins; context.outputs=outs; context.inputChannels=context.outputChannels=2; context.frames=256; reference.process(context);
            for(unsigned i=0;i<256;++i) changed=std::max(changed,std::abs(double(out.getChannel(0)[i])-left[i]));
        }
        check(monitoring && changed<2e-5,"live monitored audio uses the same COLOR DSP");
        check(c.startRecording(id),"COLOR monitor starts recording");
        for(unsigned at=0;at<8192;at+=256) {
            for(unsigned i=0;i<256;++i) for(unsigned ch=0;ch<2;++ch) in.getChannel(ch)[i]=source.getChannel(ch)[at+i];
            c.processDeviceBlockForTest(in,out,256);
        }
        const auto recordedPath=c.stopRecording(); audio::platform::DecodedAudio captured;
        bool recordedDry=!recordedPath.empty() && bool(audio::platform::decodeAudioFile(recordedPath,captured)) && captured.channels==2 && captured.frames==8192-48;
        // Recording compensation trims the first 48 frames before the capture
        // start. The stored samples themselves must be the raw physical input.
        if(recordedDry) for(unsigned i=0;i<captured.frames;++i) for(unsigned ch=0;ch<2;++ch) recordedDry&=std::abs(captured.interleaved[i*2+ch]-source.getChannel(ch)[i+48])<1e-6;
        std::printf("MEASURE COLOR dry recording: %llu frames, %u channels; %s\n",static_cast<unsigned long long>(captured.frames),captured.channels,c.recordingWarning().c_str());
        check(recordedDry,"recorded input WAV remains dry with COLOR monitoring enabled");
    }
    {
        const auto sampler=plugins::sampler::SamplerInstance::staticDescriptor();
        for(bool firstFx:{false,true}) {
            EngineController c{EngineController::TestRuntime{}}; c.initialize(48000,256,false); const auto id=c.addTrack(TrackKind::Midi,firstFx?"FX synth":"Instrument");
            std::string slot;
            if(firstFx) slot=c.addInsert(id,sampler);
            else { check(c.setTrackInstrumentPlugin(id,sampler),"MIDI instrument loads"); slot=c.project().findTrack(id)->instrument.id; }
            check(c.loadSamplerSample(id,slot,wav),"MIDI sampler audio loads");
            enable(c,id,true);
            if(!firstFx) for(const auto& effect:plugins::builtinPlugins()) if(effect.uid=="daw.equalizer") {
                const auto sampleFx=c.addSamplerFxInsert(id,slot,effect);
                check(!sampleFx.empty() && !c.trackNodes(id)->samplerInserts.empty() && path(*c.routingGraph(),c.trackNodes(id)->samplerInserts.back(),c.trackNodes(id)->channelColor),
                    "Sampler-owned processing precedes COLOR");
            }
            const auto clip=c.addMidiClip(id,0,1); c.addNote(id,clip,60,0,2,120); edit(c,id,"drive",60);
            const auto* nodes=c.trackNodes(id); const auto graph=c.routingGraph();
            const auto synth=firstFx?nodes->inserts.front():nodes->instrument;
            check(path(*graph,synth,nodes->channelColor),firstFx?"first Audio FX instrument precedes COLOR":"MIDI instrument output precedes COLOR");
            const auto wet=playback(c,12288); enable(c,id,false); const auto dry=playback(c,12288);
            double energy=0; for(float value:wet) energy+=value*value;
            check(energy>1 && difference(wet,dry)>.001,"COLOR processes generated MIDI audio rather than MIDI events");
        }
    }
    {
        EngineController c{EngineController::TestRuntime{}}; c.initialize(48000,256,false); c.setRecordDirectory(temporary.string());
        const auto id=c.importAudioToNewTrack(wav,0); c.setTrackMono(id,true);
        enable(c,id,true); edit(c,id,"drive",90);
        c.setMiniModulePostFx(id,color(c,id).id,true);
        const auto render=[&](const char* name) {
            rendering::Spec spec; spec.outputDir=temporary.string(); spec.baseName=name;
            spec.file.container=audio::platform::Container::Wav; spec.file.encoding=audio::platform::Encoding::Float32;
            spec.range=rendering::Range::Custom; spec.customEndSeconds=1;
            rendering::Report report; audio::platform::DecodedAudio data;
            if(c.renderProject(spec,{},report) && !report.files.empty()) audio::platform::decodeAudioFile(report.files.front(),data);
            return data.interleaved;
        };
        const auto before=render("color-mono"); rendering::Report freeze;
        check(bool(c.freezeTrack(id,{},freeze)),"mono COLOR track freezes"); const auto after=render("color-mono-frozen");
        check(difference(before,after)<2e-5,"freeze preserves mono folding before the magnetic nonlinearity");
    }
    {
        EngineController c{EngineController::TestRuntime{}}; c.initialize(48000,256,false); const auto sourceTrack=c.importAudioToNewTrack(wav,0);
        auto model=c.project(); const auto base=*model.findTrack(sourceTrack); model.tracks.clear();
        for(unsigned i=0;i<64;++i) {
            auto track=base; track.id=newUuid(); track.name="COLOR "+std::to_string(i+1); track.volume=1.f/64;
            track.channelColor=defaultChannelColor(track.id); track.channelColor->bypassed=false;
            for(auto& clip:track.clips) clip.id=newUuid(); model.tracks.push_back(std::move(track));
        }
        check(bool(c.materializeCollaborationProject(std::move(model),true)),"64 stereo COLOR tracks compile");
        const auto workers=c.configureAudioWorkersForTest(true,8); cold(c); c.play();
        audio::AudioBuffer in(2,256),out(2,256); in.clear(); std::vector<double> times; times.reserve(150); bool ok=true;
        for(unsigned i=0;i<170;++i) {
            const auto started=std::chrono::steady_clock::now(); ok&=c.processDeviceBlockForTest(in,out,256);
            if(i>=20) times.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count());
        }
        c.stop(); double sum=0; unsigned missed=0; for(double ms:times) {sum+=ms;missed+=ms>256.*1000/48000;}
        std::sort(times.begin(),times.end());
        std::printf("MEASURE 64 stereo COLOR tracks, 48k/256, %u workers: mean %.3f ms, p95 %.3f ms, max %.3f ms, %u/%zu deadline overruns\n",workers,sum/times.size(),times[times.size()*95/100],times.back(),missed,times.size());
        check(ok,"64-channel device graph renders continuously without processing failures");
    }
    // The directory is unique and owned by this harness.
    fs::remove_all(temporary); return failures?1:0;
}
