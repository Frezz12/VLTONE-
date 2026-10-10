#include "model/Document.hpp"
#include "collaboration/CollaborationState.hpp"
#include <new>
#include <algorithm>

namespace daw {
ClipAutomationModel::ClipAutomationModel(AutomationTarget t, std::shared_ptr<AutomationCurveContent> c)
    : m_curve(std::move(c)), target(std::move(t)), defaultValue(m_curve->defaultValue),
      active(m_curve->active), points(m_curve->points) {}
ClipAutomationModel::ClipAutomationModel() : ClipAutomationModel({}, std::make_shared<AutomationCurveContent>()) {}
ClipAutomationModel::ClipAutomationModel(const ClipAutomationModel& o)
    : ClipAutomationModel(o.target, std::make_shared<AutomationCurveContent>(*o.m_curve)) {}
ClipAutomationModel::ClipAutomationModel(ClipAutomationModel&& o) noexcept
    : ClipAutomationModel(std::move(o.target), std::move(o.m_curve)) {}
ClipAutomationModel& ClipAutomationModel::operator=(const ClipAutomationModel& o) {
    if (this != &o) { target = o.target; *m_curve = *o.m_curve; }
    return *this;
}
ClipAutomationModel& ClipAutomationModel::operator=(ClipAutomationModel&& o) noexcept {
    if (this != &o) {
        target = std::move(o.target);
        if (m_curve != o.m_curve) *m_curve = std::move(*o.m_curve);
    }
    return *this;
}
void ClipAutomationModel::bind(std::shared_ptr<AutomationCurveContent> c) {
    auto t = std::move(target);
    this->~ClipAutomationModel();
    new (this) ClipAutomationModel(std::move(t), std::move(c));
}

ClipModel::ClipModel(ClipInstanceModel instance, std::shared_ptr<ClipContent> c, AutomationTarget target)
    : ClipInstanceModel(std::move(instance)), m_content(std::move(c)),
      filePath(m_content->filePath), notes(m_content->notes), slideNotes(m_content->slideNotes),
      lanes(m_content->lanes), takes(m_content->takes), comp(m_content->comp), asset(m_content->asset),
      audioEdit(m_content->audioEdit) {
    automation.target = std::move(target);
    automation.bind(std::shared_ptr<AutomationCurveContent>(m_content, &m_content->automation));
}
ClipModel::ClipModel() : ClipModel({}, std::make_shared<ClipContent>(), {}) {}
ClipModel::~ClipModel() = default;
ClipModel::ClipModel(const ClipModel& o)
    : ClipModel(static_cast<const ClipInstanceModel&>(o), std::make_shared<ClipContent>(*o.m_content), o.automation.target) {}
ClipModel::ClipModel(ClipModel&& o) noexcept
    : ClipModel(std::move(static_cast<ClipInstanceModel&>(o)), std::move(o.m_content), std::move(o.automation.target)) {}
ClipModel& ClipModel::operator=(const ClipModel& o) {
    if (this == &o) return *this;
    if (!contentId.empty() && contentId == o.contentId && id == o.id) {
        *m_content = *o.m_content;
        static_cast<ClipInstanceModel&>(*this) = o;
        automation.target = o.automation.target;
    } else {
        ClipModel copy(o);
        this->~ClipModel(); new (this) ClipModel(std::move(copy));
    }
    return *this;
}
ClipModel& ClipModel::operator=(ClipModel&& o) noexcept {
    if (this == &o) return *this;
    if (m_content && o.m_content && !contentId.empty() && contentId == o.contentId && id == o.id) {
        if (m_content != o.m_content) *m_content = std::move(*o.m_content);
        static_cast<ClipInstanceModel&>(*this) = std::move(static_cast<ClipInstanceModel&>(o));
        automation.target = std::move(o.automation.target);
    } else { this->~ClipModel(); new (this) ClipModel(std::move(o)); }
    return *this;
}
void ClipModel::bindContent(std::shared_ptr<ClipContent> c) {
    if (m_content == c) return;
    auto local = std::move(static_cast<ClipInstanceModel&>(*this));
    auto target = std::move(automation.target);
    this->~ClipModel(); new (this) ClipModel(std::move(local), std::move(c), std::move(target));
}
void ClipModel::detachContent() {
    auto copy = std::make_shared<ClipContent>(*m_content);
    contentId = newUuid(); copy->kind = kind;
    bindContent(std::move(copy));
}
void ProjectModel::resolveClipContents() {
    // Build from the actual placements: deleted content survives only in undo
    // snapshots, and a restored track must not bind to an obsolete registry.
    decltype(clipContents) registry;
    // Prefer a still-live canonical object when undo reinserts an older view
    // before its peers in track order. Project snapshots have an empty registry.
    for (const auto& track : tracks) for (const auto& clip : track.clips) {
        const auto known = clipContents.find(clip.contentId);
        if (known != clipContents.end() && known->second == clip.contentStorage())
            registry.try_emplace(clip.contentId, known->second);
    }
    for (auto& track : tracks) for (auto& clip : track.clips) {
        if (clip.contentId.empty()) clip.contentId = clip.id.empty() ? newUuid() : clip.id;
        auto [it, inserted] = registry.try_emplace(clip.contentId, clip.contentStorage());
        if (inserted) it->second->kind = clip.kind;
        else if (it->second->kind == clip.kind) clip.bindContent(it->second);
        else { clip.detachContent(); registry.emplace(clip.contentId, clip.contentStorage()); }
    }
    // Recording/import can append a new view directly. Consume it once as a
    // new composition part, before generating the remaining linked views.
    for(auto& lane:tracks)for(auto& child:lane.clips) {
        if(child.kind!=ClipKind::Midi || !child.patternPartId.empty() || child.patternClipId.empty())continue;
        const auto* root=patternOwner(child);
        if(!root || !root->contentStorage()->patternInitialized)continue;
        child.patternPartId=child.id;
        root->contentStorage()->patternParts.push_back({child.patternPartId,lane.id,child.contentId,
            secondsToBeats(child.startSeconds-root->startSeconds,tempo)+root->contentOffsetBeats,
            secondsToBeats(child.durationSeconds,tempo),child.contentOffsetBeats!=0?
                child.contentOffsetBeats:secondsToBeats(child.offsetSeconds,tempo)});
    }
    // Only the composition is editable. MIDI children stored on instrument
    // tracks provide stable UI identities and per-placement presentation; their
    // membership and musical placement are generated from the parent content.
    struct Owner { std::string id; double start,offset; std::shared_ptr<ClipContent> content; };
    std::vector<Owner> owners;
    for(auto& track:tracks)for(auto& root:track.clips)if(root.kind==ClipKind::Pattern) {
        auto data=root.contentStorage();
        if(!data->patternInitialized)continue;
        owners.push_back({root.id,root.startSeconds,root.contentOffsetBeats,std::move(data)});
    }
    struct View { ClipInstanceModel placement; AutomationTarget target; };
    std::unordered_map<std::string,View> existing;
    for(const auto& track:tracks)for(const auto& clip:track.clips)
        if(!clip.patternClipId.empty()&&!clip.patternPartId.empty())
            existing.emplace(clip.patternClipId+"/"+clip.patternPartId,View{clip,clip.automation.target});
    // Indices remain stable while we append generated views. Resolve each
    // owner/part once instead of scanning a whole lane for every linked copy.
    std::unordered_map<std::string,std::unordered_map<std::string,std::size_t>> viewIndices;
    for (const auto& lane : tracks) for (std::size_t i=0;i<lane.clips.size();++i) {
        const auto& view=lane.clips[i];
        if (!view.patternClipId.empty() && !view.patternPartId.empty())
            viewIndices[lane.id].emplace(view.patternClipId+"/"+view.patternPartId,i);
    }
    std::unordered_map<std::string,std::string> placements;
    for(const auto& owner:owners)for(const auto& part:owner.content->patternParts) {
        auto* lane=findTrack(part.trackId);
        const auto data=registry.find(part.contentId);
        if(!lane||!trackAccepts(lane->kind,ClipKind::Midi)||data==registry.end()||data->second->kind!=ClipKind::Midi)continue;
        const std::string key=owner.id+"/"+part.id;
        placements[key]=lane->id;
        auto& indices=viewIndices[lane->id];
        auto [index,created]=indices.try_emplace(key,lane->clips.size());
        if(created) {
            ClipModel view;
            if(auto previous=existing.find(key);previous!=existing.end()) {
                static_cast<ClipInstanceModel&>(view)=previous->second.placement;
                view.automation.target=previous->second.target;
            }
            else {view.id=collab::deterministicMigrationId("pattern-part-view",key);view.name=lane->name;view.color=lane->color;view.kind=ClipKind::Midi;}
            lane->clips.push_back(std::move(view));
        }
        auto* found=&lane->clips[index->second];
        found->patternClipId=owner.id;found->patternPartId=part.id;found->contentId=part.contentId;
        found->bindContent(data->second);
        found->startSeconds=owner.start+beatsToSeconds(part.startBeats-owner.offset,tempo);
        found->durationSeconds=beatsToSeconds(part.durationBeats,tempo);
        found->contentOffsetBeats=part.offsetBeats;
        found->offsetSeconds=beatsToSeconds(part.offsetBeats,tempo);
    }
    std::unordered_set<std::string> ownerIds;
    for(const auto& owner:owners)ownerIds.insert(owner.id);
    for(auto& track:tracks)std::erase_if(track.clips,[&](const auto& clip){
        if(clip.patternPartId.empty()||!ownerIds.contains(clip.patternClipId))return false;
        const auto expected=placements.find(clip.patternClipId+"/"+clip.patternPartId);
        return expected==placements.end()||expected->second!=track.id;
    });
    decltype(clipContents) live;
    for(const auto& track:tracks)for(const auto& clip:track.clips)live.try_emplace(clip.contentId,clip.contentStorage());
    clipContents = std::move(live);
}

const ClipModel* ProjectModel::patternOwner(const ClipModel& child) const {
    if(child.patternClipId.empty())return nullptr;
    for(const auto& track:tracks)for(const auto& root:track.clips)
        if(root.id==child.patternClipId&&root.kind==ClipKind::Pattern)return &root;
    return nullptr;
}
PatternPartModel* ProjectModel::patternPart(const ClipModel& child) {
    const auto* root=patternOwner(child);if(!root||child.patternPartId.empty())return nullptr;
    auto& parts=root->contentStorage()->patternParts;
    const auto found=std::find_if(parts.begin(),parts.end(),[&](const auto& p){return p.id==child.patternPartId;});
    return found==parts.end()?nullptr:&*found;
}
void ProjectModel::setPatternPartPlacement(const ClipModel& child) {
    auto* part=patternPart(child);const auto* root=patternOwner(child);if(!part||!root)return;
    part->startBeats=secondsToBeats(child.startSeconds-root->startSeconds,tempo)+root->contentOffsetBeats;
    part->durationBeats=secondsToBeats(child.durationSeconds,tempo);
    part->offsetBeats=child.contentOffsetBeats!=0?child.contentOffsetBeats:secondsToBeats(child.offsetSeconds,tempo);
}
void ProjectModel::initializePatternContent(const std::string& clipId) {
    resolveClipContents();
    for(auto& track:tracks)for(auto& root:track.clips)if(root.id==clipId&&root.kind==ClipKind::Pattern) {
        auto data=root.contentStorage();if(data->patternInitialized)return;
        data->patternInitialized=true;
        for(auto& lane:tracks)for(auto& child:lane.clips)
            if(child.kind==ClipKind::Midi&&child.patternClipId==root.id) {
                if(child.patternPartId.empty())child.patternPartId=child.id;
                data->patternParts.push_back({child.patternPartId,lane.id,child.contentId,
                    secondsToBeats(child.startSeconds-root.startSeconds,tempo)+root.contentOffsetBeats,
                    secondsToBeats(child.durationSeconds,tempo),child.contentOffsetBeats!=0?
                        child.contentOffsetBeats:secondsToBeats(child.offsetSeconds,tempo)});
            }
        return;
    }
}
void ProjectModel::registerPatternPart(const std::string& trackId,const std::string& clipId,bool materialize) {
    auto* lane=findTrack(trackId);if(!lane)return;
    auto found=std::find_if(lane->clips.begin(),lane->clips.end(),[&](const auto& child){return child.id==clipId;});
    if(found==lane->clips.end()||found->kind!=ClipKind::Midi)return;
    const auto* root=patternOwner(*found);if(!root)return;
    if(!root->contentStorage()->patternInitialized) {
        initializePatternContent(root->id);
        if(materialize)resolveClipContents();
        return;
    }
    if(!patternPart(*found)) {
        if(found->contentId.empty())found->contentId=found->id;
        if(found->patternPartId.empty())found->patternPartId=newUuid();
        root->contentStorage()->patternParts.push_back({found->patternPartId,trackId,found->contentId,
            secondsToBeats(found->startSeconds-root->startSeconds,tempo)+root->contentOffsetBeats,
            secondsToBeats(found->durationSeconds,tempo),found->contentOffsetBeats!=0?
                found->contentOffsetBeats:secondsToBeats(found->offsetSeconds,tempo)});
    }
    if(materialize)resolveClipContents();
}
} // namespace daw
