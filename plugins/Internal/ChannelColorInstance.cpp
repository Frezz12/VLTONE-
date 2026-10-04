#include "Internal/ChannelColorInstance.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>

namespace daw::plugins::channel_color {
namespace {
static_assert(std::atomic<double>::is_always_lock_free);
double clean(double x) noexcept { return std::abs(x) < 1e-24 ? 0 : x; }
// Preserve the mild calibration around -20, then approach saturation gently.
// The asymptote limits both magnetic excitation and the wet/tube contribution.
double softDrive(double drive) noexcept {
    const double u=std::abs(drive)*.01;
    return u<=.2?u:.2+.22*(u-.2)/(.22+u-.2);
}
struct LangevinPoint { double value, slope; };
// Hermite interpolation evaluates both L and its derivative consistently.
// Table construction happens in the constructor, never on an audio worker.
const std::array<LangevinPoint, 4097>& langevinTable() {
    static const auto table = [] {
        std::array<LangevinPoint, 4097> out{};
        for (unsigned i=0; i<out.size(); ++i) {
            const double x=9.0*i/(out.size()-1), x2=x*x;
            if (x < .01) out[i]={x/3-x*x2/45+2*x*x2*x2/945, 1.0/3-x2/15+2*x2*x2/189};
            else {
                const double coth=1/std::tanh(x);
                out[i]={coth-1/x, 1/(x*x)-coth*coth+1};
            }
        }
        return out;
    }();
    return table;
}
LangevinPoint langevin(double x) noexcept {
    constexpr double step=9.0/4096;
    const double position=std::min(std::abs(x)/step, 4095.999999);
    const unsigned at=unsigned(position);
    const double t=position-at, t2=t*t, t3=t2*t;
    const auto& table=langevinTable();
    const auto p=table[at], q=table[at+1];
    const double y=(2*t3-3*t2+1)*p.value+(t3-2*t2+t)*step*p.slope+
        (-2*t3+3*t2)*q.value+(t3-t2)*step*q.slope;
    const double d=((6*t2-6*t)*p.value+(-6*t2+6*t)*q.value)/step+
        (3*t2-4*t+1)*p.slope+(3*t2-2*t)*q.slope;
    return {std::copysign(y,x), std::max(0.0,d)};
}
double tolerance(std::uint64_t& seed) {
    seed+=0x9e3779b97f4a7c15ULL;
    auto z=seed;
    z=(z^(z>>30))*0xbf58476d1ce4e5b9ULL;
    z=(z^(z>>27))*0x94d049bb133111ebULL;
    z^=z>>31;
    return 1+.01*(2*double(z>>11)/9007199254740992.0-1);
}
double plateVoltage(double grid, double resistance) {
    // Koren's published 12AX7 model; all grid voltages here are negative.
    // https://www.normankoren.com/Audio/Tubemodspice_article.html
    double lo=0, hi=300;
    for (unsigned i=0; i<40; ++i) {
        const double plate=(lo+hi)*.5;
        const double z=600*(.01+grid/std::sqrt(300+plate*plate));
        const double softplus=z>40 ? z : std::log1p(std::exp(z));
        const double e=plate/600*softplus;
        const double current=2*std::pow(std::max(0.0,e),1.4)/1060;
        if (plate+resistance*current>300) hi=plate; else lo=plate;
    }
    return (lo+hi)*.5;
}
}

std::span<const ParameterInfo> parameterTable() noexcept {
    static const std::array<ParameterInfo,2> table{{
        {0,"drive","Drive","",-100,100,0},
        {1,"tone","Tone","",-100,100,0},
    }};
    return table;
}
ChannelColorInstance::ChannelColorInstance() : m_taps(engine::dsp::halfBand65Taps()) {
    (void)langevinTable();
    for (const auto& p:parameterTable()) m_values[p.index].store(p.defaultValue);
    for (unsigned i=0; i<m_gainTable.size(); ++i)
        m_gainTable[i]=std::pow(10.0, .6*i/(m_gainTable.size()-1));
    setProfileSeed(0);
}
const PluginDescriptor& ChannelColorInstance::staticDescriptor() noexcept {
    static const auto descriptor=[] {
        PluginDescriptor d;
        d.format=Format::Internal; d.uid=d.path="daw.channel-color";
        d.name="COLOR"; d.vendor="VLTONE"; d.version="1.0";
        d.category="Effect|Distortion"; d.stateSchemaVersion=1;
        return d;
    }();
    return descriptor;
}
bool ChannelColorInstance::setBusLayout(const PluginBusLayout& wanted, PluginBusLayout& accepted) {
    if (wanted.inputs.size()>1 || wanted.outputs.size()>1) return false;
    const auto width=wanted.inputs.empty()?std::uint16_t(2):wanted.inputs[0];
    if ((width!=1 && width!=2) || (!wanted.outputs.empty() && wanted.outputs[0]!=width)) return false;
    m_layout={{width},{width}}; accepted=m_layout; return true;
}
void ChannelColorInstance::makeTriodeTable(double resistance) {
    const double zero=plateVoltage(-1.5,resistance);
    const double h=1e-4;
    const double slope=(plateVoltage(-1.5-.75*h,resistance)-plateVoltage(-1.5+.75*h,resistance))/(2*h);
    for (unsigned i=0; i<m_triode.size(); ++i) {
        const double m=2.0*i/(m_triode.size()-1)-1;
        m_triode[i]=(zero-plateVoltage(-1.5+.75*m,resistance))/slope;
    }
    m_triode[512]=0;
    for (unsigned i=0; i<m_triode.size(); ++i) {
        const unsigned l=i?i-1:i, r=std::min(i+1,unsigned(m_triode.size()-1));
        m_triodeSlope[i]=(m_triode[r]-m_triode[l])/(r-l);
    }
}
void ChannelColorInstance::setProfileSeed(std::uint64_t seed) {
    if (m_profileReady && seed==m_seed) return;
    m_seed=seed; auto sequence=seed;
    const auto factor=[&] { return seed?tolerance(sequence):1.0; };
    m_a=.063*factor(); m_k=.08*factor(); m_c=.9*factor(); m_alpha=.0016*factor();
    m_linear=m_c/(3*m_a-m_c*m_alpha);
    makeTriodeTable(100000*factor());
    m_profileReady=true;
}
bool ChannelColorInstance::activate(const PluginProcessInfo& info) {
    if (!std::isfinite(info.sampleRate) || info.sampleRate<8000 || info.sampleRate>384000 || !info.maxBlockSize) return false;
    m_rate=info.sampleRate; const double rate=4*m_rate;
    m_smoothing=std::exp(-1/(.010*rate)); m_dcPole=std::exp(-2*std::numbers::pi*5/rate);
    const double w=2*rate*std::tan(std::numbers::pi*3000/rate), bilinear=2*rate;
    const auto shelf=[&](double gain) {
        const double pole=w*std::sqrt(gain), zero=w/std::sqrt(gain);
        return Coefficients{gain*(bilinear+zero)/(bilinear+pole),
            gain*(zero-bilinear)/(bilinear+pole),0,(pole-bilinear)/(bilinear+pole),0};
    };
    for (unsigned i=0; i<m_pre.size(); ++i) {
        const double tone=2.0*i/(m_pre.size()-1)-1;
        const double gain=std::pow(10.0,-.15*tone);
        m_pre[i]=shelf(gain); m_post[i]=shelf(1/gain);
    }
    {
        const double w0=2*std::numbers::pi*70/rate, A=std::pow(10.0,.6/40);
        const double alpha=std::sin(w0)/(2*.7), den=1+alpha/A;
        m_bump={(1+alpha*A)/den,-2*std::cos(w0)/den,(1-alpha*A)/den,
            -2*std::cos(w0)/den,(1-alpha/A)/den};
    }
    {
        const double w0=2*std::numbers::pi*std::min(18000.0,rate*.4)/rate;
        const double cosine=std::cos(w0), alpha=std::sin(w0)/std::numbers::sqrt2, den=1+alpha;
        m_loss={(1-cosine)/(2*den),(1-cosine)/den,(1-cosine)/(2*den),
            -2*cosine/den,(1-alpha)/den};
    }
    m_active=true; m_processing=false; reset(); return true;
}
std::int32_t ChannelColorInstance::parameterIndexForId(std::string_view id) const noexcept {
    for (const auto& p:parameterTable()) if (p.id==id) return std::int32_t(p.index);
    return -1;
}
double ChannelColorInstance::parameterValue(std::uint32_t index) const noexcept {
    return index<2?m_values[index].load(std::memory_order_relaxed):0;
}
void ChannelColorInstance::setParameterFromHost(std::uint32_t index,double value) {
    if (index<2) m_values[index].store(std::isfinite(value)?std::clamp(value,-100.0,100.0):parameterTable()[index].defaultValue,std::memory_order_relaxed);
}
std::string ChannelColorInstance::parameterText(std::uint32_t index,double value) const {
    if (index>=2) return {};
    if (!std::isfinite(value)) value=parameterTable()[index].defaultValue;
    char buffer[48];
    if (index==0) std::snprintf(buffer,sizeof(buffer),"%s %.1f",value<0?"Tape":value>0?"Tube":"Neutral",std::abs(value));
    else std::snprintf(buffer,sizeof(buffer),"%+.1f",value);
    return buffer;
}
bool ChannelColorInstance::saveState(std::vector<std::uint8_t>& out) const {
    const auto text=nlohmann::json{{"version",1},{"profileSeed",m_seed},
        {"params",{{"drive",parameterValue(0)},{"tone",parameterValue(1)}}}}.dump();
    out.assign(text.begin(),text.end()); return true;
}
bool ChannelColorInstance::loadState(std::span<const std::uint8_t> state) {
    if (state.empty() || state.size()>65536) return false;
    const auto doc=nlohmann::json::parse(state.begin(),state.end(),nullptr,false);
    if (!doc.is_object() || !doc.contains("version") || !doc["version"].is_number_integer() ||
        doc["version"]!=1 || !doc.contains("params") || !doc["params"].is_object()) return false;
    std::array<double,2> values{-20,0};
    for (const auto& p:parameterTable()) if (const auto it=doc["params"].find(p.id);it!=doc["params"].end()) {
        if (!it->is_number() || !std::isfinite(it->get<double>())) return false;
        values[p.index]=std::clamp(it->get<double>(),-100.0,100.0);
    }
    std::uint64_t seed=0;
    if (const auto it=doc.find("profileSeed");it!=doc.end()) {
        if (!it->is_number_unsigned() && (!it->is_number_integer() || it->get<std::int64_t>()<0)) return false;
        seed=it->get<std::uint64_t>();
    }
    setProfileSeed(seed);
    for (unsigned i=0;i<2;++i) setParameterFromHost(i,values[i]);
    return true;
}
void ChannelColorInstance::reset() noexcept {
    m_channels={}; m_dryCursor=0;
    m_drive=m_driveTarget=parameterValue(0); m_tone=m_toneTarget=parameterValue(1);
}
double ChannelColorInstance::Filter::tick(double x,const Coefficients& c) noexcept {
    const double y=c.b0*x+z1;
    z1=clean(c.b1*x-c.a1*y+z2); z2=clean(c.b2*x-c.a2*y);
    return clean(y);
}
ChannelColorInstance::Coefficients ChannelColorInstance::interpolate(const std::array<Coefficients,129>& table,double tone) noexcept {
    const double position=std::clamp((tone+100)*.64,0.0,128.0);
    const unsigned at=std::min(unsigned(position),127u); const double t=position-at;
    const auto& a=table[at];const auto& b=table[at+1];
    return {std::lerp(a.b0,b.b0,t),std::lerp(a.b1,b.b1,t),0,std::lerp(a.a1,b.a1,t),0};
}
double ChannelColorInstance::magneticSlope(double h,double m,double delta) const noexcept {
    const auto l=langevin((h+m_alpha*m)/m_a);
    const double diff=l.value-m;
    const double irreversible=diff*delta>0?(1-m_c)*diff/((1-m_c)*delta*m_k-m_alpha*diff):0;
    return (irreversible+m_c*l.slope/m_a)/(1-m_c*m_alpha*l.slope/m_a);
}
double ChannelColorInstance::magnetize(Channel& s,double field) const noexcept {
    // Jiles-Atherton, integrated in H rather than through a Nyquist-sensitive
    // recursive time differentiator. Bounded field => at most 32 RK4 steps.
    // https://www.dafx.de/paper-archive/2019/DAFx2019_paper_3.pdf
    const double change=field-s.field, delta=change<0?-1.0:1.0;
    const unsigned count=std::clamp(unsigned(std::ceil(std::abs(change)/(.5*m_a))),1u,32u);
    const double step=change/count; double at=s.field, m=s.magnetization;
    for (unsigned i=0;i<count;++i) {
        const double k1=step*magneticSlope(at,m,delta);
        const double k2=step*magneticSlope(at+step*.5,m+k1*.5,delta);
        const double k3=step*magneticSlope(at+step*.5,m+k2*.5,delta);
        const double k4=step*magneticSlope(at+step,m+k3,delta);
        m+=(k1+2*k2+2*k3+k4)/6; at+=step;
    }
    s.field=field; s.magnetization=clean(std::clamp(m,-1.0,1.0));
    return s.magnetization;
}
double ChannelColorInstance::triode(double m) const noexcept {
    const double pos=std::clamp((m+1)*512,0.0,1024.0);
    const unsigned at=std::min(unsigned(pos),1023u); const double t=pos-at, t2=t*t,t3=t2*t;
    return (2*t3-3*t2+1)*m_triode[at]+(t3-2*t2+t)*m_triodeSlope[at]+
        (-2*t3+3*t2)*m_triode[at+1]+(t3-t2)*m_triodeSlope[at+1];
}
void ChannelColorInstance::render(const PluginProcessContext& c,unsigned offset,unsigned frames) noexcept {
    m_driveTarget=parameterValue(0); m_toneTarget=parameterValue(1);
    const unsigned channels=std::min(unsigned(m_layout.outputs[0]),2u);
    for (unsigned frame=offset;frame<offset+frames;++frame) {
        std::array<double,2> input{},dry{},at2{},at4{},output{};
        for (unsigned ch=0;ch<channels;++ch) {
            const double x=c.inputs && ch<c.inputChannels && c.inputs[ch]?c.inputs[ch][frame]:0;
            input[ch]=std::isfinite(x)?x:0;
            auto& s=m_channels[ch]; dry[ch]=s.dry[m_dryCursor]; s.dry[m_dryCursor]=input[ch];
        }
        for (unsigned phase2=0;phase2<2;++phase2) {
            for (unsigned ch=0;ch<channels;++ch) at2[ch]=m_channels[ch].up2.tick(phase2?0:2*input[ch],m_taps);
            std::array<double,2> down4{};
            for (unsigned phase4=0;phase4<2;++phase4) {
                m_drive=m_driveTarget+m_smoothing*(m_drive-m_driveTarget);
                m_tone=m_toneTarget+m_smoothing*(m_tone-m_toneTarget);
                if (std::abs(m_drive-m_driveTarget)<1e-12) m_drive=m_driveTarget;
                if (std::abs(m_tone-m_toneTarget)<1e-12) m_tone=m_toneTarget;
                const double u=softDrive(m_drive), tube=m_drive>0?u:0;
                const double pos=u*256; const unsigned idx=std::min(unsigned(pos),255u);
                const double gain=std::lerp(m_gainTable[idx],m_gainTable[idx+1],pos-idx);
                const auto pre=interpolate(m_pre,m_tone),post=interpolate(m_post,m_tone);
                for (unsigned ch=0;ch<channels;++ch) {
                    auto& s=m_channels[ch];
                    at4[ch]=s.up4.tick(phase4?0:2*at2[ch],m_taps);
                    const double x=s.pre.tick(at4[ch],pre);
                    const double m=magnetize(s,.5*std::tanh(.4*gain*x));
                    double wet=std::lerp(m,triode(m),tube)/(m_linear*.2*gain);
                    wet=s.loss.tick(s.bump.tick(s.post.tick(wet,post),m_bump),m_loss);
                    const double dc=clean(wet-s.dcInput+m_dcPole*s.dcOutput);
                    s.dcInput=wet; s.dcOutput=dc;
                    const double down=s.down4.tick(dc,m_taps);
                    if (!phase4) down4[ch]=down;
                }
            }
            for (unsigned ch=0;ch<channels;++ch) {
                const double down=m_channels[ch].down2.tick(down4[ch],m_taps);
                if (!phase2) output[ch]=down;
            }
        }
        const double amount=softDrive(m_drive);
        for (unsigned ch=0;ch<c.outputChannels;++ch) if (c.outputs && c.outputs[ch])
            c.outputs[ch][frame]=ch<channels?float(std::lerp(dry[ch],output[ch],amount)):0;
        m_dryCursor=(m_dryCursor+1)%kLatency;
    }
}
PluginProcessDisposition ChannelColorInstance::process(const PluginProcessContext& c) noexcept {
    if (!m_active) {
        if (c.outputs) for (unsigned ch=0;ch<c.outputChannels;++ch) if(c.outputs[ch]) std::fill_n(c.outputs[ch],c.frames,0.f);
        return PluginProcessDisposition::Error;
    }
    unsigned position=0;
    for (const auto& event:c.inputEvents) {
        if (event.kind!=PluginEvent::Kind::ParamValue || event.paramIndex>=2) continue;
        const unsigned at=std::clamp(event.frameOffset,position,c.frames);
        render(c,position,at-position); setParameterFromHost(event.paramIndex,event.value); position=at;
    }
    render(c,position,c.frames-position);
    return PluginProcessDisposition::Continue;
}
} // namespace daw::plugins::channel_color
