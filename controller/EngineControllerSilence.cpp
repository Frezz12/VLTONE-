#include "EngineController.hpp"
#include "model/ProjectMemory.hpp"
#include "platform/PathUtils.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <set>

namespace daw {
namespace {
audio::Result silenceFailure(const std::string& message) {
    return audio::Result::fail(audio::EngineError::InvalidArgument, message);
}
bool unchangedRegion(const std::vector<SilenceRegion>& regions, double duration) {
    return regions.size() == 1 && regions.front().begin <= 1e-9 &&
           regions.front().end >= duration - 1e-9;
}
}

audio::Result EngineController::prepareStripSilence(const std::vector<ClipAddress>& clips,
    std::vector<StripSilenceSource>& sources) try {
    sources.clear();
    if (cloudProjectBound()) return silenceFailure("Strip Silence is available in local projects.");
    if (isRecording()) return silenceFailure("Stop recording before processing audio clips.");
    if (clips.empty()) return silenceFailure("Select one or more audio clips.");
    flushDeferredClipSync();
    std::set<std::pair<std::string, std::string>> seen;
    std::vector<StripSilenceSource> prepared;
    for (const auto& address : clips) {
        if (!seen.emplace(address.trackId, address.clipId).second) continue;
        const auto* clip = audioClip(address.trackId, address.clipId);
        if (!clip) return silenceFailure("Strip Silence accepts audio clips only.");
        StripSilenceSource source;
        source.address = address;
        source.original = *clip;
        source.durationSeconds = effectiveClipLength(*clip);
        source.sampleRate = m_sampleRate;
        source.tempo = tempo();
        if (!(source.durationSeconds > 0) || !std::isfinite(source.durationSeconds))
            return silenceFailure("The clip has no readable audio.");
        if (offlineProcessCacheValid(address)) source.renderedPath = clip->offlineProcess.renderedFilePath;
        if (source.renderedPath.empty() && !isLayered(*clip) && !loadSamples(clip->filePath))
            return silenceFailure("Source audio is unavailable: " + clip->name);
        if (source.renderedPath.empty()) for (const auto& part : clip->comp) {
            const auto* take = findTake(*clip, part.takeId);
            if (!take || (!take->muted && !loadSamples(take->filePath)))
                return silenceFailure("A comp take's source audio is unavailable: " + clip->name);
        }
        source.fingerprint = offlineSourceFingerprint(*clip) + ":" +
            std::to_string(offlineFileFingerprint(source.renderedPath));
        auto audible = *clip;
        audible.muted = false;
        // Analyze the stored comp, independent of a temporary take audition.
        const auto solo = std::exchange(m_soloClipId, {});
        try { emitClipPlacements(audible, source.placements); }
        catch (...) { m_soloClipId = solo; throw; }
        m_soloClipId = solo;
        if (source.placements.empty()) return silenceFailure("The clip has no readable audio.");
        const auto origin = toSamples(clip->startSeconds);
        for (auto& placement : source.placements) placement.startSample -= origin;
        prepared.push_back(std::move(source));
    }
    sources = std::move(prepared);
    return audio::Result::ok();
} catch (const std::exception& error) {
    sources.clear();
    return silenceFailure(error.what());
}

audio::Result EngineController::applyStripSilence(const std::vector<StripSilenceSource>& sources,
    const std::vector<std::vector<SilenceRegion>>& regions,
    const StripSilenceSettings& settings, std::vector<ClipAddress>& created, bool recordUndo) {
    created.clear();
    if (cloudProjectBound()) return silenceFailure("Strip Silence is available in local projects.");
    if (isRecording()) return silenceFailure("Stop recording before processing audio clips.");
    if (sources.empty() || sources.size() != regions.size()) return silenceFailure("Invalid silence analysis.");
    const auto options = sanitizedStripSilenceSettings(settings);
    struct PendingFiles {
        std::vector<std::filesystem::path> paths;
        bool committed = false;
        ~PendingFiles() { if (!committed) for (const auto& path : paths) {
            std::error_code error; std::filesystem::remove(path, error);
        } }
    } files;
    std::map<std::string, TrackModel> beforeByTrack, afterByTrack;
    std::vector<TrackModel> pluginSources;
    std::size_t fragments = 0;
    std::set<std::pair<std::string, std::string>> seen;
    for (std::size_t i = 0; i < sources.size(); ++i) {
        const auto& source = sources[i];
        if (!seen.emplace(source.address.trackId, source.address.clipId).second)
            return silenceFailure("Duplicate clip in silence analysis.");
        const auto* clip = audioClip(source.address.trackId, source.address.clipId);
        if (!clip || clip->startSeconds != source.original.startSeconds || tempo() != source.tempo ||
            offlineSourceFingerprint(*clip) + ":" + std::to_string(offlineFileFingerprint(source.renderedPath)) != source.fingerprint)
            return silenceFailure("The audio changed during analysis. Open Strip Silence again.");
        double previous = 0;
        for (const auto& region : regions[i]) {
            if (!std::isfinite(region.begin) || !std::isfinite(region.end) ||
                region.begin < previous || region.end <= region.begin ||
                region.end > source.durationSeconds + 1e-9)
                return silenceFailure("Invalid silence boundaries.");
            previous = region.end;
        }
        fragments += regions[i].size();
        if (fragments > 10000) return silenceFailure("Too many fragments. Increase the minimum silence duration.");
        if (unchangedRegion(regions[i], source.durationSeconds)) { created.push_back(source.address); continue; }
        if (!beforeByTrack.contains(source.address.trackId)) {
            TrackModel track;
            track.id = source.address.trackId;
            track.clips = m_project.findTrack(track.id)->clips;
            beforeByTrack.emplace(track.id, std::move(track));
        }
        TrackModel selected;
        selected.id = source.address.trackId;
        selected.clips = {*clip};
        pluginSources.push_back(std::move(selected));
    }
    if (beforeByTrack.empty()) return audio::Result::ok();
    // Capture opaque presets as well as exposed parameters before duplicating
    // Clip FX. Undo/redo restores exactly the same sound on every fragment.
    if (auto result = captureLibraryPlugins(pluginSources); !result) return result;
    for (const auto& track : pluginSources) for (const auto& captured : track.clips)
        for (auto& original : beforeByTrack[track.id].clips)
            if (original.id == captured.id) original = captured;
    afterByTrack = beforeByTrack;
    for (std::size_t i = 0; i < sources.size(); ++i) {
        const auto& source = sources[i];
        if (unchangedRegion(regions[i], source.durationSeconds)) continue;
        auto& clips = afterByTrack[source.address.trackId].clips;
        auto at = std::find_if(clips.begin(), clips.end(), [&](const auto& clip) { return clip.id == source.address.clipId; });
        auto original = *at;
        std::string rendered = source.renderedPath;
        if (rendered.empty() && (original.sampleEdit.loopMode != 0 ||
            (original.fadeInMode == ClipFadeMode::Tape && original.fadeInSeconds > 0) ||
            (original.fadeOutMode == ClipFadeMode::Tape && original.fadeOutSeconds > 0))) {
            // Loop phase and tape speed cannot be expressed by a source trim.
            // Stream their audible result to a new file; Undo retains the exact
            // original source and settings, with no alteration of its media.
            const auto path = platform::pathFromUtf8(m_recordDir) / ("silence-" + newUuid() + ".wav");
            std::error_code error;
            std::filesystem::create_directories(path.parent_path(), error);
            files.paths.push_back(path);
            rendered = platform::pathToUtf8(path);
            try { buildSilenceEnvelope(source.placements, source.durationSeconds, source.sampleRate, {}, rendered); }
            catch (const std::exception& failure) { return silenceFailure(failure.what()); }
        }
        if (!rendered.empty()) {
            // The offline result already contains source edits and their tails.
            // Keep using those samples rather than invalidating its cache and
            // accidentally falling back to the unprocessed original on a cut.
            original.filePath = rendered;
            original.asset = {};
            original.offsetSeconds = 0;
            original.durationSeconds = source.durationSeconds;
            original.sampleEdit = {};
            original.warp = {};
            original.takes.clear(); original.comp.clear();
            original.gain = 1; original.pan = 0;
            original.fadeInSeconds = original.fadeOutSeconds = 0;
            original.offlineProcess = {};
            original.offlineVersionId.clear();
        }
        std::vector<ClipModel> pieces;
        for (const auto region : regions[i]) {
            auto piece = sliceSilenceRegion(original, region, source.durationSeconds, options.fadeMs, source.tempo);
            created.push_back({source.address.trackId, piece.id});
            pieces.push_back(std::move(piece));
        }
        const auto index = std::size_t(at - clips.begin());
        clips.erase(at);
        clips.insert(clips.begin() + index, std::make_move_iterator(pieces.begin()), std::make_move_iterator(pieces.end()));
    }
    ProjectModel before, after;
    for (auto& [id, track] : beforeByTrack) before.tracks.push_back(std::move(track));
    for (auto& [id, track] : afterByTrack) after.tracks.push_back(std::move(track));
    const auto restore = [this](const ProjectModel& state) {
        for (const auto& track : state.tracks)
            if (auto* target = m_project.findTrack(track.id)) target->clips = track.clips;
        auto result = rebuildGraph();
        if (result) result = restoreLibraryPluginStates(state.tracks, false);
        updateTimelineDuration();
        return result;
    };
    if (auto result = restore(after); !result) { restore(before); created.clear(); return result; }
    files.committed = true;
    if (recordUndo) {
        const auto bytes = estimatedProjectBytes(before) + estimatedProjectBytes(after);
        m_undo.push("Strip Silence", [restore, before] { restore(before); },
            [restore, after] { restore(after); }, bytes);
    }
    return audio::Result::ok();
}

void EngineController::stripRecordedSilence(const FinalizedRecordingTrack& recording) {
    if (recording.midi || !recording.semantics.autoSilence || cloudProjectBound()) return;
    const auto* track = m_project.findTrack(recording.trackId);
    if (!track) return;
    std::vector<ClipAddress> clips;
    for (const auto& clip : track->clips) {
        if (clip.kind == ClipKind::Audio && (clip.filePath == recording.closedWavPath ||
            std::any_of(clip.takes.begin(), clip.takes.end(), [&](const auto& take) {
                return take.filePath == recording.closedWavPath;
            }))) clips.push_back({track->id, clip.id});
    }
    if (clips.empty()) return;
    std::vector<StripSilenceSource> sources;
    auto result = prepareStripSilence(clips, sources);
    if (!result) { m_autoSilenceWarning = result.message(); return; }
    std::vector<std::vector<SilenceRegion>> regions;
    try {
        for (const auto& source : sources) {
            auto envelope = buildSilenceEnvelope(source.placements, source.durationSeconds, source.sampleRate);
            auto kept = detectSilenceRegions(envelope, recording.semantics.stripSilence, source.original.startSeconds, source.tempo);
            // A punch into an existing layered clip must leave material outside
            // this recording untouched, even if its level is below the threshold.
            std::vector<SilenceRegion> recorded;
            for (const auto& pass : recording.passes) {
                const double a = std::max(0.0, pass.startSeconds - source.original.startSeconds);
                const double b = std::min(source.durationSeconds, pass.endSeconds - source.original.startSeconds);
                if (b > a) recorded.push_back({a, b});
            }
            std::sort(recorded.begin(), recorded.end(), [](auto a, auto b) { return a.begin < b.begin; });
            double cursor = 0;
            for (const auto& region : recorded) {
                if (region.begin > cursor) kept.push_back({cursor, region.begin});
                cursor = std::max(cursor, region.end);
            }
            if (cursor < source.durationSeconds) kept.push_back({cursor, source.durationSeconds});
            std::sort(kept.begin(), kept.end(), [](auto a, auto b) { return a.begin < b.begin; });
            std::vector<SilenceRegion> merged;
            for (const auto& region : kept) {
                if (!merged.empty() && region.begin <= merged.back().end + 1e-9)
                    merged.back().end = std::max(merged.back().end, region.end);
                else merged.push_back(region);
            }
            regions.push_back(std::move(merged));
        }
        std::vector<ClipAddress> created;
        result = applyStripSilence(sources, regions, recording.semantics.stripSilence, created, false);
        if (!result) m_autoSilenceWarning = result.message();
    } catch (const std::exception& error) { m_autoSilenceWarning = error.what(); }
}

} // namespace daw
