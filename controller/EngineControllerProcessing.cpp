#include "EngineController.hpp"

#include "plugins/PluginConvert.hpp"
#include "platform/AudioFileDecoder.hpp"
#include "platform/PathUtils.hpp"

#include <algorithm>
#include <filesystem>
#include <map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace daw {
namespace {

bool layerEnabled(std::uint32_t mask, EngineController::BounceFxLayer layer) {
    return (mask & std::uint32_t(layer)) != 0;
}

std::string bounceName(const TrackModel* track) {
    return (track && !track->name.empty() ? track->name : std::string("Mix")) +
           " Bounce";
}

void removeFiles(const std::vector<std::string>& files) {
    for (const std::string& file : files) {
        std::error_code ignored;
        fs::remove(platform::pathFromUtf8(file), ignored);
    }
}

struct BounceJob {
    std::string sourceTrackId;
    std::vector<std::string> sourceTrackIds;
    std::vector<std::string> sourceClipIds;
    std::vector<EngineController::ClipAddress> affected;
    std::string captureChannelId;
    bool captureAtSource = false;
    bool capturePreFader = false;
    PlaybackInjection injection;
    std::string file;
    double renderedSeconds = 0.0;
};

} // namespace

audio::Result EngineController::bounceInPlace(
    const BounceRequest& request,
    const std::function<bool(const rendering::Progress&)>& onProgress,
    BounceReport& out) {
    out = {};
    if (cloudProjectBound()) {
        return audio::Result::fail(audio::EngineError::InvalidArgument,
                                   "Bounce in Place is local-only");
    }
    if (!(request.endSeconds > request.startSeconds)) {
        return audio::Result::fail(audio::EngineError::InvalidArgument,
                                   "the bounce range is empty");
    }

    std::vector<BounceJob> jobs;
    if (request.fullMix) {
        BounceJob job;
        for (const TrackModel& track : m_project.tracks) {
            for (const ClipModel& clip : track.clips) {
                if (clip.kind == ClipKind::Automation ||
                    clip.startSeconds + clipPlaybackDuration(clip) <=
                        request.startSeconds ||
                    clip.startSeconds >= request.endSeconds) {
                    continue;
                }
                job.affected.push_back({track.id, clip.id});
            }
        }
        jobs.push_back(std::move(job));
    } else if (!request.clips.empty()) {
        std::map<std::string, std::vector<ClipAddress>> grouped;
        for (const ClipAddress& address : request.clips) {
            const TrackModel* track = m_project.findTrack(address.trackId);
            if (!track) continue;
            const auto clip = std::find_if(
                track->clips.begin(), track->clips.end(),
                [&](const ClipModel& candidate) {
                    return candidate.id == address.clipId;
                });
            if (clip == track->clips.end() ||
                clip->kind == ClipKind::Automation ||
                clip->startSeconds + clipPlaybackDuration(*clip) <=
                    request.startSeconds ||
                clip->startSeconds >= request.endSeconds) {
                continue;
            }
            grouped[address.trackId].push_back(address);
        }
        for (auto& [trackId, clips] : grouped) {
            if (!m_project.findTrack(trackId)) continue;
            BounceJob job;
            job.sourceTrackId = trackId;
            job.sourceTrackIds.push_back(trackId);
            job.affected = clips;
            for (const ClipAddress& address : clips)
                job.sourceClipIds.push_back(address.clipId);
            jobs.push_back(std::move(job));
        }
    } else {
        for (const std::string& trackId : request.tracks) {
            const TrackModel* track = m_project.findTrack(trackId);
            if (!track) continue;
            BounceJob job;
            job.sourceTrackId = trackId;
            job.sourceTrackIds.push_back(trackId);
            for (const ClipModel& clip : track->clips) {
                if (clip.kind == ClipKind::Automation ||
                    clip.startSeconds + clipPlaybackDuration(clip) <=
                        request.startSeconds ||
                    clip.startSeconds >= request.endSeconds) {
                    continue;
                }
                job.affected.push_back({trackId, clip.id});
            }
            jobs.push_back(std::move(job));
        }
    }
    if (jobs.empty()) {
        return audio::Result::fail(audio::EngineError::InvalidArgument,
                                   "nothing selected to bounce");
    }

    // Pattern owners render through their linked member tracks. Include those
    // channels in source isolation and in the Track-FX bypass decision.
    for (BounceJob& job : jobs) {
        std::unordered_set<std::string> owners(job.sourceClipIds.begin(),
                                               job.sourceClipIds.end());
        if (const TrackModel* track = m_project.findTrack(job.sourceTrackId)) {
            if (track->kind == TrackKind::Pattern && owners.empty()) {
                for (const ClipModel& clip : track->clips)
                    if (clip.kind == ClipKind::Pattern)
                        owners.insert(clip.id);
            }
        }
        for (const TrackModel& track : m_project.tracks) {
            if (std::any_of(track.clips.begin(), track.clips.end(),
                            [&](const ClipModel& clip) {
                                return !clip.patternClipId.empty() &&
                                       owners.contains(clip.patternClipId);
                            })) {
                job.sourceTrackIds.push_back(track.id);
            }
        }
        std::sort(job.sourceTrackIds.begin(), job.sourceTrackIds.end());
        job.sourceTrackIds.erase(
            std::unique(job.sourceTrackIds.begin(), job.sourceTrackIds.end()),
            job.sourceTrackIds.end());
    }

    const bool printTrack =
        layerEnabled(request.fxLayers, BounceFxLayer::Track);
    const bool printFolders =
        layerEnabled(request.fxLayers, BounceFxLayer::Summing);
    const bool printSends =
        layerEnabled(request.fxLayers, BounceFxLayer::Sends);
    const bool printMaster =
        layerEnabled(request.fxLayers, BounceFxLayer::Master);

    for (BounceJob& job : jobs) {
        if (printMaster) {
            job.captureChannelId = kMasterChannelId;
            job.capturePreFader = true;
            job.injection.stage = PlaybackInjectionStage::BeforeMasterFader;
        } else if (printSends || job.sourceTrackId.empty()) {
            job.captureChannelId = kMasterChannelId;
            job.captureAtSource = true;
            job.injection.stage = PlaybackInjectionStage::BeforeMasterFx;
        } else {
            std::string folder;
            if (printFolders) {
                const TrackModel* routed =
                    m_project.findTrack(job.sourceTrackId);
                std::unordered_set<std::string> visited;
                while (routed && !routed->outputBusId.empty() &&
                       visited.insert(routed->id).second) {
                    routed = m_project.findTrack(routed->outputBusId);
                    if (routed && routed->summing) folder = routed->id;
                }
            }
            if (!folder.empty()) {
                job.captureChannelId = folder;
                job.capturePreFader = true;
                job.injection = {PlaybackInjectionStage::BeforeFolderFader,
                                 folder};
            } else if (printTrack) {
                job.captureChannelId = job.sourceTrackId;
                job.capturePreFader = true;
                job.injection = {PlaybackInjectionStage::BeforeTrackFader,
                                 job.sourceTrackId};
            } else {
                job.captureChannelId = job.sourceTrackId;
                job.captureAtSource = true;
                job.injection = {PlaybackInjectionStage::TrackSource,
                                 job.sourceTrackId};
            }
        }
    }

    std::vector<std::string> stagedFiles;
    for (std::size_t index = 0; index < jobs.size(); ++index) {
        BounceJob& job = jobs[index];
        rendering::Spec spec;
        spec.outputDir = m_recordDir;
        spec.baseName = "bounce-" + newUuid();
        spec.file.container = audio::platform::Container::Wav;
        spec.file.encoding = audio::platform::Encoding::Float32;
        spec.range = rendering::Range::Custom;
        spec.customStartSeconds = request.startSeconds;
        spec.customEndSeconds = request.endSeconds;
        spec.preRollSeconds = request.startSeconds;
        spec.tail = request.tail;
        spec.tailSilenceDb = request.tailSilenceDb;
        spec.tailHoldSeconds = request.tailHoldSeconds;
        spec.tailMaxSeconds = request.tailMaxSeconds;
        spec.writeMixdown = false;
        spec.stemChannelIds = {job.captureChannelId};
        spec.stemsAtSource = job.captureAtSource;
        spec.stemsPreFader = job.capturePreFader;
        spec.sourceTrackIds = job.sourceTrackIds;
        spec.sourceClipIds = job.sourceClipIds;
        spec.bypassClipInserts = !layerEnabled(
            request.fxLayers, BounceFxLayer::Clip);
        spec.bypassTrackInserts = !printTrack;
        spec.bypassSummingInserts = !printFolders;
        spec.bypassSends = !printSends;
        spec.bypassMasterChain = !printMaster;

        rendering::Report rendered;
        const auto progress = [&](const rendering::Progress& one) {
            if (!onProgress) return true;
            rendering::Progress total = one;
            total.fraction =
                (double(index) + one.fraction) / double(jobs.size());
            total.renderedSeconds =
                double(index) * one.totalSeconds + one.renderedSeconds;
            total.totalSeconds = double(jobs.size()) * one.totalSeconds;
            return onProgress(total);
        };
        audio::Result result = renderProject(spec, progress, rendered);
        if (!result || rendered.cancelled || rendered.files.size() != 1) {
            removeFiles(stagedFiles);
            out.cancelled = rendered.cancelled;
            if (rendered.cancelled) return audio::Result::ok();
            if (!result) return result;
            return audio::Result::fail(audio::EngineError::FileWriteError,
                                       "bounce produced no audio file");
        }
        job.file = rendered.files.front();
        job.renderedSeconds = rendered.renderedSeconds;
        stagedFiles.push_back(job.file);
    }

    const ProjectModel before = m_project;
    bool committed = true;
    {
        UndoStack::Suspend quiet(m_undo);

        auto affectRange = [&](const ClipAddress& original, bool remove) {
            ClipModel* clip = nullptr;
            if (TrackModel* track = m_project.findTrack(original.trackId)) {
                const auto found = std::find_if(
                    track->clips.begin(), track->clips.end(),
                    [&](const ClipModel& candidate) {
                        return candidate.id == original.clipId;
                    });
                if (found != track->clips.end()) clip = &*found;
            }
            if (!clip) return;
            std::string targetId = clip->id;
            const double clipEnd =
                clip->startSeconds + clipPlaybackDuration(*clip);
            if (clip->startSeconds < request.startSeconds &&
                clipEnd > request.startSeconds) {
                const std::string right = splitClip(
                    original.trackId, targetId, request.startSeconds);
                if (!right.empty()) targetId = right;
            }
            clip = nullptr;
            if (TrackModel* track = m_project.findTrack(original.trackId)) {
                const auto found = std::find_if(
                    track->clips.begin(), track->clips.end(),
                    [&](const ClipModel& candidate) {
                        return candidate.id == targetId;
                    });
                if (found != track->clips.end()) clip = &*found;
            }
            if (!clip) return;
            if (clip->startSeconds + clipPlaybackDuration(*clip) >
                request.endSeconds) {
                (void)splitClip(original.trackId, targetId,
                                request.endSeconds);
            }
            if (remove)
                removeClip(original.trackId, targetId);
            else
                setClipMuted(original.trackId, targetId, true);
        };

        for (BounceJob& job : jobs) {
            TrackModel* source = m_project.findTrack(job.sourceTrackId);
            const std::string sourceId = source ? source->id : std::string();
            const std::string sourceName = source ? source->name : std::string();
            const std::string sourceParent = source ? source->parentId
                                                     : std::string();
            const std::size_t sourceIndex = source
                ? m_project.indexOf(sourceId)
                : m_project.tracks.size();
            const bool replaceOnSource =
                request.destination == BounceDestination::Replace && source &&
                source->kind == TrackKind::Audio;
            std::string destination = replaceOnSource ? job.sourceTrackId
                                                      : std::string();
            if (destination.empty()) {
                destination = addTrack(
                    TrackKind::Audio,
                    sourceName.empty() ? "Bounce" : sourceName + " Bounce");
                if (destination.empty()) {
                    committed = false;
                    break;
                }
                if (!sourceId.empty()) {
                    (void)moveTrack(destination, sourceIndex + 1,
                                    sourceParent);
                }
            }
            for (const ClipAddress& address : job.affected)
                affectRange(address, replaceOnSource);

            const std::string clipId = importAudio(
                job.file, destination, request.startSeconds);
            ClipModel* bounced = clipId.empty()
                                     ? nullptr
                                     : findClip(destination, clipId);
            if (!bounced) {
                committed = false;
                break;
            }
            bounced->name = sourceName.empty() ? "Bounce"
                                                : sourceName + " Bounce";
            bounced->gain = 1.0f;
            bounced->pan = 0.0f;
            bounced->fadeInSeconds = 0.0;
            bounced->fadeOutSeconds = 0.0;
            job.injection.anchorChannelId =
                job.injection.anchorChannelId.empty() ? destination
                                                       : job.injection.anchorChannelId;
            bounced->playbackInjection = job.injection;
            out.outputs.push_back(
                {job.sourceTrackId, destination, clipId, job.file});
        }

        if (!committed) {
            m_project = before;
            out.outputs.clear();
        }
        m_deferredClipSync.clear();
        (void)rebuildGraph();
        updateTimelineDuration();
    }
    if (!committed) {
        removeFiles(stagedFiles);
        return audio::Result::fail(audio::EngineError::FileWriteError,
                                   "could not insert bounced audio");
    }
    pushProjectSnapshotUndo(before, "Bounce in Place");
    return audio::Result::ok();
}

audio::Result EngineController::renderClipsOffline(
    const std::vector<ClipAddress>& addresses, const ChannelSnapshot& chain,
    bool includeTail,
    const std::function<bool(const rendering::Progress&)>& onProgress,
    OfflineRenderReport& out) {
    out = {};
    if (cloudProjectBound()) {
        return audio::Result::fail(audio::EngineError::InvalidArgument,
                                   "Offline Render is local-only");
    }
    if (addresses.empty()) {
        return audio::Result::fail(audio::EngineError::InvalidArgument,
                                   "no audio clips selected");
    }
    for (const ClipAddress& address : addresses) {
        const TrackModel* track = m_project.findTrack(address.trackId);
        if (!track) {
            return audio::Result::fail(audio::EngineError::InvalidArgument,
                                       "Offline Render requires audio clips");
        }
        const auto clip = std::find_if(
            track->clips.begin(), track->clips.end(),
            [&](const ClipModel& item) { return item.id == address.clipId; });
        if (clip == track->clips.end() || clip->kind != ClipKind::Audio) {
            return audio::Result::fail(audio::EngineError::InvalidArgument,
                                       "Offline Render requires audio clips");
        }
    }
    for (const ChainSlotSnapshot& slot : chain.inserts) {
        if (slot.model.bypassed) continue;
        if (!m_pluginManager.find(toHostFormat(slot.model.format),
                                  slot.model.uid)) {
            return audio::Result::fail(
                audio::EngineError::FileNotFound,
                "plugin is not available: " + slot.model.name);
        }
    }

    const bool hasEnabled = std::any_of(
        chain.inserts.begin(), chain.inserts.end(),
        [](const ChainSlotSnapshot& slot) { return !slot.model.bypassed; });
    std::vector<std::string> staged;
    std::vector<double> durations;
    if (hasEnabled) {
        EngineController scratch;
        audio::Result ready =
            scratch.initialize(m_sampleRate, m_bufferSize, false);
        if (!ready) return ready;

        for (std::size_t index = 0; index < addresses.size(); ++index) {
            const ClipAddress& address = addresses[index];
            const TrackModel* sourceTrack = m_project.findTrack(address.trackId);
            const auto source = std::find_if(
                sourceTrack->clips.begin(), sourceTrack->clips.end(),
                [&](const ClipModel& clip) { return clip.id == address.clipId; });

            ProjectModel project;
            project.sampleRate = m_sampleRate;
            TrackModel track;
            track.id = newUuid();
            track.kind = TrackKind::Audio;
            track.name = source->name;
            ClipModel clip = *source;
            clip.id = newUuid();
            clip.startSeconds = 0.0;
            clip.muted = false;
            clip.inserts.clear();
            clip.offlineProcess = {};
            clip.playbackInjection = {};
            track.clips.push_back(std::move(clip));
            project.tracks.push_back(std::move(track));
            scratch.restoreProject(project, "Offline source");
            if (!scratch.pasteChannelInserts(project.tracks.front().id, chain)) {
                scratch.shutdown();
                removeFiles(staged);
                return audio::Result::fail(audio::EngineError::InvalidArgument,
                                           "could not prepare offline chain");
            }

            rendering::Spec spec;
            spec.outputDir = m_recordDir;
            spec.baseName = "offline-" + newUuid();
            spec.file.container = audio::platform::Container::Wav;
            spec.file.encoding = audio::platform::Encoding::Float32;
            spec.range = rendering::Range::Custom;
            spec.customEndSeconds = source->durationSeconds;
            spec.tail = includeTail ? rendering::Tail::UntilSilence
                                    : rendering::Tail::None;
            spec.tailSilenceDb = -96.0;
            spec.tailHoldSeconds = 0.3;
            spec.tailMaxSeconds = 30.0;
            rendering::Report rendered;
            const auto progress = [&](const rendering::Progress& one) {
                if (!onProgress) return true;
                rendering::Progress total = one;
                total.fraction =
                    (double(index) + one.fraction) / double(addresses.size());
                total.renderedSeconds =
                    double(index) * one.totalSeconds + one.renderedSeconds;
                total.totalSeconds =
                    double(addresses.size()) * one.totalSeconds;
                return onProgress(total);
            };
            audio::Result result = scratch.renderProject(spec, progress, rendered);
            if (!result || rendered.cancelled || rendered.files.size() != 1) {
                scratch.shutdown();
                removeFiles(staged);
                out.cancelled = rendered.cancelled;
                if (rendered.cancelled) return audio::Result::ok();
                if (!result) return result;
                return audio::Result::fail(
                    audio::EngineError::FileWriteError,
                    "offline processing produced no audio file");
            }
            staged.push_back(rendered.files.front());
            durations.push_back(rendered.renderedSeconds);
        }
        scratch.shutdown();
    }

    const ProjectModel before = m_project;
    for (std::size_t index = 0; index < addresses.size(); ++index) {
        ClipModel* clip = findClip(addresses[index].trackId,
                                   addresses[index].clipId);
        if (!clip) continue;
        const std::string fingerprint = offlineSourceFingerprint(*clip);
        OfflineProcessModel process;
        process.chain = cacheOfflineChain(chain.inserts);
        process.sourceFingerprint = fingerprint;
        process.sourceDurationSeconds = clip->durationSeconds;
        process.includeTail = includeTail;
        if (hasEnabled) {
            process.renderedFilePath = staged[index];
            process.renderedDurationSeconds = durations[index];
            out.files.push_back(staged[index]);
        }
        clip->offlineProcess = std::move(process);
    }
    (void)rebuildGraph();
    updateTimelineDuration();
    pushProjectSnapshotUndo(before, "Render Offline");
    return audio::Result::ok();
}

} // namespace daw
