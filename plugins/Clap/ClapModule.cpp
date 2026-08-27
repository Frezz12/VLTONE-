#include "Clap/ClapModule.hpp"
#include "platform/PathUtils.hpp"

#include <filesystem>
#include <map>
#include <mutex>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace daw::plugins {
namespace fs = std::filesystem;

namespace {

/// Where the loadable object lives inside what the user pointed us at.
///
/// On macOS a `.clap` is a bundle directory and the binary sits under
/// `Contents/MacOS/<name>`; everywhere else the path already is the object.
/// Pointing `dlopen` at the bundle directory itself silently fails, which is
/// the single easiest way to conclude a perfectly good plugin is broken.
fs::path binaryPath(const std::string& path) {
    const fs::path bundle = platform::pathFromUtf8(path);
    std::error_code ec;
#if defined(__APPLE__)
    if (fs::is_directory(bundle, ec)) {
        const fs::path macos = bundle / "Contents" / "MacOS";
        if (fs::is_directory(macos, ec)) {
            // The executable is conventionally named after the bundle, but not
            // every vendor obeys that, so take the first regular file.
            const fs::path named = macos / bundle.stem();
            if (fs::is_regular_file(named, ec)) return named;
            for (const auto& entry : fs::directory_iterator(macos, ec)) {
                if (entry.is_regular_file(ec)) return entry.path();
            }
        }
        return {};
    }
#endif
    return bundle;
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

ClapModule::~ClapModule() {
    if (m_entry && m_entry->deinit) m_entry->deinit();
    closeLibrary(m_handle);
}

std::shared_ptr<ClapModule> ClapModule::open(const std::string& path) {
    // One module per file, for the life of the process — see the same cache in
    // Vst3Module for why: repeatedly unloading and reloading a plugin binary
    // re-registers its Objective-C classes and eventually kills the plugin,
    // and opening a large module is far too slow to do per instance.
    static std::mutex* cacheMutex = new std::mutex;
    static auto* cache = new std::map<std::string, std::shared_ptr<ClapModule>>;
    {
        std::lock_guard<std::mutex> lock(*cacheMutex);
        const auto found = cache->find(path);
        if (found != cache->end()) return found->second;
    }

    const fs::path binary = binaryPath(path);
    if (binary.empty()) return nullptr;

    void* handle = openLibrary(binary);
    if (!handle) return nullptr;

    auto* entry = static_cast<const clap_plugin_entry_t*>(
        findSymbol(handle, "clap_entry"));
    if (!entry || !entry->init || !entry->get_factory) {
        closeLibrary(handle);
        return nullptr;
    }

    // A plugin built against a newer CLAP than we vendored may use ABI we do
    // not know about. Refusing here turns "mysterious crash later" into a scan
    // result the user can read.
    if (!clap_version_is_compatible(entry->clap_version)) {
        closeLibrary(handle);
        return nullptr;
    }

    // init() takes the *bundle* path, not the binary inside it.
    if (!entry->init(path.c_str())) {
        closeLibrary(handle);
        return nullptr;
    }

    auto* factory = static_cast<const clap_plugin_factory_t*>(
        entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    if (!factory || !factory->get_plugin_count || !factory->create_plugin) {
        if (entry->deinit) entry->deinit();
        closeLibrary(handle);
        return nullptr;
    }

    // Not make_shared: the constructor is private, and a module is opened once
    // per file, so the extra allocation is irrelevant.
    std::shared_ptr<ClapModule> module(new ClapModule);
    module->m_path = path;
    module->m_handle = handle;
    module->m_entry = entry;
    module->m_factory = factory;

    {
        std::lock_guard<std::mutex> lock(*cacheMutex);
        auto [entry, inserted] = cache->emplace(path, module);
        return entry->second;
    }
}

} // namespace daw::plugins
