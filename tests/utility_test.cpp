#include "Internal/UtilityInstance.hpp"
#include "Internal/InternalFactory.hpp"
#include "EngineController.hpp"
#include "ProjectSerializer.hpp"
#include "platform/AudioFileDecoder.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <new>

using namespace daw::plugins;
using namespace daw::plugins::utility;
namespace { thread_local bool countAllocations = false; unsigned allocations = 0; }
void* operator new(std::size_t n) { if (countAllocations) ++allocations; if (auto* p = std::malloc(std::max(n, std::size_t(1)))) return p; throw std::bad_alloc(); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
namespace {
int failures = 0;
void check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failures; }
using Audio = std::array<std::vector<float>, 2>;
void set(UtilityInstance& p, std::string_view id, double v) { p.setParameterFromHost(unsigned(p.parameterIndexForId(id)), v); }
Audio render(UtilityInstance& p, const Audio& in, unsigned block = 128, double rate = 48000,
             std::vector<PluginEvent> events = {}, unsigned channels = 2, double tempo = 120) {
    Audio out{std::vector<float>(in[0].size()), std::vector<float>(in[0].size())};
    PluginBusLayout accepted;
    check(p.setBusLayout({{std::uint16_t(channels)}, {std::uint16_t(channels)}}, accepted), "accept bus layout");
    check(p.activate({rate, block}), "activate"); p.startProcessing();
    std::vector<PluginEvent> blockEvents;
    for (unsigned pos = 0; pos < in[0].size(); pos += block) {
        const unsigned frames = std::min(block, unsigned(in[0].size()) - pos);
        const float* inputs[]{in[0].data()+pos, in[1].data()+pos};
        float* outputs[]{out[0].data()+pos, out[1].data()+pos};
        blockEvents.clear();
        for (auto e : events) if (e.frameOffset >= pos && e.frameOffset < pos+frames) { e.frameOffset -= pos; blockEvents.push_back(e); }
        PluginProcessContext c; c.inputs=inputs; c.outputs=outputs; c.inputChannels=c.outputChannels=std::uint16_t(channels);
        c.frames=frames; c.inputEvents=blockEvents; c.transport.tempo=tempo;
        countAllocations=true; const auto result=p.process(c); countAllocations=false;
        if (result != PluginProcessDisposition::Continue) { check(false, "process"); break; }
    }
    return out;
}
PluginEvent event(unsigned at, unsigned index, double value) {
    PluginEvent e; e.kind=PluginEvent::Kind::ParamValue; e.frameOffset=at; e.paramIndex=index; e.value=value; return e;
}
bool near(double a, double b) { return std::abs(a-b) < 2e-6; }
void dspChecks() {
    InternalFactory factory;
    for (const auto kind : {Kind::Gain, Kind::Stereo, Kind::Reverse}) {
        auto p=factory.create(descriptorFor(kind));
        check(p && p->descriptor().category == "Effect|Utility", "factory registers independent utility");
        for (const auto& param : p->parameters()) p->setParameterFromHost(param.index, param.maxValue);
        std::vector<std::uint8_t> state; p->saveState(state);
        UtilityInstance clone(kind); bool restored=clone.loadState(state);
        for (const auto& param : p->parameters()) restored &= clone.parameterValue(param.index)==param.maxValue;
        check(restored, "state and stable parameter IDs round trip");
        UtilityInstance wrong(kind == Kind::Gain ? Kind::Stereo : Kind::Gain);
        check(!wrong.loadState(state), "state cannot cross plugin identities");
        const std::string bad="{\"version\":1,\"uid\":\""+p->descriptor().uid+"\",\"params\":{\""+p->parameters()[0].id+"\":\"bad\"}}";
        check(!clone.loadState({reinterpret_cast<const std::uint8_t*>(bad.data()),bad.size()}) && clone.parameterValue(0)==p->parameters()[0].maxValue, "invalid state does not partially overwrite parameters");
    }
    Audio constant{std::vector<float>(3000,.8f),std::vector<float>(3000,.2f)};
    for (double rate : {44100.,48000.,96000.}) {
        UtilityInstance gain(Kind::Gain), stereo(Kind::Stereo);
        check(render(gain,constant,127,rate)==constant && render(stereo,constant,257,rate)==constant, "default Gain and Stereo preserve audio exactly");
        set(gain,"gain",20);
        auto out=render(gain,constant,128,rate);
        check(near(out[0].back(),8) && near(out[1].back(),2), "gain does not clip samples above full scale");
        set(gain,"mute",1); out=render(gain,constant,128,rate);
        check(out[0].back()==0 && out[1].back()==0, "mute produces silence");
    }
    UtilityInstance gain(Kind::Gain);
    auto out=render(gain,constant,512,48000,{event(13,1,1)});
    check(out[0][12]==constant[0][12] && out[0][13]<constant[0][13] && out[0][491]>0 && out[0][492]==0, "automation starts at exact frame and finishes a 10 ms ramp");
    const std::array<std::array<double,2>,5> expected{{{.8,.2},{.5,.5},{.3,-.3},{.8,.8},{.2,.2}}};
    for (unsigned mode=0;mode<5;++mode) {
        UtilityInstance p(Kind::Stereo); set(p,"mode",mode); out=render(p,constant);
        check(near(out[0].back(),expected[mode][0]) && near(out[1].back(),expected[mode][1]), "stereo routing matches M/S matrix");
    }
    UtilityInstance stereo(Kind::Stereo); set(stereo,"width",0); out=render(stereo,constant);
    check(near(out[0].back(),.5)&&near(out[1].back(),.5),"zero width is mono");
    set(stereo,"width",200); out=render(stereo,constant);
    check(near(out[0].back(),1.1)&&near(out[1].back(),-.1),"200 percent width doubles side");
    set(stereo,"width",100); set(stereo,"swap",1); set(stereo,"invertLeft",1); out=render(stereo,constant);
    check(near(out[0].back(),-.2)&&near(out[1].back(),.8),"swap precedes per-channel polarity");
    set(stereo,"mode",2); out=render(stereo,constant,128,48000,{},1);
    check(out[0].back()==0,"mono bus never invents side, even with polarity switches");
    check(reverseMilliseconds(0,500,2,120)==500 && reverseMilliseconds(0,500,0,120)==125 &&
          reverseMilliseconds(0,500,4,0)==2000 && reverseMilliseconds(0,500,4,10)==4000 &&
          reverseMilliseconds(0,500,0,1000)==50,"reverse tempo fallback, divisions and limits");
    for (double rate : {44100.,48000.,96000.}) {
        const unsigned n=unsigned(std::lround(rate*.05)), fade=unsigned(std::lround(rate*.003));
        Audio signal{std::vector<float>(4*n),std::vector<float>(4*n)};
        for (unsigned i=0;i<signal[0].size();++i) { signal[0][i]=float((i% n)+1)/n; signal[1][i]=-signal[0][i]; }
        UtilityInstance p(Kind::Reverse); set(p,"timeMode",1); set(p,"timeMs",50);
        out=render(p,signal,127,rate);
        bool reverse=true;
        for (unsigned i=0;i<n;++i) {
            const double edge=std::min({1.,double(i)/fade,double(n-1-i)/fade});
            reverse &= out[0][i]==0 && near(out[0][n+i],signal[0][n-1-i]*edge) && out[1][n+i]==-out[0][n+i];
        }
        check(reverse,"buffered slices reverse sample order with 3 ms edges at every sample rate");
        check(p.tailSamples()==2*n+unsigned(std::lround(rate*.010)), "reverse reports a finite tail bound for the observed slice size");
        countAllocations=true;p.reset();countAllocations=false;
        std::array<float,128> zero{},left{},right{}; const float* inputs[]{zero.data(),zero.data()}; float* outputs[]{left.data(),right.data()};
        PluginProcessContext c; c.inputs=inputs;c.outputs=outputs;c.inputChannels=c.outputChannels=2;c.frames=128;
        countAllocations=true;p.process(c);countAllocations=false;
        check(std::all_of(left.begin(),left.end(),[](float v){return v==0;}),"transport reset discards previous audio");
    }
    Audio signal{std::vector<float>(48000),std::vector<float>(48000)};
    for (unsigned i=0;i<signal[0].size();++i) { signal[0][i]=float(.4*std::sin(i*.037));signal[1][i]=float(.3*std::cos(i*.051)); }
    for (const auto kind : {Kind::Gain,Kind::Stereo,Kind::Reverse}) {
        UtilityInstance a(kind),b(kind);
        std::vector<PluginEvent> events;
        if(kind==Kind::Reverse) { set(a,"timeMode",1);set(b,"timeMode",1);set(a,"timeMs",50);set(b,"timeMs",50);events={event(1301,1,100),event(1407,1,75),event(6003,1,50),event(15017,3,40)}; }
        else if(kind==Kind::Gain) events={event(307,0,12),event(811,0,-6),event(1293,1,1)};
        else events={event(301,0,2),event(450,0,0),event(771,1,175),event(1001,2,1)};
        check(render(a,signal,47,48000,events)==render(b,signal,1024,48000,events),"automation and reverse size changes are independent of block partition");
    }
    UtilityInstance dry(Kind::Reverse);set(dry,"mix",0);
    check(render(dry,signal)==signal,"reverse dry path is immediate and bit exact");
    check(allocations==0,"audio processing and resets allocate no memory");
}
void projectChecks() {
    daw::EngineController c;
    check(c.initialize(48000,256,false).isOk(),"controller initialize");
    const auto track=c.addTrack(daw::TrackKind::Audio,"Utilities");
    std::vector<std::string> slots;
    for (auto kind : {Kind::Gain,Kind::Stereo,Kind::Reverse}) {
        auto slot=c.addInsert(track,descriptorFor(kind));check(!slot.empty(),"add utility insert");slots.push_back(slot);
        const auto& param=parameterTable(kind)[0];
        c.setInsertParameter(track,slot,param.id,param.maxValue);
        c.commitInsertParameterEdit(track,slot,param.id,param.defaultValue,"Utility edit");
        c.undo();check(c.insertParameter(track,slot,param.id)==param.defaultValue,"parameter undo");
        c.redo();check(c.insertParameter(track,slot,param.id)==param.maxValue,"parameter redo");
        c.setInsertBypassed(track,slot,true);
        check(c.insertModel(track,slot)->bypassed,"host bypass");
    }
    c.pumpPreviewPluginEvents();
    daw::EngineController::ChannelSnapshot snapshot;
    check(c.captureRackSelection(track,slots,snapshot).isOk(),"capture utility rack");
    const auto other=c.addTrack(daw::TrackKind::Audio,"Copy");
    check(c.pasteRackSelection(other,snapshot,0).isOk(),"copy utilities with their saved settings");
    const auto path=(std::filesystem::temp_directory_path()/ ("vlt-utility-"+track+".vlt")).string();
    check(c.saveProject(path).isOk(),"save project with three utilities");
    daw::EngineController reopened;reopened.initialize(48000,256,false);
    check(reopened.openProject(path).isOk(),"reopen utility project");
    for(unsigned i=0;i<3;++i) {
        const auto kind=Kind(i);const auto& param=parameterTable(kind)[0];
        check(reopened.hasInsert(track,slots[i]) && reopened.insertParameter(track,slots[i],param.id)==param.maxValue && reopened.insertModel(track,slots[i])->bypassed,"restored identity, parameter and bypass");
    }
}
void exportChecks() {
    namespace ap = audio::platform;
    daw::EngineController c;
    check(c.initialize(48000,128,false).isOk(),"export controller");
    const auto track=c.addTrack(daw::TrackKind::Audio,"Reverse tail");
    const auto directory=std::filesystem::temp_directory_path()/("vlt-utility-render-"+track);
    std::filesystem::create_directories(directory);
    const auto source=(directory/"source.wav").string();
    ap::AudioFileWriter writer;ap::WriteSpec file;file.encoding=ap::Encoding::Float32;
    Audio signal{std::vector<float>(2400),std::vector<float>(2400)};
    for(unsigned i=0;i<2400;++i) {signal[0][i]=float(.2*std::sin(i*.037));signal[1][i]=signal[0][i];}
    const float* channels[]{signal[0].data(),signal[1].data()};
    check(bool(writer.open(source,file,48000,2,2400)) && bool(writer.write(channels,2400)) && bool(writer.close()),"write short render source");
    check(!c.importAudio(source,track,0).empty(),"import short render source");
    auto gain=c.addInsert(track,descriptorFor(Kind::Gain));
    auto stereo=c.addInsert(track,descriptorFor(Kind::Stereo));
    auto reverse=c.addInsert(track,descriptorFor(Kind::Reverse));
    c.setInsertParameter(track,gain,"gain",-6);
    c.setInsertParameter(track,stereo,"mode",1);
    c.setInsertParameter(track,reverse,"timeMode",1);
    c.setInsertParameter(track,reverse,"timeMs",500);
    daw::rendering::Spec spec;spec.outputDir=directory.string();spec.file.encoding=ap::Encoding::Float32;
    spec.tail=daw::rendering::Tail::UntilSilence;spec.tailHoldSeconds=.1;spec.tailMaxSeconds=10;
    std::vector<float> baseline;
    for(unsigned block : {128u,1024u}) {
        spec.blockSize=block;spec.pipeline=block==1024;spec.forcePipeline=block==1024;
        spec.baseName="export-"+std::to_string(block);
        daw::rendering::Report report;
        const auto result=c.renderProject(spec,{},report);
        if (!result) std::fprintf(stderr,"utility export: %s\n",result.message().c_str());
        check(result.isOk() && report.files.size()==1,"export utility chain");
        if(!result || report.files.empty()) continue;
        ap::DecodedAudio decoded;
        check(bool(ap::decodeAudioFile(report.files[0],decoded)),"decode utility export");
        double latePeak=0;
        for(std::size_t i=std::size_t(.94*48000)*2;i<std::min(decoded.interleaved.size(),std::size_t(48000*2));++i)
            latePeak=std::max(latePeak,std::abs(double(decoded.interleaved[i])));
        check(latePeak>.01,"until-silence export retains reverse audio after initial silent buffer");
        if(baseline.empty()) baseline=decoded.interleaved;
        else {
            double difference=0;
            for(std::size_t i=0;i<std::max(baseline.size(),decoded.interleaved.size());++i) {
                const double a=i<baseline.size()?baseline[i]:0,b=i<decoded.interleaved.size()?decoded.interleaved[i]:0;
                difference=std::max(difference,std::abs(a-b));
            }
            check(difference<2e-6,"serial and pipelined utility exports agree across block sizes");
            check(report.usedPipeline,"utility chain supports offline pipeline");
        }
    }
}
}
int main() { std::setvbuf(stdout,nullptr,_IONBF,0);dspChecks();projectChecks();exportChecks();return failures?1:0; }
