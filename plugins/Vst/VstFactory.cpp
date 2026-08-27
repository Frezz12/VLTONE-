#include "Vst/VstFactory.hpp"

#include "Vst/VstInstance.hpp"
#include "Vst/VstModule.hpp"
#include "platform/KnownFolders.hpp"
#include "platform/PathUtils.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string_view>

namespace daw::plugins {
namespace fs = std::filesystem;

namespace {

struct ScanHost {};

VstIntPtr scanHostDispatch(vst::HostContext&, AEffect*, VstInt32 opcode,
                           VstInt32, VstIntPtr, void* ptr, float) noexcept {
    auto writeName = [](void* output, const char* name) {
        if (output) std::memcpy(output, name, std::strlen(name) + 1);
    };
    switch (opcode) {
        case audioMasterVersion: return kVstVersion;
        case audioMasterCurrentId: return 0; // VstModule returns host.currentId below.
        case audioMasterWantMidi: return 1;
        case audioMasterGetSampleRate: return 48000;
        case audioMasterGetBlockSize: return 512;
        case audioMasterGetCurrentProcessLevel: return kVstProcessLevelRealtime;
        case audioMasterGetVendorVersion: return 1;
        case audioMasterGetVendorString:
            writeName(ptr, "VLT Studio");
            return ptr ? 1 : 0;
        case audioMasterGetProductString:
            writeName(ptr, "VLT Studio Pro");
            return ptr ? 1 : 0;
        case audioMasterCanDo:
            if (!ptr) return 0;
            if (std::strcmp(static_cast<const char*>(ptr), "receiveVstEvents") == 0 ||
                std::strcmp(static_cast<const char*>(ptr), "receiveVstMidiEvent") == 0 ||
                std::strcmp(static_cast<const char*>(ptr), "sendVstEvents") == 0 ||
                std::strcmp(static_cast<const char*>(ptr), "sendVstMidiEvent") == 0 ||
                std::strcmp(static_cast<const char*>(ptr), "sendVstTimeInfo") == 0 ||
                std::strcmp(static_cast<const char*>(ptr), "sizeWindow") == 0) {
                return 1;
            }
            return 0;
        default: return 0;
    }
}

// audioMasterCurrentId is special: shells query it during the module entry
// point, before an AEffect exists. Keep it outside the generic scanner switch.
VstIntPtr shellScanHostDispatch(vst::HostContext& host, AEffect* effect,
                                VstInt32 opcode, VstInt32 index,
                                VstIntPtr value, void* ptr, float opt) noexcept {
    if (opcode == audioMasterCurrentId) return host.currentId;
    return scanHostDispatch(host, effect, opcode, index, value, ptr, opt);
}

std::string trimPluginString(const char* text, std::size_t capacity) {
    if (!text || capacity == 0) return {};
    std::size_t length = 0;
    while (length < capacity && text[length] != '\0') ++length;
    std::string out(text, length);
    while (!out.empty() && (out.back() == '\0' ||
                            std::isspace(static_cast<unsigned char>(out.back())))) {
        out.pop_back();
    }
    return out;
}

std::string hexUid(VstInt32 value) {
    char text[9]{};
    std::snprintf(text, sizeof(text), "%08X",
                  static_cast<unsigned>(static_cast<std::uint32_t>(value)));
    return text;
}

std::string categoryName(VstIntPtr category, bool instrument) {
    switch (category) {
        case kPlugCategSynth: return "Instrument";
        case kPlugCategAnalysis: return "Analyzer";
        case kPlugCategMastering: return "Mastering";
        case kPlugCategSpacializer: return "Spatial";
        case kPlugCategRoomFx: return "Room Fx";
        case kPlugSurroundFx: return "Surround Fx";
        case kPlugCategRestoration: return "Restoration";
        case kPlugCategOfflineProcess: return "Offline";
        case kPlugCategGenerator: return "Generator";
        default: return instrument ? "Instrument" : "Fx";
    }
}

PluginDescriptor describe(AEffect* effect, const std::string& path,
                          std::string_view shellName) {
    PluginDescriptor descriptor;
    descriptor.format = Format::Vst;
    descriptor.path = path;

    std::array<char, 256> text{};
    effect->dispatcher(effect, effGetEffectName, 0, 0, text.data(), 0.0f);
    descriptor.name = trimPluginString(text.data(), text.size());
    if (descriptor.name.empty()) descriptor.name = std::string(shellName);
    if (descriptor.name.empty()) {
        descriptor.name = platform::pathToUtf8(
            platform::pathFromUtf8(path).stem());
    }

    text.fill(0);
    effect->dispatcher(effect, effGetVendorString, 0, 0, text.data(), 0.0f);
    descriptor.vendor = trimPluginString(text.data(), text.size());

    const VstIntPtr vendorVersion =
        effect->dispatcher(effect, effGetVendorVersion, 0, 0, nullptr, 0.0f);
    const VstIntPtr vstVersion =
        effect->dispatcher(effect, effGetVstVersion, 0, 0, nullptr, 0.0f);
    if (vendorVersion > 0) descriptor.version = std::to_string(vendorVersion);
    else if (effect->version > 0) descriptor.version = std::to_string(effect->version);
    else if (vstVersion > 0) descriptor.version = std::to_string(vstVersion);

    const VstIntPtr category =
        effect->dispatcher(effect, effGetPlugCategory, 0, 0, nullptr, 0.0f);
    descriptor.isInstrument =
        (effect->flags & effFlagsIsSynth) != 0 || category == kPlugCategSynth;
    descriptor.category = categoryName(category, descriptor.isInstrument);
    descriptor.hasEditor = (effect->flags & effFlagsHasEditor) != 0;
    descriptor.wantsMidi = descriptor.isInstrument ||
        effect->dispatcher(effect, effCanDo, 0, 0,
                           const_cast<char*>("receiveVstMidiEvent"), 0.0f) > 0;
    descriptor.mainInputChannels = static_cast<std::uint16_t>(std::clamp<VstInt32>(
        effect->numInputs, 0, std::numeric_limits<std::uint16_t>::max()));
    descriptor.mainOutputChannels = static_cast<std::uint16_t>(std::clamp<VstInt32>(
        effect->numOutputs, 0, std::numeric_limits<std::uint16_t>::max()));

    descriptor.uid = effect->uniqueID != 0
                         ? hexUid(effect->uniqueID)
                         : "00000000:" + descriptor.vendor + ":" + descriptor.name;

    std::error_code ec;
    const fs::path native = platform::pathFromUtf8(path);
    descriptor.fileSize = fs::is_regular_file(native, ec)
                              ? fs::file_size(native, ec)
                              : 0;
    ec.clear();
    const auto modified = fs::last_write_time(native, ec);
    descriptor.fileModifiedTime = ec ? 0 : modified.time_since_epoch().count();
    return descriptor;
}

bool parseUid(std::string_view uid, VstInt32& value) {
    if (uid.size() != 8 &&
        !(uid.size() > 9 && uid.starts_with("00000000:"))) {
        return false;
    }
    std::uint32_t parsed = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        const unsigned char c = static_cast<unsigned char>(uid[i]);
        parsed <<= 4;
        if (c >= '0' && c <= '9') parsed |= c - '0';
        else if (c >= 'A' && c <= 'F') parsed |= c - 'A' + 10;
        else if (c >= 'a' && c <= 'f') parsed |= c - 'a' + 10;
        else return false;
    }
    value = std::bit_cast<VstInt32>(parsed);
    return true;
}

bool hasExtension(const fs::path& path, std::string_view wanted) {
    std::string extension = platform::pathToUtf8(path.extension());
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return extension == wanted;
}

} // namespace

std::vector<std::string> VstFactory::defaultSearchPaths() const {
    std::vector<std::string> paths;
#if defined(_WIN32)
    const fs::path common =
        platform::knownFolderPath(platform::KnownFolder::CommonProgramFiles);
    const fs::path programFiles = common.parent_path();
    if (!programFiles.empty()) {
        paths.push_back(platform::pathToUtf8(programFiles / "VstPlugins"));
        paths.push_back(platform::pathToUtf8(
            programFiles / "Steinberg" / "VstPlugins"));
    }
    if (!common.empty())
        paths.push_back(platform::pathToUtf8(common / "VST2"));
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"))
        paths.emplace_back(std::string(home) + "/Library/Audio/Plug-Ins/VST");
    paths.emplace_back("/Library/Audio/Plug-Ins/VST");
#endif
    return paths;
}

std::vector<std::string> VstFactory::enumerateCandidates(
    const std::string& directory) const {
    std::vector<std::string> found;
    std::error_code ec;
    const fs::path root = platform::pathFromUtf8(directory);
    if (!fs::is_directory(root, ec)) return found;

    fs::recursive_directory_iterator it(
        root, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const fs::path path = it->path();
#if defined(_WIN32)
        if (it->is_regular_file(ec) && hasExtension(path, ".dll"))
            found.push_back(platform::pathToUtf8(path));
#elif defined(__APPLE__)
        if (it->is_directory(ec) && hasExtension(path, ".vst")) {
            found.push_back(platform::pathToUtf8(path));
            it.disable_recursion_pending();
        }
#endif
    }
    return found;
}

std::vector<PluginDescriptor> VstFactory::inspect(
    const std::string& path) const {
    std::vector<PluginDescriptor> descriptors;
    auto module = VstModule::open(path);
    if (!module) return descriptors;

    ScanHost scanner;
    vst::HostContext rootHost{&scanner, &shellScanHostDispatch, 0};
    AEffect* root = module->create(rootHost);
    if (!root) return descriptors;
    root->dispatcher(root, effOpen, 0, 0, nullptr, 0.0f);
    const VstIntPtr category =
        root->dispatcher(root, effGetPlugCategory, 0, 0, nullptr, 0.0f);

    if (category != kPlugCategShell) {
        descriptors.push_back(describe(root, path, {}));
        root->dispatcher(root, effClose, 0, 0, nullptr, 0.0f);
        return descriptors;
    }

    struct ShellEntry { VstInt32 id; std::string name; };
    std::vector<ShellEntry> children;
    for (;;) {
        std::array<char, 256> name{};
        const VstIntPtr id = root->dispatcher(
            root, effShellGetNextPlugin, 0, 0, name.data(), 0.0f);
        if (id == 0) break;
        children.push_back({static_cast<VstInt32>(id),
                            trimPluginString(name.data(), name.size())});
    }
    root->dispatcher(root, effClose, 0, 0, nullptr, 0.0f);

    for (const ShellEntry& child : children) {
        vst::HostContext childHost{&scanner, &shellScanHostDispatch, child.id};
        AEffect* effect = module->create(childHost);
        if (!effect) continue;
        effect->dispatcher(effect, effOpen, 0, 0, nullptr, 0.0f);
        const VstIntPtr childCategory = effect->dispatcher(
            effect, effGetPlugCategory, 0, 0, nullptr, 0.0f);
        if (childCategory == kPlugCategShell ||
            (effect->uniqueID != 0 && effect->uniqueID != child.id)) {
            effect->dispatcher(effect, effClose, 0, 0, nullptr, 0.0f);
            continue;
        }
        PluginDescriptor descriptor = describe(effect, path, child.name);
        // A shell component is addressed by the id returned by the shell even
        // if the child forgot to copy it into AEffect::uniqueID.
        descriptor.uid = hexUid(child.id);
        descriptors.push_back(std::move(descriptor));
        effect->dispatcher(effect, effClose, 0, 0, nullptr, 0.0f);
    }
    return descriptors;
}

std::unique_ptr<PluginInstance> VstFactory::create(
    const PluginDescriptor& descriptor) {
    VstInt32 uid = 0;
    if (!parseUid(descriptor.uid, uid)) return nullptr;
    auto module = VstModule::open(descriptor.path);
    if (!module) return nullptr;
    return VstInstance::create(std::move(module), descriptor, uid);
}

} // namespace daw::plugins
