#include "Clap/ClapFactory.hpp"
#include "Clap/ClapInstance.hpp"
#include "Clap/ClapModule.hpp"
#include "platform/KnownFolders.hpp"
#include "platform/PathUtils.hpp"

#include <cstdlib>
#include <filesystem>

namespace daw::plugins {
namespace fs = std::filesystem;

namespace {

/// Split a PATH-style variable the way each platform writes it.
void appendFromEnvironment(const char* variable, std::vector<std::string>& out) {
#if defined(_WIN32)
    // CLAP_PATH is user-controlled and may itself contain non-ACP characters.
    // Reading the narrow CRT environment would undo the rest of the wide-path
    // scanner before it even reaches CreateProcessW.
    (void)variable;
    const wchar_t* raw = ::_wgetenv(L"CLAP_PATH");
    if (!raw) return;
    const std::wstring value(raw);
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t end = value.find(L';', start);
        const std::wstring entry = value.substr(
            start, end == std::wstring::npos ? std::wstring::npos : end - start);
        if (!entry.empty())
            out.push_back(platform::pathToUtf8(fs::path(entry)));
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
#else
    const char* raw = std::getenv(variable);
    if (!raw) return;
    constexpr char kSeparator = ':';
    std::string value(raw);
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t end = value.find(kSeparator, start);
        const std::string entry =
            value.substr(start, end == std::string::npos ? std::string::npos
                                                         : end - start);
        if (!entry.empty()) out.push_back(entry);
        if (end == std::string::npos) break;
        start = end + 1;
    }
#endif
}

std::string homeDirectory() {
#if defined(_WIN32)
    return platform::pathToUtf8(
        platform::knownFolderPath(platform::KnownFolder::Profile));
#else
    const char* home = std::getenv("HOME");
    return home ? home : "";
#endif
}

bool looksLikeClap(const fs::path& path) {
    return path.extension() == ".clap";
}

/// Convert one CLAP descriptor into ours. Category and instrument-ness come
/// from the feature list, which is CLAP's only classification.
PluginDescriptor describe(const clap_plugin_descriptor_t& raw,
                          const std::string& path) {
    PluginDescriptor out;
    out.format = Format::Clap;
    out.uid = raw.id ? raw.id : "";
    out.path = path;
    out.name = raw.name ? raw.name : out.uid;
    out.vendor = raw.vendor ? raw.vendor : "";
    out.version = raw.version ? raw.version : "";

    for (const char* const* feature = raw.features; feature && *feature; ++feature) {
        const std::string_view value(*feature);
        if (value == CLAP_PLUGIN_FEATURE_INSTRUMENT) {
            out.isInstrument = true;
            out.wantsMidi = true;
        } else if (value == CLAP_PLUGIN_FEATURE_NOTE_EFFECT ||
                   value == CLAP_PLUGIN_FEATURE_NOTE_DETECTOR) {
            out.wantsMidi = true;
        }
        if (out.category.empty() && value != CLAP_PLUGIN_FEATURE_INSTRUMENT &&
            value != CLAP_PLUGIN_FEATURE_AUDIO_EFFECT) {
            out.category = value;
        }
    }
    if (out.category.empty()) {
        out.category = out.isInstrument ? "Instrument" : "Audio Effect";
    }

    std::error_code ec;
    const fs::path nativePath = platform::pathFromUtf8(path);
    // Bundles are directories, so file_size would fail; the timestamp of the
    // bundle is what changes when it is replaced, and that is what a rescan
    // compares against.
    out.fileSize =
        fs::is_regular_file(nativePath, ec) ? fs::file_size(nativePath, ec) : 0;
    const auto written = fs::last_write_time(nativePath, ec);
    out.fileModifiedTime =
        ec ? 0 : written.time_since_epoch().count();
    return out;
}

} // namespace

std::vector<std::string> ClapFactory::defaultSearchPaths() const {
    std::vector<std::string> paths;
    const std::string home = homeDirectory();
#if defined(__APPLE__)
    if (!home.empty()) paths.push_back(home + "/Library/Audio/Plug-Ins/CLAP");
    paths.push_back("/Library/Audio/Plug-Ins/CLAP");
#elif defined(_WIN32)
    const fs::path common =
        platform::knownFolderPath(platform::KnownFolder::CommonProgramFiles);
    if (!common.empty()) paths.push_back(platform::pathToUtf8(common / "CLAP"));
    const fs::path local =
        platform::knownFolderPath(platform::KnownFolder::LocalAppData);
    if (!local.empty())
        paths.push_back(platform::pathToUtf8(local / "Programs" / "Common" /
                                             "CLAP"));
#else
    if (!home.empty()) paths.push_back(home + "/.clap");
    paths.push_back("/usr/lib/clap");
#endif
    appendFromEnvironment("CLAP_PATH", paths);
    return paths;
}

std::vector<std::string> ClapFactory::enumerateCandidates(
    const std::string& directory) const {
    std::vector<std::string> found;
    std::error_code ec;
    const fs::path root = platform::pathFromUtf8(directory);
    if (!fs::is_directory(root, ec)) return found;

    // Recursive, because vendors nest by brand, but never following symlinks:
    // a plug-in folder that links to its own parent is not hypothetical, and
    // this walk must terminate.
    fs::recursive_directory_iterator it(
        root, fs::directory_options::skip_permission_denied, ec);
    if (ec) return found;
    for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const fs::path& path = it->path();
        if (!looksLikeClap(path)) continue;
        found.push_back(platform::pathToUtf8(path));
        // A `.clap` bundle is a directory; do not descend into it.
        if (it->is_directory(ec)) it.disable_recursion_pending();
    }
    return found;
}

std::vector<PluginDescriptor> ClapFactory::inspect(const std::string& path) const {
    std::vector<PluginDescriptor> found;
    auto module = ClapModule::open(path);
    if (!module) return found;

    const clap_plugin_factory_t* factory = module->factory();
    const std::uint32_t count = factory->get_plugin_count(factory);
    for (std::uint32_t i = 0; i < count; ++i) {
        const clap_plugin_descriptor_t* raw =
            factory->get_plugin_descriptor(factory, i);
        if (!raw || !raw->id) continue;
        found.push_back(describe(*raw, path));
    }
    return found;
}

std::unique_ptr<PluginInstance> ClapFactory::create(
    const PluginDescriptor& descriptor) {
    auto module = ClapModule::open(descriptor.path);
    if (!module) return nullptr;

    // Chicken and egg: `create_plugin` needs a host pointer, but `host_data`
    // should point at the instance, which cannot exist until the plugin does.
    // Allocate the host separately, create with `host_data` null, then fill it
    // in. Safe because CLAP forbids the plugin from calling host callbacks
    // during create and init, so nothing can dereference it before the patch.
    auto holder = std::make_unique<clap_host_t>();
    ClapInstance::fillHost(*holder, nullptr);

    const clap_plugin_factory_t* factory = module->factory();
    const clap_plugin_t* plugin =
        factory->create_plugin(factory, holder.get(), descriptor.uid.c_str());
    if (!plugin) return nullptr;
    if (plugin->init && !plugin->init(plugin)) {
        if (plugin->destroy) plugin->destroy(plugin);
        return nullptr;
    }

    auto instance =
        std::make_unique<ClapInstance>(std::move(module), plugin, descriptor);
    // Point the host back at the instance now that one exists. The plugin was
    // forbidden from calling host callbacks during create/init, so nothing can
    // have dereferenced host_data before this line.
    holder->host_data = instance.get();
    instance->adoptHost(std::move(holder));
    return instance;
}

} // namespace daw::plugins
