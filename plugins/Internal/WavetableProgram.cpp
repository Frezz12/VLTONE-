#include "Internal/WavetableProgram.hpp"
#include "Internal/FluxProgram.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>

namespace daw::plugins::wavetable {
namespace {
using json = nlohmann::json;
struct Field { const char* id; const char* name; const char* unit; double lo, hi, initial; bool stepped = false, log = false; };
constexpr Field globals[]{
    {"output", "Output", "dB", -60, 12, -12}, {"polyphony", "Voices", "", 1, 64, 32, true},
    {"mono", "Mono", "", 0, 1, 0, true}, {"legato", "Legato", "", 0, 1, 0, true},
    {"glide", "Glide", "s", 0, 4, 0}, {"bend_range", "Pitch bend range", "st", 0, 96, 2},
    {"velocity", "Velocity sensitivity", "", 0, 1, .8}, {"filter_routing", "Parallel filters", "", 0, 1, 0, true},
    {"arp.enabled", "Arpeggiator", "", 0, 1, 0, true}, {"arp.rate", "Arp step", "beats", .015625, 4, .25},
    {"arp.gate", "Arp gate", "", .01, 1, .8}, {"arp.octaves", "Arp octaves", "", 1, 4, 1, true},
    {"arp.direction", "Arp direction", "", 0, 3, 0, true}, {"clip.enabled", "Clip player", "", 0, 1, 0, true},
    {"clip.selected", "Clip", "", 0, 11, 0, true}, {"swing", "Swing", "", 0, .75, 0}};
constexpr Field oscillators[]{
    {"enabled", "Enabled", "", 0, 1, 0, true}, {"mode", "Mode", "", 0, 4, 0, true},
    {"level", "Level", "dB", -96, 12, -6}, {"pan", "Pan", "", -1, 1, 0},
    {"octave", "Octave", "", -8, 8, 0, true}, {"semitone", "Semitone", "st", -48, 48, 0},
    {"fine", "Fine", "ct", -100, 100, 0}, {"unison", "Unison", "", 1, 16, 1, true},
    {"detune", "Detune", "", 0, 1, .15}, {"spread", "Stereo spread", "", 0, 1, .8},
    {"phase", "Phase", "", 0, 1, 0}, {"random_phase", "Random phase", "", 0, 1, 0},
    {"position", "Table position", "", 0, 1, .33}, {"warp", "Warp", "", 0, 8, 0, true},
    {"warp_amount", "Warp amount", "", 0, 1, 0}, {"warp2", "Warp 2", "", 0, 8, 0, true},
    {"warp2_amount", "Warp 2 amount", "", 0, 1, 0}, {"filter_send", "Filter send", "", 0, 1, 1},
    {"start", "Sample start", "", 0, 1, 0}, {"end", "Sample end", "", 0, 1, 1},
    {"loop", "Loop", "", 0, 1, 1, true}, {"rate", "Scan rate", "", -4, 4, 1},
    {"grain_size", "Grain size", "ms", 2, 500, 80, false, true},
    {"grain_density", "Grains per second", "Hz", 1, 200, 24, false, true},
    {"grain_spray", "Grain spray", "", 0, 1, .15}};
constexpr Field filters[]{
    {"enabled", "Enabled", "", 0, 1, 0, true}, {"type", "Filter type", "", 0, 7, 0, true},
    {"cutoff", "Cutoff", "Hz", 20, 22000, 18000, false, true}, {"resonance", "Resonance", "", 0, 1, .1},
    {"drive", "Drive", "dB", 0, 36, 0}, {"mix", "Mix", "", 0, 1, 1}};
constexpr Field envelopes[]{
    {"attack", "Attack", "s", .0001, 32, .005, false, true}, {"hold", "Hold", "s", 0, 32, 0},
    {"decay", "Decay", "s", .001, 32, .3, false, true}, {"sustain", "Sustain", "", 0, 1, .8},
    {"release", "Release", "s", .001, 32, .25, false, true}, {"curve", "Curve", "", -1, 1, 0}};
constexpr Field lfos[]{
    {"rate", "Rate", "Hz / beats", .015625, 1000, 1, false, true}, {"sync", "Tempo sync", "", 0, 1, 1, true},
    {"phase", "Phase", "", 0, 1, 0}, {"retrigger", "Retrigger", "", 0, 1, 1, true},
    {"bipolar", "Bipolar", "", 0, 1, 0, true}};
json curveJson(const Curve& c) { json a=json::array(); for(auto p:c.points) a.push_back({p.x,p.y,p.curve}); return a; }
Curve readCurve(const json& a) {
    if(!a.is_array() || a.size()>256) throw std::runtime_error("invalid curve");
    Curve c; c.points.clear(); for(auto& p:a) { if(!p.is_array() || p.size()!=3) throw std::runtime_error("invalid point"); c.points.push_back({p[0].get<float>(),p[1].get<float>(),p[2].get<float>()}); } return c;
}
bool curveValid(const Curve& c) {
    if(c.points.size()<2 || c.points.size()>256 || c.points.front().x!=0 || c.points.back().x!=1) return false;
    float last=-1;
    for(auto p:c.points) { if(!std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(p.curve)||p.x<=last||p.x>1||p.y<0||p.y>1||std::abs(p.curve)>1) return false; last=p.x; }
    return true;
}
}
std::span<const ParameterInfo> parameterTable() {
    static const auto table=[] {
        std::vector<ParameterInfo> t; t.reserve(parameterCount);
        auto add=[&](std::string prefix, const Field& f, std::string group={}) {
            ParameterInfo p; p.index=unsigned(t.size()); p.id=prefix+f.id; p.name=group+f.name; p.unit=f.unit;
            p.minValue=f.lo; p.maxValue=f.hi; p.defaultValue=f.initial; p.isStepped=f.stepped; p.isLogarithmic=f.log; t.push_back(std::move(p));
        };
        for(auto f:globals) add("",f);
        for(unsigned i=0;i<macroCount;++i) add("macro."+std::to_string(i+1), {"","Macro","",0,1,0},"Macro "+std::to_string(i+1)+" ");
        for(unsigned i=0;i<oscillatorCount;++i) for(auto f:oscillators) add("osc."+std::to_string(i+1)+".",f,"Osc "+std::to_string(i+1)+" ");
        for(unsigned i=0;i<2;++i) for(auto f:filters) add("filter."+std::to_string(i+1)+".",f,"Filter "+std::to_string(i+1)+" ");
        for(unsigned i=0;i<envelopeCount;++i) for(auto f:envelopes) add("env."+std::to_string(i+1)+".",f,"Env "+std::to_string(i+1)+" ");
        for(unsigned i=0;i<lfoCount;++i) for(auto f:lfos) add("lfo."+std::to_string(i+1)+".",f,"LFO "+std::to_string(i+1)+" ");
        for(unsigned i=0;i<fxCount;++i) {
            auto p="fx."+std::to_string(i+1)+".";
            add(p,{"enabled","Enabled","",0,1,1,true}); add(p,{"mix","Mix","",0,1,1});
            for(unsigned j=0;j<16;++j) add(p+"value."+std::to_string(j),{"","Value","",-100000,100000,0});
        }
        t[osc(0,On)].defaultValue=1;
        t[osc(4,Octave)].defaultValue=-1;
        t[osc(4,Position)].defaultValue=0;
        return t;
    }(); return table;
}
int parameterIndex(std::string_view id) noexcept {
    // Tables are initialized on the control thread by every instance constructor.
    for(auto& p:parameterTable()) if(p.id==id) return int(p.index);
    return -1;
}
State initialState() { State s; for(auto& p:parameterTable()) s.values[p.index]=p.defaultValue; for(unsigned i=0;i<12;++i)s.clips[i].name="Clip "+std::to_string(i+1); return s; }
bool validState(const State& s, std::string* error) {
    auto fail=[&](const char* e){if(error)*error=e;return false;};
    if(s.name.size()>1024||s.description.size()>16384||s.importReport.size()>4*1024*1024||s.resources.size()>256||s.routes.size()>maxRoutes) return fail("State exceeds limits");
    for(auto& p:parameterTable()) { double v=s.values[p.index]; if(!std::isfinite(v)||v<p.minValue||v>p.maxValue||(p.isStepped&&v!=std::round(v)))return fail("Invalid parameter value"); }
    for(auto& c:s.lfos) if(!curveValid(c))return fail("Invalid LFO curve");
    for(auto& r:s.routes) if(unsigned(r.source)>=unsigned(Source::Count)||parameterIndex(r.target)<0||!std::isfinite(r.amount)||std::abs(r.amount)>1||!curveValid(r.response)) return fail("Invalid modulation route");
    std::size_t samples=0;
    std::vector<std::string> ids;
    for(auto& r:s.resources) {
        if(r.id.empty()||r.id.size()>256||r.channels<1||r.channels>2||r.sampleRate<8000||r.sampleRate>384000||r.frameLength<16||r.frameLength>65536||r.samples.empty()||r.samples.size()%r.channels) return fail("Invalid resource");
        if(std::find(ids.begin(),ids.end(),r.id)!=ids.end())return fail("Duplicate resource ID"); ids.push_back(r.id);
        samples+=r.samples.size(); if(samples>kMaxPluginStateBytes/4)return fail("Resource budget exceeded");
        for(float v:r.samples)if(!std::isfinite(v)||std::abs(v)>32)return fail("Invalid source audio");
    }
    for(unsigned i=0;i<oscillatorCount;++i) {
        auto& o=s.oscillators[i]; if(o.bank>7||o.regions.size()>256)return fail("Invalid oscillator");
        if(!o.resource.empty()&&std::find(ids.begin(),ids.end(),o.resource)==ids.end())return fail("Missing oscillator resource");
        if(s.values[osc(i,Start)]>=s.values[osc(i,End)])return fail("Sample start must precede end");
        for(auto& r:o.regions)if(r.lowKey<0||r.highKey>127||r.lowKey>r.highKey||r.rootKey<0||r.rootKey>127||r.lowVelocity<0||r.highVelocity>127||r.lowVelocity>r.highVelocity||std::find(ids.begin(),ids.end(),r.resource)==ids.end())return fail("Invalid multisample region");
    }
    for(auto& e:s.effects)if(e.kind>=unsigned(flux::Kind::Count)||e.bus>2)return fail("Invalid effect");
    for(auto& c:s.clips) { if(!std::isfinite(c.length)||c.length<=0||c.length>256||c.notes.size()>4096)return fail("Invalid clip"); for(auto n:c.notes)if(!std::isfinite(n.beat)||!std::isfinite(n.length)||!std::isfinite(n.velocity)||n.beat<0||n.beat>=c.length||n.length<=0||n.length>256||n.velocity<0||n.velocity>1||n.key<0||n.key>127)return fail("Invalid clip note"); }
    return true;
}
bool encodeState(const State& s, std::vector<std::uint8_t>& out) {
    try {
        if(!validState(s))return false;
        json j={{"format","vlt.wavetable"},{"version",1},{"name",s.name},{"category",s.category},{"description",s.description},{"author",s.author},{"seed",s.seed},{"macroNames",s.macroNames},{"importReport",s.importReport}};
        for(auto& p:parameterTable())j["parameters"][p.id]=s.values[p.index];
        j["oscillators"]=json::array();
        for(auto& o:s.oscillators){json r=json::array();for(auto& a:o.regions)r.push_back({a.resource,a.lowKey,a.highKey,a.rootKey,a.lowVelocity,a.highVelocity});j["oscillators"].push_back({{"resource",o.resource},{"bank",o.bank},{"regions",r}});}
        j["lfos"]=json::array();for(auto& c:s.lfos)j["lfos"].push_back(curveJson(c));
        j["effects"]=json::array();for(auto& e:s.effects)j["effects"].push_back({e.kind,e.bus});
        j["routes"]=json::array();for(auto& r:s.routes)j["routes"].push_back({{"source",unsigned(r.source)},{"target",r.target},{"amount",r.amount},{"bipolar",r.bipolar},{"enabled",r.enabled},{"curve",curveJson(r.response)}});
        j["clips"]=json::array();for(auto& c:s.clips){json notes=json::array();for(auto n:c.notes)notes.push_back({n.beat,n.length,n.velocity,n.key});j["clips"].push_back({{"name",c.name},{"length",c.length},{"notes",notes}});}
        j["resources"]=json::array();for(auto& r:s.resources){std::vector<std::uint8_t> bytes(r.samples.size()*4);for(std::size_t i=0;i<r.samples.size();++i){auto b=std::bit_cast<std::uint32_t>(r.samples[i]);for(unsigned k=0;k<4;++k)bytes[i*4+k]=std::uint8_t(b>>(k*8));}j["resources"].push_back({{"id",r.id},{"name",r.name},{"sourcePath",r.sourcePath},{"channels",r.channels},{"sampleRate",r.sampleRate},{"frameLength",r.frameLength},{"data",json::binary(std::move(bytes))}});}
        auto bytes=json::to_cbor(j);if(bytes.size()>kMaxPluginStateBytes)return false;out=std::move(bytes);return true;
    } catch(...) {return false;}
}
std::optional<State> decodeState(std::span<const std::uint8_t> bytes) {
    if(bytes.empty()||bytes.size()>kMaxPluginStateBytes)return {};
    try {
        const auto j=json::from_cbor(bytes.begin(),bytes.end());
        if(j.at("format")!="vlt.wavetable"||j.at("version")!=1)return {};
        auto s=initialState();s.name=j.at("name");s.category=j.value("category","");s.description=j.value("description","");s.author=j.value("author","");s.seed=j.value("seed",1u);s.macroNames=j.at("macroNames").get<decltype(s.macroNames)>();s.importReport=j.value("importReport","");
        for(auto& [key,v]:j.at("parameters").items()){int p=parameterIndex(key);if(p<0)return {};s.values[p]=v.get<double>();}
        auto& os=j.at("oscillators");if(os.size()!=oscillatorCount)return {};for(unsigned i=0;i<oscillatorCount;++i){auto& o=s.oscillators[i];o.resource=os[i].at("resource");o.bank=os[i].at("bank");if(os[i].at("regions").size()>256)return {};for(auto& a:os[i].at("regions")){if(a.size()!=6)return {};o.regions.push_back({a[0],a[1],a[2],a[3],a[4],a[5]});}}
        auto& ls=j.at("lfos");if(ls.size()!=lfoCount)return {};for(unsigned i=0;i<lfoCount;++i)s.lfos[i]=readCurve(ls[i]);
        auto& es=j.at("effects");if(es.size()!=fxCount)return {};for(unsigned i=0;i<fxCount;++i){if(es[i].size()!=2)return {};s.effects[i]={es[i][0],es[i][1]};}
        if(j.at("routes").size()>maxRoutes)return {};for(auto& r:j.at("routes"))s.routes.push_back({Source(r.at("source").get<unsigned>()),r.at("target"),r.at("amount"),r.at("bipolar"),r.at("enabled"),readCurve(r.at("curve"))});
        auto& cs=j.at("clips");if(cs.size()!=12)return {};for(unsigned i=0;i<12;++i){auto& c=s.clips[i];c.name=cs[i].at("name");c.length=cs[i].at("length");if(cs[i].at("notes").size()>4096)return {};for(auto& n:cs[i].at("notes")){if(n.size()!=4)return {};c.notes.push_back({n[0],n[1],n[2],n[3]});}}
        if(j.at("resources").size()>256)return {};for(auto& r:j.at("resources")){Resource a;a.id=r.at("id");a.name=r.at("name");a.sourcePath=r.value("sourcePath","");a.channels=r.at("channels");a.sampleRate=r.at("sampleRate");a.frameLength=r.at("frameLength");const auto& data=r.at("data").get_binary();if(data.empty()||data.size()%4)return {};a.samples.resize(data.size()/4);for(std::size_t i=0;i<a.samples.size();++i){std::uint32_t b=0;for(unsigned k=0;k<4;++k)b|=std::uint32_t(data[4*i+k])<<(k*8);a.samples[i]=std::bit_cast<float>(b);}s.resources.push_back(std::move(a));}
        if(validState(s))return s;
    } catch(...) {} return {};
}
float curveValue(const Curve& c, double phase) noexcept {
    const float x=float(std::clamp(phase,0.,1.));
    auto it=std::upper_bound(c.points.begin(),c.points.end(),x,[](float a,const Point& b){return a<b.x;});
    if(it==c.points.begin())return it->y;if(it==c.points.end())return c.points.back().y;
    auto& a=*(it-1);auto& b=*it;float t=(x-a.x)/(b.x-a.x);t+=a.curve*t*(1-t);return a.y+(b.y-a.y)*t;
}
std::uint32_t hash(std::uint32_t x) noexcept {x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;return x^(x>>16);}
double unit(std::uint32_t& s) noexcept {s=hash(s+0x9e3779b9u);return double(s)/4294967296.;}
}
