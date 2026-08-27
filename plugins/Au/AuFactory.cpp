#include "Au/AuFactory.hpp"

#include "Au/AuInstance.hpp"
#include "Vst3/Vst3Factory.hpp"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <unordered_map>

namespace daw::plugins {
namespace fs = std::filesystem;

namespace au {

std::string identityToString(std::uint32_t type, std::uint32_t subtype,
                             std::uint32_t manufacturer) {
    char text[32];
    std::snprintf(text, sizeof(text), "%08X:%08X:%08X", type, subtype, manufacturer);
    return text;
}

bool identityFromString(const std::string& text, std::uint32_t& type,
                        std::uint32_t& subtype, std::uint32_t& manufacturer) {
    unsigned int a = 0, b = 0, c = 0;
    if (std::sscanf(text.c_str(), "%8X:%8X:%8X", &a, &b, &c) != 3) return false;
    type = a;
    subtype = b;
    manufacturer = c;
    return true;
}

} // namespace au

namespace {

std::string homeDirectory() {
    const char* home = std::getenv("HOME");
    return home ? home : "";
}

/// A CFString as UTF-8, or empty.
std::string toUtf8(CFStringRef text) {
    if (!text) return {};
    const CFIndex length = CFStringGetLength(text);
    const CFIndex capacity = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    std::string out(std::size_t(capacity), '\0');
    if (!CFStringGetCString(text, out.data(), capacity, kCFStringEncodingUTF8)) return {};
    out.resize(std::strlen(out.c_str()));
    return out;
}

/// A four-character code out of whatever the plist holds. Apple's own tooling
/// writes these as strings, but some vendors write the numeric value, so both
/// have to be accepted.
bool readFourCC(CFDictionaryRef entry, CFStringRef key, std::uint32_t& out) {
    const void* raw = CFDictionaryGetValue(entry, key);
    if (!raw) return false;
    if (CFGetTypeID(raw) == CFStringGetTypeID()) {
        const std::string text = toUtf8(static_cast<CFStringRef>(raw));
        if (text.size() != 4) return false;
        out = (std::uint32_t(std::uint8_t(text[0])) << 24) |
              (std::uint32_t(std::uint8_t(text[1])) << 16) |
              (std::uint32_t(std::uint8_t(text[2])) << 8) |
              std::uint32_t(std::uint8_t(text[3]));
        return true;
    }
    if (CFGetTypeID(raw) == CFNumberGetTypeID()) {
        long long value = 0;
        if (!CFNumberGetValue(static_cast<CFNumberRef>(raw), kCFNumberLongLongType, &value)) {
            return false;
        }
        out = std::uint32_t(value);
        return true;
    }
    return false;
}

std::string readString(CFDictionaryRef entry, CFStringRef key) {
    const void* raw = CFDictionaryGetValue(entry, key);
    if (!raw || CFGetTypeID(raw) != CFStringGetTypeID()) return {};
    return toUtf8(static_cast<CFStringRef>(raw));
}

/// AU's `name` is conventionally "Manufacturer: Plugin". Split it, because a
/// browser grouped by vendor wants the two halves separately.
void splitName(const std::string& full, std::string& vendor, std::string& name) {
    const std::size_t colon = full.find(':');
    if (colon == std::string::npos) {
        name = full;
        return;
    }
    vendor = full.substr(0, colon);
    name = full.substr(colon + 1);
    while (!name.empty() && name.front() == ' ') name.erase(name.begin());
    while (!vendor.empty() && vendor.back() == ' ') vendor.pop_back();
}

std::string versionString(CFDictionaryRef entry) {
    const void* raw = CFDictionaryGetValue(entry, CFSTR("version"));
    if (!raw || CFGetTypeID(raw) != CFNumberGetTypeID()) return {};
    long long value = 0;
    CFNumberGetValue(static_cast<CFNumberRef>(raw), kCFNumberLongLongType, &value);
    // Packed as 0xMMMMmmbb — major, minor, bugfix.
    char text[32];
    std::snprintf(text, sizeof(text), "%lld.%lld.%lld", (value >> 16) & 0xFFFF,
                  (value >> 8) & 0xFF, value & 0xFF);
    return text;
}

/// Keep a component's code in memory for the life of the process.
///
/// The system decides when to unload an Audio Unit's bundle, and a host that
/// opens and closes hundreds of units from one shell — Waves ships one with
/// seven hundred in it — drives that load/unload cycle hard. Repeatedly
/// unloading a binary that registers Objective-C classes eventually hands a
/// plugin a stale class and it dies inside itself; the same failure the VST3
/// module cache exists to prevent. `dlopen` on an already-loaded image only
/// bumps a reference count, so this pins it without loading anything twice.
void* loadComponentBinary(const std::string& bundlePath) {
    static std::mutex* pinnedMutex = new std::mutex;
    static auto* pinned = new std::unordered_map<std::string, void*>;
    {
        std::lock_guard<std::mutex> lock(*pinnedMutex);
        const auto found = pinned->find(bundlePath);
        if (found != pinned->end()) return found->second;
    }

    std::error_code ec;
    const fs::path directory = fs::path(bundlePath) / "Contents" / "MacOS";
    if (!fs::is_directory(directory, ec)) return nullptr;
    fs::path binary = directory / fs::path(bundlePath).stem();
    if (!fs::is_regular_file(binary, ec)) {
        binary.clear();
        for (const auto& entry : fs::directory_iterator(directory, ec)) {
            if (entry.is_regular_file(ec)) { binary = entry.path(); break; }
        }
    }
    // Never closed, deliberately — that is the whole point.
    void* handle = binary.empty() ? nullptr
                                  : ::dlopen(binary.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (!handle && std::getenv("DAW_PLUGIN_DIAGNOSTICS")) {
        std::fprintf(stderr, "AU: dlopen failed for %s: %s\n", binary.c_str(),
                     ::dlerror());
    }
    {
        std::lock_guard<std::mutex> lock(*pinnedMutex);
        (*pinned)[bundlePath] = handle;
    }
    return handle;
}

void pinComponentBinary(const std::string& bundlePath) {
    (void)loadComponentBinary(bundlePath);
}

CFDictionaryRef copyInfoPlist(const std::string& bundlePath) {
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        nullptr, reinterpret_cast<const UInt8*>(bundlePath.c_str()),
        CFIndex(bundlePath.size()), true);
    if (!url) return nullptr;
    // `CFBundleCreate` rather than reading Info.plist off disk ourselves: it
    // handles the localised and binary-plist cases, and it does not load or run
    // any of the plugin's code.
    CFBundleRef bundle = CFBundleCreate(nullptr, url);
    CFRelease(url);
    if (!bundle) return nullptr;
    CFDictionaryRef info = CFBundleGetInfoDictionary(bundle);
    CFDictionaryRef copy = info ? CFDictionaryCreateCopy(nullptr, info) : nullptr;
    CFRelease(bundle);
    return copy;
}

/// The system AudioComponent registrar can be stale even though the bundle is
/// valid (an especially common state after an in-place instrument update).
/// AUv2 explicitly supports process-local registration, so fall back to the
/// bundle's declared factory instead of leaving a cached plugin unopenable.
AudioComponent registerComponentFromBundle(const std::string& bundlePath,
                                            const AudioComponentDescription& wanted) {
    void* binary = loadComponentBinary(bundlePath);
    if (!binary) return nullptr;

    CFDictionaryRef info = copyInfoPlist(bundlePath);
    if (!info) return nullptr;
    const void* rawList = CFDictionaryGetValue(info, CFSTR("AudioComponents"));
    if (!rawList || CFGetTypeID(rawList) != CFArrayGetTypeID()) {
        CFRelease(info);
        return nullptr;
    }

    AudioComponent registered = nullptr;
    auto* list = static_cast<CFArrayRef>(rawList);
    for (CFIndex i = 0; i < CFArrayGetCount(list) && !registered; ++i) {
        const void* rawEntry = CFArrayGetValueAtIndex(list, i);
        if (!rawEntry || CFGetTypeID(rawEntry) != CFDictionaryGetTypeID()) continue;
        auto* entry = static_cast<CFDictionaryRef>(rawEntry);
        std::uint32_t type = 0, subtype = 0, manufacturer = 0;
        if (!readFourCC(entry, CFSTR("type"), type) ||
            !readFourCC(entry, CFSTR("subtype"), subtype) ||
            !readFourCC(entry, CFSTR("manufacturer"), manufacturer) ||
            type != wanted.componentType || subtype != wanted.componentSubType ||
            manufacturer != wanted.componentManufacturer) {
            continue;
        }

        const std::string factoryName = readString(entry, CFSTR("factoryFunction"));
        if (factoryName.empty()) continue;
        auto factory = reinterpret_cast<AudioComponentFactoryFunction>(
            ::dlsym(binary, factoryName.c_str()));
        if (!factory) {
            if (std::getenv("DAW_PLUGIN_DIAGNOSTICS")) {
                std::fprintf(stderr, "AU: factory %s missing in %s\n",
                             factoryName.c_str(), bundlePath.c_str());
            }
            continue;
        }

        UInt32 version = 0;
        const void* rawVersion = CFDictionaryGetValue(entry, CFSTR("version"));
        if (rawVersion && CFGetTypeID(rawVersion) == CFNumberGetTypeID()) {
            CFNumberGetValue(static_cast<CFNumberRef>(rawVersion), kCFNumberSInt32Type,
                             &version);
        }
        CFStringRef name = nullptr;
        const void* rawName = CFDictionaryGetValue(entry, CFSTR("name"));
        if (rawName && CFGetTypeID(rawName) == CFStringGetTypeID()) {
            name = static_cast<CFStringRef>(rawName);
        }
        CFStringRef fallbackName = nullptr;
        if (!name) {
            fallbackName = CFStringCreateWithCString(nullptr, bundlePath.c_str(),
                                                      kCFStringEncodingUTF8);
            name = fallbackName;
        }
        AudioComponentDescription registration = wanted;
        registration.componentFlags |= kAudioComponentFlag_SandboxSafe;
        registered = AudioComponentRegister(&registration, name, version, factory);
        if (!registered && std::getenv("DAW_PLUGIN_DIAGNOSTICS")) {
            std::fprintf(stderr, "AU: process-local registration failed for %s\n",
                         bundlePath.c_str());
        }
        if (fallbackName) CFRelease(fallbackName);
    }
    CFRelease(info);
    return registered;
}

AudioComponent findOrRegisterComponent(const std::string& bundlePath,
                                       const AudioComponentDescription& wanted) {
    if (AudioComponent component = AudioComponentFindNext(nullptr, &wanted)) {
        return component;
    }
    return registerComponentFromBundle(bundlePath, wanted);
}

std::unique_ptr<PluginInstance> createEmbeddedVst3Fallback(
    const PluginDescriptor& auDescriptor) {
    // Several vendors build their AUv2 as a thin wrapper around this exact
    // embedded VST3. It is a useful last resort when macOS has a stale AU
    // registrar entry, or when the wrapper opens but exposes neither its Cocoa
    // view nor parameters. Never substitute an unrelated system-wide plugin.
    const fs::path embedded = fs::path(auDescriptor.path) / "Contents" /
                              "Resources" / "plugin.vst3";
    std::error_code ec;
    if (!fs::is_directory(embedded, ec)) return nullptr;

    Vst3Factory vst3;
    const std::vector<PluginDescriptor> candidates = vst3.inspect(embedded.string());
    const PluginDescriptor* chosen = nullptr;
    for (const PluginDescriptor& candidate : candidates) {
        if (candidate.isInstrument != auDescriptor.isInstrument) continue;
        if (candidate.name == auDescriptor.name) {
            chosen = &candidate;
            break;
        }
        if (!chosen && candidate.vendor == auDescriptor.vendor) chosen = &candidate;
    }
    if (!chosen) return nullptr;
    auto instance = vst3.create(*chosen);
    if (instance && std::getenv("DAW_PLUGIN_DIAGNOSTICS")) {
        std::fprintf(stderr, "AU: falling back to the embedded VST3 for %s\n",
                     auDescriptor.name.c_str());
    }
    return instance;
}

} // namespace

std::vector<std::string> AuFactory::defaultSearchPaths() const {
    std::vector<std::string> paths;
    const std::string home = homeDirectory();
    if (!home.empty()) paths.push_back(home + "/Library/Audio/Plug-Ins/Components");
    paths.push_back("/Library/Audio/Plug-Ins/Components");
    return paths;
}

std::vector<std::string> AuFactory::enumerateCandidates(
    const std::string& directory) const {
    std::vector<std::string> found;
    std::error_code ec;
    if (!fs::is_directory(directory, ec)) return found;

    fs::recursive_directory_iterator it(
        directory, fs::directory_options::skip_permission_denied, ec);
    if (ec) return found;
    for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const fs::path& path = it->path();
        if (path.extension() != ".component") continue;
        found.push_back(path.string());
        if (it->is_directory(ec)) it.disable_recursion_pending();
    }
    return found;
}

std::vector<PluginDescriptor> AuFactory::inspect(const std::string& path) const {
    std::vector<PluginDescriptor> found;

    // Everything a browser needs is declared in the bundle's Info.plist, so the
    // metadata costs no code execution. The component is still opened below to
    // prove it can be — that is the whole point of scanning out of process.
    CFDictionaryRef info = copyInfoPlist(path);
    if (!info) return found;
    const void* rawList = CFDictionaryGetValue(info, CFSTR("AudioComponents"));
    if (!rawList || CFGetTypeID(rawList) != CFArrayGetTypeID()) {
        CFRelease(info);
        return found;
    }

    auto* list = static_cast<CFArrayRef>(rawList);
    const CFIndex count = CFArrayGetCount(list);
    constexpr int kSmokeTestLimit = 8;
    int opened = 0;
    for (CFIndex i = 0; i < count; ++i) {
        const void* rawEntry = CFArrayGetValueAtIndex(list, i);
        if (!rawEntry || CFGetTypeID(rawEntry) != CFDictionaryGetTypeID()) continue;
        auto* entry = static_cast<CFDictionaryRef>(rawEntry);

        std::uint32_t type = 0, subtype = 0, manufacturer = 0;
        if (!readFourCC(entry, CFSTR("type"), type) ||
            !readFourCC(entry, CFSTR("subtype"), subtype) ||
            !readFourCC(entry, CFSTR("manufacturer"), manufacturer)) {
            continue;
        }
        // Only the kinds this host can route. `auol` is offline-only and
        // `aumx` is a mixer; neither belongs in an insert slot.
        if (type != kAudioUnitType_Effect && type != kAudioUnitType_MusicEffect &&
            type != kAudioUnitType_MusicDevice && type != kAudioUnitType_Generator) {
            continue;
        }

        PluginDescriptor descriptor;
        descriptor.format = Format::AudioUnit;
        descriptor.uid = au::identityToString(type, subtype, manufacturer);
        descriptor.path = path;
        splitName(readString(entry, CFSTR("name")), descriptor.vendor, descriptor.name);
        if (descriptor.name.empty()) descriptor.name = descriptor.uid;
        if (descriptor.vendor.empty()) {
            descriptor.vendor = readString(entry, CFSTR("manufacturerName"));
        }
        descriptor.version = versionString(entry);
        descriptor.isInstrument = type == kAudioUnitType_MusicDevice ||
                                  type == kAudioUnitType_Generator;
        descriptor.wantsMidi = type == kAudioUnitType_MusicDevice ||
                               type == kAudioUnitType_MusicEffect;
        descriptor.category = descriptor.isInstrument ? "Instrument" : "Audio Effect";

        std::error_code ec;
        descriptor.fileSize = 0;   // a bundle is a directory
        const auto written = fs::last_write_time(path, ec);
        descriptor.fileModifiedTime = ec ? 0 : written.time_since_epoch().count();

        // Registration is checked for every component — it is a lookup, not a
        // load — but only the first few are actually opened.
        //
        // Opening one costs a few hundred milliseconds, and a shell like Waves
        // declares seven hundred of them in a single bundle: validating all of
        // them would take minutes and hit the scanner's deadline, which would
        // blacklist a perfectly good shell. A broken shell fails on its first
        // component anyway, so the smoke test is where the value is; past that
        // the plist is taken at its word.
        AudioComponentDescription wanted{};
        wanted.componentType = type;
        wanted.componentSubType = subtype;
        wanted.componentManufacturer = manufacturer;
        AudioComponent component = findOrRegisterComponent(path, wanted);
        if (!component) {
            auto fallback = createEmbeddedVst3Fallback(descriptor);
            if (!fallback) continue;
            descriptor.hasEditor = fallback->hasEditor();
            found.push_back(std::move(descriptor));
            continue;
        }
        if (opened < kSmokeTestLimit) {
            AudioComponentInstance instance = nullptr;
            if (AudioComponentInstanceNew(component, &instance) != noErr || !instance) {
                continue;
            }
            AudioComponentInstanceDispose(instance);
            ++opened;
        }

        found.push_back(std::move(descriptor));
    }
    CFRelease(info);
    return found;
}

std::unique_ptr<PluginInstance> AuFactory::create(const PluginDescriptor& descriptor) {
    std::uint32_t type = 0, subtype = 0, manufacturer = 0;
    if (!au::identityFromString(descriptor.uid, type, subtype, manufacturer)) {
        return nullptr;
    }

    // The embedded VST3 is a fallback, never the first choice. Preferring it
    // for every AU instrument was tried and reverted: it silently replaced
    // working AUs with a module that came up with no parameters and no editor.
    // Pigments 7 was the plugin it was meant to rescue, and its AU is in fact
    // healthy — probed directly, it publishes 2301 parameters, advertises
    // CocoaUI, and its view (SMTGAUPluginCocoaView…) constructs at 1280x802.
    // The genuine failures are all caught below, after the AU has been given
    // its chance.
    pinComponentBinary(descriptor.path);

    AudioComponentDescription wanted{};
    wanted.componentType = type;
    wanted.componentSubType = subtype;
    wanted.componentManufacturer = manufacturer;
    AudioComponent component = findOrRegisterComponent(descriptor.path, wanted);
    if (!component) return createEmbeddedVst3Fallback(descriptor);

    AudioComponentInstance unit = nullptr;
    if (AudioComponentInstanceNew(component, &unit) != noErr || !unit) {
        return createEmbeddedVst3Fallback(descriptor);
    }

    auto instance = std::make_unique<AuInstance>(unit, descriptor);
    if (!instance->initialize()) return createEmbeddedVst3Fallback(descriptor);
    if (descriptor.isInstrument && instance->parameters().empty() &&
        !instance->hasEditor()) {
        return createEmbeddedVst3Fallback(descriptor);
    }
    return instance;
}

} // namespace daw::plugins
