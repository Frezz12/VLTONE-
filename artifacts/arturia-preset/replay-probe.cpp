#include "Vst3/Vst3Support.hpp"
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <cmath>
#include <cstdio>
#include <windows.h>
using namespace Steinberg;
using namespace Steinberg::Vst;
namespace support = daw::plugins::vst3;
int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const char* mode = argc > 1 ? argv[1] : "all";
    auto module = LoadLibraryW(L"C:\\Program Files\\Common Files\\VST3\\Arturia\\Pigments.vst3");
    auto entry = reinterpret_cast<bool (*)()>(GetProcAddress(module, "InitDll"));
    if (entry) entry();
    auto factory = reinterpret_cast<IPluginFactory* (*)()>(GetProcAddress(module, "GetPluginFactory"))();
    support::HostApplication host;
    FUnknownPtr<IPluginFactory3> f3(factory);
    if (f3) f3->setHostContext(static_cast<IHostApplication*>(&host));
    PClassInfo cls{}; factory->getClassInfo(0, &cls);
    IComponent* component=nullptr;
    if (factory->createInstance(cls.cid, IComponent::iid, (void**)&component)!=kResultOk) return 1;
    component->initialize(static_cast<IHostApplication*>(&host));
    FUnknownPtr<IAudioProcessor> processor(component);
    IEditController* controller=nullptr;
    bool separate = component->queryInterface(IEditController::iid, (void**)&controller)!=kResultOk;
    if(separate) { TUID id{}; component->getControllerClassId(id); factory->createInstance(id,IEditController::iid,(void**)&controller); controller->initialize(static_cast<IHostApplication*>(&host)); }
    int flags=0;
    support::ComponentHandler handler;
    handler.onRestart = [&](int32 f){ flags|=f; std::printf("restart=%x\n",f); };
    controller->setComponentHandler(&handler);
    FUnknownPtr<IConnectionPoint> cp(component), ep(controller);
    if(cp && ep){cp->connect(ep);ep->connect(cp);}
    auto state=owned(new support::MemoryStream);
    component->getState(state);state->rewind();controller->setComponentState(state);
    std::vector<ParameterInfo> params;
    for(int i=0;i<controller->getParameterCount();++i){ ParameterInfo p{}; controller->getParameterInfo(i,p);params.push_back(p);if(p.id==0 || p.id>=1977 && p.id<=2005 || p.id>=2059 && p.id<=2071){std::string name;for(auto c:p.title){if(!c)break;name+=char(c);} std::printf("id=%u flags=%x %s val=%g\n",p.id,p.flags,name.c_str(),controller->getParamNormalized(p.id));}}
    for(int i=0;i<component->getBusCount(kAudio,kOutput);++i)component->activateBus(kAudio,kOutput,i,true);
    for(int i=0;i<component->getBusCount(kEvent,kInput);++i)component->activateBus(kEvent,kInput,i,true);
    ProcessSetup setup{kRealtime,kSample32,512,48000};processor->setupProcessing(setup);component->setActive(true);processor->setProcessing(true);
    auto changes=owned(new support::ParameterChanges);changes->reserve(params.size());
    auto events=owned(new support::EventList);events->reserve(32);
    float l[512]{}, r[512]{};float* buffers[]{l,r}; AudioBusBuffers output{};output.numChannels=2;output.channelBuffers32=buffers;
    ProcessData data{};data.processMode=kRealtime;data.symbolicSampleSize=kSample32;data.numSamples=512;data.numOutputs=1;data.outputs=&output;data.inputParameterChanges=changes;data.inputEvents=events;
    ProcessContext context{};context.sampleRate=48000;context.tempo=120;context.timeSigNumerator=4;context.timeSigDenominator=4;context.state=ProcessContext::kPlaying|ProcessContext::kTempoValid|ProcessContext::kTimeSigValid|ProcessContext::kProjectTimeMusicValid;data.processContext=&context;
    for(int phase=0;phase<3;++phase){
        changes->clear();
        if(phase==1 && std::strcmp(mode,"next")==0){controller->setParamNormalized(2005,1);changes->begin(2005)->add(0,1);} if(phase==1){int n=0;for(const auto& p:params){bool include=std::strcmp(mode,"all")==0 || (std::strcmp(mode,"auto")==0 && (p.flags&ParameterInfo::kCanAutomate)) || (std::strcmp(mode,"cc7")==0 && p.id==2066) || (std::strcmp(mode,"normal")==0 && p.id<1977);if(include){changes->begin(p.id)->add(0,controller->getParamNormalized(p.id));++n;}}std::printf("replay %d parameters mode=%s\n",n,mode);}
        double sum=0,peak=0;
        for(int b=0;b<94;++b){events->clear();if(b==0){Event e{};e.type=Event::kNoteOnEvent;e.noteOn.pitch=60;e.noteOn.velocity=.8f;e.noteOn.noteId=-1;events->addEvent(e);} if(b==70){Event e{};e.type=Event::kNoteOffEvent;e.noteOff.pitch=60;e.noteOff.noteId=-1;events->addEvent(e);} std::fill_n(l,512,0);std::fill_n(r,512,0);output.silenceFlags=0;processor->process(data);changes->clear();for(float v:l){sum+=double(v)*v;peak=std::max(peak,std::abs(double(v)));} context.projectTimeSamples+=512;context.projectTimeMusic+=512.0/24000;MSG msg;while(PeekMessage(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessage(&msg);} Sleep(2);}
        std::printf("phase=%d rms=%.9f peak=%.9f flags=%x\n",phase,std::sqrt(sum/(94*512)),peak,flags);
    }
    processor->setProcessing(false);component->setActive(false);
    if(cp&&ep){ep->disconnect(cp);cp->disconnect(ep);}cp=nullptr;ep=nullptr;
    controller->setComponentHandler(nullptr);if(separate)controller->terminate();controller->release();processor=nullptr;component->terminate();component->release();f3=nullptr;factory->release();
    auto leave = reinterpret_cast<bool (*)()>(GetProcAddress(module,"ExitDll"));if(leave)leave();FreeLibrary(module);CoUninitialize();
}


