#include "EngineController.hpp"
#include "Internal/SampleDecoder.hpp"
#include "Internal/SamplerPrecompute.hpp"

namespace daw {

std::string EngineController::warpUnavailableReason(const std::string& trackId,
                                                    const std::string& clipId) {
    if (cloudProjectBound()) return "Warp is available in local projects only.";
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
    if (!warpUnavailableReason(trackId, clipId).empty() || !validWarp(warp) ||
        (!warp.empty() && warpMaximumRatio(warp, tempo()) > 1000)) return false;
    auto* clip = findClip(trackId, clipId);
    if (!clip || clip->warp == warp) return false;
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
    cancelWarpEdit();
    if (!warpUnavailableReason(trackId, clipId).empty()) return;
    if (const auto* clip = audioClip(trackId, clipId)) m_warpEdit = WarpEdit{trackId, clipId, clip->warp};
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
    const auto apply = [this, track = edit.trackId, clipId = edit.clipId, duration](const ClipWarpModel& value) {
        applyClipWarpState(track, clipId, value, duration);
    };
    m_undo.push("Move Warp Markers", [apply, before = edit.before] { apply(before); }, [apply, after] { apply(after); });
}

void EngineController::cancelWarpEdit() {
    if (!m_warpEdit) return;
    const auto edit = *m_warpEdit;
    setClipWarp(edit.trackId, edit.clipId, edit.before);
    m_warpEdit.reset();
    flushDeferredClipSync();
}

} // namespace daw
