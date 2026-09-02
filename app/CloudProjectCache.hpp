#pragma once

#include "collaboration/CollaborationState.hpp"

#include <QString>

#include <optional>

namespace collab {

struct CachedCloudProject {
    QString projectId;
    quint64 serverSequence = 0;
    QString canonicalHash;
    daw::collab::SharedProjectDocument document;
};

/// Verified on-disk fallback for opening a cloud project without a network.
/// It stores only canonical shared snapshots (never pending ops, local paths,
/// credentials or UI state). Methods perform file I/O and belong on a worker
/// thread.
class CloudProjectCache final {
public:
    explicit CloudProjectCache(QString rootDirectory = {});

    static QString defaultRootDirectory();
    QString rootDirectory() const { return m_rootDirectory; }

    bool store(const QString& projectId,
               const daw::collab::SharedProjectDocument& document,
               const QString& canonicalHash,
               QString* error = nullptr) const;
    std::optional<CachedCloudProject> load(const QString& projectId,
                                           QString* error = nullptr) const;

private:
    QString m_rootDirectory;
};

bool checkCloudProjectCacheForTest(QString* error = nullptr);

} // namespace collab

