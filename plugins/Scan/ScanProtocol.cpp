#include "Scan/ScanProtocol.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <stdexcept>

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
        {"producesMidi", descriptor.producesMidi},
        {"inputChannels", descriptor.mainInputChannels},
        {"outputChannels", descriptor.mainOutputChannels},
        {"fileSize", descriptor.fileSize},
        {"fileModifiedTime", descriptor.fileModifiedTime},
        {"stateSchemaVersion", descriptor.stateSchemaVersion},
        {"parameterSchema", descriptor.parameterSchema},
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
    descriptor.producesMidi = value.value("producesMidi", false);
    const auto channels = [&](const char* key) {
        if (!value.contains(key)) return std::uint16_t(2);
        if (!value[key].is_number_integer()) throw std::runtime_error("invalid channel count");
        const auto count = value[key].get<std::int64_t>();
        if (count < 0 || count > 65535) throw std::runtime_error("invalid channel count");
        return std::uint16_t(count);
    };
    descriptor.mainInputChannels = channels("inputChannels");
    descriptor.mainOutputChannels = channels("outputChannels");
    descriptor.fileSize = value.value("fileSize", std::uint64_t(0));
    descriptor.fileModifiedTime = value.value("fileModifiedTime", std::int64_t(0));
    descriptor.stateSchemaVersion = value.value("stateSchemaVersion", 0);
    descriptor.parameterSchema = value.value("parameterSchema", std::string{});
    return descriptor;
}

} // namespace

std::string descriptorToJson(const PluginDescriptor& descriptor) {
    return toJson(descriptor).dump();
}

bool descriptorFromJson(const std::string& text, PluginDescriptor& out) {
    try {
        const auto value = json::parse(text);
        if (!value.is_object()) return false;
        auto descriptor = fromJson(value);
        if (descriptor.format == Format::Unknown || descriptor.uid.empty()) return false;
        out = std::move(descriptor);
        return true;
    } catch (const std::exception&) { return false; }
}

std::string encodeHandshake() {
    return json{{"schema", kSchemaVersion}, {"protocol", kProtocolVersion},
                {"capabilities", {"discover", "validate-descriptor"}},
                {"plugins", json::array()}}.dump();
}

bool decodeHandshake(const std::string& text) {
    try {
        const auto root = json::parse(text);
        if (!root.is_object() || root.value("schema", -1) != kSchemaVersion ||
            root.value("protocol", -1) != kProtocolVersion ||
            !root.contains("capabilities") || !root["capabilities"].is_array()) return false;
        const auto& capabilities = root["capabilities"];
        return std::find(capabilities.begin(), capabilities.end(), "discover") != capabilities.end() &&
               std::find(capabilities.begin(), capabilities.end(), "validate-descriptor") != capabilities.end();
    } catch (const std::exception&) { return false; }
}

std::string encodeResult(const std::vector<PluginDescriptor>& plugins) {
    json array = json::array();
    for (const PluginDescriptor& descriptor : plugins) array.push_back(toJson(descriptor));
    // Compact, not pretty: this goes down a pipe, and nobody reads it by eye.
    return json{{"schema", kSchemaVersion}, {"plugins", array}}.dump();
}

bool decodeResult(const std::string& text, std::vector<PluginDescriptor>& out, bool strict) {
    out.clear();
    json root;
    try {
        root = json::parse(text);
    } catch (const std::exception&) {
        if (strict) return false;
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
    try {
        if (!root.is_object()) return false;
        if (root.value("schema", -1) != kSchemaVersion) return false;
        if (!root.contains("plugins") || !root["plugins"].is_array()) return false;

        for (const json& entry : root["plugins"]) {
            if (!entry.is_object()) { out.clear(); return false; }
            if (strict) {
                for (const char* key : {"format", "uid", "path", "name", "vendor", "version",
                     "category", "isInstrument", "hasEditor", "wantsMidi", "producesMidi",
                     "inputChannels", "outputChannels", "stateSchemaVersion", "parameterSchema"}) {
                    if (!entry.contains(key)) { out.clear(); return false; }
                }
            }
            PluginDescriptor descriptor = fromJson(entry);
            // A descriptor with no identity is unusable, and silently keeping one
            // would put an un-loadable row in the browser.
            if (descriptor.format == Format::Unknown || descriptor.uid.empty()) {
                out.clear(); return false;
            }
            out.push_back(std::move(descriptor));
        }
        return true;
    } catch (const std::exception&) { out.clear(); return false; }
}

} // namespace daw::plugins::scan
