#pragma once

#include "model/Document.hpp"

#include <QString>
#include <QStringList>

namespace collab {

constexpr qsizetype kMaximumSnapshotAssetIds = 100000;

/// Exact, path-free asset identity set embedded in one canonical snapshot.
///
/// Repeated references to the same asset inside the musical document collapse
/// to one id. The resulting list is always canonical lowercase UUID text,
/// unique and lexically sorted so it can safely participate in upload
/// idempotency and be compared byte-for-byte with the server descriptor.
struct CloudSnapshotAssetManifest {
    QStringList assetIds;
    QString safeError;
    bool accepted = false;
};

CloudSnapshotAssetManifest collectCloudSnapshotAssetManifest(
    const daw::ProjectModel& project);

/// Validates the wire/storage representation without normalizing it. This is
/// deliberately strict: accepting a reordered or duplicated list would make
/// one snapshot upload identity have more than one representation.
bool isCanonicalCloudSnapshotAssetManifest(const QStringList& assetIds);

} // namespace collab
