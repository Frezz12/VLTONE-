#include "Vst3/Vst3Factory.hpp"

#include "Vst3/Vst3Instance.hpp"
#include "Vst3/Vst3Module.hpp"
#include "platform/KnownFolders.hpp"
#include "platform/PathUtils.hpp"

#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace daw::plugins {
namespace fs = std::filesystem;
using namespace Steinberg;

namespace {

std::string homeDirectory() {
#if defined(_WIN32)
    return platform::pathToUtf8(
        platform::knownFolderPath(platform::KnownFolder::Profile));
#else
    const char* home = std::getenv("HOME");
    return home ? home : "";
#endif
}

/// Read a class id off a plugin's `subCategories` string, which is a list of
/// tags separated by `|`.
bool hasSubCategory(const char* subCategories, const char* wanted) {
    if (!subCategories) return false;
    const std::string all(subCategories);
    const std::string needle(wanted);
    std::size_t start = 0;
    while (start <= all.size()) {
        const std::size_t end = all.find('|', start);
        const std::string tag =
            all.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (tag == needle) return true;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return false;
}

/// A `Vst3Instance` needs both halves of a VST3 plugin: the processor
/// (`IComponent` + `IAudioProcessor`) and the controller (`IEditController`).
/// They may be one object or two, and connecting the two-object case is the
/// step most naive hosts get wrong.
struct ClassEntry {
    PluginDescriptor descriptor;
    bool valid = false;
};

ClassEntry describe(IPluginFactory* factory, int32 index, const std::string& path) {
    ClassEntry entry;

    PClassInfo info{};
    if (factory->getClassInfo(index, &info) != kResultOk) return entry;
    // Only audio modules; a factory also advertises its controller classes,
    // and instantiating one of those as a plugin gets a silent, editorless
    // object that looks broken rather than absent.
    if (std::strcmp(info.category, kVstAudioEffectClass) != 0) return entry;

    entry.descriptor.format = Format::Vst3;
    entry.descriptor.uid = vst3::uidToString(info.cid);
    entry.descriptor.path = path;
    entry.descriptor.name = info.name;

    // PClassInfo2 adds vendor, version and the sub-category list — the only
    // place VST3 says whether a plugin is an instrument.
    FUnknownPtr<IPluginFactory2> factory2(factory);
    if (factory2) {
        PClassInfo2 info2{};
        if (factory2->getClassInfo2(index, &info2) == kResultOk) {
            entry.descriptor.vendor = info2.vendor;
            entry.descriptor.version = info2.version;
            entry.descriptor.category = info2.subCategories;
            entry.descriptor.isInstrument =
                hasSubCategory(info2.subCategories, "Instrument");
        }
    }
    if (entry.descriptor.isInstrument) entry.descriptor.wantsMidi = true;
    if (entry.descriptor.category.empty()) {
        entry.descriptor.category = entry.descriptor.isInstrument ? "Instrument" : "Fx";
    }

    std::error_code ec;
    const fs::path nativePath = platform::pathFromUtf8(path);
    entry.descriptor.fileSize =
        fs::is_regular_file(nativePath, ec) ? fs::file_size(nativePath, ec) : 0;
    const auto written = fs::last_write_time(nativePath, ec);
    entry.descriptor.fileModifiedTime = ec ? 0 : written.time_since_epoch().count();

    entry.valid = true;
    return entry;
}

} // namespace

namespace vst3 {

#if COM_COMPATIBLE
// VST3 stores the first three UUID fields in COM byte order on Windows.
// Project files use one canonical representation on every platform.
constexpr int kCanonicalToNative[16] = {
    3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
#endif

std::string uidToString(const char* cid) {
    // Plain canonical hex, independent of the SDK's platform-specific TUID
    // memory layout, so a project saved on Windows opens on macOS and Linux.
    std::string out;
    out.resize(32);
    for (int i = 0; i < 16; ++i) {
#if COM_COMPATIBLE
        const int native = kCanonicalToNative[i];
#else
        const int native = i;
#endif
        std::snprintf(&out[std::size_t(i) * 2], 3, "%02X",
                      static_cast<unsigned char>(cid[native]));
    }
    return out;
}

bool uidFromString(const std::string& text, char* cid) {
    if (text.size() != 32) return false;
    for (int i = 0; i < 16; ++i) {
        const std::string byte = text.substr(std::size_t(i) * 2, 2);
        char* end = nullptr;
        const long value = std::strtol(byte.c_str(), &end, 16);
        if (!end || *end != '\0') return false;
#if COM_COMPATIBLE
        cid[kCanonicalToNative[i]] = char(static_cast<unsigned char>(value));
#else
        cid[i] = char(static_cast<unsigned char>(value));
#endif
    }
    return true;
}

} // namespace vst3

std::vector<std::string> Vst3Factory::defaultSearchPaths() const {
    std::vector<std::string> paths;
    const std::string home = homeDirectory();
#if defined(__APPLE__)
    if (!home.empty()) paths.push_back(home + "/Library/Audio/Plug-Ins/VST3");
    paths.push_back("/Library/Audio/Plug-Ins/VST3");
#elif defined(_WIN32)
    const fs::path common =
        platform::knownFolderPath(platform::KnownFolder::CommonProgramFiles);
    if (!common.empty()) paths.push_back(platform::pathToUtf8(common / "VST3"));
    const fs::path local =
        platform::knownFolderPath(platform::KnownFolder::LocalAppData);
    if (!local.empty())
        paths.push_back(platform::pathToUtf8(local / "Programs" / "Common" /
                                             "VST3"));
#else
    if (!home.empty()) paths.push_back(home + "/.vst3");
    paths.push_back("/usr/lib/vst3");
#endif
    return paths;
}

std::vector<std::string> Vst3Factory::enumerateCandidates(
    const std::string& directory) const {
    std::vector<std::string> found;
    std::error_code ec;
    const fs::path root = platform::pathFromUtf8(directory);
    if (!fs::is_directory(root, ec)) return found;

    fs::recursive_directory_iterator it(
        root, fs::directory_options::skip_permission_denied, ec);
    if (ec) return found;
    for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const fs::path& path = it->path();
        if (path.extension() != ".vst3") continue;
        found.push_back(platform::pathToUtf8(path));
        // A `.vst3` bundle is a directory, and it contains its own binary —
        // descending would find nothing and cost the whole tree.
        if (it->is_directory(ec)) it.disable_recursion_pending();
    }
    return found;
}

std::vector<PluginDescriptor> Vst3Factory::inspect(const std::string& path) const {
    const bool diagnose = std::getenv("DAW_PLUGIN_DIAGNOSTICS") != nullptr;
    std::vector<PluginDescriptor> found;
    auto module = Vst3Module::open(path);
    if (!module) {
        if (diagnose)
            std::fprintf(stderr, "VST3: module '%s' would not open\n", path.c_str());
        return found;
    }

    IPluginFactory* factory = module->factory();
    if (diagnose)
        std::fprintf(stderr, "VST3: '%s' advertises %d classes\n", path.c_str(),
                     int(factory->countClasses()));
    PFactoryInfo factoryInfo{};
    const bool haveFactoryInfo = factory->getFactoryInfo(&factoryInfo) == kResultOk;

    const int32 count = factory->countClasses();
    for (int32 i = 0; i < count; ++i) {
        ClassEntry entry = describe(factory, i, path);
        if (!entry.valid) continue;
        // PClassInfo (the 1.0 struct) has no vendor; the factory's does.
        if (entry.descriptor.vendor.empty() && haveFactoryInfo) {
            entry.descriptor.vendor = factoryInfo.vendor;
        }
        found.push_back(std::move(entry.descriptor));
    }
    return found;
}

std::unique_ptr<PluginInstance> Vst3Factory::create(
    const PluginDescriptor& descriptor) {
    // Every failure here is silent to the user — the slot simply refuses to
    // fill — so each one says why under DAW_PLUGIN_DIAGNOSTICS. Finding out
    // *which* of these four steps a plugin dies on is the whole difference
    // between "this plugin is broken" and a host bug.
    const bool diagnose = std::getenv("DAW_PLUGIN_DIAGNOSTICS") != nullptr;
    auto module = Vst3Module::open(descriptor.path);
    if (!module) {
        if (diagnose)
            std::fprintf(stderr, "VST3 %s: module '%s' would not open\n",
                         descriptor.name.c_str(), descriptor.path.c_str());
        return nullptr;
    }

    TUID cid{};
    if (!vst3::uidFromString(descriptor.uid, cid)) {
        if (diagnose)
            std::fprintf(stderr, "VST3 %s: '%s' is not a class id\n",
                         descriptor.name.c_str(), descriptor.uid.c_str());
        return nullptr;
    }

    IPluginFactory* factory = module->factory();
    Vst::IComponent* component = nullptr;
    const tresult created = factory->createInstance(
        cid, Vst::IComponent::iid, reinterpret_cast<void**>(&component));
    if (created != kResultOk || !component) {
        if (diagnose)
            std::fprintf(stderr,
                         "VST3 %s: createInstance returned %d%s\n",
                         descriptor.name.c_str(), int(created),
                         component ? "" : " and no component");
        return nullptr;
    }

    auto instance = std::make_unique<Vst3Instance>(std::move(module), component,
                                                   descriptor);
    if (!instance->initialize(factory)) {
        if (diagnose)
            std::fprintf(stderr, "VST3 %s: initialize failed\n",
                         descriptor.name.c_str());
        return nullptr;
    }
    return instance;
}

} // namespace daw::plugins
