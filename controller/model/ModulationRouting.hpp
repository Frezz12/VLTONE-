#pragma once
#include "model/Document.hpp"

namespace daw {
bool validModulationRouting(const ProjectModel&, const std::string& overrideChannel = {},
                            const modulation::Rack* replacement = nullptr);
std::vector<TrackModel> captureModulationAutomation(const ProjectModel&, const std::string& channel,
                                                    const modulation::Rack&);
std::vector<TrackModel> copyModulationAutomation(const std::vector<TrackModel>&,
    const std::unordered_map<std::string, std::string>& ids, const std::string& channel, bool fresh);
void replaceModulationAutomation(ProjectModel&, const std::string& channel, const std::vector<TrackModel>&);
inline modulation::Rack copyModulation(const modulation::Rack& rack,
    const std::unordered_map<std::string, std::string>& slots, bool fresh = true,
    std::unordered_map<std::string, std::string>* mappedIds = nullptr, bool keepUnused = false) {
    modulation::Rack copy; copy.enabled = rack.enabled;
    std::unordered_map<std::string, std::string> sources;
    for (const auto& source : rack.sources) {
        if (!keepUnused && std::none_of(rack.routes.begin(), rack.routes.end(), [&](const auto& r) {
                return r.sourceId == source.id && slots.contains(r.slotId); })) continue;
        auto next = source; if (fresh) next.id = newUuid(); sources[source.id] = next.id;
        if (mappedIds) (*mappedIds)[source.id] = next.id;
        copy.sources.push_back(std::move(next));
    }
    for (auto route : rack.routes) {
        const auto slot = slots.find(route.slotId);
        if (slot == slots.end() || !sources.contains(route.sourceId)) continue;
        const auto targetSlot = slot->second;
        const auto old = route.id; if (fresh) route.id = newUuid();
        if (mappedIds) (*mappedIds)[old] = route.id;
        route.slotId = targetSlot; route.sourceId = sources.at(route.sourceId); copy.routes.push_back(std::move(route));
    }
    return copy;
}
} // namespace daw
