#include "Internal/ReverbInstance.hpp"
#include "Internal/InternalFactory.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <new>
#include <numbers>
#include <tuple>

using namespace daw::plugins;
using namespace daw::plugins::reverb;
namespace { thread_local bool countAllocations = false; unsigned allocations = 0; }
void* operator new(std::size_t n) { if (countAllocations) ++allocations; if (void* p = std::malloc(std::max(n,std::size_t(1)))) return p; throw std::bad_alloc(); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
void operator delete[](void* p,std::size_t) noexcept { std::free(p); }
namespace {
int failures = 0;
void check(bool pass,const char* text) { std::printf("%s %s\n",pass ? "PASS" : "FAIL",text); failures += !pass; }
void set(ReverbInstance& p,Param id,double v) { p.setParameterFromHost(unsigned(id),v); }
struct Audio { std::vector<float> l,r; };
Audio render(double rate,unsigned block,unsigned mode,bool automation=false,unsigned channels=2,double duration=.65) {
    ReverbInstance p; set(p,Param::Mode,mode); set(p,Param::Mix,100);
    PluginBusLayout accepted; p.setBusLayout({{std::uint16_t(channels),2},{std::uint16_t(channels)}},accepted);
    p.activate({rate,block}); p.startProcessing();
    const unsigned size = unsigned(duration*rate);
    Audio a{std::vector<float>(size),std::vector<float>(size)};
    std::vector<float> l(block),r(block);
    const float* in[]{l.data(),r.data()};
    for (unsigned at=0;at<size;at+=block) {
        const unsigned frames=std::min(block,size-at);
        for (unsigned i=0;i<frames;++i) {
            l[i] = at+i==0 ? 1.f : at+i<rate*.12 ? float(.1*std::sin((at+i)*.071)) : 0.f;
            r[i] = .7f*l[i];
        }
        std::array<PluginEvent,12> events{}; unsigned n=0;
        if (automation) for (const auto [frame,param,value] : std::array<std::tuple<unsigned,Param,double>,12>{{
            {113,Param::PreDelay,0},{701,Param::Size,90},{1503,Param::Mode,3},
            {1701,Param::Mode,1},{2809,Param::Mode,5},{4001,Param::Brightness,90},
            {6003,Param::Freeze,1},{7001,Param::Size,10},{8001,Param::Mode,4},
            {12003,Param::Freeze,0},{15007,Param::LowCut,600},{17003,Param::Mix,50}}}) {
            if(frame>=at && frame<at+frames) { auto& e=events[n++]; e.frameOffset=frame-at; e.paramIndex=unsigned(param); e.value=value; }
        }
        float* out[]{a.l.data()+at,a.r.data()+at};
        PluginProcessContext c; c.inputs=in;c.outputs=out;c.inputChannels=c.outputChannels=std::uint16_t(channels);c.frames=frames;c.inputEvents={events.data(),n};
        countAllocations=true; p.process(c); countAllocations=false;
    }
    return a;
}
double difference(const Audio& a,const Audio& b) {
    double error=0;
    for(unsigned i=0;i<a.l.size();++i) error=std::max({error,double(std::abs(a.l[i]-b.l[i])),double(std::abs(a.r[i]-b.r[i]))});
    return error;
}
double energy(const std::vector<float>& a,unsigned begin=0,unsigned end=0) {
    if(!end) end=unsigned(a.size());
    double sum=0; for(unsigned i=begin;i<end;++i) sum+=double(a[i])*a[i]; return sum/std::max(1u,end-begin);
}
Audio processAudio(const Audio& source,const Values& params,unsigned block=127) {
    ReverbInstance p;for(unsigned i=0;i<params.size();++i)p.setParameterFromHost(i,params[i]);p.activate({48000,block});
    Audio out{std::vector<float>(source.l.size()),std::vector<float>(source.r.size())};
    for(unsigned at=0;at<source.l.size();at+=block) {
        const float* input[]{source.l.data()+at,source.r.data()+at};float* output[]{out.l.data()+at,out.r.data()+at};
        PluginProcessContext c;c.inputs=input;c.outputs=output;c.inputChannels=c.outputChannels=2;c.frames=std::min(block,unsigned(source.l.size())-at);
        countAllocations=true;p.process(c);countAllocations=false;
    }
    return out;
}
void writeWave(const std::filesystem::path& path,const Audio& a) {
    std::ofstream f(path,std::ios::binary);const auto word=[&](std::uint32_t n,unsigned count){for(unsigned i=0;i<count;++i)f.put(char(n>>(i*8)));};
    f.write("RIFF",4);word(36+unsigned(a.l.size())*4,4);f.write("WAVEfmt ",8);word(16,4);word(1,2);word(2,2);word(48000,4);word(192000,4);word(4,2);word(16,2);f.write("data",4);word(unsigned(a.l.size())*4,4);
    for(unsigned i=0;i<a.l.size();++i)for(float x:{a.l[i],a.r[i]})word(std::uint16_t(std::int16_t(std::clamp(x,-1.f,1.f)*32767)),2);
    check(bool(f),"audition WAV written");
}
void audition(const std::filesystem::path& folder) {
    std::filesystem::create_directories(folder);
    constexpr double tau=2*std::numbers::pi;
    for(unsigned kind=0;kind<4;++kind) {
        Audio source{std::vector<float>(48000*12),std::vector<float>(48000*12)};std::uint32_t random=72341;
        for(unsigned i=0;i<48000*6;++i) {
            const double t=i/48000.,beat=std::fmod(t,.5),phrase=std::fmod(t,1.5);double x=0;
            random=random*1664525+1013904223;const double noise=double(random)/UINT32_MAX*2-1;
            if(kind==0) { // Vowel-like source with phrase gaps, not a recorded singer.
                const double envelope=phrase<.85?std::min(1.,phrase/.04)*std::min(1.,(.85-phrase)/.08):0;
                for(unsigned h=1;h<=20;++h){const double hz=150*h;const double formant=std::exp(-std::pow((hz-750)/350,2))+.4*std::exp(-std::pow((hz-1400)/250,2));x+=std::sin(tau*hz*t)*formant*.07*envelope;}
            } else if(kind==1) {
                const double notes[]{261.6256,329.6276,391.9954,523.2511};const double hz=notes[unsigned(t*2)%4];
                for(unsigned h=1;h<=9;++h)x+=.24/h*std::sin(tau*hz*h*beat)*std::exp(-beat*(2+h*.85));
            } else if(kind==2) x=.45*std::sin(tau*(55*beat+2.5*(1-std::exp(-beat*45))))*std::exp(-beat*20)+noise*.18*std::exp(-beat*45);
            else {const double env=std::min(1.,t/1.2)*std::min(1.,(6-t)/1.4);for(double hz:{130.8128,164.8138,195.9977})x+=.10*std::sin(tau*hz*t)*env;}
            source.l[i]=float(x);source.r[i]=float(x*.96);
        }
        const char* names[]{"synthetic-vowel","piano-like","drum-like","pad"};const std::string name=names[kind];
        writeWave(folder/(name+"-dry.wav"),source);
        auto params=factoryPresets()[0].values;writeWave(folder/(name+"-soft-hall.wav"),processAudio(source,params));
        if(kind==0){params[9]=70;writeWave(folder/(name+"-duck.wav"),processAudio(source,params));}
        if(kind==3){params=factoryPresets()[5].values;params[6]=40;writeWave(folder/(name+"-shimmer.wav"),processAudio(source,params));}
    }
}
}
int main(int argc,char** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    if(argc>2 && std::string_view(argv[1])=="--audition") {audition(argv[2]);return failures?1:0;}
    if(argc>1 && std::string_view(argv[1])=="--benchmark") {
        for(unsigned mode=0;mode<6;++mode) {
            const auto start=std::chrono::steady_clock::now(); render(48000,128,mode,false,2,10);
            const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            std::printf("%.*s: %.2f ms / 10 s audio (%.2f%% core)\n",int(modeName(mode).size()),modeName(mode).data(),ms,ms/100);
        }
        return 0;
    }
    InternalFactory factory; auto instance=factory.create(ReverbInstance::staticDescriptor());
    check(instance && instance->descriptor().name=="Reverb" && instance->parameters().size()==16,"Reverb registration and stable parameter count");
    bool ids=true; for(const auto& p:parameterTable()) ids &= p.isAutomatable && instance->parameterIndexForId(p.id)==int(p.index) && instance->parameterValue(p.index)==p.defaultValue;
    check(ids,"parameter IDs, automation and defaults");
    ReverbInstance p; set(p,Param::Decay,7); p.captureComparison('A'); p.setActiveComparison('B'); set(p,Param::Decay,2);
    p.setPresetReference("user","Vocal room");
    std::vector<std::uint8_t> bytes; p.saveState(bytes); ReverbInstance loaded;
    check(loaded.loadState(bytes) && loaded.activeComparison()=='B' && loaded.comparison('A')[4]==7 && loaded.comparison('B')[4]==2 && loaded.presetReference().second=="Vocal room","state round-trip preserves both A/B banks and preset");
    const std::string invalid=R"({"version":1,"params":{"decay":3},"a":{"mix":"bad"}})";
    check(!loaded.loadState({reinterpret_cast<const std::uint8_t*>(invalid.data()),invalid.size()}) && loaded.parameterValue(4)==2,"malformed state is rejected without partial mutation");
    PluginBusLayout accepted;
    check(p.setBusLayout({{1,2},{1}},accepted) && p.setBusLayout({{2,2},{2}},accepted) && !p.setBusLayout({{6},{6}},accepted),"mono/stereo plus optional sidechain buses");
    check(factoryPresets().size()==6 && factoryPresets()[0].values[4]==4.2,"six musical presets with Soft Hall as default");
    for(double rate:{44100.,48000.,96000.,192000.}) {
        // Dual Mono uses two independent instances of this mono path.
        for(unsigned channels:{1u,2u}) {
            const auto reference=render(rate,1,0,true,channels);
            for(unsigned block:{32u,64u,127u,512u,2048u})
                check(difference(reference,render(rate,block,0,true,channels))<1.e-6,"mono/stereo sample-offset edits and Freeze are independent of block size");
        }
        const auto mono=render(rate,127,0,false,1);
        check(energy(mono.l)>1.e-8,"mono output retains the reverberation tail");
    }
    const auto hall=render(48000,127,0);
    for(unsigned mode=0;mode<6;++mode) {
        const auto a=render(48000,127,mode,false,2,2);
        bool finite=true; double peak=0;
        for(float v:a.l) {finite &= std::isfinite(v);peak=std::max(peak,double(std::abs(v)));}
        check(finite && peak<4 && energy(a.l,24000)>1.e-9,"each mode produces a finite, decaying stereo tail");
        if(mode) check(difference(hall,render(48000,127,mode))>1.e-4,"space profiles sound different");
    }
    {
        Audio tone{std::vector<float>(96000),std::vector<float>(96000)};
        for(unsigned i=0;i<tone.l.size();++i)tone.l[i]=tone.r[i]=float(.2*std::sin(2*std::numbers::pi*200*i/48000.));
        auto params=factoryPresets()[5].values;params[3]=0;params[6]=100;params[7]=0;params[8]=0;params[12]=100;params[15]=0;
        const auto plain=processAudio(tone,params);params[15]=100;const auto shifted=processAudio(tone,params);
        const auto octave=[](const Audio& a){double re=0,im=0;for(unsigned i=48000;i<96000;++i){const double phase=2*std::numbers::pi*400*i/48000.;re+=a.l[i]*std::cos(phase);im+=a.l[i]*std::sin(phase);}return std::hypot(re,im)/24000.;};
        const double baseline=octave(plain),shimmer=octave(shifted);
        std::printf("Shimmer 400 Hz amplitude: %.8f (unshifted %.8f)\n",shimmer,baseline);
        check(shimmer>baseline*5+1.e-5,"Shimmer generates the octave above a 200 Hz source");
        Audio impulse{std::vector<float>(96000),std::vector<float>(96000)};impulse.l[0]=impulse.r[0]=1;
        params=factoryPresets()[1].values;params[3]=0;params[6]=100;params[7]=0;params[8]=0;params[12]=100;
        const auto fast=processAudio(impulse,params);params[7]=100;const auto bloomed=processAudio(impulse,params);
        const auto centroid=[](const Audio& a){double sum=0,weighted=0;for(unsigned i=0;i<a.l.size();++i){const double e=double(a.l[i])*a.l[i]+double(a.r[i])*a.r[i];sum+=e;weighted+=e*i;}return weighted/std::max(sum,1.e-20)/48000.;};
        check(centroid(bloomed)>centroid(fast)+.04,"Bloom moves late-tail energy later in time");
    }
    // Frozen network must exclude new input, retain energy and not sleep.
    ReverbInstance frozenA,frozenB;
    for(auto* v:{&frozenA,&frozenB}) {set(*v,Param::Mix,100);v->activate({48000,128});v->startProcessing();}
    std::array<float,128> input{},silent{},la{},ra{},lb{},rb{};
    const float* in[]{input.data(),input.data()}; const float* zero[]{silent.data(),silent.data()};
    float* outA[]{la.data(),ra.data()},*outB[]{lb.data(),rb.data()};
    PluginProcessContext c;c.inputs=in;c.outputs=outA;c.inputChannels=c.outputChannels=2;c.frames=128;
    double earlyEnergy=0,lateEnergy=0,error=0;
    for(unsigned b=0;b<4500;++b) {
        for(unsigned i=0;i<128;++i) input[i]=b<150 || b>800 ? float(.2*std::sin((b*128+i)*.093)) : 0.f;
        if(b==150) {set(frozenA,Param::Freeze,1);set(frozenB,Param::Freeze,1);}
        c.inputs=in;c.outputs=outA;frozenA.process(c);
        c.inputs=b>800?zero:in;c.outputs=outB;frozenB.process(c);
        if(b>800) for(unsigned i=0;i<128;++i) error=std::max(error,double(std::abs(la[i]-lb[i])));
        if(b>=1000 && b<1500) for(float v:la)earlyEnergy+=double(v)*v;
        if(b>=4000) for(float v:la)lateEnergy+=double(v)*v;
    }
    check(error<1.e-7,"Freeze excludes incoming audio after its ramp");
    check(earlyEnergy>1.e-8 && lateEnergy/earlyEnergy>.5 && lateEnergy/earlyEnergy<2,"Freeze holds energy without decay or runaway");
    check(frozenA.tailSamples()==UINT32_MAX && frozenA.latencySamples()==0,"infinite frozen tail and zero dry-path latency");
    ReverbInstance internalDuck,externalDuck;
    for(auto* v:{&internalDuck,&externalDuck})set(*v,Param::Duck,100);
    internalDuck.activate({48000,128,false,false});externalDuck.activate({48000,128,false,true});
    input.fill(.5f);c.inputs=in;c.sidechainInputs=zero;c.sidechainInputChannels=2;
    for(unsigned b=0;b<400;++b) {c.outputs=outA;internalDuck.process(c);c.outputs=outB;externalDuck.process(c);}
    check(internalDuck.telemetry().duckGain<.3f && externalDuck.telemetry().duckGain==1,"silent external sidechain never falls back to main input");
    ReverbInstance dry;set(dry,Param::Mix,0);dry.activate({48000,128});c.outputs=outA;dry.process(c);
    check(la[20]==.5f,"Mix zero is exact dry pass-through");
    for(const auto& info:parameterTable())set(dry,Param(info.index),info.maxValue);
    input.fill(.1f);
    bool bounded=true;
    for(unsigned b=0;b<1200;++b) {c.outputs=outA;dry.process(c);for(float v:la)bounded &= std::isfinite(v) && std::abs(v)<20;}
    check(bounded,"extreme settings including Shimmer and Freeze remain bounded");
    check(allocations==0,"audio processing allocates no memory");
    return failures?1:0;
}
