#pragma once

#include "model/Document.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace daw {

class EngineController;

namespace cloud {

enum class PublicationCaptureIssueKind : std::uint8_t {
    ThirdPartyPlugin,
    UnknownInternalPlugin,
    MissingLocalSource,
    UnreadableLocalSource,
    MissingLivePlugin,
    PluginStateCaptureFailed,
    StagingIo,
};

struct PublicationCaptureIssue {
    PublicationCaptureIssueKind kind =
        PublicationCaptureIssueKind::PluginStateCaptureFailed;
    std::string location;
    std::string entityId;
    std::string pluginUid;
    std::string displayName;
    std::string detail;

    friend bool operator==(const PublicationCaptureIssue&,
                           const PublicationCaptureIssue&) = default;
};

/// One file the publisher must hash, prepare and upload.
///
/// `localPath` deliberately lives outside ProjectModel. The capture document
/// uses the matching `asset.assetId`, and the eventual cloud projection strips
/// every compatibility path before serialization. `temporary` distinguishes
/// state chunks owned by this capture from media still owned by the project.
struct LocalPublicationAssetSource {
    AssetRef asset;
    std::string localPath;
    std::string location;
    bool temporary = false;
};

/// A control-thread snapshot ready for the publisher's hashing/upload phase.
///
/// The object owns its private staging directory. Keeping the object alive
/// keeps staged plugin chunks alive; destroying it (or calling cleanup())
/// removes them. It is move-only so two owners can never race to remove the
/// same directory.
class CloudPublicationCapture final {
public:
    CloudPublicationCapture(const CloudPublicationCapture&) = delete;
    CloudPublicationCapture& operator=(const CloudPublicationCapture&) = delete;
    CloudPublicationCapture(CloudPublicationCapture&&) noexcept;
    CloudPublicationCapture& operator=(CloudPublicationCapture&&) noexcept;
    ~CloudPublicationCapture();

    ProjectModel document;
    std::vector<LocalPublicationAssetSource> sources;
    std::vector<PublicationCaptureIssue> blockers;

    bool readyForHashing() const noexcept { return blockers.empty(); }
    const std::string& stagingDirectory() const noexcept;

    /// Idempotent best-effort cleanup. A failure leaves the exact directory in
    /// `stagingDirectory()` so recovery/diagnostics can retry it safely.
    bool cleanup(std::string* error = nullptr) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    explicit CloudPublicationCapture(ProjectModel source);

    void addIssue(PublicationCaptureIssue issue);
    bool bindLocalFile(AssetRef& destination, const AssetRef& preferred,
                       const std::string& localPath, AssetKind kind,
                       const std::string& location);
    bool prepareStaging(const std::string& parentDirectory);
    bool stagePluginState(AssetRef& destination, const AssetRef& preferred,
                          const std::vector<std::uint8_t>& bytes,
                          const std::string& location, bool rightChannel,
                          std::string& localPath);

    friend class ::daw::EngineController;
};

/// Pure compatibility gate used before a capture allocates state buffers or
/// creates a staging directory. V1 accepts exactly the built-in Sampler,
/// Equalizer and Gravity slots; empty slots are ignored.
std::vector<PublicationCaptureIssue> inspectCaptureCompatibilityV1(
    const ProjectModel& project);

const char* publicationCaptureIssueKindName(
    PublicationCaptureIssueKind kind) noexcept;

} // namespace cloud
} // namespace daw
