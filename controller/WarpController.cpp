#include "EngineController.hpp"
#include "Internal/SampleDecoder.hpp"
#include "Internal/SamplerPrecompute.hpp"

namespace daw {

std::string EngineController::warpUnavailableReason(const std::string& trackId,
                                                    const std::string& clipId) {
    if (!sharedEditingAllowed()) return "This session is read-only.";
    if (isTrackFrozen(trackId)) return "Unfreeze the track before using Warp.";
    const auto* clip = audioClip(trackId, clipId);
    if (!clip) return "Select an audio clip.";
    if (isLayered(*clip)) return "Bounce the comp to an audio clip before using Warp.";
    if (clip->sampleEdit.loopMode) return "Disable the sample loop before using Warp.";
    if (clip->channels > 2) return "Warp supports mono and stereo audio.";
    if (clip->fadeInMode == ClipFadeMode::Tape || clip->fadeOutMode == ClipFadeMode::Tape)
        return "Change tape fades to volume fades before using Warp.";
    return {};
}

bool EngineController::initializeClipWarp(const std::string& trackId, const std::string& clipId) {
    if (!warpUnavailableReason(trackId, clipId).empty()) return false;
    const auto* clip = audioClip(trackId, clipId);
    if (!clip->warp.empty()) return true;
    auto data = clipSampleData(trackId, clipId);
    if (!data || !data->audio || !data->baseFrames || data->audio->sampleRate() <= 0) return false;
    const double sourceEnd = double(data->baseFrames) / data->audio->sampleRate();
    const double length = clip->durationSeconds > 0 ? clip->durationSeconds :
        (sourceEnd - clip->offsetSeconds) * clip->sampleEdit.stretchTime;
    const double end = std::min(sourceEnd, clip->offsetSeconds + length / clip->sampleEdit.stretchTime);
    if (end - clip->offsetSeconds < 1e-6 || length <= 0) return false;
    ClipWarpModel warp;
    warp.enabled = true;
    warp.baselineDurationSeconds = length;
    warp.mode = std::max(1, int(clip->sampleEdit.stretchMode));
    if (clip->sampleEdit.stretchMode == ClipStretchMode::Resample) {
        warp.mode = 4;
        warp.preservePitch = std::abs(clip->sampleEdit.stretchTime - 1) < 1e-9 ||
                             std::abs(clip->sampleEdit.stretchPitch) > .001;
    }
    warp.markers = {{newUuid(), clip->offsetSeconds, 0, true},
                    {newUuid(), end, secondsToBeats(length, tempo()), true}};
    return setClipWarp(trackId, clipId, warp, "Enable Warp");
}

bool EngineController::setClipWarp(const std::string& trackId, const std::string& clipId,
                                  const ClipWarpModel& warp, const std::string& label) {
    if (!sharedGestureAllowed("clip:" + clipId)) return false;
    if (!warpUnavailableReason(trackId, clipId).empty() || !validWarp(warp) ||
        (!warp.empty() && warpMaximumRatio(warp, tempo()) > 1000)) return false;
    auto* clip = findClip(trackId, clipId);
    if (!clip || clip->warp == warp) return false;
    cancelWarpPreview();
    if (!warp.empty()) {
        auto data = clipSampleData(trackId, clipId);
        if (!data || !data->audio) {
            // An offline source must not trap a saved map in its enabled state.
            // Existing anchors remain editable within the known source range.
            if (clip->warp.empty() || warp.markers.front().sourceSeconds < clip->warp.markers.front().sourceSeconds - 1e-7 ||
                warp.markers.back().sourceSeconds > clip->warp.markers.back().sourceSeconds + 1e-7) return false;
        } else if (warp.markers.back().sourceSeconds >
                   double(data->baseFrames) / data->audio->sampleRate() + 1e-7) return false;
    }
    const auto before = clip->warp;
    const double originalDuration = clip->durationSeconds;
    if (cloudProjectBound() && !m_warpEdit) {
        auto after = *clip; after.warp = warp;
        after.durationSeconds = warp.empty() ? originalDuration : warp.enabled ?
            beatsToSeconds(warp.markers.back().targetBeats, tempo()) : warp.baselineDurationSeconds;
        if (!warp.empty()) after.offsetSeconds = warp.markers.front().sourceSeconds;
        return submitSharedMutation(collab::sharedRenderState(trackId, after), label) == collab::SharedMutationResult::Submitted;
    }
    auto apply = [this, trackId, clipId, originalDuration](const ClipWarpModel& value) {
        applyClipWarpState(trackId, clipId, value, originalDuration);
    };
    apply(warp);
    if (!m_warpEdit) m_undo.push(label, [apply, before] { apply(before); },
                                      [apply, warp] { apply(warp); });
    return true;
}

void EngineController::applyClipWarpState(const std::string& trackId, const std::string& clipId,
                                         const ClipWarpModel& value, double fallbackDuration) {
    auto* target = findClip(trackId, clipId);
    auto* track = m_project.findTrack(trackId);
    if (!target || !track) return;
    target->warp = value;
    target->durationSeconds = value.empty() ? fallbackDuration :
        value.enabled ? beatsToSeconds(value.markers.back().targetBeats, tempo()) : value.baselineDurationSeconds;
    if (!value.empty()) target->offsetSeconds = value.markers.front().sourceSeconds;
    if (m_warpEdit) deferClipSync(trackId); else syncTrackClips(*track);
    updateTimelineDuration();
}

void EngineController::beginWarpEdit(const std::string& trackId, const std::string& clipId) {
    if (!sharedGestureAllowed("clip:" + clipId)) return;
    cancelWarpPreview();
    cancelWarpEdit();
    if (!warpUnavailableReason(trackId, clipId).empty()) return;
    if (const auto* clip = audioClip(trackId, clipId)) m_warpEdit = WarpEdit{trackId, clipId, clip->warp, clip->durationSeconds, clip->offsetSeconds};
}

void EngineController::commitWarpEdit() {
    if (!m_warpEdit) return;
    auto edit = std::move(*m_warpEdit);
    const auto* clip = audioClip(edit.trackId, edit.clipId);
    const auto after = clip ? clip->warp : edit.before;
    const double duration = clip ? clip->durationSeconds : 0;
    m_warpEdit.reset();
    flushDeferredClipSync();
    if (!clip || after == edit.before) return;
    if (cloudProjectBound()) {
        auto command = collab::sharedRenderState(edit.trackId, *clip);
        if (auto* target = findClip(edit.trackId, edit.clipId)) target->offsetSeconds = edit.beforeOffset;
        applyClipWarpState(edit.trackId, edit.clipId, edit.before, edit.beforeDuration);
        (void)submitSharedMutation(std::move(command), "Move Warp Markers");
        return;
    }
    const double offset = clip->offsetSeconds;
    const auto apply = [this, track = edit.trackId, clipId = edit.clipId](const ClipWarpModel& value, double duration, double offset) {
        if (auto* target = findClip(track, clipId)) target->offsetSeconds = offset;
        applyClipWarpState(track, clipId, value, duration);
    };
    m_undo.push("Move Warp Markers", [apply, edit] { apply(edit.before, edit.beforeDuration, edit.beforeOffset); },
        [apply, after, duration, offset] { apply(after, duration, offset); });
}

void EngineController::cancelWarpEdit() {
    if (!m_warpEdit) return;
    const auto edit = *m_warpEdit;
    if (auto* target = findClip(edit.trackId, edit.clipId)) target->offsetSeconds = edit.beforeOffset;
    applyClipWarpState(edit.trackId, edit.clipId, edit.before, edit.beforeDuration);
    m_warpEdit.reset();
    flushDeferredClipSync();
}

bool EngineController::validWarpPreview() const {
    if (!m_warpPreview || !sharedEditingAllowed() || isTrackFrozen(m_warpPreview->trackId)) return false;
    const auto& session = *m_warpPreview;
    const auto* clip = audioClip(session.trackId, session.clipId);
    return clip && clip->warp == session.before && clip->startSeconds == session.startSeconds &&
        tempo() == session.tempo && m_project.timeSigNumerator == session.numerator &&
        m_project.timeSigDenominator == session.denominator &&
        offlineSourceFingerprint(*clip) == session.fingerprint;
}

bool EngineController::warpPreviewActive() {
    if (m_warpPreview && !validWarpPreview()) cancelWarpPreview();
    return m_warpPreview.has_value();
}

const ClipWarpModel* EngineController::warpPreviewMap() const {
    return validWarpPreview() ? &m_warpPreview->proposed : nullptr;
}

bool EngineController::beginWarpPreview(const std::string& trackId, const std::string& clipId) {
    if (!sharedGestureAllowed("clip:" + clipId)) return false;
    cancelWarpEdit();
    cancelWarpPreview();
    if (!warpUnavailableReason(trackId, clipId).empty()) return false;
    const auto* clip = audioClip(trackId, clipId);
    if (!clip || clip->warp.empty() || !clip->warp.enabled || !clipSampleData(trackId, clipId)) return false;
    m_warpPreview = WarpPreview{trackId, clipId, offlineSourceFingerprint(*clip), clip->warp,
        clip->warp, clip->startSeconds, tempo(), m_project.timeSigNumerator, m_project.timeSigDenominator, true};
    return true;
}

bool EngineController::updateWarpPreview(const ClipWarpModel& map) {
    if (!warpPreviewActive() || !map.enabled || !validWarp(map) || warpMaximumRatio(map, tempo()) > 1000) return false;
    auto& session = *m_warpPreview;
    if (map.markers.front() != session.before.markers.front() ||
        map.markers.back() != session.before.markers.back()) return false;
    // An assistant may insert anchors or move timing, never discard manual work.
    auto it = map.markers.begin();
    for (const auto& old : session.before.markers) {
        while (it != map.markers.end() && it->sourceSeconds < old.sourceSeconds) ++it;
        if (it == map.markers.end() || it->sourceSeconds != old.sourceSeconds ||
            it->id != old.id || it->locked != old.locked || (old.locked && it->targetBeats != old.targetBeats)) return false;
    }
    if (session.proposed == map) return true;
    session.proposed = map;
    if (session.after) if (auto* track = m_project.findTrack(session.trackId)) syncTrackClips(*track);
    return true;
}

bool EngineController::auditionWarpPreview(bool after) {
    if (!warpPreviewActive()) return false;
    if (m_warpPreview->after == after) return true;
    m_warpPreview->after = after;
    if (auto* track = m_project.findTrack(m_warpPreview->trackId)) syncTrackClips(*track);
    return true;
}

bool EngineController::commitWarpPreview() {
    if (!warpPreviewActive()) return false;
    const auto session = std::move(*m_warpPreview);
    m_warpPreview.reset();
    const bool changed = setClipWarp(session.trackId, session.clipId, session.proposed, "Align Warp Timing");
    if (!changed) if (auto* track = m_project.findTrack(session.trackId)) syncTrackClips(*track);
    return changed;
}

void EngineController::cancelWarpPreview() {
    if (!m_warpPreview) return;
    const auto trackId = m_warpPreview->trackId;
    m_warpPreview.reset();
    if (auto* track = m_project.findTrack(trackId)) syncTrackClips(*track);
}

} // namespace daw
