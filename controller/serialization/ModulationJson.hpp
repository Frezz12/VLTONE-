#pragma once
#include "Modulation/Modulation.hpp"
#include <nlohmann/json.hpp>
#include <unordered_set>

namespace daw::modulation {
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ShapePoint, x, y, bend, shape)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Source, id, name, ownerSlotId, kind, enabled, wave, sync, rate, beats,
    phase, smoothMs, audioChannelId, tap, gainDb, attackMs, releaseMs, filter, lowHz, highHz,
    points, steps, direction, seed, randomLoop, randomSmooth)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Route, id, sourceId, slotId, pluginUid, pluginFormat,
    parameterId, depth, bipolar, enabled)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Rack, enabled, sources, routes)
inline bool valid(const Rack& rack) {
    if (rack.sources.size() > 256 || rack.routes.size() > 4096) return false;
    std::unordered_set<std::string> ids, pairs;
    for (const auto& source : rack.sources)
        if (!valid(source) || !ids.insert(source.id).second) return false;
    std::unordered_set<std::string> routeIds;
    for (const auto& route : rack.routes) {
        if (route.id.empty() || route.id.size() > 128 || !routeIds.insert(route.id).second ||
            !ids.contains(route.sourceId) || route.slotId.empty() || route.slotId.size() > 128 || route.pluginUid.empty() || route.pluginUid.size() > 4096 ||
            (route.pluginFormat != "internal" && route.pluginFormat != "clap" && route.pluginFormat != "vst3" && route.pluginFormat != "vst" && route.pluginFormat != "au") ||
            route.parameterId.empty() || route.parameterId.size() > 400 ||
            !std::isfinite(route.depth) || std::abs(route.depth) > 1.0 ||
            !pairs.insert(route.sourceId + "\n" + route.slotId + "\n" + route.parameterId).second) return false;
    }
    return true;
}
inline Rack fromJson(const nlohmann::json& parent, const char* key = "modulation") {
    if (!parent.contains(key)) return {};
    auto rack = parent.at(key).get<Rack>();
    if (!valid(rack)) throw std::invalid_argument("Invalid Rack modulation");
    return rack;
}
} // namespace daw::modulation
