#include "model/ModulationRouting.hpp"
#include "EngineController.hpp"
#include "model/RackGroups.hpp"
#include <algorithm>
#include <unordered_set>

namespace daw {
namespace {
audio::Result rackError(const std::string& message) {
    return audio::Result::fail(audio::EngineError::InvalidArgument, message);
}
std::vector<InsertModel> models(const EngineController::ChannelSnapshot& snapshot) {
    std::vector<InsertModel> out;
    for (const auto& slot : snapshot.inserts)
        out.push_back(slot.model);
    return out;
}
void normalize(EngineController::ChannelSnapshot& snapshot) {
    normalizeRackGroups(snapshot.rackGroups, models(snapshot));
}
void lift(EngineController::ChannelSnapshot& snapshot, const std::unordered_set<std::string>& ids, bool normalizeResult = true) {
    if (snapshot.instrument && ids.contains(snapshot.instrument->model.id)) {
        snapshot.instrument.reset();
        snapshot.instrumentFx.clear();
    }
    std::erase_if(snapshot.inserts, [&](const auto& s) { return ids.contains(s.model.id); });
    for (auto& group : snapshot.rackGroups) {
        std::erase_if(group.insertIds, [&](const auto& id) { return ids.contains(id); });
        for (auto& layer : group.layers)
            std::erase_if(layer.insertIds, [&](const auto& id) { return ids.contains(id); });
    }
    std::erase_if(snapshot.modulation.routes, [&](const auto& route) { return ids.contains(route.slotId); });
    // Keep shared sources alive when their presentation owner leaves this channel.
    for (auto& source : snapshot.modulation.sources)
        if (ids.contains(source.ownerSlotId) &&
            std::any_of(snapshot.modulation.routes.begin(), snapshot.modulation.routes.end(),
                [&](const auto& route) { return route.sourceId == source.id; }))
            source.ownerSlotId.clear();
    std::erase_if(snapshot.modulation.sources, [&](const auto& source) { return ids.contains(source.ownerSlotId); });
    std::unordered_set<std::string> remaining;
    for (const auto& s : snapshot.modulation.sources) remaining.insert(s.id);
    for (const auto& r : snapshot.modulation.routes) remaining.insert(r.id);
    for (auto& lane : snapshot.modulationAutomation)
        std::erase_if(lane.clips, [&](const auto& c) { return !remaining.contains(c.automation.target.slotId); });
    std::erase_if(snapshot.modulationAutomation, [](const auto& t) { return t.clips.empty(); });
    if (normalizeResult) normalize(snapshot);
}
bool place(EngineController::ChannelSnapshot& target, EngineController::ChannelSnapshot source,
           std::size_t index, const std::string& groupId, bool copy, std::vector<std::string>& landed, const std::string& channel, bool copySources = false) {
    if (source.instrument && !groupId.empty())
        return false;
    RackLayerModel* branch = nullptr;
    auto group = std::find_if(target.rackGroups.begin(), target.rackGroups.end(), [&](auto& g) {
        for (auto& layer : g.layers) if (layer.id == groupId) { branch = &layer; return true; }
        return g.id == groupId;
    });
    if (!groupId.empty() && group == target.rackGroups.end())
        return false;
    index = std::min(index, target.inserts.size());
    // Dropping between two members means insertion into that group, never
    // silently splitting it into two distant spans.
    if (groupId.empty() && index && index < target.inserts.size()) {
        for (auto it = target.rackGroups.begin(); it != target.rackGroups.end(); ++it) {
            const auto contains = [&](const std::string& id) {
                return std::find(it->insertIds.begin(), it->insertIds.end(), id) != it->insertIds.end();
            };
            if (contains(target.inserts[index - 1].model.id) && contains(target.inserts[index].model.id)) {
                if (group == target.rackGroups.end() || it->insertIds.size() < group->insertIds.size()) group = it;
            }
        }
    }
    std::unordered_map<std::string, std::string> mapping;
    if (source.instrument) {
        const auto instrumentId = source.instrument->model.id;
        if (copy) {
            source.instrument->model.id = newUuid();
            source.instrument->model.windowOpen = false;
            for (auto& fx : source.instrumentFx) {
                fx.model.id = newUuid();
                fx.model.windowOpen = false;
            }
        }
        mapping[instrumentId] = source.instrument->model.id;
        landed.push_back(source.instrument->model.id);
        target.instrument = source.instrument;
        target.instrumentFx = source.instrumentFx;
        target.instrumentFxVolume = source.instrumentFxVolume;
        target.instrumentFxPan = source.instrumentFxPan;
    }
    const auto effectStart = landed.size();
    for (auto& slot : source.inserts) {
        const auto old = slot.model.id;
        if (copy) {
            slot.model.id = newUuid();
            slot.model.windowOpen = false;
        }
        mapping[old] = slot.model.id;
        landed.push_back(slot.model.id);
    }
    std::string parent;
    if (group != target.rackGroups.end()) {
        parent = group->id;
        if (!branch && !group->layers.empty()) {
            branch = &group->layers.front();
            if (index && index <= target.inserts.size())
                for (auto& layer : group->layers) if (rackContains(layer.insertIds, target.inserts[index - 1].model.id)) branch = &layer;
        }
        // Explicit group targets may only extend their existing range.
        std::size_t first = target.inserts.size(), last = 0;
        for (std::size_t i = 0; i < target.inserts.size(); ++i)
            if (std::find(group->insertIds.begin(), group->insertIds.end(), target.inserts[i].model.id) !=
                group->insertIds.end()) {
                first = std::min(first, i);
                last = i;
            }
        if (first < target.inserts.size()) index = std::clamp(index, first, last + 1);
        if (branch) {
            if (branch->insertIds.empty()) branch->muted = false;
            // Keep branch members adjacent in the flat storage order.
            if (!branch->insertIds.empty()) {
                std::size_t begin = target.inserts.size(), end = 0;
                for (std::size_t i = 0; i < target.inserts.size(); ++i) if (rackContains(branch->insertIds, target.inserts[i].model.id)) { begin = std::min(begin, i); end = i + 1; }
                if (begin < target.inserts.size()) index = std::clamp(index, begin, end);
            } else if (first < target.inserts.size()) index = last + 1;
            branch->insertIds.insert(branch->insertIds.end(), landed.begin() + std::ptrdiff_t(effectStart), landed.end());
        }
        group->insertIds.insert(group->insertIds.end(), landed.begin() + std::ptrdiff_t(effectStart),
                                landed.end());
        auto ancestor = group->parentId;
        for (int depth = 0; !ancestor.empty() && depth < 9; ++depth) {
            auto found = std::find_if(target.rackGroups.begin(), target.rackGroups.end(), [&](const auto& g) { return g.id == ancestor; });
            if (found == target.rackGroups.end()) break;
            for (auto& layer : found->layers) if (rackContains(layer.insertIds, group->insertIds.front()))
                layer.insertIds.insert(layer.insertIds.end(), landed.begin() + std::ptrdiff_t(effectStart), landed.end());
            found->insertIds.insert(found->insertIds.end(), landed.begin() + std::ptrdiff_t(effectStart), landed.end());
            ancestor = found->parentId;
        }
    }
    target.inserts.insert(target.inserts.begin() + std::ptrdiff_t(index), source.inserts.begin(),
                          source.inserts.end());
    auto groups = copyRackGroups(source.rackGroups, mapping, copy);
    for (auto& g : groups) if (g.parentId.empty()) g.parentId = parent;
    target.rackGroups.insert(target.rackGroups.end(), groups.begin(), groups.end());
    auto moved = copyModulation(source.modulation, mapping, copy || copySources, &mapping);
    for (const auto& source : moved.sources)
        if (std::none_of(target.modulation.sources.begin(), target.modulation.sources.end(), [&](const auto& s) { return s.id == source.id; }))
            target.modulation.sources.push_back(source);
    target.modulation.routes.insert(target.modulation.routes.end(), moved.routes.begin(), moved.routes.end());
    auto automation = copyModulationAutomation(source.modulationAutomation, mapping, channel, copy || copySources);
    for (auto& lane : automation) {
        auto found = std::find_if(target.modulationAutomation.begin(), target.modulationAutomation.end(), [&](const auto& t) { return t.id == lane.id; });
        if (found == target.modulationAutomation.end()) target.modulationAutomation.push_back(std::move(lane));
        else for (auto& clip : lane.clips)
            if (std::none_of(found->clips.begin(), found->clips.end(), [&](const auto& c) { return c.id == clip.id; })) found->clips.push_back(std::move(clip));
    }
    normalize(target);
    return true;
}
} // namespace

std::vector<RackGroupModel>* EngineController::mutableRackGroups(const std::string& channel) {
    if (channel == kMasterChannelId)
        return &m_project.masterRackGroups;
    if (auto* track = m_project.findTrack(channel); track && carriesAudio(*track))
        return &track->rackGroups;
    return nullptr;
}
const std::vector<RackGroupModel>& EngineController::rackGroups(const std::string& channel) const {
    static const std::vector<RackGroupModel> empty;
    if (channel == kMasterChannelId)
        return m_project.masterRackGroups;
    if (const auto* track = m_project.findTrack(channel))
        return track->rackGroups;
    return empty;
}

bool EngineController::insertSupportsChannelMode(const std::string& channel, const std::string& slot,
                                                 PluginChannelMode mode) const {
    const auto capabilities = m_runtime->pluginCapabilities({channel, slot});
    return capabilities.available && (mode != PluginChannelMode::DualMono || capabilities.dualMono);
}

bool EngineController::replaceRackGroups(const std::string& channel, std::vector<RackGroupModel> next, const std::string& label) {
    if (cloudProjectBound() || (!m_undo.isApplying() && isTrackEditLocked(channel))) return false;
    auto* groups = mutableRackGroups(channel);
    const auto* chain = channelInserts(channel);
    if (!groups || !chain) return false;
    normalizeRackGroups(next, *chain);
    const auto before = *groups;
    if (before == next) return true;
    const auto parallel = [](const auto& state) { return std::any_of(state.begin(), state.end(), [](const auto& g) { return !g.layers.empty(); }); };
    *groups = next;
    if ((parallel(before) || parallel(next)) && !rebuildGraph()) {
        *mutableRackGroups(channel) = before; rebuildGraph(); return false;
    }
    m_undo.push(label, [this, channel, before, label] { replaceRackGroups(channel, before, label); },
        [this, channel, next, label] { replaceRackGroups(channel, next, label); });
    return true;
}
bool EngineController::createRackGroup(const std::string& channel, const std::vector<std::string>& ids,
    const std::string& name, std::string* result, bool parallel) {
    if (cloudProjectBound() || ids.empty() || (!parallel && ids.size() < 2) || (!m_undo.isApplying() && isTrackEditLocked(channel))) return false;
    const auto* chain = channelInserts(channel); if (!chain) return false;
    std::vector<std::string> ordered;
    std::size_t first = chain->size(), last = 0;
    for (std::size_t i = 0; i < chain->size(); ++i) if (rackContains(ids, (*chain)[i].id) && (*chain)[i].isLoaded()) {
        if (const auto* instance = m_runtime->pluginInstance({channel, (*chain)[i].id}); instance && instance->descriptor().isInstrument) return false;
        first = std::min(first, i); last = i; ordered.push_back((*chain)[i].id);
    }
    if (ordered.size() != ids.size() || last - first + 1 != ordered.size()) return false;
    auto groups = rackGroups(channel);
    std::string parent;
    std::size_t parentSize = chain->size() + 1;
    for (const auto& g : groups) {
        const bool intersects = std::any_of(ordered.begin(), ordered.end(), [&](const auto& id) { return rackContains(g.insertIds, id); });
        if (!intersects) continue;
        const bool contains = std::all_of(ordered.begin(), ordered.end(), [&](const auto& id) { return rackContains(g.insertIds, id); });
        const bool wrapped = std::all_of(g.insertIds.begin(), g.insertIds.end(), [&](const auto& id) { return rackContains(ordered, id); });
        if (!contains && !wrapped) return false;
        if (contains && !wrapped && g.insertIds.size() < parentSize) { parent = g.id; parentSize = g.insertIds.size(); }
    }
    for (const auto& g : groups) if (g.id == parent && !g.layers.empty()) {
        if (std::none_of(g.layers.begin(), g.layers.end(), [&](const auto& layer) {
            return std::all_of(ordered.begin(), ordered.end(), [&](const auto& id) { return rackContains(layer.insertIds, id); });
        })) return false;
    }
    const auto id = newUuid();
    RackGroupModel created{id, name.empty() ? (parallel ? "Layers" : "Group") : name, ordered, parent, {}};
    for (auto& g : groups) if (g.parentId == parent &&
        std::all_of(g.insertIds.begin(), g.insertIds.end(), [&](const auto& member) { return rackContains(ordered, member); })) g.parentId = id;
    if (parallel) {
        for (std::size_t i = 0; i < ordered.size();) {
            RackLayerModel layer{newUuid(), {}, {ordered[i]}};
            for (const auto& g : groups) if (g.parentId == id && g.insertIds.front() == ordered[i]) { layer.insertIds = g.insertIds; layer.name = g.name; break; }
            if (layer.name.empty()) if (const auto* slot = insertModel(channel, ordered[i])) layer.name = slot->name;
            i += layer.insertIds.size(); created.layers.push_back(std::move(layer));
        }
        if (created.layers.size() == 1) created.layers.push_back({newUuid(), "Dry", {}, 1.f, true, false});
    }
    groups.push_back(std::move(created));
    for (const auto& g : groups) {
        auto parent = g.parentId;
        for (int depth = 0; !parent.empty(); ++depth) {
            if (depth >= 8) return false;
            const auto found = std::find_if(groups.begin(), groups.end(), [&](const auto& candidate) { return candidate.id == parent; });
            if (found == groups.end()) break;
            parent = found->parentId;
        }
    }
    if (!replaceRackGroups(channel, std::move(groups), parallel ? "Create Parallel Layers" : "Group Plugins")) return false;
    if (std::none_of(rackGroups(channel).begin(), rackGroups(channel).end(), [&](const auto& g) { return g.id == id; })) return false;
    if (result) *result = id;
    return true;
}
bool EngineController::removeRackGroup(const std::string& channel, const std::string& id) {
    auto groups = rackGroups(channel);
    const auto found = std::find_if(groups.begin(), groups.end(), [&](const auto& g) { return g.id == id; });
    if (found == groups.end()) return false;
    const auto parent = found->parentId;
    for (auto& g : groups) if (g.parentId == id) g.parentId = parent;
    std::erase_if(groups, [&](const auto& g) { return g.id == id; });
    return replaceRackGroups(channel, std::move(groups), "Ungroup Plugins");
}
bool EngineController::renameRackGroup(const std::string& channel, const std::string& id, const std::string& name) {
    if (name.empty()) return false;
    auto groups = rackGroups(channel);
    for (auto& g : groups) if (g.id == id) { g.name = name; return replaceRackGroups(channel, std::move(groups), "Rename Plugin Group"); }
    return false;
}
bool EngineController::addRackLayer(const std::string& channel, const std::string& id) {
    auto groups = rackGroups(channel);
    for (auto& g : groups) if (g.id == id && !g.layers.empty() && g.layers.size() < 32) {
        g.layers.push_back({newUuid(), "Layer " + std::to_string(g.layers.size() + 1), {}, 1.f, true, false});
        return replaceRackGroups(channel, std::move(groups), "Add Layer");
    }
    return false;
}
bool EngineController::removeRackLayer(const std::string& channel, const std::string& id, const std::string& layerId) {
    if (cloudProjectBound() || (!m_undo.isApplying() && isTrackEditLocked(channel))) return false;
    RackChannelState captured;
    if (!captureRackChannel(channel, false, captured)) return false;
    RackEditState before{{std::move(captured)}, {}}, after = before;
    auto& snapshot = after.channels.front().snapshot;
    std::vector<std::string> members;
    bool found = false;
    for (auto& g : snapshot.rackGroups) if (g.id == id && g.layers.size() > 1)
        for (const auto& layer : g.layers) if (layer.id == layerId) { members = layer.insertIds; found = true; }
    if (!found) return false;
    lift(snapshot, std::unordered_set<std::string>(members.begin(), members.end()));
    for (auto& g : snapshot.rackGroups) if (g.id == id)
        std::erase_if(g.layers, [&](const auto& layer) { return layer.id == layerId; });
    return bool(commitRackEdit(before, after, "Remove Layer"));
}
bool EngineController::setRackLayerState(const std::string& channel, const std::string& id, const std::string& layerId,
    float gain, bool muted, bool solo) {
    if (cloudProjectBound() || !std::isfinite(gain) || (!m_undo.isApplying() && isTrackEditLocked(channel))) return false;
    auto* groups = mutableRackGroups(channel); if (!groups) return false;
    for (auto& g : *groups) if (g.id == id) for (auto& layer : g.layers) if (layer.id == layerId) {
        const auto before = layer; layer.gain = std::clamp(gain, 0.f, 2.f); layer.muted = muted; layer.solo = solo;
        const auto after = layer; if (before == after) return true;
        if (auto* track = m_project.findTrack(channel); track && track->freeze.active()) { track->freeze = {}; rebuildGraph(); }
        else m_runtime->updateRackLayers(channel, *groups);
        m_undo.push("Change Layer Mix",
            [this, channel, id, layerId, before] { setRackLayerState(channel, id, layerId, before.gain, before.muted, before.solo); },
            [this, channel, id, layerId, after] { setRackLayerState(channel, id, layerId, after.gain, after.muted, after.solo); });
        return true;
    }
    return false;
}
bool EngineController::renameRackLayer(const std::string& channel, const std::string& id, const std::string& layerId, const std::string& name) {
    if (name.empty()) return false;
    auto groups = rackGroups(channel);
    for (auto& g : groups) if (g.id == id) for (auto& layer : g.layers) if (layer.id == layerId) {
        layer.name = name; return replaceRackGroups(channel, std::move(groups), "Rename Layer");
    }
    return false;
}
bool EngineController::setRackParameters(const std::string& channel, const std::string& id,
                                         const std::vector<std::string>& parameters) {
    if (!m_undo.isApplying() && isTrackEditLocked(channel)) return false;
    if (cloudProjectBound() || parameters.size() > 8)
        return false;
    auto* slot = mutableInsertSlot(channel, id);
    if (!slot || slot->rackParameterIds == parameters)
        return false;
    const auto before = slot->rackParameterIds;
    slot->rackParameterIds = parameters;
    const auto put = [this, channel, id](const auto& pins) {
        if (auto* s = mutableInsertSlot(channel, id))
            s->rackParameterIds = pins;
    };
    m_undo.push(
        "Configure Rack Parameters", [put, before] { put(before); }, [put, parameters] { put(parameters); });
    return true;
}

audio::Result EngineController::captureRackSelection(const std::string& channel,
                                                     const std::vector<std::string>& ids,
                                                     ChannelSnapshot& out) {
    out = {};
    const auto* chain = channelInserts(channel);
    if (!chain)
        return rackError("Channel is no longer available.");
    const std::unordered_set<std::string> selected(ids.begin(), ids.end());
    ChannelSnapshot snapshot;
    std::unordered_map<std::string, std::string> mapping;
    const auto* track = m_project.findTrack(channel);
    if (track && track->instrument.isLoaded() && selected.contains(track->instrument.id)) {
        ChainSlotSnapshot instrument;
        if (const auto result = captureInsertState(channel, track->instrument, instrument);
            !result)
            return result;
        snapshot.instrument = std::move(instrument);
        if (track->samplerFx.isOwnedBy(track->instrument)) {
            snapshot.instrumentFxVolume = track->samplerFx.volume;
            snapshot.instrumentFxPan = track->samplerFx.pan;
            for (const auto& fx : track->samplerFx.inserts) {
                ChainSlotSnapshot captured;
                if (const auto result = captureInsertState(channel, fx, captured);
                    !result)
                    return result;
                snapshot.instrumentFx.push_back(std::move(captured));
            }
        }
    }
    for (const auto& model : *chain)
        if (selected.contains(model.id)) {
            ChainSlotSnapshot captured;
            const auto result = captureInsertState(channel, model, captured);
            if (!result)
                return result;
            snapshot.inserts.push_back(std::move(captured));
            mapping[model.id] = model.id;
        }
    if (snapshot.inserts.size() + (snapshot.instrument ? 1 : 0) != selected.size())
        return rackError("A selected plugin is no longer available.");
    if (snapshot.instrument) mapping[snapshot.instrument->model.id] = snapshot.instrument->model.id;
    snapshot.modulation = copyModulation(rackModulation(channel), mapping, false);
    snapshot.modulationAutomation = captureModulationAutomation(m_project, channel, snapshot.modulation);
    snapshot.rackGroups = copyRackGroups(rackGroups(channel), mapping, false);
    snapshot.sourceName = channel == kMasterChannelId ? "Master" : m_project.findTrack(channel)->name;
    out = std::move(snapshot);
    return audio::Result::ok();
}

audio::Result EngineController::captureRackChannel(const std::string& channel, bool instrument,
                                                   RackChannelState& state) {
    const auto* chain = channelInserts(channel);
    if (!chain)
        return rackError("Channel is no longer available.");
    std::vector<std::string> ids;
    for (const auto& slot : *chain)
        ids.push_back(slot.id);
    if (instrument) {
        const auto* track = m_project.findTrack(channel);
        if (!track || !trackAccepts(track->kind, ClipKind::Midi))
            return rackError("Choose an instrument channel for this device.");
        if (track->instrument.isLoaded())
            ids.push_back(track->instrument.id);
    }
    state.channelId = channel;
    state.instrumentChanged = instrument;
    auto result = captureRackSelection(channel, ids, state.snapshot);
    if (result) {
        state.snapshot.modulation = rackModulation(channel);
        state.snapshot.modulationAutomation = captureModulationAutomation(m_project, channel, state.snapshot.modulation);
    }
    return result;
}

bool EngineController::applyRackEdit(const RackEditState& state) {
    std::vector<ChainReplacement> replacements;
    for (const auto& channel : state.channels) {
        if (!m_undo.isApplying() && isTrackEditLocked(channel.channelId)) return false;
        if (!mutableRackGroups(channel.channelId))
            return false;
        replacements.push_back({channel.channelId, channel.snapshot.inserts,
                                channel.instrumentChanged ? &channel.snapshot : nullptr});
    }
    const auto previousProject = m_project;
    for (const auto& channel : state.channels) {
        auto* rack = mutableRackModulation(channel.channelId);
        *rack = channel.snapshot.modulation;
        // Publish routing and device changes in the same audio session.
        *mutableRackGroups(channel.channelId) = channel.snapshot.rackGroups;
    }
    for (const auto& channel : state.channels)
        replaceModulationAutomation(m_project, channel.channelId, channel.snapshot.modulationAutomation);
    invalidateAutomationReadoutCache();
    std::vector<RackAutomationAddress> previous;
    for (const auto& address : state.automation)
        if (auto* clip = findClip(address.trackId, address.clipId)) {
            previous.push_back({address.trackId, address.clipId, clip->automation.target});
            clip->automation.target = address.target;
        }
    if (!applyChains(replacements)) {
        m_project = previousProject;
        invalidateAutomationReadoutCache();
        rebuildGraph();
        for (const auto& address : previous)
            if (auto* clip = findClip(address.trackId, address.clipId))
                clip->automation.target = address.target;
        return false;
    }
    return true;
}
bool EngineController::commitRackEdit(const RackEditState& before, const RackEditState& after,
                                      const std::string& label) {
    if (!applyRackEdit(after))
        return false;
    const auto stateBytes = [](const RackEditState& state) {
        std::size_t bytes = sizeof(state) + state.automation.size() * sizeof(RackAutomationAddress);
        const auto slotBytes = [](const ChainSlotSnapshot& slot) {
            std::size_t result = sizeof(slot) + slot.state.size() + slot.rightState.size();
            result += slot.model.name.size() + slot.model.uid.size() + slot.model.path.size();
            for (const auto& parameter : slot.model.parameters)
                result += sizeof(parameter) + parameter.id.size();
            for (const auto& parameter : slot.model.rightParameters)
                result += sizeof(parameter) + parameter.id.size();
            for (const auto& id : slot.model.rackParameterIds)
                result += sizeof(id) + id.size();
            return result;
        };
        for (const auto& channel : state.channels) {
            bytes += sizeof(channel) + channel.channelId.size();
            if (channel.snapshot.instrument)
                bytes += slotBytes(*channel.snapshot.instrument);
            for (const auto& slot : channel.snapshot.inserts)
                bytes += slotBytes(slot);
            for (const auto& slot : channel.snapshot.instrumentFx)
                bytes += slotBytes(slot);
            for (const auto& group : channel.snapshot.rackGroups) {
                bytes += sizeof(group) + group.id.size() + group.name.size() + group.parentId.size();
                for (const auto& id : group.insertIds)
                    bytes += sizeof(id) + id.size();
                for (const auto& layer : group.layers) {
                    bytes += sizeof(layer) + layer.id.size() + layer.name.size();
                    for (const auto& id : layer.insertIds) bytes += sizeof(id) + id.size();
                }
            }
        }
        return bytes;
    };
    m_undo.push(
        label, [this, before] { applyRackEdit(before); }, [this, after] { applyRackEdit(after); },
        stateBytes(before) + stateBytes(after));
    return true;
}

audio::Result EngineController::pasteRackSelection(const std::string& channel, const ChannelSnapshot& source,
                                                   std::size_t index, const std::string& groupId,
                                                   std::vector<std::string>* added) {
    if (!m_undo.isApplying() && isTrackEditLocked(channel)) return rackError("Track editing is locked.");
    if (!source.sourceDocument.empty() && source.sourceDocument != m_documentIdentity)
        return pasteRackSelection(channel, prepareChannelTransfer(source), index, groupId, added);
    if (source.resources) m_transferResources.push_back(source.resources);
    if (added)
        added->clear();
    if (!mutableRackGroups(channel) || (source.inserts.empty() && !source.instrument))
        return rackError("Rack paste requires a local audio channel.");
    if (cloudProjectBound()) {
        if (!groupId.empty())
            return rackError("Rack groups are available in local projects.");
        const bool cut =
            !source.rackCutSource.empty() &&
            (!source.instrument || !insertModel(source.rackCutSource, source.instrument->model.id)) &&
            std::none_of(source.inserts.begin(), source.inserts.end(), [&](const auto& slot) {
                return insertModel(source.rackCutSource, slot.model.id) != nullptr;
            });
        const auto result = submitSharedRackTransfer(cut ? source.rackCutSource : std::string{}, source,
                                                     channel, index, !cut, added);
        if (result && cut)
            sharedChannelClipboard().rackCutSource.clear();
        return result;
    }
    RackChannelState captured;
    if (const auto result = captureRackChannel(channel, bool(source.instrument), captured); !result)
        return result;
    RackEditState before{{std::move(captured)}, {}}, after = before;
    bool cut = !source.rackCutSource.empty();
    std::unordered_set<std::string> moving;
    for (const auto& slot : source.inserts)
        moving.insert(slot.model.id);
    if (source.instrument)
        moving.insert(source.instrument->model.id);
    for (const auto& fx : source.instrumentFx)
        moving.insert(fx.model.id);
    // Undoing Cut while its clipboard survives turns the next paste into a
    // copy. A live identity must never be published on two channels.
    if (cut) {
        for (const auto& track : m_project.tracks) {
            if (moving.contains(track.instrument.id))
                cut = false;
            for (const auto& slot : track.inserts)
                if (moving.contains(slot.id))
                    cut = false;
        }
        for (const auto& slot : m_project.masterInserts)
            if (moving.contains(slot.id))
                cut = false;
    }
    if (cut) {
        for (const auto& track : m_project.tracks)
            for (const auto& clip : track.clips)
                if (clip.kind == ClipKind::Automation &&
                    clip.automation.target.channelId == source.rackCutSource &&
                    (moving.contains(clip.automation.target.slotId) ||
                     (source.instrument && clip.automation.target.slotId.empty())))
                    before.automation.push_back({track.id, clip.id, clip.automation.target});
        after.automation = before.automation;
        for (auto& address : after.automation)
            address.target.channelId = channel;
    }
    std::vector<std::string> landed;
    if (!place(after.channels[0].snapshot, source, index, groupId, !cut, landed, channel, cut && source.rackCutSource != channel))
        return rackError("The destination group is unavailable.");
    if (!commitRackEdit(before, after, "Paste Rack Devices"))
        return rackError("Could not restore plugin settings. The chain was left unchanged.");
    if (added)
        *added = std::move(landed);
    if (cut)
        sharedChannelClipboard().rackCutSource.clear();
    return audio::Result::ok();
}
audio::Result EngineController::transferRackSelection(const std::string& from,
                                                      const std::vector<std::string>& ids,
                                                      const std::string& to, std::size_t index, bool copy,
                                                      const std::string& groupId,
                                                      std::vector<std::string>* landed) {
    if (!m_undo.isApplying() && (isTrackEditLocked(to) || (!copy && isTrackEditLocked(from))))
        return rackError("Track editing is locked.");
    if (landed)
        landed->clear();
    if (ids.empty() || !mutableRackGroups(from) || !mutableRackGroups(to))
        return rackError("Rack transfer requires local audio channels.");
    ChannelSnapshot source;
    if (const auto result = captureRackSelection(from, ids, source); !result)
        return result;
    if (cloudProjectBound()) {
        if (!groupId.empty())
            return rackError("Rack groups are available in local projects.");
        return submitSharedRackTransfer(from, source, to, index, copy, landed);
    }
    if (copy)
        return pasteRackSelection(to, source, index, groupId, landed);
    if (from == to && !groupId.empty()) {
        std::string container;
        for (const auto& g : rackGroups(from))
            if (g.id == groupId || std::any_of(g.layers.begin(), g.layers.end(), [&](const auto& layer) { return layer.id == groupId; })) container = g.id;
        // Moving devices between branches does not move their enclosing rack into itself.
        for (int depth = 0; !container.empty() && depth < 9; ++depth) {
            std::erase_if(source.rackGroups, [&](const auto& g) { return g.id == container; });
            const auto found = std::find_if(rackGroups(from).begin(), rackGroups(from).end(), [&](const auto& g) { return g.id == container; });
            container = found == rackGroups(from).end() ? std::string{} : found->parentId;
        }
        for (auto& g : source.rackGroups)
            if (std::none_of(source.rackGroups.begin(), source.rackGroups.end(), [&](const auto& parent) { return parent.id == g.parentId; })) g.parentId.clear();
    }
    std::unordered_set<std::string> selected(ids.begin(), ids.end());
    for (const auto& fx : source.instrumentFx)
        selected.insert(fx.model.id);
    RackChannelState captured;
    if (const auto result = captureRackChannel(from, bool(source.instrument), captured); !result)
        return result;
    RackEditState before{{std::move(captured)}, {}}, after;
    if (from != to) {
        if (const auto result = captureRackChannel(to, bool(source.instrument), captured); !result)
            return result;
        before.channels.push_back(std::move(captured));
    }
    for (const auto& track : m_project.tracks)
        for (const auto& clip : track.clips)
            if (clip.kind == ClipKind::Automation &&
                clip.automation.target.kind == AutomationTargetKind::PluginParameter &&
                clip.automation.target.channelId == from &&
                (selected.contains(clip.automation.target.slotId) ||
                 (source.instrument && clip.automation.target.slotId.empty())))
                before.automation.push_back({track.id, clip.id, clip.automation.target});
    after = before;
    if (from == to) {
        const auto& chain = before.channels[0].snapshot.inserts;
        index = std::min(index, chain.size());
        const auto liftedBefore = std::count_if(chain.begin(), chain.begin() + std::ptrdiff_t(index),
                                                [&](const auto& s) { return selected.contains(s.model.id); });
        index -= std::size_t(liftedBefore);
    }
    lift(after.channels[0].snapshot, selected, from != to || groupId.empty());
    // Fully moved containers travel with the captured subtree, not as empty duplicates.
    for (const auto& g : source.rackGroups)
        std::erase_if(after.channels[0].snapshot.rackGroups, [&](const auto& old) { return old.id == g.id; });
    // Moving all members within the same named group retains the container.
    std::string destination = groupId;
    for (const auto& group : source.rackGroups)
        if (group.id == destination)
            destination.clear();
    std::vector<std::string> newIds;
    if (!place(after.channels.back().snapshot, source, index, destination, false, newIds, to, from != to))
        return rackError("The destination group is unavailable.");
    for (auto& address : after.automation)
        address.target.channelId = to;
    if (from == to && before.channels[0].snapshot.rackGroups == after.channels[0].snapshot.rackGroups) {
        const auto& old = before.channels[0].snapshot.inserts;
        const auto& next = after.channels[0].snapshot.inserts;
        if (old.size() == next.size() &&
            std::equal(old.begin(), old.end(), next.begin(),
                       [](const auto& a, const auto& b) { return a.model.id == b.model.id; })) {
            if (landed)
                *landed = std::move(newIds);
            return audio::Result::ok();
        }
    }
    if (!commitRackEdit(before, after, "Move Rack Devices"))
        return rackError("Could not move plugins. The chains were left unchanged.");
    if (landed)
        *landed = std::move(newIds);
    return audio::Result::ok();
}
bool EngineController::removeRackSelection(const std::string& channel, const std::vector<std::string>& ids) {
    if (ids.empty() || !mutableRackGroups(channel))
        return false;
    ChannelSnapshot captured;
    if (!captureRackSelection(channel, ids, captured))
        return false;
    if (cloudProjectBound()) {
        auto batch = std::make_shared<collab::BatchCommand>();
        const collab::PluginLocation location =
            channel == kMasterChannelId ? collab::PluginLocation{collab::PluginChain::Master, {}, {}}
                                        : collab::PluginLocation{collab::PluginChain::Track, channel, {}};
        for (const auto& slot : captured.inserts) {
            collab::ProjectCommand command;
            command.body = collab::DeletePluginInsert{location, slot.model.id};
            batch->commands.push_back(std::move(command));
        }
        if (captured.instrument) {
            collab::ProjectCommand command;
            command.body = collab::DeletePluginInsert{{collab::PluginChain::Instrument, channel, {}},
                                                      captured.instrument->model.id};
            batch->commands.push_back(std::move(command));
        }
        return submitSharedMutation(collab::CommandBody{std::move(batch)}, "Delete Rack Devices") ==
               collab::SharedMutationResult::Submitted;
    }
    RackChannelState state;
    if (!captureRackChannel(channel, bool(captured.instrument), state))
        return false;
    RackEditState before{{std::move(state)}, {}}, after = before;
    lift(after.channels[0].snapshot, {ids.begin(), ids.end()});
    return commitRackEdit(before, after, "Delete Rack Devices");
}
bool EngineController::bypassRackSelection(const std::string& channel, const std::vector<std::string>& ids,
                                           bool bypassed) {
    if (ids.empty())
        return false;
    if (!cloudProjectBound()) {
        ChannelSnapshot selected;
        if (!captureRackSelection(channel, ids, selected))
            return false;
        RackChannelState captured;
        if (!captureRackChannel(channel, bool(selected.instrument), captured))
            return false;
        RackEditState before{{std::move(captured)}, {}}, after = before;
        const std::unordered_set<std::string> selection(ids.begin(), ids.end());
        bool changed = false;
        const auto apply = [&](ChainSlotSnapshot& slot) {
            if (selection.contains(slot.model.id) && slot.model.bypassed != bypassed) {
                slot.model.bypassed = bypassed;
                changed = true;
            }
        };
        auto& snapshot = after.channels[0].snapshot;
        if (snapshot.instrument)
            apply(*snapshot.instrument);
        for (auto& slot : snapshot.inserts)
            apply(slot);
        return changed &&
               commitRackEdit(before, after, bypassed ? "Bypass Rack Devices" : "Enable Rack Devices");
    }
    const auto group = beginUndoGroup();
    bool changed = false;
    for (const auto& id : ids)
        if (const auto* slot = insertModel(channel, id); slot && slot->bypassed != bypassed) {
            setInsertBypassed(channel, id, bypassed);
            changed = true;
        }
    collapseUndo(group, bypassed ? "Bypass Rack Devices" : "Enable Rack Devices");
    return changed;
}
} // namespace daw
