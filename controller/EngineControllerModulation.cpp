#include "EngineController.hpp"
#include "model/ModulationRouting.hpp"
#include "serialization/ModulationJson.hpp"

namespace daw {
modulation::Rack* EngineController::mutableRackModulation(const std::string& channel) {
    if (channel == kMasterChannelId) return &m_project.masterModulation;
    auto* track = m_project.findTrack(channel);
    return track && carriesAudio(*track) ? &track->modulation : nullptr;
}
const modulation::Rack& EngineController::rackModulation(const std::string& channel) const {
    static const modulation::Rack empty;
    if (channel == kMasterChannelId) return m_project.masterModulation;
    const auto* track = m_project.findTrack(channel);
    return track ? track->modulation : empty;
}
bool EngineController::setRackModulation(const std::string& channel, const modulation::Rack& rack, const std::string& label) {
    auto* current = mutableRackModulation(channel);
    if (cloudProjectBound() || !current || !modulation::valid(rack) || !validModulationRouting(m_project, channel, &rack)) return false;
    if (*current == rack) return true;
    const auto before = *current;
    const auto* owner = m_project.findTrack(channel);
    const auto previousFreeze = owner ? owner->freeze : TrackFreezeState{};
    bool topology = before.sources.size() != rack.sources.size() || before.routes.size() != rack.routes.size();
    if (!topology) {
        for (std::size_t i = 0; i < rack.sources.size(); ++i) {
            const auto& a = before.sources[i]; const auto& b = rack.sources[i];
            topology |= a.id != b.id || a.kind != b.kind || a.audioChannelId != b.audioChannelId || a.tap != b.tap;
        }
        for (std::size_t i = 0; i < rack.routes.size(); ++i) {
            const auto& a = before.routes[i]; const auto& b = rack.routes[i];
            topology |= a.id != b.id || a.sourceId != b.sourceId || a.slotId != b.slotId ||
                a.pluginUid != b.pluginUid || a.pluginFormat != b.pluginFormat || a.parameterId != b.parameterId;
        }
    }
    if (auto* track = m_project.findTrack(channel); track && track->freeze.active()) {
        track->freeze = {}; topology = true;
    }
    *current = rack;
    if (!(topology ? bool(rebuildGraph()) : m_runtime->updateModulation(channel, rack))) {
        if (auto* restore = mutableRackModulation(channel)) *restore = before;
        if (auto* track = m_project.findTrack(channel)) track->freeze = previousFreeze;
        rebuildGraph(); return false;
    }
    for (const auto& source : rack.sources) if (modulation::hasRate(source.kind)) {
        AutomationTarget target{AutomationTargetKind::ModulatorRate, channel, source.id, "rate", {}};
        followPassiveAutomation(target, plainToAutomation(target, source.sync ? 1.0 / source.beats : source.rate));
    }
    for (const auto& route : rack.routes) {
        AutomationTarget target{AutomationTargetKind::ModulationDepth, channel, route.id, "depth", {}};
        followPassiveAutomation(target, plainToAutomation(target, route.depth));
    }
    TrackModel master; master.id = kMasterChannelId;
    if (const auto* track = channel == kMasterChannelId ? &master : m_project.findTrack(channel)) syncTrackAutomation(*track);
    invalidateAutomationReadoutCache();
    m_undo.push(label, [this, channel, before, label] { setRackModulation(channel, before, label); },
        [this, channel, rack, label] { setRackModulation(channel, rack, label); });
    return true;
}
std::string EngineController::addRackModulator(const std::string& channel, modulation::Kind kind) {
    auto rack = rackModulation(channel); modulation::Source source;
    source.id = newUuid(); source.kind = kind; source.name = modulation::kindName(kind);
    if (kind == modulation::Kind::Steps) source.beats = .25;
    if (kind == modulation::Kind::Random) {
        // Persist the seed once; duplication intentionally preserves the same pattern.
        source.seed = 2166136261U;
        for (const auto ch : source.id) source.seed = (source.seed ^ std::uint8_t(ch)) * 16777619U;
    }
    rack.sources.push_back(source);
    return setRackModulation(channel, rack, "Add " + source.name) ? source.id : std::string{};
}
std::string EngineController::assignRackModulation(const std::string& channel, const std::string& sourceId,
    const std::string& slotId, const std::string& parameterId) {
    auto rack = rackModulation(channel);
    for (const auto& route : rack.routes) if (route.sourceId == sourceId && route.slotId == slotId && route.parameterId == parameterId) return route.id;
    const auto source = std::find_if(rack.sources.begin(), rack.sources.end(), [&](const auto& s) { return s.id == sourceId; });
    const auto* slot = insertModel(channel, slotId);
    const auto info = m_runtime->pluginParameterInfo({channel, slotId}, parameterId);
    const auto* instance = m_runtime->pluginInstance({channel, slotId});
    if (source == rack.sources.end() || !slot || !instance || !info || !info->isAutomatable || info->isReadOnly ||
        !std::isfinite(info->minValue) || !std::isfinite(info->maxValue) || info->minValue >= info->maxValue) return {};
    modulation::Route route; route.id = newUuid(); route.sourceId = sourceId; route.slotId = slotId;
    route.pluginUid = slot->uid; route.pluginFormat = plugins::toString(instance->descriptor().format); route.parameterId = parameterId;
    route.bipolar = source->kind == modulation::Kind::Lfo || source->kind == modulation::Kind::Steps || source->kind == modulation::Kind::Random;
    rack.routes.push_back(route);
    return setRackModulation(channel, rack, "Assign Modulation") ? route.id : std::string{};
}
std::string EngineController::duplicateRackModulator(const std::string& channel, const std::string& sourceId) {
    if (cloudProjectBound()) return {};
    RackChannelState captured;
    if (!captureRackChannel(channel, false, captured)) return {};
    RackEditState before{{std::move(captured)}, {}}, after = before;
    auto& snapshot = after.channels.front().snapshot;
    const auto found = std::find_if(snapshot.modulation.sources.begin(), snapshot.modulation.sources.end(),
        [&](const auto& source) { return source.id == sourceId; });
    if (found == snapshot.modulation.sources.end()) return {};
    auto copy = *found; copy.id = newUuid(); copy.name += " 2";
    std::unordered_map<std::string, std::string> ids{{sourceId, copy.id}};
    snapshot.modulation.sources.push_back(copy);
    for (const auto& route : before.channels.front().snapshot.modulation.routes) if (route.sourceId == sourceId) {
        auto next = route; next.id = newUuid(); next.sourceId = copy.id;
        ids.emplace(route.id, next.id); snapshot.modulation.routes.push_back(std::move(next));
    }
    auto automation = copyModulationAutomation(snapshot.modulationAutomation, ids, channel, true);
    snapshot.modulationAutomation.insert(snapshot.modulationAutomation.end(), automation.begin(), automation.end());
    if (!modulation::valid(snapshot.modulation)) return {};
    return commitRackEdit(before, after, "Duplicate Modulator") ? copy.id : std::string{};
}
float EngineController::rackModulationValue(const std::string& channel, const std::string& source) const {
    return m_runtime->modulationValue(channel, source);
}
float EngineController::rackModulationPosition(const std::string& channel, const std::string& source) const {
    return m_runtime->modulationPosition(channel, source);
}
std::string EngineController::rackModulationStatus(const std::string& channel, const std::string& route) const {
    return m_runtime->modulationStatus(channel, route);
}
} // namespace daw
