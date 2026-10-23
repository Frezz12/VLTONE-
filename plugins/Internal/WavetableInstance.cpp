#include "Internal/WavetableInstance.hpp"
#include "Internal/WavetableDsp.hpp"
#include "Common/RealtimeSnapshot.hpp"
#include <atomic>
#include <cmath>
#include <cstdio>

namespace daw::plugins::wavetable {
struct WavetableInstance::Impl {
    engine::RealtimeSnapshot<PreparedPatch> patch;
    Renderer renderer;
    std::array<std::atomic<double>,parameterCount> parameters;
    std::array<double,parameterCount> values{};
    std::atomic<unsigned> voices{0},latency{0};std::atomic<float> peak{0};
    double rate=48000;std::uint64_t revision=0,audioRevision=0;
};
WavetableInstance::WavetableInstance():m(std::make_unique<Impl>()) {
    static_assert(std::atomic<double>::is_always_lock_free);
    for(auto& p:parameterTable())m->parameters[p.index].store(p.defaultValue,std::memory_order_relaxed);
    setState(initialState());
}
WavetableInstance::~WavetableInstance()=default;
const PluginDescriptor& WavetableInstance::staticDescriptor() {
    static const PluginDescriptor d=[] {PluginDescriptor p;p.format=Format::Internal;p.uid=p.path=uid;p.name="VLT Wavetable";p.vendor="VLTONE";p.version="0.1.0";p.stateSchemaVersion=1;p.category="Instrument";p.isInstrument=p.wantsMidi=true;p.mainInputChannels=0;p.mainOutputChannels=2;return p;}();return d;
}
bool WavetableInstance::activate(const PluginProcessInfo& info) {
    if(!std::isfinite(info.sampleRate)||info.sampleRate<8000||info.sampleRate>384000||!info.maxBlockSize)return false;
    auto saved=state();m->rate=info.sampleRate;m->renderer.prepare(m->rate);
    if(!setState(saved))return false;m_active=true;reset();return true;
}
void WavetableInstance::deactivate(){m_active=m_processing=false;reset();}
double WavetableInstance::parameterValue(std::uint32_t i) const noexcept {return i<parameterCount?m->parameters[i].load(std::memory_order_relaxed):0;}
std::string WavetableInstance::parameterText(std::uint32_t i,double v) const {
    if(i>=parameterCount||!std::isfinite(v))return {};
    char text[80];auto& def=parameterTable()[i];std::snprintf(text,sizeof(text),def.isStepped?"%.0f %s":"%.3g %s",v,def.unit.c_str());return text;
}
void WavetableInstance::setParameterFromHost(std::uint32_t i,double v) {
    if(i>=parameterCount||!std::isfinite(v))return;auto& p=parameterTable()[i];v=std::clamp(p.isStepped?std::round(v):v,p.minValue,p.maxValue);m->parameters[i].store(v,std::memory_order_relaxed);
}
State WavetableInstance::state() const {
    auto p=m->patch.controlCopy();auto s=p?p->state:initialState();for(unsigned i=0;i<parameterCount;++i)s.values[i]=parameterValue(i);return s;
}
bool WavetableInstance::setState(const State& s) {
    try {auto p=preparePatch(s,m->rate,m->revision+1);if(!p)return false;const unsigned latency=p->latency;
        m->patch.publish(std::move(p));++m->revision;for(unsigned i=0;i<parameterCount;++i)m->parameters[i].store(s.values[i],std::memory_order_relaxed);
        if(m->latency.exchange(latency)!=latency&&m_listener)m_listener->onLatencyChanged();return true;
    } catch(...) {return false;}
}
bool WavetableInstance::saveState(std::vector<std::uint8_t>& bytes) const{return encodeState(state(),bytes);}
bool WavetableInstance::loadState(std::span<const std::uint8_t> bytes){auto decoded=decodeState(bytes);return decoded&&setState(*decoded);}
Telemetry WavetableInstance::telemetry() const noexcept {return {m->voices.load(std::memory_order_relaxed),m->peak.load(std::memory_order_relaxed),m->latency.load(std::memory_order_relaxed)};}
std::uint32_t WavetableInstance::latencySamples() const noexcept {return m->latency.load(std::memory_order_relaxed);}
std::uint32_t WavetableInstance::tailSamples() const noexcept {return std::uint32_t(m->rate*32);}
void WavetableInstance::reset() noexcept {m->renderer.reset();m->voices.store(0,std::memory_order_relaxed);m->peak.store(0,std::memory_order_relaxed);auto p=m->patch.read();if(p){for(auto& fx:p->effects)if(fx)fx->reset();for(auto& h:const_cast<PreparedPatch&>(*p).busAlign)h.reset();}}
PluginProcessDisposition WavetableInstance::process(const PluginProcessContext& c) noexcept {
    if(!c.outputs)return PluginProcessDisposition::Error;
    auto p=m->patch.read();
    if(!m_active||!m_processing||!p){for(unsigned ch=0;ch<c.outputChannels;++ch)if(c.outputs[ch])std::fill_n(c.outputs[ch],c.frames,0.f);return PluginProcessDisposition::Continue;}
    for(unsigned ch=0;ch<c.outputChannels;++ch)if(!c.outputs[ch])return PluginProcessDisposition::Error;
    if(m->audioRevision!=p->revision){m->renderer.reset();m->audioRevision=p->revision;}
    for(unsigned i=0;i<parameterCount;++i)m->values[i]=m->parameters[i].load(std::memory_order_relaxed);
    unsigned cursor=0;
    for(auto& event:c.inputEvents){unsigned at=std::clamp(event.frameOffset,cursor,c.frames);if(at>cursor)m->renderer.render(const_cast<PreparedPatch&>(*p),c,m->values,cursor,at);m->renderer.event(event,*p,m->values);if(event.kind==PluginEvent::Kind::ParamValue&&event.paramIndex<parameterCount)m->parameters[event.paramIndex].store(m->values[event.paramIndex],std::memory_order_relaxed);cursor=at;}
    if(cursor<c.frames)m->renderer.render(const_cast<PreparedPatch&>(*p),c,m->values,cursor,c.frames);
    float peak=0;for(unsigned ch=0;ch<c.outputChannels;++ch)for(unsigned i=0;i<c.frames;++i)peak=std::max(peak,std::abs(c.outputs[ch][i]));m->peak.store(peak,std::memory_order_relaxed);m->voices.store(m->renderer.activeVoices(),std::memory_order_relaxed);return PluginProcessDisposition::Continue;
}
}
