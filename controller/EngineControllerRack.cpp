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
void lift(EngineController::ChannelSnapshot& snapshot, const std::unordered_set<std::string>& ids) {
    if (snapshot.instrument && ids.contains(snapshot.instrument->model.id)) {
        snapshot.instrument.reset();
        snapshot.instrumentFx.clear();
    }
    std::erase_if(snapshot.inserts, [&](const auto& s) { return ids.contains(s.model.id); });
    for (auto& group : snapshot.rackGroups)
        std::erase_if(group.insertIds, [&](const auto& id) { return ids.contains(id); });
    normalize(snapshot);
}
bool place(EngineController::ChannelSnapshot& target, EngineController::ChannelSnapshot source,
           std::size_t index, const std::string& groupId, bool copy, std::vector<std::string>& landed) {
    if (source.instrument && !groupId.empty())
        return false;
    auto group = std::find_if(target.rackGroups.begin(), target.rackGroups.end(),
                              [&](const auto& g) { return g.id == groupId; });
    if (!groupId.empty() && (group == target.rackGroups.end() || !source.rackGroups.empty()))
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
                if (!source.rackGroups.empty())
                    return false;
                group = it;
                break;
            }
        }
    }
    std::unordered_map<std::string, std::string> mapping;
    if (source.instrument) {
        if (copy) {
            source.instrument->model.id = newUuid();
            source.instrument->model.windowOpen = false;
            for (auto& fx : source.instrumentFx) {
                fx.model.id = newUuid();
                fx.model.windowOpen = false;
            }
        }
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
    if (group != target.rackGroups.end()) {
        // Explicit group targets may only extend their existing range.
        std::size_t first = target.inserts.size(), last = 0;
        for (std::size_t i = 0; i < target.inserts.size(); ++i)
            if (std::find(group->insertIds.begin(), group->insertIds.end(), target.inserts[i].model.id) !=
                group->insertIds.end()) {
                first = std::min(first, i);
                last = i;
            }
        index = std::clamp(index, first, last + 1);
        group->insertIds.insert(group->insertIds.end(), landed.begin() + std::ptrdiff_t(effectStart),
                                landed.end());
    }
    target.inserts.insert(target.inserts.begin() + std::ptrdiff_t(index), source.inserts.begin(),
                          source.inserts.end());
    const auto groups = copyRackGroups(source.rackGroups, mapping, copy);
    target.rackGroups.insert(target.rackGroups.end(), groups.begin(), groups.end());
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

bool EngineController::createRackGroup(const std::string& channel, const std::vector<std::string>& ids,
                                       const std::string& name, std::string* result) {
    if (cloudProjectBound() || ids.size() < 2)
        return false;
    auto* groups = mutableRackGroups(channel);
    const auto* chain = channelInserts(channel);
    if (!groups || !chain)
        return false;
    const std::unordered_set<std::string> selected(ids.begin(), ids.end());
    std::vector<std::string> ordered;
    std::size_t first = chain->size(), last = 0;
    for (std::size_t i = 0; i < chain->size(); ++i)
        if (selected.contains((*chain)[i].id) && (*chain)[i].isLoaded()) {
            first = std::min(first, i);
            last = i;
            ordered.push_back((*chain)[i].id);
        }
    if (ordered.size() != ids.size() || last - first + 1 != ids.size())
        return false;
    for (const auto& group : *groups)
        for (const auto& id : group.insertIds)
            if (selected.contains(id))
                return false;
    const auto before = *groups;
    const auto id = newUuid();
    groups->push_back({id, name.empty() ? "Group" : name, ordered});
    normalizeRackGroups(*groups, *chain);
    const auto after = *groups;
    const auto put = [this, channel](const auto& state) {
        if (auto* g = mutableRackGroups(channel))
            *g = state;
    };
    m_undo.push("Group Plugins", [put, before] { put(before); }, [put, after] { put(after); });
    if (result)
        *result = id;
    return true;
}
bool EngineController::removeRackGroup(const std::string& channel, const std::string& id) {
    if (cloudProjectBound())
        return false;
    auto* groups = mutableRackGroups(channel);
    if (!groups)
        return false;
    const auto before = *groups;
    if (!std::erase_if(*groups, [&](const auto& g) { return g.id == id; }))
        return false;
    const auto after = *groups;
    const auto put = [this, channel](const auto& state) {
        if (auto* g = mutableRackGroups(channel))
            *g = state;
    };
    m_undo.push("Ungroup Plugins", [put, before] { put(before); }, [put, after] { put(after); });
    return true;
}
bool EngineController::renameRackGroup(const std::string& channel, const std::string& id,
                                       const std::string& name) {
    if (cloudProjectBound() || name.empty())
        return false;
    auto* groups = mutableRackGroups(channel);
    if (!groups)
        return false;
    auto it = std::find_if(groups->begin(), groups->end(), [&](const auto& g) { return g.id == id; });
    if (it == groups->end() || it->name == name)
        return false;
    const auto before = it->name;
    it->name = name;
    const auto put = [this, channel, id](const auto& text) {
        if (auto* list = mutableRackGroups(channel))
            for (auto& group : *list)
                if (group.id == id)
                    group.name = text;
    };
    m_undo.push("Rename Plugin Group", [put, before] { put(before); }, [put, name] { put(name); });
    return true;
}
bool EngineController::setRackParameters(const std::string& channel, const std::string& id,
                                         const std::vector<std::string>& parameters) {
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
            !result && hasInsert(channel, track->instrument.id))
            return result;
        snapshot.instrument = std::move(instrument);
        if (track->samplerFx.isOwnedBy(track->instrument)) {
            snapshot.instrumentFxVolume = track->samplerFx.volume;
            snapshot.instrumentFxPan = track->samplerFx.pan;
            for (const auto& fx : track->samplerFx.inserts) {
                ChainSlotSnapshot captured;
                if (const auto result = captureInsertState(channel, fx, captured);
                    !result && hasInsert(channel, fx.id))
                    return result;
                snapshot.instrumentFx.push_back(std::move(captured));
            }
        }
    }
    for (const auto& model : *chain)
        if (selected.contains(model.id)) {
            ChainSlotSnapshot captured;
            const auto result = captureInsertState(channel, model, captured);
            if (!result && hasInsert(channel, model.id))
                return result;
            snapshot.inserts.push_back(std::move(captured));
            mapping[model.id] = model.id;
        }
    if (snapshot.inserts.size() + (snapshot.instrument ? 1 : 0) != selected.size())
        return rackError("A selected plugin is no longer available.");
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
    return captureRackSelection(channel, ids, state.snapshot);
}

bool EngineController::applyRackEdit(const RackEditState& state) {
    std::vector<ChainReplacement> replacements;
    for (const auto& channel : state.channels) {
        if (!mutableRackGroups(channel.channelId))
            return false;
        replacements.push_back({channel.channelId, channel.snapshot.inserts,
                                channel.instrumentChanged ? &channel.snapshot : nullptr});
    }
    std::vector<RackAutomationAddress> previous;
    for (const auto& address : state.automation)
        if (auto* clip = findClip(address.trackId, address.clipId)) {
            previous.push_back({address.trackId, address.clipId, clip->automation.target});
            clip->automation.target = address.target;
        }
    if (!applyChains(replacements)) {
        for (const auto& address : previous)
            if (auto* clip = findClip(address.trackId, address.clipId))
                clip->automation.target = address.target;
        return false;
    }
    for (const auto& channel : state.channels)
        *mutableRackGroups(channel.channelId) = channel.snapshot.rackGroups;
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
                bytes += sizeof(group) + group.id.size() + group.name.size();
                for (const auto& id : group.insertIds)
                    bytes += sizeof(id) + id.size();
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
            m_channelClipboard.rackCutSource.clear();
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
    if (!place(after.channels[0].snapshot, source, index, groupId, !cut, landed))
        return rackError("Groups cannot be nested.");
    if (!commitRackEdit(before, after, "Paste Rack Devices"))
        return rackError("Could not restore plugin settings. The chain was left unchanged.");
    if (added)
        *added = std::move(landed);
    if (cut)
        m_channelClipboard.rackCutSource.clear();
    return audio::Result::ok();
}
audio::Result EngineController::transferRackSelection(const std::string& from,
                                                      const std::vector<std::string>& ids,
                                                      const std::string& to, std::size_t index, bool copy,
                                                      const std::string& groupId,
                                                      std::vector<std::string>* landed) {
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
    lift(after.channels[0].snapshot, selected);
    // Moving all members within the same named group retains the container.
    std::string destination = groupId;
    for (const auto& group : source.rackGroups)
        if (group.id == destination)
            destination.clear();
    std::vector<std::string> newIds;
    if (!place(after.channels.back().snapshot, source, index, destination, false, newIds))
        return rackError("Groups cannot be nested.");
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
