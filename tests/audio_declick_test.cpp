// Signal-level regression tests for user-triggered audio transitions.
#include "Engine/RealtimeEngine.hpp"
#include "Nodes/PlaybackNodes.hpp"
#include "Nodes/PreviewPlayerNode.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <numbers>
#include <vector>

using namespace daw::engine;
namespace {
int failures = 0;
void check(bool ok, const char* label) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    failures += !ok;
}
struct Output {
    std::vector<float> left, right;
    float* channels[2];
    explicit Output(FrameCount frames) : left(frames), right(frames), channels{left.data(),right.data()} {}
    AudioBlock block() { return {channels,2,FrameCount(left.size())}; }
    bool smooth(float previous, float maximumDelta = .025f) const {
        for (std::size_t i=0;i<left.size();++i) {
            const auto value=left[i];
            if (!std::isfinite(value) || std::abs(value)>1.f ||
                value!=right[i] || std::abs(value-previous)>maximumDelta) return false;
            previous=value;
        }
        return true;
    }
    bool silent() const {
        return std::all_of(left.begin(),left.end(),[](float v){return v==0.f;});
    }
};
std::shared_ptr<SampleBuffer> dc(SampleRate rate, FrameCount frames, float value) {
    auto sample=std::make_shared<SampleBuffer>(2,frames,rate);
    for(ChannelCount ch=0;ch<2;++ch) std::fill_n(sample->writableChannel(ch),frames,value);
    return sample;
}
struct TransportTone : Node {
    float level = .8f;
    std::string_view name() const noexcept override { return "transport tone"; }
    bool isSource() const noexcept override { return true; }
    void process(const ProcessContext& c) override {
        const auto value = !c.playing ? .2f : c.timelinePosition >= 10000 ? -level : level;
        for(ChannelCount ch=0;ch<c.output.numChannels();++ch)
            std::fill_n(c.output.data(ch),c.frames,value);
    }
};
void previewTransitions(SampleRate rate, FrameCount frames) {
    PreviewPlayerNode player;
    player.prepare({rate,frames,2,false});
    Output output(frames);
    ProcessContext c; c.output=output.block(); c.frames=frames; c.sampleRate=rate;
    auto positive=dc(rate,FrameCount(rate*2),.8f);
    auto negative=dc(rate,FrameCount(rate*2),-.8f);
    const int drain=int(std::ceil(rate*.005/frames))+2;
    float previous=0.f;
    auto render=[&] {
        player.process(c);
        const bool smooth=output.smooth(previous);
        previous=output.left.back();
        return smooth;
    };
    bool smooth=true;
    player.start(positive);
    for(int i=0;i<drain;++i) smooth &= render();
    smooth &= output.left.back()==.8f;
    player.start(negative);
    for(int i=0;i<drain;++i) smooth &= render();
    smooth &= output.left.back()==-.8f;
    player.setGain(.25f);
    for(int i=0;i<drain;++i) smooth &= render();
    smooth &= std::abs(output.left.back()+.2f)<1e-6f;
    player.stop();
    smooth &= render() && !player.playing() && player.positionFrames()==0;
    for(int i=0;i<drain;++i) smooth &= render();
    check(smooth && output.silent(), "preview start, file swap, level and stop are continuous and drain in 5 ms");

    player.start(positive); player.stop(); player.start(negative);
    player.process(c);
    check(player.playing() && player.positionFrames()==frames,
          "rapid start/stop/start uses the latest source and restarts its read head");
    for(int i=0;i<drain;++i) player.process(c);
    check(std::abs(output.left.back()+.2f)<1e-6f,"latest preview source reaches its correct level");
    player.start(positive); player.stop();
    for(int i=0;i<drain;++i) player.process(c);
    check(!player.playing() && output.silent(),"the latest stop cannot be overwritten by an earlier start");

    player.reset(); player.setGain(1.f); previous=0.f;
    auto bass=dc(rate,FrameCount(rate),0.f);
    for(ChannelCount ch=0;ch<2;++ch)
        for(FrameCount i=0;i<bass->frames();++i)
            bass->writableChannel(ch)[i]=.8f*std::sin(2.*std::numbers::pi*30.*i/rate);
    player.start(bass);
    for(int i=0;i<drain;++i) render();
    smooth=true; player.seekFrames(FrameCount(rate*.075));
    smooth &= render() && player.positionFrames()==FrameCount(rate*.075)+frames;
    for(int i=0;i<drain;++i) smooth &= render();
    player.start(negative);
    for(int i=0;i<drain;++i) smooth &= render();
    check(smooth,"seeking and switching a bass sample have no sample discontinuity");

    player.reset(); previous=0.f;
    auto shortSample=dc(rate,frames*3,.8f);
    player.start(shortSample); smooth=true;
    for(int i=0;i<3;++i) smooth &= render();
    smooth &= !player.playing();
    for(int i=0;i<drain;++i) smooth &= render();
    check(smooth && output.silent(),"natural preview EOF fades without changing source duration");

    player.reset();player.setLoop(true);previous=0.f;
    auto loopBass=dc(rate,FrameCount(rate*.011),0.f);
    for(ChannelCount ch=0;ch<2;++ch)
        for(FrameCount i=0;i<loopBass->frames();++i)
            loopBass->writableChannel(ch)[i]=.8f*std::cos(2.*std::numbers::pi*30.*i/rate);
    player.start(loopBass);smooth=true;
    for(int i=0;i<drain+8;++i)smooth &= render();
    check(smooth && player.playing(),"bass preview loop seams stay continuous, including several wraps inside a callback");

    for(FrameCount loopFrames : {1u,7u,1024u}) {
        player.reset(); player.setLoop(true); player.start(dc(rate,loopFrames,.8f));
        bool valid=true;
        for(int i=0;i<drain+2;++i) {
            player.process(c);
            valid &= player.playing() && std::all_of(output.left.begin(),output.left.end(),
                [](float v){return std::isfinite(v) && v>=0.f && v<=.8f;});
        }
        check(valid && output.left.back()==.8f,"short constant preview loops cannot stall an unfinished fade");
    }
    c.offline=true; player.process(c);
    check(output.silent(),"preview transitions remain absent from offline exports");
}
void transportTransitions(SampleRate rate, FrameCount frames) {
    RealtimeEngine engine(1);
    auto node=engine.graph().adoptNode(std::make_shared<TransportTone>());
    engine.graph().setSink(node);
    check(bool(engine.prepare(rate,frames,2)),"transition graph prepares");
    Output out(frames);
    const int drain=int(std::ceil(rate*.005/frames))+2;
    float previous=.2f;
    engine.renderBlock(out.block(),nullptr,0,frames);
    auto render=[&] {
        engine.renderBlock(out.block(),nullptr,0,frames);
        const bool ok=out.smooth(previous);
        previous=out.left.back();
        return ok;
    };
    bool smooth=true;
    engine.transport().play();
    for(int i=0;i<drain;++i) smooth &= render();
    smooth &= out.left.back()==.8f;
    engine.transport().seek(20000); smooth &= render();
    smooth &= engine.lastBlockPosition()==20000;
    for(int i=0;i<drain;++i) smooth &= render();
    smooth &= out.left.back()==-.8f;
    engine.transport().pause();
    smooth &= render() && !engine.transport().isPlaying();
    for(int i=0;i<drain;++i) smooth &= render();
    smooth &= out.left.back()==.2f;
    engine.transport().play();
    for(int i=0;i<drain;++i) smooth &= render();
    engine.transport().stop();
    smooth &= render() && !engine.transport().isPlaying();
    for(int i=0;i<drain;++i) smooth &= render();
    check(smooth && out.left.back()==.2f,"transport play, seek, pause and stop are continuous and preserve live monitoring");
    engine.transport().seek(0);engine.transport().play();
    for(int i=0;i<drain;++i)render();
    auto replacement=std::make_shared<TransportTone>();replacement->level=-.8f;
    engine.graph().setSink(engine.graph().adoptNode(replacement));
    check(bool(engine.commitGraph()),"live graph replacement commits");
    smooth=true;
    for(int i=0;i<drain;++i)smooth &= render();
    check(smooth && out.left.back()==-.8f,"replacing the live routing graph preserves the signal boundary");

}
void clipAndInputTransitions() {
    constexpr FrameCount frames=512;
    Output out(frames);
    ProcessContext c; c.output=out.block(); c.frames=frames; c.sampleRate=48000; c.playing=true;
    ClipPlayerNode player;
    player.prepare({48000,frames,2,false});
    auto clips=std::make_shared<ClipPlayerNode::ClipList>();
    ClipPlacement placement; placement.audio=dc(48000,4096,.8f); placement.lengthSamples=4096;
    clips->push_back(placement); player.setClips(clips);
    player.process(c);
    check(out.left.front()==0.f && out.left.back()==.8f,"a timeline clip fades in at its exact boundary");
    c.timelinePosition=512; player.process(c);
    const float previous=out.left.back();
    player.setClips(std::make_shared<ClipPlayerNode::ClipList>());
    c.timelinePosition=1024; player.process(c);
    check(out.left.front()==previous && out.smooth(previous) && out.left.back()==0.f,
          "removing a playing clip drains its output instead of cutting it");
    player.setClips(clips); c.offline=true; c.timelinePosition=0; player.process(c);
    check(std::all_of(out.left.begin(),out.left.end(),[](float v){return v==.8f;}),
          "offline clip rendering keeps the source and user fade math unchanged");

    Output input(frames);
    std::fill(input.left.begin(),input.left.end(),.8f);
    std::fill(input.right.begin(),input.right.end(),-.8f);
    const float* channels[2]{input.left.data(),input.right.data()};
    InputBus bus{channels,2,frames};
    InputNode monitor("monitor",&bus,0,1);
    c.offline=false; monitor.process(c);
    monitor.setRouting(1,1,true); monitor.process(c);
    check(out.left.front()==.8f && out.left.back()==-.8f && out.smooth(.8f),
          "changing monitored input channels crossfades without a click");
    monitor.setEnabled(false); monitor.process(c);
    check(out.left.front()==-.8f && out.smooth(-.8f) && out.left.back()==0.f,
          "disabling live monitoring fades to silence");
}
}
int main() {
    for(SampleRate rate:{44100.,48000.,96000.})
        for(FrameCount frames:{32u,256u,1024u}) {
            std::printf("rate %.0f, block %u\n",rate,frames);
            previewTransitions(rate,frames);
            transportTransitions(rate,frames);
        }
    clipAndInputTransitions();
    return failures ? 1 : 0;
}
