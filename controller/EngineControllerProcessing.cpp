#include "EngineController.hpp"

#include "plugins/PluginConvert.hpp"
#include "platform/AudioFileDecoder.hpp"
#include "platform/PathUtils.hpp"

#include <algorithm>
#include <cmath>
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

ClipAudioVersionSource renderedSource(const std::string& path, double seconds) {
    ClipAudioVersionSource source;
    source.filePath = path;
    source.durationSeconds = seconds;
    source.channels = 2;
    return source;
}

std::string effectNames(const std::vector<InsertModel>& chain) {
    std::string label;
    for (const auto& slot : chain) {
        if (slot.bypassed) continue;
        if (!label.empty()) label += " · ";
        label += slot.name;
    }
    return label;
}

// Upgrade the old single-cache representation on the first history operation.
// Merely opening an old project remains read-only and plays exactly as before.
void ensureOfflineHistory(ClipModel& clip, bool legacyValid) {
    if (!clip.offlineHistory.empty()) return;
    clip.offlineHistory.push_back({newUuid(), {}, {}, captureClipAudioVersion(clip)});
    clip.offlineVersionId = clip.offlineHistory.front().id;
    if (legacyValid) {
        auto source = renderedSource(clip.offlineProcess.renderedFilePath,
                                      clip.offlineProcess.renderedDurationSeconds);
        clip.offlineHistory.push_back({newUuid(), clip.offlineVersionId,
                                       effectNames(clip.offlineProcess.chain), source});
        clip.offlineVersionId = clip.offlineHistory.back().id;
        applyClipAudioVersion(clip, source);
    }
    clip.offlineProcess = {};
}

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

    // With no active instruments/effects there is no DSP history to warm.
    const auto hasPlugin = [](const auto& slots) {
        return std::any_of(slots.begin(), slots.end(), [](const InsertModel& slot) {
            return slot.isLoaded() && !slot.bypassed;
        });
    };
    bool needsWarmup = hasPlugin(m_project.masterInserts);
    for (const auto& track : m_project.tracks) {
        needsWarmup |= track.instrument.isLoaded() || hasPlugin(track.inserts) || hasPlugin(track.samplerFx.inserts);
        for (const auto& clip : track.clips) needsWarmup |= hasPlugin(clip.inserts);
    }
    const auto sourceRevision = projectRevision();
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
        spec.preRollSeconds = request.preRollSeconds < 0.0
            ? request.startSeconds : std::min(request.preRollSeconds, request.startSeconds);
        if (!needsWarmup) spec.preRollSeconds = 0.0;
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
            const bool proceed = onProgress(total);
            return proceed && projectRevision() == sourceRevision;
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
            const ChannelSnapshot sourceStrip =
                source ? copyChannelStrip(sourceId, true) : ChannelSnapshot{};
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
                if (TrackModel* target = m_project.findTrack(destination);
                    target && source) {
                    // TrackSource and BeforeTrackFader leave the source
                    // channel's fader live. Move those settings to the channel
                    // that now owns playback; later capture points already
                    // contain them in the rendered file.
                    if (job.injection.stage ==
                            PlaybackInjectionStage::TrackSource ||
                        job.injection.stage ==
                            PlaybackInjectionStage::BeforeTrackFader) {
                        target->volume = sourceStrip.volume;
                        target->pan = sourceStrip.pan;
                        target->mono = sourceStrip.mono;
                    }
                    target->outputBusId = sourceStrip.outputBusId;
                    if (!printSends) {
                        target->sends = sourceStrip.sends;
                        for (SendModel& send : target->sends)
                            send.id = newUuid();
                    }
                    if (!printTrack && !sourceStrip.inserts.empty() &&
                        !pasteChannelInserts(destination, sourceStrip)) {
                        committed = false;
                        break;
                    }
                }
            }
            if (!committed) break;
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
            // A clip created on a new track belongs to that track's channel
            // strip. Keeping the source injection here made the arrangement
            // and the graph disagree: mute/fader/pan on the visible bounce
            // track did nothing while the old track still controlled it.
            // Injection is only needed when replacing material on its existing
            // channel, where it prevents an already printed stage running
            // twice.
            if (replaceOnSource) {
                job.injection.anchorChannelId =
                    job.injection.anchorChannelId.empty()
                        ? destination
                        : job.injection.anchorChannelId;
                bounced->playbackInjection = job.injection;
            } else {
                bounced->playbackInjection = {};
            }
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
    if (cloudProjectBound() || m_exportInProgress) {
        return audio::Result::fail(audio::EngineError::InvalidArgument,
                                   "Offline Render is unavailable in this project state");
    }
    if (addresses.empty())
        return audio::Result::fail(audio::EngineError::InvalidArgument, "no audio clips selected");

    ChannelSnapshot enabledChain = chain;
    std::erase_if(enabledChain.inserts, [](const auto& slot) { return slot.model.bypassed; });
    if (enabledChain.inserts.empty())
        return audio::Result::fail(audio::EngineError::InvalidArgument, "add or enable an offline effect");
    for (const auto& slot : enabledChain.inserts) {
        if (!m_pluginManager.find(toHostFormat(slot.model.format), slot.model.uid))
            return audio::Result::fail(audio::EngineError::FileNotFound,
                                       "plugin is not available: " + slot.model.name);
    }

    struct StagedFiles {
        std::vector<std::string> files;
        bool committed = false;
        ~StagedFiles() { if (!committed) removeFiles(files); }
    } staged;
    struct BusyScope {
        bool& flag;
        explicit BusyScope(bool& value) : flag(value) { flag = true; }
        ~BusyScope() { flag = false; }
    } busy(m_exportInProgress);

    ProjectModel before;
    bool committing = false;
    try {
        const auto sourceRevision = projectRevision();
        before = m_project;
        std::vector<ClipAddress> clips;
        std::vector<ClipModel> sources;
        std::unordered_set<std::string> seen;
        for (const auto& address : addresses) {
            const auto* source = audioClip(address.trackId, address.clipId);
            if (!source)
                return audio::Result::fail(audio::EngineError::InvalidArgument,
                                           "Offline Render requires audio clips");
            if (!seen.insert(address.trackId + "/" + address.clipId).second) continue;
            ClipModel copy = *source;
            ensureOfflineHistory(copy, offlineProcessCacheValid(address));
            const double duration = effectiveClipLength(copy);
            if (!std::isfinite(duration) || duration <= 0.0)
                return audio::Result::fail(audio::EngineError::InvalidArgument, "audio clip has no duration");
            copy.durationSeconds = duration;
            const auto readable = [this](const std::string& path) {
                std::error_code error;
                if (!fs::is_regular_file(platform::pathFromUtf8(path), error) || error) return false;
                const auto samples = loadSamples(path);
                return samples && samples->frames() > 0;
            };
            if (copy.takes.empty()) {
                if (!readable(copy.filePath))
                    return audio::Result::fail(audio::EngineError::FileNotFound,
                                               "clip audio is unavailable: " + copy.filePath);
            } else {
                for (const auto& segment : copy.comp) {
                    const auto take = std::find_if(copy.takes.begin(), copy.takes.end(),
                        [&](const auto& item) { return item.id == segment.takeId; });
                    if (take == copy.takes.end() || !readable(take->filePath))
                        return audio::Result::fail(audio::EngineError::FileNotFound,
                                                   "a comp source is unavailable");
                }
            }
            clips.push_back(address);
            sources.push_back(std::move(copy));
        }
        out.clipCount = clips.size();

        EngineController scratch;
        if (const auto ready = scratch.initialize(m_sampleRate, m_bufferSize, false); !ready)
            return ready;
        std::vector<double> durations;
        for (std::size_t index = 0; index < clips.size(); ++index) {
            out.clipIndex = index;
            ProjectModel project;
            project.sampleRate = m_sampleRate;
            project.tempo = before.tempo;
            project.timeSigNumerator = before.timeSigNumerator;
            project.timeSigDenominator = before.timeSigDenominator;
            project.keyRoot = before.keyRoot;
            project.scale = before.scale;
            TrackModel track;
            track.id = newUuid();
            track.kind = TrackKind::Audio;
            track.name = sources[index].name;
            ClipModel clip = sources[index];
            clip.id = newUuid();
            clip.muted = false;
            clip.inserts.clear();
            clip.offlineProcess = {};
            clip.offlineHistory.clear();
            clip.offlineVersionId.clear();
            clip.playbackInjection = {};
            track.clips.push_back(std::move(clip));
            project.tracks.push_back(std::move(track));
            scratch.restoreProject(project, "Offline source");
            if (!scratch.pasteChannelInserts(project.tracks.front().id, enabledChain))
                return audio::Result::fail(audio::EngineError::InvalidArgument,
                                           "could not prepare offline chain");
            for (const auto& slot : *scratch.channelInserts(project.tracks.front().id)) {
                if (!scratch.insertInstance(project.tracks.front().id, slot.id))
                    return audio::Result::fail(audio::EngineError::InvalidArgument,
                                               "could not load offline plugin: " + slot.name);
            }

            rendering::Spec spec;
            spec.outputDir = m_recordDir;
            spec.baseName = "offline-" + newUuid();
            spec.file.container = audio::platform::Container::Wav;
            spec.file.encoding = audio::platform::Encoding::Float32;
            spec.range = rendering::Range::Custom;
            // Keep musical position and tempo for synced/time-aware plugins.
            spec.customStartSeconds = sources[index].startSeconds;
            spec.customEndSeconds = spec.customStartSeconds + sources[index].durationSeconds;
            spec.tail = includeTail ? rendering::Tail::UntilSilence : rendering::Tail::None;
            spec.tailSilenceDb = -96.0;
            spec.tailHoldSeconds = 0.3;
            spec.tailMaxSeconds = 30.0;
            rendering::Report rendered;
            const auto progress = [&](const rendering::Progress& one) {
                rendering::Progress total = one;
                total.fraction = (double(index) + one.fraction) / double(clips.size());
                const bool proceed = !onProgress || onProgress(total);
                return proceed && projectRevision() == sourceRevision;
            };
            const auto result = scratch.renderProject(spec, progress, rendered);
            staged.files.insert(staged.files.end(), rendered.files.begin(), rendered.files.end());
            if (!result) return result;
            if (rendered.cancelled || projectRevision() != sourceRevision) {
                out.cancelled = true;
                return audio::Result::ok();
            }
            if (rendered.files.size() != 1 || !(rendered.renderedSeconds > 0.0))
                return audio::Result::fail(audio::EngineError::FileWriteError,
                                           "offline processing produced no audio file");
            durations.push_back(rendered.renderedSeconds);
        }
        scratch.shutdown();
        // No project mutation is allowed between validation and this commit.
        if (projectRevision() != sourceRevision) {
            out.cancelled = true;
            return audio::Result::ok();
        }
        std::vector<InsertModel> models;
        for (const auto& slot : enabledChain.inserts) models.push_back(slot.model);
        const auto label = effectNames(models);
        committing = true;
        for (std::size_t index = 0; index < clips.size(); ++index) {
            auto* clip = findClip(clips[index].trackId, clips[index].clipId);
            // Use the snapshotted history, including any legacy migration.
            clip->offlineHistory = sources[index].offlineHistory;
            const auto source = renderedSource(staged.files[index], durations[index]);
            clip->offlineHistory.push_back({newUuid(), sources[index].offlineVersionId, label, source});
            clip->offlineVersionId = clip->offlineHistory.back().id;
            applyClipAudioVersion(*clip, source);
        }
        m_exportInProgress = false;
        if (const auto rebuilt = rebuildGraph(); !rebuilt) {
            m_project = before;
            (void)rebuildGraph();
            updateTimelineDuration();
            committing = false;
            return rebuilt;
        }
        updateTimelineDuration();
        pushProjectSnapshotUndo(before, "Render Offline");
        out.files = staged.files;
        staged.committed = true;
        committing = false;
        return audio::Result::ok();
    } catch (const std::exception& error) {
        if (committing) {
            m_project = std::move(before);
            m_exportInProgress = false;
            (void)rebuildGraph();
            updateTimelineDuration();
        }
        return audio::Result::fail(audio::EngineError::Unknown, error.what());
    } catch (...) {
        if (committing) {
            m_project = std::move(before);
            m_exportInProgress = false;
            (void)rebuildGraph();
            updateTimelineDuration();
        }
        return audio::Result::fail(audio::EngineError::Unknown, "offline processing failed");
    }
}

audio::Result EngineController::selectOfflineRenderVersion(
    const ClipAddress& address, const std::string& versionId) {
    if (cloudProjectBound() || m_exportInProgress)
        return audio::Result::fail(audio::EngineError::InvalidArgument, "offline history is unavailable");
    auto* clip = findClip(address.trackId, address.clipId);
    if (!clip || clip->kind != ClipKind::Audio)
        return audio::Result::fail(audio::EngineError::InvalidArgument, "audio clip not found");
    const auto found = std::find_if(clip->offlineHistory.begin(), clip->offlineHistory.end(),
        [&](const auto& version) { return version.id == versionId; });
    if (found == clip->offlineHistory.end())
        return audio::Result::fail(audio::EngineError::InvalidArgument, "offline version not found");
    // Missing history media must never replace a playable version with silence.
    const auto available = [this](const std::string& path) {
        std::error_code error;
        if (!fs::is_regular_file(platform::pathFromUtf8(path), error) || error) return false;
        const auto samples = loadSamples(path);
        return samples && samples->frames() > 0;
    };
    if (found->source.takes.empty()) {
        if (!available(found->source.filePath))
            return audio::Result::fail(audio::EngineError::FileNotFound, "version audio is unavailable");
    } else {
        for (const auto& segment : found->source.comp) {
            const auto take = std::find_if(found->source.takes.begin(), found->source.takes.end(),
                [&](const auto& item) { return item.id == segment.takeId; });
            if (take == found->source.takes.end() || !available(take->filePath))
                return audio::Result::fail(audio::EngineError::FileNotFound, "version comp audio is unavailable");
        }
    }
    const ProjectModel before = m_project;
    const auto source = found->source;
    applyClipAudioVersion(*clip, source);
    clip->offlineVersionId = versionId;
    if (const auto rebuilt = rebuildGraph(); !rebuilt) {
        m_project = before;
        (void)rebuildGraph();
        return rebuilt;
    }
    updateTimelineDuration();
    pushProjectSnapshotUndo(before, "Change Offline Render Version");
    return audio::Result::ok();
}

audio::Result EngineController::restoreOfflineRenderOriginal(const ClipAddress& address) {
    if (cloudProjectBound() || m_exportInProgress)
        return audio::Result::fail(audio::EngineError::InvalidArgument, "offline history is unavailable");
    auto* clip = findClip(address.trackId, address.clipId);
    if (!clip || clip->kind != ClipKind::Audio)
        return audio::Result::fail(audio::EngineError::InvalidArgument, "audio clip not found");
    if (!clip->offlineHistory.empty())
        return selectOfflineRenderVersion(address, clip->offlineHistory.front().id);
    if (clip->offlineProcess.empty()) return audio::Result::ok();
    const ProjectModel before = m_project;
    ensureOfflineHistory(*clip, offlineProcessCacheValid(address));
    const auto result = selectOfflineRenderVersion(address, clip->offlineHistory.front().id);
    if (!result) { m_project = before; return result; }
    // selectOfflineRenderVersion's undo retains the migrated history, which
    // preserves both the original and the previous rendered audio.
    return result;
}

} // namespace daw
