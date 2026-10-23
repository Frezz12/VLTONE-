#include "Internal/WavetableProgram.hpp"
#include "Internal/FluxProgram.hpp"
#include <cmath>
#include <numbers>

namespace daw::plugins::wavetable {
namespace {
constexpr unsigned counts[]{32,32,32,32,48,32,32,16};
constexpr const char* categories[]{"Bass","Lead","Keys","Pluck","Pad","Sequence","Texture","Percussion"};
constexpr const char* families[][8]{
    {"Subcurrent","Rubber","Copper","Acid","Reese","Circuit","Velvet","Pulse"},
    {"Prism","Beacon","Ribbon","Voltage","Neon","Whistle","Laser","Orbit"},
    {"Tines","Glass","Reed","Felt","Celesta","Drawbars","Copper Keys","Toybox"},
    {"Harp","Marimba","Nylon","Kalimba","Raindrop","Bellwood","Wire","Pick"},
    {"Stratosphere","Cloudbank","Dusk","Chorale","Polar","Nebula","Silk","Horizon"},
    {"Clockwork","Telemetry","Lattice","Cascade","Footsteps","Satellite","Mosaic","Orbitals"},
    {"Stardust","Undertow","Machinery","Embers","Crystals","Mist","Insects","Radio"},
    {"Kick","Snare","Tom","Clap","Hat","Rim","Shaker","Zap"}};
constexpr const char* colors[]{"Amber","Cobalt","Ivory","Jade","Rose","Silver"};
void effect(State& s,unsigned slot,flux::Kind kind,double wet) {
    s.effects[slot].kind=unsigned(kind);s.values[fx(slot,0)]=1;s.values[fx(slot,1)]=wet;
    auto fields=flux::fields(kind);for(unsigned f=0;f<fields.size();++f)s.values[fx(slot,2+f)]=fields[f].initial;
}
void route(State& s,Source source,unsigned target,double amount,bool bipolar=false) {
    s.routes.push_back({source,parameterTable()[target].id,amount,bipolar,true});
}
}
std::span<const FactoryPreset> factoryPresets() {
    static const auto presets=[] {
        std::vector<FactoryPreset> out;out.reserve(256);
        for(unsigned category=0;category<8;++category)for(unsigned i=0;i<counts[category];++i){
            auto name=std::string(families[category][i%8])+" "+colors[i/8];
            out.push_back({name,categories[category],std::string("Original VLTONE ")+categories[category]+". Tone shapes brightness; Movement changes motion; Texture adds harmonics; Space controls the effect tail."});
        }return out;
    }();return presets;
}
State factoryState(unsigned index) {
    auto s=initialState();if(index>=factoryPresets().size())return s;
    auto meta=factoryPresets()[index];s.name=meta.name;s.category=meta.category;s.description=meta.description;s.seed=hash(index+1);
    unsigned category=0,variant=index;while(variant>=counts[category])variant-=counts[category++];
    const unsigned family=variant%8,color=variant/8;const double hue=double(color)/5.;
    s.oscillators[0].bank=family;s.oscillators[1].bank=(family+2+color)%8;s.oscillators[2].bank=(family+5)%8;
    s.values[osc(0,Position)]=.1+.1*color;s.values[osc(1,Position)]=.18+.08*family;
    s.values[osc(1,On)]=1;s.values[osc(1,Level)]=-13-2*hue;s.values[osc(1,Fine)]=3+color*2;
    s.values[filter(0,FilterOn)]=1;s.values[filter(0,Cutoff)]=1800+family*530+color*320;
    s.values[Output]=-12;s.values[env(1,Attack)]=.002;s.values[env(1,Decay)]=.25+.1*hue;s.values[env(1,Sustain)]=0;
    s.values[lfo(0,LfoRate)]=1+color*.5;
    effect(s,0,flux::Kind::Reverb,.12);
    if(category==0){
        s.values[Mono]=1;s.values[Legato]=1;s.values[Glide]=.025+color*.015;s.values[osc(0,Octave)]=-1;s.values[osc(1,Octave)]=-1;
        s.values[osc(4,On)]=1;s.values[osc(4,Level)]=-12;s.values[filter(0,Cutoff)]=180+family*180+color*50;
        s.values[filter(0,Resonance)]=.08+.055*family;s.values[filter(0,Drive)]=3+color*2;
        s.values[env(0,Decay)]=.15+color*.1;s.values[env(0,Sustain)]=.65;s.values[env(0,Release)]=.06;
        if(family==4){s.values[osc(0,Unison)]=5;s.values[osc(0,Detune)]=.2+.04*color;}
        if(family==0){s.oscillators[0].bank=0;s.values[osc(0,Position)]=0;s.values[osc(1,On)]=0;}
        route(s,Source::Env2,filter(0,Cutoff),.18+.02*color);s.values[fx(0,1)]=0;
    } else if(category==1){
        s.values[osc(0,Unison)]=1+family%4;s.values[osc(0,Detune)]=.06+.04*color;s.values[env(0,Sustain)]=.8;s.values[env(0,Release)]=.12+.07*color;
        s.values[osc(1,Octave)]=family%2;s.values[osc(0,WarpMode)]=family%3==0?unsigned(Warp::FM):unsigned(Warp::Bend);s.values[osc(0,WarpAmount)]=.03+.04*hue;
        effect(s,1,flux::Kind::Delay,.18);route(s,Source::Lfo1,osc(0,Fine),.02,true);
    } else if(category==2||category==3){
        s.values[env(0,Attack)]=.0015+color*.001;s.values[env(0,Decay)]=(category==2?1.6:.35)+family*.08+color*.1;s.values[env(0,Sustain)]=category==2?.12:0;s.values[env(0,Release)]=.15+.06*color;
        s.values[osc(1,Octave)]=1+int(family%2);s.values[osc(1,Level)]=-19;s.values[filter(0,Cutoff)]=1000+family*500+color*600;
        route(s,Source::Env2,filter(0,Cutoff),.28);route(s,Source::Env2,osc(0,Position),.12+.02*color);
        if(family==2||family==6){s.values[osc(0,WarpMode)]=unsigned(Warp::FM);s.values[osc(0,WarpAmount)]=.035+.025*hue;}
        if(category==3)effect(s,1,flux::Kind::Delay,.12);
    } else if(category==4){
        s.values[osc(0,Unison)]=4+family%3;s.values[osc(0,Detune)]=.09+.025*color;s.values[osc(0,RandomPhase)]=1;
        s.values[osc(2,On)]=1;s.values[osc(2,Level)]=-20;s.values[osc(2,Octave)]=1;
        s.values[env(0,Attack)]=.35+family*.17+color*.15;s.values[env(0,Release)]=2+color*.45;s.values[env(0,Sustain)]=.8;
        s.values[lfo(0,LfoRate)]=4+color*2;route(s,Source::Lfo1,osc(0,Position),.2,true);route(s,Source::Lfo1,filter(0,Cutoff),.08,true);
        s.values[fx(0,1)]=.3;effect(s,1,flux::Kind::Chorus,.18);
    } else if(category==5){
        s.values[ArpEnabled]=1;s.values[ArpRate]=family%3==0?.125:.25;s.values[ArpOctaves]=1+family%3;s.values[ArpDirection]=color%4;s.values[ArpGate]=.35+.08*color;
        s.values[env(0,Decay)]=.18+.04*color;s.values[env(0,Sustain)]=.05;s.values[env(0,Release)]=.08;
        route(s,Source::Env2,filter(0,Cutoff),.3);effect(s,1,flux::Kind::Delay,.22);
        for(unsigned c=0;c<12;++c){auto& clip=s.clips[c];clip.length=4;for(unsigned n=0;n<16;++n)clip.notes.push_back({n*.25,.16+.01*color,.65+.15*((n+family)%3)/2,48+int((n*3+family+c)%12)});}
    } else if(category==6){
        s.values[env(0,Attack)]=.1+color*.4;s.values[env(0,Release)]=1+color*.5;
        s.values[osc(3,On)]=1;s.values[osc(3,Level)]=-22+color;s.values[osc(0,WarpMode)]=unsigned(Warp::FM);s.values[osc(0,WarpAmount)]=.12+family*.025;
        s.values[osc(1,Semitone)]=7;s.values[lfo(0,LfoRate)]=3+color;s.values[filter(0,Resonance)]=.45;
        route(s,Source::Lfo1,filter(0,Cutoff),.3,true);route(s,Source::Lfo1,osc(0,WarpAmount),.2,true);s.values[fx(0,1)]=.4;
        effect(s,1,flux::Kind::Phaser,.2);
    } else {
        s.values[env(0,Attack)]=.0001;s.values[env(0,Decay)]=.08+family*.015+color*.08;s.values[env(0,Sustain)]=0;s.values[env(0,Release)]=.04;s.values[osc(1,On)]=0;
        s.values[osc(3,On)]=family!=0&&family!=2;s.values[osc(3,Level)]=-6;s.values[filter(0,FilterType)]=family>3?1:0;s.values[filter(0,Cutoff)]=family>3?4500:1800;
        s.values[env(1,Decay)]=.025+.02*color;route(s,Source::Env2,osc(0,Semitone),.25);
        if(family==0||family==2){s.oscillators[0].bank=0;s.values[osc(0,Position)]=0;s.values[osc(0,Octave)]=-2;}
        s.values[fx(0,1)]=.03;
    }
    route(s,Source::Macro1,filter(0,Cutoff),.35);
    route(s,Source(unsigned(Source::Macro1)+1),lfo(0,LfoRate),.12);
    route(s,Source(unsigned(Source::Macro1)+2),osc(0,Position),.35);
    route(s,Source(unsigned(Source::Macro1)+3),fx(0,1),.45);
    route(s,Source(unsigned(Source::Macro1)+4),filter(0,Drive),.4);
    route(s,Source(unsigned(Source::Macro1)+5),osc(0,Spread),.5);
    route(s,Source(unsigned(Source::Macro1)+6),env(0,Attack),.6);
    route(s,Source(unsigned(Source::Macro1)+7),env(0,Release),.5);
    return s;
}
}
