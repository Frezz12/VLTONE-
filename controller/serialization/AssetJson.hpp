#pragma once

#include "model/Document.hpp"

#include <nlohmann/json_fwd.hpp>

namespace daw::serialization {

nlohmann::json assetRefToJson(const AssetRef& asset);
AssetRef assetRefFromJson(const nlohmann::json& value);

nlohmann::json pluginAssetBindingsToJson(
    const std::vector<PluginAssetBinding>& bindings);
std::vector<PluginAssetBinding> pluginAssetBindingsFromJson(
    const nlohmann::json& parent, const char* key);

} // namespace daw::serialization
