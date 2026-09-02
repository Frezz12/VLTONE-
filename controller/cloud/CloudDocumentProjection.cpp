#include "cloud/CloudDocumentProjection.hpp"

#include "collaboration/CollaborationState.hpp"

#include <algorithm>

namespace daw::cloud {
namespace {

std::string basenameOnly(const std::string& value) {
    if (value.empty())
        return {};
    std::string normalized = value;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    const std::size_t separator = normalized.find_last_of('/');
    return separator == std::string::npos ? normalized
                                          : normalized.substr(separator + 1);
}

void sanitizeAsset(AssetRef& asset) {
    asset.originalName = basenameOnly(asset.originalName);
}

void projectInsert(InsertModel& insert, const std::string& location,
                   std::vector<CloudProjectionIssue>& blockers) {
    if (!insert.stateFile.empty() && insert.stateAsset.empty()) {
        blockers.push_back({CloudProjectionIssueKind::MissingAsset,
                            location + "/state",
                            "plugin state has no completed cloud asset"});
    }
    if (!insert.rightStateFile.empty() && insert.rightStateAsset.empty()) {
        blockers.push_back({CloudProjectionIssueKind::MissingAsset,
                            location + "/right-state",
                            "dual-mono plugin state has no completed cloud asset"});
    }
    insert.path.clear();
    insert.stateFile.clear();
    insert.rightStateFile.clear();
    insert.editorChannel = PluginEditorChannel::Left;
    insert.windowX = insert.windowY = insert.windowWidth = insert.windowHeight = 0;
    insert.windowOpen = false;
    sanitizeAsset(insert.stateAsset);
    sanitizeAsset(insert.rightStateAsset);
    for (PluginAssetBinding& binding : insert.assetBindings)
        sanitizeAsset(binding.asset);
}

void projectInserts(std::vector<InsertModel>& inserts,
                    const std::string& location,
                    std::vector<CloudProjectionIssue>& blockers) {
    for (std::size_t index = 0; index < inserts.size(); ++index) {
        projectInsert(inserts[index],
                      location + "/insert:" +
                          (inserts[index].id.empty()
                               ? std::to_string(index)
                               : inserts[index].id),
                      blockers);
    }
}

bool inspectInsertForLeak(const InsertModel& insert,
                          const std::string& location,
                          std::string* firstLocation) {
    if (!insert.path.empty() || !insert.stateFile.empty() ||
        !insert.rightStateFile.empty() || insert.windowOpen ||
        insert.windowX != 0 || insert.windowY != 0 ||
        insert.windowWidth != 0 || insert.windowHeight != 0 ||
        insert.editorChannel != PluginEditorChannel::Left) {
        if (firstLocation) *firstLocation = location;
        return true;
    }
    return false;
}

bool inspectInsertsForLeak(const std::vector<InsertModel>& inserts,
                           const std::string& location,
                           std::string* firstLocation) {
    for (std::size_t index = 0; index < inserts.size(); ++index) {
        if (inspectInsertForLeak(inserts[index],
                                 location + "/insert:" +
                                     std::to_string(index),
                                 firstLocation))
            return true;
    }
    return false;
}

} // namespace

CloudDocumentProjection projectForCloudSnapshotV1(const ProjectModel& source) {
    CloudDocumentProjection projection;
    projection.document = source;
    collab::ensureStableCollaborationIds(projection.document);

    const PublishPreflightReport preflight = inspectForPublishV1(projection.document);
    for (const PublishIssue& issue : preflight.blockers) {
        projection.blockers.push_back({CloudProjectionIssueKind::PublishBlocker,
                                       issue.location, issue.detail});
    }

    ProjectModel& document = projection.document;
    document.loopStartSeconds = 0.0;
    document.loopEndSeconds = 0.0;
    document.loopEnabled = false;
    projectInserts(document.masterInserts, "master", projection.blockers);

    for (std::size_t trackIndex = 0; trackIndex < document.tracks.size();
         ++trackIndex) {
        TrackModel& track = document.tracks[trackIndex];
        const std::string location =
            "track:" + (track.id.empty() ? std::to_string(trackIndex) : track.id);

        track.soloed = false;
        track.armed = false;
        track.monitor = false;
        track.monitorAuto = false;
        track.recordMode = TrackRecordMode::UseGlobal;
        track.inputChannel = 0;
        track.inputChannelCount = 1;
        track.inputEnabled = false;
        track.height = 72.0;
        track.expanded = true;
        track.automationExpanded = false;

        projectInsert(track.instrument, location + "/instrument",
                      projection.blockers);
        projectInserts(track.samplerFx.inserts, location + "/sampler-fx",
                       projection.blockers);
        projectInserts(track.inserts, location, projection.blockers);

        for (std::size_t clipIndex = 0; clipIndex < track.clips.size();
             ++clipIndex) {
            ClipModel& clip = track.clips[clipIndex];
            const std::string clipLocation =
                location + "/clip:" +
                (clip.id.empty() ? std::to_string(clipIndex) : clip.id);
            if (!clip.filePath.empty() && clip.kind == ClipKind::Audio &&
                clip.asset.empty()) {
                projection.blockers.push_back({
                    CloudProjectionIssueKind::MissingAsset,
                    clipLocation + "/audio",
                    "audio clip has no completed cloud asset",
                });
            }
            clip.filePath.clear();
            clip.expanded = false;
            sanitizeAsset(clip.asset);
            projectInserts(clip.inserts, clipLocation, projection.blockers);
            for (std::size_t takeIndex = 0; takeIndex < clip.takes.size();
                 ++takeIndex) {
                TakeModel& take = clip.takes[takeIndex];
                if (!take.filePath.empty() && take.asset.empty()) {
                    projection.blockers.push_back({
                        CloudProjectionIssueKind::MissingAsset,
                        clipLocation + "/take:" + std::to_string(takeIndex),
                        "recorded take has no completed cloud asset",
                    });
                }
                take.filePath.clear();
                sanitizeAsset(take.asset);
            }
        }
    }

    std::string leakLocation;
    if (containsLocalPathOrUiState(document, &leakLocation)) {
        projection.blockers.push_back({
            CloudProjectionIssueKind::PublishBlocker,
            leakLocation,
            "canonical snapshot retained local path or UI/session state",
        });
    }
    return projection;
}

bool containsLocalPathOrUiState(const ProjectModel& document,
                                std::string* firstLocation) {
    if (document.loopEnabled || document.loopStartSeconds != 0.0 ||
        document.loopEndSeconds != 0.0) {
        if (firstLocation) *firstLocation = "project/transport";
        return true;
    }
    if (inspectInsertsForLeak(document.masterInserts, "master", firstLocation))
        return true;
    for (std::size_t trackIndex = 0; trackIndex < document.tracks.size();
         ++trackIndex) {
        const TrackModel& track = document.tracks[trackIndex];
        const std::string location = "track:" + std::to_string(trackIndex);
        if (track.soloed || track.armed || track.monitor || track.monitorAuto ||
            track.recordMode != TrackRecordMode::UseGlobal ||
            track.inputEnabled || track.inputChannel != 0 ||
            track.inputChannelCount != 1 || track.height != 72.0 ||
            !track.expanded || track.automationExpanded) {
            if (firstLocation) *firstLocation = location + "/local-state";
            return true;
        }
        if (inspectInsertForLeak(track.instrument, location + "/instrument",
                                 firstLocation) ||
            inspectInsertsForLeak(track.samplerFx.inserts,
                                  location + "/sampler-fx", firstLocation) ||
            inspectInsertsForLeak(track.inserts, location, firstLocation))
            return true;
        for (std::size_t clipIndex = 0; clipIndex < track.clips.size();
             ++clipIndex) {
            const ClipModel& clip = track.clips[clipIndex];
            const std::string clipLocation =
                location + "/clip:" + std::to_string(clipIndex);
            if (!clip.filePath.empty() || clip.expanded ||
                inspectInsertsForLeak(clip.inserts, clipLocation,
                                      firstLocation)) {
                if (firstLocation && firstLocation->empty())
                    *firstLocation = clipLocation;
                return true;
            }
            for (std::size_t takeIndex = 0; takeIndex < clip.takes.size();
                 ++takeIndex) {
                if (!clip.takes[takeIndex].filePath.empty()) {
                    if (firstLocation)
                        *firstLocation = clipLocation + "/take:" +
                                         std::to_string(takeIndex);
                    return true;
                }
            }
        }
    }
    return false;
}

} // namespace daw::cloud
