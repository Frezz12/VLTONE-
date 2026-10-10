#include "EngineController.hpp"
#include "MediaWorker.hpp"
#include "SampleLoader.hpp"
#include "Common/WarpMap.hpp"
#include "platform/PathUtils.hpp"
#include <future>
#include <mutex>
#include <deque>

namespace daw {
EngineController::PreparedCompEdit EngineController::prepareCompAudioEdit(
    ClipModel source, double rate, const std::string& directory, const audioedit::Continue& keepGoing) {
    if(source.kind!=ClipKind::Audio || source.comp.empty() || (keepGoing && !keepGoing())) return {};
    // Reuse the comp playback renderer in a private, device-free controller.
    // Only take gains and seam crossfades are baked; instance processing follows
    // the resulting document just as it follows an ordinary imported source.
    EngineController worker;
    worker.m_isRenderClone=true;
    if(!worker.initialize(rate,512,false)) return {};
    worker.setRecordDirectory(directory);
    worker.m_sampleLoadContinue=keepGoing;
    const double stretch=std::max(.001,source.sampleEdit.stretchTime);
    source.startSeconds=0;source.durationSeconds/=stretch;source.offsetSeconds=0;
    source.gain=1;source.pan=0;source.muted=false;
    source.fadeInSeconds=source.fadeOutSeconds=0;
    source.sampleEdit={};source.warp={};source.offlineProcess={};source.inserts.clear();
    source.compCrossfadeMs/=stretch;
    for(auto& segment:source.comp){segment.startSeconds/=stretch;segment.endSeconds/=stretch;}
    for(auto& take:source.takes)take.clipOffsetSeconds/=stretch;
    TrackModel track;track.id=newUuid();track.kind=TrackKind::Audio;
    const auto trackId=track.id,clipId=source.id;
    track.clips.push_back(std::move(source));worker.m_project.tracks.push_back(std::move(track));
    const auto flattened=worker.flattenComp(trackId,clipId,false);
    const auto* clip=worker.findClip(trackId,clipId);
    const auto* take=clip?findTake(*clip,flattened):nullptr;
    if(!take)return {};
    std::shared_ptr<const engine::SampleBuffer> audio;
    if((keepGoing && !keepGoing()) || !loadSampleBuffer(take->filePath,audio,keepGoing)) {
        std::error_code error;std::filesystem::remove(platform::pathFromUtf8(take->filePath),error);return {};
    }
    auto document=audioedit::fromSource({newUuid(),take->filePath,audio->sampleRate(),AudioEditFrame(audio->frames()),int(audio->channels())});
    return {std::move(document),std::move(audio)};
}

bool EngineController::adoptCompAudioEdit(const AudioEditTarget& target,PreparedCompEdit prepared) {
    if(target.instrument || cloudProjectBound() || !sharedEditingAllowed() || isRecording() || m_audioEditGesture ||
       !prepared.audio || !prepared.document.initialized || !audioedit::valid(prepared.document)) return false;
    auto* clip=findClip(target.trackId,target.objectId);
    if(!clip || clip->kind!=ClipKind::Audio || clip->takes.empty())return false;
    const auto before=m_project;
    const auto peers=linkedClips({target.trackId,target.objectId});
    for(const auto& peer:peers)if(auto* instance=findClip(peer.trackId,peer.clipId)) {
        if(instance->offlineHistory.empty()){
            instance->offlineHistory.push_back({newUuid(),{},"Original takes",captureClipAudioVersion(*instance)});
            instance->offlineVersionId=instance->offlineHistory.back().id;
        }
    }
    clip->filePath=prepared.document.sources.front().filePath;clip->asset={};
    clip->takes.clear();clip->comp.clear();
    if(!applyAudioEdit(target,prepared.document,prepared.audio)){m_project=before;return false;}
    for(const auto& peer:peers)if(auto* instance=findClip(peer.trackId,peer.clipId)) {
        instance->offsetSeconds=0;instance->expanded=false;
        instance->offlineHistory.push_back({newUuid(),instance->offlineVersionId,"Editable comp",captureClipAudioVersion(*instance)});
        instance->offlineVersionId=instance->offlineHistory.back().id;
    }
    rebuildGraph();updateTimelineDuration();
    pushProjectSnapshotUndo(before,"Create Editable Comp Version");return true;
}

AudioEditDocument EngineController::audioEditDocument(const AudioEditTarget& target) {
    if (target.instrument) {
        const auto* track=m_project.findTrack(target.trackId);
        if(!track || track->instrument.id!=target.objectId || track->instrument.uid!="daw.sampler") return {};
        if(track->instrument.audioEdit.initialized) return track->instrument.audioEdit;
        if(auto* sampler=samplerInstance(target.trackId,target.objectId)) if(auto raw=sampler->rawSample())
            return audioedit::fromSource({target.objectId,sampler->samplePath(),raw->sampleRate(),AudioEditFrame(raw->frames()),int(raw->channels())});
    } else if(const auto* clip=audioClip(target.trackId,target.objectId)) {
        if(clip->audioEdit.initialized) return clip->audioEdit;
        if(!clip->takes.empty()) return {};
        if(const auto* raw=cachedSourceSamples(clip->filePath))
            return audioedit::fromSource({clip->contentId.empty()?clip->id:clip->contentId,clip->filePath,raw->sampleRate(),AudioEditFrame(raw->frames()),int(raw->channels()),clip->asset.assetId,clip->asset.sha256});
        if(clip->asset.frames>0)
            return audioedit::fromSource({clip->contentId.empty()?clip->id:clip->contentId,clip->filePath,clip->asset.sampleRate,AudioEditFrame(clip->asset.frames),int(clip->asset.channels),clip->asset.assetId,clip->asset.sha256});
    }
    return {};
}

std::shared_ptr<const engine::SampleBuffer> EngineController::renderAudioEdit(const AudioEditDocument& doc, const audioedit::Continue& progress) {
    return audioedit::render(doc,[&progress](const AudioEditSource& source){
        std::shared_ptr<const engine::SampleBuffer> result;
        if(!loadSampleBuffer(source.filePath,result,progress)) return std::shared_ptr<const engine::SampleBuffer>{};
        // Keep a bounded set of source buffers alive across knob previews. The
        // loader still checks file identity and modification time on each call.
        static std::mutex mutex;
        static std::deque<std::shared_ptr<const engine::SampleBuffer>> recent;
        static std::uint64_t bytes=0;
        {
            std::lock_guard lock(mutex);
            if(std::find(recent.begin(),recent.end(),result)==recent.end()) {
                const auto size=std::uint64_t(result->frames())*result->channels()*sizeof(float);
                if(size<=256u*1024u*1024u) {
                    recent.push_back(result);bytes+=size;
                    while(bytes>256u*1024u*1024u) {
                        bytes-=std::uint64_t(recent.front()->frames())*recent.front()->channels()*sizeof(float);
                        recent.pop_front();
                    }
                }
            }
        }
        return result;
    },progress);
}
std::shared_ptr<const engine::SampleBuffer> EngineController::resolveEditedAudio(const AudioEditDocument& doc) {
    if(!doc.initialized) return {};
    if(const auto it=m_audioEditAudio.find(doc.revision);it!=m_audioEditAudio.end()) return it->second;
    // Exact consumers (load/export/undo) wait for worker preparation. Interactive
    // edits arrive with a prepared buffer and never enter this path.
    // Exact renders can themselves originate on BackgroundExecutor. A separate
    // preparation task avoids waiting on that executor's sole playback worker.
    auto ready=std::async(std::launch::async,[doc]{return renderAudioEdit(doc);});
    auto audio=ready.get();
    if(audio) m_audioEditAudio[doc.revision]=audio;
    return audio;
}
std::shared_ptr<const engine::SampleBuffer> EngineController::cachedAudioEditSamples(const AudioEditTarget& target) {
    auto doc=audioEditDocument(target);
    if(const auto it=m_audioEditAudio.find(doc.revision);it!=m_audioEditAudio.end()) return it->second;
    if(target.instrument) {
        if(auto* sampler=samplerInstance(target.trackId,target.objectId)) return sampler->rawSample();
    } else if(const auto* clip=audioClip(target.trackId,target.objectId)) {
        if(!clip->audioEdit.initialized) {
            if(const auto it=m_sourceSamples.find(clip->filePath);it!=m_sourceSamples.end()) return it->second;
            if(const auto it=m_samples.find(clip->filePath);it!=m_samples.end()) return it->second;
        }
    }
    return {};
}
bool EngineController::applyAudioEdit(const AudioEditTarget& target,const AudioEditDocument& doc,
                                     std::shared_ptr<const engine::SampleBuffer> audio) {
    if(!audio || !doc.initialized || !audioedit::valid(doc) ||
       audio->frames() != std::uint64_t(doc.frames) || audio->sampleRate() != doc.sampleRate ||
       audio->channels() != unsigned(doc.channels)) return false;
    m_audioEditAudio[doc.revision]=audio;
    std::uint64_t cacheBytes = 0;
    for (const auto& [revision, buffer] : m_audioEditAudio)
        cacheBytes += buffer->frames() * buffer->channels() * sizeof(float);
    for (auto it = m_audioEditAudio.begin(); cacheBytes > 256u * 1024u * 1024u && it != m_audioEditAudio.end();) {
        if (it->first == doc.revision) { ++it; continue; }
        cacheBytes -= it->second->frames() * it->second->channels() * sizeof(float);
        it = m_audioEditAudio.erase(it);
    }
    if(target.instrument) {
        auto* track=m_project.findTrack(target.trackId);
        auto* sampler=samplerInstance(target.trackId,target.objectId);
        if(!track || !sampler || track->instrument.id!=target.objectId) return false;
        track->instrument.audioEdit=doc;
        sampler->adoptSample(sampler->samplePath(),audio);
        m_recoveryTrackParts.erase(track->id);
    } else {
        auto* clip=findClip(target.trackId,target.objectId);
        if(!clip || clip->kind!=ClipKind::Audio || !clip->takes.empty()) return false;
        // Migration creates the first canonical binding without changing sound.
        if(clip->contentId.empty()) {clip->contentId=clip->id;m_project.resolveClipContents();}
        clip->audioEdit=doc;
        std::unordered_set<std::string> tracks;
        for(const auto& a:linkedClips({target.trackId,target.objectId})) {
            auto* c=findClip(a.trackId,a.clipId);
            c->musicalAnalysis={};
            m_clipDisplayPaths.erase(c->id);
            tracks.insert(a.trackId);
        }
        m_sharedClipSampleCache.clear();
        for(const auto& id:tracks) { m_recoveryTrackParts.erase(id); deferClipSync(id); }
        ++m_clipWaveformRevision;
    }
    updateTimelineDuration();
    return true;
}
bool EngineController::beginAudioEdit(const AudioEditTarget& target) {
    if(cloudProjectBound() || !sharedEditingAllowed() || isRecording()) return false;
    if (m_audioEditGesture) return false;
    auto doc=audioEditDocument(target);
    auto audio=cachedAudioEditSamples(target);
    if(!doc.initialized || !audio) return false;
    m_audioEditGesture=AudioEditGesture{target,doc,doc,audio,audio};
    return true;
}
bool EngineController::updateAudioEdit(const AudioEditDocument& doc,std::shared_ptr<const engine::SampleBuffer> audio) {
    if(!m_audioEditGesture || !audio) return false;
    if(!applyAudioEdit(m_audioEditGesture->target,doc,audio)) return false;
    m_audioEditGesture->after=doc; m_audioEditGesture->afterAudio=std::move(audio); return true;
}
bool EngineController::commitAudioEdit(const std::string& label) {
    if(!m_audioEditGesture) return false;
    auto edit=std::move(*m_audioEditGesture);m_audioEditGesture.reset();
    if(audioEditDocument(edit.target).revision != edit.after.revision) return false;
    if(edit.before==edit.after) return false;
    m_undo.push(label,[this,edit]{applyAudioEdit(edit.target,edit.before,edit.beforeAudio);},
                      [this,edit]{applyAudioEdit(edit.target,edit.after,edit.afterAudio);},
                      (edit.beforeAudio->frames()*edit.beforeAudio->channels() +
                       edit.afterAudio->frames()*edit.afterAudio->channels()) * sizeof(float));
    return true;
}
void EngineController::cancelAudioEdit() {
    if(!m_audioEditGesture) return;
    auto edit=std::move(*m_audioEditGesture);m_audioEditGesture.reset();
    if(edit.before!=edit.after && audioEditDocument(edit.target).revision == edit.after.revision)
        applyAudioEdit(edit.target,edit.before,edit.beforeAudio);
}
bool EngineController::setAudioEdit(const AudioEditTarget& target,const AudioEditDocument& doc,
                                  std::shared_ptr<const engine::SampleBuffer> audio,const std::string& label) {
    if(!beginAudioEdit(target)) return false;
    if(!updateAudioEdit(doc,std::move(audio))) {cancelAudioEdit();return false;}
    return commitAudioEdit(label);
}
bool EngineController::setAudioEditWindow(const AudioEditTarget& target,double first,double last,const std::string& label) {
    if(cloudProjectBound() || !sharedEditingAllowed() || isRecording())return false;
    if(!std::isfinite(first)||!std::isfinite(last)||last<=first||first<0) return false;
    const auto doc=audioEditDocument(target);if(!doc.initialized||doc.frames<=0) return false;
    const double duration=double(doc.frames)/doc.sampleRate;
    first=std::clamp(first,0.,duration);last=std::clamp(last,first,duration);
    if(last<=first) return false;
    if(target.instrument) {
        const auto group=m_undo.beginGroup();
        for(const auto& [id,value]:std::vector<std::pair<std::string,double>>{{"startoffset",first/duration},{"endoffset",last/duration}}) {
            const auto before=insertParameter(target.trackId,target.objectId,id);
            setInsertParameter(target.trackId,target.objectId,id,value);
            commitInsertParameterEdit(target.trackId,target.objectId,id,before,label);
        }
        m_undo.collapseGroup(group,label);return true;
    }
    auto* clip=findClip(target.trackId,target.objectId);if(!clip)return false;
    struct Window { double offset,length; ClipWarpModel warp; ClipMusicalAnalysisModel analysis; };
    const Window before{clip->offsetSeconds,clip->durationSeconds,clip->warp,clip->musicalAnalysis};
    Window after{first,(last-first)*clip->sampleEdit.stretchTime,clip->warp,{}};
    if(!clip->warp.empty()) {
        const double a=warpBeatAt(clip->warp,first),b=warpBeatAt(clip->warp,last);
        after.warp=sliceWarp(clip->warp,a,b);
        if(clip->warp.enabled)after.length=beatsToSeconds(b-a,tempo());
    }
    if(before.offset==after.offset&&before.length==after.length&&before.warp==after.warp)return false;
    // Editor boundaries can be one sample apart. The timeline's larger
    // pointer-drag minimum must not round these exact source coordinates.
    const auto apply=[this,target](const Window& window){
        if(auto* c=findClip(target.trackId,target.objectId)) {
            c->offsetSeconds=window.offset;c->durationSeconds=window.length;
            c->warp=window.warp;c->musicalAnalysis=window.analysis;
            m_recoveryTrackParts.erase(target.trackId);
            if(auto* track=m_project.findTrack(target.trackId))syncTrackClips(*track);
            updateTimelineDuration();
        }
    };
    apply(after);m_undo.push(label,[apply,before]{apply(before);},[apply,after]{apply(after);});return true;
}
bool EngineController::slipAudioEdit(const AudioEditTarget& target,double seconds) {
    if(cloudProjectBound() || !sharedEditingAllowed() || isRecording() || !std::isfinite(seconds))return false;
    const auto doc=audioEditDocument(target);if(!doc.initialized)return false;
    if(target.instrument) {
        const double start=insertParameter(target.trackId,target.objectId,"startoffset")*doc.frames/doc.sampleRate;
        const double end=insertParameter(target.trackId,target.objectId,"endoffset")*doc.frames/doc.sampleRate;
        const double offset=std::clamp(start+seconds,0.,std::max(0.,double(doc.frames)/doc.sampleRate-(end-start)));
        return setAudioEditWindow(target,offset,offset+end-start,"Slip Sample Content");
    }
    auto* clip=findClip(target.trackId,target.objectId);if(!clip)return false;
    const auto before=clip->offsetSeconds;
    const auto beforeWarp=clip->warp;
    auto afterWarp=beforeWarp;
    auto after=std::max(0.,before+seconds);
    if(!afterWarp.empty()) {
        const double delta=std::clamp(seconds,-afterWarp.markers.front().sourceSeconds,
            std::max(0.,double(doc.frames)/doc.sampleRate-afterWarp.markers.back().sourceSeconds));
        for(auto& marker:afterWarp.markers)marker.sourceSeconds+=delta;
        after=afterWarp.markers.front().sourceSeconds;
    }
    const auto apply=[this,target](double offset,const ClipWarpModel& warp){if(auto* c=findClip(target.trackId,target.objectId)) {
        c->offsetSeconds=offset;c->warp=warp;m_recoveryTrackParts.erase(target.trackId);
        if(auto* t=m_project.findTrack(target.trackId))syncTrackClips(*t);++m_clipGeometryRevision;
    }};
    if(before==after)return false;apply(after,afterWarp);
    m_undo.push("Slip Sample Content",[apply,before,beforeWarp]{apply(before,beforeWarp);},
        [apply,after,afterWarp]{apply(after,afterWarp);});return true;
}
} // namespace daw
