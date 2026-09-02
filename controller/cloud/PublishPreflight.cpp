#include "cloud/PublishPreflight.hpp"
#include "collaboration/ProjectCommand.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace daw::cloud {
namespace {

constexpr std::array<std::string_view, 3> kBuiltinUids{
    "daw.sampler",
    "daw.equalizer",
    "daw.gravity",
};

bool validSha256(const std::string& value) {
    return value.size() == 64 &&
           std::all_of(value.begin(), value.end(), [](unsigned char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
}

void addAsset(const AssetRef& asset, const std::string& location,
              PublishPreflightReport& report,
              std::unordered_map<std::string, std::string>& seenAssets,
              AssetKind expectedKind = AssetKind::Unknown) {
    if (asset.empty())
        return;
    if (!collab::isUuid(asset.assetId) || !validSha256(asset.sha256) ||
        asset.byteSize == 0 || asset.kind == AssetKind::Unknown ||
        (expectedKind != AssetKind::Unknown && asset.kind != expectedKind)) {
        report.blockers.push_back(PublishIssue{
            PublishIssueKind::InvalidAssetIdentity,
            location,
            asset.assetId,
            {},
            asset.originalName,
            "cloud assets require a UUID assetId, kind, non-zero size and lowercase SHA-256",
        });
        return;
    }
    const std::string identity = asset.sha256 + "|" +
                                 std::to_string(asset.byteSize) + "|" +
                                 toString(asset.kind);
    const auto [seen, inserted] = seenAssets.emplace(asset.assetId, identity);
    if (!inserted && seen->second != identity) {
        report.blockers.push_back(PublishIssue{
            PublishIssueKind::InvalidAssetIdentity,
            location,
            asset.assetId,
            {},
            asset.originalName,
            "one assetId cannot refer to different content",
        });
        return;
    }
    if (inserted)
        report.referencedAssets.push_back(asset);
}

void inspectInsert(const InsertModel& insert, const std::string& location,
                   PublishPreflightReport& report,
                   std::unordered_map<std::string, std::string>& seenAssets) {
    if (!insert.isLoaded())
        return;

    if (insert.format != PluginFormat::Internal) {
        report.blockers.push_back(PublishIssue{
            PublishIssueKind::ThirdPartyPlugin,
            location,
            insert.id,
            insert.uid,
            insert.name,
            "V1 cloud projects accept built-in plugins only; remove or bounce this slot",
        });
        return;
    }
    if (!isSupportedBuiltinV1(insert)) {
        report.blockers.push_back(PublishIssue{
            PublishIssueKind::UnknownInternalPlugin,
            location,
            insert.id,
            insert.uid,
            insert.name,
            "the internal plugin is not part of the v1 compatibility set",
        });
        return;
    }
    if (insert.pluginVersion.empty() || insert.pluginVersion.size() > 64 ||
        insert.stateSchemaVersion <= 0) {
        report.blockers.push_back(PublishIssue{
            PublishIssueKind::UnknownInternalPlugin,
            location,
            insert.id,
            insert.uid,
            insert.name,
            "built-in plugin version and state schema are required for collaboration",
        });
        return;
    }

    addAsset(insert.stateAsset, location + "/state", report, seenAssets,
             AssetKind::PluginState);
    addAsset(insert.rightStateAsset, location + "/right-state", report,
             seenAssets, AssetKind::PluginState);
    const bool sampler = insert.uid == "daw.sampler";
    std::unordered_set<std::string> bindingKeys;
    for (const PluginAssetBinding& binding : insert.assetBindings) {
        if (binding.key.empty() || binding.key.size() > 96 ||
            !bindingKeys.insert(binding.key).second) {
            report.blockers.push_back(PublishIssue{
                PublishIssueKind::InvalidAssetIdentity,
                location,
                insert.id,
                insert.uid,
                insert.name,
                "plugin asset binding keys must be non-empty and unique",
            });
            continue;
        }
        addAsset(binding.asset, location + "/binding:" + binding.key, report,
                 seenAssets,
                 binding.key == "sample" ? AssetKind::Audio
                                           : AssetKind::Unknown);
        if (binding.asset.empty() ||
            (sampler && binding.key == "sample" && !binding.required)) {
            report.blockers.push_back(PublishIssue{
                PublishIssueKind::InvalidAssetIdentity,
                location,
                insert.id,
                insert.uid,
                insert.name,
                binding.asset.empty()
                    ? "plugin asset binding '" + binding.key + "' is empty"
                    : "Sampler sample binding must remain required",
            });
        }
    }
}

void requireId(const std::string& id, const std::string& location,
               PublishPreflightReport& report) {
    if (collab::isUuid(id))
        return;
    report.blockers.push_back(PublishIssue{
        PublishIssueKind::MissingEntityId,
        location,
        {},
        {},
        {},
        "shared entities require UUID identities before publication",
    });
}

void inspectInsertList(const std::vector<InsertModel>& inserts,
                       const std::string& location,
                       PublishPreflightReport& report,
                       std::unordered_map<std::string, std::string>& seenAssets) {
    for (std::size_t index = 0; index < inserts.size(); ++index) {
        requireId(inserts[index].id,
                  location + "/insert:" + std::to_string(index), report);
        inspectInsert(inserts[index],
                      location + "/insert:" +
                          (inserts[index].id.empty()
                               ? std::to_string(index)
                               : inserts[index].id),
                      report, seenAssets);
    }
}

} // namespace

bool isSupportedBuiltinV1(const InsertModel& insert) noexcept {
    if (insert.format != PluginFormat::Internal)
        return false;
    return std::find(kBuiltinUids.begin(), kBuiltinUids.end(), insert.uid) !=
           kBuiltinUids.end();
}

const char* publishIssueKindName(PublishIssueKind kind) noexcept {
    switch (kind) {
        case PublishIssueKind::ThirdPartyPlugin: return "third_party_plugin";
        case PublishIssueKind::UnknownInternalPlugin: return "unknown_internal_plugin";
        case PublishIssueKind::MissingEntityId: return "missing_entity_id";
        case PublishIssueKind::InvalidAssetIdentity: return "invalid_asset_identity";
    }
    return "unknown";
}

PublishPreflightReport inspectForPublishV1(const ProjectModel& project) {
    PublishPreflightReport report;
    std::unordered_map<std::string, std::string> seenAssets;

    inspectInsertList(project.masterInserts, "master", report, seenAssets);
    for (std::size_t trackIndex = 0; trackIndex < project.tracks.size();
         ++trackIndex) {
        const TrackModel& track = project.tracks[trackIndex];
        const std::string trackLocation =
            "track:" + (track.id.empty() ? std::to_string(trackIndex) : track.id);
        requireId(track.id, trackLocation, report);
        if (track.instrument.isLoaded() || !track.instrument.id.empty())
            requireId(track.instrument.id, trackLocation + "/instrument", report);
        inspectInsert(track.instrument, trackLocation + "/instrument", report,
                      seenAssets);
        inspectInsertList(track.samplerFx.inserts, trackLocation + "/sampler-fx",
                          report, seenAssets);
        inspectInsertList(track.inserts, trackLocation, report, seenAssets);

        for (std::size_t clipIndex = 0; clipIndex < track.clips.size();
             ++clipIndex) {
            const ClipModel& clip = track.clips[clipIndex];
            const std::string clipLocation =
                trackLocation + "/clip:" +
                (clip.id.empty() ? std::to_string(clipIndex) : clip.id);
            requireId(clip.id, clipLocation, report);
            addAsset(clip.asset, clipLocation + "/audio", report, seenAssets,
                     AssetKind::Audio);
            inspectInsertList(clip.inserts, clipLocation, report, seenAssets);
            for (std::size_t takeIndex = 0; takeIndex < clip.takes.size();
                 ++takeIndex) {
                const TakeModel& take = clip.takes[takeIndex];
                const std::string takeLocation =
                    clipLocation + "/take:" +
                    (take.id.empty() ? std::to_string(takeIndex) : take.id);
                requireId(take.id, takeLocation, report);
                addAsset(take.asset, takeLocation + "/audio", report,
                         seenAssets, AssetKind::Audio);
            }
            for (std::size_t segmentIndex = 0;
                 segmentIndex < clip.comp.size(); ++segmentIndex) {
                requireId(clip.comp[segmentIndex].id,
                          clipLocation + "/comp:" +
                              std::to_string(segmentIndex),
                          report);
            }
            for (std::size_t pointIndex = 0;
                 pointIndex < clip.automation.points.size(); ++pointIndex) {
                requireId(clip.automation.points[pointIndex].id,
                          clipLocation + "/automation-point:" +
                              std::to_string(pointIndex),
                          report);
            }
            for (std::size_t laneIndex = 0; laneIndex < clip.lanes.size();
                 ++laneIndex) {
                const ControllerLane& lane = clip.lanes[laneIndex];
                requireId(lane.id,
                          clipLocation + "/lane:" +
                              std::to_string(laneIndex),
                          report);
                for (std::size_t pointIndex = 0; pointIndex < lane.points.size();
                     ++pointIndex) {
                    requireId(lane.points[pointIndex].id,
                              clipLocation + "/lane:" + lane.id + "/point:" +
                                  std::to_string(pointIndex),
                              report);
                }
            }
        }
    }
    return report;
}

} // namespace daw::cloud
