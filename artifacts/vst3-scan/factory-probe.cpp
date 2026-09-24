#include "Vst3/Vst3Support.hpp"
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <cstdio>
#include <windows.h>
using namespace Steinberg;
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    auto module = LoadLibraryW(L"C:\\Program Files\\Common Files\\VST3\\Arturia\\MiniFreak V.vst3");
    if (!module) { std::printf("LoadLibrary failed: %lu\n", GetLastError()); return 1; }
    auto entry = reinterpret_cast<bool (*)()>(GetProcAddress(module, "InitDll"));
    if (entry && !entry()) { std::puts("InitDll failed"); return 2; }
    auto factory = reinterpret_cast<IPluginFactory* (*)()>(GetProcAddress(module, "GetPluginFactory"))();
    FUnknownPtr<IPluginFactory3> factory3(factory);
    std::printf("classes: %d\n", factory->countClasses());
    if (factory3) factory3->setHostContext(daw::plugins::vst3::hostApplication());
    for (int i = 0; i < factory->countClasses(); ++i) {
        PClassInfo info{};
        factory->getClassInfo(i, &info);
        std::printf("class %s (%s)\n", info.name, info.category);
        const FUID* ids[] = {&Vst::IComponent::iid, &IPluginBase::iid, &FUnknown::iid};
        const char* names[] = {"IComponent", "IPluginBase", "FUnknown"};
        for (int j = 0; j < 3; ++j) {
            void* raw = nullptr;
            auto result = factory->createInstance(info.cid, *ids[j], &raw);
            std::printf("  %s: %08X, object=%p\n", names[j], unsigned(result), raw);
            if (raw) {
                auto unknown = static_cast<FUnknown*>(raw);
                void* component = nullptr;
                auto query = unknown->queryInterface(Vst::IComponent::iid, &component);
                std::printf("    query IComponent: %08X, object=%p\n", unsigned(query), component);
                if (component) static_cast<Vst::IComponent*>(component)->release();
                unknown->release();
            }
        }
    }
    factory3 = nullptr;
    factory->release();
    auto exit = reinterpret_cast<bool (*)()>(GetProcAddress(module, "ExitDll"));
    if (exit) exit();
    FreeLibrary(module);
}
