#include "serialization/InsertJson.hpp"
#include "serialization/AssetJson.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>

namespace daw::serialization {

using json = nlohmann::json;

json parametersToJson(const std::vector<InsertParameter>& values) {
    json out = json::array();
    for (const InsertParameter& value : values) {
        if (!value.id.empty() && std::isfinite(value.value))
            out.push_back(json{{"id", value.id}, {"value", value.value}});
    }
    return out;
}

std::vector<InsertParameter> parametersFromJson(const json& parent,
                                                const char* key) {
    std::vector<InsertParameter> out;
    if (!parent.contains(key) || !parent.at(key).is_array()) return out;
    for (const json& value : parent.at(key)) {
        if (!value.is_object() || !value.contains("id") ||
            !value.at("id").is_string() || !value.contains("value") ||
            !value.at("value").is_number()) {
            continue;
        }
        InsertParameter parameter{value.at("id").get<std::string>(),
                                  value.at("value").get<double>()};
        if (!parameter.id.empty() && std::isfinite(parameter.value))
            out.push_back(std::move(parameter));
    }
    return out;
}

json insertToJson(const InsertModel& i) {
    json j{{"id", i.id}, {"name", i.name}, {"bypassed", i.bypassed}};
    // A free slot is written in exactly the shape every pre-hosting project
    // already has, so older project readers still see a harmless empty slot.
    if (i.format == PluginFormat::None) return j;

    j["format"] = toString(i.format);
    j["uid"] = i.uid;
    j["path"] = i.path;
    j["vendor"] = i.vendor;
    if (!i.pluginVersion.empty()) j["pluginVersion"] = i.pluginVersion;
    if (i.stateSchemaVersion > 0)
        j["stateSchemaVersion"] = i.stateSchemaVersion;
    j["mix"] = i.mix;
    if (i.channelMode != PluginChannelMode::Auto)
        j["channelMode"] = toString(i.channelMode);
    if (i.editorChannel != PluginEditorChannel::Left)
        j["editorChannel"] = toString(i.editorChannel);
    if (!i.sidechainTrackId.empty()) j["sidechain"] = i.sidechainTrackId;
    if (!i.stateFile.empty()) j["stateFile"] = i.stateFile;
    if (!i.rightStateFile.empty()) j["rightStateFile"] = i.rightStateFile;
    if (!i.stateAsset.empty()) j["stateAsset"] = assetRefToJson(i.stateAsset);
    if (!i.rightStateAsset.empty())
        j["rightStateAsset"] = assetRefToJson(i.rightStateAsset);
    if (!i.assetBindings.empty())
        j["assetBindings"] = pluginAssetBindingsToJson(i.assetBindings);
    if (json parameters = parametersToJson(i.parameters); !parameters.empty()) {
        j["parameters"] = std::move(parameters);
    }
    if (json parameters = parametersToJson(i.rightParameters);
        !parameters.empty()) {
        j["rightParameters"] = std::move(parameters);
    }
    if (i.windowWidth > 0) {
        j["window"] = json{{"x", i.windowX},
                           {"y", i.windowY},
                           {"w", i.windowWidth},
                           {"h", i.windowHeight},
                           {"open", i.windowOpen}};
    }
    return j;
}

InsertModel insertFromJson(const json& j) {
    InsertModel i;
    i.id = j.value("id", newUuid());
    i.name = j.value("name", "");
    i.bypassed = j.value("bypassed", false);

    i.format = pluginFormatFromString(j.value("format", std::string()));
    i.uid = j.value("uid", "");
    i.path = j.value("path", "");
    i.vendor = j.value("vendor", "");
    i.pluginVersion = j.value("pluginVersion", "");
    i.stateSchemaVersion = std::max(0, j.value("stateSchemaVersion", 0));
    i.mix = j.value("mix", 1.0f);
    i.channelMode = pluginChannelModeFromString(
        j.value("channelMode", std::string("auto")));
    i.editorChannel = pluginEditorChannelFromString(
        j.value("editorChannel", std::string("left")));
    i.sidechainTrackId = j.value("sidechain", "");
    i.stateFile = j.value("stateFile", "");
    i.rightStateFile = j.value("rightStateFile", "");
    if (j.contains("stateAsset"))
        i.stateAsset = assetRefFromJson(j.at("stateAsset"));
    if (j.contains("rightStateAsset"))
        i.rightStateAsset = assetRefFromJson(j.at("rightStateAsset"));
    i.assetBindings = pluginAssetBindingsFromJson(j, "assetBindings");
    i.parameters = parametersFromJson(j, "parameters");
    i.rightParameters = parametersFromJson(j, "rightParameters");
    if (j.contains("window") && j.at("window").is_object()) {
        const auto& w = j.at("window");
        i.windowX = w.value("x", 0);
        i.windowY = w.value("y", 0);
        i.windowWidth = w.value("w", 0);
        i.windowHeight = w.value("h", 0);
        i.windowOpen = w.value("open", false);
    }
    return i;
}

json insertsToJson(const std::vector<InsertModel>& inserts) {
    json out = json::array();
    for (const auto& insert : inserts) out.push_back(insertToJson(insert));
    return out;
}

std::vector<InsertModel> insertsFromJson(const json& parent, const char* key) {
    std::vector<InsertModel> out;
    if (!parent.contains(key) || !parent.at(key).is_array()) return out;
    for (const auto& entry : parent.at(key)) out.push_back(insertFromJson(entry));
    return out;
}

} // namespace daw::serialization
