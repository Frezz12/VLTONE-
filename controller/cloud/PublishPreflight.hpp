#pragma once

#include "model/Document.hpp"

#include <string>
#include <vector>

namespace daw::cloud {

enum class PublishIssueKind {
    ThirdPartyPlugin,
    UnknownInternalPlugin,
    MissingEntityId,
    InvalidAssetIdentity,
};

struct PublishIssue {
    PublishIssueKind kind = PublishIssueKind::ThirdPartyPlugin;
    std::string location;
    std::string entityId;
    std::string pluginUid;
    std::string displayName;
    std::string detail;

    friend bool operator==(const PublishIssue&, const PublishIssue&) = default;
};

struct PublishPreflightReport {
    std::vector<PublishIssue> blockers;
    std::vector<AssetRef> referencedAssets;

    bool canPublish() const noexcept { return blockers.empty(); }
};

/// Inspect every document-owned plugin slot and cloud asset reference.
///
/// This is intentionally pure: it never probes a plugin, opens a file or
/// changes the project. The publish workflow performs this check before it
/// creates an uploading cloud project. V1 accepts only the four application
/// built-ins; a user must remove or manually bounce every other plugin.
PublishPreflightReport inspectForPublishV1(const ProjectModel& project);

bool isSupportedBuiltinV1(const InsertModel& insert) noexcept;
const char* publishIssueKindName(PublishIssueKind kind) noexcept;

} // namespace daw::cloud

