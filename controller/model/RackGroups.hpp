#pragma once
#include "model/Document.hpp"
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace daw {
inline bool rackContains(const std::vector<std::string>& ids, const std::string& id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}
inline void normalizeRackGroups(std::vector<RackGroupModel>& groups, const std::vector<InsertModel>& inserts) {
    std::unordered_map<std::string, std::size_t> positions;
    for (std::size_t i = 0; i < inserts.size(); ++i)
        if (inserts[i].isLoaded()) positions.emplace(inserts[i].id, i);
    std::unordered_set<std::string> identities;
    for (auto& group : groups) {
        std::erase_if(group.insertIds, [&](const auto& id) { return !positions.contains(id); });
        std::sort(group.insertIds.begin(), group.insertIds.end(), [&](const auto& a, const auto& b) { return positions[a] < positions[b]; });
        group.insertIds.erase(std::unique(group.insertIds.begin(), group.insertIds.end()), group.insertIds.end());
        if (group.id.empty() || !identities.insert(group.id).second) group.insertIds.clear();
        if (group.insertIds.empty()) continue;
        const auto first = positions[group.insertIds.front()], last = positions[group.insertIds.back()];
        group.insertIds.clear();
        for (auto i = first; i <= last && inserts[i].isLoaded(); ++i) group.insertIds.push_back(inserts[i].id);
    }
    std::erase_if(groups, [](const auto& g) { return g.insertIds.empty(); });
    const auto find = [&](const std::string& id) -> RackGroupModel* {
        for (auto& g : groups) if (g.id == id) return &g;
        return nullptr;
    };
    for (auto& g : groups) {
        std::unordered_set<std::string> path{g.id};
        auto parent = g.parentId;
        for (int depth = 0; !parent.empty(); ++depth) {
            auto* p = find(parent);
            if (!p || !path.insert(parent).second || depth >= 8 ||
                !std::all_of(g.insertIds.begin(), g.insertIds.end(), [&](const auto& id) { return rackContains(p->insertIds, id); })) {
                g.parentId.clear(); break;
            }
            parent = p->parentId;
        }
        if (g.layers.empty()) continue;
        if (g.layers.size() > 32) g.layers.resize(32);
        std::unordered_set<std::string> claimed, layerIds;
        for (auto& layer : g.layers) {
            if (layer.id.empty() || !layerIds.insert(layer.id).second) layer.id = newUuid();
            layer.gain = std::isfinite(layer.gain) ? std::clamp(layer.gain, 0.f, 2.f) : 1.f;
            std::erase_if(layer.insertIds, [&](const auto& id) { return !rackContains(g.insertIds, id) || !claimed.insert(id).second; });
            std::sort(layer.insertIds.begin(), layer.insertIds.end(), [&](const auto& a, const auto& b) { return positions[a] < positions[b]; });
        }
        // Inserts added inside a group join the nearest preceding branch.
        for (const auto& id : g.insertIds) if (!claimed.contains(id)) {
            auto* target = &g.layers.front();
            std::size_t nearest = 0;
            for (auto& layer : g.layers) for (const auto& member : layer.insertIds)
                if (positions[member] <= positions[id] && positions[member] >= nearest) { target = &layer; nearest = positions[member]; }
            target->insertIds.push_back(id);
        }
        for (auto& layer : g.layers)
            std::sort(layer.insertIds.begin(), layer.insertIds.end(), [&](const auto& a, const auto& b) { return positions[a] < positions[b]; });
    }
    // Sibling spans must not overlap: imported groups cannot run a plugin twice.
    std::unordered_map<std::string, std::unordered_set<std::string>> claimed;
    for (auto& g : groups) {
        auto& ids = claimed[g.parentId];
        if (std::any_of(g.insertIds.begin(), g.insertIds.end(), [&](const auto& id) { return ids.contains(id); })) g.insertIds.clear();
        else ids.insert(g.insertIds.begin(), g.insertIds.end());
    }
    // A nested container must stay in one parallel branch.
    for (auto& g : groups) if (const auto* p = find(g.parentId); p && !p->layers.empty()) {
        const bool inside = std::any_of(p->layers.begin(), p->layers.end(), [&](const auto& layer) {
            return std::all_of(g.insertIds.begin(), g.insertIds.end(), [&](const auto& id) { return rackContains(layer.insertIds, id); });
        });
        if (!inside) g.insertIds.clear();
    }
    // Drop invalid subtrees together; promoting an orphan could overlap a valid root.
    for (int depth = 0; depth < 9; ++depth) {
        bool changed = false;
        for (auto& g : groups) if (!g.insertIds.empty() && !g.parentId.empty()) {
            const auto* parent = find(g.parentId);
            if (!parent || parent->insertIds.empty()) { g.insertIds.clear(); changed = true; }
        }
        if (!changed) break;
    }
    std::erase_if(groups, [](const auto& g) { return g.insertIds.empty(); });
    std::stable_sort(groups.begin(), groups.end(), [&](const auto& a, const auto& b) { return positions[a.insertIds.front()] < positions[b.insertIds.front()]; });
}

inline std::vector<RackGroupModel> copyRackGroups(const std::vector<RackGroupModel>& groups,
    const std::unordered_map<std::string, std::string>& ids, bool mintIds = true) {
    std::vector<RackGroupModel> out;
    std::unordered_map<std::string, std::string> groupIds;
    for (const auto& g : groups)
        if (std::all_of(g.insertIds.begin(), g.insertIds.end(), [&](const auto& id) { return ids.contains(id); }))
            groupIds[g.id] = mintIds ? newUuid() : g.id;
    for (const auto& g : groups) {
        if (!groupIds.contains(g.id)) continue;
        auto copy = g; copy.id = groupIds.at(g.id);
        copy.parentId = groupIds.contains(g.parentId) ? groupIds.at(g.parentId) : std::string{};
        for (auto& id : copy.insertIds) id = ids.at(id);
        for (auto& layer : copy.layers) {
            if (mintIds) layer.id = newUuid();
            for (auto& id : layer.insertIds) id = ids.at(id);
        }
        out.push_back(std::move(copy));
    }
    return out;
}
} // namespace daw
