#include "Vst/VstModule.hpp"

#include "platform/PathUtils.hpp"

#include <filesystem>
#include <map>
#include <mutex>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif !defined(__APPLE__)
#include <dlfcn.h>
#endif

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace daw::plugins {
namespace fs = std::filesystem;

namespace vst {
namespace {
thread_local HostContext* g_creatingHost = nullptr;
}

VstIntPtr VSTCALLBACK hostCallback(AEffect* effect, VstInt32 opcode,
                                   VstInt32 index, VstIntPtr value, void* ptr,
                                   float opt) noexcept {
    HostContext* host = effect && effect->user
                            ? static_cast<HostContext*>(effect->user)
                            : g_creatingHost;
    if (!host || !host->dispatch) return 0;
    return host->dispatch(*host, effect, opcode, index, value, ptr, opt);
}

class CreationScope {
public:
    explicit CreationScope(HostContext& host) noexcept
        : m_previous(g_creatingHost) {
        g_creatingHost = &host;
    }
    ~CreationScope() { g_creatingHost = m_previous; }

private:
    HostContext* m_previous = nullptr;
};

} // namespace vst

VstModule::~VstModule() {
#if defined(__APPLE__)
    if (m_bundle) {
        CFBundleUnloadExecutable(static_cast<CFBundleRef>(m_bundle));
        CFRelease(static_cast<CFBundleRef>(m_bundle));
    }
#elif defined(_WIN32)
    if (m_handle) ::FreeLibrary(static_cast<HMODULE>(m_handle));
#else
    if (m_handle) ::dlclose(m_handle);
#endif
}

std::shared_ptr<VstModule> VstModule::open(const std::string& path) {
    static std::mutex* cacheMutex = new std::mutex;
    static auto* cache = new std::map<std::string, std::shared_ptr<VstModule>>;
    {
        std::lock_guard<std::mutex> lock(*cacheMutex);
        if (const auto found = cache->find(path); found != cache->end())
            return found->second;
    }

    auto module = std::shared_ptr<VstModule>(new VstModule);
    module->m_path = path;

#if defined(__APPLE__)
    const std::string utf8 = platform::pathToUtf8(platform::pathFromUtf8(path));
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        nullptr, reinterpret_cast<const UInt8*>(utf8.data()),
        CFIndex(utf8.size()), true);
    if (!url) return nullptr;
    CFBundleRef bundle = CFBundleCreate(nullptr, url);
    CFRelease(url);
    if (!bundle || !CFBundleLoadExecutable(bundle)) {
        if (bundle) CFRelease(bundle);
        return nullptr;
    }
    auto symbol = [&](CFStringRef name) {
        return CFBundleGetFunctionPointerForName(bundle, name);
    };
    module->m_entry = reinterpret_cast<EntryProc>(symbol(CFSTR("VSTPluginMain")));
    if (!module->m_entry)
        module->m_entry = reinterpret_cast<EntryProc>(symbol(CFSTR("main_macho")));
    if (!module->m_entry)
        module->m_entry = reinterpret_cast<EntryProc>(symbol(CFSTR("main")));
    if (!module->m_entry) {
        CFBundleUnloadExecutable(bundle);
        CFRelease(bundle);
        return nullptr;
    }
    module->m_bundle = bundle;
#elif defined(_WIN32)
    HMODULE handle = ::LoadLibraryW(platform::pathFromUtf8(path).c_str());
    if (!handle) return nullptr;
    module->m_entry = reinterpret_cast<EntryProc>(
        ::GetProcAddress(handle, "VSTPluginMain"));
    if (!module->m_entry)
        module->m_entry = reinterpret_cast<EntryProc>(::GetProcAddress(handle, "main"));
    if (!module->m_entry) {
        ::FreeLibrary(handle);
        return nullptr;
    }
    module->m_handle = handle;
#else
    void* handle = ::dlopen(platform::pathFromUtf8(path).c_str(),
                            RTLD_LOCAL | RTLD_NOW);
    if (!handle) return nullptr;
    module->m_entry = reinterpret_cast<EntryProc>(::dlsym(handle, "VSTPluginMain"));
    if (!module->m_entry)
        module->m_entry = reinterpret_cast<EntryProc>(::dlsym(handle, "main"));
    if (!module->m_entry) {
        ::dlclose(handle);
        return nullptr;
    }
    module->m_handle = handle;
#endif

    std::lock_guard<std::mutex> lock(*cacheMutex);
    const auto entry = cache->emplace(path, module).first;
    return entry->second;
}

AEffect* VstModule::create(vst::HostContext& host) const noexcept {
    if (!m_entry) return nullptr;
    vst::CreationScope scope(host);
    AEffect* effect = m_entry(&vst::hostCallback);
    if (!effect || effect->magic != kEffectMagic || !effect->dispatcher ||
        effect->numPrograms < 0 || effect->numParams < 0 ||
        effect->numInputs < 0 || effect->numOutputs < 0 ||
        effect->numInputs > 65535 || effect->numOutputs > 65535 ||
        (!effect->processReplacing && !effect->process) ||
        (effect->numParams > 0 && (!effect->setParameter || !effect->getParameter))) {
        return nullptr;
    }
    effect->user = &host;
    return effect;
}

} // namespace daw::plugins
