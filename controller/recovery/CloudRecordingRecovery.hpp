#pragma once

/// Durable sidecar for cloud recordings that have finished locally but have
/// not yet reached one committed project operation.
///
/// The manifest lives inside the current RecoveryJournal session directory.
/// This class deliberately has no destructor cleanup: a process crash must
/// leave both the WAV files and the manifest discoverable on the next launch.
/// Deletion APIs are named for the point at which they are safe to use --
/// after the future recording commit has been durably acknowledged.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace daw::recovery {

inline constexpr const char* kCloudRecordingRecoveryFile =
    "cloud-recordings.json";
/// A short-lived durable intent used only while one committed run is being
/// removed. It lets the next process finish WAV cleanup if the previous one
/// stopped after publishing the replacement manifest.
inline constexpr const char* kCloudRecordingRunCleanupFile =
    "cloud-recording-run-cleanup.json";
inline constexpr const char* kCloudRecordingRunCleanupFormat =
    "vlt-cloud-recording-run-cleanup";
inline constexpr std::uint32_t kCloudRecordingRunCleanupVersion = 1;
inline constexpr const char* kCloudRecordingRecoveryFormat =
    "vlt-cloud-recording-recovery";
inline constexpr std::uint32_t kCloudRecordingRecoveryLegacyVersion = 1;
inline constexpr std::uint32_t kCloudRecordingRecoveryVersion = 2;
inline constexpr std::size_t kMaxCloudRecordingRecoveryBytes = 1024 * 1024;
inline constexpr std::size_t kMaxCloudRecordingRuns = 256;
inline constexpr std::size_t kMaxCloudRecordingCaptures = 256;
inline constexpr std::size_t kMaxCloudRecordingPassesPerCapture = 4096;
inline constexpr std::size_t kMaxCloudRecordingPathBytes = 32768;

enum class CloudRecordingMode : std::uint8_t {
    Overwrite,
    Layers,
};

/// Whether the finalized file is immediately usable by the future commit
/// planner. Non-ready states are deliberately durable too: a zero-frame or
/// unreadable closed WAV must remain discoverable instead of disappearing.
enum class CloudRecordingCaptureStatus : std::uint8_t {
    Ready,
    ZeroFrames,
    Unreadable,
    WriteFailed,
};

/// Maps the closed recorder's durable accounting to the v2 recovery state.
/// A readable prefix is not publishable when the writer lost any frames or
/// failed its final close/flush; it remains a WriteFailed recovery artifact.
CloudRecordingCaptureStatus classifyCloudRecordingCaptureStatus(
    bool fileWriteSucceeded,
    std::uint64_t capturedFrames,
    std::uint64_t writtenFrames,
    std::uint64_t droppedFrames,
    bool audioReadable,
    std::uint64_t readableFrames) noexcept;

/// V2 is written before upload/commit are wired into the runtime. These
/// explicit phases record the only facts the current runtime actually knows;
/// later phases require a future schema carrying verified server responses.
enum class CloudRecordingUploadPhase : std::uint8_t {
    Captured,
};

enum class CloudRecordingCommitPhase : std::uint8_t {
    Pending,
};

struct CloudRecordingPass {
    double startSeconds = 0.0;
    double endSeconds = 0.0;
    double captureOffsetSeconds = 0.0;

    friend bool operator==(const CloudRecordingPass&,
                           const CloudRecordingPass&) = default;
};

struct CloudRecordingSemantics {
    CloudRecordingMode mode = CloudRecordingMode::Overwrite;
    /// False only for migrated v1 rows, where the old sidecar persisted mode
    /// but not the other frozen recording choices. A later planner must not
    /// guess those choices.
    bool complete = true;
    bool loopEnabled = false;
    double loopStartSeconds = 0.0;
    double loopEndSeconds = 0.0;
    bool loopCreatesTakes = true;
    bool trimTakesToRegion = true;
    bool autoExpandAfterRecord = false;
    double compCrossfadeMs = 5.0;

    friend bool operator==(const CloudRecordingSemantics&,
                           const CloudRecordingSemantics&) = default;
};

struct CloudRecordingCapture {
    std::string captureId;
    std::string trackId;
    std::string leaseId;
    /// Client-generated idempotency identities for the future resumable asset
    /// workflow. They are not server upload-session credentials.
    std::string uploadId;
    std::string assetId;
    CloudRecordingUploadPhase uploadPhase =
        CloudRecordingUploadPhase::Captured;
    CloudRecordingCaptureStatus status =
        CloudRecordingCaptureStatus::Ready;
    /// Absolute UTF-8 path is the one intentionally local datum required to
    /// recover the closed WAV. No project paths or remote URLs are persisted.
    std::string localWavPath;
    double startSeconds = 0.0;
    double durationSeconds = 0.0;
    double sampleRate = 0.0;
    std::uint32_t channels = 0;
    std::uint64_t frames = 0;
    std::vector<CloudRecordingPass> passes;
    CloudRecordingSemantics semantics;

    friend bool operator==(const CloudRecordingCapture&,
                           const CloudRecordingCapture&) = default;
};

struct CloudRecordingRecoveryRun {
    std::string runId;
    /// Reserved once at capture time so an eventual recording.commit retry is
    /// idempotent and keeps one typed-undo transaction identity.
    std::string opId;
    std::string transactionId;
    std::int64_t createdAtUnixMs = 0;
    bool recoveryOnly = false;
    bool lostLease = false;
    CloudRecordingCommitPhase commitPhase =
        CloudRecordingCommitPhase::Pending;
    std::vector<CloudRecordingCapture> captures;

    friend bool operator==(const CloudRecordingRecoveryRun&,
                           const CloudRecordingRecoveryRun&) = default;
};

struct CloudRecordingRecoveryManifest {
    std::uint32_t version = kCloudRecordingRecoveryVersion;
    std::string projectId;
    std::string sessionId;
    /// Unix epoch milliseconds. Serialized under the exact key `createdAt`.
    std::int64_t createdAtUnixMs = 0;
    std::vector<CloudRecordingRecoveryRun> runs;

    friend bool operator==(const CloudRecordingRecoveryManifest&,
                           const CloudRecordingRecoveryManifest&) = default;
};

enum class CloudRecordingRecoveryCode : std::uint8_t {
    Ok,
    NotFound,
    Invalid,
    IoError,
};

struct CloudRecordingRecoveryResult {
    CloudRecordingRecoveryCode code = CloudRecordingRecoveryCode::Invalid;
    std::string message;

    bool ok() const noexcept { return code == CloudRecordingRecoveryCode::Ok; }
    explicit operator bool() const noexcept { return ok(); }
};

enum class CloudRecordingRunCleanupCode : std::uint8_t {
    Removed,
    AlreadyAbsent,
    Invalid,
    IoError,
};

/// Typed result for exact-run post-commit cleanup. `Removed` includes recovery
/// of a previously published cleanup intent; `AlreadyAbsent` is the
/// idempotent no-op state.
struct CloudRecordingRunCleanupResult {
    CloudRecordingRunCleanupCode code =
        CloudRecordingRunCleanupCode::Invalid;
    std::string message;

    bool ok() const noexcept {
        return code == CloudRecordingRunCleanupCode::Removed ||
               code == CloudRecordingRunCleanupCode::AlreadyAbsent;
    }
    bool removed() const noexcept {
        return code == CloudRecordingRunCleanupCode::Removed;
    }
    explicit operator bool() const noexcept { return ok(); }
};

/// Validates the complete closed manifest contract, including canonical UUIDs,
/// numeric bounds, pass/file consistency and existence of every WAV.
CloudRecordingRecoveryResult validateCloudRecordingRecoveryManifest(
    const CloudRecordingRecoveryManifest& manifest);

class CloudRecordingRecoveryStore {
public:
    /// `sessionDirectory` is the current RecoveryJournal::sessionDir(). It must
    /// already exist and be absolute; the store never invents recovery sessions.
    explicit CloudRecordingRecoveryStore(std::string sessionDirectory);

    const std::string& sessionDirectory() const noexcept {
        return m_sessionDirectory;
    }
    std::string manifestPath() const;

    /// Validate, durably write a same-directory temporary, then atomically
    /// replace the visible manifest. A rejected write leaves the old generation
    /// untouched.
    CloudRecordingRecoveryResult write(
        const CloudRecordingRecoveryManifest& manifest) const;

    /// Strict closed-schema read. `out` is changed only after the entire
    /// manifest and all referenced WAVs have passed validation.
    CloudRecordingRecoveryResult read(
        CloudRecordingRecoveryManifest& out) const;

    /// True while either the manifest or a resumable exact-run cleanup intent
    /// remains. Treating an intent-only last-run crash as recoverable prevents
    /// the surrounding RecoveryJournal session from being discarded early.
    bool exists() const;

    /// Legacy idempotent cleanup point that removes the complete sidecar after
    /// a future successful durable `recording.commit` acknowledgement.
    CloudRecordingRecoveryResult removeAfterCommit() const;

    /// Atomically retires only `runId` after its recording.commit is known to
    /// be durable. Other runs and their WAV files remain untouched. The
    /// replacement manifest (or last-run sidecar removal) is durably published
    /// before this run's WAVs can be deleted. A small intent makes a crash in
    /// between those steps resumable and keeps retries idempotent.
    CloudRecordingRunCleanupResult removeRunAfterCommit(
        std::string_view runId) const;

private:
    std::string m_sessionDirectory;
};

} // namespace daw::recovery
