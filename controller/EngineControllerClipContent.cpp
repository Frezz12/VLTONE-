#include "EngineController.hpp"
#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace daw {
void EngineController::setMidiClipView(const std::string& trackId,
                                      const std::string& clipId, MidiClipView view) {
    auto* clip = findClip(trackId, clipId);
    if (!clip || clip->kind != ClipKind::Midi) return;
    view.pitch = std::clamp(view.pitch, 0, 127);
    view.stepBeats = std::isfinite(view.stepBeats)
        ? std::clamp(view.stepBeats, 1.0 / 64.0, 4.0) : 0.25;
    if (clip->midiView == view) return;
    clip->midiView = view;
    ++m_clipGeometryRevision;
}

std::string EngineController::splitLinkedClip(const ClipAddress& address, double at) {
    if (!sharedEditingAllowed() || cloudProjectBound()) return {};
    const auto* original = findClip(address.trackId, address.clipId);
    if (!original) return {};
    if(original->kind==ClipKind::Pattern) {
        m_project.initializePatternContent(original->id);
        original=findClip(address.trackId,address.clipId);
    }
    const double length = effectiveClipLength(*original);
    const double leftLength = at - original->startSeconds;
    if (leftLength < .001 || length - leftLength < .001) return {};
    if(auto* definition=m_project.patternPart(*original)) {
        const auto before=m_project;
        auto right=*original;right.id=newUuid();right.patternPartId=newUuid();
        right.startSeconds=at;right.durationSeconds=length-leftLength;
        right.contentOffsetBeats=definition->offsetBeats+secondsToBeats(leftLength,tempo());
        right.offsetSeconds=beatsToSeconds(right.contentOffsetBeats,tempo());
        auto tail=*definition;tail.id=right.patternPartId;
        tail.startBeats+=secondsToBeats(leftLength,tempo());tail.durationBeats=secondsToBeats(length-leftLength,tempo());
        tail.offsetBeats=right.contentOffsetBeats;
        definition->durationBeats=secondsToBeats(leftLength,tempo());
        const auto rightId=right.id;
        m_project.patternOwner(*original)->contentStorage()->patternParts.push_back(tail);
        right.bindContent(original->contentStorage());
        m_project.findTrack(address.trackId)->clips.push_back(std::move(right));
        m_project.resolveClipContents();rebuildGraph();updateTimelineDuration();
        pushProjectSnapshotUndo(before,"Split Pattern Part");
        return rightId;
    }
    struct Part { ClipAddress address; ClipInstanceModel before, left; ClipModel right; };
    std::vector<Part> parts;
    const auto makePart = [](const ClipAddress& a, const ClipModel& c) {
        Part part{a, static_cast<const ClipInstanceModel&>(c), static_cast<const ClipInstanceModel&>(c), c};
        part.right.id = newUuid();
        for (auto& insert : part.right.inserts) insert.id = newUuid();
        return part;
    };
    auto root = makePart(address, *original);
    root.left.durationSeconds = leftLength;
    root.left.fadeOutSeconds = 0;
    root.right.startSeconds = at;
    root.right.durationSeconds = length - leftLength;
    root.right.offsetSeconds += leftLength / std::max(.001, original->sampleEdit.stretchTime);
    if(original->kind==ClipKind::Audio && !original->warp.empty()) {
        const double cut=original->warp.enabled ? secondsToBeats(leftLength,tempo())
            : warpBeatAt(original->warp,root.right.offsetSeconds);
        root.left.warp=sliceWarp(original->warp,0,cut);
        root.right.warp=sliceWarp(original->warp,cut,original->warp.markers.back().targetBeats);
        if(!root.right.warp.empty())root.right.offsetSeconds=root.right.warp.markers.front().sourceSeconds;
    }
    if (original->kind == ClipKind::Midi || original->kind == ClipKind::Automation || original->kind==ClipKind::Pattern)
        root.right.contentOffsetBeats = (original->contentOffsetBeats != 0 ? original->contentOffsetBeats :
            secondsToBeats(original->offsetSeconds, tempo())) + secondsToBeats(leftLength, tempo());
    root.right.fadeInSeconds = 0;
    const auto rightId = root.right.id;
    parts.push_back(std::move(root));
    if (original->kind == ClipKind::Pattern)
        for (const auto& track : m_project.tracks) for (const auto& child : track.clips)
            if (child.patternClipId == address.clipId) {
                auto part = makePart({track.id, child.id}, child);
                // Each parent gates its own half. Child content and its timing
                // stay intact, including notes that cross the knife.
                part.right.patternClipId = rightId;
                parts.push_back(std::move(part));
            }
    const auto apply = [this, parts](bool split) {
        for (const auto& part : parts) {
            auto* track = m_project.findTrack(part.address.trackId);
            if (!track) continue;
            std::erase_if(track->clips, [&](const auto& c) { return c.id == part.right.id; });
            auto* left = findClip(part.address.trackId, part.address.clipId);
            if (!left) continue;
            static_cast<ClipInstanceModel&>(*left) = split ? part.left : part.before;
            if (split) {
                auto right = part.right;
                right.bindContent(left->contentStorage());
                track->clips.push_back(std::move(right));
            }
        }
        m_project.resolveClipContents(); rebuildGraph(); updateTimelineDuration();
    };
    apply(true);
    m_undo.push("Split Linked Clip", [apply] { apply(false); }, [apply] { apply(true); });
    return rightId;
}

std::vector<EngineController::ClipAddress> EngineController::linkedClips(const ClipAddress& address) const {
    const auto* owner = m_project.findTrack(address.trackId);
    const ClipModel* original = nullptr;
    if (owner) for (const auto& c : owner->clips) if (c.id == address.clipId) original = &c;
    std::vector<ClipAddress> out;
    if (!original) return out;
    for (const auto& track : m_project.tracks) for (const auto& clip : track.clips)
        if (clip.id == original->id || (!original->contentId.empty() && clip.contentId == original->contentId))
            out.push_back({track.id, clip.id});
    return out;
}

std::vector<EngineController::ClipAddress> EngineController::duplicateLinkedClips(const std::vector<ClipAddress>& selected) {
    if (selected.empty() || !sharedEditingAllowed() || cloudProjectBound()) return {};
    for(const auto& address:selected)if(const auto* root=findClip(address.trackId,address.clipId);root&&root->kind==ClipKind::Pattern)
        m_project.initializePatternContent(root->id);
    m_project.resolveClipContents();
    std::unordered_set<std::string> parents;
    for (const auto& a : selected) if (const auto* c = findClip(a.trackId,a.clipId); c && c->kind == ClipKind::Pattern) parents.insert(c->id);
    std::vector<ClipAddress> sources;
    double first = std::numeric_limits<double>::max(), end = 0;
    for (const auto& a : selected) if (const auto* c = findClip(a.trackId,a.clipId)) {
        if (!c->patternClipId.empty() && parents.contains(c->patternClipId)) continue;
        if (std::any_of(sources.begin(),sources.end(),[&](const auto& b){return b.clipId==a.clipId;})) continue;
        sources.push_back(a); first = std::min(first,c->startSeconds); end = std::max(end,c->startSeconds+effectiveClipLength(*c));
    }
    if (sources.empty()) return {};
    const double delta = std::max(0.001,end-first);
    const auto group = m_undo.beginGroup();
    struct Binding { ClipAddress address; std::string id; std::shared_ptr<ClipContent> content; };
    std::vector<Binding> bindings;
    std::vector<ClipAddress> result;
    for (const auto& a : sources) {
        const auto* c = findClip(a.trackId,a.clipId);
        const auto content = c->contentStorage(); const auto id = c->contentId;
        const auto kind = c->kind; const double start = c->startSeconds+delta;
        std::vector<Binding> members;
        if (kind == ClipKind::Pattern) for (const auto& t : m_project.tracks) for (const auto& child : t.clips)
            if (child.patternClipId==a.clipId) members.push_back({{t.id,child.id},child.contentId,child.contentStorage()});
        const auto copyId = duplicateClipAt(a.trackId,a.clipId,start);
        if (copyId.empty()) continue;
        result.push_back({a.trackId,copyId}); bindings.push_back({{a.trackId,copyId},id,content});
        for (const auto& member : members) {
            auto* track = m_project.findTrack(member.address.trackId);
            const auto child = std::find_if(track->clips.begin(),track->clips.end(),[&](const auto& item){
                return item.patternClipId==copyId && std::none_of(bindings.begin(),bindings.end(),[&](const auto& b){return b.address.clipId==item.id;});
            });
            if (child!=track->clips.end()) bindings.push_back({{track->id,child->id},member.id,member.content});
        }
    }
    const auto apply = [this,bindings] {
        for (const auto& b : bindings) if (auto* c=findClip(b.address.trackId,b.address.clipId)) {
            auto content=b.content;
            for (const auto& t:m_project.tracks) for (const auto& peer:t.clips)
                if(peer.id!=c->id && peer.contentId==b.id) {content=peer.contentStorage();break;}
            c->contentId=b.id; c->bindContent(std::move(content));
            if(auto* part=m_project.patternPart(*c))part->contentId=b.id;
        }
        m_project.resolveClipContents(); rebuildGraph(); updateTimelineDuration();
    };
    if (!bindings.empty()) { apply(); m_undo.push("Link Clip Content",[]{},apply); }
    m_undo.collapseGroup(group,"Create Linked Copies");
    return result;
}

bool EngineController::makeClipsIndependent(const std::vector<ClipAddress>& selected) {
    if (!sharedEditingAllowed() || cloudProjectBound()) return false;
    struct Binding { ClipAddress address; std::string beforeId, afterId; std::shared_ptr<ClipContent> before, after; };
    std::vector<Binding> changes;
    std::unordered_set<std::string> ids;
    for (const auto& a:selected) if (const auto* c=findClip(a.trackId,a.clipId)) {
        if (linkedClips(a).size()<2) continue;
        ids.insert(c->id);
        const auto* root = c->kind == ClipKind::Pattern ? c : m_project.patternOwner(*c);
        if (root) {
            // A part cannot override the common composition of a linked
            // Pattern. Detach its containing instance and its full music tree.
            ids.insert(root->id);
            for (const auto& t : m_project.tracks) for (const auto& child : t.clips)
                if (child.patternClipId == root->id) ids.insert(child.id);
        }
    }
    for (auto& t:m_project.tracks) for (auto& c:t.clips) if(ids.contains(c.id))
        changes.push_back({{t.id,c.id},c.contentId,newUuid(),c.contentStorage(),std::make_shared<ClipContent>(*c.contentStorage())});
    if(changes.empty()) return false;
    const auto apply=[this,changes](bool independent){
        for(const auto& b:changes) if(auto* c=findClip(b.address.trackId,b.address.clipId)) {
            c->contentId=independent?b.afterId:b.beforeId;
            auto storage=independent?b.after:b.before;
            if(!independent) for(const auto& t:m_project.tracks) for(const auto& peer:t.clips)
                if(peer.id!=c->id && peer.contentId==b.beforeId) {storage=peer.contentStorage();break;}
            c->bindContent(storage);
        }
        // Bind all roots before editing their composition, independent of
        // track order. Otherwise a child can mutate the original shared root.
        for (const auto& b : changes) if (auto* child=findClip(b.address.trackId,b.address.clipId))
            if (auto* part=m_project.patternPart(*child)) part->contentId=child->contentId;
        if(independent)for(const auto& b:changes)if(auto* root=findClip(b.address.trackId,b.address.clipId);root&&root->kind==ClipKind::Pattern)
            for(const auto& track:m_project.tracks)for(const auto& child:track.clips)if(child.patternClipId==root->id)
                for(auto& part:root->contentStorage()->patternParts)if(part.id==child.patternPartId)part.contentId=child.contentId;
        m_project.resolveClipContents(); rebuildGraph(); updateTimelineDuration();
    };
    apply(true);
    m_undo.push("Make Clips Independent",[apply]{apply(false);},[apply]{apply(true);});
    return true;
}

void EngineController::syncLinkedNoteTracks(const TrackModel& changed, bool geometry) {
    m_recoveryTrackParts.erase(changed.id);
    if(m_syncingLinkedContent) return;
    m_syncingLinkedContent=true;
    std::unordered_set<std::string> contents;
    for(const auto& c:changed.clips) if(!c.contentId.empty()) contents.insert(c.contentId);
    for(const auto& track:m_project.tracks) {
        if(track.id==changed.id) continue;
        if(std::any_of(track.clips.begin(),track.clips.end(),[&](const auto& c){return contents.contains(c.contentId);})) {
            bumpMidiNotesRevision(track.id);
            syncTrackNotes(track,geometry); syncTrackAutomation(track);
        }
    }
    m_syncingLinkedContent=false;
}
} // namespace daw
