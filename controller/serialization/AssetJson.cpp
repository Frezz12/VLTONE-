#include "serialization/AssetJson.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace daw::serialization {

using json = nlohmann::json;

json assetRefToJson(const AssetRef& asset) {
    if (asset.empty()) return nullptr;
    json out{
        {"assetId", asset.assetId},
        {"sha256", asset.sha256},
        {"kind", toString(asset.kind)},
        {"byteSize", asset.byteSize},
        {"originalName", asset.originalName},
    };
    json audioMetadata = json::object();
    if (!asset.mimeType.empty()) audioMetadata["mimeType"] = asset.mimeType;
    if (!asset.codec.empty()) audioMetadata["codec"] = asset.codec;
    if (asset.sampleRate > 0.0) audioMetadata["sampleRate"] = asset.sampleRate;
    if (asset.channels > 0) audioMetadata["channels"] = asset.channels;
    if (asset.frames > 0) audioMetadata["frames"] = asset.frames;
    if (!audioMetadata.empty()) out["audioMetadata"] = std::move(audioMetadata);
    return out;
}

AssetRef assetRefFromJson(const json& value) {
    AssetRef asset;
    if (!value.is_object()) return asset;
    asset.assetId = value.value("assetId", std::string());
    asset.sha256 = value.value("sha256", std::string());
    asset.kind = assetKindFromString(value.value("kind", std::string()));
    asset.byteSize = value.value("byteSize", std::uint64_t(0));
    asset.originalName = value.value("originalName", std::string());
    const json& metadata = value.contains("audioMetadata") &&
                                   value.at("audioMetadata").is_object()
                               ? value.at("audioMetadata")
                               : value;
    asset.mimeType = metadata.value("mimeType", std::string());
    asset.codec = metadata.value("codec", std::string());
    asset.sampleRate = std::clamp(metadata.value("sampleRate", 0.0), 0.0,
                                  768000.0);
    asset.channels = std::clamp(metadata.value("channels", std::uint32_t(0)),
                                std::uint32_t(0), std::uint32_t(1024));
    asset.frames = metadata.value("frames", std::uint64_t(0));
    return asset;
}

json pluginAssetBindingsToJson(
    const std::vector<PluginAssetBinding>& bindings) {
    json out = json::array();
    for (const PluginAssetBinding& binding : bindings) {
        if (binding.key.empty() || binding.asset.empty()) continue;
        out.push_back(json{{"key", binding.key},
                           {"required", binding.required},
                           {"asset", assetRefToJson(binding.asset)}});
    }
    return out;
}

std::vector<PluginAssetBinding> pluginAssetBindingsFromJson(
    const json& parent, const char* key) {
    std::vector<PluginAssetBinding> out;
    if (!parent.contains(key) || !parent.at(key).is_array()) return out;
    for (const json& value : parent.at(key)) {
        if (!value.is_object()) continue;
        PluginAssetBinding binding;
        binding.key = value.value("key", value.value("name", std::string()));
        binding.required = value.value("required", true);
        if (value.contains("asset"))
            binding.asset = assetRefFromJson(value.at("asset"));
        if (!binding.key.empty() && !binding.asset.empty())
            out.push_back(std::move(binding));
    }
    return out;
}

} // namespace daw::serialization
