#include "CloudSnapshotAssetManifest.hpp"

#include <QSet>
#include <QUuid>

#include <algorithm>

namespace collab {
namespace {

QString canonicalUuid(const std::string& value) {
    const QString text = QString::fromStdString(value);
    const QUuid uuid(text);
    if (uuid.isNull()) return {};
    const QString canonical =
        uuid.toString(QUuid::WithoutBraces).toLower();
    return text == canonical ? canonical : QString();
}

template <typename Visitor>
void visitInsertAssets(const daw::InsertModel& insert, Visitor&& visitor) {
    visitor(insert.stateAsset);
    visitor(insert.rightStateAsset);
    for (const daw::PluginAssetBinding& binding : insert.assetBindings)
        visitor(binding.asset);
}

template <typename Visitor>
void visitInsertListAssets(const std::vector<daw::InsertModel>& inserts,
                           Visitor&& visitor) {
    for (const daw::InsertModel& insert : inserts)
        visitInsertAssets(insert, visitor);
}

template <typename Visitor>
void visitProjectAssets(const daw::ProjectModel& project, Visitor&& visitor) {
    visitInsertListAssets(project.masterInserts, visitor);
    for (const daw::TrackModel& track : project.tracks) {
        visitInsertAssets(track.instrument, visitor);
        visitInsertListAssets(track.samplerFx.inserts, visitor);
        visitInsertListAssets(track.inserts, visitor);
        for (const daw::ClipModel& clip : track.clips) {
            visitor(clip.asset);
            visitInsertListAssets(clip.inserts, visitor);
            for (const daw::TakeModel& take : clip.takes)
                visitor(take.asset);
        }
    }
}

} // namespace

CloudSnapshotAssetManifest collectCloudSnapshotAssetManifest(
    const daw::ProjectModel& project) {
    CloudSnapshotAssetManifest result;
    QSet<QString> uniqueIds;
    visitProjectAssets(project, [&](const daw::AssetRef& asset) {
        if (!result.safeError.isEmpty()) return;
        if (asset.assetId.empty()) {
            if (!asset.empty()) {
                result.safeError = QStringLiteral(
                    "Cloud snapshot contains an asset without an identity");
            }
            return;
        }
        const QString id = canonicalUuid(asset.assetId);
        if (id.isEmpty()) {
            result.safeError = QStringLiteral(
                "Cloud snapshot contains a malformed asset identity");
            return;
        }
        uniqueIds.insert(id);
        if (uniqueIds.size() > kMaximumSnapshotAssetIds) {
            result.safeError = QStringLiteral(
                "Cloud snapshot contains too many asset identities");
        }
    });
    if (!result.safeError.isEmpty()) return result;

    result.assetIds = QStringList(uniqueIds.cbegin(), uniqueIds.cend());
    std::sort(result.assetIds.begin(), result.assetIds.end());
    result.accepted = true;
    return result;
}

bool isCanonicalCloudSnapshotAssetManifest(const QStringList& assetIds) {
    if (assetIds.size() > kMaximumSnapshotAssetIds) return false;
    QString previous;
    for (const QString& id : assetIds) {
        const QUuid uuid(id);
        if (uuid.isNull() ||
            uuid.toString(QUuid::WithoutBraces).toLower() != id ||
            (!previous.isEmpty() && !(previous < id))) {
            return false;
        }
        previous = id;
    }
    return true;
}

} // namespace collab
