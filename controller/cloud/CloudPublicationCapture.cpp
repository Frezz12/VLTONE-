#include "cloud/CloudPublicationCapture.hpp"

#include "cloud/PublishPreflight.hpp"
#include "collaboration/ProjectCommand.hpp"
#include "platform/PathUtils.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

namespace daw::cloud {
namespace fs = std::filesystem;

namespace {

std::string basename(const fs::path& path) {
    return platform::pathToUtf8(path.filename());
}

std::string audioMimeType(const fs::path& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value) { return char(std::tolower(value)); });
    if (extension == ".wav" || extension == ".wave") return "audio/wav";
    if (extension == ".flac") return "audio/flac";
    if (extension == ".aif" || extension == ".aiff") return "audio/aiff";
    if (extension == ".mp3") return "audio/mpeg";
    if (extension == ".ogg" || extension == ".oga") return "audio/ogg";
    if (extension == ".m4a" || extension == ".aac") return "audio/mp4";
    return "application/octet-stream";
}

template <typename Visitor>
void visitInserts(const ProjectModel& project, Visitor&& visitor) {
    for (std::size_t index = 0; index < project.masterInserts.size(); ++index) {
        visitor(project.masterInserts[index],
                "master/insert:" + std::to_string(index));
    }
    for (std::size_t trackIndex = 0; trackIndex < project.tracks.size();
         ++trackIndex) {
        const TrackModel& track = project.tracks[trackIndex];
        const std::string trackLocation =
            "track:" + (track.id.empty() ? std::to_string(trackIndex)
                                          : track.id);
        if (track.instrument.isLoaded())
            visitor(track.instrument, trackLocation + "/instrument");
        for (std::size_t index = 0; index < track.samplerFx.inserts.size();
             ++index) {
            visitor(track.samplerFx.inserts[index],
                    trackLocation + "/sampler-fx/insert:" +
                        std::to_string(index));
        }
        for (std::size_t clipIndex = 0; clipIndex < track.clips.size();
             ++clipIndex) {
            const ClipModel& clip = track.clips[clipIndex];
            const std::string clipLocation =
                trackLocation + "/clip:" +
                (clip.id.empty() ? std::to_string(clipIndex) : clip.id);
            for (std::size_t index = 0; index < clip.inserts.size(); ++index) {
                visitor(clip.inserts[index],
                        clipLocation + "/insert:" + std::to_string(index));
            }
        }
        for (std::size_t index = 0; index < track.inserts.size(); ++index) {
            visitor(track.inserts[index],
                    trackLocation + "/insert:" + std::to_string(index));
        }
    }
}

PublicationCaptureIssue compatibilityIssue(
    PublicationCaptureIssueKind kind, const InsertModel& insert,
    std::string location, std::string detail) {
    return PublicationCaptureIssue{kind,
                                   std::move(location),
                                   insert.id,
                                   insert.uid,
                                   insert.name,
                                   std::move(detail)};
}

} // namespace

struct CloudPublicationCapture::Impl {
    std::string stagingDirectory;
    std::unordered_map<std::string, AssetRef> audioByCanonicalPath;
    std::unordered_set<std::string> usedAssetIds;

    ~Impl() {
        if (stagingDirectory.empty()) return;
        try {
            std::error_code ignored;
            fs::remove_all(platform::pathFromUtf8(stagingDirectory), ignored);
        } catch (...) {
        }
    }
};

CloudPublicationCapture::CloudPublicationCapture(ProjectModel source)
    : document(std::move(source)), m_impl(std::make_unique<Impl>()) {}

CloudPublicationCapture::CloudPublicationCapture(
    CloudPublicationCapture&&) noexcept = default;
CloudPublicationCapture& CloudPublicationCapture::operator=(
    CloudPublicationCapture&&) noexcept = default;

CloudPublicationCapture::~CloudPublicationCapture() { (void)cleanup(); }

const std::string& CloudPublicationCapture::stagingDirectory() const noexcept {
    static const std::string empty;
    return m_impl ? m_impl->stagingDirectory : empty;
}

bool CloudPublicationCapture::cleanup(std::string* error) noexcept {
    try {
        if (error) error->clear();
        if (!m_impl || m_impl->stagingDirectory.empty()) return true;
        const fs::path staging =
            platform::pathFromUtf8(m_impl->stagingDirectory);
        std::error_code removeError;
        fs::remove_all(staging, removeError);
        if (removeError) {
            if (error) *error = removeError.message();
            return false;
        }
        m_impl->stagingDirectory.clear();
        return true;
    } catch (const std::exception& exception) {
        if (error) {
            try {
                *error = exception.what();
            } catch (...) {
            }
        }
        return false;
    } catch (...) {
        return false;
    }
}

void CloudPublicationCapture::addIssue(PublicationCaptureIssue issue) {
    blockers.push_back(std::move(issue));
}

bool CloudPublicationCapture::bindLocalFile(
    AssetRef& destination, const AssetRef& preferred,
    const std::string& localPath, AssetKind kind,
    const std::string& location) {
    if (localPath.empty()) {
        addIssue({PublicationCaptureIssueKind::MissingLocalSource,
                  location,
                  preferred.assetId,
                  {},
                  preferred.originalName,
                  "the local asset source path is empty"});
        return false;
    }

    std::error_code pathError;
    fs::path path = platform::pathFromUtf8(localPath);
    if (path.is_relative()) {
        path = fs::absolute(path, pathError);
        if (pathError) {
            addIssue({PublicationCaptureIssueKind::UnreadableLocalSource,
                      location,
                      preferred.assetId,
                      {},
                      basename(path),
                      "cannot resolve the local asset source to an absolute path"});
            return false;
        }
    }
    if (!pathError) {
        const fs::path canonical = fs::weakly_canonical(path, pathError);
        if (!pathError) path = canonical;
    }
    pathError.clear();
    if (!fs::is_regular_file(path, pathError) || pathError) {
        addIssue({PublicationCaptureIssueKind::MissingLocalSource,
                  location,
                  preferred.assetId,
                  {},
                  basename(path),
                  "referenced local asset is missing or is not a regular file"});
        return false;
    }
    pathError.clear();
    const std::uintmax_t byteSize = fs::file_size(path, pathError);
    std::ifstream readable(path, std::ios::binary);
    if (pathError || byteSize == 0 || !readable) {
        addIssue({PublicationCaptureIssueKind::UnreadableLocalSource,
                  location,
                  preferred.assetId,
                  {},
                  basename(path),
                  "referenced local asset is empty or unreadable"});
        return false;
    }

    const std::string canonicalPath =
        platform::pathToUtf8(path.lexically_normal());
    if (kind == AssetKind::Audio) {
        const auto existing = m_impl->audioByCanonicalPath.find(canonicalPath);
        if (existing != m_impl->audioByCanonicalPath.end()) {
            destination = existing->second;
            return true;
        }
    }

    AssetRef asset;
    if (collab::isUuid(preferred.assetId) &&
        !m_impl->usedAssetIds.contains(preferred.assetId)) {
        asset.assetId = preferred.assetId;
    } else {
        do {
            asset.assetId = newUuid();
        } while (m_impl->usedAssetIds.contains(asset.assetId));
    }
    m_impl->usedAssetIds.insert(asset.assetId);
    asset.kind = kind;
    asset.byteSize = static_cast<std::uint64_t>(byteSize);
    asset.originalName = basename(path);
    asset.mimeType = kind == AssetKind::Audio
                         ? audioMimeType(path)
                         : "application/octet-stream";
    if (kind == AssetKind::Audio) {
        asset.codec = preferred.codec;
        asset.sampleRate = preferred.sampleRate;
        asset.channels = preferred.channels;
        asset.frames = preferred.frames;
        m_impl->audioByCanonicalPath.emplace(canonicalPath, asset);
    }
    destination = asset;
    sources.push_back(LocalPublicationAssetSource{
        asset, canonicalPath, location, false});
    return true;
}

bool CloudPublicationCapture::prepareStaging(
    const std::string& parentDirectory) {
    fs::path parent;
    std::error_code error;
    if (parentDirectory.empty()) {
        parent = fs::temp_directory_path(error);
        if (error) {
            addIssue({PublicationCaptureIssueKind::StagingIo,
                      "staging",
                      {},
                      {},
                      {},
                      "cannot resolve the temporary directory: " +
                          error.message()});
            return false;
        }
    } else {
        parent = platform::pathFromUtf8(parentDirectory);
    }

    fs::path staging;
    do {
        staging = parent / ("vlt-cloud-publication-" + newUuid() +
                            ".partial");
    } while (fs::exists(staging, error) && !error);
    error.clear();
    fs::create_directories(staging, error);
    if (error) {
        addIssue({PublicationCaptureIssueKind::StagingIo,
                  "staging",
                  {},
                  {},
                  {},
                  "cannot create cloud publication staging: " +
                      error.message()});
        return false;
    }
    m_impl->stagingDirectory = platform::pathToUtf8(staging);
    return true;
}

bool CloudPublicationCapture::stagePluginState(
    AssetRef& destination, const AssetRef& preferred,
    const std::vector<std::uint8_t>& bytes, const std::string& location,
    bool rightChannel, std::string& localPath) {
    localPath.clear();
    if (!m_impl || m_impl->stagingDirectory.empty() || bytes.empty()) {
        addIssue({PublicationCaptureIssueKind::PluginStateCaptureFailed,
                  location,
                  preferred.assetId,
                  {},
                  {},
                  "plugin returned an empty state chunk"});
        return false;
    }

    AssetRef asset;
    if (collab::isUuid(preferred.assetId) &&
        !m_impl->usedAssetIds.contains(preferred.assetId)) {
        asset.assetId = preferred.assetId;
    } else {
        do {
            asset.assetId = newUuid();
        } while (m_impl->usedAssetIds.contains(asset.assetId));
    }
    m_impl->usedAssetIds.insert(asset.assetId);
    asset.kind = AssetKind::PluginState;
    asset.byteSize = static_cast<std::uint64_t>(bytes.size());
    asset.originalName = rightChannel ? "plugin-state-right.bin"
                                      : "plugin-state.bin";
    asset.mimeType = "application/vnd.vlt.plugin-state";

    const fs::path directory =
        platform::pathFromUtf8(m_impl->stagingDirectory);
    const fs::path target = directory / (asset.assetId + ".bin");
    fs::path temporary = target;
    temporary += ".tmp-" + newUuid();
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (stream) {
        stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        stream.flush();
    }
    if (!stream || !stream.good()) {
        stream.close();
        std::error_code cleanupError;
        fs::remove(temporary, cleanupError);
        addIssue({PublicationCaptureIssueKind::StagingIo,
                  location,
                  asset.assetId,
                  {},
                  asset.originalName,
                  "cannot write staged plugin state"});
        return false;
    }
    stream.close();

    std::error_code renameError;
    fs::rename(temporary, target, renameError);
    if (renameError) {
        std::error_code cleanupError;
        fs::remove(temporary, cleanupError);
        addIssue({PublicationCaptureIssueKind::StagingIo,
                  location,
                  asset.assetId,
                  {},
                  asset.originalName,
                  "cannot publish staged plugin state: " +
                      renameError.message()});
        return false;
    }

    localPath = platform::pathToUtf8(target);
    destination = asset;
    sources.push_back(LocalPublicationAssetSource{
        asset, localPath, location, true});
    return true;
}

std::vector<PublicationCaptureIssue> inspectCaptureCompatibilityV1(
    const ProjectModel& project) {
    std::vector<PublicationCaptureIssue> issues;
    visitInserts(project, [&](const InsertModel& insert,
                              const std::string& location) {
        if (!insert.isLoaded()) return;
        if (insert.format != PluginFormat::Internal) {
            issues.push_back(compatibilityIssue(
                PublicationCaptureIssueKind::ThirdPartyPlugin, insert,
                location,
                "V1 cloud projects accept built-in plugins only; remove or "
                "manually bounce this slot"));
        } else if (!isSupportedBuiltinV1(insert)) {
            issues.push_back(compatibilityIssue(
                PublicationCaptureIssueKind::UnknownInternalPlugin, insert,
                location,
                "the internal plugin is not part of the v1 compatibility set"));
        }
    });
    return issues;
}

const char* publicationCaptureIssueKindName(
    PublicationCaptureIssueKind kind) noexcept {
    switch (kind) {
        case PublicationCaptureIssueKind::ThirdPartyPlugin:
            return "third_party_plugin";
        case PublicationCaptureIssueKind::UnknownInternalPlugin:
            return "unknown_internal_plugin";
        case PublicationCaptureIssueKind::MissingLocalSource:
            return "missing_local_source";
        case PublicationCaptureIssueKind::UnreadableLocalSource:
            return "unreadable_local_source";
        case PublicationCaptureIssueKind::MissingLivePlugin:
            return "missing_live_plugin";
        case PublicationCaptureIssueKind::PluginStateCaptureFailed:
            return "plugin_state_capture_failed";
        case PublicationCaptureIssueKind::StagingIo: return "staging_io";
    }
    return "unknown";
}

} // namespace daw::cloud
