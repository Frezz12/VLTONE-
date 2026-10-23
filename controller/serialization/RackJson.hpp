#pragma once
#include "model/RackGroups.hpp"
#include <nlohmann/json.hpp>

namespace daw::serialization {
inline nlohmann::json rackGroupsToJson(const std::vector<RackGroupModel>& groups) {
    auto result = nlohmann::json::array();
    for (const auto& group : groups) {
        auto layers = nlohmann::json::array();
        for (const auto& layer : group.layers)
            layers.push_back({{"id", layer.id}, {"name", layer.name}, {"inserts", layer.insertIds},
                              {"gain", layer.gain}, {"muted", layer.muted}, {"solo", layer.solo}});
        result.push_back({{"id", group.id}, {"name", group.name}, {"inserts", group.insertIds},
                          {"parentId", group.parentId}, {"layers", layers}});
    }
    return result;
}
inline std::vector<RackGroupModel> rackGroupsFromJson(const nlohmann::json& parent, const char* key,
                                                      const std::vector<InsertModel>& inserts) {
    std::vector<RackGroupModel> result;
    const auto list = parent.find(key);
    if (list == parent.end() || !list->is_array())
        return result;
    for (const auto& item : *list) {
        if (!item.is_object() || !item.contains("id") || !item["id"].is_string() ||
            !item.contains("inserts") || !item["inserts"].is_array())
            continue;
        RackGroupModel group{item["id"].get<std::string>(), item.value("name", std::string("Group")), {}};
        for (const auto& id : item["inserts"])
            if (id.is_string())
                group.insertIds.push_back(id.get<std::string>());
        group.parentId = item.value("parentId", std::string{});
        if (const auto layers = item.find("layers"); layers != item.end() && layers->is_array())
            for (const auto& value : *layers) {
                if (!value.is_object() || !value.contains("id") || !value["id"].is_string()) continue;
                RackLayerModel layer{value["id"].get<std::string>(), value.value("name", std::string("Layer")), {}};
                if (const auto members = value.find("inserts"); members != value.end() && members->is_array())
                    for (const auto& id : *members) if (id.is_string()) layer.insertIds.push_back(id.get<std::string>());
                layer.gain = value.value("gain", 1.f); layer.muted = value.value("muted", false); layer.solo = value.value("solo", false);
                group.layers.push_back(std::move(layer));
            }
        result.push_back(std::move(group));
    }
    normalizeRackGroups(result, inserts);
    return result;
}
} // namespace daw::serialization
