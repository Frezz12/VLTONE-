#pragma once
#include "model/Document.hpp"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace daw {
/// Remove stale members and order the surviving groups by processing order.
/// An intervening new insert joins its surrounding group. Explicit transfers
/// remove their members first, so dragging out never pulls the group along.
inline void normalizeRackGroups(std::vector<RackGroupModel>& groups,
                                const std::vector<InsertModel>& inserts) {
    std::unordered_map<std::string, std::size_t> positions;
    for (std::size_t i = 0; i < inserts.size(); ++i)
        if (inserts[i].isLoaded())
            positions.emplace(inserts[i].id, i);
    std::unordered_set<std::string> claimed, groupIds;
    for (auto& group : groups) {
        std::size_t first = inserts.size(), last = 0;
        for (const auto& id : group.insertIds)
            if (const auto it = positions.find(id); it != positions.end()) {
                first = std::min(first, it->second);
                last = std::max(last, it->second);
            }
        group.insertIds.clear();
        if (group.id.empty() || !groupIds.insert(group.id).second || first == inserts.size())
            continue;
        for (auto i = first; i <= last; ++i) {
            if (!inserts[i].isLoaded() || claimed.contains(inserts[i].id))
                break;
            group.insertIds.push_back(inserts[i].id);
            claimed.insert(inserts[i].id);
        }
    }
    std::erase_if(groups, [](const auto& g) { return g.insertIds.empty(); });
    std::sort(groups.begin(), groups.end(), [&](const auto& a, const auto& b) {
        return positions.at(a.insertIds.front()) < positions.at(b.insertIds.front());
    });
}

inline std::vector<RackGroupModel> copyRackGroups(const std::vector<RackGroupModel>& groups,
                                                  const std::unordered_map<std::string, std::string>& ids,
                                                  bool mintIds = true) {
    std::vector<RackGroupModel> out;
    for (const auto& group : groups) {
        if (!std::all_of(group.insertIds.begin(), group.insertIds.end(),
                         [&](const auto& id) { return ids.contains(id); }))
            continue;
        auto copy = group;
        if (mintIds)
            copy.id = newUuid();
        for (auto& id : copy.insertIds)
            id = ids.at(id);
        out.push_back(std::move(copy));
    }
    return out;
}
} // namespace daw
