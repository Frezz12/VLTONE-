#include "Internal/WavetableDsp.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <mutex>

namespace daw::plugins::wavetable {
namespace {
constexpr double pi=std::numbers::pi, tau=2*pi;
double frac(double x) noexcept { return x-std::floor(x); }
double db(double x) noexcept {return x<=-96?0:std::pow(10.,x/20.);}
double clean(double x) noexcept {return std::isfinite(x)?std::clamp(x,-32.,32.):0;}
std::shared_ptr<const Table> makeTable(std::span<const float> source, unsigned channels, unsigned length, bool spectral=false) {
    auto out=std::make_shared<Table>();
    const std::size_t samples=source.size()/channels;
    out->frames=unsigned(std::clamp<std::size_t>(samples/length,1,maxFrames));
    for(auto& level:out->levels)level.resize(std::size_t(out->frames)*(frameSize+1));
    std::array<std::complex<double>,frameSize> spectrum, work;
    for(unsigned frame=0;frame<out->frames;++frame) {
        const std::size_t offset=spectral && samples>length ? std::size_t((samples-length)*double(frame)/std::max(1u,out->frames-1)) : std::size_t(frame)*length;
        for(unsigned i=0;i<frameSize;++i) {
            const double at=double(i)*length/frameSize;const auto index=std::min(samples-1,offset+std::size_t(at));
            double v=0;for(unsigned c=0;c<channels;++c)v+=source[index*channels+c]/channels;
            spectrum[i]=v*(spectral ? .5-.5*std::cos(tau*i/frameSize):1);
        }
        fft(spectrum,false); spectrum[0]=0;
        for(unsigned octave=0;octave<11;++octave) {
            work=spectrum;unsigned limit=std::max(1u,(frameSize/2)>>octave);
            for(unsigned k=limit+1;k<frameSize-limit;++k)work[k]=0;
            fft(work,true);auto* dst=out->levels[octave].data()+std::size_t(frame)*(frameSize+1);
            for(unsigned i=0;i<frameSize;++i)dst[i]=float(work[i].real());dst[frameSize]=dst[0];
        }
    }
    return out;
}
std::shared_ptr<const Table> builtin(unsigned bank) {
    static std::mutex mutex;
    static std::array<std::shared_ptr<const Table>,8> cache;
    const std::lock_guard lock(mutex);
    if(cache[bank])return cache[bank];
    std::vector<float> samples(frameSize*8);
    for(unsigned f=0;f<8;++f)for(unsigned i=0;i<frameSize;++i){
        const double t=double(i)/frameSize,p=double(f)/7;double x=0;
        if(bank==0) { // continuous sine -> triangle -> saw -> pulse family
            const double sine=std::sin(tau*t), triangle=2/pi*std::asin(sine), saw=2*t-1, pulse=t<(.5-.4*p)?1:-1;
            const double at=p*3;const double waves[]{sine,triangle,saw,pulse};unsigned a=std::min(2u,unsigned(at));x=std::lerp(waves[a],waves[a+1],at-a);
        } else if(bank==1)x=t<.05+.9*p?1:-1;
        else for(unsigned h=1;h<=32;++h){
            double amp=0;
            if(bank==2)amp=std::pow(double(h),-(1.+p*3));
            if(bank==3)amp=std::exp(-std::pow((h-(2+p*14))/3,2))*.35;
            if(bank==4)amp=(h%3==1?1.:.12)*std::exp(-h*(.12+.2*p));
            if(bank==5)amp=std::cos(h*p*pi)*std::exp(-h*.13)/3;
            if(bank==6)amp=(h%2?1.:p)*std::exp(-h*.06)/(h*.7);
            if(bank==7)amp=(h==1?1.:h==2?.8*(1-p):h==3?.7*p:h==4?.3:h==8?.2*p:0);
            x+=amp*std::sin(tau*h*t);
        }
        samples[f*frameSize+i]=float(x);
    }
    double peak=0;for(float s:samples)peak=std::max(peak,std::abs(double(s)));for(auto& s:samples)s/=float(std::max(1.,peak));
    return cache[bank]=makeTable(samples,1,frameSize);
}
}
void fft(std::span<std::complex<double>> d,bool inverse) {
    const auto n=d.size();
    for(std::size_t i=1,j=0;i<n;++i){std::size_t bit=n>>1;for(;j&bit;bit>>=1)j^=bit;j^=bit;if(i<j)std::swap(d[i],d[j]);}
    for(std::size_t len=2;len<=n;len<<=1){const auto wlen=std::polar(1.,(inverse?tau:-tau)/len);for(std::size_t i=0;i<n;i+=len){std::complex<double>w=1;for(std::size_t j=0;j<len/2;++j){auto u=d[i+j],v=d[i+j+len/2]*w;d[i+j]=u+v;d[i+j+len/2]=u-v;w*=wlen;}}}
    if(inverse)for(auto& x:d)x/=double(n);
}
float Table::sample(double phase,double position,double increment) const noexcept {
    const double octave=std::clamp(std::log2(std::max(1.,increment*frameSize)),0.,10.);
    const unsigned lo=unsigned(octave), hi=std::min(10u,lo+1);
    const double at=std::clamp(position,0.,1.)*(frames-1), t=frac(phase)*frameSize;
    const unsigned f=unsigned(at), next=std::min(frames-1,f+1), i=std::min(frameSize-1,unsigned(t));
    auto read=[&](unsigned level){auto frame=[&](unsigned index){auto* p=levels[level].data()+std::size_t(index)*(frameSize+1);return std::lerp(double(p[i]),double(p[i+1]),t-i);};return std::lerp(frame(f),frame(next),at-f);};
    return float(std::lerp(read(lo),read(hi),octave-lo));
}
std::shared_ptr<PreparedPatch> preparePatch(const State& s,double rate,std::uint64_t revision) {
    if(!validState(s))return {};
    auto p=std::make_shared<PreparedPatch>();p->state=s;p->revision=revision;p->resources.fill(-1);
    for(unsigned i=0;i<oscillatorCount;++i) {
        auto& o=s.oscillators[i];const Resource* source=nullptr;
        for(unsigned r=0;r<s.resources.size();++r)if(s.resources[r].id==o.resource){source=&s.resources[r];p->resources[i]=int(r);break;}
        auto mode=OscMode(unsigned(s.values[osc(i,Mode)]));
        p->tables[i]=source&&(mode==OscMode::Wavetable||mode==OscMode::Spectral)?makeTable(source->samples,source->channels,source->frameLength,mode==OscMode::Spectral):builtin(o.bank);
    }
    for(auto& r:s.routes)if(r.enabled){p->routes.push_back({unsigned(r.source),unsigned(parameterIndex(r.target)),r.amount,r.bipolar,r.response});if(r.source>=Source::Lfo1&&r.source<=Source::Lfo10)p->usedLfos[unsigned(r.source)-unsigned(Source::Lfo1)]=true;}
    for(unsigned i=0;i<fxCount;++i)if(s.effects[i].kind) {
        p->effects[i]=std::make_unique<flux::dsp::Processor>(flux::Kind(s.effects[i].kind),rate,false,s.seed+i);
        p->effectLatency[i]=p->effects[i]->latency();p->busLatency[s.effects[i].bus]+=p->effectLatency[i];
    }
    p->latency=*std::max_element(p->busLatency.begin(),p->busLatency.end());
    for(unsigned b=0;b<3;++b)p->busAlign[b].prepare(p->latency-p->busLatency[b]+8);
    return p;
}
namespace {
struct Envelope {
    unsigned stage=0;double value=0,from=0,time=0;
    void trigger(bool legato){if(legato&&stage&&stage!=5)return;stage=1;time=0;from=value;}
    void release(){if(stage&&stage!=5){stage=5;time=0;from=value;}}
    double tick(double step,double attack,double hold,double decay,double sustain,double release,double curve) noexcept {
        time+=step;double t=0;
        if(stage==1){t=std::min(1.,time/attack);value=std::lerp(from,1.,t+curve*t*(1-t));if(t>=1){stage=2;time=0;}}
        else if(stage==2){value=1;if(time>=hold){stage=3;time=0;}}
        else if(stage==3){t=std::min(1.,time/decay);value=std::lerp(1.,sustain,t+curve*t*(1-t));if(t>=1){stage=4;time=0;}}
        else if(stage==4)value=sustain;
        else if(stage==5){t=std::min(1.,time/release);value=from*(1-t)*(1-t);if(t>=1){stage=0;value=0;}}
        return value;
    }
};
struct Filter {
    double a=0,b=0,c=0,d=0;
    std::array<float,4096> history{};unsigned cursor=0;
    double tick(double input,unsigned type,double cutoff,double resonance,double drive,double rate) noexcept {
        double x=std::tanh(input*db(drive));
        if(type==6){const unsigned delay=std::clamp(unsigned(rate/cutoff),1u,4095u);double echo=history[(cursor+4096-delay)%4096];x=std::tanh(x+echo*resonance*.98);history[cursor]=float(x);cursor=(cursor+1)%4096;return x;}
        const double g=std::tan(pi*std::min(cutoff,rate*.45)/rate),k=2-1.95*resonance;
        const double v1=(g*(x-b)+a)/(1+g*(g+k)),v2=b+g*v1;a=2*v1-a;b=2*v2-b;
        double out=type==1?x-k*v1-v2:type==2?v1:type==3?x-k*v1:type==7?v1*2:v2;
        if(type==4||type==5){double v=(g*(out-d)+c)/(1+g*(g+k)),w=d+g*v;c=2*v-c;d=2*w-d;out=type==5?std::tanh(w):w;}
        return clean(out);
    }
};
struct Grain { double position=0,increment=1;unsigned age=0,length=0;double pan=0; };
struct OscVoice { std::array<double,16> phase{};double cursor=0,spawn=0;std::array<Grain,256> grains{};unsigned usedGrains=0;int resource=-1;int root=60;double previous=0; };
struct Voice {
    bool active=false,held=false,sustained=false;int key=60,channel=0,id=-1;std::uint64_t stamp=0;
    double velocity=1,note=60,target=60,random=0,pan=0,age=0;std::uint32_t seed=1;
    std::array<Envelope,envelopeCount> envelopes;
    std::array<double,lfoCount> phases{};
    std::array<OscVoice,oscillatorCount> osc;
    std::array<std::array<Filter,2>,2> filters;
};
double sampleAt(const Resource& r,double frame,unsigned channel) noexcept {
    const auto n=r.samples.size()/r.channels;if(n<2||frame<0||frame>=n-1)return 0;
    auto i=std::size_t(frame);auto c=std::min(channel,r.channels-1);
    return std::lerp(double(r.samples[i*r.channels+c]),double(r.samples[(i+1)*r.channels+c]),frame-i);
}
double warpPhase(double phase,unsigned type,double amount,double mod) noexcept {
    switch(Warp(type)) {
        case Warp::Sync:return frac(phase*(1+amount*15));
        case Warp::Bend:return frac(phase+amount*.45*std::sin(tau*phase));
        case Warp::Pulse:return phase<std::max(.02,1-amount*.95)?phase/std::max(.02,1-amount*.95):0;
        case Warp::Mirror:return frac(std::abs(phase-.5)*2*(1+amount*3));
        case Warp::FM:return frac(phase+mod*amount*2);
        case Warp::Quantize:{double steps=std::max(2.,128*(1-amount));return std::floor(phase*steps)/steps;}
        default:return phase;
    }
}
}
struct Renderer::Impl {
    double rate=48000;std::array<Voice,maxVoices> voices;
    std::array<double,parameterCount> smooth{};
    std::array<double,16> bend{},wheel{},pressure{};std::array<bool,16>sustain{};
    std::array<std::array<float,128>,16> held{};
    std::uint64_t clock=0;std::int64_t arpStep=-1;int arpKey=-1,arpChannel=0;
    std::array<bool,4096> clipHeld{};double lastBeat=-1;
    bool initialized=false;double transition=0;std::array<double,2> last{},anchor{};
    void noteOff(int key,int channel,int id) noexcept {
        for(auto& v:voices)if(v.active&&v.channel==channel&&(id>=0?v.id==id:v.key==key)) {
            v.held=false;v.sustained=sustain[channel];if(!v.sustained)for(auto& e:v.envelopes)e.release();
        }
    }
    void noteOn(int key,int channel,int id,double velocity,double pan,const PreparedPatch& p,const std::array<double,parameterCount>& values) noexcept {
        if(key<0||key>127||channel<0||channel>15)return;
        const unsigned limit=unsigned(values[Polyphony]);Voice* selected=nullptr;
        if(values[Mono]>.5)selected=&voices[0];
        else {for(unsigned i=0;i<limit;++i)if(!voices[i].active){selected=&voices[i];break;}if(!selected)selected=&*std::min_element(voices.begin(),voices.begin()+limit,[](auto& a,auto& b){return a.envelopes[0].value<b.envelopes[0].value||(a.envelopes[0].value==b.envelopes[0].value&&a.stamp<b.stamp);});}
        bool legato=selected->active&&values[Mono]>.5&&values[Legato]>.5;
        double previous=selected->note;auto& v=*selected;
        if(!legato) {v=Voice{};v.seed=hash(p.state.seed+unsigned(++clock));v.random=unit(v.seed);}
        v.active=v.held=true;v.sustained=false;v.key=key;v.channel=channel;v.id=id;v.velocity=velocity;v.pan=pan;v.target=key;v.note=values[Glide]>0&&legato?previous:key;v.stamp=++clock;
        for(auto& e:v.envelopes)e.trigger(legato);
        for(unsigned i=0;i<lfoCount;++i)if(!legato&&values[lfo(i,LfoRetrigger)]>.5)v.phases[i]=values[lfo(i,LfoPhase)];
        if(!legato)for(unsigned o=0;o<oscillatorCount;++o){auto& ov=v.osc[o];for(auto& phase:ov.phase)phase=frac(values[osc(o,Phase)]+unit(v.seed)*values[osc(o,RandomPhase)]);ov.resource=p.resources[o];ov.root=60;
            for(auto& r:p.state.oscillators[o].regions)if(key>=r.lowKey&&key<=r.highKey&&velocity*127>=r.lowVelocity&&velocity*127<=r.highVelocity){for(unsigned j=0;j<p.state.resources.size();++j)if(p.state.resources[j].id==r.resource){ov.resource=int(j);ov.root=r.rootKey;break;}break;}
            if(ov.resource>=0)ov.cursor=values[osc(o,Start)]*(p.state.resources[ov.resource].samples.size()/p.state.resources[ov.resource].channels-1);
        }
    }
};
Renderer::Renderer():m(std::make_unique<Impl>()){}
Renderer::~Renderer()=default;
void Renderer::prepare(double rate){m->rate=rate;reset();}
void Renderer::reset() noexcept {m->anchor=m->last;m->transition=1;for(auto& v:m->voices)v.active=false;m->bend.fill(0);m->pressure.fill(0);m->wheel.fill(0);m->sustain.fill(false);for(auto& h:m->held)h.fill(0);m->clipHeld.fill(false);m->clock=0;m->arpStep=-1;m->arpKey=-1;m->lastBeat=-1;m->initialized=false;}
unsigned Renderer::activeVoices() const noexcept {unsigned n=0;for(auto& v:m->voices)n+=v.active;return n;}
void Renderer::event(const PluginEvent& e,const PreparedPatch& p,std::array<double,parameterCount>& values) noexcept {
    const int ch=std::clamp<int>(e.channel,0,15),key=std::clamp<int>(e.key,0,127);
    if(e.kind==PluginEvent::Kind::ParamValue&&e.paramIndex<parameterCount&&std::isfinite(e.value)) {auto& def=parameterTable()[e.paramIndex];values[e.paramIndex]=std::clamp(def.isStepped?std::round(e.value):e.value,def.minValue,def.maxValue);return;}
    if(e.kind==PluginEvent::Kind::NoteOn) {if(e.value<=0){m->held[ch][key]=0;m->noteOff(key,ch,e.noteId);return;}m->held[ch][key]=float(e.value);if(values[ArpEnabled]<.5)m->noteOn(key,ch,e.noteId,e.value,e.notePan,p,values);}
    if(e.kind==PluginEvent::Kind::NoteOff||e.kind==PluginEvent::Kind::NoteChoke){m->held[ch][key]=0;m->noteOff(key,ch,e.noteId);if(e.kind==PluginEvent::Kind::NoteChoke)for(auto& v:m->voices)if(v.active&&v.channel==ch&&(e.noteId>=0?v.id==e.noteId:v.key==key))v.active=false;}
    if(e.kind==PluginEvent::Kind::MidiController){if(e.paramIndex==1)m->wheel[ch]=e.value;if(e.paramIndex==128)m->pressure[ch]=e.value;if(e.paramIndex==129)m->bend[ch]=(e.value*2-1)*values[BendRange];if(e.paramIndex==64){m->sustain[ch]=e.value>=.5;if(!m->sustain[ch])for(auto& v:m->voices)if(v.channel==ch&&!v.held&&v.sustained){v.sustained=false;for(auto& env:v.envelopes)env.release();}}if(e.paramIndex==120||e.paramIndex==123){m->held[ch].fill(0);for(auto& v:m->voices)if(v.channel==ch){v.held=v.sustained=false;if(e.paramIndex==120)v.active=false;else for(auto& env:v.envelopes)env.release();}}}
    if(e.kind==PluginEvent::Kind::PolyPressure)for(auto& v:m->voices)if(v.active&&v.channel==ch&&v.key==key)m->pressure[ch]=e.value;
    if(e.kind==PluginEvent::Kind::NotePitch)for(auto& v:m->voices)if(v.active&&v.channel==ch&&(e.noteId>=0?v.id==e.noteId:v.key==key))v.target=v.key+e.value;
}
void Renderer::render(PreparedPatch& p,const PluginProcessContext& context,std::array<double,parameterCount>& values,unsigned start,unsigned end) noexcept {
    auto& d=*m;const double step=1/d.rate,tempo=std::max(1.,context.transport.tempo),beatStep=tempo/(60*d.rate);
    if(!d.initialized){d.smooth=values;d.initialized=true;}
    const double slew=1-std::exp(-1/(.003*d.rate));
    std::array<double,parameterCount> deltas{};
    for(unsigned frame=start;frame<end;++frame) {
        for(auto& def:parameterTable()){auto i=def.index;d.smooth[i]=def.isStepped?values[i]:d.smooth[i]+(values[i]-d.smooth[i])*slew;}
        auto& base=d.smooth;double beat=context.transport.ppqPosition+frame*beatStep;
        if(base[ArpEnabled]>.5) {
            const auto stepIndex=std::int64_t(std::floor(beat/base[ArpRate]));
            if(stepIndex!=d.arpStep){if(d.arpKey>=0)d.noteOff(d.arpKey,d.arpChannel,-100);d.arpKey=-1;std::array<std::array<int,3>,512> notes{};unsigned count=0;
                for(int k=0;k<128;++k)for(int c=0;c<16;++c)if(d.held[c][k]>0&&count<512)notes[count++]={k,c,int(d.held[c][k]*127)};
                if(count){unsigned total=count*unsigned(base[ArpOctaves]),at=unsigned(std::abs(stepIndex))%total;const unsigned direction=unsigned(base[ArpDirection]);if(direction==1)at=total-1-at;else if(direction==2&&total>1){at=unsigned(std::abs(stepIndex))%(total*2-2);if(at>=total)at=total*2-2-at;}else if(direction==3)at=hash(p.state.seed+unsigned(stepIndex))%total;
                    auto n=notes[at%count];int key=n[0]+12*int(at/count);if(key<=127){d.arpKey=key;d.arpChannel=n[1];d.noteOn(key,n[1],-100,n[2]/127.,0,p,values);}}
                d.arpStep=stepIndex;
            }
            if(d.arpKey>=0&&frac(beat/base[ArpRate])>=base[ArpGate]){d.noteOff(d.arpKey,d.arpChannel,-100);d.arpKey=-1;}
        } else if(d.arpKey>=0){d.noteOff(d.arpKey,d.arpChannel,-100);d.arpKey=-1;d.arpStep=-1;}
        if(base[ClipEnabled]>.5&&context.playing){auto& clip=p.state.clips[unsigned(base[ClipSelect])];double pos=frac(beat/clip.length)*clip.length;for(unsigned i=0;i<clip.notes.size();++i){auto n=clip.notes[i];bool on=pos>=n.beat&&pos<std::min(clip.length,n.beat+n.length);if(on&&!d.clipHeld[i])d.noteOn(n.key,0,10000+int(i),n.velocity,0,p,values);if(!on&&d.clipHeld[i])d.noteOff(n.key,0,10000+int(i));d.clipHeld[i]=on;}}else {for(unsigned i=0;i<d.clipHeld.size();++i)if(d.clipHeld[i]){for(auto& v:d.voices)if(v.id==10000+int(i))for(auto& e:v.envelopes)e.release();d.clipHeld[i]=false;}}
        flux::dsp::Stereo sum{};
        for(auto& v:d.voices)if(v.active) {
            std::array<double,unsigned(Source::Count)> sources{};
            for(unsigned e=0;e<envelopeCount;++e)sources[e]=v.envelopes[e].tick(step,base[env(e,Attack)],base[env(e,Hold)],base[env(e,Decay)],base[env(e,Sustain)],base[env(e,Release)],base[env(e,EnvCurve)]);
            if(v.envelopes[0].stage==0){v.active=false;continue;}
            for(unsigned l=0;l<lfoCount;++l)if(p.usedLfos[l]){double speed=base[lfo(l,LfoSync)]>.5?tempo/(60*base[lfo(l,LfoRate)]):base[lfo(l,LfoRate)];double phase=base[lfo(l,LfoRetrigger)]>.5?v.phases[l]:beat*60/tempo*speed+base[lfo(l,LfoPhase)];double value=curveValue(p.state.lfos[l],frac(phase));sources[unsigned(Source::Lfo1)+l]=base[lfo(l,LfoBipolar)]>.5?value*2-1:value;v.phases[l]=frac(v.phases[l]+speed*step);}
            for(unsigned i=0;i<macroCount;++i)sources[unsigned(Source::Macro1)+i]=base[Macro1+i];
            sources[unsigned(Source::Velocity)]=v.velocity;sources[unsigned(Source::Note)]=v.key/127.;sources[unsigned(Source::ModWheel)]=d.wheel[v.channel];sources[unsigned(Source::Pressure)]=d.pressure[v.channel];sources[unsigned(Source::Random)]=v.random;sources[unsigned(Source::Pitch)]=d.bend[v.channel]/std::max(1.,base[BendRange]);
            for(auto& r:p.routes)deltas[r.target]=0;
            for(auto& r:p.routes){double x=sources[r.source];bool signedSource=(r.source>=unsigned(Source::Lfo1)&&r.source<=unsigned(Source::Lfo10)&&base[lfo(r.source-unsigned(Source::Lfo1),LfoBipolar)]>.5)||r.source==unsigned(Source::Pitch);double shaped=curveValue(r.response,signedSource?(x+1)*.5:x);if(r.bipolar||signedSource)shaped=shaped*2-1;deltas[r.target]+=shaped*r.amount;}
            auto val=[&](unsigned id){auto& def=parameterTable()[id];double out=def.isLogarithmic?base[id]*std::pow(def.maxValue/def.minValue,deltas[id]):base[id]+deltas[id]*(def.maxValue-def.minValue);out=std::clamp(out,def.minValue,def.maxValue);return def.isStepped?std::round(out):out;};
            v.note=base[Glide]>0?v.note+(v.target-v.note)*std::min(1.,step/base[Glide]):v.target;
            flux::dsp::Stereo wet{},dry{};
            for(unsigned o=0;o<oscillatorCount;++o)if(val(osc(o,On))>.5) {
                auto& ov=v.osc[o];const auto mode=OscMode(unsigned(val(osc(o,Mode))));const Resource* resource=ov.resource>=0?&p.state.resources[ov.resource]:nullptr;
                double note=v.note+d.bend[v.channel]+(v.channel?d.bend[0]:0)+12*val(osc(o,Octave))+val(osc(o,Semitone))+val(osc(o,Fine))*.01;
                double frequency=440*std::exp2((note-69)/12),gain=db(val(osc(o,Level))),pan=std::clamp(val(osc(o,Pan))+v.pan,-1.,1.);
                flux::dsp::Stereo output{};
                unsigned unison=unsigned(val(osc(o,Unison)));
                if(mode==OscMode::Wavetable||mode==OscMode::Spectral||o==4) {
                    double position=val(osc(o,Position));if(mode==OscMode::Spectral&&resource){double duration=double(resource->samples.size()/resource->channels)/resource->sampleRate;position=frac(position+v.age*val(osc(o,Rate))/std::max(.001,duration));frequency=double(resource->sampleRate)/frameSize*std::exp2((note-60)/12);}
                    const double mod=v.osc[(o+1)%3].previous;double mono=0;
                    for(unsigned u=0;u<unison;++u){double spread=unison==1?0:2.*u/(unison-1)-1;double inc=std::min(.49,frequency*std::exp2(spread*val(osc(o,Detune))*.5)/d.rate);double phase=ov.phase[u];unsigned warp1=unsigned(val(osc(o,WarpMode))),warp2=unsigned(val(osc(o,Warp2Mode)));double a=val(osc(o,WarpAmount)),b=val(osc(o,Warp2Amount));phase=warpPhase(phase,warp1,a,mod);phase=warpPhase(phase,warp2,b,mod);double x=p.tables[o]->sample(phase,position,inc*(1+(warp1==unsigned(Warp::Sync)?a*15:0)));
                        if(warp1==unsigned(Warp::RM))x*=std::lerp(1.,mod,a);if(warp2==unsigned(Warp::RM))x*=std::lerp(1.,mod,b);if(warp1==unsigned(Warp::Fold))x=std::sin(x*(1+a*8));if(warp2==unsigned(Warp::Fold))x=std::sin(x*(1+b*8));
                        double upan=std::clamp(pan+spread*val(osc(o,Spread)),-1.,1.);output[0]+=x*std::sqrt((1-upan)*.5)/unison;output[1]+=x*std::sqrt((1+upan)*.5)/unison;mono+=x/unison;ov.phase[u]=frac(ov.phase[u]+inc);
                    }ov.previous=mono;
                } else if(mode==OscMode::Granular&&resource) {
                    const double ratio=std::exp2((note-ov.root)/12)*resource->sampleRate/d.rate;const double length=double(resource->samples.size()/resource->channels);ov.spawn-=step;
                    if(ov.spawn<=0){ov.spawn+=1/val(osc(o,GrainDensity));for(unsigned g=0;g<256;++g)if(ov.grains[g].age>=ov.grains[g].length){auto& grain=ov.grains[g];grain.position=std::clamp((val(osc(o,Position))+(unit(v.seed)*2-1)*val(osc(o,GrainSpray))*.5)*length,0.,std::max(0.,length-2));grain.increment=ratio;grain.age=0;grain.length=unsigned(val(osc(o,GrainSize))*.001*d.rate);grain.pan=pan;ov.usedGrains=std::max(ov.usedGrains,g+1);break;}}
                    for(unsigned g=0;g<ov.usedGrains;++g){auto& grain=ov.grains[g];if(grain.age<grain.length){double window=.5-.5*std::cos(tau*grain.age/grain.length);for(unsigned c=0;c<2;++c)output[c]+=sampleAt(*resource,grain.position,c)*window;grain.position+=grain.increment;++grain.age;}}
                    double norm=std::sqrt(std::max(1.,val(osc(o,GrainSize))*.001*val(osc(o,GrainDensity))*.5));for(auto& x:output)x/=norm;
                } else if(resource) {
                    const double length=double(resource->samples.size()/resource->channels),first=val(osc(o,Start))*(length-1),last=val(osc(o,End))*(length-1),span=std::max(1.,last-first);
                    if(ov.cursor>=last||ov.cursor<first){if(val(osc(o,Loop))>.5)ov.cursor=first+frac((ov.cursor-first)/span)*span;}
                    for(unsigned c=0;c<2;++c)output[c]=sampleAt(*resource,ov.cursor,c)*std::sqrt(c?(1+pan)*.5:(1-pan)*.5);
                    ov.cursor+=std::exp2((note-ov.root)/12)*resource->sampleRate/d.rate*val(osc(o,Rate));
                } else if(o==3) {double x=unit(v.seed)*2-1;output={x*.707,x*.707};}
                const double send=val(osc(o,FilterSend));for(unsigned c=0;c<2;++c){wet[c]+=output[c]*gain*send;dry[c]+=output[c]*gain*(1-send);}
            }
            auto applyFilter=[&](unsigned f,flux::dsp::Stereo input){if(val(filter(f,FilterOn))<.5)return input;for(unsigned c=0;c<2;++c){double x=v.filters[f][c].tick(input[c],unsigned(val(filter(f,FilterType))),val(filter(f,Cutoff)),val(filter(f,Resonance)),val(filter(f,Drive)),d.rate);input[c]=std::lerp(input[c],x,val(filter(f,FilterMix)));}return input;};
            if(base[FilterRouting]>.5){auto a=applyFilter(0,wet),b=applyFilter(1,wet);for(unsigned c=0;c<2;++c)wet[c]=(a[c]+b[c])*.5;}else wet=applyFilter(1,applyFilter(0,wet));
            double amp=sources[0]*std::lerp(1.,v.velocity,base[Velocity]);for(unsigned c=0;c<2;++c)sum[c]+=(dry[c]+wet[c])*amp;v.age+=step;
        }
        std::array<flux::dsp::Stereo,3>busses{sum,sum,sum};std::array<bool,3>used{};used[0]=true;
        flux::dsp::Clock clock;clock.rate=d.rate;clock.tempo=tempo;clock.beat=beat;clock.sample=context.sampleTime+frame;clock.seed=p.state.seed;
        for(unsigned i=0;i<fxCount;++i)if(p.effects[i]){unsigned b=p.state.effects[i].bus;used[b]=true;if(base[fx(i,0)]<.5)continue;flux::dsp::Values effectValues{};auto defs=flux::fields(flux::Kind(p.state.effects[i].kind));for(unsigned f=0;f<defs.size();++f)effectValues[f]=std::clamp(base[fx(i,2+f)],defs[f].min,defs[f].max);auto processed=p.effects[i]->process(busses[b],{},effectValues,clock);for(unsigned c=0;c<2;++c)busses[b][c]=std::lerp(busses[b][c],processed[c],base[fx(i,1)]);}
        sum={};unsigned buses=0;for(unsigned b=0;b<3;++b)if(used[b]){p.busAlign[b].push(busses[b]);auto x=p.latency>p.busLatency[b]?p.busAlign[b].ago(p.latency-p.busLatency[b]):busses[b];for(unsigned c=0;c<2;++c)sum[c]+=x[c];++buses;}
        const double gain=db(base[Output])/std::sqrt(double(buses));for(unsigned c=0;c<context.outputChannels;++c){double out=clean(sum[std::min(1u,c)]*gain);if(d.transition>0)out=std::lerp(out,d.anchor[std::min(1u,c)],d.transition);context.outputs[c][frame]=float(out);d.last[std::min(1u,c)]=out;}d.transition=std::max(0.,d.transition-step/.003);d.lastBeat=beat;
    }
}
}
