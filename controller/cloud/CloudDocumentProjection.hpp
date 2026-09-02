#pragma once

#include "cloud/PublishPreflight.hpp"

#include <string>
#include <vector>

namespace daw::cloud {

enum class CloudProjectionIssueKind {
    PublishBlocker,
    MissingAsset,
};

struct CloudProjectionIssue {
    CloudProjectionIssueKind kind = CloudProjectionIssueKind::PublishBlocker;
    std::string location;
    std::string detail;
};

struct CloudDocumentProjection {
    ProjectModel document;
    std::vector<CloudProjectionIssue> blockers;

    bool valid() const noexcept { return blockers.empty(); }
};

/// Create the musical document uploaded in a canonical cloud snapshot.
///
/// The result carries no absolute/cache/plugin-module paths and no per-user
/// transport, capture or window state. File-backed audio/state must already
/// have AssetRef identities; missing identities are reported as blockers and
/// the unsafe local path is still removed from the projected document.
CloudDocumentProjection projectForCloudSnapshotV1(const ProjectModel& source);

/// Defense-in-depth validation used before serialization/upload.
bool containsLocalPathOrUiState(const ProjectModel& document,
                                std::string* firstLocation = nullptr);

} // namespace daw::cloud

