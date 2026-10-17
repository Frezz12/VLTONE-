#include "Internal/Vlt1073Instance.hpp"
#include "Internal/InternalFactory.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <tuple>
#include <vector>

namespace { std::atomic<bool> trackAllocations{false}; std::atomic<unsigned> allocations{0}; }
void* operator new(std::size_t n){if(trackAllocations)++allocations;if(void* p=std::malloc(std::max(n,std::size_t(1))))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}
void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}
void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
namespace {
using namespace daw::plugins;using namespace daw::plugins::vlt1073;
int failures=0;
void check(bool yes,const char* name){std::printf("%s %s\n",yes?"PASS":"FAIL",name);failures+=!yes;}
double db(double v){return 20*std::log10(std::max(std::abs(v),1e-20));}
struct Block {
    std::vector<float> l,r,a,b;const float* in[2];float* out[2];
    explicit Block(unsigned n):l(n),r(n),a(n),b(n),in{l.data(),r.data()},out{a.data(),b.data()}{}
    void run(Vlt1073Instance& p,unsigned n=0,std::span<const PluginEvent> events={}){PluginProcessContext c;c.inputs=in;c.outputs=out;c.inputChannels=c.outputChannels=2;c.frames=n?n:unsigned(l.size());c.inputEvents=events;p.process(c);}
};
using Edits=std::vector<std::pair<unsigned,double>>;
std::vector<float> render(double rate,double hz,double amplitude,const Edits& edits={},unsigned block=257,bool events=false,double seconds=.5,bool offline=false){
    Vlt1073Instance p;for(auto [i,v]:edits)p.setParameterFromHost(i,v);p.activate({rate,block,offline});p.startProcessing();Block b(block);unsigned n=unsigned(rate*seconds);std::vector<float> out(n);
    for(unsigned at=0;at<n;at+=block){unsigned frames=std::min(block,n-at);for(unsigned i=0;i<frames;++i)b.l[i]=b.r[i]=float(amplitude*std::sin(2*std::numbers::pi*hz*(at+i)/rate));
        std::array<PluginEvent,6> ev{};unsigned count=0;
        if(events)for(auto [pos,id,value]:std::array<std::tuple<unsigned,unsigned,double>,6>{{{13,6,12},{151,14,-12},{901,7,4},{1301,3,15},{2318,11,3},{4027,12,0}}})if(pos>=at&&pos<at+frames){auto& e=ev[count++];e.kind=PluginEvent::Kind::ParamValue;e.frameOffset=pos-at;e.paramIndex=id;e.value=value;}
        b.run(p,frames,{ev.data(),count});std::copy_n(b.a.begin(),frames,out.begin()+at);
    }return out;
}
double component(const std::vector<float>& v,double hz,double rate,unsigned n){double re=0,im=0;for(unsigned i=0;i<n;++i){double a=2*std::numbers::pi*hz*i/rate;re+=v[v.size()-n+i]*std::cos(a);im+=v[v.size()-n+i]*std::sin(a);}return 2*std::hypot(re,im)/n;}
double maxError(const std::vector<float>& a,const std::vector<float>& b,double scale=1){double e=0;if(a.size()!=b.size())return 1e9;for(unsigned i=0;i<a.size();++i)e=std::max(e,std::abs(double(a[i])-b[i]*scale));return e;}
}
int main(){
    std::setvbuf(stdout,nullptr,_IONBF,0);InternalFactory f;auto instance=f.create(Vlt1073Instance::staticDescriptor());check(instance&&instance->descriptor().name=="VLT 1073"&&instance->descriptor().vendor=="VLTONE"&&instance->descriptor().stateSchemaVersion==1,"registered identity and schema");
    bool defaults=true;for(const auto& p:parameterTable())defaults&=instance->parameterIndexForId(p.id)==int(p.index)&&instance->parameterValue(p.index)==p.defaultValue;
    check(defaults&&parameterTable().size()==16,"stable parameter IDs and defaults");
    Vlt1073Instance state;for(const auto& p:parameterTable())state.setParameterFromHost(p.index,p.maxValue);state.setParameterFromHost(14,-90);
    std::vector<std::uint8_t> bytes;state.saveState(bytes);Vlt1073Instance copy;bool roundtrip=copy.loadState(bytes);for(const auto& p:parameterTable())roundtrip&=state.parameterValue(p.index)==copy.parameterValue(p.index);
    check(roundtrip&&copy.parameterText(14,-90)=="-inf dB","complete state and finite mute sentinel round trip");
    const std::string malformed=R"({"version":1,"params":{"highGain":4,"lowGain":"bad"}})";
    check(!copy.loadState({reinterpret_cast<const std::uint8_t*>(malformed.data()),malformed.size()})&&copy.parameterValue(6)==16,"invalid state rejected atomically");
    state.setParameterFromHost(2,77);state.setParameterFromHost(3,12);check(state.parameterValue(2)==75&&state.parameterValue(3)==10,"gain switches quantize to 5 dB");
    PluginBusLayout accepted;check(state.setBusLayout({{1},{1}},accepted)&&state.setBusLayout({{2},{2}},accepted)&&!state.setBusLayout({{2,2},{2}},accepted)&&!state.setBusLayout({{1},{2}},accepted),"mono/stereo bus contract");
    Vlt1073Instance impulse;impulse.activate({48000,512});impulse.startProcessing();Block b(512);b.l[0]=b.r[0]=1e-6f;b.run(impulse);
    check(unsigned(std::max_element(b.a.begin(),b.a.end())-b.a.begin())==48&&impulse.latencySamples()==48,"4x FIR latency is 48 samples");
    bool robust=true,independent=true,reset=true;
    for(double rate:{44100.,48000.,88200.,96000.,176400.,192000.})for(unsigned block:{1u,17u,257u,1024u}){
        Vlt1073Instance p;p.activate({rate,block});p.startProcessing();Block t(block);for(unsigned k=0;k<8;++k){for(unsigned i=0;i<block;++i){t.l[i]=k<4?float((i%3-1.)*3):0;t.r[i]=0;}
            if(k==2){for(const auto& param:parameterTable())p.setParameterFromHost(param.index,param.maxValue);}if(k==5){for(const auto& param:parameterTable())p.setParameterFromHost(param.index,param.minValue);}
            if(k==6)t.l[0]=std::numeric_limits<float>::quiet_NaN();if(k==7)t.l[0]=std::numeric_limits<float>::infinity();
            trackAllocations=true;t.run(p);trackAllocations=false;
            for(unsigned i=0;i<block;++i){robust&=std::isfinite(t.a[i])&&std::abs(t.a[i])<100;independent&=t.b[i]==0;}}
        p.reset();std::fill(t.l.begin(),t.l.end(),0);t.run(p);for(float x:t.a)reset&=x==0;
    }
    check(robust,"finite extremes and invalid input at 6 rates / 4 block sizes");check(independent,"stereo channel memories are independent");check(reset,"reset clears every filter and transformer memory");check(allocations==0,"no audio-thread allocations including automation");
    const auto event1=render(48000,431,.1,{},1,true),event257=render(48000,431,.1,{},257,true),event1024=render(48000,431,.1,{},1024,true,.5,true);
    std::printf("MEASURE event/block difference %.9g / %.9g\n",maxError(event1,event257),maxError(event1,event1024));
    check(maxError(event1,event257)<1e-7&&maxError(event1,event1024)<1e-7,"sample-offset automation independent of blocks and offline flag");
    auto flat=render(48000,1000,1e-5),line=render(48000,1000,1e-5,{{3,20}}),mic=render(48000,1000,1e-5,{{0,1},{2,20}}),pad=render(48000,1000,1e-5,{{0,1},{2,20},{5,1}});
    const double ref=component(flat,1000,48000,12000);check(std::abs(db(component(line,1000,48000,12000)/ref)-20)<.02&&std::abs(db(component(mic,1000,48000,12000)/ref)-20)<.02,"physical MIC and LINE gain (no automatic compensation)");
    check(std::abs(db(component(mic,1000,48000,12000)/component(pad,1000,48000,12000))-20)<.02,"PAD attenuates before the microphone amplifier");
    check(maxError(flat,render(48000,1000,1e-5,{{4,0},{5,1}}))==0,"PAD and MIC Z have no effect in LINE");
    check(maxError(render(48000,431,.5,{{1,0}}),std::vector<float>(24000))==0&&maxError(render(48000,431,.5,{{14,-90}}),std::vector<float>(24000))==0,"input OFF and Level -infinity produce silence");
    const auto driven=render(48000,1000,.25,{{3,20}}),trimmed=render(48000,1000,.25,{{3,20},{15,-12}}),leveled=render(48000,1000,.25,{{3,20},{14,-12}});
    check(maxError(trimmed,driven,std::pow(10.,-12./20))<2e-7,"Output scales the complete waveform without changing relative harmonics");
    auto thd=[&](const std::vector<float>& y){return std::hypot(component(y,2000,48000,12000),component(y,3000,48000,12000))/component(y,1000,48000,12000);};
    std::printf("MEASURE driven H2+H3 %.6f%%; Level -12 %.6f%%; Output -12 %.6f%%\n",thd(driven)*100,thd(leveled)*100,thd(trimmed)*100);
    check(thd(leveled)<thd(driven)*.9,"Level changes the output-stage distortion");
    bool frequencies=true;for(unsigned i=1;i<7;++i){auto y=render(48000,kMidHz[i],1e-6,{{7,double(i)},{8,18}});auto n=render(48000,kMidHz[i],1e-6);double gain=db(component(y,kMidHz[i],48000,12000)/component(n,kMidHz[i],48000,12000));frequencies&=std::abs(gain-18)<.12;}
    check(frequencies,"all six presence detents reach +18 dB at their physical frequency");
    auto hp1=render(48000,20,1e-5,{{11,4}}),hp2=render(48000,40,1e-5,{{11,4}});double slope=db(component(hp2,40,48000,12000)/component(hp1,20,48000,12000));
    std::printf("MEASURE HPF low-frequency slope %.4f dB/octave\n",slope);check(slope>17.8&&slope<18.5,"HPF has an 18 dB/octave stopband");
    std::array<Vlt1073Instance,8> processors;for(auto& p:processors){p.activate({48000,256});p.startProcessing();p.setParameterFromHost(6,9);p.setParameterFromHost(8,-8);}
    Block bench(256);for(unsigned i=0;i<256;++i)bench.l[i]=bench.r[i]=float(.1*std::sin(i));auto start=std::chrono::steady_clock::now();
    for(unsigned i=0;i<375;++i)for(auto& p:processors)bench.run(p);
    double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();std::printf("MEASURE 8 stereo instances / 2 s audio: %.6f s CPU wall, %.2f%% of one core\n",seconds,seconds/2*100);
    return failures?1:0;
}
