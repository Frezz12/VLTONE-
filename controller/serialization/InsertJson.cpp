#include "serialization/InsertJson.hpp"

#include <nlohmann/json.hpp>

namespace daw::serialization {

using json = nlohmann::json;

json insertToJson(const InsertModel& i) {
    json j{{"id", i.id}, {"name", i.name}, {"bypassed", i.bypassed}};
    // A free slot is written in exactly the shape every pre-hosting project
    // already has, so older project readers still see a harmless empty slot.
    if (i.format == PluginFormat::None) return j;

    j["format"] = toString(i.format);
    j["uid"] = i.uid;
    j["path"] = i.path;
    j["vendor"] = i.vendor;
    j["mix"] = i.mix;
    if (i.channelMode != PluginChannelMode::Auto)
        j["channelMode"] = toString(i.channelMode);
    if (i.editorChannel != PluginEditorChannel::Left)
        j["editorChannel"] = toString(i.editorChannel);
    if (!i.sidechainTrackId.empty()) j["sidechain"] = i.sidechainTrackId;
    if (!i.stateFile.empty()) j["stateFile"] = i.stateFile;
    if (!i.rightStateFile.empty()) j["rightStateFile"] = i.rightStateFile;
    if (!i.parameters.empty()) {
        json parameters = json::array();
        for (const InsertParameter& p : i.parameters)
            parameters.push_back(json{{"id", p.id}, {"value", p.value}});
        j["parameters"] = std::move(parameters);
    }
    if (!i.rightParameters.empty()) {
        json parameters = json::array();
        for (const InsertParameter& p : i.rightParameters)
            parameters.push_back(json{{"id", p.id}, {"value", p.value}});
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
    i.mix = j.value("mix", 1.0f);
    i.channelMode = pluginChannelModeFromString(
        j.value("channelMode", std::string("auto")));
    i.editorChannel = pluginEditorChannelFromString(
        j.value("editorChannel", std::string("left")));
    i.sidechainTrackId = j.value("sidechain", "");
    i.stateFile = j.value("stateFile", "");
    i.rightStateFile = j.value("rightStateFile", "");
    if (j.contains("parameters") && j.at("parameters").is_array()) {
        for (const auto& jp : j.at("parameters")) {
            InsertParameter p;
            p.id = jp.value("id", "");
            p.value = jp.value("value", 0.0);
            if (!p.id.empty()) i.parameters.push_back(std::move(p));
        }
    }
    if (j.contains("rightParameters") && j.at("rightParameters").is_array()) {
        for (const auto& jp : j.at("rightParameters")) {
            InsertParameter p;
            p.id = jp.value("id", "");
            p.value = jp.value("value", 0.0);
            if (!p.id.empty()) i.rightParameters.push_back(std::move(p));
        }
    }
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
