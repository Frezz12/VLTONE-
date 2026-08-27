#include "Scan/ScanProtocol.hpp"

#include <nlohmann/json.hpp>

namespace daw::plugins::scan {
namespace {

using nlohmann::json;

json toJson(const PluginDescriptor& descriptor) {
    return json{
        {"format", std::string(toString(descriptor.format))},
        {"uid", descriptor.uid},
        {"path", descriptor.path},
        {"name", descriptor.name},
        {"vendor", descriptor.vendor},
        {"version", descriptor.version},
        {"category", descriptor.category},
        {"isInstrument", descriptor.isInstrument},
        {"hasEditor", descriptor.hasEditor},
        {"wantsMidi", descriptor.wantsMidi},
        {"inputChannels", descriptor.mainInputChannels},
        {"outputChannels", descriptor.mainOutputChannels},
        {"fileSize", descriptor.fileSize},
        {"fileModifiedTime", descriptor.fileModifiedTime},
    };
}

PluginDescriptor fromJson(const json& value) {
    PluginDescriptor descriptor;
    // Every field is read with a default, so adding one later stays readable by
    // an older build and vice versa.
    descriptor.format = formatFromString(value.value("format", std::string()));
    descriptor.uid = value.value("uid", std::string());
    descriptor.path = value.value("path", std::string());
    descriptor.name = value.value("name", std::string());
    descriptor.vendor = value.value("vendor", std::string());
    descriptor.version = value.value("version", std::string());
    descriptor.category = value.value("category", std::string());
    descriptor.isInstrument = value.value("isInstrument", false);
    descriptor.hasEditor = value.value("hasEditor", false);
    descriptor.wantsMidi = value.value("wantsMidi", false);
    descriptor.mainInputChannels = value.value("inputChannels", std::uint16_t(2));
    descriptor.mainOutputChannels = value.value("outputChannels", std::uint16_t(2));
    descriptor.fileSize = value.value("fileSize", std::uint64_t(0));
    descriptor.fileModifiedTime = value.value("fileModifiedTime", std::int64_t(0));
    return descriptor;
}

} // namespace

std::string descriptorToJson(const PluginDescriptor& descriptor) {
    return toJson(descriptor).dump();
}

std::string encodeResult(const std::vector<PluginDescriptor>& plugins) {
    json array = json::array();
    for (const PluginDescriptor& descriptor : plugins) array.push_back(toJson(descriptor));
    // Compact, not pretty: this goes down a pipe, and nobody reads it by eye.
    return json{{"schema", kSchemaVersion}, {"plugins", array}}.dump();
}

bool decodeResult(const std::string& text, std::vector<PluginDescriptor>& out) {
    out.clear();
    json root;
    try {
        root = json::parse(text);
    } catch (const std::exception&) {
        // Third-party modules occasionally print diagnostics to stdout during
        // static initialisation. The scanner's framed object is emitted last;
        // recover it without accepting arbitrary partial JSON.
        const std::size_t payload = text.rfind("{\"schema\"");
        if (payload == std::string::npos) return false;
        try {
            root = json::parse(text.substr(payload));
        } catch (const std::exception&) {
            return false;
        }
    }
    if (!root.is_object()) return false;
    if (root.value("schema", -1) != kSchemaVersion) return false;
    if (!root.contains("plugins") || !root["plugins"].is_array()) return false;

    for (const json& entry : root["plugins"]) {
        if (!entry.is_object()) continue;
        PluginDescriptor descriptor = fromJson(entry);
        // A descriptor with no identity is unusable, and silently keeping one
        // would put an un-loadable row in the browser.
        if (descriptor.format == Format::Unknown || descriptor.uid.empty()) continue;
        out.push_back(std::move(descriptor));
    }
    return true;
}

} // namespace daw::plugins::scan
