#include "collaboration/ProjectReducer.hpp"
#include "collaboration/CommandJson.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <type_traits>

namespace daw::collab {
namespace {

constexpr double kMaximumSendLevel = 1.9953; // +6 dB, matches EngineController

ApplyResult reject(ApplyCode code, std::string message) {
    ApplyResult result;
    result.code = code;
    result.message = std::move(message);
    return result;
}

ProjectCommand inverseShell(const ProjectCommand& source, CommandBody body) {
    ProjectCommand inverse;
    inverse.meta.schemaVersion = kProjectCommandSchemaVersion;
    inverse.meta.projectId = source.meta.projectId;
    inverse.meta.transactionId = source.meta.operationId;
    inverse.body = std::move(body);
    return inverse;
}

bool doubleValue(const ScalarValue& value, double& out) {
    if (const auto* number = std::get_if<double>(&value)) {
        if (!std::isfinite(*number)) return false;
        out = *number;
        return true;
    }
    if (const auto* integer = std::get_if<std::int64_t>(&value)) {
        out = double(*integer);
        return true;
    }
    return false;
}

bool integerValue(const ScalarValue& value, std::int64_t& out) {
    if (const auto* integer = std::get_if<std::int64_t>(&value)) {
        out = *integer;
        return true;
    }
    return false;
}

std::string predecessorOf(const ProjectModel& project, const std::string& trackId) {
    const std::size_t at = project.indexOf(trackId);
    if (at == std::string::npos || at == 0) return {};
    return project.tracks[at - 1].id;
}

bool validAnchor(const ProjectModel& project, const std::string& afterId,
                 const std::string& movingId = {}) {
    if (afterId.empty()) return true;
    return afterId != movingId && project.findTrack(afterId) != nullptr;
}

bool insertAfter(ProjectModel& project, TrackModel track,
                 const std::string& afterId, bool missingAnchorFallsBack) {
    if (afterId.empty()) {
        project.tracks.insert(project.tracks.begin(), std::move(track));
        return true;
    }
    const std::size_t anchor = project.indexOf(afterId);
    if (anchor == std::string::npos) {
        if (!missingAnchorFallsBack) return false;
        project.tracks.push_back(std::move(track));
        return true;
    }
    project.tracks.insert(project.tracks.begin() + std::ptrdiff_t(anchor + 1),
                          std::move(track));
    return true;
}

template <typename Entity>
std::size_t entityIndexOf(const std::vector<Entity>& entities,
                          const std::string& id) {
    const auto found = std::find_if(
        entities.begin(), entities.end(), [&](const Entity& entity) {
            return entity.id == id;
        });
    return found == entities.end()
               ? std::string::npos
               : std::size_t(std::distance(entities.begin(), found));
}

template <typename Entity>
std::string entityPredecessor(const std::vector<Entity>& entities,
                              const std::string& id) {
    const std::size_t at = entityIndexOf(entities, id);
    if (at == std::string::npos || at == 0) return {};
    return entities[at - 1].id;
}

template <typename Entity>
bool validEntityAnchor(const std::vector<Entity>& entities,
                       const std::string& afterId,
                       const std::string& movingId = {}) {
    if (afterId.empty()) return true;
    return afterId != movingId &&
           entityIndexOf(entities, afterId) != std::string::npos;
}

template <typename Entity>
bool insertEntityAfter(std::vector<Entity>& entities, Entity entity,
                       const std::string& afterId,
                       bool missingAnchorFallsBack) {
    if (afterId.empty()) {
        entities.insert(entities.begin(), std::move(entity));
        return true;
    }
    const std::size_t anchor = entityIndexOf(entities, afterId);
    if (anchor == std::string::npos) {
        if (!missingAnchorFallsBack) return false;
        entities.push_back(std::move(entity));
        return true;
    }
    entities.insert(entities.begin() + std::ptrdiff_t(anchor + 1),
                    std::move(entity));
    return true;
}

struct ClipLocation {
    TrackModel* track = nullptr;
    ClipModel* clip = nullptr;
    std::size_t clipIndex = std::string::npos;
};

ClipLocation findClip(ProjectModel& project, const std::string& clipId) {
    for (TrackModel& track : project.tracks) {
        const std::size_t index = entityIndexOf(track.clips, clipId);
        if (index != std::string::npos)
            return ClipLocation{&track, &track.clips[index], index};
    }
    return {};
}

bool deletedTrackContainsClip(const SharedProjectDocument& state,
                              const std::string& clipId) {
    return std::any_of(
        state.deletedTracks.begin(), state.deletedTracks.end(),
        [&](const auto& item) {
            return entityIndexOf(item.second.track.clips, clipId) !=
                   std::string::npos;
        });
}

bool clipIsDeleted(const SharedProjectDocument& state,
                   const std::string& clipId) {
    return state.deletedClips.contains(clipId) ||
           deletedTrackContainsClip(state, clipId);
}

bool clipScopeIsDeleted(const SharedProjectDocument& state,
                        const std::string& trackId,
                        const std::string& clipId) {
    return state.deletedTracks.contains(trackId) || clipIsDeleted(state, clipId);
}

ControllerLane* findControllerLane(ClipModel& clip,
                                   const std::string& laneId) {
    const auto found = std::find_if(
        clip.lanes.begin(), clip.lanes.end(), [&](const ControllerLane& lane) {
            return lane.id == laneId;
        });
    return found == clip.lanes.end() ? nullptr : &*found;
}

bool liveControllerLaneExistsOutside(const ProjectModel& project,
                                     const std::string& laneId,
                                     const std::string& allowedClipId) {
    for (const TrackModel& track : project.tracks) {
        for (const ClipModel& clip : track.clips) {
            if (clip.id != allowedClipId &&
                entityIndexOf(clip.lanes, laneId) != std::string::npos) {
                return true;
            }
        }
    }
    return false;
}

bool controllerLaneIsDeleted(const SharedProjectDocument& state,
                             const std::string& laneId) {
    if (state.deletedControllerLanes.contains(laneId)) return true;
    for (const auto& [ignored, tombstone] : state.deletedClips) {
        if (entityIndexOf(tombstone.clip.lanes, laneId) != std::string::npos)
            return true;
    }
    for (const auto& [ignored, tombstone] : state.deletedTracks) {
        for (const ClipModel& clip : tombstone.track.clips) {
            if (entityIndexOf(clip.lanes, laneId) != std::string::npos)
                return true;
        }
    }
    return false;
}

bool liveTakeExistsOutside(const ProjectModel& project,
                           const std::string& takeId,
                           const std::string& allowedClipId) {
    for (const TrackModel& track : project.tracks) {
        for (const ClipModel& clip : track.clips) {
            if (clip.id != allowedClipId &&
                entityIndexOf(clip.takes, takeId) != std::string::npos) {
                return true;
            }
        }
    }
    return false;
}

bool takeIsDeleted(const SharedProjectDocument& state,
                   const std::string& takeId) {
    if (state.deletedTakes.contains(takeId)) return true;
    for (const auto& [ignored, tombstone] : state.deletedClips) {
        if (entityIndexOf(tombstone.clip.takes, takeId) != std::string::npos)
            return true;
    }
    for (const auto& [ignored, tombstone] : state.deletedTracks) {
        for (const ClipModel& clip : tombstone.track.clips) {
            if (entityIndexOf(clip.takes, takeId) != std::string::npos)
                return true;
        }
    }
    return false;
}

bool liveCompSegmentExistsOutside(const ProjectModel& project,
                                  const std::string& segmentId,
                                  const std::string& allowedClipId) {
    for (const TrackModel& track : project.tracks) {
        for (const ClipModel& clip : track.clips) {
            if (clip.id != allowedClipId &&
                entityIndexOf(clip.comp, segmentId) != std::string::npos) {
                return true;
            }
        }
    }
    return false;
}

bool compSegmentIsDeleted(const SharedProjectDocument& state,
                          const std::string& segmentId) {
    if (state.deletedCompSegments.contains(segmentId)) return true;
    for (const auto& [ignored, tombstone] : state.deletedClips) {
        if (entityIndexOf(tombstone.clip.comp, segmentId) != std::string::npos)
            return true;
    }
    for (const auto& [ignored, tombstone] : state.deletedTracks) {
        for (const ClipModel& clip : tombstone.track.clips) {
            if (entityIndexOf(clip.comp, segmentId) != std::string::npos)
                return true;
        }
    }
    return false;
}

std::vector<AutomationPoint>* automationPointsFor(ClipModel& clip,
                                                   const std::string& laneId) {
    if (laneId.empty())
        return clip.kind == ClipKind::Automation ? &clip.automation.points
                                                 : nullptr;
    const auto lane = std::find_if(
        clip.lanes.begin(), clip.lanes.end(), [&](const ControllerLane& value) {
            return value.id == laneId;
        });
    return lane == clip.lanes.end() ? nullptr : &lane->points;
}

const std::vector<AutomationPoint>* automationPointsFor(
    const ClipModel& clip, const std::string& laneId) {
    if (laneId.empty())
        return clip.kind == ClipKind::Automation ? &clip.automation.points
                                                 : nullptr;
    const auto lane = std::find_if(
        clip.lanes.begin(), clip.lanes.end(), [&](const ControllerLane& value) {
            return value.id == laneId;
        });
    return lane == clip.lanes.end() ? nullptr : &lane->points;
}

bool liveNoteExistsOutside(const ProjectModel& project,
                           const std::string& noteId,
                           const std::string& allowedClipId) {
    for (const TrackModel& track : project.tracks) {
        for (const ClipModel& clip : track.clips) {
            if (clip.id != allowedClipId &&
                entityIndexOf(clip.notes, noteId) != std::string::npos)
                return true;
        }
    }
    return false;
}

bool noteIsDeleted(const SharedProjectDocument& state,
                   const std::string& noteId) {
    if (state.deletedNotes.contains(noteId)) return true;
    for (const auto& [ignored, tombstone] : state.deletedClips) {
        if (entityIndexOf(tombstone.clip.notes, noteId) != std::string::npos)
            return true;
    }
    for (const auto& [ignored, tombstone] : state.deletedTracks) {
        for (const ClipModel& clip : tombstone.track.clips) {
            if (entityIndexOf(clip.notes, noteId) != std::string::npos)
                return true;
        }
    }
    return false;
}

bool liveAutomationPointExistsOutside(const ProjectModel& project,
                                      const std::string& pointId,
                                      const std::string& allowedClipId,
                                      const std::string& allowedLaneId) {
    for (const TrackModel& track : project.tracks) {
        for (const ClipModel& clip : track.clips) {
            if (const auto* points = automationPointsFor(clip, {});
                points && (clip.id != allowedClipId || !allowedLaneId.empty()) &&
                entityIndexOf(*points, pointId) != std::string::npos)
                return true;
            for (const ControllerLane& lane : clip.lanes) {
                if ((clip.id != allowedClipId || lane.id != allowedLaneId) &&
                    entityIndexOf(lane.points, pointId) != std::string::npos)
                    return true;
            }
        }
    }
    return false;
}

bool clipContainsAutomationPoint(const ClipModel& clip,
                                 const std::string& pointId) {
    if (entityIndexOf(clip.automation.points, pointId) != std::string::npos)
        return true;
    return std::any_of(
        clip.lanes.begin(), clip.lanes.end(), [&](const ControllerLane& lane) {
            return entityIndexOf(lane.points, pointId) != std::string::npos;
        });
}

bool automationPointIsDeleted(const SharedProjectDocument& state,
                              const std::string& pointId) {
    if (state.deletedAutomationPoints.contains(pointId)) return true;
    for (const auto& [ignored, tombstone] : state.deletedControllerLanes) {
        if (entityIndexOf(tombstone.lane.points, pointId) != std::string::npos)
            return true;
    }
    for (const auto& [ignored, tombstone] : state.deletedClips) {
        if (clipContainsAutomationPoint(tombstone.clip, pointId)) return true;
    }
    for (const auto& [ignored, tombstone] : state.deletedTracks) {
        for (const ClipModel& clip : tombstone.track.clips) {
            if (clipContainsAutomationPoint(clip, pointId)) return true;
        }
    }
    return false;
}

bool laneTargetIsValid(const ControllerLaneTarget& target) {
    if (target.cc < -1 || target.cc > 127 || target.parameterId.size() > 4096)
        return false;
    return target.cc != -1 || !target.parameterId.empty();
}

bool automationTargetIsValid(const SharedProjectDocument& state,
                             const AutomationTarget& target) {
    if (target.parameterId.size() > 4096) return false;
    const TrackModel* channel = state.project.findTrack(target.channelId);
    if (!channel) return false;
    switch (target.kind) {
        case AutomationTargetKind::TrackVolume:
        case AutomationTargetKind::TrackPan:
        case AutomationTargetKind::TrackMute:
            return target.slotId.empty() && target.parameterId.empty() &&
                   target.sendId.empty();
        case AutomationTargetKind::SendLevel:
            return target.slotId.empty() && target.parameterId.empty() &&
                   !target.sendId.empty();
        case AutomationTargetKind::PluginParameter:
            return !target.parameterId.empty() && target.sendId.empty();
    }
    return false;
}

bool lowercaseSha256(const std::string& value) {
    return value.size() == 64 && std::all_of(
        value.begin(), value.end(), [](unsigned char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
}

bool completeAsset(const AssetRef& asset, AssetKind expectedKind,
                   bool allowEmpty) {
    if (allowEmpty && asset.empty()) return true;
    return !asset.assetId.empty() && lowercaseSha256(asset.sha256) &&
           asset.kind == expectedKind && asset.byteSize > 0 &&
           asset.originalName.size() <= 4096 && asset.mimeType.size() <= 255 &&
           asset.codec.size() <= 255 && std::isfinite(asset.sampleRate) &&
           asset.sampleRate >= 0.0 && asset.sampleRate <= 768000.0 &&
           asset.channels <= 1024;
}

bool sampleEditEqual(const ClipSampleEditModel& a,
                     const ClipSampleEditModel& b) {
    return a.loopMode == b.loopMode && a.loopStart == b.loopStart &&
           a.loopEnd == b.loopEnd && a.stretchMode == b.stretchMode &&
           a.stretchTime == b.stretchTime &&
           a.stretchPitch == b.stretchPitch && a.formant == b.formant &&
           a.rootNote == b.rootNote && a.boost == b.boost &&
           a.eqLow == b.eqLow && a.eqMid == b.eqMid &&
           a.eqHigh == b.eqHigh && a.ringMix == b.ringMix &&
           a.ringFreq == b.ringFreq && a.cut == b.cut && a.res == b.res &&
           a.reverbType == b.reverbType && a.reverb == b.reverb &&
           a.stereoDelay == b.stereoDelay && a.pogo == b.pogo &&
           a.removeDc == b.removeDc &&
           a.reversePolarity == b.reversePolarity &&
           a.normalize == b.normalize && a.fadeStereo == b.fadeStereo &&
           a.reverse == b.reverse && a.swapStereo == b.swapStereo;
}

bool validSampleEdit(const ClipSampleEditModel& value) {
    const auto finiteRange = [](double number, double minimum,
                                double maximum) {
        return std::isfinite(number) && number >= minimum && number <= maximum;
    };
    const int stretchMode = static_cast<int>(value.stretchMode);
    return value.loopMode >= 0 && value.loopMode <= 2 &&
           finiteRange(value.loopStart, 0.0, 1.0) &&
           finiteRange(value.loopEnd, 0.0, 1.0) &&
           value.loopStart <= value.loopEnd && stretchMode >= 0 &&
           stretchMode <= static_cast<int>(ClipStretchMode::Complex) &&
           finiteRange(value.stretchTime, 0.01, 100.0) &&
           finiteRange(value.stretchPitch, -96.0, 96.0) &&
           finiteRange(value.formant, -96.0, 96.0) && value.rootNote >= 0 &&
           value.rootNote <= 127 && finiteRange(value.boost, -4.0, 4.0) &&
           finiteRange(value.eqLow, -4.0, 4.0) &&
           finiteRange(value.eqMid, -4.0, 4.0) &&
           finiteRange(value.eqHigh, -4.0, 4.0) &&
           finiteRange(value.ringMix, 0.0, 1.0) &&
           finiteRange(value.ringFreq, 0.0, 1.0) &&
           finiteRange(value.cut, 0.0, 1.0) &&
           finiteRange(value.res, 0.0, 1.0) && value.reverbType >= 0 &&
           value.reverbType <= 16 && finiteRange(value.reverb, 0.0, 1.0) &&
           finiteRange(value.stereoDelay, 0.0, 1.0) &&
           finiteRange(value.pogo, 0.0, 1.0);
}

bool musicalAnalysisEqual(const ClipMusicalAnalysisModel& a,
                          const ClipMusicalAnalysisModel& b) {
    return a.algorithmVersion == b.algorithmVersion &&
           a.analyzedOffsetSeconds == b.analyzedOffsetSeconds &&
           a.analyzedDurationSeconds == b.analyzedDurationSeconds &&
           a.tempo.status == b.tempo.status && a.tempo.bpm == b.tempo.bpm &&
           a.tempo.confidence == b.tempo.confidence &&
           a.tempo.stability == b.tempo.stability &&
           a.tempo.alternatives == b.tempo.alternatives &&
           a.tempo.variable == b.tempo.variable &&
           a.key.status == b.key.status && a.key.root == b.key.root &&
           a.key.scale == b.key.scale &&
           a.key.confidence == b.key.confidence &&
           a.key.alternateRoot == b.key.alternateRoot &&
           a.key.alternateScale == b.key.alternateScale &&
           a.key.tuningCents == b.key.tuningCents;
}

bool validMusicalAnalysis(const ClipMusicalAnalysisModel& value) {
    const auto finiteRange = [](double number, double minimum,
                                double maximum) {
        return std::isfinite(number) && number >= minimum && number <= maximum;
    };
    const int tempoStatus = static_cast<int>(value.tempo.status);
    const int keyStatus = static_cast<int>(value.key.status);
    if (value.algorithmVersion < 0 || value.algorithmVersion > 1000000 ||
        !finiteRange(value.analyzedOffsetSeconds, 0.0, 1.0e12) ||
        !finiteRange(value.analyzedDurationSeconds, 0.0, 1.0e12) ||
        tempoStatus < int(MusicalAnalysisStatus::Unavailable) ||
        tempoStatus > int(MusicalAnalysisStatus::Available) ||
        keyStatus < int(MusicalAnalysisStatus::Unavailable) ||
        keyStatus > int(MusicalAnalysisStatus::Available) ||
        !finiteRange(value.tempo.bpm, 0.0, 300.0) ||
        !finiteRange(value.tempo.confidence, 0.0, 1.0) ||
        !finiteRange(value.tempo.stability, 0.0, 1.0) ||
        value.tempo.alternatives.size() > 3 || value.key.root < -1 ||
        value.key.root > 11 || value.key.alternateRoot < -1 ||
        value.key.alternateRoot > 11 || value.key.scale.size() > 128 ||
        value.key.alternateScale.size() > 128 ||
        !finiteRange(value.key.confidence, 0.0, 1.0) ||
        !finiteRange(value.key.tuningCents, -200.0, 200.0)) {
        return false;
    }
    return std::all_of(value.tempo.alternatives.begin(),
                       value.tempo.alternatives.end(), [](double bpm) {
                           return std::isfinite(bpm) && bpm > 0.0 &&
                                  bpm <= 300.0;
                       });
}

bool validSendDestination(const ProjectModel& project,
                          const std::string& sourceId,
                          const std::string& destinationId) {
    if (sourceId == destinationId) return false;
    const TrackModel* destination = project.findTrack(destinationId);
    if (!destination) return false;
    return destination->kind == TrackKind::Bus ||
           destination->kind == TrackKind::Aux ||
           destination->kind == TrackKind::Group ||
           destination->kind == TrackKind::Master ||
           destination->kind == TrackKind::Pattern ||
           (destination->kind == TrackKind::Folder && destination->summing);
}

bool trackParentWouldCycle(const ProjectModel& project,
                           const std::string& trackId,
                           const std::string& parentId) {
    std::string cursor = parentId;
    for (std::size_t depth = 0; !cursor.empty() && depth <= project.tracks.size();
         ++depth) {
        if (cursor == trackId) return true;
        const TrackModel* parent = project.findTrack(cursor);
        if (!parent) return false;
        cursor = parent->parentId;
    }
    return !cursor.empty();
}

bool trackOutputWouldCycle(const ProjectModel& project,
                           const std::string& trackId,
                           const std::string& outputId) {
    std::string cursor = outputId;
    for (std::size_t depth = 0; !cursor.empty() && depth <= project.tracks.size();
         ++depth) {
        if (cursor == trackId) return true;
        const TrackModel* output = project.findTrack(cursor);
        if (!output) return false;
        cursor = output->outputBusId;
    }
    return !cursor.empty();
}

bool sendExistsOutside(const ProjectModel& project, const std::string& sendId,
                       const std::string& allowedTrackId) {
    for (const TrackModel& track : project.tracks) {
        if (track.id != allowedTrackId &&
            entityIndexOf(track.sends, sendId) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool sendIsDeleted(const SharedProjectDocument& state,
                   const std::string& sendId) {
    if (state.deletedSends.contains(sendId)) return true;
    for (const auto& [ignored, tombstone] : state.deletedTracks) {
        if (entityIndexOf(tombstone.track.sends, sendId) != std::string::npos)
            return true;
    }
    return false;
}

bool samePluginLocation(const PluginLocation& a, const PluginLocation& b) {
    return a == b;
}

std::vector<InsertModel>* pluginList(ProjectModel& project,
                                     const PluginLocation& location) {
    switch (location.chain) {
        case PluginChain::Master:
            return location.trackId.empty() && location.clipId.empty()
                       ? &project.masterInserts
                       : nullptr;
        case PluginChain::Track: {
            TrackModel* track = project.findTrack(location.trackId);
            return track && location.clipId.empty() ? &track->inserts : nullptr;
        }
        case PluginChain::SamplerFx: {
            TrackModel* track = project.findTrack(location.trackId);
            return track && location.clipId.empty() &&
                           track->samplerFx.isOwnedBy(track->instrument)
                       ? &track->samplerFx.inserts
                       : nullptr;
        }
        case PluginChain::Clip: {
            ClipLocation clip = findClip(project, location.clipId);
            return clip.clip && clip.track->id == location.trackId
                       ? &clip.clip->inserts
                       : nullptr;
        }
        case PluginChain::Instrument:
            return nullptr;
    }
    return nullptr;
}

InsertModel* pluginAt(ProjectModel& project, const PluginLocation& location,
                      const std::string& insertId) {
    if (location.chain == PluginChain::Instrument) {
        TrackModel* track = project.findTrack(location.trackId);
        if (!track || !location.clipId.empty() ||
            track->instrument.id != insertId) {
            return nullptr;
        }
        return &track->instrument;
    }
    std::vector<InsertModel>* inserts = pluginList(project, location);
    if (!inserts) return nullptr;
    const std::size_t index = entityIndexOf(*inserts, insertId);
    return index == std::string::npos ? nullptr : &(*inserts)[index];
}

bool pluginLocationParentDeleted(const SharedProjectDocument& state,
                                 const PluginLocation& location) {
    if (!location.trackId.empty() &&
        state.deletedTracks.contains(location.trackId)) {
        return true;
    }
    return !location.clipId.empty() && clipIsDeleted(state, location.clipId);
}

template <typename Callback>
void forEachLivePlugin(const ProjectModel& project, Callback&& callback) {
    for (const InsertModel& insert : project.masterInserts) callback(insert);
    for (const TrackModel& track : project.tracks) {
        if (!track.instrument.id.empty()) callback(track.instrument);
        for (const InsertModel& insert : track.samplerFx.inserts)
            callback(insert);
        for (const InsertModel& insert : track.inserts) callback(insert);
        for (const ClipModel& clip : track.clips)
            for (const InsertModel& insert : clip.inserts) callback(insert);
    }
}

bool livePluginExists(const ProjectModel& project, const std::string& insertId) {
    bool found = false;
    forEachLivePlugin(project, [&](const InsertModel& insert) {
        found = found || insert.id == insertId;
    });
    return found;
}

bool pluginIsDeleted(const SharedProjectDocument& state,
                     const std::string& insertId) {
    if (state.deletedPluginInserts.contains(insertId)) return true;
    const auto contains = [&](const TrackModel& track) {
        if (track.instrument.id == insertId) return true;
        if (entityIndexOf(track.samplerFx.inserts, insertId) !=
                std::string::npos ||
            entityIndexOf(track.inserts, insertId) != std::string::npos) {
            return true;
        }
        return std::any_of(track.clips.begin(), track.clips.end(),
                           [&](const ClipModel& clip) {
                               return entityIndexOf(clip.inserts, insertId) !=
                                      std::string::npos;
                           });
    };
    for (const auto& [ignored, tombstone] : state.deletedTracks)
        if (contains(tombstone.track)) return true;
    for (const auto& [ignored, tombstone] : state.deletedClips)
        if (entityIndexOf(tombstone.clip.inserts, insertId) != std::string::npos)
            return true;
    return false;
}

bool supportedBuiltin(const InsertModel& insert) {
    return insert.format == PluginFormat::Internal &&
           (insert.uid == "daw.sampler" || insert.uid == "daw.equalizer" ||
            insert.uid == "daw.gravity");
}

bool validParameters(const std::vector<InsertParameter>& parameters) {
    std::vector<std::string> ids;
    ids.reserve(parameters.size());
    for (const InsertParameter& parameter : parameters) {
        if (parameter.id.empty() || parameter.id.size() > 4096 ||
            !std::isfinite(parameter.value) ||
            std::find(ids.begin(), ids.end(), parameter.id) != ids.end()) {
            return false;
        }
        ids.push_back(parameter.id);
    }
    return true;
}

bool validBindings(const std::vector<PluginAssetBinding>& bindings,
                   bool sampler) {
    std::vector<std::string> keys;
    for (const PluginAssetBinding& binding : bindings) {
        if (binding.key.empty() || binding.key.size() > 96 ||
            std::find(keys.begin(), keys.end(), binding.key) != keys.end()) {
            return false;
        }
        keys.push_back(binding.key);
        const AssetKind expected = binding.key == "sample"
                                       ? AssetKind::Audio
                                       : binding.asset.kind;
        if (expected == AssetKind::Unknown ||
            !completeAsset(binding.asset, expected, false)) {
            return false;
        }
        if (sampler && binding.key == "sample" && !binding.required)
            return false;
    }
    return true;
}

bool validSharedInsert(const InsertModel& insert) {
    return supportedBuiltin(insert) && !insert.id.empty() &&
           insert.name.size() <= 4096 && insert.vendor.size() <= 4096 &&
           insert.path.empty() && insert.stateFile.empty() &&
           insert.rightStateFile.empty() && insert.pluginVersion.size() > 0 &&
           insert.pluginVersion.size() <= 64 && insert.stateSchemaVersion > 0 &&
           std::isfinite(insert.mix) && insert.mix >= 0.0f &&
           insert.mix <= 1.0f && insert.windowX == 0 && insert.windowY == 0 &&
           insert.windowWidth == 0 && insert.windowHeight == 0 &&
           !insert.windowOpen &&
           insert.editorChannel == PluginEditorChannel::Left &&
           completeAsset(insert.stateAsset, AssetKind::PluginState, true) &&
           completeAsset(insert.rightStateAsset, AssetKind::PluginState, true) &&
           validParameters(insert.parameters) &&
           validParameters(insert.rightParameters) &&
           validBindings(insert.assetBindings, insert.uid == "daw.sampler");
}

bool parameterVectorsEqual(const std::vector<InsertParameter>& a,
                           const std::vector<InsertParameter>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t index = 0; index < a.size(); ++index) {
        if (a[index].id != b[index].id || a[index].value != b[index].value)
            return false;
    }
    return true;
}

bool sharedInsertEqual(const InsertModel& a, const InsertModel& b) {
    return a.id == b.id && a.name == b.name && a.bypassed == b.bypassed &&
           a.format == b.format && a.uid == b.uid && a.vendor == b.vendor &&
           a.pluginVersion == b.pluginVersion &&
           a.stateSchemaVersion == b.stateSchemaVersion && a.mix == b.mix &&
           a.channelMode == b.channelMode &&
           a.sidechainTrackId == b.sidechainTrackId &&
           a.stateAsset == b.stateAsset &&
           a.rightStateAsset == b.rightStateAsset &&
           parameterVectorsEqual(a.parameters, b.parameters) &&
           parameterVectorsEqual(a.rightParameters, b.rightParameters) &&
           a.assetBindings == b.assetBindings;
}

bool completeAudioTake(const TakeModel& take) {
    return take.filePath.empty() && take.notes.empty() &&
           take.name.size() <= 4096 && std::isfinite(take.offsetSeconds) &&
           take.offsetSeconds >= 0.0 && std::isfinite(take.lengthSeconds) &&
           take.lengthSeconds >= 0.0 &&
           std::isfinite(take.clipOffsetSeconds) &&
           take.clipOffsetSeconds >= 0.0 && std::isfinite(take.gain) &&
           take.gain >= 0.0f && take.gain <= 4.0f && take.channels >= 1 &&
           take.channels <= 1024 && take.asset.kind == AssetKind::Audio &&
           take.asset.byteSize > 0 && lowercaseSha256(take.asset.sha256) &&
           take.asset.originalName.size() <= 4096 &&
           take.asset.mimeType.size() <= 255 && take.asset.codec.size() <= 255 &&
           std::isfinite(take.asset.sampleRate) &&
           take.asset.sampleRate >= 0.0 && take.asset.sampleRate <= 768000.0 &&
           take.asset.channels <= 1024;
}

bool compSegmentEqual(const CompSegment& a, const CompSegment& b) {
    return a.id == b.id && a.takeId == b.takeId &&
           a.startSeconds == b.startSeconds &&
           a.endSeconds == b.endSeconds;
}

bool compVectorsEqual(const std::vector<CompSegment>& a,
                      const std::vector<CompSegment>& b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), compSegmentEqual);
}

bool compPlacementIsValid(const std::vector<CompSegment>& segments,
                          const std::string& afterId,
                          const CompSegment& segment) {
    const std::size_t insertion = afterId.empty()
                                      ? 0
                                      : entityIndexOf(segments, afterId) + 1;
    if (insertion > segments.size()) return false;
    if (insertion > 0 &&
        segments[insertion - 1].endSeconds > segment.startSeconds) {
        return false;
    }
    return insertion == segments.size() ||
           segment.endSeconds <= segments[insertion].startSeconds;
}

bool pointPlacementIsChronological(
    const std::vector<AutomationPoint>& points, const std::string& afterId,
    double beats) {
    const std::size_t insertion = afterId.empty()
                                      ? 0
                                      : entityIndexOf(points, afterId) + 1;
    if (insertion > points.size()) return false;
    if (insertion > 0 && points[insertion - 1].beats > beats) return false;
    return insertion == points.size() || beats <= points[insertion].beats;
}

void markWriter(SharedProjectDocument& state, const std::string& field,
                const std::string& operationId, ChangeImpact& impact) {
    state.lastWriterByField[field] = operationId;
    impact.fieldKeys.insert(field);
}

void markCommandWriters(SharedProjectDocument& state,
                        const ProjectCommand& command, ChangeImpact& impact) {
    for (const std::string& field : commandTouchedFields(command))
        markWriter(state, field, command.meta.operationId, impact);
}

void markClipDescendantsWriter(SharedProjectDocument& state,
                               const std::string& clipId,
                               const ProjectCommand& command,
                               ChangeImpact& impact) {
    if (!clipId.empty()) {
        markWriter(state, ProjectReducer::clipDescendantsKey(clipId),
                   command.meta.operationId, impact);
    }
}

void markPluginGenerationWriter(SharedProjectDocument& state,
                                const std::string& insertId,
                                const ProjectCommand& command,
                                ChangeImpact& impact) {
    markWriter(state, "plugin:" + insertId + ":generation",
               command.meta.operationId, impact);
}

FieldWriterIs pluginGenerationCondition(const std::string& insertId,
                                        const ProjectCommand& command) {
    return FieldWriterIs{"plugin:" + insertId + ":generation",
                         command.meta.operationId};
}

bool checkConditions(const SharedProjectDocument& state,
                     const ProjectCommand& command, std::string& error) {
    for (const CommandCondition& condition : command.conditions) {
        const auto found = state.lastWriterByField.find(condition.fieldKey);
        const bool met = found != state.lastWriterByField.end() &&
                         found->second == condition.operationId;
        if (!met) {
            error = "command precondition no longer holds";
            return false;
        }
    }
    return true;
}

ApplyResult applyImpl(SharedProjectDocument& state,
                      const ProjectCommand& command, bool allowBatch,
                      bool recordOperation);

ApplyResult applyScalar(SharedProjectDocument& state,
                        const ProjectCommand& command,
                        const SetProjectScalar& body) {
    const std::string key = ProjectReducer::projectFieldKey(body.field);
    ScalarValue before;
    bool same = false;
    switch (body.field) {
        case ProjectScalar::Name:
        case ProjectScalar::AiInstructions: {
            const auto* value = std::get_if<std::string>(&body.value);
            if (!value || value->size() > 65536)
                return reject(ApplyCode::InvalidCommand, "invalid string scalar");
            std::string& target = body.field == ProjectScalar::Name
                                      ? state.project.name
                                      : state.project.aiInstructions;
            before = target;
            same = target == *value;
            target = *value;
            break;
        }
        case ProjectScalar::Tempo: {
            double value = 0.0;
            if (!doubleValue(body.value, value) || value <= 0.0 || value > 999.0)
                return reject(ApplyCode::InvalidCommand, "invalid tempo");
            const double previousTempo = state.project.tempo;
            before = previousTempo;
            same = state.project.tempo == value;
            state.project.tempo = value;
            if (!same) {
                // Project clip positions are stored in seconds but authored in
                // bars. Mirror EngineController::retimeToTempo exactly for the
                // shared document: every clip retains its start beat; MIDI
                // duration/fades are musical, while audio duration/fades stay
                // tied to the recorded material's clock.
                const double ratio = previousTempo / value;
                for (TrackModel& track : state.project.tracks) {
                    for (ClipModel& clip : track.clips) {
                        clip.startSeconds *= ratio;
                        if (clip.kind == ClipKind::Midi) {
                            clip.durationSeconds *= ratio;
                            clip.fadeInSeconds *= ratio;
                            clip.fadeOutSeconds *= ratio;
                        }
                    }
                }
            }
            break;
        }
        case ProjectScalar::RenderSampleRate: {
            double value = 0.0;
            if (!doubleValue(body.value, value) || value < 8000.0 ||
                value > 768000.0) {
                return reject(ApplyCode::InvalidCommand,
                              "invalid render sample rate");
            }
            before = state.project.sampleRate;
            same = state.project.sampleRate == value;
            state.project.sampleRate = value;
            break;
        }
        case ProjectScalar::MasterVolume: {
            double value = 0.0;
            if (!doubleValue(body.value, value) || value < 0.0 || value > 2.0)
                return reject(ApplyCode::InvalidCommand, "invalid master volume");
            before = double(state.project.masterVolume);
            same = state.project.masterVolume == float(value);
            state.project.masterVolume = float(value);
            break;
        }
        case ProjectScalar::MasterPan: {
            double value = 0.0;
            if (!doubleValue(body.value, value) || value < -1.0 || value > 1.0)
                return reject(ApplyCode::InvalidCommand, "invalid master pan");
            before = double(state.project.masterPan);
            same = state.project.masterPan == float(value);
            state.project.masterPan = float(value);
            break;
        }
    }

    ApplyResult result;
    result.code = same ? ApplyCode::NoChange : ApplyCode::Applied;
    result.impact.documentChanged = !same;
    result.impact.transportProjectionChanged =
        body.field == ProjectScalar::Tempo && !same;
    result.impact.timelineChanged = body.field == ProjectScalar::Tempo && !same;
    if (body.field == ProjectScalar::Tempo && !same) {
        for (const TrackModel& track : state.project.tracks) {
            if (!track.clips.empty()) result.impact.trackIds.insert(track.id);
        }
    }
    result.impact.masterGainChanged =
        (body.field == ProjectScalar::MasterVolume ||
         body.field == ProjectScalar::MasterPan) && !same;
    markWriter(state, key, command.meta.operationId, result.impact);
    if (body.field == ProjectScalar::Tempo) {
        markWriter(state, "project:tempoCascade", command.meta.operationId,
                   result.impact);
    }
    if (!same) {
        ProjectCommand inverse = inverseShell(command, SetProjectScalar{body.field, before});
        inverse.conditions.push_back(FieldWriterIs{key, command.meta.operationId});
        if (body.field == ProjectScalar::Tempo) {
            inverse.conditions.push_back(
                FieldWriterIs{"project:tempoCascade",
                              command.meta.operationId});
        }
        result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    }
    return result;
}

ApplyResult applyTimeSignature(SharedProjectDocument& state,
                               const ProjectCommand& command,
                               const SetTimeSignature& body) {
    const bool powerOfTwo = body.denominator > 0 &&
                            (body.denominator & (body.denominator - 1)) == 0;
    if (body.numerator < 1 || body.numerator > 32 || !powerOfTwo ||
        body.denominator > 32) {
        return reject(ApplyCode::InvalidCommand, "invalid time signature");
    }
    const SetTimeSignature before{state.project.timeSigNumerator,
                                  state.project.timeSigDenominator};
    const bool same = before.numerator == body.numerator &&
                      before.denominator == body.denominator;
    state.project.timeSigNumerator = body.numerator;
    state.project.timeSigDenominator = body.denominator;
    const std::string key = "project:timeSignature";
    ApplyResult result;
    result.code = same ? ApplyCode::NoChange : ApplyCode::Applied;
    result.impact.documentChanged = !same;
    result.impact.transportProjectionChanged = !same;
    markWriter(state, key, command.meta.operationId, result.impact);
    if (!same) {
        ProjectCommand inverse = inverseShell(command, before);
        inverse.conditions.push_back(FieldWriterIs{key, command.meta.operationId});
        result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    }
    return result;
}

ApplyResult applyProjectKey(SharedProjectDocument& state,
                            const ProjectCommand& command,
                            const SetProjectKey& body) {
    if (body.scale.empty() || body.scale.size() > 128)
        return reject(ApplyCode::InvalidCommand, "invalid project scale");
    const SetProjectKey before{state.project.keyRoot, state.project.scale};
    const int root = ((body.root % 12) + 12) % 12;
    const bool same = before.root == root && before.scale == body.scale;
    state.project.keyRoot = root;
    state.project.scale = body.scale;
    const std::string key = "project:key";
    ApplyResult result;
    result.code = same ? ApplyCode::NoChange : ApplyCode::Applied;
    result.impact.documentChanged = !same;
    markWriter(state, key, command.meta.operationId, result.impact);
    if (!same) {
        ProjectCommand inverse = inverseShell(command, before);
        inverse.conditions.push_back(FieldWriterIs{key, command.meta.operationId});
        result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    }
    return result;
}

ApplyResult applyAddTrack(SharedProjectDocument& state,
                          const ProjectCommand& command, const AddTrack& body) {
    if (body.trackId.empty())
        return reject(ApplyCode::InvalidCommand, "track id is required");
    if (state.project.findTrack(body.trackId))
        return reject(ApplyCode::InvalidCommand, "track id already exists");
    if (state.deletedTracks.contains(body.trackId))
        return reject(ApplyCode::DeletedEntity,
                      "tombstoned track ids cannot be reused");
    if (!validAnchor(state.project, body.afterId))
        return reject(ApplyCode::MissingAnchor, "track anchor does not exist");
    if (!body.parentId.empty()) {
        const TrackModel* parent = state.project.findTrack(body.parentId);
        if (!parent || (parent->kind != TrackKind::Folder &&
                        parent->kind != TrackKind::Pattern)) {
            return reject(ApplyCode::MissingEntity, "track parent does not exist");
        }
    }
    TrackModel track;
    track.id = body.trackId;
    track.kind = body.kind;
    track.name = body.name;
    track.color = body.color;
    track.parentId = body.parentId;
    if (!insertAfter(state.project, std::move(track), body.afterId, false))
        return reject(ApplyCode::MissingAnchor, "track anchor does not exist");

    ApplyResult result;
    result.code = ApplyCode::Applied;
    result.impact.documentChanged = true;
    result.impact.graphRebuild = true;
    result.impact.timelineChanged = true;
    result.impact.trackIds.insert(body.trackId);
    markWriter(state, ProjectReducer::trackLifecycleKey(body.trackId),
               command.meta.operationId, result.impact);
    markWriter(state, ProjectReducer::trackPositionKey(body.trackId),
               command.meta.operationId, result.impact);
    ProjectCommand inverse = inverseShell(command, DeleteTrack{body.trackId});
    inverse.conditions.push_back(FieldWriterIs{
        ProjectReducer::trackLifecycleKey(body.trackId),
        command.meta.operationId});
    result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    return result;
}

ApplyResult applyDeleteTrack(SharedProjectDocument& state,
                             const ProjectCommand& command,
                             const DeleteTrack& body) {
    if (body.trackId.empty())
        return reject(ApplyCode::InvalidCommand, "track id is required");
    if (state.deletedTracks.contains(body.trackId) &&
        !state.project.findTrack(body.trackId)) {
        ApplyResult result;
        result.code = ApplyCode::NoChange;
        result.message = "track is already deleted";
        markCommandWriters(state, command, result.impact);
        return result;
    }
    const std::size_t index = state.project.indexOf(body.trackId);
    if (index == std::string::npos)
        return reject(ApplyCode::MissingEntity, "track does not exist");

    TrackTombstone tombstone;
    tombstone.track = state.project.tracks[index];
    tombstone.afterId = index == 0 ? std::string() : state.project.tracks[index - 1].id;
    tombstone.deleteOperationId = command.meta.operationId;
    tombstone.deleteServerSequence = command.meta.serverSequence;
    state.project.tracks.erase(state.project.tracks.begin() + std::ptrdiff_t(index));
    ApplyResult result;
    result.code = ApplyCode::Applied;
    result.impact.documentChanged = true;
    result.impact.graphRebuild = true;
    result.impact.timelineChanged = true;
    result.impact.trackIds.insert(body.trackId);
    state.deletedTracks[body.trackId] = std::move(tombstone);
    markWriter(state, ProjectReducer::trackLifecycleKey(body.trackId),
               command.meta.operationId, result.impact);

    ProjectCommand inverse = inverseShell(
        command, RestoreTrack{body.trackId, command.meta.operationId});
    inverse.conditions.push_back(FieldWriterIs{
        ProjectReducer::trackLifecycleKey(body.trackId),
        command.meta.operationId});
    result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    return result;
}

ApplyResult applyRestoreTrack(SharedProjectDocument& state,
                              const ProjectCommand& command,
                              const RestoreTrack& body) {
    auto found = state.deletedTracks.find(body.trackId);
    if (found == state.deletedTracks.end() ||
        found->second.deleteOperationId != body.deleteOperationId) {
        return reject(ApplyCode::PreconditionsFailed,
                      "matching track tombstone does not exist");
    }
    if (state.project.findTrack(body.trackId))
        return reject(ApplyCode::InvalidCommand, "track already exists");
    TrackTombstone tombstone = found->second;
    insertAfter(state.project, tombstone.track, tombstone.afterId,
                /*missingAnchorFallsBack=*/true);
    state.deletedTracks.erase(found);

    ApplyResult result;
    result.code = ApplyCode::Applied;
    result.impact.documentChanged = true;
    result.impact.graphRebuild = true;
    result.impact.timelineChanged = true;
    result.impact.trackIds.insert(body.trackId);
    markWriter(state, ProjectReducer::trackLifecycleKey(body.trackId),
               command.meta.operationId, result.impact);
    markWriter(state, ProjectReducer::trackPositionKey(body.trackId),
               command.meta.operationId, result.impact);
    ProjectCommand inverse = inverseShell(command, DeleteTrack{body.trackId});
    inverse.conditions.push_back(FieldWriterIs{
        ProjectReducer::trackLifecycleKey(body.trackId),
        command.meta.operationId});
    result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    return result;
}

ApplyResult applyMoveTrack(SharedProjectDocument& state,
                           const ProjectCommand& command, const MoveTrack& body) {
    if (state.deletedTracks.contains(body.trackId)) {
        return reject(ApplyCode::DeletedEntity,
                      "delete wins over track move");
    }
    const std::size_t from = state.project.indexOf(body.trackId);
    if (from == std::string::npos)
        return reject(ApplyCode::MissingEntity, "track does not exist");
    if (!validAnchor(state.project, body.afterId, body.trackId))
        return reject(ApplyCode::MissingAnchor, "track anchor does not exist");
    const std::string beforeAfter = predecessorOf(state.project, body.trackId);
    const bool same = beforeAfter == body.afterId;
    if (!same) {
        TrackModel track = std::move(state.project.tracks[from]);
        state.project.tracks.erase(state.project.tracks.begin() + std::ptrdiff_t(from));
        if (!insertAfter(state.project, std::move(track), body.afterId, false))
            return reject(ApplyCode::MissingAnchor, "track anchor disappeared");
    }
    ApplyResult result;
    result.code = same ? ApplyCode::NoChange : ApplyCode::Applied;
    result.impact.documentChanged = !same;
    result.impact.graphRebuild = !same;
    result.impact.timelineChanged = !same;
    result.impact.trackIds.insert(body.trackId);
    const std::string key = ProjectReducer::trackPositionKey(body.trackId);
    markWriter(state, key, command.meta.operationId, result.impact);
    if (!same) {
        ProjectCommand inverse = inverseShell(command, MoveTrack{body.trackId, beforeAfter});
        inverse.conditions.push_back(FieldWriterIs{key, command.meta.operationId});
        result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    }
    return result;
}

ApplyResult applyTrackProperty(SharedProjectDocument& state,
                               const ProjectCommand& command,
                               const SetTrackProperty& body) {
    if (state.deletedTracks.contains(body.trackId)) {
        return reject(ApplyCode::DeletedEntity,
                      "delete wins over track property update");
    }
    TrackModel* track = state.project.findTrack(body.trackId);
    if (!track) return reject(ApplyCode::MissingEntity, "track does not exist");
    ScalarValue before;
    bool same = false;
    switch (body.property) {
        case TrackProperty::Name: {
            const auto* value = std::get_if<std::string>(&body.value);
            if (!value || value->size() > 4096)
                return reject(ApplyCode::InvalidCommand, "invalid track name");
            before = track->name;
            same = track->name == *value;
            track->name = *value;
            break;
        }
        case TrackProperty::Color: {
            std::int64_t value = 0;
            if (!integerValue(body.value, value) || value < 0 ||
                value > std::numeric_limits<std::uint32_t>::max()) {
                return reject(ApplyCode::InvalidCommand, "invalid track color");
            }
            before = std::int64_t(track->color);
            same = track->color == std::uint32_t(value);
            track->color = std::uint32_t(value);
            break;
        }
        case TrackProperty::Volume:
        case TrackProperty::Pan: {
            double value = 0.0;
            const double minimum = body.property == TrackProperty::Volume ? 0.0 : -1.0;
            const double maximum = body.property == TrackProperty::Volume ? 2.0 : 1.0;
            if (!doubleValue(body.value, value) || value < minimum || value > maximum)
                return reject(ApplyCode::InvalidCommand, "invalid track gain value");
            float& target = body.property == TrackProperty::Volume ? track->volume
                                                                    : track->pan;
            before = double(target);
            same = target == float(value);
            target = float(value);
            break;
        }
        case TrackProperty::Muted:
        case TrackProperty::Mono: {
            const auto* value = std::get_if<bool>(&body.value);
            if (!value)
                return reject(ApplyCode::InvalidCommand, "invalid track flag");
            bool& target = body.property == TrackProperty::Muted ? track->muted
                                                                 : track->mono;
            before = target;
            same = target == *value;
            target = *value;
            break;
        }
        case TrackProperty::Summing: {
            const auto* value = std::get_if<bool>(&body.value);
            if (!value || track->kind != TrackKind::Folder)
                return reject(ApplyCode::InvalidCommand,
                              "summing is valid only for folders");
            before = track->summing;
            same = track->summing == *value;
            track->summing = *value;
            break;
        }
    }
    ApplyResult result;
    result.code = same ? ApplyCode::NoChange : ApplyCode::Applied;
    result.impact.documentChanged = !same;
    result.impact.graphRebuild =
        !same && body.property == TrackProperty::Summing;
    result.impact.trackIds.insert(body.trackId);
    const std::string key = ProjectReducer::trackFieldKey(body.trackId,
                                                          body.property);
    markWriter(state, key, command.meta.operationId, result.impact);
    if (!same) {
        ProjectCommand inverse = inverseShell(
            command, SetTrackProperty{body.trackId, body.property, before});
        inverse.conditions.push_back(FieldWriterIs{key, command.meta.operationId});
        result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    }
    return result;
}

ApplyResult applySetTrackParent(SharedProjectDocument& state,
                                const ProjectCommand& command,
                                const SetTrackParent& body) {
    if (state.deletedTracks.contains(body.trackId) ||
        (!body.parentId.empty() && state.deletedTracks.contains(body.parentId))) {
        return reject(ApplyCode::DeletedEntity,
                      "delete wins over track parent update");
    }
    TrackModel* track = state.project.findTrack(body.trackId);
    if (!track) return reject(ApplyCode::MissingEntity, "track does not exist");
    if (!body.parentId.empty()) {
        const TrackModel* parent = state.project.findTrack(body.parentId);
        if (!parent || (parent->kind != TrackKind::Folder &&
                        parent->kind != TrackKind::Pattern)) {
            return reject(ApplyCode::MissingEntity,
                          "track parent does not exist or cannot own tracks");
        }
        if (trackParentWouldCycle(state.project, body.trackId, body.parentId))
            return reject(ApplyCode::InvalidCommand,
                          "track parent would create a cycle");
    }
    const std::string before = track->parentId;
    const bool same = before == body.parentId;
    track->parentId = body.parentId;
    const std::string key = "track:" + body.trackId + ":parentId";
    ApplyResult result;
    result.code = same ? ApplyCode::NoChange : ApplyCode::Applied;
    result.impact.documentChanged = !same;
    result.impact.graphRebuild = !same;
    result.impact.timelineChanged = !same;
    result.impact.trackIds.insert(body.trackId);
    markWriter(state, key, command.meta.operationId, result.impact);
    if (!same) {
        ProjectCommand inverse = inverseShell(
            command, SetTrackParent{body.trackId, before});
        inverse.conditions.push_back(FieldWriterIs{key, command.meta.operationId});
        result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    }
    return result;
}

ApplyResult applySetTrackOutput(SharedProjectDocument& state,
                                const ProjectCommand& command,
                                const SetTrackOutput& body) {
    if (state.deletedTracks.contains(body.trackId) ||
        (!body.outputTrackId.empty() &&
         state.deletedTracks.contains(body.outputTrackId))) {
        return reject(ApplyCode::DeletedEntity,
                      "delete wins over track output update");
    }
    TrackModel* track = state.project.findTrack(body.trackId);
    if (!track) return reject(ApplyCode::MissingEntity, "track does not exist");
    if (!body.outputTrackId.empty() &&
        (!validSendDestination(state.project, body.trackId,
                               body.outputTrackId) ||
         trackOutputWouldCycle(state.project, body.trackId,
                               body.outputTrackId))) {
        return reject(ApplyCode::InvalidCommand,
                      "track output is missing, unsupported, or cyclic");
    }
    const std::string before = track->outputBusId;
    const bool same = before == body.outputTrackId;
    track->outputBusId = body.outputTrackId;
    const std::string key = "track:" + body.trackId + ":outputTrackId";
    ApplyResult result;
    result.code = same ? ApplyCode::NoChange : ApplyCode::Applied;
    result.impact.documentChanged = !same;
    result.impact.graphRebuild = !same;
    result.impact.trackIds.insert(body.trackId);
    if (!body.outputTrackId.empty())
        result.impact.trackIds.insert(body.outputTrackId);
    markWriter(state, key, command.meta.operationId, result.impact);
    if (!same) {
        ProjectCommand inverse = inverseShell(
            command, SetTrackOutput{body.trackId, before});
        inverse.conditions.push_back(FieldWriterIs{key, command.meta.operationId});
        result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    }
    return result;
}

ApplyResult applyAddSend(SharedProjectDocument& state,
                         const ProjectCommand& command, const AddSend& body) {
    if (state.deletedTracks.contains(body.trackId) ||
        state.deletedTracks.contains(body.send.destinationTrackId) ||
        sendIsDeleted(state, body.send.id)) {
        return reject(ApplyCode::DeletedEntity, "delete wins over send creation");
    }
    TrackModel* track = state.project.findTrack(body.trackId);
    if (!track) return reject(ApplyCode::MissingEntity, "send track does not exist");
    if (entityIndexOf(track->sends, body.send.id) != std::string::npos ||
        sendExistsOutside(state.project, body.send.id, body.trackId)) {
        return reject(ApplyCode::InvalidCommand, "send id already exists");
    }
    if (!validEntityAnchor(track->sends, body.afterId))
        return reject(ApplyCode::MissingAnchor, "send anchor does not exist");
    if (!validSendDestination(state.project, body.trackId,
                              body.send.destinationTrackId) ||
        !std::isfinite(body.send.level) || body.send.level < 0.0f ||
        body.send.level > float(kMaximumSendLevel)) {
        return reject(ApplyCode::InvalidCommand, "invalid send payload");
    }
    insertEntityAfter(track->sends, body.send, body.afterId, false);
    ApplyResult result;
    result.code = ApplyCode::Applied;
    result.impact.documentChanged = true;
    result.impact.graphRebuild = true;
    result.impact.trackIds.insert(body.trackId);
    result.impact.trackIds.insert(body.send.destinationTrackId);
    result.impact.sendIds.insert(body.send.id);
    markCommandWriters(state, command, result.impact);
    ProjectCommand inverse = inverseShell(
        command, DeleteSend{body.trackId, body.send.id});
    inverse.conditions.push_back(FieldWriterIs{
        ProjectReducer::sendLifecycleKey(body.send.id),
        command.meta.operationId});
    result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    return result;
}

ApplyResult applyDeleteSend(SharedProjectDocument& state,
                            const ProjectCommand& command,
                            const DeleteSend& body) {
    if (state.deletedSends.contains(body.sendId)) {
        ApplyResult result;
        result.code = ApplyCode::NoChange;
        result.message = "send is already deleted";
        markCommandWriters(state, command, result.impact);
        return result;
    }
    if (state.deletedTracks.contains(body.trackId) ||
        sendIsDeleted(state, body.sendId)) {
        return reject(ApplyCode::DeletedEntity,
                      "parent delete wins over send delete");
    }
    TrackModel* track = state.project.findTrack(body.trackId);
    if (!track) return reject(ApplyCode::MissingEntity, "send track does not exist");
    const std::size_t index = entityIndexOf(track->sends, body.sendId);
    if (index == std::string::npos)
        return reject(ApplyCode::MissingEntity, "send does not exist");
    SendTombstone tombstone;
    tombstone.trackId = body.trackId;
    tombstone.send = track->sends[index];
    tombstone.afterId = index == 0 ? std::string() : track->sends[index - 1].id;
    tombstone.deleteOperationId = command.meta.operationId;
    tombstone.deleteServerSequence = command.meta.serverSequence;
    track->sends.erase(track->sends.begin() + std::ptrdiff_t(index));
    state.deletedSends[body.sendId] = std::move(tombstone);
    ApplyResult result;
    result.code = ApplyCode::Applied;
    result.impact.documentChanged = true;
    result.impact.graphRebuild = true;
    result.impact.trackIds.insert(body.trackId);
    result.impact.sendIds.insert(body.sendId);
    markCommandWriters(state, command, result.impact);
    ProjectCommand inverse = inverseShell(
        command, RestoreSend{body.trackId, body.sendId,
                             command.meta.operationId});
    inverse.conditions.push_back(FieldWriterIs{
        ProjectReducer::sendLifecycleKey(body.sendId),
        command.meta.operationId});
    result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    return result;
}

ApplyResult applyRestoreSend(SharedProjectDocument& state,
                             const ProjectCommand& command,
                             const RestoreSend& body) {
    auto found = state.deletedSends.find(body.sendId);
    if (found == state.deletedSends.end() ||
        found->second.trackId != body.trackId ||
        found->second.deleteOperationId != body.deleteOperationId) {
        return reject(ApplyCode::PreconditionsFailed,
                      "matching send tombstone does not exist");
    }
    if (state.deletedTracks.contains(body.trackId))
        return reject(ApplyCode::DeletedEntity,
                      "delete wins over send restore");
    TrackModel* track = state.project.findTrack(body.trackId);
    if (!track) return reject(ApplyCode::MissingEntity, "send track does not exist");
    if (entityIndexOf(track->sends, body.sendId) != std::string::npos ||
        sendExistsOutside(state.project, body.sendId, body.trackId)) {
        return reject(ApplyCode::InvalidCommand, "send id already exists");
    }
    SendTombstone tombstone = found->second;
    if (!validSendDestination(state.project, body.trackId,
                              tombstone.send.destinationTrackId)) {
        return reject(ApplyCode::MissingEntity,
                      "send destination no longer exists");
    }
    insertEntityAfter(track->sends, tombstone.send, tombstone.afterId, true);
    state.deletedSends.erase(found);
    ApplyResult result;
    result.code = ApplyCode::Applied;
    result.impact.documentChanged = true;
    result.impact.graphRebuild = true;
    result.impact.trackIds.insert(body.trackId);
    result.impact.sendIds.insert(body.sendId);
    markCommandWriters(state, command, result.impact);
    ProjectCommand inverse = inverseShell(command,
                                          DeleteSend{body.trackId, body.sendId});
    inverse.conditions.push_back(FieldWriterIs{
        ProjectReducer::sendLifecycleKey(body.sendId),
        command.meta.operationId});
    result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    return result;
}

ApplyResult applyMoveSend(SharedProjectDocument& state,
                          const ProjectCommand& command,
                          const MoveSend& body) {
    if (state.deletedTracks.contains(body.trackId) ||
        sendIsDeleted(state, body.sendId)) {
        return reject(ApplyCode::DeletedEntity, "delete wins over send move");
    }
    TrackModel* track = state.project.findTrack(body.trackId);
    if (!track) return reject(ApplyCode::MissingEntity, "send track does not exist");
    const std::size_t index = entityIndexOf(track->sends, body.sendId);
    if (index == std::string::npos)
        return reject(ApplyCode::MissingEntity, "send does not exist");
    if (!validEntityAnchor(track->sends, body.afterId, body.sendId))
        return reject(ApplyCode::MissingAnchor, "send anchor does not exist");
    const std::string before = entityPredecessor(track->sends, body.sendId);
    const bool same = before == body.afterId;
    if (!same) {
        SendModel moved = std::move(track->sends[index]);
        track->sends.erase(track->sends.begin() + std::ptrdiff_t(index));
        insertEntityAfter(track->sends, std::move(moved), body.afterId, false);
    }
    ApplyResult result;
    result.code = same ? ApplyCode::NoChange : ApplyCode::Applied;
    result.impact.documentChanged = !same;
    result.impact.graphRebuild = !same;
    result.impact.trackIds.insert(body.trackId);
    result.impact.sendIds.insert(body.sendId);
    const std::string key = ProjectReducer::sendPositionKey(body.sendId);
    markWriter(state, key, command.meta.operationId, result.impact);
    if (!same) {
        ProjectCommand inverse = inverseShell(
            command, MoveSend{body.trackId, body.sendId, before});
        inverse.conditions.push_back(FieldWriterIs{key, command.meta.operationId});
        result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    }
    return result;
}

ApplyResult applySetSendProperty(SharedProjectDocument& state,
                                 const ProjectCommand& command,
                                 const SetSendProperty& body) {
    if (state.deletedTracks.contains(body.trackId) ||
        sendIsDeleted(state, body.sendId)) {
        return reject(ApplyCode::DeletedEntity,
                      "delete wins over send property update");
    }
    TrackModel* track = state.project.findTrack(body.trackId);
    if (!track) return reject(ApplyCode::MissingEntity, "send track does not exist");
    const std::size_t index = entityIndexOf(track->sends, body.sendId);
    if (index == std::string::npos)
        return reject(ApplyCode::MissingEntity, "send does not exist");
    SendModel& send = track->sends[index];
    ScalarValue before;
    bool same = false;
    switch (body.property) {
        case SendProperty::DestinationTrackId: {
            const auto* value = std::get_if<std::string>(&body.value);
            if (!value || !validSendDestination(state.project, body.trackId,
                                                *value)) {
                return reject(ApplyCode::InvalidCommand,
                              "invalid send destination");
            }
            before = send.destinationTrackId;
            same = send.destinationTrackId == *value;
            send.destinationTrackId = *value;
            break;
        }
        case SendProperty::Level: {
            double value = 0.0;
            if (!doubleValue(body.value, value) || value < 0.0 ||
                value > kMaximumSendLevel)
                return reject(ApplyCode::InvalidCommand, "invalid send level");
            before = double(send.level);
            same = send.level == float(value);
            send.level = float(value);
            break;
        }
        case SendProperty::PreFader:
        case SendProperty::Enabled: {
            const auto* value = std::get_if<bool>(&body.value);
            if (!value)
                return reject(ApplyCode::InvalidCommand, "invalid send flag");
            bool& target = body.property == SendProperty::PreFader
                               ? send.preFader
                               : send.enabled;
            before = target;
            same = target == *value;
            target = *value;
            break;
        }
    }
    ApplyResult result;
    result.code = same ? ApplyCode::NoChange : ApplyCode::Applied;
    result.impact.documentChanged = !same;
    result.impact.graphRebuild = !same;
    result.impact.trackIds.insert(body.trackId);
    result.impact.sendIds.insert(body.sendId);
    const std::string key = "send:" + body.sendId + ":" +
                            sendPropertyName(body.property);
    markWriter(state, key, command.meta.operationId, result.impact);
    if (!same) {
        ProjectCommand inverse = inverseShell(
            command, SetSendProperty{body.trackId, body.sendId, body.property,
                                     before});
        inverse.conditions.push_back(FieldWriterIs{key, command.meta.operationId});
        result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    }
    return result;
}

ApplyResult applyAddClip(SharedProjectDocument& state,
                         const ProjectCommand& command,
                         const AddClip& body) {
    TrackModel* track = state.project.findTrack(body.trackId);
    if (!track) {
        if (state.deletedTracks.contains(body.trackId)) {
            return reject(ApplyCode::DeletedEntity,
                          "delete wins over clip creation");
        }
        return reject(ApplyCode::MissingEntity, "clip track does not exist");
    }
    if (findClip(state.project, body.clipId).clip)
        return reject(ApplyCode::InvalidCommand, "clip id already exists");
    if (clipIsDeleted(state, body.clipId))
        return reject(ApplyCode::DeletedEntity,
                      "tombstoned clip ids cannot be reused");
    if (!validEntityAnchor(track->clips, body.afterId))
        return reject(ApplyCode::MissingAnchor, "clip anchor does not exist");
    if (!std::isfinite(body.startSeconds) || body.startSeconds < 0.0 ||
        !std::isfinite(body.durationSeconds) || body.durationSeconds < 0.0 ||
        body.name.size() > 4096) {
        return reject(ApplyCode::InvalidCommand, "invalid clip payload");
    }

    ClipModel clip;
    clip.id = body.clipId;
    clip.kind = body.kind;
    clip.name = body.name;
    clip.startSeconds = body.startSeconds;
    clip.durationSeconds = body.durationSeconds;
    clip.color = body.color;
    insertEntityAfter(track->clips, std::move(clip), body.afterId, false);

    ApplyResult result;
    result.code = ApplyCode::Applied;
    result.impact.documentChanged = true;
    result.impact.graphRebuild = true;
    result.impact.timelineChanged = true;
    result.impact.trackIds.insert(body.trackId);
    result.impact.clipIds.insert(body.clipId);
    markCommandWriters(state, command, result.impact);
    ProjectCommand inverse = inverseShell(
        command, DeleteClip{body.trackId, body.clipId});
    for (const std::string& field : commandTouchedFields(command)) {
        inverse.conditions.push_back(
            FieldWriterIs{field, command.meta.operationId});
    }
    result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    return result;
}

ApplyResult applyDeleteClip(SharedProjectDocument& state,
                            const ProjectCommand& command,
                            const DeleteClip& body) {
    if (state.deletedClips.contains(body.clipId)) {
        ApplyResult result;
        result.code = ApplyCode::NoChange;
        result.message = "clip is already deleted";
        markCommandWriters(state, command, result.impact);
        return result;
    }
    if (state.deletedTracks.contains(body.trackId) ||
        deletedTrackContainsClip(state, body.clipId)) {
        return reject(ApplyCode::DeletedEntity,
                      "parent track delete wins over clip delete");
    }
    ClipLocation location = findClip(state.project, body.clipId);
    if (!location.clip)
        return reject(ApplyCode::MissingEntity, "clip does not exist");
    if (location.track->id != body.trackId)
        return reject(ApplyCode::MissingEntity,
                      "clip does not belong to the requested track");

    ClipTombstone tombstone;
    tombstone.trackId = body.trackId;
    tombstone.clip = location.track->clips[location.clipIndex];
    tombstone.afterId = location.clipIndex == 0
                            ? std::string()
                            : location.track->clips[location.clipIndex - 1].id;
    tombstone.deleteOperationId = command.meta.operationId;
    tombstone.deleteServerSequence = command.meta.serverSequence;
    location.track->clips.erase(location.track->clips.begin() +
                                std::ptrdiff_t(location.clipIndex));
    state.deletedClips[body.clipId] = std::move(tombstone);

    ApplyResult result;
    result.code = ApplyCode::Applied;
    result.impact.documentChanged = true;
    result.impact.graphRebuild = true;
    result.impact.timelineChanged = true;
    result.impact.trackIds.insert(body.trackId);
    result.impact.clipIds.insert(body.clipId);
    markCommandWriters(state, command, result.impact);
    ProjectCommand inverse = inverseShell(
        command, RestoreClip{body.trackId, body.clipId,
                             command.meta.operationId});
    inverse.conditions.push_back(FieldWriterIs{
        ProjectReducer::clipLifecycleKey(body.clipId),
        command.meta.operationId});
    inverse.conditions.push_back(FieldWriterIs{
        ProjectReducer::clipDescendantsKey(body.clipId),
        command.meta.operationId});
    result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    return result;
}

ApplyResult applyRestoreClip(SharedProjectDocument& state,
                             const ProjectCommand& command,
                             const RestoreClip& body) {
    auto found = state.deletedClips.find(body.clipId);
    if (found == state.deletedClips.end() ||
        found->second.deleteOperationId != body.deleteOperationId ||
        found->second.trackId != body.trackId) {
        return reject(ApplyCode::PreconditionsFailed,
                      "matching clip tombstone does not exist");
    }
    TrackModel* track = state.project.findTrack(body.trackId);
    if (!track) {
        ApplyResult result;
        result.code = state.deletedTracks.contains(body.trackId)
                          ? ApplyCode::DeletedEntity
                          : ApplyCode::MissingEntity;
        result.message = "clip restore track does not exist";
        return result;
    }
    if (findClip(state.project, body.clipId).clip)
        return reject(ApplyCode::InvalidCommand, "clip already exists");
    ClipTombstone tombstone = found->second;
    insertEntityAfter(track->clips, tombstone.clip, tombstone.afterId,
                      /*missingAnchorFallsBack=*/true);
    state.deletedClips.erase(found);

    ApplyResult result;
    result.code = ApplyCode::Applied;
    result.impact.documentChanged = true;
    result.impact.graphRebuild = true;
    result.impact.timelineChanged = true;
    result.impact.trackIds.insert(body.trackId);
    result.impact.clipIds.insert(body.clipId);
    markCommandWriters(state, command, result.impact);
    ProjectCommand inverse = inverseShell(
        command, DeleteClip{body.trackId, body.clipId});
    inverse.conditions.push_back(FieldWriterIs{
        ProjectReducer::clipLifecycleKey(body.clipId),
        command.meta.operationId});
    inverse.conditions.push_back(FieldWriterIs{
        ProjectReducer::clipDescendantsKey(body.clipId),
        command.meta.operationId});
    result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    return result;
}

ApplyResult applyMoveClip(SharedProjectDocument& state,
                          const ProjectCommand& command,
                          const MoveClip& body) {
    if (clipIsDeleted(state, body.clipId) ||
        state.deletedTracks.contains(body.trackId)) {
        return reject(ApplyCode::DeletedEntity,
                      "delete wins over clip move");
    }
    ClipLocation source = findClip(state.project, body.clipId);
    if (!source.clip)
        return reject(ApplyCode::MissingEntity, "clip does not exist");
    if (source.track->id != body.sourceTrackId)
        return reject(ApplyCode::PreconditionsFailed,
                      "clip source track changed");
    TrackModel* destination = state.project.findTrack(body.trackId);
    if (!destination)
        return reject(ApplyCode::MissingEntity,
                      "clip destination track does not exist");
    if (!validEntityAnchor(destination->clips, body.afterId, body.clipId))
        return reject(ApplyCode::MissingAnchor, "clip anchor does not exist");

    const std::string beforeTrackId = source.track->id;
    const std::string beforeAfter =
        entityPredecessor(source.track->clips, body.clipId);
    const bool same = beforeTrackId == body.trackId &&
                      beforeAfter == body.afterId;
    if (!same) {
        ClipModel moved = std::move(source.track->clips[source.clipIndex]);
        source.track->clips.erase(source.track->clips.begin() +
                                  std::ptrdiff_t(source.clipIndex));
        insertEntityAfter(destination->clips, std::move(moved), body.afterId,
                          false);
    }

    ApplyResult result;
    result.code = same ? ApplyCode::NoChange : ApplyCode::Applied;
    result.impact.documentChanged = !same;
    result.impact.graphRebuild = !same;
    result.impact.timelineChanged = !same;
    result.impact.trackIds.insert(beforeTrackId);
    result.impact.trackIds.insert(body.trackId);
    result.impact.clipIds.insert(body.clipId);
    const std::string key = ProjectReducer::clipPositionKey(body.clipId);
    markWriter(state, key, command.meta.operationId, result.impact);
    if (!same) {
        ProjectCommand inverse = inverseShell(
            command, MoveClip{body.clipId, body.trackId, beforeTrackId,
                              beforeAfter});
        inverse.conditions.push_back(
            FieldWriterIs{key, command.meta.operationId});
        result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    }
    return result;
}

ApplyResult applyClipProperty(SharedProjectDocument& state,
                              const ProjectCommand& command,
                              const SetClipProperty& body) {
    if (clipScopeIsDeleted(state, body.trackId, body.clipId)) {
        return reject(ApplyCode::DeletedEntity,
                      "delete wins over clip property update");
    }
    ClipLocation location = findClip(state.project, body.clipId);
    if (!location.clip || location.track->id != body.trackId)
        return reject(ApplyCode::MissingEntity, "clip does not exist in track");
    ClipModel& clip = *location.clip;
    ScalarValue before;
    bool same = false;
    switch (body.property) {
        case ClipProperty::Name: {
            const auto* value = std::get_if<std::string>(&body.value);
            if (!value || value->size() > 4096)
                return reject(ApplyCode::InvalidCommand, "invalid clip name");
            before = clip.name;
            same = clip.name == *value;
            clip.name = *value;
            break;
        }
        case ClipProperty::StartSeconds:
        case ClipProperty::DurationSeconds:
        case ClipProperty::OffsetSeconds: {
            double value = 0.0;
            if (!doubleValue(body.value, value) || value < 0.0)
                return reject(ApplyCode::InvalidCommand, "invalid clip time");
            double* target = body.property == ClipProperty::StartSeconds
                                 ? &clip.startSeconds
                                 : (body.property == ClipProperty::DurationSeconds
                                        ? &clip.durationSeconds
                                        : &clip.offsetSeconds);
            before = *target;
            same = *target == value;
            *target = value;
            break;
        }
        case ClipProperty::Gain:
        case ClipProperty::Pan: {
            double value = 0.0;
            const double minimum = body.property == ClipProperty::Gain ? 0.0 : -1.0;
            const double maximum = body.property == ClipProperty::Gain ? 4.0 : 1.0;
            if (!doubleValue(body.value, value) || value < minimum ||
                value > maximum) {
                return reject(ApplyCode::InvalidCommand,
                              "invalid clip gain value");
            }
            float& target = body.property == ClipProperty::Gain ? clip.gain
                                                                 : clip.pan;
            before = double(target);
            same = target == float(value);
            target = float(value);
            break;
        }
        case ClipProperty::Muted: {
            const auto* value = std::get_if<bool>(&body.value);
            if (!value)
                return reject(ApplyCode::InvalidCommand, "invalid clip mute flag");
            before = clip.muted;
            same = clip.muted == *value;
            clip.muted = *value;
            break;
        }
        case ClipProperty::Color: {
            std::int64_t value = 0;
            if (!integerValue(body.value, value) || value < 0 ||
                value > std::numeric_limits<std::uint32_t>::max()) {
                return reject(ApplyCode::InvalidCommand, "invalid clip color");
            }
            before = std::int64_t(clip.color);
            same = clip.color == std::uint32_t(value);
            clip.color = std::uint32_t(value);
            break;
        }
        case ClipProperty::CompCrossfadeMs: {
            double value = 0.0;
            if (!doubleValue(body.value, value) || value < 0.0 ||
                value > 20.0) {
                return reject(ApplyCode::InvalidCommand,
                              "invalid comp crossfade value");
            }
            before = clip.compCrossfadeMs;
            same = clip.compCrossfadeMs == value;
            clip.compCrossfadeMs = value;
            break;
        }
    }

    ApplyResult result;
    result.code = same ? ApplyCode::NoChange : ApplyCode::Applied;
    result.impact.documentChanged = !same;
    result.impact.timelineChanged = !same;
    result.impact.graphRebuild = !same &&
        (body.property == ClipProperty::Gain ||
         body.property == ClipProperty::Pan ||
         body.property == ClipProperty::Muted ||
         body.property == ClipProperty::CompCrossfadeMs ||
         body.property == ClipProperty::OffsetSeconds ||
         body.property == ClipProperty::DurationSeconds);
    result.impact.trackIds.insert(body.trackId);
    result.impact.clipIds.insert(body.clipId);
    const std::string key =
        ProjectReducer::clipFieldKey(body.clipId, body.property);
    markWriter(state, key, command.meta.operationId, result.impact);
    const bool tempoSensitive =
        body.property == ClipProperty::StartSeconds ||
        body.property == ClipProperty::DurationSeconds;
    if (tempoSensitive) {
        markWriter(state, "project:tempoCascade", command.meta.operationId,
                   result.impact);
    }
    if (!same) {
        ProjectCommand inverse = inverseShell(
            command,
            SetClipProperty{body.trackId, body.clipId, body.property, before});
        inverse.conditions.push_back(
            FieldWriterIs{key, command.meta.operationId});
        if (tempoSensitive) {
            inverse.conditions.push_back(
                FieldWriterIs{"project:tempoCascade",
                              command.meta.operationId});
        }
        result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    }
    return result;
}

ApplyResult applySetClipAsset(SharedProjectDocument& state,
                              const ProjectCommand& command,
                              const SetClipAsset& body) {
    if (clipScopeIsDeleted(state, body.trackId, body.clipId))
        return reject(ApplyCode::DeletedEntity,
                      "delete wins over clip asset update");
    ClipLocation location = findClip(state.project, body.clipId);
    if (!location.clip || location.track->id != body.trackId ||
        location.clip->kind != ClipKind::Audio) {
        return reject(ApplyCode::MissingEntity,
                      "audio clip does not exist in track");
    }
    if (!completeAsset(body.asset, AssetKind::Audio, true))
        return reject(ApplyCode::InvalidCommand,
                      "clip asset is not a complete audio reference");
    const AssetRef before = location.clip->asset;
    // A shared clip never carries a machine-local decoder result. Keep this
    // small piece of derived metadata canonical under the asset field itself,
    // so recording.commit can create a flat clip without inventing another
    // whitelisted mutation solely for AssetRef::channels.
    const int canonicalChannels = body.asset.empty()
                                      ? 0
                                      : int(body.asset.channels);
    const bool same = before == body.asset;
    if (!same) {
        location.clip->asset = body.asset;
        location.clip->channels = canonicalChannels;
    }
    const std::string key = "clip:" + body.clipId + ":asset";
    ApplyResult result;
    result.code = same ? ApplyCode::NoChange : ApplyCode::Applied;
    result.impact.documentChanged = !same;
    result.impact.graphRebuild = !same;
    result.impact.timelineChanged = !same;
    result.impact.trackIds.insert(body.trackId);
    result.impact.clipIds.insert(body.clipId);
    markWriter(state, key, command.meta.operationId, result.impact);
    if (!same) {
        ProjectCommand inverse = inverseShell(
            command, SetClipAsset{body.trackId, body.clipId, before});
        inverse.conditions.push_back(FieldWriterIs{key, command.meta.operationId});
        result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    }
    return result;
}

ApplyResult applySetClipSampleEdit(SharedProjectDocument& state,
                                   const ProjectCommand& command,
                                   const SetClipSampleEdit& body) {
    if (clipScopeIsDeleted(state, body.trackId, body.clipId))
        return reject(ApplyCode::DeletedEntity,
                      "delete wins over clip sample edit");
    ClipLocation location = findClip(state.project, body.clipId);
    if (!location.clip || location.track->id != body.trackId ||
        location.clip->kind != ClipKind::Audio) {
        return reject(ApplyCode::MissingEntity,
                      "audio clip does not exist in track");
    }
    if (!validSampleEdit(body.sampleEdit))
        return reject(ApplyCode::InvalidCommand, "invalid clip sample edit");
    const ClipSampleEditModel before = location.clip->sampleEdit;
    const bool same = sampleEditEqual(before, body.sampleEdit);
    location.clip->sampleEdit = body.sampleEdit;
    const std::string key = "clip:" + body.clipId + ":sampleEdit";
    ApplyResult result;
    result.code = same ? ApplyCode::NoChange : ApplyCode::Applied;
    result.impact.documentChanged = !same;
    result.impact.graphRebuild = !same;
    result.impact.timelineChanged = !same;
    result.impact.trackIds.insert(body.trackId);
    result.impact.clipIds.insert(body.clipId);
    markWriter(state, key, command.meta.operationId, result.impact);
    if (!same) {
        ProjectCommand inverse = inverseShell(
            command, SetClipSampleEdit{body.trackId, body.clipId, before});
        inverse.conditions.push_back(FieldWriterIs{key, command.meta.operationId});
        result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    }
    return result;
}

ApplyResult applySetClipFade(SharedProjectDocument& state,
                             const ProjectCommand& command,
                             const SetClipFade& body) {
    if (clipScopeIsDeleted(state, body.trackId, body.clipId))
        return reject(ApplyCode::DeletedEntity,
                      "delete wins over clip fade edit");
    ClipLocation location = findClip(state.project, body.clipId);
    if (!location.clip || location.track->id != body.trackId)
        return reject(ApplyCode::MissingEntity,
                      "clip does not exist in track");
    if (!std::isfinite(body.fadeInSeconds) ||
        !std::isfinite(body.fadeOutSeconds) || body.fadeInSeconds < 0.0 ||
        body.fadeOutSeconds < 0.0 ||
        body.fadeInSeconds > location.clip->durationSeconds ||
        body.fadeOutSeconds > location.clip->durationSeconds ||
        body.fadeInSeconds + body.fadeOutSeconds >
            location.clip->durationSeconds) {
        return reject(ApplyCode::InvalidCommand, "invalid clip fades");
    }
    const SetClipFade before{body.trackId, body.clipId,
                             location.clip->fadeInSeconds,
                             location.clip->fadeOutSeconds};
    const bool same = before.fadeInSeconds == body.fadeInSeconds &&
                      before.fadeOutSeconds == body.fadeOutSeconds;
    location.clip->fadeInSeconds = body.fadeInSeconds;
    location.clip->fadeOutSeconds = body.fadeOutSeconds;
    ApplyResult result;
    result.code = same ? ApplyCode::NoChange : ApplyCode::Applied;
    result.impact.documentChanged = !same;
    result.impact.graphRebuild = !same;
    result.impact.timelineChanged = !same;
    result.impact.trackIds.insert(body.trackId);
    result.impact.clipIds.insert(body.clipId);
    markCommandWriters(state, command, result.impact);
    if (!same) {
        ProjectCommand inverse = inverseShell(command, before);
        inverse.conditions.push_back(FieldWriterIs{
            "clip:" + body.clipId + ":fadeInSeconds",
            command.meta.operationId});
        inverse.conditions.push_back(FieldWriterIs{
            "clip:" + body.clipId + ":fadeOutSeconds",
            command.meta.operationId});
        inverse.conditions.push_back(FieldWriterIs{
            "project:tempoCascade", command.meta.operationId});
        result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    }
    return result;
}

ApplyResult applySetClipFadeCurve(SharedProjectDocument& state,
                                  const ProjectCommand& command,
                                  const SetClipFadeCurve& body) {
    if (clipScopeIsDeleted(state, body.trackId, body.clipId))
        return reject(ApplyCode::DeletedEntity,
                      "delete wins over clip fade curve edit");
    ClipLocation location = findClip(state.project, body.clipId);
    if (!location.clip || location.track->id != body.trackId)
        return reject(ApplyCode::MissingEntity,
                      "clip does not exist in track");
    if (!std::isfinite(body.curve) || body.curve < -1.0 || body.curve > 1.0)
        return reject(ApplyCode::InvalidCommand, "invalid clip fade curve");
    double& target = body.edge == ClipEdge::In
                         ? location.clip->fadeInCurve
                         : location.clip->fadeOutCurve;
    const double beforeValue = target;
    const bool same = beforeValue == body.curve;
    target = body.curve;
    const std::string key = "clip:" + body.clipId + ":fade" +
                            (body.edge == ClipEdge::In ? "In" : "Out") +
                            "Curve";
    ApplyResult result;
    result.code = same ? ApplyCode::NoChange : ApplyCode::Applied;
    result.impact.documentChanged = !same;
    result.impact.graphRebuild = !same;
    result.impact.timelineChanged = !same;
    result.impact.trackIds.insert(body.trackId);
    result.impact.clipIds.insert(body.clipId);
    markWriter(state, key, command.meta.operationId, result.impact);
    if (!same) {
        ProjectCommand inverse = inverseShell(
            command, SetClipFadeCurve{body.trackId, body.clipId, body.edge,
                                      beforeValue});
        inverse.conditions.push_back(FieldWriterIs{key,
                                                    command.meta.operationId});
        result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    }
    return result;
}

ApplyResult applySetClipFadeMode(SharedProjectDocument& state,
                                 const ProjectCommand& command,
                                 const SetClipFadeMode& body) {
    if (clipScopeIsDeleted(state, body.trackId, body.clipId))
        return reject(ApplyCode::DeletedEntity,
                      "delete wins over clip fade mode edit");
    ClipLocation location = findClip(state.project, body.clipId);
    if (!location.clip || location.track->id != body.trackId)
        return reject(ApplyCode::MissingEntity,
                      "clip does not exist in track");
    if (body.mode == ClipFadeMode::Tape &&
        location.clip->kind != ClipKind::Audio) {
        return reject(ApplyCode::InvalidCommand,
                      "tape fade requires an audio clip");
    }
    ClipFadeMode& target = body.edge == ClipEdge::In
                               ? location.clip->fadeInMode
                               : location.clip->fadeOutMode;
    const ClipFadeMode beforeValue = target;
    const bool same = beforeValue == body.mode;
    target = body.mode;
    const std::string key = "clip:" + body.clipId + ":fade" +
                            (body.edge == ClipEdge::In ? "In" : "Out") +
                            "Mode";
    ApplyResult result;
    result.code = same ? ApplyCode::NoChange : ApplyCode::Applied;
    result.impact.documentChanged = !same;
    result.impact.graphRebuild = !same;
    result.impact.timelineChanged = !same;
    result.impact.trackIds.insert(body.trackId);
    result.impact.clipIds.insert(body.clipId);
    markWriter(state, key, command.meta.operationId, result.impact);
    if (!same) {
        ProjectCommand inverse = inverseShell(
            command, SetClipFadeMode{body.trackId, body.clipId, body.edge,
                                     beforeValue});
        inverse.conditions.push_back(FieldWriterIs{key,
                                                    command.meta.operationId});
        result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    }
    return result;
}

ApplyResult applySetClipPatternOwner(SharedProjectDocument& state,
                                     const ProjectCommand& command,
                                     const SetClipPatternOwner& body) {
    if (clipScopeIsDeleted(state, body.trackId, body.clipId) ||
        (!body.patternClipId.empty() &&
         clipIsDeleted(state, body.patternClipId))) {
        return reject(ApplyCode::DeletedEntity,
                      "delete wins over clip pattern ownership edit");
    }
    ClipLocation location = findClip(state.project, body.clipId);
    if (!location.clip || location.track->id != body.trackId ||
        location.clip->kind != ClipKind::Midi) {
        return reject(ApplyCode::MissingEntity,
                      "MIDI clip does not exist in track");
    }
    if (!body.patternClipId.empty()) {
        ClipLocation owner = findClip(state.project, body.patternClipId);
        if (!owner.clip || owner.clip->kind != ClipKind::Pattern ||
            owner.clip->id == body.clipId) {
            return reject(ApplyCode::MissingEntity,
                          "pattern owner does not exist");
        }
    }
    const std::string before = location.clip->patternClipId;
    const bool same = before == body.patternClipId;
    location.clip->patternClipId = body.patternClipId;
    const std::string key = "clip:" + body.clipId + ":patternClipId";
    ApplyResult result;
    result.code = same ? ApplyCode::NoChange : ApplyCode::Applied;
    result.impact.documentChanged = !same;
    result.impact.graphRebuild = !same;
    result.impact.timelineChanged = !same;
    result.impact.trackIds.insert(body.trackId);
    result.impact.clipIds.insert(body.clipId);
    markCommandWriters(state, command, result.impact);
    if (!same) {
        ProjectCommand inverse = inverseShell(
            command, SetClipPatternOwner{body.trackId, body.clipId, before});
        inverse.conditions.push_back(FieldWriterIs{key,
                                                    command.meta.operationId});
        result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    }
    return result;
}

ApplyResult applySetClipMusicalAnalysis(
    SharedProjectDocument& state, const ProjectCommand& command,
    const SetClipMusicalAnalysis& body) {
    if (clipScopeIsDeleted(state, body.trackId, body.clipId))
        return reject(ApplyCode::DeletedEntity,
                      "delete wins over clip analysis edit");
    ClipLocation location = findClip(state.project, body.clipId);
    if (!location.clip || location.track->id != body.trackId ||
        location.clip->kind != ClipKind::Audio) {
        return reject(ApplyCode::MissingEntity,
                      "audio clip does not exist in track");
    }
    if (!validMusicalAnalysis(body.analysis))
        return reject(ApplyCode::InvalidCommand,
                      "invalid clip musical analysis");
    const ClipMusicalAnalysisModel before = location.clip->musicalAnalysis;
    const bool same = musicalAnalysisEqual(before, body.analysis);
    location.clip->musicalAnalysis = body.analysis;
    const std::string key = "clip:" + body.clipId + ":musicalAnalysis";
    ApplyResult result;
    result.code = same ? ApplyCode::NoChange : ApplyCode::Applied;
    result.impact.documentChanged = !same;
    result.impact.timelineChanged = !same;
    result.impact.trackIds.insert(body.trackId);
    result.impact.clipIds.insert(body.clipId);
    markWriter(state, key, command.meta.operationId, result.impact);
    if (!same) {
        ProjectCommand inverse = inverseShell(
            command,
            SetClipMusicalAnalysis{body.trackId, body.clipId, before});
        inverse.conditions.push_back(FieldWriterIs{key,
                                                    command.meta.operationId});
        result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    }
    return result;
}

bool pluginKindMatchesLocation(const PluginLocation& location,
                               const InsertModel& insert) {
    const bool instrument = insert.uid == "daw.sampler";
    return location.chain == PluginChain::Instrument ? instrument : !instrument;
}

ApplyResult applyAddPlugin(SharedProjectDocument& state,
                           const ProjectCommand& command,
                           const AddPluginInsert& body) {
    if (pluginLocationParentDeleted(state, body.location) ||
        pluginIsDeleted(state, body.insert.id)) {
        return reject(ApplyCode::DeletedEntity,
                      "delete wins over plugin creation");
    }
    if (!validSharedInsert(body.insert) ||
        !pluginKindMatchesLocation(body.location, body.insert)) {
        return reject(ApplyCode::InvalidCommand,
                      "plugin must be a clean supported built-in for its chain");
    }
    if (!body.insert.sidechainTrackId.empty() &&
        (!state.project.findTrack(body.insert.sidechainTrackId) ||
         body.insert.sidechainTrackId == body.location.trackId)) {
        return reject(ApplyCode::InvalidCommand,
                      "plugin sidechain track does not exist or is self-routed");
    }
    if (livePluginExists(state.project, body.insert.id))
        return reject(ApplyCode::InvalidCommand, "plugin id already exists");

    if (body.location.chain == PluginChain::Instrument) {
        TrackModel* track = state.project.findTrack(body.location.trackId);
        if (!track || !body.location.clipId.empty())
            return reject(ApplyCode::MissingEntity,
                          "instrument track does not exist");
        if (!track->instrument.id.empty() || !body.afterId.empty())
            return reject(ApplyCode::InvalidCommand,
                          "instrument chain has exactly one unanchored slot");
        track->instrument = body.insert;
        if (body.insert.uid == "daw.sampler")
            track->samplerFx.ownerInstrumentId = body.insert.id;
    } else {
        std::vector<InsertModel>* inserts = pluginList(state.project,
                                                       body.location);
        if (!inserts)
            return reject(ApplyCode::MissingEntity,
                          "plugin chain does not exist");
        if (!validEntityAnchor(*inserts, body.afterId))
            return reject(ApplyCode::MissingAnchor,
                          "plugin anchor does not exist");
        insertEntityAfter(*inserts, body.insert, body.afterId, false);
    }
    ApplyResult result;
    result.code = ApplyCode::Applied;
    result.impact.documentChanged = true;
    result.impact.graphRebuild = true;
    if (!body.location.trackId.empty())
        result.impact.trackIds.insert(body.location.trackId);
    if (!body.location.clipId.empty())
        result.impact.clipIds.insert(body.location.clipId);
    result.impact.pluginInsertIds.insert(body.insert.id);
    markCommandWriters(state, command, result.impact);
    ProjectCommand inverse = inverseShell(
        command, DeletePluginInsert{body.location, body.insert.id});
    inverse.conditions.push_back(FieldWriterIs{
        ProjectReducer::pluginLifecycleKey(body.insert.id),
        command.meta.operationId});
    inverse.conditions.push_back(
        pluginGenerationCondition(body.insert.id, command));
    result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    return result;
}

ApplyResult applyDeletePlugin(SharedProjectDocument& state,
                              const ProjectCommand& command,
                              const DeletePluginInsert& body) {
    if (state.deletedPluginInserts.contains(body.insertId)) {
        ApplyResult result;
        result.code = ApplyCode::NoChange;
        result.message = "plugin is already deleted";
        markCommandWriters(state, command, result.impact);
        return result;
    }
    if (pluginLocationParentDeleted(state, body.location) ||
        pluginIsDeleted(state, body.insertId)) {
        return reject(ApplyCode::DeletedEntity,
                      "parent delete wins over plugin delete");
    }
    InsertModel* insert = pluginAt(state.project, body.location, body.insertId);
    if (!insert)
        return reject(ApplyCode::MissingEntity,
                      "plugin does not exist in chain");
    PluginInsertTombstone tombstone;
    tombstone.location = body.location;
    tombstone.insert = *insert;
    tombstone.deleteOperationId = command.meta.operationId;
    tombstone.deleteServerSequence = command.meta.serverSequence;
    if (body.location.chain == PluginChain::Instrument) {
        TrackModel* track = state.project.findTrack(body.location.trackId);
        if (!track) return reject(ApplyCode::MissingEntity, "track does not exist");
        tombstone.afterId.clear();
        track->instrument = {};
    } else {
        std::vector<InsertModel>* inserts = pluginList(state.project,
                                                       body.location);
        if (!inserts)
            return reject(ApplyCode::MissingEntity, "plugin chain does not exist");
        const std::size_t index = entityIndexOf(*inserts, body.insertId);
        tombstone.afterId = index == 0 ? std::string()
                                       : (*inserts)[index - 1].id;
        inserts->erase(inserts->begin() + std::ptrdiff_t(index));
    }
    state.deletedPluginInserts[body.insertId] = std::move(tombstone);
    ApplyResult result;
    result.code = ApplyCode::Applied;
    result.impact.documentChanged = true;
    result.impact.graphRebuild = true;
    if (!body.location.trackId.empty())
        result.impact.trackIds.insert(body.location.trackId);
    if (!body.location.clipId.empty())
        result.impact.clipIds.insert(body.location.clipId);
    result.impact.pluginInsertIds.insert(body.insertId);
    markCommandWriters(state, command, result.impact);
    ProjectCommand inverse = inverseShell(
        command, RestorePluginInsert{body.location, body.insertId,
                                     command.meta.operationId});
    inverse.conditions.push_back(FieldWriterIs{
        ProjectReducer::pluginLifecycleKey(body.insertId),
        command.meta.operationId});
    result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    return result;
}

ApplyResult applyRestorePlugin(SharedProjectDocument& state,
                               const ProjectCommand& command,
                               const RestorePluginInsert& body) {
    auto found = state.deletedPluginInserts.find(body.insertId);
    if (found == state.deletedPluginInserts.end() ||
        found->second.deleteOperationId != body.deleteOperationId ||
        !samePluginLocation(found->second.location, body.location)) {
        return reject(ApplyCode::PreconditionsFailed,
                      "matching plugin tombstone does not exist");
    }
    if (pluginLocationParentDeleted(state, body.location))
        return reject(ApplyCode::DeletedEntity,
                      "delete wins over plugin restore");
    if (livePluginExists(state.project, body.insertId))
        return reject(ApplyCode::InvalidCommand, "plugin id already exists");
    PluginInsertTombstone tombstone = found->second;
    if (body.location.chain == PluginChain::Instrument) {
        TrackModel* track = state.project.findTrack(body.location.trackId);
        if (!track)
            return reject(ApplyCode::MissingEntity,
                          "instrument track does not exist");
        if (!track->instrument.id.empty())
            return reject(ApplyCode::PreconditionsFailed,
                          "instrument slot is no longer empty");
        track->instrument = tombstone.insert;
        if (tombstone.insert.uid == "daw.sampler")
            track->samplerFx.ownerInstrumentId = tombstone.insert.id;
    } else {
        std::vector<InsertModel>* inserts = pluginList(state.project,
                                                       body.location);
        if (!inserts)
            return reject(ApplyCode::MissingEntity,
                          "plugin chain does not exist");
        insertEntityAfter(*inserts, tombstone.insert, tombstone.afterId, true);
    }
    state.deletedPluginInserts.erase(found);
    ApplyResult result;
    result.code = ApplyCode::Applied;
    result.impact.documentChanged = true;
    result.impact.graphRebuild = true;
    if (!body.location.trackId.empty())
        result.impact.trackIds.insert(body.location.trackId);
    if (!body.location.clipId.empty())
        result.impact.clipIds.insert(body.location.clipId);
    result.impact.pluginInsertIds.insert(body.insertId);
    markCommandWriters(state, command, result.impact);
    ProjectCommand inverse = inverseShell(
        command, DeletePluginInsert{body.location, body.insertId});
    inverse.conditions.push_back(FieldWriterIs{
        ProjectReducer::pluginLifecycleKey(body.insertId),
        command.meta.operationId});
    inverse.conditions.push_back(
        pluginGenerationCondition(body.insertId, command));
    result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    return result;
}

ApplyResult applyMovePlugin(SharedProjectDocument& state,
                            const ProjectCommand& command,
                            const MovePluginInsert& body) {
    if (pluginLocationParentDeleted(state, body.location) ||
        pluginIsDeleted(state, body.insertId)) {
        return reject(ApplyCode::DeletedEntity, "delete wins over plugin move");
    }
    if (body.location.chain == PluginChain::Instrument) {
        if (!pluginAt(state.project, body.location, body.insertId))
            return reject(ApplyCode::MissingEntity,
                          "instrument does not exist");
        if (!body.afterId.empty())
            return reject(ApplyCode::InvalidCommand,
                          "instrument slot cannot be reordered");
        ApplyResult result;
        result.code = ApplyCode::NoChange;
        markCommandWriters(state, command, result.impact);
        return result;
    }
    std::vector<InsertModel>* inserts = pluginList(state.project,
                                                   body.location);
    if (!inserts)
        return reject(ApplyCode::MissingEntity, "plugin chain does not exist");
    const std::size_t index = entityIndexOf(*inserts, body.insertId);
    if (index == std::string::npos)
        return reject(ApplyCode::MissingEntity, "plugin does not exist");
    if (!validEntityAnchor(*inserts, body.afterId, body.insertId))
        return reject(ApplyCode::MissingAnchor,
                      "plugin anchor does not exist");
    const std::string before = entityPredecessor(*inserts, body.insertId);
    const bool same = before == body.afterId;
    if (!same) {
        InsertModel moved = std::move((*inserts)[index]);
        inserts->erase(inserts->begin() + std::ptrdiff_t(index));
        insertEntityAfter(*inserts, std::move(moved), body.afterId, false);
    }
    ApplyResult result;
    result.code = same ? ApplyCode::NoChange : ApplyCode::Applied;
    result.impact.documentChanged = !same;
    result.impact.graphRebuild = !same;
    if (!body.location.trackId.empty())
        result.impact.trackIds.insert(body.location.trackId);
    if (!body.location.clipId.empty())
        result.impact.clipIds.insert(body.location.clipId);
    result.impact.pluginInsertIds.insert(body.insertId);
    const std::string key = ProjectReducer::pluginPositionKey(body.insertId);
    markWriter(state, key, command.meta.operationId, result.impact);
    markPluginGenerationWriter(state, body.insertId, command, result.impact);
    markClipDescendantsWriter(state, body.location.clipId, command,
                              result.impact);
    if (!same) {
        ProjectCommand inverse = inverseShell(
            command, MovePluginInsert{body.location, body.insertId, before});
        inverse.conditions.push_back(FieldWriterIs{key, command.meta.operationId});
        inverse.conditions.push_back(
            pluginGenerationCondition(body.insertId, command));
        result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    }
    return result;
}

ApplyResult applyReplacePlugin(SharedProjectDocument& state,
                               const ProjectCommand& command,
                               const ReplacePluginInsert& body) {
    if (pluginLocationParentDeleted(state, body.location) ||
        pluginIsDeleted(state, body.insertId)) {
        return reject(ApplyCode::DeletedEntity,
                      "delete wins over plugin replacement");
    }
    InsertModel* insert = pluginAt(state.project, body.location, body.insertId);
    if (!insert)
        return reject(ApplyCode::MissingEntity, "plugin does not exist");
    if (body.replacement.id != body.insertId ||
        !validSharedInsert(body.replacement) ||
        !pluginKindMatchesLocation(body.location, body.replacement)) {
        return reject(ApplyCode::InvalidCommand,
                      "replacement must preserve a clean supported insert id");
    }
    if (!body.replacement.sidechainTrackId.empty() &&
        (!state.project.findTrack(body.replacement.sidechainTrackId) ||
         body.replacement.sidechainTrackId == body.location.trackId)) {
        return reject(ApplyCode::InvalidCommand,
                      "plugin sidechain track does not exist or is self-routed");
    }
    const InsertModel before = *insert;
    const bool same = sharedInsertEqual(before, body.replacement);
    *insert = body.replacement;
    if (body.location.chain == PluginChain::Instrument) {
        TrackModel* track = state.project.findTrack(body.location.trackId);
        if (track) track->samplerFx.ownerInstrumentId = body.insertId;
    }
    ApplyResult result;
    result.code = same ? ApplyCode::NoChange : ApplyCode::Applied;
    result.impact.documentChanged = !same;
    result.impact.graphRebuild = !same;
    if (!body.location.trackId.empty())
        result.impact.trackIds.insert(body.location.trackId);
    if (!body.location.clipId.empty())
        result.impact.clipIds.insert(body.location.clipId);
    result.impact.pluginInsertIds.insert(body.insertId);
    markCommandWriters(state, command, result.impact);
    if (!same) {
        ProjectCommand inverse = inverseShell(
            command, ReplacePluginInsert{body.location, body.insertId, before});
        inverse.conditions.push_back(FieldWriterIs{
            "plugin:" + body.insertId + ":generation",
            command.meta.operationId});
        result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    }
    return result;
}

ApplyResult applySetPluginProperty(SharedProjectDocument& state,
                                   const ProjectCommand& command,
                                   const SetPluginProperty& body) {
    if (pluginLocationParentDeleted(state, body.location) ||
        pluginIsDeleted(state, body.insertId)) {
        return reject(ApplyCode::DeletedEntity,
                      "delete wins over plugin property update");
    }
    InsertModel* insert = pluginAt(state.project, body.location, body.insertId);
    if (!insert)
        return reject(ApplyCode::MissingEntity, "plugin does not exist");
    ScalarValue before;
    bool same = false;
    switch (body.property) {
        case PluginProperty::Name: {
            const auto* value = std::get_if<std::string>(&body.value);
            if (!value || value->size() > 4096)
                return reject(ApplyCode::InvalidCommand, "invalid plugin name");
            before = insert->name;
            same = insert->name == *value;
            insert->name = *value;
            break;
        }
        case PluginProperty::Bypassed: {
            const auto* value = std::get_if<bool>(&body.value);
            if (!value)
                return reject(ApplyCode::InvalidCommand,
                              "invalid plugin bypass flag");
            before = insert->bypassed;
            same = insert->bypassed == *value;
            insert->bypassed = *value;
            break;
        }
        case PluginProperty::Mix: {
            double value = 0.0;
            if (!doubleValue(body.value, value) || value < 0.0 || value > 1.0)
                return reject(ApplyCode::InvalidCommand, "invalid plugin mix");
            before = double(insert->mix);
            same = insert->mix == float(value);
            insert->mix = float(value);
            break;
        }
        case PluginProperty::ChannelMode: {
            const auto* value = std::get_if<std::string>(&body.value);
            if (!value || (*value != "auto" && *value != "mono" &&
                           *value != "stereo" && *value != "dual-mono")) {
                return reject(ApplyCode::InvalidCommand,
                              "invalid plugin channel mode");
            }
            before = toString(insert->channelMode);
            const PluginChannelMode parsed = pluginChannelModeFromString(*value);
            same = insert->channelMode == parsed;
            insert->channelMode = parsed;
            break;
        }
        case PluginProperty::SidechainTrackId: {
            const auto* value = std::get_if<std::string>(&body.value);
            if (!value || (!value->empty() &&
                           (!state.project.findTrack(*value) ||
                            *value == body.location.trackId))) {
                return reject(ApplyCode::InvalidCommand,
                              "invalid plugin sidechain track");
            }
            before = insert->sidechainTrackId;
            same = insert->sidechainTrackId == *value;
            insert->sidechainTrackId = *value;
            break;
        }
    }
    ApplyResult result;
    result.code = same ? ApplyCode::NoChange : ApplyCode::Applied;
    result.impact.documentChanged = !same;
    result.impact.graphRebuild = !same;
    if (!body.location.trackId.empty())
        result.impact.trackIds.insert(body.location.trackId);
    if (!body.location.clipId.empty())
        result.impact.clipIds.insert(body.location.clipId);
    result.impact.pluginInsertIds.insert(body.insertId);
    const std::string key = "plugin:" + body.insertId + ":" +
                            pluginPropertyName(body.property);
    markWriter(state, key, command.meta.operationId, result.impact);
    markPluginGenerationWriter(state, body.insertId, command, result.impact);
    markClipDescendantsWriter(state, body.location.clipId, command,
                              result.impact);
    if (!same) {
        ProjectCommand inverse = inverseShell(
            command, SetPluginProperty{body.location, body.insertId,
                                       body.property, before});
        inverse.conditions.push_back(FieldWriterIs{key, command.meta.operationId});
        inverse.conditions.push_back(
            pluginGenerationCondition(body.insertId, command));
        result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    }
    return result;
}

SetPluginState pluginStateOf(const PluginLocation& location,
                             const InsertModel& insert) {
    return SetPluginState{location,
                          insert.id,
                          insert.pluginVersion,
                          insert.stateSchemaVersion,
                          insert.stateAsset,
                          insert.rightStateAsset,
                          insert.parameters,
                          insert.rightParameters,
                          insert.assetBindings};
}

bool pluginStateEqual(const InsertModel& insert, const SetPluginState& state) {
    return insert.pluginVersion == state.pluginVersion &&
           insert.stateSchemaVersion == state.stateSchemaVersion &&
           insert.stateAsset == state.stateAsset &&
           insert.rightStateAsset == state.rightStateAsset &&
           parameterVectorsEqual(insert.parameters, state.parameters) &&
           parameterVectorsEqual(insert.rightParameters,
                                 state.rightParameters) &&
           insert.assetBindings == state.assetBindings;
}

ApplyResult applySetPluginState(SharedProjectDocument& state,
                                const ProjectCommand& command,
                                const SetPluginState& body) {
    if (pluginLocationParentDeleted(state, body.location) ||
        pluginIsDeleted(state, body.insertId)) {
        return reject(ApplyCode::DeletedEntity,
                      "delete wins over plugin state update");
    }
    InsertModel* insert = pluginAt(state.project, body.location, body.insertId);
    if (!insert)
        return reject(ApplyCode::MissingEntity, "plugin does not exist");
    InsertModel candidate = *insert;
    candidate.pluginVersion = body.pluginVersion;
    candidate.stateSchemaVersion = body.stateSchemaVersion;
    candidate.stateAsset = body.stateAsset;
    candidate.rightStateAsset = body.rightStateAsset;
    candidate.parameters = body.parameters;
    candidate.rightParameters = body.rightParameters;
    candidate.assetBindings = body.assetBindings;
    if (!validSharedInsert(candidate))
        return reject(ApplyCode::InvalidCommand,
                      "invalid built-in plugin state");
    const SetPluginState before = pluginStateOf(body.location, *insert);
    const bool same = pluginStateEqual(*insert, body);
    *insert = std::move(candidate);
    ApplyResult result;
    result.code = same ? ApplyCode::NoChange : ApplyCode::Applied;
    result.impact.documentChanged = !same;
    result.impact.graphRebuild = !same;
    if (!body.location.trackId.empty())
        result.impact.trackIds.insert(body.location.trackId);
    if (!body.location.clipId.empty())
        result.impact.clipIds.insert(body.location.clipId);
    result.impact.pluginInsertIds.insert(body.insertId);
    const std::string key = "plugin:" + body.insertId + ":state";
    markWriter(state, key, command.meta.operationId, result.impact);
    markPluginGenerationWriter(state, body.insertId, command, result.impact);
    markClipDescendantsWriter(state, body.location.clipId, command,
                              result.impact);
    if (!same) {
        ProjectCommand inverse = inverseShell(command, before);
        inverse.conditions.push_back(FieldWriterIs{key, command.meta.operationId});
        inverse.conditions.push_back(
            pluginGenerationCondition(body.insertId, command));
        result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    }
    return result;
}

std::vector<InsertParameter>::iterator findParameter(
    std::vector<InsertParameter>& parameters, const std::string& parameterId) {
    return std::find_if(parameters.begin(), parameters.end(),
                        [&](const InsertParameter& value) {
                            return value.id == parameterId;
                        });
}

ApplyResult applySetPluginParameter(SharedProjectDocument& state,
                                    const ProjectCommand& command,
                                    const SetPluginParameter& body) {
    if (pluginLocationParentDeleted(state, body.location) ||
        pluginIsDeleted(state, body.insertId)) {
        return reject(ApplyCode::DeletedEntity,
                      "delete wins over plugin parameter update");
    }
    InsertModel* insert = pluginAt(state.project, body.location, body.insertId);
    if (!insert)
        return reject(ApplyCode::MissingEntity, "plugin does not exist");
    if (body.parameterId.empty() ||
        body.parameterId.size() > kMaxPluginParameterIdBytes ||
        !std::isfinite(body.value)) {
        return reject(ApplyCode::InvalidCommand, "invalid plugin parameter");
    }
    std::vector<InsertParameter>& parameters = body.rightChannel
                                                   ? insert->rightParameters
                                                   : insert->parameters;
    auto found = findParameter(parameters, body.parameterId);
    const bool inserted = found == parameters.end();
    const double before = inserted ? 0.0 : found->value;
    const bool same = !inserted && before == body.value;
    if (inserted)
        parameters.push_back(InsertParameter{body.parameterId, body.value});
    else
        found->value = body.value;
    ApplyResult result;
    result.code = same ? ApplyCode::NoChange : ApplyCode::Applied;
    result.impact.documentChanged = !same;
    result.impact.graphRebuild = !same;
    if (!body.location.trackId.empty())
        result.impact.trackIds.insert(body.location.trackId);
    if (!body.location.clipId.empty())
        result.impact.clipIds.insert(body.location.clipId);
    result.impact.pluginInsertIds.insert(body.insertId);
    const std::string key = "plugin:" + body.insertId + ":parameter:" +
                            (body.rightChannel ? "right:" : "left:") +
                            body.parameterId;
    const std::string stateKey = "plugin:" + body.insertId + ":state";
    markWriter(state, key, command.meta.operationId, result.impact);
    markWriter(state, stateKey, command.meta.operationId, result.impact);
    markPluginGenerationWriter(state, body.insertId, command, result.impact);
    markClipDescendantsWriter(state, body.location.clipId, command,
                              result.impact);
    if (!same) {
        ProjectCommand inverse = inserted
            ? inverseShell(command,
                           RemovePluginParameter{body.location, body.insertId,
                                                 body.parameterId,
                                                 body.rightChannel})
            : inverseShell(command,
                           SetPluginParameter{body.location, body.insertId,
                                              body.parameterId, before,
                                              body.rightChannel});
        inverse.conditions.push_back(FieldWriterIs{key, command.meta.operationId});
        inverse.conditions.push_back(
            FieldWriterIs{stateKey, command.meta.operationId});
        inverse.conditions.push_back(
            pluginGenerationCondition(body.insertId, command));
        result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    }
    return result;
}

ApplyResult applyRemovePluginParameter(SharedProjectDocument& state,
                                       const ProjectCommand& command,
                                       const RemovePluginParameter& body) {
    if (pluginLocationParentDeleted(state, body.location) ||
        pluginIsDeleted(state, body.insertId)) {
        return reject(ApplyCode::DeletedEntity,
                      "delete wins over plugin parameter removal");
    }
    InsertModel* insert = pluginAt(state.project, body.location, body.insertId);
    if (!insert)
        return reject(ApplyCode::MissingEntity, "plugin does not exist");
    if (body.parameterId.empty() ||
        body.parameterId.size() > kMaxPluginParameterIdBytes) {
        return reject(ApplyCode::InvalidCommand, "invalid plugin parameter");
    }
    std::vector<InsertParameter>& parameters = body.rightChannel
                                                   ? insert->rightParameters
                                                   : insert->parameters;
    auto found = findParameter(parameters, body.parameterId);
    const bool same = found == parameters.end();
    const double before = same ? 0.0 : found->value;
    if (!same) parameters.erase(found);
    ApplyResult result;
    result.code = same ? ApplyCode::NoChange : ApplyCode::Applied;
    result.impact.documentChanged = !same;
    result.impact.graphRebuild = !same;
    if (!body.location.trackId.empty())
        result.impact.trackIds.insert(body.location.trackId);
    if (!body.location.clipId.empty())
        result.impact.clipIds.insert(body.location.clipId);
    result.impact.pluginInsertIds.insert(body.insertId);
    const std::string key = "plugin:" + body.insertId + ":parameter:" +
                            (body.rightChannel ? "right:" : "left:") +
                            body.parameterId;
    const std::string stateKey = "plugin:" + body.insertId + ":state";
    markWriter(state, key, command.meta.operationId, result.impact);
    markWriter(state, stateKey, command.meta.operationId, result.impact);
    markPluginGenerationWriter(state, body.insertId, command, result.impact);
    markClipDescendantsWriter(state, body.location.clipId, command,
                              result.impact);
    if (!same) {
        ProjectCommand inverse = inverseShell(
            command, SetPluginParameter{body.location, body.insertId,
                                        body.parameterId, before,
                                        body.rightChannel});
        inverse.conditions.push_back(FieldWriterIs{key, command.meta.operationId});
        inverse.conditions.push_back(
            FieldWriterIs{stateKey, command.meta.operationId});
        inverse.conditions.push_back(
            pluginGenerationCondition(body.insertId, command));
        result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    }
    return result;
}

std::vector<PluginAssetBinding>::iterator findBinding(
    std::vector<PluginAssetBinding>& bindings, const std::string& key) {
    return std::find_if(bindings.begin(), bindings.end(),
                        [&](const PluginAssetBinding& value) {
                            return value.key == key;
                        });
}

ApplyResult applySetPluginBinding(SharedProjectDocument& state,
                                  const ProjectCommand& command,
                                  const SetPluginAssetBinding& body) {
    if (pluginLocationParentDeleted(state, body.location) ||
        pluginIsDeleted(state, body.insertId)) {
        return reject(ApplyCode::DeletedEntity,
                      "delete wins over plugin asset binding update");
    }
    InsertModel* insert = pluginAt(state.project, body.location, body.insertId);
    if (!insert)
        return reject(ApplyCode::MissingEntity, "plugin does not exist");
    const AssetKind expected = body.binding.key == "sample"
                                   ? AssetKind::Audio
                                   : body.binding.asset.kind;
    if (body.binding.key.empty() || body.binding.key.size() > 96 ||
        expected == AssetKind::Unknown ||
        !completeAsset(body.binding.asset, expected, false)) {
        return reject(ApplyCode::InvalidCommand,
                      "invalid plugin asset binding");
    }
    std::vector<PluginAssetBinding>& bindings = insert->assetBindings;
    auto found = findBinding(bindings, body.binding.key);
    const bool inserted = found == bindings.end();
    PluginAssetBinding before;
    if (!inserted) before = *found;
    const bool same = !inserted && before == body.binding;
    std::vector<PluginAssetBinding> candidate = bindings;
    auto candidateFound = findBinding(candidate, body.binding.key);
    if (candidateFound == candidate.end())
        candidate.push_back(body.binding);
    else
        *candidateFound = body.binding;
    if (!validBindings(candidate, insert->uid == "daw.sampler")) {
        return reject(ApplyCode::InvalidCommand,
                      "plugin asset bindings violate the built-in schema");
    }
    bindings = std::move(candidate);
    ApplyResult result;
    result.code = same ? ApplyCode::NoChange : ApplyCode::Applied;
    result.impact.documentChanged = !same;
    result.impact.graphRebuild = !same;
    if (!body.location.trackId.empty())
        result.impact.trackIds.insert(body.location.trackId);
    if (!body.location.clipId.empty())
        result.impact.clipIds.insert(body.location.clipId);
    result.impact.pluginInsertIds.insert(body.insertId);
    const std::string key = "plugin:" + body.insertId + ":assetBinding:" +
                            body.binding.key;
    const std::string stateKey = "plugin:" + body.insertId + ":state";
    markWriter(state, key, command.meta.operationId, result.impact);
    markWriter(state, stateKey, command.meta.operationId, result.impact);
    markPluginGenerationWriter(state, body.insertId, command, result.impact);
    markClipDescendantsWriter(state, body.location.clipId, command,
                              result.impact);
    if (!same) {
        ProjectCommand inverse = inserted
            ? inverseShell(command,
                           RemovePluginAssetBinding{body.location,
                                                    body.insertId,
                                                    body.binding.key})
            : inverseShell(command,
                           SetPluginAssetBinding{body.location, body.insertId,
                                                 before});
        inverse.conditions.push_back(FieldWriterIs{key, command.meta.operationId});
        inverse.conditions.push_back(
            FieldWriterIs{stateKey, command.meta.operationId});
        inverse.conditions.push_back(
            pluginGenerationCondition(body.insertId, command));
        result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    }
    return result;
}

ApplyResult applyRemovePluginBinding(SharedProjectDocument& state,
                                     const ProjectCommand& command,
                                     const RemovePluginAssetBinding& body) {
    if (pluginLocationParentDeleted(state, body.location) ||
        pluginIsDeleted(state, body.insertId)) {
        return reject(ApplyCode::DeletedEntity,
                      "delete wins over plugin asset binding removal");
    }
    InsertModel* insert = pluginAt(state.project, body.location, body.insertId);
    if (!insert)
        return reject(ApplyCode::MissingEntity, "plugin does not exist");
    auto found = findBinding(insert->assetBindings, body.key);
    const bool same = found == insert->assetBindings.end();
    PluginAssetBinding before;
    if (!same) {
        before = *found;
        insert->assetBindings.erase(found);
    }
    ApplyResult result;
    result.code = same ? ApplyCode::NoChange : ApplyCode::Applied;
    result.impact.documentChanged = !same;
    result.impact.graphRebuild = !same;
    if (!body.location.trackId.empty())
        result.impact.trackIds.insert(body.location.trackId);
    if (!body.location.clipId.empty())
        result.impact.clipIds.insert(body.location.clipId);
    result.impact.pluginInsertIds.insert(body.insertId);
    const std::string key = "plugin:" + body.insertId + ":assetBinding:" +
                            body.key;
    const std::string stateKey = "plugin:" + body.insertId + ":state";
    markWriter(state, key, command.meta.operationId, result.impact);
    markWriter(state, stateKey, command.meta.operationId, result.impact);
    markPluginGenerationWriter(state, body.insertId, command, result.impact);
    markClipDescendantsWriter(state, body.location.clipId, command,
                              result.impact);
    if (!same) {
        ProjectCommand inverse = inverseShell(
            command, SetPluginAssetBinding{body.location, body.insertId,
                                           before});
        inverse.conditions.push_back(FieldWriterIs{key, command.meta.operationId});
        inverse.conditions.push_back(
            FieldWriterIs{stateKey, command.meta.operationId});
        inverse.conditions.push_back(
            pluginGenerationCondition(body.insertId, command));
        result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    }
    return result;
}

ApplyResult applySetSamplerFxLevels(SharedProjectDocument& state,
                                    const ProjectCommand& command,
                                    const SetSamplerFxLevels& body) {
    if (state.deletedTracks.contains(body.trackId))
        return reject(ApplyCode::DeletedEntity,
                      "delete wins over Sampler FX levels");
    TrackModel* track = state.project.findTrack(body.trackId);
    if (!track || track->instrument.id != body.instrumentId ||
        !track->samplerFx.isOwnedBy(track->instrument)) {
        return reject(ApplyCode::MissingEntity,
                      "owned Sampler FX strip does not exist");
    }
    if (!std::isfinite(body.volume) || body.volume < 0.0 ||
        body.volume > 2.0 || !std::isfinite(body.pan) || body.pan < -1.0 ||
        body.pan > 1.0) {
        return reject(ApplyCode::InvalidCommand,
                      "invalid Sampler FX levels");
    }
    const SetSamplerFxLevels before{body.trackId, body.instrumentId,
                                    track->samplerFx.volume,
                                    track->samplerFx.pan};
    const bool same = track->samplerFx.volume == float(body.volume) &&
                      track->samplerFx.pan == float(body.pan);
    track->samplerFx.volume = float(body.volume);
    track->samplerFx.pan = float(body.pan);
    ApplyResult result;
    result.code = same ? ApplyCode::NoChange : ApplyCode::Applied;
    result.impact.documentChanged = !same;
    result.impact.graphRebuild = !same;
    result.impact.trackIds.insert(body.trackId);
    result.impact.pluginInsertIds.insert(body.instrumentId);
    markCommandWriters(state, command, result.impact);
    if (!same) {
        ProjectCommand inverse = inverseShell(command, before);
        inverse.conditions.push_back(FieldWriterIs{
            "samplerFx:" + body.instrumentId + ":volume",
            command.meta.operationId});
        inverse.conditions.push_back(FieldWriterIs{
            "samplerFx:" + body.instrumentId + ":pan",
            command.meta.operationId});
        result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    }
    return result;
}

bool validNote(const NoteModel& note) {
    return note.pitch >= 0 && note.pitch <= 127 &&
           std::isfinite(note.startBeats) && note.startBeats >= 0.0 &&
           std::isfinite(note.lengthBeats) && note.lengthBeats > 0.0 &&
           note.velocity >= 1 && note.velocity <= 127 &&
           std::isfinite(note.pan) && note.pan >= -1.0f && note.pan <= 1.0f;
}

ApplyResult applyUpsertMidiNote(SharedProjectDocument& state,
                                const ProjectCommand& command,
                                const UpsertMidiNote& body) {
    if (clipScopeIsDeleted(state, body.trackId, body.clipId) ||
        noteIsDeleted(state, body.note.id)) {
        return reject(ApplyCode::DeletedEntity,
                      "delete wins over MIDI note upsert");
    }
    ClipLocation location = findClip(state.project, body.clipId);
    if (!location.clip || location.track->id != body.trackId ||
        location.clip->kind != ClipKind::Midi) {
        return reject(ApplyCode::MissingEntity,
                      "MIDI clip does not exist in track");
    }
    if (!validNote(body.note))
        return reject(ApplyCode::InvalidCommand, "invalid MIDI note");
    if (liveNoteExistsOutside(state.project, body.note.id, body.clipId))
        return reject(ApplyCode::InvalidCommand,
                      "note id belongs to another clip");

    std::vector<NoteModel> candidate = location.clip->notes;
    const std::size_t existing = entityIndexOf(candidate, body.note.id);
    const bool inserted = existing == std::string::npos;
    NoteModel before;
    std::string beforeAfter;
    if (!inserted) {
        before = candidate[existing];
        beforeAfter = existing == 0 ? std::string() : candidate[existing - 1].id;
        candidate.erase(candidate.begin() + std::ptrdiff_t(existing));
    }
    if (!validEntityAnchor(candidate, body.afterId))
        return reject(ApplyCode::MissingAnchor, "note anchor does not exist");
    insertEntityAfter(candidate, body.note, body.afterId, false);
    const bool same = candidate == location.clip->notes;
    location.clip->notes = std::move(candidate);

    ApplyResult result;
    result.code = same ? ApplyCode::NoChange : ApplyCode::Applied;
    result.impact.documentChanged = !same;
    result.impact.timelineChanged = !same;
    result.impact.trackIds.insert(body.trackId);
    result.impact.clipIds.insert(body.clipId);
    result.impact.noteIds.insert(body.note.id);
    markCommandWriters(state, command, result.impact);
    if (!same) {
        ProjectCommand inverse = inserted
            ? inverseShell(command, DeleteMidiNote{body.trackId, body.clipId,
                                                   body.note.id})
            : inverseShell(command, UpsertMidiNote{body.trackId, body.clipId,
                                                   before, beforeAfter});
        const std::string descendantsKey =
            ProjectReducer::clipDescendantsKey(body.clipId);
        for (const std::string& field : commandTouchedFields(command)) {
            if (field == descendantsKey) continue;
            inverse.conditions.push_back(
                FieldWriterIs{field, command.meta.operationId});
        }
        result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    }
    return result;
}

ApplyResult applyDeleteMidiNote(SharedProjectDocument& state,
                                const ProjectCommand& command,
                                const DeleteMidiNote& body) {
    if (state.deletedNotes.contains(body.noteId)) {
        ApplyResult result;
        result.code = ApplyCode::NoChange;
        result.message = "MIDI note is already deleted";
        markCommandWriters(state, command, result.impact);
        return result;
    }
    if (clipScopeIsDeleted(state, body.trackId, body.clipId) ||
        noteIsDeleted(state, body.noteId)) {
        return reject(ApplyCode::DeletedEntity,
                      "parent delete wins over MIDI note delete");
    }
    ClipLocation location = findClip(state.project, body.clipId);
    if (!location.clip || location.track->id != body.trackId ||
        location.clip->kind != ClipKind::Midi) {
        return reject(ApplyCode::MissingEntity,
                      "MIDI clip does not exist in track");
    }
    const std::size_t index = entityIndexOf(location.clip->notes, body.noteId);
    if (index == std::string::npos)
        return reject(ApplyCode::MissingEntity, "MIDI note does not exist");
    MidiNoteTombstone tombstone;
    tombstone.trackId = body.trackId;
    tombstone.clipId = body.clipId;
    tombstone.note = location.clip->notes[index];
    tombstone.afterId = index == 0 ? std::string()
                                   : location.clip->notes[index - 1].id;
    tombstone.deleteOperationId = command.meta.operationId;
    tombstone.deleteServerSequence = command.meta.serverSequence;
    location.clip->notes.erase(location.clip->notes.begin() +
                               std::ptrdiff_t(index));
    state.deletedNotes[body.noteId] = std::move(tombstone);

    ApplyResult result;
    result.code = ApplyCode::Applied;
    result.impact.documentChanged = true;
    result.impact.timelineChanged = true;
    result.impact.trackIds.insert(body.trackId);
    result.impact.clipIds.insert(body.clipId);
    result.impact.noteIds.insert(body.noteId);
    markCommandWriters(state, command, result.impact);
    ProjectCommand inverse = inverseShell(
        command, RestoreMidiNote{body.trackId, body.clipId, body.noteId,
                                 command.meta.operationId});
    inverse.conditions.push_back(FieldWriterIs{
        ProjectReducer::noteLifecycleKey(body.noteId),
        command.meta.operationId});
    result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    return result;
}

ApplyResult applyRestoreMidiNote(SharedProjectDocument& state,
                                 const ProjectCommand& command,
                                 const RestoreMidiNote& body) {
    auto found = state.deletedNotes.find(body.noteId);
    if (found == state.deletedNotes.end() ||
        found->second.deleteOperationId != body.deleteOperationId ||
        found->second.trackId != body.trackId ||
        found->second.clipId != body.clipId) {
        return reject(ApplyCode::PreconditionsFailed,
                      "matching MIDI note tombstone does not exist");
    }
    if (clipScopeIsDeleted(state, body.trackId, body.clipId)) {
        return reject(ApplyCode::DeletedEntity,
                      "delete wins over MIDI note restore");
    }
    ClipLocation location = findClip(state.project, body.clipId);
    if (!location.clip || location.track->id != body.trackId ||
        location.clip->kind != ClipKind::Midi) {
        return reject(ApplyCode::MissingEntity,
                      "MIDI clip does not exist in track");
    }
    if (liveNoteExistsOutside(state.project, body.noteId, body.clipId) ||
        entityIndexOf(location.clip->notes, body.noteId) != std::string::npos) {
        return reject(ApplyCode::InvalidCommand, "MIDI note already exists");
    }
    MidiNoteTombstone tombstone = found->second;
    insertEntityAfter(location.clip->notes, tombstone.note, tombstone.afterId,
                      /*missingAnchorFallsBack=*/true);
    state.deletedNotes.erase(found);

    ApplyResult result;
    result.code = ApplyCode::Applied;
    result.impact.documentChanged = true;
    result.impact.timelineChanged = true;
    result.impact.trackIds.insert(body.trackId);
    result.impact.clipIds.insert(body.clipId);
    result.impact.noteIds.insert(body.noteId);
    markCommandWriters(state, command, result.impact);
    ProjectCommand inverse = inverseShell(
        command, DeleteMidiNote{body.trackId, body.clipId, body.noteId});
    inverse.conditions.push_back(FieldWriterIs{
        ProjectReducer::noteLifecycleKey(body.noteId),
        command.meta.operationId});
    result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    return result;
}

bool validAutomationPoint(const AutomationPoint& point) {
    return std::isfinite(point.beats) && point.beats >= 0.0 &&
           std::isfinite(point.value) && point.value >= 0.0 &&
           point.value <= 1.0 && std::isfinite(point.curve) &&
           point.curve >= -1.0 && point.curve <= 1.0;
}

ApplyResult applyUpsertAutomationPoint(
    SharedProjectDocument& state, const ProjectCommand& command,
    const UpsertAutomationPoint& body) {
    if (clipScopeIsDeleted(state, body.trackId, body.clipId) ||
        (!body.laneId.empty() &&
         controllerLaneIsDeleted(state, body.laneId)) ||
        automationPointIsDeleted(state, body.point.id)) {
        return reject(ApplyCode::DeletedEntity,
                      "delete wins over automation point upsert");
    }
    ClipLocation location = findClip(state.project, body.clipId);
    if (!location.clip || location.track->id != body.trackId)
        return reject(ApplyCode::MissingEntity, "automation clip does not exist");
    std::vector<AutomationPoint>* points =
        automationPointsFor(*location.clip, body.laneId);
    if (!points)
        return reject(ApplyCode::MissingEntity,
                      "automation curve or controller lane does not exist");
    if (!validAutomationPoint(body.point))
        return reject(ApplyCode::InvalidCommand, "invalid automation point");
    if (liveAutomationPointExistsOutside(
            state.project, body.point.id, body.clipId, body.laneId)) {
        return reject(ApplyCode::InvalidCommand,
                      "automation point id belongs to another curve");
    }

    std::vector<AutomationPoint> candidate = *points;
    const std::size_t existing = entityIndexOf(candidate, body.point.id);
    const bool inserted = existing == std::string::npos;
    AutomationPoint before;
    std::string beforeAfter;
    if (!inserted) {
        before = candidate[existing];
        beforeAfter = existing == 0 ? std::string() : candidate[existing - 1].id;
        candidate.erase(candidate.begin() + std::ptrdiff_t(existing));
    }
    if (!validEntityAnchor(candidate, body.afterId))
        return reject(ApplyCode::MissingAnchor,
                      "automation point anchor does not exist");
    if (!pointPlacementIsChronological(candidate, body.afterId,
                                       body.point.beats)) {
        return reject(ApplyCode::InvalidCommand,
                      "automation point anchor violates beat ordering");
    }
    insertEntityAfter(candidate, body.point, body.afterId, false);
    const bool same = candidate == *points;
    *points = std::move(candidate);

    ApplyResult result;
    result.code = same ? ApplyCode::NoChange : ApplyCode::Applied;
    result.impact.documentChanged = !same;
    result.impact.timelineChanged = !same;
    result.impact.trackIds.insert(body.trackId);
    result.impact.clipIds.insert(body.clipId);
    result.impact.automationPointIds.insert(body.point.id);
    markCommandWriters(state, command, result.impact);
    if (!same) {
        ProjectCommand inverse = inserted
            ? inverseShell(command, DeleteAutomationPoint{
                                        body.trackId, body.clipId, body.laneId,
                                        body.point.id})
            : inverseShell(command, UpsertAutomationPoint{
                                        body.trackId, body.clipId, body.laneId,
                                        before, beforeAfter});
        const std::string descendantsKey =
            ProjectReducer::clipDescendantsKey(body.clipId);
        for (const std::string& field : commandTouchedFields(command)) {
            if (field == descendantsKey) continue;
            inverse.conditions.push_back(
                FieldWriterIs{field, command.meta.operationId});
        }
        result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    }
    return result;
}

ApplyResult applyDeleteAutomationPoint(
    SharedProjectDocument& state, const ProjectCommand& command,
    const DeleteAutomationPoint& body) {
    if (state.deletedAutomationPoints.contains(body.pointId)) {
        ApplyResult result;
        result.code = ApplyCode::NoChange;
        result.message = "automation point is already deleted";
        markCommandWriters(state, command, result.impact);
        return result;
    }
    if (clipScopeIsDeleted(state, body.trackId, body.clipId) ||
        (!body.laneId.empty() &&
         controllerLaneIsDeleted(state, body.laneId)) ||
        automationPointIsDeleted(state, body.pointId)) {
        return reject(ApplyCode::DeletedEntity,
                      "parent delete wins over automation point delete");
    }
    ClipLocation location = findClip(state.project, body.clipId);
    if (!location.clip || location.track->id != body.trackId)
        return reject(ApplyCode::MissingEntity, "automation clip does not exist");
    std::vector<AutomationPoint>* points =
        automationPointsFor(*location.clip, body.laneId);
    if (!points)
        return reject(ApplyCode::MissingEntity,
                      "automation curve or controller lane does not exist");
    const std::size_t index = entityIndexOf(*points, body.pointId);
    if (index == std::string::npos)
        return reject(ApplyCode::MissingEntity,
                      "automation point does not exist");
    AutomationPointTombstone tombstone;
    tombstone.trackId = body.trackId;
    tombstone.clipId = body.clipId;
    tombstone.laneId = body.laneId;
    tombstone.point = (*points)[index];
    tombstone.afterId = index == 0 ? std::string() : (*points)[index - 1].id;
    tombstone.deleteOperationId = command.meta.operationId;
    tombstone.deleteServerSequence = command.meta.serverSequence;
    points->erase(points->begin() + std::ptrdiff_t(index));
    state.deletedAutomationPoints[body.pointId] = std::move(tombstone);

    ApplyResult result;
    result.code = ApplyCode::Applied;
    result.impact.documentChanged = true;
    result.impact.timelineChanged = true;
    result.impact.trackIds.insert(body.trackId);
    result.impact.clipIds.insert(body.clipId);
    result.impact.automationPointIds.insert(body.pointId);
    markCommandWriters(state, command, result.impact);
    ProjectCommand inverse = inverseShell(
        command, RestoreAutomationPoint{
                     body.trackId, body.clipId, body.laneId, body.pointId,
                     command.meta.operationId});
    inverse.conditions.push_back(FieldWriterIs{
        ProjectReducer::automationPointLifecycleKey(body.pointId),
        command.meta.operationId});
    result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    return result;
}

ApplyResult applyRestoreAutomationPoint(
    SharedProjectDocument& state, const ProjectCommand& command,
    const RestoreAutomationPoint& body) {
    auto found = state.deletedAutomationPoints.find(body.pointId);
    if (found == state.deletedAutomationPoints.end() ||
        found->second.deleteOperationId != body.deleteOperationId ||
        found->second.trackId != body.trackId ||
        found->second.clipId != body.clipId ||
        found->second.laneId != body.laneId) {
        return reject(ApplyCode::PreconditionsFailed,
                      "matching automation point tombstone does not exist");
    }
    if (clipScopeIsDeleted(state, body.trackId, body.clipId) ||
        (!body.laneId.empty() &&
         controllerLaneIsDeleted(state, body.laneId))) {
        return reject(ApplyCode::DeletedEntity,
                      "delete wins over automation point restore");
    }
    ClipLocation location = findClip(state.project, body.clipId);
    if (!location.clip || location.track->id != body.trackId)
        return reject(ApplyCode::MissingEntity, "automation clip does not exist");
    std::vector<AutomationPoint>* points =
        automationPointsFor(*location.clip, body.laneId);
    if (!points)
        return reject(ApplyCode::MissingEntity,
                      "automation curve or controller lane does not exist");
    if (liveAutomationPointExistsOutside(
            state.project, body.pointId, body.clipId, body.laneId) ||
        entityIndexOf(*points, body.pointId) != std::string::npos) {
        return reject(ApplyCode::InvalidCommand,
                      "automation point already exists");
    }
    AutomationPointTombstone tombstone = found->second;
    if (validEntityAnchor(*points, tombstone.afterId) &&
        pointPlacementIsChronological(*points, tombstone.afterId,
                                      tombstone.point.beats)) {
        insertEntityAfter(*points, tombstone.point, tombstone.afterId, false);
    } else {
        const auto at = std::upper_bound(
            points->begin(), points->end(), tombstone.point.beats,
            [](double beats, const AutomationPoint& point) {
                return beats < point.beats;
            });
        points->insert(at, tombstone.point);
    }
    state.deletedAutomationPoints.erase(found);

    ApplyResult result;
    result.code = ApplyCode::Applied;
    result.impact.documentChanged = true;
    result.impact.timelineChanged = true;
    result.impact.trackIds.insert(body.trackId);
    result.impact.clipIds.insert(body.clipId);
    result.impact.automationPointIds.insert(body.pointId);
    markCommandWriters(state, command, result.impact);
    ProjectCommand inverse = inverseShell(
        command, DeleteAutomationPoint{
                     body.trackId, body.clipId, body.laneId, body.pointId});
    inverse.conditions.push_back(FieldWriterIs{
        ProjectReducer::automationPointLifecycleKey(body.pointId),
        command.meta.operationId});
    result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    return result;
}

ApplyResult applyAddControllerLane(SharedProjectDocument& state,
                                   const ProjectCommand& command,
                                   const AddControllerLane& body) {
    if (clipScopeIsDeleted(state, body.trackId, body.clipId) ||
        controllerLaneIsDeleted(state, body.laneId)) {
        return reject(ApplyCode::DeletedEntity,
                      "delete wins over controller lane add");
    }
    ClipLocation location = findClip(state.project, body.clipId);
    if (!location.clip || location.track->id != body.trackId ||
        location.clip->kind != ClipKind::Midi) {
        return reject(ApplyCode::MissingEntity,
                      "MIDI clip does not exist in track");
    }
    if (body.name.size() > 4096 || !laneTargetIsValid(body.target) ||
        !std::isfinite(body.defaultValue) || body.defaultValue < 0.0 ||
        body.defaultValue > 1.0) {
        return reject(ApplyCode::InvalidCommand,
                      "invalid controller lane");
    }
    if (findControllerLane(*location.clip, body.laneId) ||
        liveControllerLaneExistsOutside(state.project, body.laneId,
                                        body.clipId)) {
        return reject(ApplyCode::InvalidCommand,
                      "controller lane id already exists");
    }
    if (!validEntityAnchor(location.clip->lanes, body.afterId))
        return reject(ApplyCode::MissingAnchor,
                      "controller lane anchor does not exist");
    ControllerLane lane;
    lane.id = body.laneId;
    lane.name = body.name;
    lane.cc = body.target.cc;
    lane.parameterId = body.target.parameterId;
    lane.slotId = body.target.slotId;
    lane.defaultValue = body.defaultValue;
    insertEntityAfter(location.clip->lanes, std::move(lane), body.afterId,
                      false);

    ApplyResult result;
    result.code = ApplyCode::Applied;
    result.impact.documentChanged = true;
    result.impact.timelineChanged = true;
    result.impact.trackIds.insert(body.trackId);
    result.impact.clipIds.insert(body.clipId);
    result.impact.controllerLaneIds.insert(body.laneId);
    markCommandWriters(state, command, result.impact);
    ProjectCommand inverse = inverseShell(
        command, DeleteControllerLane{body.trackId, body.clipId, body.laneId});
    inverse.conditions.push_back(FieldWriterIs{
        ProjectReducer::controllerLaneLifecycleKey(body.laneId),
        command.meta.operationId});
    result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    return result;
}

ApplyResult applyDeleteControllerLane(SharedProjectDocument& state,
                                      const ProjectCommand& command,
                                      const DeleteControllerLane& body) {
    if (state.deletedControllerLanes.contains(body.laneId)) {
        ApplyResult result;
        result.code = ApplyCode::NoChange;
        result.message = "controller lane is already deleted";
        markCommandWriters(state, command, result.impact);
        return result;
    }
    if (clipScopeIsDeleted(state, body.trackId, body.clipId) ||
        controllerLaneIsDeleted(state, body.laneId)) {
        return reject(ApplyCode::DeletedEntity,
                      "parent delete wins over controller lane delete");
    }
    ClipLocation location = findClip(state.project, body.clipId);
    if (!location.clip || location.track->id != body.trackId ||
        location.clip->kind != ClipKind::Midi) {
        return reject(ApplyCode::MissingEntity,
                      "MIDI clip does not exist in track");
    }
    const std::size_t index = entityIndexOf(location.clip->lanes, body.laneId);
    if (index == std::string::npos)
        return reject(ApplyCode::MissingEntity,
                      "controller lane does not exist");
    ControllerLaneTombstone tombstone;
    tombstone.trackId = body.trackId;
    tombstone.clipId = body.clipId;
    tombstone.lane = location.clip->lanes[index];
    tombstone.afterId = index == 0 ? std::string()
                                   : location.clip->lanes[index - 1].id;
    tombstone.deleteOperationId = command.meta.operationId;
    tombstone.deleteServerSequence = command.meta.serverSequence;
    location.clip->lanes.erase(location.clip->lanes.begin() +
                               std::ptrdiff_t(index));
    state.deletedControllerLanes[body.laneId] = std::move(tombstone);

    ApplyResult result;
    result.code = ApplyCode::Applied;
    result.impact.documentChanged = true;
    result.impact.timelineChanged = true;
    result.impact.trackIds.insert(body.trackId);
    result.impact.clipIds.insert(body.clipId);
    result.impact.controllerLaneIds.insert(body.laneId);
    markCommandWriters(state, command, result.impact);
    ProjectCommand inverse = inverseShell(
        command, RestoreControllerLane{body.trackId, body.clipId, body.laneId,
                                       command.meta.operationId});
    inverse.conditions.push_back(FieldWriterIs{
        ProjectReducer::controllerLaneLifecycleKey(body.laneId),
        command.meta.operationId});
    result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    return result;
}

ApplyResult applyRestoreControllerLane(SharedProjectDocument& state,
                                       const ProjectCommand& command,
                                       const RestoreControllerLane& body) {
    auto found = state.deletedControllerLanes.find(body.laneId);
    if (found == state.deletedControllerLanes.end() ||
        found->second.deleteOperationId != body.deleteOperationId ||
        found->second.trackId != body.trackId ||
        found->second.clipId != body.clipId) {
        return reject(ApplyCode::PreconditionsFailed,
                      "matching controller lane tombstone does not exist");
    }
    if (clipScopeIsDeleted(state, body.trackId, body.clipId)) {
        return reject(ApplyCode::DeletedEntity,
                      "delete wins over controller lane restore");
    }
    ClipLocation location = findClip(state.project, body.clipId);
    if (!location.clip || location.track->id != body.trackId ||
        location.clip->kind != ClipKind::Midi) {
        return reject(ApplyCode::MissingEntity,
                      "MIDI clip does not exist in track");
    }
    if (findControllerLane(*location.clip, body.laneId) ||
        liveControllerLaneExistsOutside(state.project, body.laneId,
                                        body.clipId)) {
        return reject(ApplyCode::InvalidCommand,
                      "controller lane already exists");
    }
    ControllerLaneTombstone tombstone = found->second;
    insertEntityAfter(location.clip->lanes, tombstone.lane,
                      tombstone.afterId, /*missingAnchorFallsBack=*/true);
    state.deletedControllerLanes.erase(found);

    ApplyResult result;
    result.code = ApplyCode::Applied;
    result.impact.documentChanged = true;
    result.impact.timelineChanged = true;
    result.impact.trackIds.insert(body.trackId);
    result.impact.clipIds.insert(body.clipId);
    result.impact.controllerLaneIds.insert(body.laneId);
    markCommandWriters(state, command, result.impact);
    ProjectCommand inverse = inverseShell(
        command, DeleteControllerLane{body.trackId, body.clipId, body.laneId});
    inverse.conditions.push_back(FieldWriterIs{
        ProjectReducer::controllerLaneLifecycleKey(body.laneId),
        command.meta.operationId});
    result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    return result;
}

ApplyResult applySetControllerLaneTarget(
    SharedProjectDocument& state, const ProjectCommand& command,
    const SetControllerLaneTarget& body) {
    if (clipScopeIsDeleted(state, body.trackId, body.clipId) ||
        controllerLaneIsDeleted(state, body.laneId)) {
        return reject(ApplyCode::DeletedEntity,
                      "delete wins over controller lane target edit");
    }
    ClipLocation location = findClip(state.project, body.clipId);
    ControllerLane* lane = location.clip
                               ? findControllerLane(*location.clip, body.laneId)
                               : nullptr;
    if (!lane || location.track->id != body.trackId ||
        location.clip->kind != ClipKind::Midi) {
        return reject(ApplyCode::MissingEntity,
                      "controller lane does not exist in MIDI clip");
    }
    if (!laneTargetIsValid(body.target))
        return reject(ApplyCode::InvalidCommand,
                      "invalid controller lane target");
    const ControllerLaneTarget before{lane->cc, lane->parameterId, lane->slotId};
    const bool same = before == body.target;
    lane->cc = body.target.cc;
    lane->parameterId = body.target.parameterId;
    lane->slotId = body.target.slotId;

    ApplyResult result;
    result.code = same ? ApplyCode::NoChange : ApplyCode::Applied;
    result.impact.documentChanged = !same;
    result.impact.timelineChanged = !same;
    result.impact.trackIds.insert(body.trackId);
    result.impact.clipIds.insert(body.clipId);
    result.impact.controllerLaneIds.insert(body.laneId);
    markCommandWriters(state, command, result.impact);
    if (!same) {
        ProjectCommand inverse = inverseShell(
            command, SetControllerLaneTarget{body.trackId, body.clipId,
                                             body.laneId, before});
        inverse.conditions.push_back(FieldWriterIs{
            "controllerLane:" + body.laneId + ":target",
            command.meta.operationId});
        result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    }
    return result;
}

ApplyResult applySetControllerLaneDefault(
    SharedProjectDocument& state, const ProjectCommand& command,
    const SetControllerLaneDefault& body) {
    if (clipScopeIsDeleted(state, body.trackId, body.clipId) ||
        controllerLaneIsDeleted(state, body.laneId)) {
        return reject(ApplyCode::DeletedEntity,
                      "delete wins over controller lane default edit");
    }
    ClipLocation location = findClip(state.project, body.clipId);
    ControllerLane* lane = location.clip
                               ? findControllerLane(*location.clip, body.laneId)
                               : nullptr;
    if (!lane || location.track->id != body.trackId ||
        location.clip->kind != ClipKind::Midi) {
        return reject(ApplyCode::MissingEntity,
                      "controller lane does not exist in MIDI clip");
    }
    if (!std::isfinite(body.defaultValue) || body.defaultValue < 0.0 ||
        body.defaultValue > 1.0) {
        return reject(ApplyCode::InvalidCommand,
                      "invalid controller lane default value");
    }
    const double before = lane->defaultValue;
    const bool same = before == body.defaultValue;
    lane->defaultValue = body.defaultValue;
    const std::string key = "controllerLane:" + body.laneId + ":defaultValue";
    ApplyResult result;
    result.code = same ? ApplyCode::NoChange : ApplyCode::Applied;
    result.impact.documentChanged = !same;
    result.impact.timelineChanged = !same;
    result.impact.trackIds.insert(body.trackId);
    result.impact.clipIds.insert(body.clipId);
    result.impact.controllerLaneIds.insert(body.laneId);
    markWriter(state, key, command.meta.operationId, result.impact);
    markClipDescendantsWriter(state, body.clipId, command, result.impact);
    if (!same) {
        ProjectCommand inverse = inverseShell(
            command, SetControllerLaneDefault{body.trackId, body.clipId,
                                              body.laneId, before});
        inverse.conditions.push_back(
            FieldWriterIs{key, command.meta.operationId});
        result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    }
    return result;
}

ApplyResult applySetAutomationTarget(SharedProjectDocument& state,
                                     const ProjectCommand& command,
                                     const SetAutomationTarget& body) {
    if (clipScopeIsDeleted(state, body.trackId, body.clipId))
        return reject(ApplyCode::DeletedEntity,
                      "delete wins over automation target edit");
    if (state.deletedTracks.contains(body.target.channelId))
        return reject(ApplyCode::DeletedEntity,
                      "automation target track is deleted");
    ClipLocation location = findClip(state.project, body.clipId);
    if (!location.clip || location.track->id != body.trackId ||
        location.clip->kind != ClipKind::Automation) {
        return reject(ApplyCode::MissingEntity,
                      "automation clip does not exist in track");
    }
    if (!automationTargetIsValid(state, body.target))
        return reject(ApplyCode::InvalidCommand, "invalid automation target");
    const AutomationTarget before = location.clip->automation.target;
    const bool same = before == body.target;
    location.clip->automation.target = body.target;
    const std::string key = "clip:" + body.clipId + ":automationTarget";
    ApplyResult result;
    result.code = same ? ApplyCode::NoChange : ApplyCode::Applied;
    result.impact.documentChanged = !same;
    result.impact.timelineChanged = !same;
    result.impact.trackIds.insert(body.trackId);
    result.impact.clipIds.insert(body.clipId);
    markWriter(state, key, command.meta.operationId, result.impact);
    if (!same) {
        ProjectCommand inverse = inverseShell(
            command, SetAutomationTarget{body.trackId, body.clipId, before});
        inverse.conditions.push_back(
            FieldWriterIs{key, command.meta.operationId});
        result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    }
    return result;
}

ApplyResult applySetAutomationDefault(SharedProjectDocument& state,
                                      const ProjectCommand& command,
                                      const SetAutomationDefault& body) {
    if (clipScopeIsDeleted(state, body.trackId, body.clipId))
        return reject(ApplyCode::DeletedEntity,
                      "delete wins over automation default edit");
    ClipLocation location = findClip(state.project, body.clipId);
    if (!location.clip || location.track->id != body.trackId ||
        location.clip->kind != ClipKind::Automation) {
        return reject(ApplyCode::MissingEntity,
                      "automation clip does not exist in track");
    }
    if (!std::isfinite(body.defaultValue) || body.defaultValue < 0.0 ||
        body.defaultValue > 1.0)
        return reject(ApplyCode::InvalidCommand,
                      "invalid automation default value");
    const double before = location.clip->automation.defaultValue;
    const bool same = before == body.defaultValue;
    location.clip->automation.defaultValue = body.defaultValue;
    const std::string key =
        "clip:" + body.clipId + ":automationDefaultValue";
    ApplyResult result;
    result.code = same ? ApplyCode::NoChange : ApplyCode::Applied;
    result.impact.documentChanged = !same;
    result.impact.timelineChanged = !same;
    result.impact.trackIds.insert(body.trackId);
    result.impact.clipIds.insert(body.clipId);
    markWriter(state, key, command.meta.operationId, result.impact);
    if (!same) {
        ProjectCommand inverse = inverseShell(
            command, SetAutomationDefault{body.trackId, body.clipId, before});
        inverse.conditions.push_back(
            FieldWriterIs{key, command.meta.operationId});
        result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    }
    return result;
}

ApplyResult applySetAutomationActive(SharedProjectDocument& state,
                                     const ProjectCommand& command,
                                     const SetAutomationActive& body) {
    if (clipScopeIsDeleted(state, body.trackId, body.clipId))
        return reject(ApplyCode::DeletedEntity,
                      "delete wins over automation active edit");
    ClipLocation location = findClip(state.project, body.clipId);
    if (!location.clip || location.track->id != body.trackId ||
        location.clip->kind != ClipKind::Automation) {
        return reject(ApplyCode::MissingEntity,
                      "automation clip does not exist in track");
    }
    const bool before = location.clip->automation.active;
    const bool same = before == body.active;
    location.clip->automation.active = body.active;
    const std::string key = "clip:" + body.clipId + ":automationActive";
    ApplyResult result;
    result.code = same ? ApplyCode::NoChange : ApplyCode::Applied;
    result.impact.documentChanged = !same;
    result.impact.timelineChanged = !same;
    result.impact.trackIds.insert(body.trackId);
    result.impact.clipIds.insert(body.clipId);
    markWriter(state, key, command.meta.operationId, result.impact);
    if (!same) {
        ProjectCommand inverse = inverseShell(
            command, SetAutomationActive{body.trackId, body.clipId, before});
        inverse.conditions.push_back(
            FieldWriterIs{key, command.meta.operationId});
        result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    }
    return result;
}

ApplyResult applyAddTake(SharedProjectDocument& state,
                         const ProjectCommand& command,
                         const AddTake& body) {
    if (clipScopeIsDeleted(state, body.trackId, body.clipId) ||
        takeIsDeleted(state, body.take.id)) {
        return reject(ApplyCode::DeletedEntity, "delete wins over take add");
    }
    ClipLocation location = findClip(state.project, body.clipId);
    if (!location.clip || location.track->id != body.trackId ||
        location.clip->kind != ClipKind::Audio) {
        return reject(ApplyCode::MissingEntity,
                      "audio clip does not exist in track");
    }
    if (!completeAudioTake(body.take))
        return reject(ApplyCode::InvalidCommand,
                      "take requires a complete cloud audio AssetRef");
    if (entityIndexOf(location.clip->takes, body.take.id) != std::string::npos ||
        liveTakeExistsOutside(state.project, body.take.id, body.clipId)) {
        return reject(ApplyCode::InvalidCommand, "take id already exists");
    }
    if (!validEntityAnchor(location.clip->takes, body.afterId))
        return reject(ApplyCode::MissingAnchor, "take anchor does not exist");
    insertEntityAfter(location.clip->takes, body.take, body.afterId, false);

    ApplyResult result;
    result.code = ApplyCode::Applied;
    result.impact.documentChanged = true;
    result.impact.graphRebuild = true;
    result.impact.timelineChanged = true;
    result.impact.trackIds.insert(body.trackId);
    result.impact.clipIds.insert(body.clipId);
    result.impact.takeIds.insert(body.take.id);
    markCommandWriters(state, command, result.impact);
    ProjectCommand inverse = inverseShell(
        command, DeleteTake{body.trackId, body.clipId, body.take.id});
    const std::string descendants =
        ProjectReducer::clipDescendantsKey(body.clipId);
    for (const auto& field : commandTouchedFields(command)) {
        if (field != descendants) {
            inverse.conditions.push_back(
                FieldWriterIs{field, command.meta.operationId});
        }
    }
    result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    return result;
}

ApplyResult applyDeleteTake(SharedProjectDocument& state,
                            const ProjectCommand& command,
                            const DeleteTake& body) {
    if (state.deletedTakes.contains(body.takeId)) {
        ApplyResult result;
        result.code = ApplyCode::NoChange;
        result.message = "take is already deleted";
        markCommandWriters(state, command, result.impact);
        return result;
    }
    if (clipScopeIsDeleted(state, body.trackId, body.clipId) ||
        takeIsDeleted(state, body.takeId)) {
        return reject(ApplyCode::DeletedEntity,
                      "parent delete wins over take delete");
    }
    ClipLocation location = findClip(state.project, body.clipId);
    if (!location.clip || location.track->id != body.trackId ||
        location.clip->kind != ClipKind::Audio) {
        return reject(ApplyCode::MissingEntity,
                      "audio clip does not exist in track");
    }
    const std::size_t index = entityIndexOf(location.clip->takes, body.takeId);
    if (index == std::string::npos)
        return reject(ApplyCode::MissingEntity, "take does not exist");
    // Keep stable comp references dangling. The engine projection treats a
    // segment whose take is tombstoned as silence; restoring the take revives
    // the same comp without an undeclared comp mutation. This also lets the
    // generic server authorize the delete from lifecycle heads alone.
    TakeTombstone tombstone;
    tombstone.trackId = body.trackId;
    tombstone.clipId = body.clipId;
    tombstone.take = location.clip->takes[index];
    tombstone.afterId = index == 0 ? std::string()
                                   : location.clip->takes[index - 1].id;
    tombstone.deleteOperationId = command.meta.operationId;
    tombstone.deleteServerSequence = command.meta.serverSequence;
    location.clip->takes.erase(location.clip->takes.begin() +
                               std::ptrdiff_t(index));
    state.deletedTakes[body.takeId] = std::move(tombstone);

    ApplyResult result;
    result.code = ApplyCode::Applied;
    result.impact.documentChanged = true;
    result.impact.graphRebuild = true;
    result.impact.timelineChanged = true;
    result.impact.trackIds.insert(body.trackId);
    result.impact.clipIds.insert(body.clipId);
    result.impact.takeIds.insert(body.takeId);
    markCommandWriters(state, command, result.impact);
    ProjectCommand inverse = inverseShell(
        command, RestoreTake{body.trackId, body.clipId, body.takeId,
                             command.meta.operationId});
    inverse.conditions.push_back(FieldWriterIs{
        ProjectReducer::takeLifecycleKey(body.takeId),
        command.meta.operationId});
    result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    return result;
}

ApplyResult applyRestoreTake(SharedProjectDocument& state,
                             const ProjectCommand& command,
                             const RestoreTake& body) {
    auto found = state.deletedTakes.find(body.takeId);
    if (found == state.deletedTakes.end() ||
        found->second.deleteOperationId != body.deleteOperationId ||
        found->second.trackId != body.trackId ||
        found->second.clipId != body.clipId) {
        return reject(ApplyCode::PreconditionsFailed,
                      "matching take tombstone does not exist");
    }
    if (clipScopeIsDeleted(state, body.trackId, body.clipId))
        return reject(ApplyCode::DeletedEntity,
                      "delete wins over take restore");
    ClipLocation location = findClip(state.project, body.clipId);
    if (!location.clip || location.track->id != body.trackId ||
        location.clip->kind != ClipKind::Audio) {
        return reject(ApplyCode::MissingEntity,
                      "audio clip does not exist in track");
    }
    if (entityIndexOf(location.clip->takes, body.takeId) != std::string::npos ||
        liveTakeExistsOutside(state.project, body.takeId, body.clipId)) {
        return reject(ApplyCode::InvalidCommand, "take already exists");
    }
    TakeTombstone tombstone = found->second;
    insertEntityAfter(location.clip->takes, tombstone.take, tombstone.afterId,
                      /*missingAnchorFallsBack=*/true);
    state.deletedTakes.erase(found);
    ApplyResult result;
    result.code = ApplyCode::Applied;
    result.impact.documentChanged = true;
    result.impact.graphRebuild = true;
    result.impact.timelineChanged = true;
    result.impact.trackIds.insert(body.trackId);
    result.impact.clipIds.insert(body.clipId);
    result.impact.takeIds.insert(body.takeId);
    markCommandWriters(state, command, result.impact);
    ProjectCommand inverse = inverseShell(
        command, DeleteTake{body.trackId, body.clipId, body.takeId});
    const std::string descendants =
        ProjectReducer::clipDescendantsKey(body.clipId);
    for (const auto& field : commandTouchedFields(command)) {
        if (field != descendants) {
            inverse.conditions.push_back(
                FieldWriterIs{field, command.meta.operationId});
        }
    }
    result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    return result;
}

ApplyResult applyMoveTake(SharedProjectDocument& state,
                          const ProjectCommand& command,
                          const MoveTake& body) {
    if (clipScopeIsDeleted(state, body.trackId, body.clipId) ||
        takeIsDeleted(state, body.takeId)) {
        return reject(ApplyCode::DeletedEntity, "delete wins over take move");
    }
    ClipLocation location = findClip(state.project, body.clipId);
    if (!location.clip || location.track->id != body.trackId ||
        location.clip->kind != ClipKind::Audio) {
        return reject(ApplyCode::MissingEntity,
                      "audio clip does not exist in track");
    }
    const std::size_t index = entityIndexOf(location.clip->takes, body.takeId);
    if (index == std::string::npos)
        return reject(ApplyCode::MissingEntity, "take does not exist");
    if (!validEntityAnchor(location.clip->takes, body.afterId, body.takeId))
        return reject(ApplyCode::MissingAnchor, "take anchor does not exist");

    const std::string before =
        entityPredecessor(location.clip->takes, body.takeId);
    const bool same = before == body.afterId;
    if (!same) {
        TakeModel moved = std::move(location.clip->takes[index]);
        location.clip->takes.erase(location.clip->takes.begin() +
                                   std::ptrdiff_t(index));
        insertEntityAfter(location.clip->takes, std::move(moved),
                          body.afterId, false);
    }

    ApplyResult result;
    result.code = same ? ApplyCode::NoChange : ApplyCode::Applied;
    result.impact.documentChanged = !same;
    result.impact.graphRebuild = !same;
    result.impact.timelineChanged = !same;
    result.impact.trackIds.insert(body.trackId);
    result.impact.clipIds.insert(body.clipId);
    result.impact.takeIds.insert(body.takeId);
    const std::string key = ProjectReducer::takePositionKey(body.takeId);
    markWriter(state, key, command.meta.operationId, result.impact);
    if (!same) {
        ProjectCommand inverse = inverseShell(
            command,
            MoveTake{body.trackId, body.clipId, body.takeId, before});
        inverse.conditions.push_back(
            FieldWriterIs{key, command.meta.operationId});
        result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    }
    return result;
}

ApplyResult applySetTakeProperty(SharedProjectDocument& state,
                                 const ProjectCommand& command,
                                 const SetTakeProperty& body) {
    if (clipScopeIsDeleted(state, body.trackId, body.clipId) ||
        takeIsDeleted(state, body.takeId)) {
        return reject(ApplyCode::DeletedEntity,
                      "delete wins over take property edit");
    }
    ClipLocation location = findClip(state.project, body.clipId);
    if (!location.clip || location.track->id != body.trackId ||
        location.clip->kind != ClipKind::Audio) {
        return reject(ApplyCode::MissingEntity,
                      "audio clip does not exist in track");
    }
    const std::size_t index = entityIndexOf(location.clip->takes, body.takeId);
    if (index == std::string::npos)
        return reject(ApplyCode::MissingEntity, "take does not exist");
    TakeModel& take = location.clip->takes[index];
    ScalarValue before;
    bool same = false;
    switch (body.property) {
        case TakeProperty::Name: {
            const auto* value = std::get_if<std::string>(&body.value);
            if (!value || value->size() > 4096)
                return reject(ApplyCode::InvalidCommand, "invalid take name");
            before = take.name;
            same = take.name == *value;
            take.name = *value;
            break;
        }
        case TakeProperty::OffsetSeconds:
        case TakeProperty::LengthSeconds:
        case TakeProperty::ClipOffsetSeconds: {
            double value = 0.0;
            if (!doubleValue(body.value, value) || value < 0.0)
                return reject(ApplyCode::InvalidCommand,
                              "invalid take time value");
            double* target = body.property == TakeProperty::OffsetSeconds
                                 ? &take.offsetSeconds
                                 : (body.property == TakeProperty::LengthSeconds
                                        ? &take.lengthSeconds
                                        : &take.clipOffsetSeconds);
            before = *target;
            same = *target == value;
            *target = value;
            break;
        }
        case TakeProperty::Gain: {
            double value = 0.0;
            if (!doubleValue(body.value, value) || value < 0.0 || value > 4.0)
                return reject(ApplyCode::InvalidCommand, "invalid take gain");
            before = double(take.gain);
            same = take.gain == float(value);
            take.gain = float(value);
            break;
        }
        case TakeProperty::Muted: {
            const auto* value = std::get_if<bool>(&body.value);
            if (!value)
                return reject(ApplyCode::InvalidCommand,
                              "invalid take mute flag");
            before = take.muted;
            same = take.muted == *value;
            take.muted = *value;
            break;
        }
        case TakeProperty::Color: {
            std::int64_t value = 0;
            if (!integerValue(body.value, value) || value < 0 ||
                value > std::numeric_limits<std::uint32_t>::max()) {
                return reject(ApplyCode::InvalidCommand,
                              "invalid take color");
            }
            before = std::int64_t(take.color);
            same = take.color == std::uint32_t(value);
            take.color = std::uint32_t(value);
            break;
        }
    }
    const std::string key = "take:" + body.takeId + ":" +
                            takePropertyName(body.property);
    ApplyResult result;
    result.code = same ? ApplyCode::NoChange : ApplyCode::Applied;
    result.impact.documentChanged = !same;
    result.impact.graphRebuild =
        !same && body.property != TakeProperty::Name &&
        body.property != TakeProperty::Color;
    result.impact.timelineChanged = !same;
    result.impact.trackIds.insert(body.trackId);
    result.impact.clipIds.insert(body.clipId);
    result.impact.takeIds.insert(body.takeId);
    markCommandWriters(state, command, result.impact);
    if (!same) {
        ProjectCommand inverse = inverseShell(
            command, SetTakeProperty{body.trackId, body.clipId, body.takeId,
                                     body.property, before});
        inverse.conditions.push_back(FieldWriterIs{key,
                                                    command.meta.operationId});
        result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    }
    return result;
}

bool validCompSegment(const ClipModel& clip, const CompSegment& segment) {
    return std::isfinite(segment.startSeconds) &&
           std::isfinite(segment.endSeconds) &&
           segment.startSeconds >= 0.0 &&
           segment.endSeconds - segment.startSeconds >= 0.001 &&
           (clip.durationSeconds <= 0.0 ||
            segment.endSeconds <= clip.durationSeconds);
}

ApplyResult applyUpsertCompSegment(SharedProjectDocument& state,
                                   const ProjectCommand& command,
                                   const UpsertCompSegment& body) {
    if (clipScopeIsDeleted(state, body.trackId, body.clipId) ||
        compSegmentIsDeleted(state, body.segment.id)) {
        return reject(ApplyCode::DeletedEntity,
                      "delete wins over comp segment upsert");
    }
    ClipLocation location = findClip(state.project, body.clipId);
    if (!location.clip || location.track->id != body.trackId ||
        location.clip->kind != ClipKind::Audio) {
        return reject(ApplyCode::MissingEntity,
                      "audio clip does not exist in track");
    }
    if (!findTake(*location.clip, body.segment.takeId))
        return takeIsDeleted(state, body.segment.takeId)
                   ? reject(ApplyCode::DeletedEntity,
                            "comp segment take is deleted")
                   : reject(ApplyCode::MissingEntity,
                            "comp segment take does not exist");
    if (!validCompSegment(*location.clip, body.segment))
        return reject(ApplyCode::InvalidCommand, "invalid comp segment range");
    if (liveCompSegmentExistsOutside(state.project, body.segment.id,
                                     body.clipId)) {
        return reject(ApplyCode::InvalidCommand,
                      "comp segment id belongs to another clip");
    }

    std::vector<CompSegment> candidate = location.clip->comp;
    const std::size_t existing = entityIndexOf(candidate, body.segment.id);
    const bool inserted = existing == std::string::npos;
    CompSegment before;
    std::string beforeAfter;
    if (!inserted) {
        before = candidate[existing];
        beforeAfter = existing == 0 ? std::string()
                                    : candidate[existing - 1].id;
        candidate.erase(candidate.begin() + std::ptrdiff_t(existing));
    }
    if (!validEntityAnchor(candidate, body.afterId))
        return reject(ApplyCode::MissingAnchor,
                      "comp segment anchor does not exist");
    if (!compPlacementIsValid(candidate, body.afterId, body.segment))
        return reject(ApplyCode::InvalidCommand,
                      "comp segment overlaps or violates time ordering");
    insertEntityAfter(candidate, body.segment, body.afterId, false);
    const bool same = compVectorsEqual(candidate, location.clip->comp);
    location.clip->comp = std::move(candidate);

    ApplyResult result;
    result.code = same ? ApplyCode::NoChange : ApplyCode::Applied;
    result.impact.documentChanged = !same;
    result.impact.graphRebuild = !same;
    result.impact.timelineChanged = !same;
    result.impact.trackIds.insert(body.trackId);
    result.impact.clipIds.insert(body.clipId);
    result.impact.compSegmentIds.insert(body.segment.id);
    markCommandWriters(state, command, result.impact);
    if (!same) {
        ProjectCommand inverse = inserted
            ? inverseShell(command, DeleteCompSegment{
                                        body.trackId, body.clipId,
                                        body.segment.id})
            : inverseShell(command, UpsertCompSegment{
                                        body.trackId, body.clipId, before,
                                        beforeAfter});
        const std::string descendantsKey =
            ProjectReducer::clipDescendantsKey(body.clipId);
        for (const std::string& field : commandTouchedFields(command)) {
            if (field == descendantsKey) continue;
            inverse.conditions.push_back(
                FieldWriterIs{field, command.meta.operationId});
        }
        result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    }
    return result;
}

ApplyResult applyDeleteCompSegment(SharedProjectDocument& state,
                                   const ProjectCommand& command,
                                   const DeleteCompSegment& body) {
    if (state.deletedCompSegments.contains(body.segmentId)) {
        ApplyResult result;
        result.code = ApplyCode::NoChange;
        result.message = "comp segment is already deleted";
        markCommandWriters(state, command, result.impact);
        return result;
    }
    if (clipScopeIsDeleted(state, body.trackId, body.clipId) ||
        compSegmentIsDeleted(state, body.segmentId)) {
        return reject(ApplyCode::DeletedEntity,
                      "parent delete wins over comp segment delete");
    }
    ClipLocation location = findClip(state.project, body.clipId);
    if (!location.clip || location.track->id != body.trackId ||
        location.clip->kind != ClipKind::Audio) {
        return reject(ApplyCode::MissingEntity,
                      "audio clip does not exist in track");
    }
    const std::size_t index = entityIndexOf(location.clip->comp, body.segmentId);
    if (index == std::string::npos)
        return reject(ApplyCode::MissingEntity,
                      "comp segment does not exist");
    CompSegmentTombstone tombstone;
    tombstone.trackId = body.trackId;
    tombstone.clipId = body.clipId;
    tombstone.segment = location.clip->comp[index];
    tombstone.afterId = index == 0 ? std::string()
                                   : location.clip->comp[index - 1].id;
    tombstone.deleteOperationId = command.meta.operationId;
    tombstone.deleteServerSequence = command.meta.serverSequence;
    location.clip->comp.erase(location.clip->comp.begin() +
                              std::ptrdiff_t(index));
    state.deletedCompSegments[body.segmentId] = std::move(tombstone);
    ApplyResult result;
    result.code = ApplyCode::Applied;
    result.impact.documentChanged = true;
    result.impact.graphRebuild = true;
    result.impact.timelineChanged = true;
    result.impact.trackIds.insert(body.trackId);
    result.impact.clipIds.insert(body.clipId);
    result.impact.compSegmentIds.insert(body.segmentId);
    markCommandWriters(state, command, result.impact);
    ProjectCommand inverse = inverseShell(
        command, RestoreCompSegment{body.trackId, body.clipId, body.segmentId,
                                    command.meta.operationId});
    inverse.conditions.push_back(FieldWriterIs{
        ProjectReducer::compSegmentLifecycleKey(body.segmentId),
        command.meta.operationId});
    result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    return result;
}

ApplyResult applyRestoreCompSegment(SharedProjectDocument& state,
                                    const ProjectCommand& command,
                                    const RestoreCompSegment& body) {
    auto found = state.deletedCompSegments.find(body.segmentId);
    if (found == state.deletedCompSegments.end() ||
        found->second.deleteOperationId != body.deleteOperationId ||
        found->second.trackId != body.trackId ||
        found->second.clipId != body.clipId) {
        return reject(ApplyCode::PreconditionsFailed,
                      "matching comp segment tombstone does not exist");
    }
    if (clipScopeIsDeleted(state, body.trackId, body.clipId))
        return reject(ApplyCode::DeletedEntity,
                      "delete wins over comp segment restore");
    ClipLocation location = findClip(state.project, body.clipId);
    if (!location.clip || location.track->id != body.trackId ||
        location.clip->kind != ClipKind::Audio) {
        return reject(ApplyCode::MissingEntity,
                      "audio clip does not exist in track");
    }
    CompSegmentTombstone tombstone = found->second;
    if (!findTake(*location.clip, tombstone.segment.takeId))
        return reject(ApplyCode::DeletedEntity,
                      "comp segment take is deleted");
    if (entityIndexOf(location.clip->comp, body.segmentId) !=
            std::string::npos ||
        liveCompSegmentExistsOutside(state.project, body.segmentId,
                                     body.clipId)) {
        return reject(ApplyCode::InvalidCommand,
                      "comp segment already exists");
    }
    std::string afterId = tombstone.afterId;
    if (!validEntityAnchor(location.clip->comp, afterId) ||
        !compPlacementIsValid(location.clip->comp, afterId,
                              tombstone.segment)) {
        const auto insertion = std::lower_bound(
            location.clip->comp.begin(), location.clip->comp.end(),
            tombstone.segment.startSeconds,
            [](const CompSegment& segment, double start) {
                return segment.startSeconds < start;
            });
        afterId = insertion == location.clip->comp.begin()
                      ? std::string()
                      : std::prev(insertion)->id;
        if (!compPlacementIsValid(location.clip->comp, afterId,
                                  tombstone.segment)) {
            return reject(ApplyCode::PreconditionsFailed,
                          "comp segment restore would overlap current comp");
        }
    }
    insertEntityAfter(location.clip->comp, tombstone.segment, afterId, false);
    state.deletedCompSegments.erase(found);
    ApplyResult result;
    result.code = ApplyCode::Applied;
    result.impact.documentChanged = true;
    result.impact.graphRebuild = true;
    result.impact.timelineChanged = true;
    result.impact.trackIds.insert(body.trackId);
    result.impact.clipIds.insert(body.clipId);
    result.impact.compSegmentIds.insert(body.segmentId);
    markCommandWriters(state, command, result.impact);
    ProjectCommand inverse = inverseShell(
        command, DeleteCompSegment{body.trackId, body.clipId, body.segmentId});
    inverse.conditions.push_back(FieldWriterIs{
        ProjectReducer::compSegmentLifecycleKey(body.segmentId),
        command.meta.operationId});
    result.inverse = std::make_shared<ProjectCommand>(std::move(inverse));
    return result;
}

ApplyResult applyBatch(SharedProjectDocument& state,
                       const ProjectCommand& command,
                       const std::shared_ptr<BatchCommand>& batch) {
    if (!batch || batch->commands.empty())
        return reject(ApplyCode::InvalidCommand, "batch is empty");
    if (batch->commands.size() > kMaxProjectCommandBatchSize)
        return reject(ApplyCode::InvalidCommand, "batch is too large");
    SharedProjectDocument candidate = state;
    ChangeImpact impact;
    std::vector<ProjectCommand> inverses;
    inverses.reserve(batch->commands.size());
    bool anyChanged = false;
    std::vector<ProjectCommand> children;
    children.reserve(batch->commands.size());
    for (std::size_t i = 0; i < batch->commands.size(); ++i) {
        ProjectCommand child = batch->commands[i];
        if (std::holds_alternative<std::shared_ptr<BatchCommand>>(child.body) ||
            std::holds_alternative<RecordingCommit>(child.body)) {
            return reject(ApplyCode::Unsupported,
                          "nested transactions are not supported");
        }
        child.meta.schemaVersion = command.meta.schemaVersion;
        child.meta.projectId = command.meta.projectId;
        child.meta.actorId = command.meta.actorId;
        child.meta.clientId = command.meta.clientId;
        child.meta.clientSequence = command.meta.clientSequence;
        child.meta.baseServerSequence = command.meta.baseServerSequence;
        child.meta.serverSequence = command.meta.serverSequence;
        child.meta.transactionId = command.meta.operationId;
        child.meta.operationId = command.meta.operationId;
        std::string conditionError;
        if (!checkConditions(state, child, conditionError)) {
            return reject(ApplyCode::PreconditionsFailed,
                          "batch child " + std::to_string(i) + ": " +
                              conditionError);
        }
        children.push_back(std::move(child));
    }
    for (std::size_t i = 0; i < children.size(); ++i) {
        ProjectCommand child = std::move(children[i]);
        // All guards were evaluated against the same pre-transaction state.
        // Clearing them here lets two reversed inverse children touch the same
        // field while every resulting writer remains the durable outer op id.
        child.conditions.clear();
        ApplyResult childResult = applyImpl(candidate, child, false, false);
        if (!childResult.accepted()) {
            childResult.message = "batch child " + std::to_string(i) + ": " +
                                  childResult.message;
            return childResult;
        }
        anyChanged = anyChanged || childResult.changed();
        impact.merge(childResult.impact);
        if (childResult.inverse) inverses.push_back(*childResult.inverse);
    }
    std::reverse(inverses.begin(), inverses.end());
    state = std::move(candidate);
    ApplyResult result;
    result.code = anyChanged ? ApplyCode::Applied : ApplyCode::NoChange;
    result.impact = std::move(impact);
    // recording.commit owns a server-authoritative per-leased-track landing
    // head in addition to its child fields. Materialize that outer metadata on
    // clients too, including for a take-only commit, so snapshot hashes and
    // later conditional state stay in lockstep with the Go authority.
    if (std::holds_alternative<RecordingCommit>(command.body))
        markCommandWriters(state, command, result.impact);
    if (!inverses.empty()) {
        auto inverseBatch = std::make_shared<BatchCommand>();
        inverseBatch->commands = std::move(inverses);
        result.inverse = std::make_shared<ProjectCommand>(
            inverseShell(command, std::move(inverseBatch)));
    }
    return result;
}

ApplyResult applyImpl(SharedProjectDocument& state,
                      const ProjectCommand& command, bool allowBatch,
                      bool recordOperation) {
    if (command.meta.schemaVersion != kProjectCommandSchemaVersion)
        return reject(ApplyCode::Unsupported, "unsupported command schema version");
    if (command.meta.operationId.empty())
        return reject(ApplyCode::InvalidCommand, "operation id is required");
    if (recordOperation &&
        state.appliedOperationIds.contains(command.meta.operationId)) {
        ApplyResult result;
        result.code = ApplyCode::Duplicate;
        result.message = "operation already applied";
        return result;
    }
    std::string conditionError;
    if (!checkConditions(state, command, conditionError))
        return reject(ApplyCode::PreconditionsFailed, std::move(conditionError));

    ApplyResult result = std::visit([&](const auto& body) -> ApplyResult {
        using T = std::decay_t<decltype(body)>;
        if constexpr (std::is_same_v<T, SetProjectScalar>)
            return applyScalar(state, command, body);
        else if constexpr (std::is_same_v<T, SetTimeSignature>)
            return applyTimeSignature(state, command, body);
        else if constexpr (std::is_same_v<T, SetProjectKey>)
            return applyProjectKey(state, command, body);
        else if constexpr (std::is_same_v<T, AddTrack>)
            return applyAddTrack(state, command, body);
        else if constexpr (std::is_same_v<T, DeleteTrack>)
            return applyDeleteTrack(state, command, body);
        else if constexpr (std::is_same_v<T, RestoreTrack>)
            return applyRestoreTrack(state, command, body);
        else if constexpr (std::is_same_v<T, MoveTrack>)
            return applyMoveTrack(state, command, body);
        else if constexpr (std::is_same_v<T, SetTrackProperty>)
            return applyTrackProperty(state, command, body);
        else if constexpr (std::is_same_v<T, SetTrackParent>)
            return applySetTrackParent(state, command, body);
        else if constexpr (std::is_same_v<T, SetTrackOutput>)
            return applySetTrackOutput(state, command, body);
        else if constexpr (std::is_same_v<T, AddSend>)
            return applyAddSend(state, command, body);
        else if constexpr (std::is_same_v<T, DeleteSend>)
            return applyDeleteSend(state, command, body);
        else if constexpr (std::is_same_v<T, RestoreSend>)
            return applyRestoreSend(state, command, body);
        else if constexpr (std::is_same_v<T, MoveSend>)
            return applyMoveSend(state, command, body);
        else if constexpr (std::is_same_v<T, SetSendProperty>)
            return applySetSendProperty(state, command, body);
        else if constexpr (std::is_same_v<T, AddClip>)
            return applyAddClip(state, command, body);
        else if constexpr (std::is_same_v<T, DeleteClip>)
            return applyDeleteClip(state, command, body);
        else if constexpr (std::is_same_v<T, RestoreClip>)
            return applyRestoreClip(state, command, body);
        else if constexpr (std::is_same_v<T, MoveClip>)
            return applyMoveClip(state, command, body);
        else if constexpr (std::is_same_v<T, SetClipProperty>)
            return applyClipProperty(state, command, body);
        else if constexpr (std::is_same_v<T, SetClipAsset>)
            return applySetClipAsset(state, command, body);
        else if constexpr (std::is_same_v<T, SetClipSampleEdit>)
            return applySetClipSampleEdit(state, command, body);
        else if constexpr (std::is_same_v<T, SetClipFade>)
            return applySetClipFade(state, command, body);
        else if constexpr (std::is_same_v<T, SetClipFadeCurve>)
            return applySetClipFadeCurve(state, command, body);
        else if constexpr (std::is_same_v<T, SetClipFadeMode>)
            return applySetClipFadeMode(state, command, body);
        else if constexpr (std::is_same_v<T, SetClipPatternOwner>)
            return applySetClipPatternOwner(state, command, body);
        else if constexpr (std::is_same_v<T, SetClipMusicalAnalysis>)
            return applySetClipMusicalAnalysis(state, command, body);
        else if constexpr (std::is_same_v<T, AddPluginInsert>)
            return applyAddPlugin(state, command, body);
        else if constexpr (std::is_same_v<T, DeletePluginInsert>)
            return applyDeletePlugin(state, command, body);
        else if constexpr (std::is_same_v<T, RestorePluginInsert>)
            return applyRestorePlugin(state, command, body);
        else if constexpr (std::is_same_v<T, MovePluginInsert>)
            return applyMovePlugin(state, command, body);
        else if constexpr (std::is_same_v<T, ReplacePluginInsert>)
            return applyReplacePlugin(state, command, body);
        else if constexpr (std::is_same_v<T, SetPluginProperty>)
            return applySetPluginProperty(state, command, body);
        else if constexpr (std::is_same_v<T, SetPluginState>)
            return applySetPluginState(state, command, body);
        else if constexpr (std::is_same_v<T, SetPluginParameter>)
            return applySetPluginParameter(state, command, body);
        else if constexpr (std::is_same_v<T, RemovePluginParameter>)
            return applyRemovePluginParameter(state, command, body);
        else if constexpr (std::is_same_v<T, SetPluginAssetBinding>)
            return applySetPluginBinding(state, command, body);
        else if constexpr (std::is_same_v<T, RemovePluginAssetBinding>)
            return applyRemovePluginBinding(state, command, body);
        else if constexpr (std::is_same_v<T, SetSamplerFxLevels>)
            return applySetSamplerFxLevels(state, command, body);
        else if constexpr (std::is_same_v<T, UpsertMidiNote>)
            return applyUpsertMidiNote(state, command, body);
        else if constexpr (std::is_same_v<T, DeleteMidiNote>)
            return applyDeleteMidiNote(state, command, body);
        else if constexpr (std::is_same_v<T, RestoreMidiNote>)
            return applyRestoreMidiNote(state, command, body);
        else if constexpr (std::is_same_v<T, UpsertAutomationPoint>)
            return applyUpsertAutomationPoint(state, command, body);
        else if constexpr (std::is_same_v<T, DeleteAutomationPoint>)
            return applyDeleteAutomationPoint(state, command, body);
        else if constexpr (std::is_same_v<T, RestoreAutomationPoint>)
            return applyRestoreAutomationPoint(state, command, body);
        else if constexpr (std::is_same_v<T, AddControllerLane>)
            return applyAddControllerLane(state, command, body);
        else if constexpr (std::is_same_v<T, DeleteControllerLane>)
            return applyDeleteControllerLane(state, command, body);
        else if constexpr (std::is_same_v<T, RestoreControllerLane>)
            return applyRestoreControllerLane(state, command, body);
        else if constexpr (std::is_same_v<T, SetControllerLaneTarget>)
            return applySetControllerLaneTarget(state, command, body);
        else if constexpr (std::is_same_v<T, SetControllerLaneDefault>)
            return applySetControllerLaneDefault(state, command, body);
        else if constexpr (std::is_same_v<T, SetAutomationTarget>)
            return applySetAutomationTarget(state, command, body);
        else if constexpr (std::is_same_v<T, SetAutomationDefault>)
            return applySetAutomationDefault(state, command, body);
        else if constexpr (std::is_same_v<T, SetAutomationActive>)
            return applySetAutomationActive(state, command, body);
        else if constexpr (std::is_same_v<T, AddTake>)
            return applyAddTake(state, command, body);
        else if constexpr (std::is_same_v<T, DeleteTake>)
            return applyDeleteTake(state, command, body);
        else if constexpr (std::is_same_v<T, RestoreTake>)
            return applyRestoreTake(state, command, body);
        else if constexpr (std::is_same_v<T, MoveTake>)
            return applyMoveTake(state, command, body);
        else if constexpr (std::is_same_v<T, SetTakeProperty>)
            return applySetTakeProperty(state, command, body);
        else if constexpr (std::is_same_v<T, UpsertCompSegment>)
            return applyUpsertCompSegment(state, command, body);
        else if constexpr (std::is_same_v<T, DeleteCompSegment>)
            return applyDeleteCompSegment(state, command, body);
        else if constexpr (std::is_same_v<T, RestoreCompSegment>)
            return applyRestoreCompSegment(state, command, body);
        else if constexpr (std::is_same_v<T, RecordingCommit>)
            return allowBatch
                       ? applyBatch(state, command, body.batch)
                       : reject(ApplyCode::Unsupported,
                                "nested recording.commit is not supported");
        else if constexpr (std::is_same_v<T,
                                          std::shared_ptr<BatchCommand>>)
            return allowBatch ? applyBatch(state, command, body)
                              : reject(ApplyCode::Unsupported,
                                       "nested batches are not supported");
        else
            return reject(ApplyCode::Unsupported,
                          "command reducer is not implemented");
    }, command.body);
    // Keep reducer field-writer state exactly aligned with the public touched
    // field contract. Individual handlers mark the keys used by their inverse;
    // this final pass also covers coarse ordering/generation heads.
    if (result.accepted())
        markCommandWriters(state, command, result.impact);
    if (recordOperation && result.accepted())
        state.appliedOperationIds.insert(command.meta.operationId);
    return result;
}

} // namespace

void ChangeImpact::merge(const ChangeImpact& other) {
    fullProjection = fullProjection || other.fullProjection;
    documentChanged = documentChanged || other.documentChanged;
    graphRebuild = graphRebuild || other.graphRebuild;
    timelineChanged = timelineChanged || other.timelineChanged;
    transportProjectionChanged =
        transportProjectionChanged || other.transportProjectionChanged;
    masterGainChanged = masterGainChanged || other.masterGainChanged;
    trackIds.insert(other.trackIds.begin(), other.trackIds.end());
    clipIds.insert(other.clipIds.begin(), other.clipIds.end());
    noteIds.insert(other.noteIds.begin(), other.noteIds.end());
    automationPointIds.insert(other.automationPointIds.begin(),
                              other.automationPointIds.end());
    controllerLaneIds.insert(other.controllerLaneIds.begin(),
                             other.controllerLaneIds.end());
    takeIds.insert(other.takeIds.begin(), other.takeIds.end());
    compSegmentIds.insert(other.compSegmentIds.begin(),
                          other.compSegmentIds.end());
    sendIds.insert(other.sendIds.begin(), other.sendIds.end());
    pluginInsertIds.insert(other.pluginInsertIds.begin(),
                           other.pluginInsertIds.end());
    fieldKeys.insert(other.fieldKeys.begin(), other.fieldKeys.end());
}

ApplyResult ProjectReducer::apply(SharedProjectDocument& state,
                                  const ProjectCommand& command) {
    std::string idError;
    if (!commandHasValidIds(command, &idError))
        return reject(ApplyCode::InvalidCommand, std::move(idError));
    if (commandTouchedFields(command).size() >
        kMaxProjectCommandTouchedFields) {
        return reject(ApplyCode::InvalidCommand,
                      "command touches too many fields");
    }
    if ((std::holds_alternative<std::shared_ptr<BatchCommand>>(command.body) ||
         std::holds_alternative<RecordingCommit>(command.body)) &&
        serializedProjectCommandPayloadSize(command) >
            kMaxProjectCommandBatchBytes) {
        return reject(ApplyCode::InvalidCommand,
                      "batch command exceeds the 1 MiB payload limit");
    }
    return applyImpl(state, command, true, true);
}

ApplyResult ProjectReducer::applyPrevalidated(SharedProjectDocument& state,
                                              const ProjectCommand& command) {
    return applyImpl(state, command, true, true);
}

std::string ProjectReducer::projectFieldKey(ProjectScalar field) {
    return "project:" + projectScalarName(field);
}

std::string ProjectReducer::trackFieldKey(const std::string& trackId,
                                          TrackProperty property) {
    return "track:" + trackId + ":" + trackPropertyName(property);
}

std::string ProjectReducer::trackPositionKey(const std::string& trackId) {
    return "track:" + trackId + ":position";
}

std::string ProjectReducer::trackLifecycleKey(const std::string& trackId) {
    return "track:" + trackId + ":lifecycle";
}

std::string ProjectReducer::clipFieldKey(const std::string& clipId,
                                         ClipProperty property) {
    return "clip:" + clipId + ":" + clipPropertyName(property);
}

std::string ProjectReducer::clipPositionKey(const std::string& clipId) {
    return "clip:" + clipId + ":position";
}

std::string ProjectReducer::clipLifecycleKey(const std::string& clipId) {
    return "clip:" + clipId + ":lifecycle";
}

std::string ProjectReducer::clipDescendantsKey(const std::string& clipId) {
    return "clip:" + clipId + ":descendants";
}

std::string ProjectReducer::notePositionKey(const std::string& noteId) {
    return "note:" + noteId + ":position";
}

std::string ProjectReducer::noteLifecycleKey(const std::string& noteId) {
    return "note:" + noteId + ":lifecycle";
}

std::string ProjectReducer::automationPointPositionKey(
    const std::string& pointId) {
    return "automationPoint:" + pointId + ":position";
}

std::string ProjectReducer::automationPointLifecycleKey(
    const std::string& pointId) {
    return "automationPoint:" + pointId + ":lifecycle";
}

std::string ProjectReducer::controllerLanePositionKey(
    const std::string& laneId) {
    return "controllerLane:" + laneId + ":position";
}

std::string ProjectReducer::controllerLaneLifecycleKey(
    const std::string& laneId) {
    return "controllerLane:" + laneId + ":lifecycle";
}

std::string ProjectReducer::takePositionKey(const std::string& takeId) {
    return "take:" + takeId + ":position";
}

std::string ProjectReducer::takeLifecycleKey(const std::string& takeId) {
    return "take:" + takeId + ":lifecycle";
}

std::string ProjectReducer::compSegmentPositionKey(
    const std::string& segmentId) {
    return "compSegment:" + segmentId + ":position";
}

std::string ProjectReducer::compSegmentLifecycleKey(
    const std::string& segmentId) {
    return "compSegment:" + segmentId + ":lifecycle";
}

std::string ProjectReducer::sendPositionKey(const std::string& sendId) {
    return "send:" + sendId + ":position";
}

std::string ProjectReducer::sendLifecycleKey(const std::string& sendId) {
    return "send:" + sendId + ":lifecycle";
}

std::string ProjectReducer::pluginPositionKey(const std::string& insertId) {
    return "plugin:" + insertId + ":position";
}

std::string ProjectReducer::pluginLifecycleKey(const std::string& insertId) {
    return "plugin:" + insertId + ":lifecycle";
}

} // namespace daw::collab
