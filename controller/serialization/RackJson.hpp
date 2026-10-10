#pragma once
#include "model/RackGroups.hpp"
#include <nlohmann/json.hpp>

namespace daw::serialization {
inline nlohmann::json rackGroupsToJson(const std::vector<RackGroupModel>& groups) {
    auto result = nlohmann::json::array();
    for (const auto& group : groups)
        result.push_back({{"id", group.id}, {"name", group.name}, {"inserts", group.insertIds}});
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
        result.push_back(std::move(group));
    }
    normalizeRackGroups(result, inserts);
    return result;
}
} // namespace daw::serialization
