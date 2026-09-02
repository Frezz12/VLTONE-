#include "CloudRecordingRecoveryUpload.hpp"

#include <QByteArray>
#include <QDir>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

namespace collab {
namespace {

constexpr double kMaximumTimeSeconds = 365.0 * 24.0 * 60.0 * 60.0;
constexpr double kMaximumSampleRate = 768000.0;
constexpr std::uint32_t kMaximumChannels = 1024;
constexpr double kMinimumRecoveredPassSeconds = 0.01;

using daw::recovery::CloudRecordingCapture;
using daw::recovery::CloudRecordingCaptureStatus;
using daw::recovery::CloudRecordingMode;
using daw::recovery::CloudRecordingPass;
using daw::recovery::CloudRecordingRecoveryManifest;
using daw::recovery::CloudRecordingRecoveryRun;

CloudRecordingRecoveryUploadResult failure(
    CloudRecordingRecoveryUploadCode code, QString message) {
    CloudRecordingRecoveryUploadResult result;
    result.code = code;
    result.safeMessage = std::move(message);
    return result;
}

bool canonicalUuid(std::string_view value) {
    if (value.size() != 36) return false;
    bool nonzero = false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (value[index] != '-') return false;
            continue;
        }
        const char ch = value[index];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
            return false;
        nonzero = nonzero || ch != '0';
    }
    return nonzero;
}

bool canonicalUuid(const QString& value) {
    const QByteArray utf8 = value.toUtf8();
    return QString::fromUtf8(utf8) == value &&
           canonicalUuid(std::string_view(utf8.constData(),
                                          std::size_t(utf8.size())));
}

QString uuidText(const std::string& value) {
    return QString::fromLatin1(value.data(), qsizetype(value.size()));
}

bool boundedFinite(double value, double minimum, double maximum,
                   bool minimumExclusive = false) {
    if (!std::isfinite(value) || value > maximum) return false;
    return minimumExclusive ? value > minimum : value >= minimum;
}

double captureTolerance(const CloudRecordingCapture& capture) {
    return capture.sampleRate > 0.0
               ? std::max(0.001, 2.0 / capture.sampleRate)
               : 0.001;
}

bool nearlyEqual(double left, double right, double tolerance) {
    return std::isfinite(left) && std::isfinite(right) &&
           std::abs(left - right) <= tolerance;
}

std::optional<std::vector<CloudRecordingPass>> expectedPasses(
    const CloudRecordingCapture& capture) {
    std::vector<CloudRecordingPass> passes;
    const auto& semantics = capture.semantics;
    const double loopLength =
        semantics.loopEndSeconds - semantics.loopStartSeconds;
    const bool splitsAtLoop = semantics.loopEnabled && loopLength > 0.0 &&
                              capture.startSeconds >=
                                  semantics.loopStartSeconds &&
                              capture.startSeconds < semantics.loopEndSeconds;
    if (!splitsAtLoop ||
        semantics.loopEndSeconds - capture.startSeconds >=
            capture.durationSeconds) {
        passes.push_back({capture.startSeconds,
                          capture.startSeconds + capture.durationSeconds,
                          0.0});
    } else {
        double from = 0.0;
        double at = capture.startSeconds;
        double boundary = semantics.loopEndSeconds - capture.startSeconds;
        while (boundary < capture.durationSeconds) {
            if (passes.size() >=
                daw::recovery::kMaxCloudRecordingPassesPerCapture) {
                return std::nullopt;
            }
            passes.push_back({at, at + (boundary - from), from});
            from = boundary;
            at = semantics.loopStartSeconds;
            boundary += loopLength;
        }
        if (capture.durationSeconds - from > 0.0) {
            passes.push_back(
                {at, at + (capture.durationSeconds - from), from});
        }
    }

    std::erase_if(passes, [](const CloudRecordingPass& pass) {
        return pass.endSeconds - pass.startSeconds <=
               kMinimumRecoveredPassSeconds;
    });
    if (!semantics.loopCreatesTakes && passes.size() > 1) {
        passes.erase(passes.begin(), std::prev(passes.end()));
    }
    return passes;
}

bool soundPassGeometry(const CloudRecordingCapture& capture) {
    if (capture.passes.empty() ||
        capture.passes.size() >
            daw::recovery::kMaxCloudRecordingPassesPerCapture) {
        return false;
    }
    const double tolerance = captureTolerance(capture);
    double previousCaptureEnd = -1.0;
    for (const CloudRecordingPass& pass : capture.passes) {
        const double length = pass.endSeconds - pass.startSeconds;
        if (!boundedFinite(pass.startSeconds, 0.0, kMaximumTimeSeconds) ||
            !boundedFinite(pass.endSeconds, pass.startSeconds,
                           kMaximumTimeSeconds, true) ||
            !boundedFinite(pass.captureOffsetSeconds, 0.0,
                           capture.durationSeconds) ||
            pass.captureOffsetSeconds + length >
                capture.durationSeconds + tolerance ||
            pass.captureOffsetSeconds + tolerance < previousCaptureEnd) {
            return false;
        }
        previousCaptureEnd = pass.captureOffsetSeconds + length;
    }

    const auto expected = expectedPasses(capture);
    if (!expected || expected->size() != capture.passes.size()) return false;
    for (std::size_t index = 0; index < expected->size(); ++index) {
        const CloudRecordingPass& actual = capture.passes[index];
        const CloudRecordingPass& wanted = (*expected)[index];
        if (!nearlyEqual(actual.startSeconds, wanted.startSeconds,
                         tolerance) ||
            !nearlyEqual(actual.endSeconds, wanted.endSeconds, tolerance) ||
            !nearlyEqual(actual.captureOffsetSeconds,
                         wanted.captureOffsetSeconds, tolerance)) {
            return false;
        }
    }
    return true;
}

bool soundReadyCapture(const CloudRecordingCapture& capture) {
    const auto& semantics = capture.semantics;
    const bool modeKnown = semantics.mode == CloudRecordingMode::Overwrite ||
                           semantics.mode == CloudRecordingMode::Layers;
    if (!modeKnown || !semantics.complete ||
        !boundedFinite(semantics.loopStartSeconds, 0.0,
                       kMaximumTimeSeconds) ||
        !boundedFinite(semantics.loopEndSeconds, 0.0,
                       kMaximumTimeSeconds) ||
        !boundedFinite(semantics.compCrossfadeMs, 0.0, 20.0) ||
        (semantics.loopEnabled &&
         semantics.loopEndSeconds <= semantics.loopStartSeconds) ||
        !boundedFinite(capture.startSeconds, 0.0,
                       kMaximumTimeSeconds) ||
        !boundedFinite(capture.durationSeconds, 0.0,
                       kMaximumTimeSeconds, true) ||
        !boundedFinite(capture.sampleRate, 0.0, kMaximumSampleRate, true) ||
        capture.channels == 0 || capture.channels > kMaximumChannels ||
        capture.frames == 0 ||
        capture.frames > std::uint64_t(kMaximumSampleRate *
                                       kMaximumTimeSeconds)) {
        return false;
    }
    const double tolerance = captureTolerance(capture);
    return nearlyEqual(double(capture.frames) / capture.sampleRate,
                       capture.durationSeconds, tolerance) &&
           soundPassGeometry(capture);
}

std::optional<std::pair<QString, QString>> localSourceAndBasename(
    const std::string& utf8Path) {
    if (utf8Path.empty() ||
        utf8Path.size() > daw::recovery::kMaxCloudRecordingPathBytes) {
        return std::nullopt;
    }
    const QByteArray raw(utf8Path.data(), qsizetype(utf8Path.size()));
    const QString sourcePath = QString::fromUtf8(raw);
    if (sourcePath.toUtf8() != raw || sourcePath.contains(QChar::Null) ||
        !QDir::isAbsolutePath(sourcePath)) {
        return std::nullopt;
    }

    QString displayName = sourcePath;
    displayName.replace(QLatin1Char('\\'), QLatin1Char('/'));
    displayName = displayName.section(QLatin1Char('/'), -1).trimmed();
    if (displayName.isEmpty() || displayName == QLatin1String(".") ||
        displayName == QLatin1String("..") || displayName.size() > 255 ||
        !displayName.endsWith(QLatin1String(".wav"),
                              Qt::CaseInsensitive) ||
        displayName.contains(QLatin1Char('/')) ||
        displayName.contains(QLatin1Char('\\'))) {
        return std::nullopt;
    }
    for (const QChar character : displayName) {
        if (character.unicode() < 0x20 || character.unicode() == 0x7f)
            return std::nullopt;
    }
    return std::pair<QString, QString>{sourcePath, displayName};
}

bool structurallyValidIdentities(
    const CloudRecordingRecoveryManifest& manifest,
    const CloudRecordingRecoveryRun*& selected,
    const std::string& selectedRunId) {
    if (manifest.version != daw::recovery::kCloudRecordingRecoveryVersion ||
        !canonicalUuid(manifest.projectId) ||
        !canonicalUuid(manifest.sessionId) ||
        manifest.createdAtUnixMs <= 0 || manifest.runs.empty() ||
        manifest.runs.size() > daw::recovery::kMaxCloudRecordingRuns) {
        return false;
    }

    std::set<std::string> runIds;
    std::set<std::string> opIds;
    std::set<std::string> transactionIds;
    std::set<std::string> captureIds;
    std::set<std::string> leaseIds;
    std::set<std::string> uploadIds;
    std::set<std::string> assetIds;
    std::set<std::string> localPaths;
    std::size_t totalCaptures = 0;
    for (const CloudRecordingRecoveryRun& run : manifest.runs) {
        if (!canonicalUuid(run.runId) || !canonicalUuid(run.opId) ||
            !canonicalUuid(run.transactionId) ||
            !runIds.insert(run.runId).second ||
            !opIds.insert(run.opId).second ||
            !transactionIds.insert(run.transactionId).second ||
            run.createdAtUnixMs <= 0 || run.captures.empty() ||
            run.captures.size() >
                daw::recovery::kMaxCloudRecordingCaptures) {
            return false;
        }
        totalCaptures += run.captures.size();
        if (totalCaptures > daw::recovery::kMaxCloudRecordingCaptures)
            return false;

        std::set<std::string> trackIds;
        for (const CloudRecordingCapture& capture : run.captures) {
            if (!canonicalUuid(capture.captureId) ||
                !canonicalUuid(capture.trackId) ||
                !canonicalUuid(capture.leaseId) ||
                !canonicalUuid(capture.uploadId) ||
                !canonicalUuid(capture.assetId) ||
                !captureIds.insert(capture.captureId).second ||
                !trackIds.insert(capture.trackId).second ||
                !leaseIds.insert(capture.leaseId).second ||
                !uploadIds.insert(capture.uploadId).second ||
                !assetIds.insert(capture.assetId).second ||
                capture.localWavPath.empty() ||
                !localPaths.insert(capture.localWavPath).second) {
                return false;
            }
        }
        if (run.runId == selectedRunId) selected = &run;
    }
    return true;
}

CloudRecordingCapture testCapture(int ordinal, double startSeconds) {
    const auto id = [ordinal](int group) {
        return std::string("00000000-0000-4000-8000-") +
               std::to_string(group) + std::to_string(ordinal) +
               "0000000000";
    };
    CloudRecordingCapture capture;
    capture.captureId = id(1);
    capture.trackId = id(2);
    capture.leaseId = id(3);
    capture.uploadId = id(4);
    capture.assetId = id(5);
    if (ordinal == 1) {
        capture.localWavPath =
            "/definitely/not/read/recovery/Take One.wav";
    } else if (ordinal == 2) {
        capture.localWavPath =
            "/definitely/not/read/recovery/nested\\Take Two.WAV";
    } else {
        capture.localWavPath =
            "/definitely/not/read/recovery/Take-" +
            std::to_string(ordinal) + ".wav";
    }
    capture.startSeconds = startSeconds;
    capture.durationSeconds = 1.0;
    capture.sampleRate = 48000.0;
    capture.channels = 2;
    capture.frames = 48000;
    capture.passes.push_back({startSeconds, startSeconds + 1.0, 0.0});
    return capture;
}

CloudRecordingRecoveryManifest testManifest() {
    CloudRecordingRecoveryManifest manifest;
    manifest.projectId = "11111111-1111-4111-8111-111111111111";
    manifest.sessionId = "22222222-2222-4222-8222-222222222222";
    manifest.createdAtUnixMs = 1;
    CloudRecordingRecoveryRun run;
    run.runId = "33333333-3333-4333-8333-333333333333";
    run.opId = "44444444-4444-4444-8444-444444444444";
    run.transactionId = "55555555-5555-4555-8555-555555555555";
    run.createdAtUnixMs = 1;
    run.captures = {testCapture(1, 3.0), testCapture(2, 7.0)};
    manifest.runs.push_back(std::move(run));
    return manifest;
}

bool failTest(QString* error, QString message) {
    if (error) *error = std::move(message);
    return false;
}

} // namespace

CloudRecordingRecoveryUploadResult prepareCloudRecordingRecoveryUpload(
    const CloudRecordingRecoveryManifest& manifest, const QString& runId) {
    if (!canonicalUuid(runId)) {
        return failure(CloudRecordingRecoveryUploadCode::Invalid,
                       QStringLiteral("Recovery run identity is invalid"));
    }
    const std::string selectedRunId = runId.toLatin1().toStdString();
    const CloudRecordingRecoveryRun* selected = nullptr;
    if (!structurallyValidIdentities(manifest, selected, selectedRunId)) {
        return failure(CloudRecordingRecoveryUploadCode::Invalid,
                       QStringLiteral("Recovery manifest identity is invalid"));
    }
    if (!selected) {
        return failure(CloudRecordingRecoveryUploadCode::NotFound,
                       QStringLiteral("Recovery run was not found"));
    }
    if (selected->captures.size() >
            std::size_t(kMaximumCloudRecordingRecoveryUploadCaptures) ||
        selected->commitPhase !=
            daw::recovery::CloudRecordingCommitPhase::Pending ||
        selected->recoveryOnly || selected->lostLease) {
        return failure(CloudRecordingRecoveryUploadCode::Unsafe,
                       QStringLiteral("Recovery run is not safe to upload"));
    }
    for (const CloudRecordingCapture& capture : selected->captures) {
        if (capture.status != CloudRecordingCaptureStatus::Ready ||
            capture.uploadPhase !=
                daw::recovery::CloudRecordingUploadPhase::Captured ||
            !capture.semantics.complete) {
            return failure(
                CloudRecordingRecoveryUploadCode::Unsafe,
                QStringLiteral("Recovery run contains an ineligible capture"));
        }
    }

    QVector<ClosedRecordingAsset> recordings;
    QVector<CloudRecordingRecoveryUploadCorrelation> correlations;
    recordings.reserve(qsizetype(selected->captures.size()));
    correlations.reserve(qsizetype(selected->captures.size()));
    const QString projectId = uuidText(manifest.projectId);
    const QString capturedSessionId = uuidText(manifest.sessionId);
    const QString selectedId = uuidText(selected->runId);
    const QString opId = uuidText(selected->opId);
    const QString transactionId = uuidText(selected->transactionId);
    for (const CloudRecordingCapture& capture : selected->captures) {
        if (!soundReadyCapture(capture)) {
            return failure(
                CloudRecordingRecoveryUploadCode::Invalid,
                QStringLiteral("Recovery capture metadata is invalid"));
        }
        const auto source = localSourceAndBasename(capture.localWavPath);
        if (!source) {
            return failure(CloudRecordingRecoveryUploadCode::Invalid,
                           QStringLiteral("Recovery source path is invalid"));
        }

        ClosedRecordingAsset recording;
        recording.projectId = projectId;
        recording.uploadId = uuidText(capture.uploadId);
        recording.assetId = uuidText(capture.assetId);
        recording.sourcePath = source->first;
        recording.displayName = source->second;
        recording.contentType = QStringLiteral("audio/wav");
        recording.codec = QStringLiteral("pcm_f32le");
        recording.sampleRate = capture.sampleRate;
        recording.channels = capture.channels;
        recording.frames = capture.frames;
        recordings.push_back(std::move(recording));

        correlations.push_back({projectId,
                                capturedSessionId,
                                selectedId,
                                uuidText(capture.captureId),
                                opId,
                                transactionId,
                                uuidText(capture.trackId),
                                uuidText(capture.leaseId),
                                uuidText(capture.uploadId),
                                uuidText(capture.assetId)});
    }

    CloudRecordingRecoveryUploadResult result;
    result.code = CloudRecordingRecoveryUploadCode::Prepared;
    result.projectId = projectId;
    result.capturedSessionId = capturedSessionId;
    result.runId = selectedId;
    result.opId = opId;
    result.transactionId = transactionId;
    result.recordings = std::move(recordings);
    result.correlations = std::move(correlations);
    return result;
}

bool checkCloudRecordingRecoveryUploadForTest(QString* error) {
    const CloudRecordingRecoveryManifest valid = testManifest();
    const QString runId = uuidText(valid.runs.front().runId);
    const auto prepared = prepareCloudRecordingRecoveryUpload(valid, runId);
    if (!prepared || prepared.recordings.size() != 2 ||
        prepared.correlations.size() != 2 ||
        prepared.recordings[0].displayName != QLatin1String("Take One.wav") ||
        prepared.recordings[1].displayName != QLatin1String("Take Two.WAV") ||
        prepared.recordings[0].sourcePath !=
            QString::fromStdString(valid.runs[0].captures[0].localWavPath) ||
        prepared.recordings[0].codec != QLatin1String("pcm_f32le") ||
        prepared.capturedSessionId != uuidText(valid.sessionId) ||
        prepared.correlations[0].captureId !=
            uuidText(valid.runs[0].captures[0].captureId) ||
        prepared.correlations[0].capturedSessionId !=
            uuidText(valid.sessionId) ||
        prepared.correlations[0].capturedLeaseId !=
            uuidText(valid.runs[0].captures[0].leaseId) ||
        prepared.correlations[1].uploadId !=
            prepared.recordings[1].uploadId) {
        return failTest(error,
                        QStringLiteral("valid recovery upload was not adapted"));
    }

    const auto missing = prepareCloudRecordingRecoveryUpload(
        valid, QStringLiteral("66666666-6666-4666-8666-666666666666"));
    if (missing.code != CloudRecordingRecoveryUploadCode::NotFound ||
        !missing.recordings.isEmpty() || !missing.correlations.isEmpty()) {
        return failTest(error,
                        QStringLiteral("missing recovery run was not isolated"));
    }
    const auto malformed = prepareCloudRecordingRecoveryUpload(
        valid, QStringLiteral("{33333333-3333-4333-8333-333333333333}"));
    if (malformed.code != CloudRecordingRecoveryUploadCode::Invalid ||
        !malformed.recordings.isEmpty() || !malformed.correlations.isEmpty()) {
        return failTest(error,
                        QStringLiteral("non-canonical run id was accepted"));
    }

    CloudRecordingRecoveryManifest unsafe = valid;
    unsafe.runs[0].captures[1].status =
        CloudRecordingCaptureStatus::WriteFailed;
    auto rejected = prepareCloudRecordingRecoveryUpload(unsafe, runId);
    if (rejected.code != CloudRecordingRecoveryUploadCode::Unsafe ||
        !rejected.recordings.isEmpty() || !rejected.correlations.isEmpty()) {
        return failTest(error,
                        QStringLiteral("failed capture did not reject its run"));
    }
    for (const CloudRecordingCaptureStatus status : {
             CloudRecordingCaptureStatus::ZeroFrames,
             CloudRecordingCaptureStatus::Unreadable}) {
        unsafe = valid;
        unsafe.runs[0].captures[0].status = status;
        rejected = prepareCloudRecordingRecoveryUpload(unsafe, runId);
        if (rejected.code != CloudRecordingRecoveryUploadCode::Unsafe ||
            !rejected.recordings.isEmpty() ||
            !rejected.correlations.isEmpty()) {
            return failTest(
                error,
                QStringLiteral("non-ready capture did not reject its run"));
        }
    }
    unsafe = valid;
    unsafe.runs[0].captures[0].semantics.complete = false;
    rejected = prepareCloudRecordingRecoveryUpload(unsafe, runId);
    if (rejected.code != CloudRecordingRecoveryUploadCode::Unsafe) {
        return failTest(error,
                        QStringLiteral("incomplete semantics were accepted"));
    }
    for (const bool lostLease : {false, true}) {
        unsafe = valid;
        unsafe.runs[0].recoveryOnly = !lostLease;
        unsafe.runs[0].lostLease = lostLease;
        rejected = prepareCloudRecordingRecoveryUpload(unsafe, runId);
        if (rejected.code != CloudRecordingRecoveryUploadCode::Unsafe ||
            !rejected.recordings.isEmpty() ||
            !rejected.correlations.isEmpty()) {
            return failTest(
                error,
                lostLease
                    ? QStringLiteral("lease-lost run was accepted")
                    : QStringLiteral("recovery-only run was accepted"));
        }
    }

    CloudRecordingRecoveryManifest invalid = valid;
    invalid.runs[0].captures[1].uploadId =
        invalid.runs[0].captures[0].uploadId;
    auto invalidResult = prepareCloudRecordingRecoveryUpload(invalid, runId);
    if (invalidResult.code != CloudRecordingRecoveryUploadCode::Invalid ||
        !invalidResult.recordings.isEmpty() ||
        !invalidResult.correlations.isEmpty()) {
        return failTest(error,
                        QStringLiteral("duplicate upload identity was accepted"));
    }
    invalid = valid;
    invalid.runs[0].captures[1].passes[0].captureOffsetSeconds = 0.25;
    invalidResult = prepareCloudRecordingRecoveryUpload(invalid, runId);
    if (invalidResult.code != CloudRecordingRecoveryUploadCode::Invalid ||
        !invalidResult.recordings.isEmpty() ||
        !invalidResult.correlations.isEmpty()) {
        return failTest(error,
                        QStringLiteral("forged pass geometry was accepted"));
    }
    invalid = valid;
    invalid.runs[0].captures[0].localWavPath =
        "/definitely/not/read/recovery/bad\nname.wav";
    invalidResult = prepareCloudRecordingRecoveryUpload(invalid, runId);
    if (invalidResult.code != CloudRecordingRecoveryUploadCode::Invalid ||
        !invalidResult.recordings.isEmpty() ||
        !invalidResult.correlations.isEmpty()) {
        return failTest(error,
                        QStringLiteral("unsafe basename was accepted"));
    }

    unsafe = valid;
    for (int ordinal = 3; ordinal <= 9; ++ordinal)
        unsafe.runs[0].captures.push_back(testCapture(ordinal, ordinal * 2.0));
    rejected = prepareCloudRecordingRecoveryUpload(unsafe, runId);
    if (rejected.code != CloudRecordingRecoveryUploadCode::Unsafe ||
        !rejected.recordings.isEmpty() || !rejected.correlations.isEmpty()) {
        return failTest(error,
                        QStringLiteral("unbounded recovery run was accepted"));
    }
    return true;
}

} // namespace collab
