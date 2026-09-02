#include "collaboration/CommandJson.hpp"
#include "serialization/AssetJson.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <type_traits>

namespace daw::collab {
namespace {

using json = nlohmann::json;

bool hasExactKeys(const json& value,
                  std::initializer_list<const char*> keys) {
    if (!value.is_object() || value.size() != keys.size()) return false;
    return std::all_of(keys.begin(), keys.end(),
                       [&](const char* key) { return value.contains(key); });
}

json scalarToJson(const ScalarValue& value) {
    return std::visit([](const auto& item) { return json(item); }, value);
}

bool scalarFromJson(const json& value, ScalarValue& out) {
    if (value.is_string()) out = value.get<std::string>();
    else if (value.is_boolean()) out = value.get<bool>();
    else if (value.is_number_integer()) out = value.get<std::int64_t>();
    else if (value.is_number()) out = value.get<double>();
    else return false;
    return true;
}

json noteToJson(const NoteModel& note) {
    return json{{"id", note.id},
                {"pitch", note.pitch},
                {"startBeats", note.startBeats},
                {"lengthBeats", note.lengthBeats},
                {"velocity", note.velocity},
                {"muted", note.muted},
                {"color", note.color},
                {"pan", note.pan}};
}

bool noteFromJson(const json& value, NoteModel& note) {
    if (!value.is_object()) return false;
    note.id = value.value("id", std::string());
    note.pitch = value.value("pitch", 60);
    note.startBeats = value.value("startBeats", 0.0);
    note.lengthBeats = value.value("lengthBeats", 1.0);
    note.velocity = value.value("velocity", 100);
    note.muted = value.value("muted", false);
    note.color = value.value("color", std::uint32_t(0));
    note.pan = value.value("pan", 0.0f);
    return true;
}

json automationPointToJson(const AutomationPoint& point) {
    return json{{"id", point.id},
                {"beats", point.beats},
                {"value", point.value},
                {"shape", toString(point.shape)},
                {"curve", point.curve}};
}

bool automationPointFromJson(const json& value, AutomationPoint& point) {
    if (!value.is_object()) return false;
    const std::string shape = value.value("shape", std::string());
    if (shape != "linear" && shape != "hold" && shape != "scurve")
        return false;
    point.id = value.value("id", std::string());
    point.beats = value.value("beats", 0.0);
    point.value = value.value("value", 0.0);
    point.shape = automationSegmentFromString(shape);
    point.curve = value.value("curve", 0.0);
    return true;
}

json controllerLaneTargetToJson(const ControllerLaneTarget& target) {
    return json{{"cc", target.cc},
                {"parameterId", target.parameterId},
                {"slotId", target.slotId}};
}

bool controllerLaneTargetFromJson(const json& value,
                                  ControllerLaneTarget& target) {
    if (!value.is_object() || value.size() != 3) return false;
    target.cc = value.value("cc", -2);
    target.parameterId = value.value("parameterId", std::string());
    target.slotId = value.value("slotId", std::string());
    return value.contains("cc") && value.contains("parameterId") &&
           value.contains("slotId");
}

json automationTargetToJson(const AutomationTarget& target) {
    return json{{"kind", toString(target.kind)},
                {"channelId", target.channelId},
                {"slotId", target.slotId},
                {"parameterId", target.parameterId},
                {"sendId", target.sendId}};
}

bool automationTargetFromJson(const json& value, AutomationTarget& target) {
    if (!value.is_object() || value.size() != 5) return false;
    const std::string kind = value.value("kind", std::string());
    if (kind != "volume" && kind != "pan" && kind != "mute" &&
        kind != "send" && kind != "parameter") {
        return false;
    }
    target.kind = automationTargetKindFromString(kind);
    target.channelId = value.value("channelId", std::string());
    target.slotId = value.value("slotId", std::string());
    target.parameterId = value.value("parameterId", std::string());
    target.sendId = value.value("sendId", std::string());
    return value.contains("channelId") && value.contains("slotId") &&
           value.contains("parameterId") && value.contains("sendId");
}

json takeToJson(const TakeModel& take) {
    return json{{"id", take.id},
                {"name", take.name},
                {"offsetSeconds", take.offsetSeconds},
                {"lengthSeconds", take.lengthSeconds},
                {"clipOffsetSeconds", take.clipOffsetSeconds},
                {"gain", take.gain},
                {"muted", take.muted},
                {"channels", take.channels},
                {"color", take.color},
                {"asset", serialization::assetRefToJson(take.asset)}};
}

bool takeFromJson(const json& value, TakeModel& take) {
    if (!hasExactKeys(value,
                      {"id", "name", "offsetSeconds", "lengthSeconds",
                       "clipOffsetSeconds", "gain", "muted", "channels",
                       "color", "asset"}) ||
        !value.contains("asset") || !value.at("asset").is_object()) {
        return false;
    }
    const json& asset = value.at("asset");
    const bool assetShape =
        (hasExactKeys(asset, {"assetId", "sha256", "kind", "byteSize",
                              "originalName"}) ||
         hasExactKeys(asset, {"assetId", "sha256", "kind", "byteSize",
                              "originalName", "audioMetadata"}));
    if (!assetShape || asset.value("kind", std::string()) != "audio")
        return false;
    if (asset.contains("audioMetadata") &&
        !hasExactKeys(asset.at("audioMetadata"),
                      {"mimeType", "codec", "sampleRate", "channels",
                       "frames"})) {
        // AssetJson omits individual empty metadata fields, so accept any
        // subset but reject unknown keys.
        if (!asset.at("audioMetadata").is_object()) return false;
        static constexpr const char* allowed[] = {
            "mimeType", "codec", "sampleRate", "channels", "frames"};
        for (const auto& [key, ignored] : asset.at("audioMetadata").items()) {
            if (std::find(std::begin(allowed), std::end(allowed), key) ==
                std::end(allowed)) {
                return false;
            }
        }
    }
    take.id = value.value("id", std::string());
    take.name = value.value("name", std::string());
    take.offsetSeconds = value.value("offsetSeconds", 0.0);
    take.lengthSeconds = value.value("lengthSeconds", 0.0);
    take.clipOffsetSeconds = value.value("clipOffsetSeconds", 0.0);
    take.gain = value.value("gain", 1.0f);
    take.muted = value.value("muted", false);
    take.channels = value.value("channels", 0);
    take.color = value.value("color", std::uint32_t(0x4A90D9));
    take.asset = serialization::assetRefFromJson(value.at("asset"));
    return value.contains("id") && value.contains("name") &&
           value.contains("offsetSeconds") &&
           value.contains("lengthSeconds") &&
           value.contains("clipOffsetSeconds") && value.contains("gain") &&
           value.contains("muted") && value.contains("channels") &&
           value.contains("color");
}

json compSegmentToJson(const CompSegment& segment) {
    return json{{"id", segment.id},
                {"takeId", segment.takeId},
                {"startSeconds", segment.startSeconds},
                {"endSeconds", segment.endSeconds}};
}

bool compSegmentFromJson(const json& value, CompSegment& segment) {
    if (!value.is_object() || value.size() != 4) return false;
    segment.id = value.value("id", std::string());
    segment.takeId = value.value("takeId", std::string());
    segment.startSeconds = value.value("startSeconds", 0.0);
    segment.endSeconds = value.value("endSeconds", 0.0);
    return value.contains("id") && value.contains("takeId") &&
           value.contains("startSeconds") && value.contains("endSeconds");
}

json pluginLocationToJson(const PluginLocation& location) {
    return json{{"chain", pluginChainName(location.chain)},
                {"trackId", location.trackId},
                {"clipId", location.clipId}};
}

bool pluginLocationFromJson(const json& value, PluginLocation& location) {
    if (!hasExactKeys(value, {"chain", "trackId", "clipId"})) return false;
    if (!pluginChainFromName(value.value("chain", std::string()),
                             location.chain)) {
        return false;
    }
    location.trackId = value.value("trackId", std::string());
    location.clipId = value.value("clipId", std::string());
    return true;
}

json sendToJson(const SendModel& send) {
    return json{{"id", send.id},
                {"destinationTrackId", send.destinationTrackId},
                {"level", send.level},
                {"preFader", send.preFader},
                {"enabled", send.enabled}};
}

bool sendFromJson(const json& value, SendModel& send) {
    if (!hasExactKeys(value, {"id", "destinationTrackId", "level",
                              "preFader", "enabled"}) ||
        !value.at("id").is_string() ||
        !value.at("destinationTrackId").is_string() ||
        !value.at("level").is_number() ||
        !value.at("preFader").is_boolean() ||
        !value.at("enabled").is_boolean()) {
        return false;
    }
    send.id = value.at("id").get<std::string>();
    send.destinationTrackId = value.at("destinationTrackId").get<std::string>();
    send.level = value.at("level").get<float>();
    send.preFader = value.at("preFader").get<bool>();
    send.enabled = value.at("enabled").get<bool>();
    return true;
}

json sampleEditToJson(const ClipSampleEditModel& edit) {
    return json{{"loopMode", edit.loopMode},
                {"loopStart", edit.loopStart},
                {"loopEnd", edit.loopEnd},
                {"stretchMode", static_cast<int>(edit.stretchMode)},
                {"stretchTime", edit.stretchTime},
                {"stretchPitch", edit.stretchPitch},
                {"formant", edit.formant},
                {"rootNote", edit.rootNote},
                {"boost", edit.boost},
                {"eqLow", edit.eqLow},
                {"eqMid", edit.eqMid},
                {"eqHigh", edit.eqHigh},
                {"ringMix", edit.ringMix},
                {"ringFreq", edit.ringFreq},
                {"cut", edit.cut},
                {"res", edit.res},
                {"reverbType", edit.reverbType},
                {"reverb", edit.reverb},
                {"stereoDelay", edit.stereoDelay},
                {"pogo", edit.pogo},
                {"removeDc", edit.removeDc},
                {"reversePolarity", edit.reversePolarity},
                {"normalize", edit.normalize},
                {"fadeStereo", edit.fadeStereo},
                {"reverse", edit.reverse},
                {"swapStereo", edit.swapStereo}};
}

bool sampleEditFromJson(const json& value, ClipSampleEditModel& edit) {
    if (!hasExactKeys(value,
                      {"loopMode", "loopStart", "loopEnd", "stretchMode",
                       "stretchTime", "stretchPitch", "formant", "rootNote",
                       "boost", "eqLow", "eqMid", "eqHigh", "ringMix",
                       "ringFreq", "cut", "res", "reverbType", "reverb",
                       "stereoDelay", "pogo", "removeDc", "reversePolarity",
                       "normalize", "fadeStereo", "reverse", "swapStereo"})) {
        return false;
    }
	static constexpr const char* integer[] = {
	    "loopMode", "stretchMode", "rootNote", "reverbType"};
	for (const char* key : integer) {
	    if (!value.at(key).is_number_integer()) return false;
	}
	static constexpr const char* numeric[] = {
	    "loopStart", "loopEnd", "stretchTime", "stretchPitch", "formant",
	    "boost", "eqLow", "eqMid", "eqHigh", "ringMix", "ringFreq", "cut",
	    "res", "reverb", "stereoDelay", "pogo"};
    for (const char* key : numeric) {
        if (!value.at(key).is_number()) return false;
    }
    static constexpr const char* boolean[] = {
        "removeDc", "reversePolarity", "normalize", "fadeStereo", "reverse",
        "swapStereo"};
    for (const char* key : boolean) {
        if (!value.at(key).is_boolean()) return false;
    }
    edit.loopMode = value.at("loopMode").get<int>();
    edit.loopStart = value.at("loopStart").get<double>();
    edit.loopEnd = value.at("loopEnd").get<double>();
    edit.stretchMode = static_cast<ClipStretchMode>(
        value.at("stretchMode").get<int>());
    edit.stretchTime = value.at("stretchTime").get<double>();
    edit.stretchPitch = value.at("stretchPitch").get<double>();
    edit.formant = value.at("formant").get<double>();
    edit.rootNote = value.at("rootNote").get<int>();
    edit.boost = value.at("boost").get<double>();
    edit.eqLow = value.at("eqLow").get<double>();
    edit.eqMid = value.at("eqMid").get<double>();
    edit.eqHigh = value.at("eqHigh").get<double>();
    edit.ringMix = value.at("ringMix").get<double>();
    edit.ringFreq = value.at("ringFreq").get<double>();
    edit.cut = value.at("cut").get<double>();
    edit.res = value.at("res").get<double>();
    edit.reverbType = value.at("reverbType").get<int>();
    edit.reverb = value.at("reverb").get<double>();
    edit.stereoDelay = value.at("stereoDelay").get<double>();
    edit.pogo = value.at("pogo").get<double>();
    edit.removeDc = value.at("removeDc").get<bool>();
    edit.reversePolarity = value.at("reversePolarity").get<bool>();
    edit.normalize = value.at("normalize").get<bool>();
    edit.fadeStereo = value.at("fadeStereo").get<bool>();
    edit.reverse = value.at("reverse").get<bool>();
    edit.swapStereo = value.at("swapStereo").get<bool>();
    return true;
}

json parameterToJson(const InsertParameter& parameter) {
    return json{{"id", parameter.id}, {"value", parameter.value}};
}

bool parameterFromJson(const json& value, InsertParameter& parameter) {
    if (!hasExactKeys(value, {"id", "value"}) ||
        !value.at("id").is_string() || !value.at("value").is_number()) {
        return false;
    }
    parameter.id = value.at("id").get<std::string>();
    parameter.value = value.at("value").get<double>();
    return true;
}

json parametersToJson(const std::vector<InsertParameter>& parameters) {
    json out = json::array();
    for (const InsertParameter& parameter : parameters)
        out.push_back(parameterToJson(parameter));
    return out;
}

bool parametersFromJson(const json& value,
                        std::vector<InsertParameter>& parameters) {
    if (!value.is_array() || value.size() > 16384) return false;
    parameters.clear();
    parameters.reserve(value.size());
    for (const json& item : value) {
        InsertParameter parameter;
        if (!parameterFromJson(item, parameter)) return false;
        parameters.push_back(std::move(parameter));
    }
    return true;
}

json bindingToJson(const PluginAssetBinding& binding) {
    return json{{"key", binding.key},
                {"asset", serialization::assetRefToJson(binding.asset)},
                {"required", binding.required}};
}

bool assetFromJson(const json& value, AssetRef& asset, bool allowEmpty) {
    if (allowEmpty && value.is_null()) {
        asset = {};
        return true;
    }
    if (!value.is_object()) return false;
    if (!hasExactKeys(value, {"assetId", "sha256", "kind", "byteSize",
                              "originalName"}) &&
        !hasExactKeys(value, {"assetId", "sha256", "kind", "byteSize",
                              "originalName", "audioMetadata"})) {
        return false;
    }
	if (!value.at("assetId").is_string() || !value.at("sha256").is_string() ||
	    !value.at("kind").is_string() ||
	    !value.at("byteSize").is_number_unsigned() ||
	    !value.at("originalName").is_string()) {
	    return false;
	}
	if (value.contains("audioMetadata")) {
	    const json& metadata = value.at("audioMetadata");
	    if (!metadata.is_object()) return false;
	    for (auto item = metadata.begin(); item != metadata.end(); ++item) {
	        const std::string& key = item.key();
	        if (key != "mimeType" && key != "codec" && key != "sampleRate" &&
	            key != "channels" && key != "frames") {
	            return false;
	        }
	    }
	    if ((metadata.contains("mimeType") &&
	         !metadata.at("mimeType").is_string()) ||
	        (metadata.contains("codec") &&
	         !metadata.at("codec").is_string()) ||
	        (metadata.contains("sampleRate") &&
	         !metadata.at("sampleRate").is_number()) ||
	        (metadata.contains("channels") &&
	         !metadata.at("channels").is_number_unsigned()) ||
	        (metadata.contains("frames") &&
	         !metadata.at("frames").is_number_unsigned())) {
	        return false;
	    }
	}
    asset = serialization::assetRefFromJson(value);
    return true;
}

bool bindingFromJson(const json& value, PluginAssetBinding& binding) {
    if (!hasExactKeys(value, {"key", "asset", "required"}) ||
        !value.at("key").is_string() ||
        !value.at("required").is_boolean() ||
        !assetFromJson(value.at("asset"), binding.asset, false)) {
        return false;
    }
    binding.key = value.at("key").get<std::string>();
    binding.required = value.at("required").get<bool>();
    return true;
}

json bindingsToJson(const std::vector<PluginAssetBinding>& bindings) {
    json out = json::array();
    for (const PluginAssetBinding& binding : bindings)
        out.push_back(bindingToJson(binding));
    return out;
}

bool bindingsFromJson(const json& value,
                      std::vector<PluginAssetBinding>& bindings) {
    if (!value.is_array() || value.size() > 1024) return false;
    bindings.clear();
    bindings.reserve(value.size());
    for (const json& item : value) {
        PluginAssetBinding binding;
        if (!bindingFromJson(item, binding)) return false;
        bindings.push_back(std::move(binding));
    }
    return true;
}

json sharedInsertToJson(const InsertModel& insert) {
    return json{{"id", insert.id},
                {"name", insert.name},
                {"bypassed", insert.bypassed},
                {"format", toString(insert.format)},
                {"uid", insert.uid},
                {"vendor", insert.vendor},
                {"pluginVersion", insert.pluginVersion},
                {"stateSchemaVersion", insert.stateSchemaVersion},
                {"mix", insert.mix},
                {"channelMode", toString(insert.channelMode)},
                {"sidechainTrackId", insert.sidechainTrackId},
                {"stateAsset", serialization::assetRefToJson(insert.stateAsset)},
                {"rightStateAsset",
                 serialization::assetRefToJson(insert.rightStateAsset)},
                {"parameters", parametersToJson(insert.parameters)},
                {"rightParameters", parametersToJson(insert.rightParameters)},
                {"assetBindings", bindingsToJson(insert.assetBindings)}};
}

bool sharedInsertFromJson(const json& value, InsertModel& insert) {
    if (!hasExactKeys(value,
                      {"id", "name", "bypassed", "format", "uid", "vendor",
                       "pluginVersion", "stateSchemaVersion", "mix",
                       "channelMode", "sidechainTrackId", "stateAsset",
                       "rightStateAsset", "parameters", "rightParameters",
                       "assetBindings"}) ||
        !value.at("id").is_string() || !value.at("name").is_string() ||
        !value.at("bypassed").is_boolean() ||
        !value.at("format").is_string() || !value.at("uid").is_string() ||
        !value.at("vendor").is_string() ||
        !value.at("pluginVersion").is_string() ||
        !value.at("stateSchemaVersion").is_number_integer() ||
        !value.at("mix").is_number() ||
        !value.at("channelMode").is_string() ||
        !value.at("sidechainTrackId").is_string()) {
        return false;
    }
    const std::string format = value.at("format").get<std::string>();
    const std::string channelMode = value.at("channelMode").get<std::string>();
    if (format != "internal" ||
        (channelMode != "auto" && channelMode != "mono" &&
         channelMode != "stereo" && channelMode != "dual-mono")) {
        return false;
    }
    insert = {};
    insert.id = value.at("id").get<std::string>();
    insert.name = value.at("name").get<std::string>();
    insert.bypassed = value.at("bypassed").get<bool>();
    insert.format = pluginFormatFromString(format);
    insert.uid = value.at("uid").get<std::string>();
    insert.vendor = value.at("vendor").get<std::string>();
    insert.pluginVersion = value.at("pluginVersion").get<std::string>();
    insert.stateSchemaVersion = value.at("stateSchemaVersion").get<int>();
    insert.mix = value.at("mix").get<float>();
    insert.channelMode = pluginChannelModeFromString(channelMode);
    insert.sidechainTrackId = value.at("sidechainTrackId").get<std::string>();
    return assetFromJson(value.at("stateAsset"), insert.stateAsset, true) &&
           assetFromJson(value.at("rightStateAsset"), insert.rightStateAsset,
                         true) &&
           parametersFromJson(value.at("parameters"), insert.parameters) &&
           parametersFromJson(value.at("rightParameters"),
                              insert.rightParameters) &&
           bindingsFromJson(value.at("assetBindings"), insert.assetBindings);
}

json conditionToJson(const CommandCondition& condition) {
    return json{{"kind", "fieldWriterIs"},
                {"fieldKey", condition.fieldKey},
                {"operationId", condition.operationId}};
}

bool conditionFromJson(const json& value, CommandCondition& out) {
    if (!value.is_object()) return false;
    const std::string kind = value.value("kind", std::string());
    if (kind == "fieldWriterIs") {
        FieldWriterIs condition;
        condition.fieldKey = value.value("fieldKey", std::string());
        condition.operationId = value.value("operationId", std::string());
        if (condition.fieldKey.empty() || condition.operationId.empty()) return false;
        out = std::move(condition);
        return true;
    }
    return false;
}

CommandMeta metaFromJson(const json& value) {
    CommandMeta meta;
    meta.schemaVersion = value.value("schemaVersion", std::uint32_t(0));
    meta.operationId = value.value("opId", std::string());
    meta.baseServerSequence =
        value.value("baseServerSeq", std::uint64_t(0));
    meta.transactionId = value.value("transactionId", std::string());
    return meta;
}

json bodyToJson(const ProjectCommand& command) {
    return std::visit([](const auto& body) -> json {
        using T = std::decay_t<decltype(body)>;
        if constexpr (std::is_same_v<T, SetProjectScalar>) {
            return json{{"field", projectScalarName(body.field)},
                        {"value", scalarToJson(body.value)}};
        } else if constexpr (std::is_same_v<T, SetTimeSignature>) {
            return json{{"numerator", body.numerator},
                        {"denominator", body.denominator}};
        } else if constexpr (std::is_same_v<T, SetProjectKey>) {
            return json{{"root", body.root}, {"scale", body.scale}};
        } else if constexpr (std::is_same_v<T, AddTrack>) {
            return json{{"trackId", body.trackId},
                        {"trackKind", toString(body.kind)},
                        {"name", body.name},
                        {"color", body.color},
                        {"parentId", body.parentId},
                        {"afterId", body.afterId}};
        } else if constexpr (std::is_same_v<T, DeleteTrack>) {
            return json{{"trackId", body.trackId}};
        } else if constexpr (std::is_same_v<T, RestoreTrack>) {
            return json{{"trackId", body.trackId},
                        {"deleteOperationId", body.deleteOperationId}};
        } else if constexpr (std::is_same_v<T, MoveTrack>) {
            return json{{"trackId", body.trackId}, {"afterId", body.afterId}};
        } else if constexpr (std::is_same_v<T, SetTrackProperty>) {
            return json{{"trackId", body.trackId},
                        {"property", trackPropertyName(body.property)},
                        {"value", scalarToJson(body.value)}};
        } else if constexpr (std::is_same_v<T, SetTrackParent>) {
            return json{{"trackId", body.trackId},
                        {"parentId", body.parentId}};
        } else if constexpr (std::is_same_v<T, SetTrackOutput>) {
            return json{{"trackId", body.trackId},
                        {"outputTrackId", body.outputTrackId}};
        } else if constexpr (std::is_same_v<T, AddSend>) {
            return json{{"trackId", body.trackId},
                        {"send", sendToJson(body.send)},
                        {"afterId", body.afterId}};
        } else if constexpr (std::is_same_v<T, DeleteSend>) {
            return json{{"trackId", body.trackId}, {"sendId", body.sendId}};
        } else if constexpr (std::is_same_v<T, RestoreSend>) {
            return json{{"trackId", body.trackId},
                        {"sendId", body.sendId},
                        {"deleteOperationId", body.deleteOperationId}};
        } else if constexpr (std::is_same_v<T, MoveSend>) {
            return json{{"trackId", body.trackId},
                        {"sendId", body.sendId},
                        {"afterId", body.afterId}};
        } else if constexpr (std::is_same_v<T, SetSendProperty>) {
            return json{{"trackId", body.trackId},
                        {"sendId", body.sendId},
                        {"property", sendPropertyName(body.property)},
                        {"value", scalarToJson(body.value)}};
        } else if constexpr (std::is_same_v<T, AddClip>) {
            return json{{"trackId", body.trackId},
                        {"clipId", body.clipId},
                        {"clipKind", toString(body.kind)},
                        {"name", body.name},
                        {"startSeconds", body.startSeconds},
                        {"durationSeconds", body.durationSeconds},
                        {"color", body.color},
                        {"afterId", body.afterId}};
        } else if constexpr (std::is_same_v<T, DeleteClip>) {
            return json{{"trackId", body.trackId}, {"clipId", body.clipId}};
        } else if constexpr (std::is_same_v<T, RestoreClip>) {
            return json{{"trackId", body.trackId},
                        {"clipId", body.clipId},
                        {"deleteOperationId", body.deleteOperationId}};
        } else if constexpr (std::is_same_v<T, MoveClip>) {
            return json{{"clipId", body.clipId},
                        {"trackId", body.trackId},
                        {"afterId", body.afterId}};
        } else if constexpr (std::is_same_v<T, SetClipProperty>) {
            return json{{"trackId", body.trackId},
                        {"clipId", body.clipId},
                        {"property", clipPropertyName(body.property)},
                        {"value", scalarToJson(body.value)}};
        } else if constexpr (std::is_same_v<T, SetClipAsset>) {
            return json{{"trackId", body.trackId},
                        {"clipId", body.clipId},
                        {"asset", serialization::assetRefToJson(body.asset)}};
        } else if constexpr (std::is_same_v<T, SetClipSampleEdit>) {
            return json{{"trackId", body.trackId},
                        {"clipId", body.clipId},
                        {"sampleEdit", sampleEditToJson(body.sampleEdit)}};
        } else if constexpr (std::is_same_v<T, AddPluginInsert>) {
            return json{{"location", pluginLocationToJson(body.location)},
                        {"insert", sharedInsertToJson(body.insert)},
                        {"afterId", body.afterId}};
        } else if constexpr (std::is_same_v<T, DeletePluginInsert>) {
            return json{{"location", pluginLocationToJson(body.location)},
                        {"insertId", body.insertId}};
        } else if constexpr (std::is_same_v<T, RestorePluginInsert>) {
            return json{{"location", pluginLocationToJson(body.location)},
                        {"insertId", body.insertId},
                        {"deleteOperationId", body.deleteOperationId}};
        } else if constexpr (std::is_same_v<T, MovePluginInsert>) {
            return json{{"location", pluginLocationToJson(body.location)},
                        {"insertId", body.insertId},
                        {"afterId", body.afterId}};
        } else if constexpr (std::is_same_v<T, SetPluginProperty>) {
            return json{{"location", pluginLocationToJson(body.location)},
                        {"insertId", body.insertId},
                        {"property", pluginPropertyName(body.property)},
                        {"value", scalarToJson(body.value)}};
        } else if constexpr (std::is_same_v<T, SetPluginState>) {
            return json{{"location", pluginLocationToJson(body.location)},
                        {"insertId", body.insertId},
                        {"pluginVersion", body.pluginVersion},
                        {"stateSchemaVersion", body.stateSchemaVersion},
                        {"stateAsset",
                         serialization::assetRefToJson(body.stateAsset)},
                        {"rightStateAsset",
                         serialization::assetRefToJson(body.rightStateAsset)},
                        {"parameters", parametersToJson(body.parameters)},
                        {"rightParameters",
                         parametersToJson(body.rightParameters)},
                        {"assetBindings", bindingsToJson(body.assetBindings)}};
        } else if constexpr (std::is_same_v<T, SetPluginParameter>) {
            return json{{"location", pluginLocationToJson(body.location)},
                        {"insertId", body.insertId},
                        {"parameterId", body.parameterId},
                        {"value", body.value},
                        {"rightChannel", body.rightChannel}};
        } else if constexpr (std::is_same_v<T, RemovePluginParameter>) {
            return json{{"location", pluginLocationToJson(body.location)},
                        {"insertId", body.insertId},
                        {"parameterId", body.parameterId},
                        {"rightChannel", body.rightChannel}};
        } else if constexpr (std::is_same_v<T, SetPluginAssetBinding>) {
            return json{{"location", pluginLocationToJson(body.location)},
                        {"insertId", body.insertId},
                        {"binding", bindingToJson(body.binding)}};
        } else if constexpr (std::is_same_v<T, RemovePluginAssetBinding>) {
            return json{{"location", pluginLocationToJson(body.location)},
                        {"insertId", body.insertId},
                        {"key", body.key}};
        } else if constexpr (std::is_same_v<T, UpsertMidiNote>) {
            return json{{"trackId", body.trackId},
                        {"clipId", body.clipId},
                        {"note", noteToJson(body.note)},
                        {"afterId", body.afterId}};
        } else if constexpr (std::is_same_v<T, DeleteMidiNote>) {
            return json{{"trackId", body.trackId},
                        {"clipId", body.clipId},
                        {"noteId", body.noteId}};
        } else if constexpr (std::is_same_v<T, RestoreMidiNote>) {
            return json{{"trackId", body.trackId},
                        {"clipId", body.clipId},
                        {"noteId", body.noteId},
                        {"deleteOperationId", body.deleteOperationId}};
        } else if constexpr (std::is_same_v<T, UpsertAutomationPoint>) {
            return json{{"trackId", body.trackId},
                        {"clipId", body.clipId},
                        {"laneId", body.laneId},
                        {"point", automationPointToJson(body.point)},
                        {"afterId", body.afterId}};
        } else if constexpr (std::is_same_v<T, DeleteAutomationPoint>) {
            return json{{"trackId", body.trackId},
                        {"clipId", body.clipId},
                        {"laneId", body.laneId},
                        {"pointId", body.pointId}};
        } else if constexpr (std::is_same_v<T, RestoreAutomationPoint>) {
            return json{{"trackId", body.trackId},
                        {"clipId", body.clipId},
                        {"laneId", body.laneId},
                        {"pointId", body.pointId},
                        {"deleteOperationId", body.deleteOperationId}};
        } else if constexpr (std::is_same_v<T, AddControllerLane>) {
            return json{{"trackId", body.trackId},
                        {"clipId", body.clipId},
                        {"laneId", body.laneId},
                        {"name", body.name},
                        {"target", controllerLaneTargetToJson(body.target)},
                        {"defaultValue", body.defaultValue},
                        {"afterId", body.afterId}};
        } else if constexpr (std::is_same_v<T, DeleteControllerLane>) {
            return json{{"trackId", body.trackId},
                        {"clipId", body.clipId},
                        {"laneId", body.laneId}};
        } else if constexpr (std::is_same_v<T, RestoreControllerLane>) {
            return json{{"trackId", body.trackId},
                        {"clipId", body.clipId},
                        {"laneId", body.laneId},
                        {"deleteOperationId", body.deleteOperationId}};
        } else if constexpr (std::is_same_v<T, SetControllerLaneTarget>) {
            return json{{"trackId", body.trackId},
                        {"clipId", body.clipId},
                        {"laneId", body.laneId},
                        {"target", controllerLaneTargetToJson(body.target)}};
        } else if constexpr (std::is_same_v<T, SetControllerLaneDefault>) {
            return json{{"trackId", body.trackId},
                        {"clipId", body.clipId},
                        {"laneId", body.laneId},
                        {"defaultValue", body.defaultValue}};
        } else if constexpr (std::is_same_v<T, SetAutomationTarget>) {
            return json{{"trackId", body.trackId},
                        {"clipId", body.clipId},
                        {"target", automationTargetToJson(body.target)}};
        } else if constexpr (std::is_same_v<T, SetAutomationDefault>) {
            return json{{"trackId", body.trackId},
                        {"clipId", body.clipId},
                        {"defaultValue", body.defaultValue}};
        } else if constexpr (std::is_same_v<T, SetAutomationActive>) {
            return json{{"trackId", body.trackId},
                        {"clipId", body.clipId},
                        {"active", body.active}};
        } else if constexpr (std::is_same_v<T, AddTake>) {
            return json{{"trackId", body.trackId},
                        {"clipId", body.clipId},
                        {"take", takeToJson(body.take)},
                        {"afterId", body.afterId}};
        } else if constexpr (std::is_same_v<T, DeleteTake>) {
            return json{{"trackId", body.trackId},
                        {"clipId", body.clipId},
                        {"takeId", body.takeId}};
        } else if constexpr (std::is_same_v<T, RestoreTake>) {
            return json{{"trackId", body.trackId},
                        {"clipId", body.clipId},
                        {"takeId", body.takeId},
                        {"deleteOperationId", body.deleteOperationId}};
        } else if constexpr (std::is_same_v<T, UpsertCompSegment>) {
            return json{{"trackId", body.trackId},
                        {"clipId", body.clipId},
                        {"segment", compSegmentToJson(body.segment)},
                        {"afterId", body.afterId}};
        } else if constexpr (std::is_same_v<T, DeleteCompSegment>) {
            return json{{"trackId", body.trackId},
                        {"clipId", body.clipId},
                        {"segmentId", body.segmentId}};
        } else if constexpr (std::is_same_v<T, RestoreCompSegment>) {
            return json{{"trackId", body.trackId},
                        {"clipId", body.clipId},
                        {"segmentId", body.segmentId},
                        {"deleteOperationId", body.deleteOperationId}};
        } else if constexpr (std::is_same_v<T, RecordingCommit>) {
            json leases = json::array();
            for (const RecordingLeaseClaim& lease : body.leases) {
                leases.push_back(
                    json{{"trackId", lease.trackId},
                         {"leaseId", lease.leaseId}});
            }
            json children = json::array();
            if (body.batch) {
                for (const ProjectCommand& child : body.batch->commands) {
                    const json encoded = projectCommandToJson(child);
                    children.push_back(json{
                        {"kind", encoded.at("kind")},
                        {"payload", encoded.at("payload")},
                        {"preconditions", encoded.at("preconditions")},
                    });
                }
            }
            return json{{"leases", std::move(leases)},
                        {"commands", std::move(children)}};
        } else if constexpr (std::is_same_v<
                                 T, std::shared_ptr<BatchCommand>>) {
            json children = json::array();
            if (body) {
                for (const ProjectCommand& child : body->commands) {
                    const json encoded = projectCommandToJson(child);
                    children.push_back(json{
                        {"kind", encoded.at("kind")},
                        {"payload", encoded.at("payload")},
                        {"preconditions", encoded.at("preconditions")},
                    });
                }
            }
            return json{{"commands", std::move(children)}};
        } else {
            static_assert(!sizeof(T), "unhandled ProjectCommand body");
        }
    }, command.body);
}

bool parseBody(const std::string& kind, const json& payload, CommandBody& out,
               std::string& error) {
    if (!payload.is_object()) {
        error = "payload must be an object";
        return false;
    }
    if (kind == "project.setScalar") {
        SetProjectScalar body;
        if (!projectScalarFromName(payload.value("field", std::string()),
                                   body.field) ||
            !payload.contains("value") ||
            !scalarFromJson(payload.at("value"), body.value)) {
            error = "invalid project scalar payload";
            return false;
        }
        out = std::move(body);
        return true;
    }
    if (kind == "project.setTimeSignature") {
        out = SetTimeSignature{payload.value("numerator", 0),
                               payload.value("denominator", 0)};
        return true;
    }
    if (kind == "project.setKey") {
        out = SetProjectKey{payload.value("root", 0),
                            payload.value("scale", std::string())};
        return true;
    }
    if (kind == "track.add") {
        AddTrack body;
        body.trackId = payload.value("trackId", std::string());
        body.kind = trackKindFromString(payload.value("trackKind", "audio"));
        body.name = payload.value("name", std::string());
        body.color = payload.value("color", std::uint32_t(0x4A90D9));
        body.parentId = payload.value("parentId", std::string());
        body.afterId = payload.value("afterId", std::string());
        out = std::move(body);
        return true;
    }
    if (kind == "track.delete") {
        out = DeleteTrack{payload.value("trackId", std::string())};
        return true;
    }
    if (kind == "track.restore") {
        out = RestoreTrack{payload.value("trackId", std::string()),
                           payload.value("deleteOperationId", std::string())};
        return true;
    }
    if (kind == "track.move") {
        out = MoveTrack{payload.value("trackId", std::string()),
                        payload.value("afterId", std::string())};
        return true;
    }
    if (kind == "track.setProperty") {
        SetTrackProperty body;
        body.trackId = payload.value("trackId", std::string());
        if (!trackPropertyFromName(payload.value("property", std::string()),
                                   body.property) ||
            !payload.contains("value") ||
            !scalarFromJson(payload.at("value"), body.value)) {
            error = "invalid track property payload";
            return false;
        }
        out = std::move(body);
        return true;
    }
    if (kind == "track.setParent") {
        if (!hasExactKeys(payload, {"trackId", "parentId"}) ||
            !payload.at("trackId").is_string() ||
            !payload.at("parentId").is_string()) {
            error = "invalid track parent payload";
            return false;
        }
        out = SetTrackParent{payload.at("trackId").get<std::string>(),
                             payload.at("parentId").get<std::string>()};
        return true;
    }
    if (kind == "track.setOutput") {
        if (!hasExactKeys(payload, {"trackId", "outputTrackId"}) ||
            !payload.at("trackId").is_string() ||
            !payload.at("outputTrackId").is_string()) {
            error = "invalid track output payload";
            return false;
        }
        out = SetTrackOutput{payload.at("trackId").get<std::string>(),
                             payload.at("outputTrackId").get<std::string>()};
        return true;
    }
    if (kind == "send.add") {
        if (!hasExactKeys(payload, {"trackId", "send", "afterId"}) ||
            !payload.at("trackId").is_string() ||
            !payload.at("afterId").is_string()) {
            error = "invalid send add payload";
            return false;
        }
        AddSend body;
        body.trackId = payload.at("trackId").get<std::string>();
        body.afterId = payload.at("afterId").get<std::string>();
        if (!sendFromJson(payload.at("send"), body.send)) {
            error = "invalid send value";
            return false;
        }
        out = std::move(body);
        return true;
    }
    if (kind == "send.delete") {
        if (!hasExactKeys(payload, {"trackId", "sendId"})) {
            error = "invalid send delete payload";
            return false;
        }
        out = DeleteSend{payload.value("trackId", std::string()),
                         payload.value("sendId", std::string())};
        return true;
    }
    if (kind == "send.restore") {
        if (!hasExactKeys(payload,
                          {"trackId", "sendId", "deleteOperationId"})) {
            error = "invalid send restore payload";
            return false;
        }
        out = RestoreSend{payload.value("trackId", std::string()),
                          payload.value("sendId", std::string()),
                          payload.value("deleteOperationId", std::string())};
        return true;
    }
    if (kind == "send.move") {
        if (!hasExactKeys(payload, {"trackId", "sendId", "afterId"})) {
            error = "invalid send move payload";
            return false;
        }
        out = MoveSend{payload.value("trackId", std::string()),
                       payload.value("sendId", std::string()),
                       payload.value("afterId", std::string())};
        return true;
    }
    if (kind == "send.setProperty") {
        if (!hasExactKeys(payload,
                          {"trackId", "sendId", "property", "value"})) {
            error = "invalid send property payload";
            return false;
        }
        SetSendProperty body;
        body.trackId = payload.value("trackId", std::string());
        body.sendId = payload.value("sendId", std::string());
        if (!sendPropertyFromName(payload.value("property", std::string()),
                                  body.property) ||
            !scalarFromJson(payload.at("value"), body.value)) {
            error = "invalid send property payload";
            return false;
        }
        out = std::move(body);
        return true;
    }
    if (kind == "clip.add") {
        const std::string clipKind = payload.value("clipKind", std::string());
        if (clipKind != "audio" && clipKind != "midi" &&
            clipKind != "pattern" && clipKind != "automation") {
            error = "invalid clip kind";
            return false;
        }
        AddClip body;
        body.trackId = payload.value("trackId", std::string());
        body.clipId = payload.value("clipId", std::string());
        body.kind = clipKindFromString(clipKind);
        body.name = payload.value("name", std::string());
        body.startSeconds = payload.value("startSeconds", 0.0);
        body.durationSeconds = payload.value("durationSeconds", 0.0);
        body.color = payload.value("color", std::uint32_t(0x4A90D9));
        body.afterId = payload.value("afterId", std::string());
        out = std::move(body);
        return true;
    }
    if (kind == "clip.delete") {
        out = DeleteClip{payload.value("trackId", std::string()),
                         payload.value("clipId", std::string())};
        return true;
    }
    if (kind == "clip.restore") {
        out = RestoreClip{payload.value("trackId", std::string()),
                          payload.value("clipId", std::string()),
                          payload.value("deleteOperationId", std::string())};
        return true;
    }
    if (kind == "clip.move") {
        out = MoveClip{payload.value("clipId", std::string()),
                       payload.value("trackId", std::string()),
                       payload.value("afterId", std::string())};
        return true;
    }
    if (kind == "clip.setProperty") {
        SetClipProperty body;
        body.trackId = payload.value("trackId", std::string());
        body.clipId = payload.value("clipId", std::string());
        if (!clipPropertyFromName(payload.value("property", std::string()),
                                  body.property) ||
            !payload.contains("value") ||
            !scalarFromJson(payload.at("value"), body.value)) {
            error = "invalid clip property payload";
            return false;
        }
        out = std::move(body);
        return true;
    }
    if (kind == "clip.setAsset") {
        if (!hasExactKeys(payload, {"trackId", "clipId", "asset"})) {
            error = "invalid clip asset payload";
            return false;
        }
        SetClipAsset body;
        body.trackId = payload.value("trackId", std::string());
        body.clipId = payload.value("clipId", std::string());
        if (!assetFromJson(payload.at("asset"), body.asset, true)) {
            error = "invalid clip asset reference";
            return false;
        }
        out = std::move(body);
        return true;
    }
    if (kind == "clip.setSampleEdit") {
        if (!hasExactKeys(payload, {"trackId", "clipId", "sampleEdit"})) {
            error = "invalid clip sample edit payload";
            return false;
        }
        SetClipSampleEdit body;
        body.trackId = payload.value("trackId", std::string());
        body.clipId = payload.value("clipId", std::string());
        if (!sampleEditFromJson(payload.at("sampleEdit"), body.sampleEdit)) {
            error = "invalid clip sample edit value";
            return false;
        }
        out = std::move(body);
        return true;
    }
    if (kind == "plugin.add") {
        if (!hasExactKeys(payload, {"location", "insert", "afterId"})) {
            error = "invalid plugin add payload";
            return false;
        }
        AddPluginInsert body;
        body.afterId = payload.value("afterId", std::string());
        if (!pluginLocationFromJson(payload.at("location"), body.location) ||
            !sharedInsertFromJson(payload.at("insert"), body.insert)) {
            error = "invalid plugin add value";
            return false;
        }
        out = std::move(body);
        return true;
    }
    if (kind == "plugin.delete") {
        if (!hasExactKeys(payload, {"location", "insertId"})) {
            error = "invalid plugin delete payload";
            return false;
        }
        DeletePluginInsert body;
        body.insertId = payload.value("insertId", std::string());
        if (!pluginLocationFromJson(payload.at("location"), body.location)) {
            error = "invalid plugin location";
            return false;
        }
        out = std::move(body);
        return true;
    }
    if (kind == "plugin.restore") {
        if (!hasExactKeys(payload,
                          {"location", "insertId", "deleteOperationId"})) {
            error = "invalid plugin restore payload";
            return false;
        }
        RestorePluginInsert body;
        body.insertId = payload.value("insertId", std::string());
        body.deleteOperationId =
            payload.value("deleteOperationId", std::string());
        if (!pluginLocationFromJson(payload.at("location"), body.location)) {
            error = "invalid plugin location";
            return false;
        }
        out = std::move(body);
        return true;
    }
    if (kind == "plugin.move") {
        if (!hasExactKeys(payload, {"location", "insertId", "afterId"})) {
            error = "invalid plugin move payload";
            return false;
        }
        MovePluginInsert body;
        body.insertId = payload.value("insertId", std::string());
        body.afterId = payload.value("afterId", std::string());
        if (!pluginLocationFromJson(payload.at("location"), body.location)) {
            error = "invalid plugin location";
            return false;
        }
        out = std::move(body);
        return true;
    }
    if (kind == "plugin.setProperty") {
        if (!hasExactKeys(payload,
                          {"location", "insertId", "property", "value"})) {
            error = "invalid plugin property payload";
            return false;
        }
        SetPluginProperty body;
        body.insertId = payload.value("insertId", std::string());
        if (!pluginLocationFromJson(payload.at("location"), body.location) ||
            !pluginPropertyFromName(
                payload.value("property", std::string()), body.property) ||
            !scalarFromJson(payload.at("value"), body.value)) {
            error = "invalid plugin property payload";
            return false;
        }
        out = std::move(body);
        return true;
    }
	    if (kind == "plugin.setState") {
        if (!hasExactKeys(payload,
                          {"location", "insertId", "pluginVersion",
                           "stateSchemaVersion", "stateAsset",
                           "rightStateAsset", "parameters", "rightParameters",
                           "assetBindings"})) {
            error = "invalid plugin state payload";
            return false;
        }
	        if (!payload.at("insertId").is_string() ||
	            !payload.at("pluginVersion").is_string() ||
	            !payload.at("stateSchemaVersion").is_number_integer()) {
	            error = "invalid plugin state scalar value";
	            return false;
	        }
	        SetPluginState body;
        body.insertId = payload.value("insertId", std::string());
        body.pluginVersion = payload.value("pluginVersion", std::string());
        body.stateSchemaVersion = payload.value("stateSchemaVersion", 0);
        if (!pluginLocationFromJson(payload.at("location"), body.location) ||
            !assetFromJson(payload.at("stateAsset"), body.stateAsset, true) ||
            !assetFromJson(payload.at("rightStateAsset"), body.rightStateAsset,
                           true) ||
            !parametersFromJson(payload.at("parameters"), body.parameters) ||
            !parametersFromJson(payload.at("rightParameters"),
                                body.rightParameters) ||
            !bindingsFromJson(payload.at("assetBindings"),
                              body.assetBindings)) {
            error = "invalid plugin state value";
            return false;
        }
        out = std::move(body);
        return true;
    }
	    if (kind == "plugin.setParameter") {
        if (!hasExactKeys(payload,
                          {"location", "insertId", "parameterId", "value",
                           "rightChannel"}) ||
	            !payload.at("insertId").is_string() ||
	            !payload.at("parameterId").is_string() ||
	            !payload.at("value").is_number() ||
            !payload.at("rightChannel").is_boolean()) {
            error = "invalid plugin parameter payload";
            return false;
        }
        SetPluginParameter body;
        body.insertId = payload.value("insertId", std::string());
        body.parameterId = payload.value("parameterId", std::string());
        body.value = payload.at("value").get<double>();
        body.rightChannel = payload.at("rightChannel").get<bool>();
        if (!pluginLocationFromJson(payload.at("location"), body.location)) {
            error = "invalid plugin location";
            return false;
        }
        out = std::move(body);
        return true;
    }
	    if (kind == "plugin.removeParameter") {
        if (!hasExactKeys(payload,
                          {"location", "insertId", "parameterId",
                           "rightChannel"}) ||
	            !payload.at("insertId").is_string() ||
	            !payload.at("parameterId").is_string() ||
	            !payload.at("rightChannel").is_boolean()) {
            error = "invalid plugin parameter removal payload";
            return false;
        }
        RemovePluginParameter body;
        body.insertId = payload.value("insertId", std::string());
        body.parameterId = payload.value("parameterId", std::string());
        body.rightChannel = payload.at("rightChannel").get<bool>();
        if (!pluginLocationFromJson(payload.at("location"), body.location)) {
            error = "invalid plugin location";
            return false;
        }
        out = std::move(body);
        return true;
    }
    if (kind == "plugin.setAssetBinding") {
        if (!hasExactKeys(payload, {"location", "insertId", "binding"})) {
            error = "invalid plugin asset binding payload";
            return false;
        }
        SetPluginAssetBinding body;
        body.insertId = payload.value("insertId", std::string());
        if (!pluginLocationFromJson(payload.at("location"), body.location) ||
            !bindingFromJson(payload.at("binding"), body.binding)) {
            error = "invalid plugin asset binding value";
            return false;
        }
        out = std::move(body);
        return true;
    }
    if (kind == "plugin.removeAssetBinding") {
        if (!hasExactKeys(payload, {"location", "insertId", "key"})) {
            error = "invalid plugin asset binding removal payload";
            return false;
        }
        RemovePluginAssetBinding body;
        body.insertId = payload.value("insertId", std::string());
        body.key = payload.value("key", std::string());
        if (!pluginLocationFromJson(payload.at("location"), body.location)) {
            error = "invalid plugin location";
            return false;
        }
        out = std::move(body);
        return true;
    }
    if (kind == "note.upsert") {
        UpsertMidiNote body;
        body.trackId = payload.value("trackId", std::string());
        body.clipId = payload.value("clipId", std::string());
        body.afterId = payload.value("afterId", std::string());
        if (!payload.contains("note") ||
            !noteFromJson(payload.at("note"), body.note)) {
            error = "invalid MIDI note payload";
            return false;
        }
        out = std::move(body);
        return true;
    }
    if (kind == "note.delete") {
        out = DeleteMidiNote{payload.value("trackId", std::string()),
                             payload.value("clipId", std::string()),
                             payload.value("noteId", std::string())};
        return true;
    }
    if (kind == "note.restore") {
        out = RestoreMidiNote{
            payload.value("trackId", std::string()),
            payload.value("clipId", std::string()),
            payload.value("noteId", std::string()),
            payload.value("deleteOperationId", std::string())};
        return true;
    }
    if (kind == "automationPoint.upsert") {
        UpsertAutomationPoint body;
        body.trackId = payload.value("trackId", std::string());
        body.clipId = payload.value("clipId", std::string());
        body.laneId = payload.value("laneId", std::string());
        body.afterId = payload.value("afterId", std::string());
        if (!payload.contains("point") ||
            !automationPointFromJson(payload.at("point"), body.point)) {
            error = "invalid automation point payload";
            return false;
        }
        out = std::move(body);
        return true;
    }
    if (kind == "automationPoint.delete") {
        out = DeleteAutomationPoint{
            payload.value("trackId", std::string()),
            payload.value("clipId", std::string()),
            payload.value("laneId", std::string()),
            payload.value("pointId", std::string())};
        return true;
    }
    if (kind == "automationPoint.restore") {
        out = RestoreAutomationPoint{
            payload.value("trackId", std::string()),
            payload.value("clipId", std::string()),
            payload.value("laneId", std::string()),
            payload.value("pointId", std::string()),
            payload.value("deleteOperationId", std::string())};
        return true;
    }
    if (kind == "controllerLane.add") {
        if (!hasExactKeys(payload,
                          {"trackId", "clipId", "laneId", "name", "target",
                           "defaultValue", "afterId"})) {
            error = "invalid controller lane add payload shape";
            return false;
        }
        AddControllerLane body;
        body.trackId = payload.value("trackId", std::string());
        body.clipId = payload.value("clipId", std::string());
        body.laneId = payload.value("laneId", std::string());
        body.name = payload.value("name", std::string());
        body.defaultValue = payload.value("defaultValue", 0.0);
        body.afterId = payload.value("afterId", std::string());
        if (!payload.contains("target") ||
            !controllerLaneTargetFromJson(payload.at("target"), body.target)) {
            error = "invalid controller lane target";
            return false;
        }
        out = std::move(body);
        return true;
    }
    if (kind == "controllerLane.delete") {
        if (!hasExactKeys(payload, {"trackId", "clipId", "laneId"})) {
            error = "invalid controller lane delete payload shape";
            return false;
        }
        out = DeleteControllerLane{payload.value("trackId", std::string()),
                                   payload.value("clipId", std::string()),
                                   payload.value("laneId", std::string())};
        return true;
    }
    if (kind == "controllerLane.restore") {
        if (!hasExactKeys(payload, {"trackId", "clipId", "laneId",
                                    "deleteOperationId"})) {
            error = "invalid controller lane restore payload shape";
            return false;
        }
        out = RestoreControllerLane{
            payload.value("trackId", std::string()),
            payload.value("clipId", std::string()),
            payload.value("laneId", std::string()),
            payload.value("deleteOperationId", std::string())};
        return true;
    }
    if (kind == "controllerLane.setTarget") {
        if (!hasExactKeys(payload,
                          {"trackId", "clipId", "laneId", "target"})) {
            error = "invalid controller lane target payload shape";
            return false;
        }
        SetControllerLaneTarget body;
        body.trackId = payload.value("trackId", std::string());
        body.clipId = payload.value("clipId", std::string());
        body.laneId = payload.value("laneId", std::string());
        if (!payload.contains("target") ||
            !controllerLaneTargetFromJson(payload.at("target"), body.target)) {
            error = "invalid controller lane target";
            return false;
        }
        out = std::move(body);
        return true;
    }
    if (kind == "controllerLane.setDefault") {
        if (!hasExactKeys(payload, {"trackId", "clipId", "laneId",
                                    "defaultValue"})) {
            error = "invalid controller lane default payload shape";
            return false;
        }
        out = SetControllerLaneDefault{
            payload.value("trackId", std::string()),
            payload.value("clipId", std::string()),
            payload.value("laneId", std::string()),
            payload.value("defaultValue", 0.0)};
        return true;
    }
    if (kind == "automation.setTarget") {
        if (!hasExactKeys(payload, {"trackId", "clipId", "target"})) {
            error = "invalid automation target payload shape";
            return false;
        }
        SetAutomationTarget body;
        body.trackId = payload.value("trackId", std::string());
        body.clipId = payload.value("clipId", std::string());
        if (!payload.contains("target") ||
            !automationTargetFromJson(payload.at("target"), body.target)) {
            error = "invalid automation target";
            return false;
        }
        out = std::move(body);
        return true;
    }
    if (kind == "automation.setDefault") {
        if (!hasExactKeys(payload,
                          {"trackId", "clipId", "defaultValue"})) {
            error = "invalid automation default payload shape";
            return false;
        }
        out = SetAutomationDefault{payload.value("trackId", std::string()),
                                   payload.value("clipId", std::string()),
                                   payload.value("defaultValue", 0.0)};
        return true;
    }
    if (kind == "automation.setActive") {
        if (!hasExactKeys(payload, {"trackId", "clipId", "active"})) {
            error = "invalid automation active payload shape";
            return false;
        }
        if (!payload.contains("active") || !payload.at("active").is_boolean()) {
            error = "invalid automation active flag";
            return false;
        }
        out = SetAutomationActive{payload.value("trackId", std::string()),
                                  payload.value("clipId", std::string()),
                                  payload.at("active").get<bool>()};
        return true;
    }
    if (kind == "take.add") {
        if (!hasExactKeys(payload, {"trackId", "clipId", "take",
                                    "afterId"})) {
            error = "invalid take add payload shape";
            return false;
        }
        AddTake body;
        body.trackId = payload.value("trackId", std::string());
        body.clipId = payload.value("clipId", std::string());
        body.afterId = payload.value("afterId", std::string());
        if (!payload.contains("take") ||
            !takeFromJson(payload.at("take"), body.take)) {
            error = "invalid take payload";
            return false;
        }
        out = std::move(body);
        return true;
    }
    if (kind == "take.delete") {
        if (!hasExactKeys(payload, {"trackId", "clipId", "takeId"})) {
            error = "invalid take delete payload shape";
            return false;
        }
        out = DeleteTake{payload.value("trackId", std::string()),
                         payload.value("clipId", std::string()),
                         payload.value("takeId", std::string())};
        return true;
    }
    if (kind == "take.restore") {
        if (!hasExactKeys(payload, {"trackId", "clipId", "takeId",
                                    "deleteOperationId"})) {
            error = "invalid take restore payload shape";
            return false;
        }
        out = RestoreTake{payload.value("trackId", std::string()),
                          payload.value("clipId", std::string()),
                          payload.value("takeId", std::string()),
                          payload.value("deleteOperationId", std::string())};
        return true;
    }
    if (kind == "compSegment.upsert") {
        if (!hasExactKeys(payload,
                          {"trackId", "clipId", "segment", "afterId"})) {
            error = "invalid comp segment upsert payload shape";
            return false;
        }
        UpsertCompSegment body;
        body.trackId = payload.value("trackId", std::string());
        body.clipId = payload.value("clipId", std::string());
        body.afterId = payload.value("afterId", std::string());
        if (!payload.contains("segment") ||
            !compSegmentFromJson(payload.at("segment"), body.segment)) {
            error = "invalid comp segment payload";
            return false;
        }
        out = std::move(body);
        return true;
    }
    if (kind == "compSegment.delete") {
        if (!hasExactKeys(payload, {"trackId", "clipId", "segmentId"})) {
            error = "invalid comp segment delete payload shape";
            return false;
        }
        out = DeleteCompSegment{payload.value("trackId", std::string()),
                                payload.value("clipId", std::string()),
                                payload.value("segmentId", std::string())};
        return true;
    }
    if (kind == "compSegment.restore") {
        if (!hasExactKeys(payload, {"trackId", "clipId", "segmentId",
                                    "deleteOperationId"})) {
            error = "invalid comp segment restore payload shape";
            return false;
        }
        out = RestoreCompSegment{
            payload.value("trackId", std::string()),
            payload.value("clipId", std::string()),
            payload.value("segmentId", std::string()),
            payload.value("deleteOperationId", std::string())};
        return true;
    }
    if (kind == "recording.commit") {
        if (!hasExactKeys(payload, {"leases", "commands"}) ||
            !payload.at("leases").is_array() ||
            !payload.at("commands").is_array()) {
            error = "invalid recording commit payload shape";
            return false;
        }
        if (payload.at("leases").empty() ||
            payload.at("leases").size() > kMaxProjectCommandBatchSize) {
            error = "recording lease count is out of bounds";
            return false;
        }
        if (payload.at("commands").empty() ||
            payload.at("commands").size() > kMaxProjectCommandBatchSize) {
            error = "recording command count is out of bounds";
            return false;
        }
        RecordingCommit commit;
        commit.leases.reserve(payload.at("leases").size());
        for (const json& leaseJson : payload.at("leases")) {
            if (!hasExactKeys(leaseJson, {"trackId", "leaseId"}) ||
                !leaseJson.at("trackId").is_string() ||
                !leaseJson.at("leaseId").is_string()) {
                error = "invalid recording lease shape";
                return false;
            }
            commit.leases.push_back(RecordingLeaseClaim{
                leaseJson.at("trackId").get<std::string>(),
                leaseJson.at("leaseId").get<std::string>()});
        }
        commit.batch = std::make_shared<BatchCommand>();
        for (const json& childJson : payload.at("commands")) {
            if (!childJson.is_object() || childJson.size() != 3 ||
                !childJson.contains("kind") ||
                !childJson.contains("payload") ||
                !childJson.contains("preconditions")) {
                error =
                    "recording child must use the locked body envelope";
                return false;
            }
            const std::string childKind =
                childJson.value("kind", std::string());
            if (childKind == "batch" || childKind == "recording.commit") {
                error =
                    "recording.commit cannot contain nested transactions";
                return false;
            }
            if (!childJson.at("preconditions").is_array()) {
                error = "recording child preconditions must be an array";
                return false;
            }
            ProjectCommand child;
            child.meta.schemaVersion = kProjectCommandSchemaVersion;
            for (const json& item : childJson.at("preconditions")) {
                CommandCondition condition;
                if (!conditionFromJson(item, condition)) {
                    error = "invalid recording child precondition";
                    return false;
                }
                child.conditions.push_back(std::move(condition));
            }
            std::string childError;
            if (!parseBody(childKind,
                           childJson.value("payload", json::object()),
                           child.body, childError)) {
                error = "invalid recording child: " + childError;
                return false;
            }
            commit.batch->commands.push_back(std::move(child));
        }
        out = std::move(commit);
        return true;
    }
    if (kind == "batch") {
        if (!payload.contains("commands") || !payload.at("commands").is_array()) {
            error = "batch commands must be an array";
            return false;
        }
        if (payload.at("commands").empty() ||
            payload.at("commands").size() > kMaxProjectCommandBatchSize) {
            error = "batch command count is out of bounds";
            return false;
        }
        auto batch = std::make_shared<BatchCommand>();
        for (const json& childJson : payload.at("commands")) {
            if (!childJson.is_object() || childJson.size() != 3 ||
                !childJson.contains("kind") ||
                !childJson.contains("payload") ||
                !childJson.contains("preconditions")) {
                error = "batch child must use the locked body envelope";
                return false;
            }
            const std::string childKind =
                childJson.value("kind", std::string());
            if (childKind == "batch" || childKind == "recording.commit") {
                error = "batch cannot contain nested transactions";
                return false;
            }
            ProjectCommand child;
            child.meta.schemaVersion = kProjectCommandSchemaVersion;
            if (childJson.contains("preconditions")) {
                if (!childJson.at("preconditions").is_array()) {
                    error = "batch child preconditions must be an array";
                    return false;
                }
                for (const json& item : childJson.at("preconditions")) {
                    CommandCondition condition;
                    if (!conditionFromJson(item, condition)) {
                        error = "invalid batch child precondition";
                        return false;
                    }
                    child.conditions.push_back(std::move(condition));
                }
            }
            std::string childError;
            if (!parseBody(childKind,
                           childJson.value("payload", json::object()), child.body,
                           childError)) {
                error = "invalid batch child: " + childError;
                return false;
            }
            batch->commands.push_back(std::move(child));
        }
        out = std::move(batch);
        return true;
    }
    error = "unknown command kind: " + kind;
    return false;
}

} // namespace

nlohmann::json projectCommandToJson(const ProjectCommand& command) {
    json conditions = json::array();
    for (const CommandCondition& condition : command.conditions)
        conditions.push_back(conditionToJson(condition));
    json out{{"schemaVersion", command.meta.schemaVersion},
             {"opId", command.meta.operationId},
             {"transactionId", command.meta.transactionId},
             {"baseServerSeq", command.meta.baseServerSequence},
             {"kind", commandKind(command)},
             {"payload", bodyToJson(command)},
             {"preconditions", std::move(conditions)},
             {"touchedFields", commandTouchedFields(command)}};
    return out;
}

std::optional<ProjectCommand> projectCommandFromJson(const nlohmann::json& value,
                                                     std::string* error) {
    auto fail = [&](std::string message) -> std::optional<ProjectCommand> {
        if (error) *error = std::move(message);
        return std::nullopt;
    };
    try {
        if (!value.is_object()) return fail("command must be an object");
        static constexpr const char* required[] = {
            "schemaVersion", "opId", "transactionId", "baseServerSeq",
            "kind", "payload", "preconditions", "touchedFields"};
        if (value.size() != std::size(required))
            return fail("command contains fields outside the locked envelope");
        for (const char* key : required) {
            if (!value.contains(key))
                return fail(std::string("missing command field: ") + key);
        }
        ProjectCommand command;
        command.meta = metaFromJson(value);
        if (command.meta.schemaVersion != kProjectCommandSchemaVersion)
            return fail("unsupported project command schema version");
        if (value.contains("preconditions")) {
            if (!value.at("preconditions").is_array())
                return fail("preconditions must be an array");
            if (value.at("preconditions").size() >
                kMaxProjectCommandPreconditions) {
                return fail("command has too many preconditions");
            }
            for (const json& item : value.at("preconditions")) {
                CommandCondition condition;
                if (!conditionFromJson(item, condition))
                    return fail("invalid command condition");
                command.conditions.push_back(std::move(condition));
            }
        }
        std::string bodyError;
        if (!parseBody(value.value("kind", std::string()),
                       value.value("payload", json::object()), command.body,
                       bodyError)) {
            return fail(std::move(bodyError));
        }
        if (!value.at("touchedFields").is_array())
            return fail("touchedFields must be an array");
        if (value.at("touchedFields").size() >
            kMaxProjectCommandTouchedFields) {
            return fail("command touches too many fields");
        }
        std::vector<std::string> touchedFields;
        touchedFields.reserve(value.at("touchedFields").size());
        for (const json& field : value.at("touchedFields")) {
            if (!field.is_string() || field.get_ref<const std::string&>().empty())
                return fail("touchedFields must contain non-empty strings");
            touchedFields.push_back(field.get<std::string>());
        }
        const std::vector<std::string> expected = commandTouchedFields(command);
        if (touchedFields != expected)
            return fail("touchedFields do not match the command payload");
        std::string idError;
        if (!commandHasValidIds(command, &idError)) return fail(std::move(idError));
        if (error) error->clear();
        return command;
    } catch (const json::exception& exception) {
        return fail(std::string("invalid command JSON: ") + exception.what());
    }
}

std::string serializeProjectCommand(const ProjectCommand& command) {
    return projectCommandToJson(command).dump();
}

std::optional<ProjectCommand> deserializeProjectCommand(std::string_view bytes,
                                                        std::string* error) {
    const json value = json::parse(bytes, nullptr, false);
    if (value.is_discarded()) {
        if (error) *error = "invalid command JSON";
        return std::nullopt;
    }
    return projectCommandFromJson(value, error);
}

} // namespace daw::collab
