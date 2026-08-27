#include "Vst3/Vst3Module.hpp"

#include "Vst3/Vst3Support.hpp"
#include "platform/PathUtils.hpp"

#include <pluginterfaces/base/ipluginbase.h>

#include <filesystem>
#include <map>
#include <mutex>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace daw::plugins {
namespace fs = std::filesystem;

namespace {

using GetFactoryProc = Steinberg::IPluginFactory* (*)();

/// The loadable object inside what the user pointed us at.
///
/// A `.vst3` is a bundle directory on macOS and Linux — the binary is under
/// `Contents/MacOS/` or `Contents/<arch>-linux/` — and a plain DLL on Windows.
fs::path binaryPath(const std::string& path) {
    const fs::path bundle = platform::pathFromUtf8(path);
    std::error_code ec;
    if (!fs::is_directory(bundle, ec)) return bundle;

#if defined(__APPLE__)
    const fs::path directory = bundle / "Contents" / "MacOS";
#elif defined(_WIN32)
    const fs::path directory = bundle / "Contents" / "x86_64-win";
#else
    const fs::path directory = bundle / "Contents" / "x86_64-linux";
#endif
    if (!fs::is_directory(directory, ec)) return {};

    // Conventionally named after the bundle, but not every vendor obeys that,
    // so fall back to the first regular file in there.
    const fs::path named = directory / bundle.stem();
    if (fs::is_regular_file(named, ec)) return named;
    for (const auto& entry : fs::directory_iterator(directory, ec)) {
        if (entry.is_regular_file(ec)) return entry.path();
    }
    return {};
}

void* openLibrary(const fs::path& path) {
#if defined(_WIN32)
    return static_cast<void*>(::LoadLibraryW(path.c_str()));
#else
    return ::dlopen(path.c_str(), RTLD_LOCAL | RTLD_NOW);
#endif
}

void closeLibrary(void* handle) {
    if (!handle) return;
#if defined(_WIN32)
    ::FreeLibrary(static_cast<HMODULE>(handle));
#else
    ::dlclose(handle);
#endif
}

void* findSymbol(void* handle, const char* name) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(
        ::GetProcAddress(static_cast<HMODULE>(handle), name));
#else
    return ::dlsym(handle, name);
#endif
}

} // namespace

Vst3Module::~Vst3Module() {
    if (m_factory) m_factory->release();
    if (m_exit) m_exit();
#if defined(__APPLE__)
    if (m_bundle) CFRelease(static_cast<CFBundleRef>(m_bundle));
#endif
    closeLibrary(m_handle);
}

std::shared_ptr<Vst3Module> Vst3Module::open(const std::string& path) {
    // One module per file, for the life of the process.
    //
    // Not an optimisation, though it is a large one — a shell like Waves takes
    // over a second to open and holds 700+ plugins, and this host used to pay
    // that per *instance*. It is a correctness fix: repeatedly `dlopen`ing and
    // `dlclose`ing the same module re-registers its Objective-C classes each
    // time, and after a few hundred cycles the runtime hands a plugin a stale
    // class and it dies inside itself. Real hosts keep plugin binaries resident
    // for the same reason.
    //
    // Deliberately never emptied: unloading at exit would run third-party
    // teardown after our own statics have gone, which buys nothing and can
    // crash on the way out.
    static std::mutex* cacheMutex = new std::mutex;
    static auto* cache = new std::map<std::string, std::shared_ptr<Vst3Module>>;
    {
        std::lock_guard<std::mutex> lock(*cacheMutex);
        const auto found = cache->find(path);
        if (found != cache->end()) return found->second;
    }

    const fs::path binary = binaryPath(path);
    if (binary.empty()) return nullptr;

    void* handle = openLibrary(binary);
    if (!handle) return nullptr;

    void* bundle = nullptr;
    bool entered = false;
    bool (*exitProc)() = nullptr;

    // Each platform has its own name for "the module is being loaded", and a
    // module whose entry point fails must not be used at all.
#if defined(__APPLE__)
    using BundleEntryProc = bool (*)(CFBundleRef);
    auto* entry = reinterpret_cast<BundleEntryProc>(findSymbol(handle, "bundleEntry"));
    exitProc = reinterpret_cast<bool (*)()>(findSymbol(handle, "bundleExit"));
    if (entry) {
        // A real CFBundleRef, not null: plugins read their own resources
        // (presets, images, localisation) through it, and the ones that assume
        // it is valid crash on null rather than degrading.
        CFURLRef url = CFURLCreateFromFileSystemRepresentation(
            nullptr, reinterpret_cast<const UInt8*>(path.c_str()),
            CFIndex(path.size()), true);
        if (url) {
            bundle = CFBundleCreate(nullptr, url);
            CFRelease(url);
        }
        entered = entry(static_cast<CFBundleRef>(bundle));
    }
#elif defined(_WIN32)
    auto* entry = reinterpret_cast<bool (*)()>(findSymbol(handle, "InitDll"));
    exitProc = reinterpret_cast<bool (*)()>(findSymbol(handle, "ExitDll"));
    if (entry) entered = entry();
#else
    using ModuleEntryProc = bool (*)(void*);
    auto* entry = reinterpret_cast<ModuleEntryProc>(findSymbol(handle, "ModuleEntry"));
    exitProc = reinterpret_cast<bool (*)()>(findSymbol(handle, "ModuleExit"));
    if (entry) entered = entry(handle);
#endif

    auto fail = [&] {
        if (entered && exitProc) exitProc();
#if defined(__APPLE__)
        if (bundle) CFRelease(static_cast<CFBundleRef>(bundle));
#endif
        closeLibrary(handle);
        return std::shared_ptr<Vst3Module>();
    };

    // An entry point that exists and refused is a hard no. One that does not
    // exist at all is tolerated: a few older modules ship without it.
    if (entry && !entered) return fail();

    auto* getFactory = reinterpret_cast<GetFactoryProc>(
        findSymbol(handle, "GetPluginFactory"));
    if (!getFactory) return fail();

    Steinberg::IPluginFactory* factory = getFactory();
    if (!factory) return fail();

    // Hand the factory the host context before anything is created from it.
    //
    // `IPluginFactory3::setHostContext` reads as optional and is anything but:
    // a shell that holds hundreds of plugins uses it to find out who it is
    // talking to, and refuses `createInstance` outright without it. Every Waves
    // plugin on this machine — 88 of a 120-plugin sample — answered
    // `kResultFalse` and looked to the user like a plugin that simply would not
    // load. Once per module, which is once per process: the cache below makes
    // sure of that.
    Steinberg::FUnknownPtr<Steinberg::IPluginFactory3> factory3(factory);
    if (factory3) factory3->setHostContext(vst3::hostApplication());

    std::shared_ptr<Vst3Module> module(new Vst3Module);
    module->m_path = path;
    module->m_handle = handle;
    module->m_bundle = bundle;
    module->m_factory = factory;   // the reference GetPluginFactory returned
    module->m_exit = entered ? exitProc : nullptr;

    {
        std::lock_guard<std::mutex> lock(*cacheMutex);
        // Another thread may have opened it while this one was loading; keep
        // whichever got there first so there is only ever one per file.
        auto [entry, inserted] = cache->emplace(path, module);
        return entry->second;
    }
}

} // namespace daw::plugins
