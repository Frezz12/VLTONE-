#include "EngineController.hpp"
#include "model/ProjectMemory.hpp"
#include <cmath>
#include <map>
#include <set>

namespace daw {
namespace {
audio::Result failure(const std::string& message) {
    return audio::Result::fail(audio::EngineError::InvalidArgument, message);
}
std::string anchorState(const ClipModel& clip) {
    std::string value = ":" + clip.offlineVersionId + ":" + clip.offlineProcess.sourceFingerprint;
    value += ":" + clip.offlineProcess.renderedFilePath;
    for (const auto& marker : clip.warp.markers) value += ":" + marker.id + (marker.locked ? "!" : "?");
    return value;
}
}
std::string EngineController::vocalAlignmentUnavailableReason() const {
    if (cloudProjectBound()) return "Editable vocal alignment is available in local projects only.";
    if (!sharedEditingAllowed()) return "This session is read-only.";
    if (isRecording()) return "Stop recording before aligning vocals.";
    return {};
}

audio::Result EngineController::prepareVocalAlignment(const std::vector<ClipAddress>& clips,
    std::vector<VocalAlignmentClip>& sources) try {
    sources.clear();
    if (const auto reason = vocalAlignmentUnavailableReason(); !reason.empty()) return failure(reason);
    if (clips.size() < 2) return failure("Choose a guide and at least one target audio clip.");
    std::vector<VocalAlignmentClip> ready;
    std::set<std::string> seen;
    for (const auto& address : clips) {
        if (!seen.insert(address.clipId).second) return failure("A clip cannot align to itself or appear twice.");
        const auto* clip = audioClip(address.trackId, address.clipId);
        if (!clip) return failure("Select audio clips only.");
        if (!clip->warp.enabled && clip->warp.markers.size() > 2)
            return failure(clip->name + ": Enable or reset the existing Warp map before aligning vocals.");
        if (const auto reason = warpUnavailableReason(address.trackId, address.clipId); !reason.empty())
            return failure(clip->name + ": " + reason);
        VocalAlignmentClip item;
        item.address = address; item.original = *clip; item.prepared = *clip;
        item.expectedStart = clip->startSeconds; item.generation = projectGeneration();
        item.expectedFingerprint = offlineSourceFingerprint(*clip) + ":" + std::to_string(offlineFileFingerprint(clip->offlineProcess.renderedFilePath)) + anchorState(*clip);
        auto& prepared = item.prepared;
        if (offlineProcessCacheValid(address)) {
            // A printed source is local to this placement, even when the
            // unprocessed content is shared by other clips in the project.
            prepared.detachContent();
            const auto parent = prepared.offlineVersionId;
            const auto originalId = newUuid(), renderedId = newUuid();
            prepared.offlineHistory.push_back({originalId, parent, "Before vocal alignment", captureClipAudioVersion(prepared)});
            ClipAudioVersionSource rendered;
            rendered.filePath = clip->offlineProcess.renderedFilePath;
            rendered.asset = clip->offlineProcess.renderedAsset;
            rendered.durationSeconds = clipPlaybackDuration(*clip);
            rendered.channels = clip->channels;
            prepared.offlineHistory.push_back({renderedId, originalId, "Processed audio", rendered});
            applyClipAudioVersion(prepared, rendered);
            prepared.offlineVersionId = renderedId;
            prepared.offlineProcess = {};
        }
        const auto data = processedClipSample(prepared, prepared.filePath, loadSamples(prepared.filePath));
        if (!data || !data->audio || !data->baseFrames) return failure(clip->name + ": Source audio is unavailable.");
        auto& source = item.source;
        source.audio = data->audio; source.startSeconds = clip->startSeconds;
        source.tempo = tempo(); source.durationSeconds = prepared.durationSeconds;
        source.pitch = prepared.sampleEdit.stretchPitch; source.formant = prepared.sampleEdit.formant;
        if (!prepared.warp.empty() && prepared.warp.enabled) {
            if (!prepared.warp.preservePitch)
                return failure(clip->name + ": Render pitch-changing Warp before aligning vocals.");
            source.baseline = prepared.warp;
        } else {
            const double end = std::min(double(data->baseFrames) / data->audio->sampleRate(),
                prepared.offsetSeconds + source.durationSeconds / prepared.sampleEdit.stretchTime);
            if (!std::isfinite(end) || end <= prepared.offsetSeconds || source.durationSeconds <= 0)
                return failure(clip->name + ": The clip has no readable audio.");
            source.baseline.enabled = true; source.baseline.mode = 3;
            source.baseline.baselineDurationSeconds = source.durationSeconds;
            source.baseline.markers = {{newUuid(), prepared.offsetSeconds, 0, true},
                {newUuid(), end, source.durationSeconds * tempo() / 60, true}};
            // Uniform resampling already changes pitch. Preserve that audible
            // tuning when the new map switches to pitch-preserving stretching.
            if (prepared.sampleEdit.stretchMode == ClipStretchMode::Resample) {
                source.pitch += 12 * std::log2(1 / prepared.sampleEdit.stretchTime);
                prepared.sampleEdit.stretchPitch = source.pitch;
            }
        }
        if (!validWarp(source.baseline) || !std::isfinite(source.durationSeconds) || source.durationSeconds <= 0)
            return failure(clip->name + ": Invalid source Warp map.");
        ready.push_back(std::move(item));
    }
    sources = std::move(ready);
    return audio::Result::ok();
} catch (const std::exception& e) { sources.clear(); return failure(e.what()); }

bool EngineController::vocalAlignmentSourcesCurrent(const std::vector<VocalAlignmentClip>& sources) const {
    if (!vocalAlignmentUnavailableReason().empty()) return false;
    for (const auto& source : sources) {
        const auto* clip = audioClip(source.address.trackId, source.address.clipId);
        if (source.generation != projectGeneration() || !clip || isTrackFrozen(source.address.trackId) ||
            tempo() != source.source.tempo || clip->startSeconds != source.expectedStart ||
            source.expectedFingerprint != offlineSourceFingerprint(*clip) + ":" + std::to_string(offlineFileFingerprint(clip->offlineProcess.renderedFilePath)) + anchorState(*clip)) return false;
    }
    return true;
}

audio::Result EngineController::applyVocalAlignment(std::vector<VocalAlignmentClip>& sources,
    const std::vector<alignment::AlignmentProposal>& proposals) {
    for (const auto& source : sources) if (isTrackEditLocked(source.address.trackId)) return audio::Result::fail(audio::EngineError::InvalidArgument, "Track editing is locked");
    if (sources.size() != proposals.size() + 1 || sources.size() < 2)
        return failure("Invalid vocal alignment result.");
    if (!vocalAlignmentSourcesCurrent(sources)) return failure("The audio changed during analysis. Take the selection again.");
    std::map<std::string, TrackModel> beforeByTrack, afterByTrack;
    for (std::size_t i = 0; i < proposals.size(); ++i) {
        const auto& source = sources[i + 1]; const auto& result = proposals[i];
        const bool reset = result.status == alignment::AlignmentStatus::Skipped;
        if (reset && !source.applied) continue;
        if (!validWarp(result.map) || result.map.empty() || !std::isfinite(result.startSeconds) || result.startSeconds < 0 ||
            !std::isfinite(result.durationSeconds) || result.durationSeconds <= 0 ||
            std::abs(result.map.markers.front().sourceSeconds - source.source.baseline.markers.front().sourceSeconds) > 1e-7 ||
            std::abs(result.map.markers.back().sourceSeconds - source.source.baseline.markers.back().sourceSeconds) > 1e-7 ||
            std::abs(result.durationSeconds - result.map.markers.back().targetBeats * 60 / tempo()) > 1e-7 ||
            warpMaximumRatio(result.map, tempo()) > 1000) return failure("Invalid vocal alignment Warp map.");
        for (std::size_t k = 1; k + 1 < source.source.baseline.markers.size(); ++k) {
            const auto& locked = source.source.baseline.markers[k];
            if (!locked.locked) continue;
            const auto found = std::find_if(result.map.markers.begin(), result.map.markers.end(), [&](const auto& marker) { return marker.id == locked.id; });
            if (found == result.map.markers.end() || !found->locked || found->sourceSeconds != locked.sourceSeconds ||
                std::abs(result.startSeconds + found->targetBeats * 60 / tempo() - source.source.startSeconds - locked.targetBeats * 60 / tempo()) > 1e-7)
                return failure("Invalid vocal alignment Warp map.");
        }
        const auto& id = source.address.trackId;
        if (!beforeByTrack.contains(id)) {
            TrackModel track; track.id = id; track.clips = m_project.findTrack(id)->clips;
            beforeByTrack.emplace(id, track); afterByTrack.emplace(id, std::move(track));
        }
        auto& clips = afterByTrack[id].clips;
        auto clip = std::find_if(clips.begin(), clips.end(), [&](const auto& c) { return c.id == source.address.clipId; });
        // Start from the current clip so unrelated gain, name, colour, inserts
        // and routing edits are never overwritten by the analysis snapshot.
        if (reset || result.rigidShift) {
            applyClipAudioVersion(*clip, captureClipAudioVersion(source.original));
            clip->contentId = source.original.contentId;
            clip->offlineProcess = source.original.offlineProcess;
            clip->offlineHistory = source.original.offlineHistory;
            clip->offlineVersionId = source.original.offlineVersionId;
            clip->startSeconds = reset ? source.original.startSeconds : result.startSeconds;
            continue;
        }
        if (source.original.offlineProcess.renderedFilePath != source.prepared.offlineProcess.renderedFilePath) {
            applyClipAudioVersion(*clip, captureClipAudioVersion(source.prepared));
            clip->contentId = source.prepared.contentId;
            clip->offlineHistory = source.prepared.offlineHistory;
            clip->offlineVersionId = source.prepared.offlineVersionId;
            clip->offlineProcess = {};
        }
        clip->sampleEdit.stretchPitch = source.prepared.sampleEdit.stretchPitch;
        clip->warp = result.map; clip->startSeconds = result.startSeconds;
        clip->durationSeconds = result.durationSeconds;
        clip->offsetSeconds = result.map.markers.front().sourceSeconds;
    }
    if (beforeByTrack.empty()) return audio::Result::ok();
    cancelWarpPreview(); cancelWarpEdit();
    ProjectModel before, after;
    for (auto& [id, track] : beforeByTrack) before.tracks.push_back(std::move(track));
    for (auto& [id, track] : afterByTrack) after.tracks.push_back(std::move(track));
    const auto restore = [this](const ProjectModel& state) {
        for (const auto& track : state.tracks)
            if (auto* target = m_project.findTrack(track.id)) target->clips = track.clips;
        m_project.resolveClipContents();
        auto result = rebuildGraphWithNewClipStates(state.tracks);
        updateTimelineDuration();
        return result;
    };
    try {
        if (auto result = restore(after); !result) { restore(before); return result; }
    } catch (const std::exception& error) {
        restore(before);
        return failure(error.what());
    }
    m_undo.push("Align Vocals", [restore, before] { restore(before); }, [restore, after] { restore(after); },
                estimatedProjectBytes(before) + estimatedProjectBytes(after));
    for (auto& source : sources) {
        const auto* clip = audioClip(source.address.trackId, source.address.clipId);
        source.expectedStart = clip->startSeconds;
        source.expectedFingerprint = offlineSourceFingerprint(*clip) + ":" + std::to_string(offlineFileFingerprint(clip->offlineProcess.renderedFilePath)) + anchorState(*clip);
    }
    for (std::size_t i = 0; i < proposals.size(); ++i)
        sources[i + 1].applied = proposals[i].status != alignment::AlignmentStatus::Skipped;
    return audio::Result::ok();
}
} // namespace daw
