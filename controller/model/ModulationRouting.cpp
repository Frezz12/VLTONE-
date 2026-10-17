#include "model/ModulationRouting.hpp"
#include <functional>

namespace daw {
namespace {
bool isModulationTarget(const AutomationTarget& target) {
    return target.kind == AutomationTargetKind::ModulatorRate || target.kind == AutomationTargetKind::ModulationDepth;
}
}
std::vector<TrackModel> captureModulationAutomation(const ProjectModel& project, const std::string& channel,
                                                    const modulation::Rack& rack) {
    std::unordered_set<std::string> ids;
    for (const auto& source : rack.sources) ids.insert(source.id);
    for (const auto& route : rack.routes) ids.insert(route.id);
    std::vector<TrackModel> result;
    for (const auto& track : project.tracks) {
        if (!isAutomationLane(track)) continue;
        TrackModel lane = track; lane.clips.clear();
        for (const auto& clip : track.clips) if (clip.kind == ClipKind::Automation &&
            clip.automation.target.channelId == channel && isModulationTarget(clip.automation.target) &&
            ids.contains(clip.automation.target.slotId)) lane.clips.push_back(clip);
        if (!lane.clips.empty()) result.push_back(std::move(lane));
    }
    return result;
}
std::vector<TrackModel> copyModulationAutomation(const std::vector<TrackModel>& lanes,
    const std::unordered_map<std::string, std::string>& ids, const std::string& channel, bool fresh) {
    std::vector<TrackModel> result;
    for (auto lane : lanes) {
        if (fresh) lane.id = newUuid();
        lane.parentId = channel;
        std::erase_if(lane.clips, [&](auto& clip) {
            const auto target = ids.find(clip.automation.target.slotId);
            if (target == ids.end()) return true;
            clip.automation.target.slotId = target->second; clip.automation.target.channelId = channel;
            if (fresh) {
                clip.id = newUuid();
                clip.detachContent();
                clip.patternPartId.clear();
                for (auto& point : clip.automation.points) point.id = newUuid();
            }
            return false;
        });
        if (!lane.clips.empty()) result.push_back(std::move(lane));
    }
    return result;
}
void replaceModulationAutomation(ProjectModel& project, const std::string& channel,
                                 const std::vector<TrackModel>& lanes) {
    std::erase_if(project.tracks, [&](auto& track) {
        if (!isAutomationLane(track)) return false;
        const auto count = std::erase_if(track.clips, [&](const auto& clip) {
            return clip.kind == ClipKind::Automation && clip.automation.target.channelId == channel && isModulationTarget(clip.automation.target);
        });
        return count && track.clips.empty();
    });
    for (const auto& lane : lanes) {
        auto found = std::find_if(project.tracks.begin(), project.tracks.end(), [&](const auto& t) { return t.id == lane.id; });
        if (found == project.tracks.end()) project.tracks.push_back(lane);
        else for (const auto& clip : lane.clips)
            if (std::none_of(found->clips.begin(), found->clips.end(), [&](const auto& c) { return c.id == clip.id; })) found->clips.push_back(clip);
    }
    project.invalidateTrackIndex(); project.invalidateStructure();
}
bool validModulationRouting(const ProjectModel& project, const std::string& overrideChannel,
                            const modulation::Rack* replacement) {
    const auto rackFor = [&](const std::string& id) -> const modulation::Rack& {
        if (replacement && id == overrideChannel) return *replacement;
        return id == "master" ? project.masterModulation : project.findTrack(id)->modulation;
    };
    bool any = !rackFor("master").routes.empty();
    for (const auto& track : project.tracks) if (carriesAudio(track)) any |= !rackFor(track.id).routes.empty();
    if (!any) return true;
    // Document-only topology mirrors the three audio taps and plugin ordering.
    // Reject local edits before touching the running graph or loading plugins.
    std::unordered_map<std::string, std::vector<std::string>> edges;
    std::unordered_map<std::string, const InsertModel*> slots;
    const auto edge = [&](const std::string& a, const std::string& b) { edges[a].push_back(b); };
    const auto build = [&](const std::string& id, const InsertModel* instrument,
                           const auto& inserts, const auto& modules) {
        auto head = id + "/pre";
        if (instrument && instrument->isLoaded()) {
            slots[id + "/" + instrument->id] = instrument;
            edge(id + "/" + instrument->id, head);
        }
        const auto append = [&](const InsertModel& slot) {
            if (!slot.isLoaded()) return;
            const auto key = id + "/" + slot.id;
            slots[key] = &slot; edge(head, key); head = key;
        };
        for (const auto& slot : modules) if (!slot.miniModulePostFx) append(slot);
        for (const auto& slot : inserts) append(slot);
        for (const auto& slot : modules) if (slot.miniModulePostFx) append(slot);
        edge(head, id + "/post"); edge(id + "/post", id + "/out");
    };
    build("master", nullptr, project.masterInserts, project.masterMiniModules);
    for (const auto& track : project.tracks) if (carriesAudio(track))
        build(track.id, &track.instrument, track.inserts, track.miniModules);
    for (const auto& track : project.tracks) if (carriesAudio(track)) {
        const auto* bus = project.findTrack(track.outputBusId);
        edge(track.id + "/out", bus && carriesAudio(*bus) ? bus->id + "/pre" : "master/pre");
        for (const auto& send : track.sends) if (const auto* target = project.findTrack(send.destinationTrackId); target && carriesAudio(*target))
            edge(track.id + (send.preFader ? "/post" : "/out"), target->id + "/pre");
    }
    for (const auto& [key, slot] : slots) {
        const auto channel = key.substr(0, key.find('/'));
        const auto excluded = sidechainFeedbackSources(project, channel);
        for (const auto& source : slot->sidechainTrackIds)
            if (!excluded.contains(source) && (source == "master" || project.findTrack(source))) edge(source + "/out", key);
    }
    const auto reaches = [&](const std::string& begin, const std::string& end) {
        std::unordered_set<std::string> visited;
        std::vector<std::string> pending{begin};
        while (!pending.empty()) {
            auto at = std::move(pending.back()); pending.pop_back();
            if (at == end) return true;
            if (!visited.insert(at).second) continue;
            if (const auto next = edges.find(at); next != edges.end())
                pending.insert(pending.end(), next->second.begin(), next->second.end());
        }
        return false;
    };
    const auto connect = [&](const std::string& channel) {
        const auto& rack = rackFor(channel);
        for (const auto& route : rack.routes) {
            const auto slotKey = channel + "/" + route.slotId;
            const auto slot = slots.find(slotKey);
            if (slot == slots.end() || slot->second->uid != route.pluginUid || toString(slot->second->format) != route.pluginFormat) continue;
            const auto source = std::find_if(rack.sources.begin(), rack.sources.end(), [&](const auto& s) { return s.id == route.sourceId; });
            if (source == rack.sources.end() || source->kind != modulation::Kind::Follower) continue;
            const auto audio = source->audioChannelId.empty() ? channel : source->audioChannelId;
            if (audio != "master" && !project.findTrack(audio)) continue;
            const auto tap = audio + (source->audioChannelId.empty() || source->tap == modulation::Tap::BeforeEffects ? "/pre"
                : source->tap == modulation::Tap::AfterEffects ? "/post" : "/out");
            if (reaches(slotKey, tap)) return false;
            edge(tap, slotKey);
        }
        return true;
    };
    if (!connect("master")) return false;
    for (const auto& track : project.tracks) if (carriesAudio(track) && !connect(track.id)) return false;
    return true;
}
} // namespace daw
