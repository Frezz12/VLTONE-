#pragma once

#include "CloudRecordingAssetCoordinator.hpp"
#include "recovery/CloudRecordingRecovery.hpp"

#include <QString>
#include <QVector>

namespace collab {

inline constexpr qsizetype kMaximumCloudRecordingRecoveryUploadCaptures = 8;

enum class CloudRecordingRecoveryUploadCode : quint8 {
    Prepared,
    NotFound,
    Unsafe,
    Invalid,
};

/// Stable identities needed to bind an uploaded asset back to the frozen
/// recording commit. It intentionally contains no local path.
struct CloudRecordingRecoveryUploadCorrelation {
    QString projectId;
    /// Historical provenance only. A restarted client must join the current
    /// session and acquire a fresh lease; neither captured id is authorization.
    QString capturedSessionId;
    QString runId;
    QString captureId;
    QString opId;
    QString transactionId;
    QString trackId;
    QString capturedLeaseId;
    QString uploadId;
    QString assetId;

    friend bool operator==(const CloudRecordingRecoveryUploadCorrelation&,
                           const CloudRecordingRecoveryUploadCorrelation&) =
        default;
};

/// Pure, all-or-nothing input for CloudRecordingAssetCoordinator. A prepared
/// result has one correlation for every recording at the same index; every
/// other result has both vectors empty.
struct CloudRecordingRecoveryUploadResult {
    CloudRecordingRecoveryUploadCode code =
        CloudRecordingRecoveryUploadCode::Invalid;
    QString safeMessage;
    QString projectId;
    QString capturedSessionId;
    QString runId;
    QString opId;
    QString transactionId;
    QVector<ClosedRecordingAsset> recordings;
    QVector<CloudRecordingRecoveryUploadCorrelation> correlations;

    bool prepared() const noexcept {
        return code == CloudRecordingRecoveryUploadCode::Prepared;
    }
    explicit operator bool() const noexcept { return prepared(); }
};

/// Selects one canonical run from an already validated v2 manifest and adapts
/// it without opening, hashing, deleting or submitting any file. The frozen
/// manifest capture order is preserved exactly.
CloudRecordingRecoveryUploadResult prepareCloudRecordingRecoveryUpload(
    const daw::recovery::CloudRecordingRecoveryManifest& manifest,
    const QString& runId);

bool checkCloudRecordingRecoveryUploadForTest(QString* error = nullptr);

} // namespace collab
