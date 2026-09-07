#include "EngineController.hpp"
#include "ProjectSerializer.hpp"
#include "platform/PathUtils.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>

namespace daw {
namespace {
void normalizeFreezeSlot(InsertModel& slot) {
    // Saving state and moving an editor do not change the baked sound.
    slot.stateFile.clear(); slot.rightStateFile.clear();
    slot.stateAsset = {}; slot.rightStateAsset = {};
    slot.windowOpen = false;
    slot.windowX = slot.windowY = slot.windowWidth = slot.windowHeight = 0;
    slot.editorChannel = PluginEditorChannel::Left;
}
}

bool EngineController::isTrackFrozen(const std::string& id) const {
    const auto* track = m_project.findTrack(id);
    return track && track->freeze.active();
}

std::string EngineController::freezeUnavailableReason(const std::string& id) {
    const auto* track = m_project.findTrack(id);
    if (!track) return "Track does not exist";
    if (cloudProjectBound()) return "Freeze is available in local projects";
    if (track->kind != TrackKind::Audio && track->kind != TrackKind::Midi &&
        track->kind != TrackKind::Instrument) return "Only audio and MIDI source tracks can be frozen";
    if (track->armed || track->monitor) return "Disable recording and monitoring before freezing";
    const auto external = [](const InsertModel& slot) { return !slot.sidechainTrackId.empty(); };
    if (external(track->instrument) || std::any_of(track->inserts.begin(), track->inserts.end(), external) ||
        std::any_of(track->samplerFx.inserts.begin(), track->samplerFx.inserts.end(), external))
        return "Tracks with an external sidechain cannot be frozen independently";
    bool material = false;
    for (const auto& clip : track->clips) {
        if (clip.playbackInjection.active() || !clip.patternClipId.empty() ||
            std::any_of(clip.inserts.begin(), clip.inserts.end(), external))
            return "Linked clips and external clip routing cannot be frozen independently";
        material |= clip.kind == ClipKind::Audio || clip.kind == ClipKind::Midi;
    }
    if (!material) return "The track has no audio or MIDI clips";
    for (const auto& other : m_project.tracks) {
        if (other.id != id && other.outputBusId == id)
            return "The track receives audio from another track";
        for (const auto& send : other.sends)
            if (send.destinationTrackId == id) return "The track receives a send";
        for (const auto& clip : other.clips) {
            if (clip.playbackInjection.active() && clip.playbackInjection.anchorChannelId == id)
                return "The track receives externally routed clips";
            // First version leaves channel automation live, but requires plugin
            // automation to be made independent before baking this source.
            if (clip.kind == ClipKind::Automation && clip.automation.active &&
                clip.automation.target.channelId == id &&
                clip.automation.target.kind == AutomationTargetKind::PluginParameter)
                return "Disable external plugin automation before freezing";
        }
    }
    return {};
}

std::string EngineController::freezeFingerprint(const TrackModel& track) const {
    ProjectModel source;
    source.tempo = m_project.tempo;
    source.sampleRate = m_project.sampleRate;
    TrackModel copy;
    copy.id = track.id; copy.kind = track.kind;
    copy.instrument = track.instrument; copy.inserts = track.inserts;
    copy.samplerFx = track.samplerFx; copy.clips = track.clips;
    normalizeFreezeSlot(copy.instrument);
    for (auto& slot : copy.inserts) normalizeFreezeSlot(slot);
    for (auto& slot : copy.samplerFx.inserts) normalizeFreezeSlot(slot);
    for (auto& clip : copy.clips) for (auto& slot : clip.inserts) normalizeFreezeSlot(slot);
    source.tracks.push_back(std::move(copy));
    std::string bytes;
    (void)ProjectSerializer::serializeDocument(source, bytes, MediaPaths::Absolute);
    return bytes; // Exact comparison, with no hash collision or RT access.
}

bool EngineController::invalidateTrackFreeze(const TrackModel& source) {
    if (!source.freeze.active() || m_rebuildingFrozenGraph) return false;
    if (!freezeUnavailableReason(source.id).empty() ||
        source.freeze.sourceFingerprint != freezeFingerprint(source)) {
        if (auto* track = m_project.findTrack(source.id)) track->freeze = {};
        (void)rebuildGraph();
        return true;
    }
    return false;
}

bool EngineController::unfreezeTrack(const std::string& id, bool undoable) {
    auto* track = m_project.findTrack(id);
    if (!track || !track->freeze.active()) return false;
    std::optional<ProjectModel> before;
    if (undoable) before = m_project;
    track->freeze = {};
    (void)rebuildGraph();
    if (before) pushProjectSnapshotUndo(*before, "Unfreeze Track");
    return true;
}

audio::Result EngineController::freezeTrack(const std::string& id,
    const std::function<bool(const rendering::Progress&)>& onProgress,
    rendering::Report& out) {
    out = {};
    const auto reason = freezeUnavailableReason(id);
    if (!reason.empty()) return audio::Result::fail(audio::EngineError::InvalidArgument, reason);
    if (isTrackFrozen(id)) return audio::Result::ok();
    const auto revision = projectRevision();
    const auto fingerprint = freezeFingerprint(*m_project.findTrack(id));
    rendering::Spec spec;
    spec.outputDir = m_recordDir; spec.baseName = "freeze-" + newUuid();
    spec.file.container = audio::platform::Container::Wav;
    spec.file.encoding = audio::platform::Encoding::Float32;
    spec.range = rendering::Range::Custom;
    for (const auto& clip : m_project.findTrack(id)->clips)
        spec.customEndSeconds = std::max(spec.customEndSeconds, clip.startSeconds + clipPlaybackDuration(clip));
    spec.tail = rendering::Tail::UntilSilence;
    spec.writeMixdown = false; spec.stemChannelIds = {id};
    spec.independentTrackId = id; spec.stemsPreFader = true;
    spec.bypassSends = true; spec.bypassMasterChain = true; spec.ignoreMuteSolo = true;
    auto result = renderProject(spec, [&](const rendering::Progress& progress) {
        const bool proceed = !onProgress || onProgress(progress);
        return proceed && projectRevision() == revision;
    }, out);
    auto discard = [&] {
        for (const auto& file : out.files) {
            m_samples.erase(file);
            std::error_code ignored;
            std::filesystem::remove(platform::pathFromUtf8(file), ignored);
        }
        out.files.clear();
    };
    auto* track = m_project.findTrack(id);
    const bool stale = projectRevision() != revision || !track ||
        freezeFingerprint(*track) != fingerprint || !freezeUnavailableReason(id).empty();
    if (!result || out.cancelled || stale || out.files.size() != 1) {
        discard();
        if (stale || out.cancelled) { out.cancelled = true; return audio::Result::ok(); }
        if (!result) return result;
        return audio::Result::fail(audio::EngineError::FileWriteError, "Freeze produced no audio");
    }
    auto samples = loadSamples(out.files.front());
    if (!samples) { discard(); return audio::Result::fail(audio::EngineError::FileNotFound, "Cannot read frozen audio"); }
    const auto before = m_project;
    track->freeze = {out.files.front(), out.renderedSeconds, m_sampleRate, fingerprint};
    result = rebuildGraph();
    if (!result) { m_project = before; (void)rebuildGraph(); discard(); return result; }
    pushProjectSnapshotUndo(before, "Freeze Track");
    return audio::Result::ok();
}
} // namespace daw
