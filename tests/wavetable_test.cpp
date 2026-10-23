#include "Internal/WavetableInstance.hpp"
#include "Internal/WavetableDsp.hpp"
#include "Internal/InternalFactory.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <set>

namespace {thread_local bool audit=false;thread_local unsigned allocations=0,releases=0;}
void* operator new(std::size_t n){if(audit)++allocations;if(auto p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{if(audit&&p)++releases;std::free(p);}
void operator delete[](void* p)noexcept{::operator delete(p);}
void operator delete(void* p,std::size_t)noexcept{::operator delete(p);}
void operator delete[](void* p,std::size_t)noexcept{::operator delete(p);}
using namespace daw::plugins;
using namespace daw::plugins::wavetable;
namespace {
unsigned failures=0;
void check(bool v,const char* text){std::printf("%s %s\n",v?"PASS":"FAIL",text);failures+=!v;}
std::vector<float> render(WavetableInstance& synth,unsigned frames,unsigned block,double rate=48000) {
    synth.activate({rate,block});synth.startProcessing();std::vector<float> out(frames*2);
    for(unsigned i=0;i<frames;i+=block){unsigned n=std::min(block,frames-i);float* outputs[]{out.data()+i,out.data()+frames+i};PluginProcessContext c;c.outputs=outputs;c.outputChannels=2;c.frames=n;c.playing=true;c.sampleTime=i;c.transport.tempo=120;c.transport.ppqPosition=double(i)/rate*2;
        PluginEvent event;event.kind=PluginEvent::Kind::NoteOn;event.key=69;event.value=.8;event.noteId=1;event.frameOffset=17;if(i==0)c.inputEvents={&event,1};
        audit=true;synth.process(c);audit=false;
    }return out;
}
}
int main(){
    auto initial=initialState();check(parameterTable().size()==parameterCount,"parameter schema size");check(validState(initial),"initial state valid");
    std::set<std::string> ids;for(auto& p:parameterTable())ids.insert(p.id);check(ids.size()==parameterCount,"stable parameter IDs are unique");
    std::vector<std::uint8_t> bytes;check(encodeState(initial,bytes)&&decodeState(bytes)==initial,"state CBOR round trip");
    auto invalid=initial;invalid.values[Output]=NAN;check(!validState(invalid)&&!encodeState(invalid,bytes),"reject non-finite state");
    check(factoryPresets().size()==256,"256 factory presets");bool library=true;std::set<std::string> names;
    for(unsigned i=0;i<256;++i){auto s=factoryState(i);library&=validState(s);names.insert(s.name);}
    check(library&&names.size()==256,"factory library is valid and names are unique");
    WavetableInstance a,b;auto x=render(a,4800,64),y=render(b,4800,512);double difference=0,energy=0;for(unsigned i=0;i<x.size();++i){difference=std::max(difference,std::abs(double(x[i]-y[i])));energy+=x[i]*x[i];}
    check(energy>1&&std::isfinite(energy),"instrument produces finite audio");check(difference<1e-6,"render invariant to block size");check(std::all_of(x.begin(),x.begin()+17,[](float v){return v==0;}),"MIDI note starts at exact sample offset");
    check(allocations==0&&releases==0,"audio callback does not allocate or release heap memory");
    auto before=a.state();check(!a.loadState({reinterpret_cast<const std::uint8_t*>("bad"),3})&&a.state()==before,"failed state import preserves instrument");
    check(a.saveState(bytes)&&b.loadState(bytes)&&a.state()==b.state(),"instance state round trip");
    a.reset();check(a.telemetry().voices==0,"reset releases voices");
    InternalFactory factory;auto instance=factory.create(WavetableInstance::staticDescriptor());check(bool(instance)&&instance->descriptor().isInstrument&&instance->pitchCapabilities().mpe,"native factory and MIDI capability contract");
    for(double rate:{44100.,48000.,96000.}){WavetableInstance s;auto output=render(s,512,128,rate);check(std::all_of(output.begin(),output.end(),[](float v){return std::isfinite(v);}),"supported sample rate");}
    return failures?1:0;
}
