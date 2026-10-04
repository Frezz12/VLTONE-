#include "Internal/ChannelColorInstance.hpp"
#include "Internal/InternalFactory.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <numbers>
#include <tuple>
#include <vector>

namespace { std::atomic<bool> countAllocations{false}; std::atomic<unsigned> allocations{0}; }
void* operator new(std::size_t size) {
    if(countAllocations.load(std::memory_order_relaxed)) ++allocations;
    if(void* p=std::malloc(std::max(size,std::size_t(1)))) return p; throw std::bad_alloc();
}
void* operator new[](std::size_t s) { return ::operator new(s); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
void operator delete[](void* p,std::size_t) noexcept { std::free(p); }

namespace {
using namespace daw::plugins;
using namespace daw::plugins::channel_color;
int failures=0;
void check(bool ok,const char* text) { std::printf("%s %s\n",ok?"PASS":"FAIL",text); failures+=!ok; }
double db(double x) { return 20*std::log10(std::max(std::abs(x),1e-20)); }
struct Block {
    std::vector<float> left,right,out,other;
    const float* input[2]; float* output[2];
    explicit Block(unsigned n):left(n),right(n),out(n),other(n),input{left.data(),right.data()},output{out.data(),other.data()} {}
    void run(ChannelColorInstance& c,unsigned n=0,std::span<const PluginEvent> events={}) {
        PluginProcessContext ctx; ctx.inputs=input; ctx.outputs=output; ctx.frames=n?n:unsigned(left.size());
        ctx.inputChannels=ctx.outputChannels=2; ctx.inputEvents=events; c.process(ctx);
    }
};
std::vector<float> render(double rate,double hz,double amplitude,double drive,double tone,unsigned block=257,
                          bool automation=false,double seconds=.8,std::uint64_t seed=0) {
    ChannelColorInstance c; c.setProfileSeed(seed); c.setParameterFromHost(0,drive); c.setParameterFromHost(1,tone);
    c.activate({rate,block,true}); Block b(block); const unsigned total=unsigned(rate*seconds);
    std::vector<float> output(total);
    for(unsigned start=0;start<total;start+=block) {
        const unsigned frames=std::min(block,total-start);
        for(unsigned i=0;i<frames;++i) b.left[i]=b.right[i]=float(amplitude*std::sin(2*std::numbers::pi*hz*(start+i)/rate));
        std::array<PluginEvent,4> events{}; unsigned n=0;
        if(automation) for(const auto [at,param,value]:std::array<std::tuple<unsigned,unsigned,double>,4>{{
            {143,0,55},{1701,1,100},{2518,0,-75},{3737,1,-80}}}) {
            if(at<start || at>=start+frames) continue;
            auto& event=events[n++]; event.kind=PluginEvent::Kind::ParamValue; event.frameOffset=at-start; event.paramIndex=param; event.value=value;
        }
        b.run(c,frames,std::span(events.data(),n)); std::copy_n(b.out.begin(),frames,output.begin()+start);
    }
    return output;
}
double amplitude(const std::vector<float>& samples,double rate,double hz,unsigned count) {
    double re=0,im=0; const unsigned start=unsigned(samples.size())-count;
    for(unsigned i=0;i<count;++i) { const double angle=2*std::numbers::pi*hz*i/rate; re+=samples[start+i]*std::cos(angle); im+=samples[start+i]*std::sin(angle); }
    return 2*std::hypot(re,im)/count;
}
struct Spectrum { double fundamental,even,odd,thd,dc; };
Spectrum spectrum(const std::vector<float>& samples,double rate,double hz=1000) {
    Spectrum s{}; const unsigned count=unsigned(rate/5); s.fundamental=amplitude(samples,rate,hz,count);
    for(unsigned i=2;i<=12 && i*hz<rate*.5;++i) {
        const double a=amplitude(samples,rate,hz*i,count); if(i%2) s.odd+=a*a; else s.even+=a*a;
    }
    s.thd=100*std::sqrt(s.even+s.odd)/s.fundamental; s.even=std::sqrt(s.even); s.odd=std::sqrt(s.odd);
    for(auto i=samples.end()-count;i!=samples.end();++i) s.dc+=*i; s.dc/=count;
    return s;
}
}

int main() {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    daw::engine::dsp::HalfBandFir65 sharedFir;
    std::array<double,65> oldHistory{}; unsigned oldCursor=0; bool identicalFir=true;
    const auto& taps=daw::engine::dsp::halfBand65Taps();
    for(unsigned i=0;i<10000;++i) {
        const double x=std::sin(i*.173)+.23*std::cos(i*.541);
        oldHistory[oldCursor]=x;
        double old=.5*oldHistory[(oldCursor+65-32)%65];
        for(unsigned tap=1;tap<32;tap+=2)
            old+=taps[tap]*(oldHistory[(oldCursor+65-tap)%65]+oldHistory[(oldCursor+65-(64-tap))%65]);
        oldCursor=(oldCursor+1)%65; identicalFir&=sharedFir.tick(x,taps)==old;
    }
    check(identicalFir,"shared FIR is bit-identical to the original CLA-2A filter");
    InternalFactory factory; auto plugin=factory.create(ChannelColorInstance::staticDescriptor());
    check(plugin && plugin->parameters().size()==2 && plugin->descriptor().uid=="daw.channel-color", "COLOR factory, stable parameters and fixed stage identity");
    ChannelColorInstance c; c.setProfileSeed(0x123456789abcdef0ULL);
    c.setParameterFromHost(0,75); c.setParameterFromHost(1,-30);
    std::vector<std::uint8_t> state; c.saveState(state); ChannelColorInstance loaded;
    check(loaded.loadState(state) && loaded.profileSeed()==c.profileSeed() && loaded.parameterValue(0)==75 && loaded.parameterValue(1)==-30,"profile and settings survive state round-trip");
    bool malformed=true;
    for(const auto* bad:{"{\"version\":\"bad\"}","{\"version\":1,\"params\":{\"drive\":\"bad\"}}","{\"version\":1,\"params\":{},\"profileSeed\":-1}"})
        malformed &= !loaded.loadState({reinterpret_cast<const std::uint8_t*>(bad),std::char_traits<char>::length(bad)}) && loaded.parameterValue(0)==75;
    check(malformed,"malformed state is rejected atomically without throwing");
    c.setParameterFromHost(0,std::numeric_limits<double>::infinity()); c.setParameterFromHost(1,1000);
    check(c.parameterValue(0)==0 && c.parameterValue(1)==100,"invalid parameters are sanitized and bounded");
    bool silence=true,neutral=true,bounded=true;
    for(double rate:{44100.,48000.,96000.,192000.}) {
        for(double drive:{-100.,0.,100.}) {
            ChannelColorInstance processor; processor.setParameterFromHost(0,drive); processor.setParameterFromHost(1,100); processor.activate({rate,512}); Block b(512);
            b.run(processor); for(const float value:b.out) silence &= value==0;
            if(drive==0) {
                for(unsigned i=0;i<512;++i) b.left[i]=b.right[i]=float(std::sin(i*.731)*.8);
                b.run(processor); for(unsigned i=0;i<512;++i) neutral &= b.out[i]==(i<48?0:b.left[i-48]);
            }
            processor.reset();
            for(unsigned i=0;i<512;++i) b.left[i]=float(i%2?1e30:-1e30),b.right[i]=std::numeric_limits<float>::quiet_NaN();
            b.run(processor); for(unsigned i=0;i<512;++i) bounded &= std::isfinite(b.out[i]) && std::isfinite(b.other[i]);
        }
        const double a=std::pow(10.,-18./20); const auto signal=render(rate,1000,a,-20,0);
        const auto s=spectrum(signal,rate);
        std::printf("MEASURE %.0f Hz: reference tape THD %.6f%%, level %.6f dB, DC %.3g\n",rate,s.thd,db(s.fundamental/a),s.dc);
        check(s.thd>=.05 && s.thd<=.2 && std::abs(db(s.fundamental/a))<.2 && std::abs(s.dc)<1e-5,"moderate tape calibration, unit gain and no DC");
    }
    check(silence && neutral && c.latencySamples()==48,"silence and exact 48-sample neutral path at every sample rate");
    check(bounded,"overload and NaN/Inf inputs remain finite");
    {
        ChannelColorInstance mono,stereo; PluginBusLayout accepted;
        mono.setBusLayout({{1},{1}},accepted);
        for(auto* processor:{&mono,&stereo}) {
            processor->setProfileSeed(73); processor->setParameterFromHost(0,75);
            processor->setParameterFromHost(1,56); processor->activate({48000,256});
        }
        Block monoBlock(256),stereoBlock(256); bool independent=true;
        for(unsigned at=0;at<8192;at+=256) {
            for(unsigned i=0;i<256;++i) {
                monoBlock.left[i]=stereoBlock.left[i]=float(.31*std::sin((at+i)*.31));
                stereoBlock.right[i]=at<4096?0.f:float(.7*std::cos((at+i)*.071));
            }
            PluginProcessContext context; context.inputs=monoBlock.input; context.outputs=monoBlock.output;
            context.frames=256; context.inputChannels=context.outputChannels=1; mono.process(context); stereoBlock.run(stereo);
            for(unsigned i=0;i<256;++i) independent&=monoBlock.out[i]==stereoBlock.out[i] && (at>=4096 || stereoBlock.other[i]==0);
        }
        check(independent,"mono and stereo use identical profiles with independent channel histories");
    }
    const auto a=render(48000,1373,.5,-20,0,37,true,.3,73);
    const auto b=render(48000,1373,.5,-20,0,1024,true,.3,73);
    check(a==b,"sample-offset automation and magnetic memory are block-size independent");
    const auto tape=spectrum(render(48000,1000,.25,-80,0),48000);
    const auto tube=spectrum(render(48000,1000,.25,80,0),48000);
    std::printf("MEASURE tape odd %.6g even %.6g; tube odd %.6g even %.6g\n",tape.odd,tape.even,tube.odd,tube.even);
    check(tape.odd>tape.even*100 && tube.even>tape.even*100,"tape odd harmonics and triode even harmonics");
    check(render(48000,700,.2,-30,0,128,false,.3,1)!=render(48000,700,.2,-30,0,128,false,.3,2),"component profiles are deterministic and distinct");
    ChannelColorInstance rt; rt.activate({48000,256}); Block block(256);
    for(unsigned i=0;i<256;++i) block.left[i]=block.right[i]=float(.2*std::sin(i*.2));
    allocations=0; countAllocations=true; for(unsigned i=0;i<100;++i) block.run(rt); countAllocations=false;
    check(allocations==0,"process allocates no memory");
    std::vector<std::unique_ptr<ChannelColorInstance>> channels;
    for(unsigned i=0;i<64;++i) { auto color=std::make_unique<ChannelColorInstance>(); color->setProfileSeed(i+1); color->activate({48000,256}); channels.push_back(std::move(color)); }
    const auto start=std::chrono::steady_clock::now();
    for(unsigned i=0;i<100;++i) for(auto& channel:channels) block.run(*channel);
    const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    std::printf("MEASURE 64 stereo tracks 48k/256: %.3f ms/block, %.1f%% one-core realtime budget\n",seconds*10,seconds/(256.*100/48000)*100);
    for(double rate:{44100.,48000.,96000.,192000.}) {
        const double amp=.05;
        for(double hz:{30.,70.,1000.,15000.}) {
            const auto signal=render(rate,hz,amp,-100,0); const double gain=db(amplitude(signal,rate,hz,unsigned(rate/5))/amp);
            std::printf("MEASURE response %.0f Hz / %.0f Hz: %.3f dB\n",rate,hz,gain);
            check(std::isfinite(gain) && gain > -9 && gain < 2,"bounded colored frequency response");
        }
        for(double drive:{-100.,100.}) for(double tone:{-100.,100.}) {
            const double hz=rate*.211; const auto signal=render(rate,hz,.5,drive,tone);
            const auto s=spectrum(render(rate,1000,.5,drive,tone),rate);
            const double alias=db(amplitude(signal,rate,rate*.367,10000)/amplitude(signal,rate,hz,10000));
            std::printf("MEASURE extremes %.0f Hz / Drive %.0f / Tone %.0f: THD %.4f%% DC %.3g; folded high harmonics %.1f dBc\n",rate,drive,tone,s.thd,s.dc,alias);
            check(std::isfinite(s.thd) && s.thd<=5 && std::abs(s.dc)<1e-4 && alias < -80,"THD below 5%, DC removed and high harmonics suppressed at extreme drive and tone");
        }
    }
    return failures?1:0;
}
