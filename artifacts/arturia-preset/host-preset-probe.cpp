#include "Host/PluginInstance.hpp"
#include <windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
using namespace daw::plugins;
struct Listener final : PluginListener {
    bool restart=false, reload=false;
    int valueChanges=0, restarts=0;
    void onParameterChanged(std::uint32_t,double) noexcept override {++valueChanges;}
    void onParameterGesture(std::uint32_t,bool) noexcept override {}
    void onLatencyChanged() noexcept override {restart=true;}
    void onRestartRequested() noexcept override {restart=true;++restarts;}
    void onReloadRequested() noexcept override {reload=true;}
};
int main(int argc,char** argv){
    std::setvbuf(stdout,nullptr,_IONBF,0);
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    const std::string path=argc>1?argv[1]:"C:\\Program Files\\Common Files\\VST3\\Arturia\\Pigments.vst3";
    auto* factory=factoryFor(Format::Vst3);auto descriptors=factory->inspect(path);if(descriptors.empty())return 1;
    Listener listener;auto plugin=factory->create(descriptors.front());if(!plugin)return 2;plugin->setListener(&listener);
    int next=-1;for(const auto& p:plugin->parameters())if(p.name=="Next Preset")next=p.index;
    std::printf("plugin=%s parameters=%zu nextPreset=%d\n",plugin->descriptor().name.c_str(),plugin->parameters().size(),next);if(next<0)return 3;
    PluginProcessInfo info{48000,512,false};if(!plugin->activate(info))return 4;plugin->startProcessing();
    std::array<float,512> left{},right{};float* out[]{left.data(),right.data()};PluginProcessContext context;context.outputs=out;context.outputChannels=2;context.frames=512;context.transport.tempo=120;context.playing=true;
    bool passed=true;
    for(int phase=0;phase<5;++phase){double sum=0,peak=0;const int oldChanges=listener.valueChanges;const int oldRestarts=listener.restarts;
        for(int b=0;b<188;++b){std::vector<PluginEvent> events;
            if(b==0 && phase>0){PluginEvent edit;edit.kind=PluginEvent::Kind::ParamValue;edit.paramIndex=next;edit.value=phase%2?1:0;plugin->setParameterFromHost(next,edit.value);events.push_back(edit);}
            if(b==24 || b==100){PluginEvent note;note.kind=PluginEvent::Kind::NoteOn;note.key=60;note.value=.8;events.push_back(note);}
            if(b==85 || b==160){PluginEvent note;note.kind=PluginEvent::Kind::NoteOff;note.key=60;events.push_back(note);}
            context.inputEvents=events;plugin->process(context);context.sampleTime+=512;context.transport.ppqPosition+=512.0/24000;
            if(b>=24)for(float x:left){sum+=double(x)*x;peak=std::max(peak,std::abs(double(x)));}
            MSG msg;while(PeekMessage(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessage(&msg);}
            plugin->pumpMainThread();
            if(listener.restart){listener.restart=false;plugin->stopProcessing();plugin->deactivate();if(!plugin->activate(info))return 5;plugin->startProcessing();}
            if(listener.reload)return 6;
            Sleep(5);
        }
        double rms=std::sqrt(sum/(164*512));std::printf("phase=%d rms=%.9f peak=%.9f cacheValues=%d restarts=%d\n",phase,rms,peak,listener.valueChanges-oldChanges,listener.restarts-oldRestarts);passed &= rms>0.0001;
    }
    plugin->stopProcessing();plugin->deactivate();plugin.reset();CoUninitialize();std::printf("%s\n",passed?"PASS":"FAIL");return passed?0:7;
}


