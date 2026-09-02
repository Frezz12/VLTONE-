#include "collaboration/RecordingCommitPlanner.hpp"

#include "collaboration/ProjectReducer.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <set>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace daw::collab {
namespace {

constexpr double kMaximumTimeSeconds = 365.0 * 24.0 * 60.0 * 60.0;
constexpr double kMaximumSampleRate = 768000.0;
constexpr std::uint32_t kMaximumChannels = 1024;
constexpr double kMinimumRecoveredPassSeconds = 0.01;
constexpr double kMinimumCompSegmentSeconds = 0.001;

using recovery::CloudRecordingCapture;
using recovery::CloudRecordingCaptureStatus;
using recovery::CloudRecordingMode;
using recovery::CloudRecordingPass;
using recovery::CloudRecordingRecoveryRun;
using recovery::CloudRecordingSemantics;

RecordingCommitPlanResult failure(RecordingCommitPlanCode code,
                                  std::string message) {
    RecordingCommitPlanResult result;
    result.code = code;
    result.message = std::move(message);
    return result;
}

RecordingCommitPreflightResult preflightFailure(
    RecordingCommitPlanCode code, std::string message) {
    RecordingCommitPreflightResult result;
    result.code = code;
    result.message = std::move(message);
    return result;
}

RecordingCommitPreflightResult preflightReady() {
    RecordingCommitPreflightResult result;
    result.code = RecordingCommitPlanCode::Planned;
    return result;
}

bool canonicalUuid(std::string_view value) {
    if (!isUuid(value)) return false;
    return std::any_of(value.begin(), value.end(), [](char ch) {
        return ch != '0' && ch != '-';
    });
}

bool optionalCanonicalUuid(std::string_view value) {
    return value.empty() || canonicalUuid(value);
}

bool boundedFinite(double value, double minimum, double maximum,
                   bool minimumExclusive = false) {
    if (!std::isfinite(value) || value > maximum) return false;
    return minimumExclusive ? value > minimum : value >= minimum;
}

bool lowercaseSha256(std::string_view value) {
    return value.size() == 64 &&
           std::all_of(value.begin(), value.end(), [](char ch) {
               return (ch >= '0' && ch <= '9') ||
                      (ch >= 'a' && ch <= 'f');
           });
}

bool safeOriginalName(std::string_view value) {
    if (value.empty()) return true;
    if (value.size() > 255 || value == "." || value == "..") return false;
    for (unsigned char ch : value) {
        if (ch == '/' || ch == '\\' || ch < 0x20 || ch == 0x7f)
            return false;
    }
    const std::size_t colon = value.find(':');
    if (colon == std::string_view::npos || colon == 0) return true;
    const unsigned char first = static_cast<unsigned char>(value.front());
    if (!((first >= 'a' && first <= 'z') ||
          (first >= 'A' && first <= 'Z'))) {
        return true;
    }
    return !std::all_of(value.begin() + 1, value.begin() + colon,
                        [](unsigned char ch) {
                            return (ch >= 'a' && ch <= 'z') ||
                                   (ch >= 'A' && ch <= 'Z') ||
                                   (ch >= '0' && ch <= '9') || ch == '+' ||
                                   ch == '-' || ch == '.';
                        });
}

double captureTolerance(const CloudRecordingCapture& capture) {
    return capture.sampleRate > 0.0
               ? std::max(0.001, 2.0 / capture.sampleRate)
               : 0.001;
}

bool nearlyEqual(double left, double right, double tolerance) {
    return std::abs(left - right) <= tolerance;
}

bool completeUploadedAudio(const AssetRef& asset) {
    return canonicalUuid(asset.assetId) && lowercaseSha256(asset.sha256) &&
           asset.kind == AssetKind::Audio && asset.byteSize > 0 &&
           safeOriginalName(asset.originalName) &&
           asset.mimeType.size() <= 255 && asset.codec.size() <= 255 &&
           boundedFinite(asset.sampleRate, 0.0, kMaximumSampleRate, true) &&
           asset.channels > 0 && asset.channels <= kMaximumChannels &&
           asset.frames > 0;
}

std::optional<std::vector<CloudRecordingPass>> expectedPasses(
    const CloudRecordingCapture& capture) {
    std::vector<CloudRecordingPass> passes;
    if (capture.durationSeconds <= 0.0) return passes;

    const CloudRecordingSemantics& semantics = capture.semantics;
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
            if (passes.size() >= kMaxRecordingCommitPassesPerCapture) {
                return std::nullopt;
            }
            passes.push_back({at, at + (boundary - from), from});
            from = boundary;
            at = semantics.loopStartSeconds;
            boundary += loopLength;
        }
        if (capture.durationSeconds - from > 0.0) {
            if (passes.size() >= kMaxRecordingCommitPassesPerCapture)
                return std::nullopt;
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

bool validFrozenPasses(const CloudRecordingCapture& capture) {
    const auto expected = expectedPasses(capture);
    if (!expected || expected->empty() ||
        expected->size() != capture.passes.size()) {
        return false;
    }
    const double tolerance = captureTolerance(capture);
    for (std::size_t index = 0; index < expected->size(); ++index) {
        const CloudRecordingPass& actual = capture.passes[index];
        const CloudRecordingPass& wanted = (*expected)[index];
        if (!nearlyEqual(actual.startSeconds, wanted.startSeconds, tolerance) ||
            !nearlyEqual(actual.endSeconds, wanted.endSeconds, tolerance) ||
            !nearlyEqual(actual.captureOffsetSeconds,
                         wanted.captureOffsetSeconds, tolerance)) {
            return false;
        }
    }
    return true;
}

struct TimelineRange {
    double start = 0.0;
    double end = 0.0;
};

bool overlaps(const TimelineRange& left, const TimelineRange& right) {
    return left.start < right.end && left.end > right.start;
}

std::optional<double> knownClipLength(const ClipModel& clip) {
    if (!std::isfinite(clip.durationSeconds) || clip.durationSeconds < 0.0 ||
        !std::isfinite(clip.offsetSeconds) || clip.offsetSeconds < 0.0) {
        return std::nullopt;
    }
    if (clip.durationSeconds > 0.0) return clip.durationSeconds;
    if (clip.kind != ClipKind::Audio || clip.asset.sampleRate <= 0.0 ||
        clip.asset.frames == 0) {
        return std::nullopt;
    }
    const double remaining =
        double(clip.asset.frames) / clip.asset.sampleRate - clip.offsetSeconds;
    return std::isfinite(remaining) && remaining > 0.0
               ? std::optional<double>(remaining)
               : std::nullopt;
}

bool rangeIsEmptyInTrack(const TrackModel& track, const TimelineRange& range,
                         std::string& reason) {
    for (const ClipModel& clip : track.clips) {
        if (!std::isfinite(clip.startSeconds) || clip.startSeconds < 0.0) {
            reason = "target track contains invalid clip geometry";
            return false;
        }
        if (clip.startSeconds >= range.end) continue;
        const std::optional<double> length = knownClipLength(clip);
        if (!length) {
            reason = "target range may overlap a clip with unknown length";
            return false;
        }
        if (overlaps(range,
                     TimelineRange{clip.startSeconds,
                                   clip.startSeconds + *length})) {
            reason = clip.kind == ClipKind::Audio
                         ? "recording has an existing punch target"
                         : "recording overlaps existing material";
            return false;
        }
    }
    return true;
}

bool liveClipAnchor(const TrackModel& track, std::string_view afterId) {
    if (afterId.empty()) return true;
    return std::any_of(track.clips.begin(), track.clips.end(),
                       [&](const ClipModel& clip) {
                           return clip.id == afterId;
                       });
}

bool reserveEntityId(const std::string& id, std::set<std::string>& ids) {
    return canonicalUuid(id) && ids.insert(id).second;
}

bool validAnchoredChain(const std::vector<RecordingCommitAnchoredId>& chain,
                        std::string_view firstAfterId,
                        std::set<std::string>& entityIds) {
    for (std::size_t index = 0; index < chain.size(); ++index) {
        const RecordingCommitAnchoredId& item = chain[index];
        const std::string_view expectedAfter =
            index == 0 ? firstAfterId : std::string_view(chain[index - 1].id);
        if (!reserveEntityId(item.id, entityIds) ||
            !optionalCanonicalUuid(item.afterId) ||
            item.afterId != expectedAfter) {
            return false;
        }
    }
    return true;
}

struct CompPiece {
    std::size_t passIndex = 0;
    double start = 0.0;
    double end = 0.0;
};

std::vector<CompPiece> finalCompPieces(
    const CloudRecordingCapture& capture, double runStart, double runEnd) {
    if (!capture.semantics.trimTakesToRegion) {
        return {{capture.passes.size() - 1, 0.0, runEnd - runStart}};
    }

    std::vector<CompPiece> pieces;
    for (std::size_t passIndex = 0; passIndex < capture.passes.size();
         ++passIndex) {
        const CloudRecordingPass& pass = capture.passes[passIndex];
        const double from = pass.startSeconds - runStart;
        const double to = pass.endSeconds - runStart;
        std::vector<CompPiece> next;
        next.reserve(pieces.size() + 2);
        for (const CompPiece& piece : pieces) {
            if (piece.end <= from || piece.start >= to) {
                next.push_back(piece);
                continue;
            }
            if (from - piece.start >= kMinimumCompSegmentSeconds)
                next.push_back({piece.passIndex, piece.start, from});
            if (piece.end - to >= kMinimumCompSegmentSeconds)
                next.push_back({piece.passIndex, to, piece.end});
        }
        next.push_back({passIndex, from, to});
        std::sort(next.begin(), next.end(), [](const CompPiece& left,
                                               const CompPiece& right) {
            return left.start < right.start;
        });

        pieces.clear();
        for (const CompPiece& piece : next) {
            if (piece.end - piece.start < kMinimumCompSegmentSeconds)
                continue;
            if (!pieces.empty() &&
                pieces.back().passIndex == piece.passIndex &&
                std::abs(pieces.back().end - piece.start) <
                    kMinimumCompSegmentSeconds) {
                pieces.back().end = piece.end;
            } else {
                pieces.push_back(piece);
            }
        }
    }
    return pieces;
}

ProjectCommand child(CommandBody body) {
    ProjectCommand command;
    command.body = std::move(body);
    return command;
}

} // namespace

RecordingCommitPreflightResult RecordingCommitPlanner::preflight(
    const SharedProjectDocument& snapshot,
    const CloudRecordingRecoveryRun& frozenRun,
    const RecordingCommitPreflightInput& input) {
    if (!canonicalUuid(frozenRun.runId) || !canonicalUuid(frozenRun.opId) ||
        !canonicalUuid(frozenRun.transactionId) ||
        frozenRun.createdAtUnixMs <= 0 || frozenRun.captures.empty() ||
        frozenRun.captures.size() > kMaxRecordingCommitCaptures ||
        frozenRun.commitPhase != recovery::CloudRecordingCommitPhase::Pending) {
        return preflightFailure(RecordingCommitPlanCode::InvalidInput,
                                "frozen recovery run is invalid");
    }
    if (frozenRun.recoveryOnly || frozenRun.lostLease) {
        return preflightFailure(
            RecordingCommitPlanCode::UnsafeRecovery,
            "recovery-only or lease-lost audio cannot be committed");
    }
    if (input.baseServerSequence != snapshot.confirmedSequence) {
        return preflightFailure(
            RecordingCommitPlanCode::SnapshotConflict,
            "recording base sequence is not the snapshot head");
    }
    if (snapshot.appliedOperationIds.contains(frozenRun.opId)) {
        return preflightFailure(
            RecordingCommitPlanCode::SnapshotConflict,
            "recording operation is already materialized");
    }
    if (input.leases.size() != frozenRun.captures.size()) {
        return preflightFailure(RecordingCommitPlanCode::InvalidInput,
                                "capture and lease counts do not match");
    }

    // This is intentionally ahead of UUID maps, pass reconstruction and
    // timeline scans. Layers also needs one command for its frozen container
    // crossfade, so a size-only 3N+1 worst-case budget is a deterministic
    // pre-upload rejection and cannot be amplified into the quadratic comp
    // construction below.
    std::size_t worstCaseChildCommands = 0;
    for (const CloudRecordingCapture& capture : frozenRun.captures) {
        if (capture.semantics.mode != CloudRecordingMode::Overwrite &&
            capture.semantics.mode != CloudRecordingMode::Layers) {
            return preflightFailure(
                RecordingCommitPlanCode::InvalidInput,
                "frozen recording mode is invalid");
        }
        const std::size_t passCount = capture.passes.size();
        if (passCount > kMaxRecordingCommitPassesPerCapture) {
            return preflightFailure(
                RecordingCommitPlanCode::UnsupportedSemantics,
                "recording capture exceeds the bounded pass limit");
        }
        // Layers can produce one container, one frozen-crossfade property,
        // N takes and at most (2N - 1) comp pieces: 3N + 1 children. Overwrite
        // remains at most 3N, so this is a safe common pre-upload bound.
        const std::size_t captureBudget = passCount * 3 + 1;
        if (captureBudget >
            kMaxProjectCommandBatchSize - worstCaseChildCommands) {
            return preflightFailure(
                RecordingCommitPlanCode::UnsupportedSemantics,
                "recording commit worst-case exceeds the atomic batch limit");
        }
        worstCaseChildCommands += captureBudget;
    }

    std::unordered_map<std::string, std::string> leaseByTrack;
    std::set<std::string> leaseIds;
    for (const RecordingLeaseClaim& lease : input.leases) {
        if (!canonicalUuid(lease.trackId) || !canonicalUuid(lease.leaseId) ||
            !leaseByTrack.emplace(lease.trackId, lease.leaseId).second ||
            !leaseIds.insert(lease.leaseId).second) {
            return preflightFailure(
                RecordingCommitPlanCode::InvalidInput,
                "recording lease identity is invalid or duplicated");
        }
    }

    std::vector<const CloudRecordingCapture*> captures;
    captures.reserve(frozenRun.captures.size());
    std::set<std::string> captureIds;
    std::set<std::string> trackIds;
    std::set<std::string> frozenLeaseIds;
    std::set<std::string> uploadIds;
    std::set<std::string> assetIds;
    bool rebindsFrozenLease = false;
    for (const CloudRecordingCapture& capture : frozenRun.captures) {
        if (!canonicalUuid(capture.captureId) ||
            !canonicalUuid(capture.trackId) ||
            !canonicalUuid(capture.leaseId) ||
            !canonicalUuid(capture.uploadId) ||
            !canonicalUuid(capture.assetId) ||
            !captureIds.insert(capture.captureId).second ||
            !trackIds.insert(capture.trackId).second ||
            !frozenLeaseIds.insert(capture.leaseId).second ||
            !uploadIds.insert(capture.uploadId).second ||
            !assetIds.insert(capture.assetId).second) {
            return preflightFailure(
                RecordingCommitPlanCode::InvalidInput,
                "frozen capture identities are invalid or duplicated");
        }
        const auto suppliedLease = leaseByTrack.find(capture.trackId);
        if (suppliedLease == leaseByTrack.end()) {
            return preflightFailure(
                RecordingCommitPlanCode::InvalidInput,
                "caller lease is missing for a frozen capture");
        }
        rebindsFrozenLease = rebindsFrozenLease ||
                             suppliedLease->second != capture.leaseId;
        captures.push_back(&capture);
    }

    // A restarted client cannot infer absence from appliedOperationIds: older
    // operations may have fallen outside the snapshot's reconstructed heads.
    // Rebinding is therefore allowed only after the operation-status endpoint
    // observed this exact op absent at the exact snapshot/base head. A
    // recovery-only or explicitly lost-lease run remains ineligible above and
    // must land through the separate recovery-track workflow.
    if (rebindsFrozenLease) {
        const auto& proof = input.operationAbsenceProof;
        if (!proof || !canonicalUuid(proof->operationId) ||
            proof->operationId != frozenRun.opId ||
            proof->observedServerSequence != snapshot.confirmedSequence ||
            proof->observedServerSequence != input.baseServerSequence) {
            return preflightFailure(
                RecordingCommitPlanCode::SnapshotConflict,
                "fresh recording leases require authoritative operation "
                "absence at the exact snapshot head");
        }
    }
    std::sort(captures.begin(), captures.end(),
              [](const CloudRecordingCapture* left,
                 const CloudRecordingCapture* right) {
                  return left->trackId < right->trackId;
              });

    for (const CloudRecordingCapture* capturePointer : captures) {
        const CloudRecordingCapture& capture = *capturePointer;
        if (capture.uploadPhase !=
                recovery::CloudRecordingUploadPhase::Captured ||
            capture.status != CloudRecordingCaptureStatus::Ready) {
            return preflightFailure(RecordingCommitPlanCode::InvalidInput,
                                    "capture is not a ready recording");
        }
        if (!capture.semantics.complete) {
            return preflightFailure(
                RecordingCommitPlanCode::UnsupportedSemantics,
                "frozen recording semantics are incomplete");
        }
        const CloudRecordingSemantics& semantics = capture.semantics;
        if (!boundedFinite(semantics.loopStartSeconds, 0.0,
                           kMaximumTimeSeconds) ||
            !boundedFinite(semantics.loopEndSeconds, 0.0,
                           kMaximumTimeSeconds) ||
            !boundedFinite(semantics.compCrossfadeMs, 0.0, 20.0) ||
            (semantics.loopEnabled &&
             semantics.loopEndSeconds <= semantics.loopStartSeconds)) {
            return preflightFailure(
                RecordingCommitPlanCode::InvalidInput,
                "frozen recording semantics are invalid");
        }
        if (!boundedFinite(capture.startSeconds, 0.0,
                           kMaximumTimeSeconds) ||
            !boundedFinite(capture.durationSeconds, 0.0,
                           kMaximumTimeSeconds, true) ||
            !boundedFinite(capture.sampleRate, 0.0, kMaximumSampleRate,
                           true) ||
            capture.channels == 0 || capture.channels > kMaximumChannels ||
            capture.frames == 0 || !validFrozenPasses(capture)) {
            return preflightFailure(
                RecordingCommitPlanCode::InvalidInput,
                "ready capture metadata or pass geometry is invalid");
        }

        const TrackModel* track = snapshot.project.findTrack(capture.trackId);
        if (!track || track->kind != TrackKind::Audio ||
            snapshot.deletedTracks.contains(capture.trackId)) {
            return preflightFailure(
                RecordingCommitPlanCode::SnapshotConflict,
                "recording target is not a live audio track");
        }

        if (semantics.mode == CloudRecordingMode::Overwrite) {
            std::vector<TimelineRange> passRanges;
            passRanges.reserve(capture.passes.size());
            for (const CloudRecordingPass& pass : capture.passes) {
                passRanges.push_back({pass.startSeconds, pass.endSeconds});
                std::string reason;
                if (!rangeIsEmptyInTrack(*track, passRanges.back(), reason)) {
                    return preflightFailure(
                        RecordingCommitPlanCode::SnapshotConflict,
                        std::move(reason));
                }
            }
            std::sort(passRanges.begin(), passRanges.end(),
                      [](const TimelineRange& left,
                         const TimelineRange& right) {
                          return left.start < right.start;
                      });
            for (std::size_t index = 1; index < passRanges.size(); ++index) {
                if (overlaps(passRanges[index - 1], passRanges[index])) {
                    return preflightFailure(
                        RecordingCommitPlanCode::UnsupportedSemantics,
                        "overwrite passes would have to delete one another");
                }
            }
            continue;
        }

        double runStart = std::numeric_limits<double>::max();
        double runEnd = 0.0;
        for (const CloudRecordingPass& pass : capture.passes) {
            runStart = std::min(runStart, pass.startSeconds);
            runEnd = std::max(runEnd, pass.endSeconds);
        }
        std::string overlapReason;
        if (!rangeIsEmptyInTrack(*track, {runStart, runEnd},
                                 overlapReason)) {
            return preflightFailure(RecordingCommitPlanCode::SnapshotConflict,
                                    std::move(overlapReason));
        }
    }

    return preflightReady();
}

RecordingCommitPlanResult RecordingCommitPlanner::plan(
    const SharedProjectDocument& snapshot,
    const CloudRecordingRecoveryRun& frozenRun,
    const RecordingCommitPlanInput& input) {
    const CommandMeta& meta = input.meta;
    if (meta.schemaVersion != kProjectCommandSchemaVersion ||
        !canonicalUuid(meta.projectId) || !canonicalUuid(meta.operationId) ||
        !canonicalUuid(meta.transactionId) || meta.serverSequence != 0 ||
        meta.operationId != frozenRun.opId ||
        meta.transactionId != frozenRun.transactionId) {
        return failure(RecordingCommitPlanCode::InvalidInput,
                       "command metadata does not match the frozen run");
    }

    RecordingCommitPreflightInput preflightInput;
    preflightInput.baseServerSequence = meta.baseServerSequence;
    preflightInput.leases = input.leases;
    preflightInput.operationAbsenceProof = input.operationAbsenceProof;
    const RecordingCommitPreflightResult preflightResult =
        preflight(snapshot, frozenRun, preflightInput);
    if (!preflightResult) {
        return failure(preflightResult.code, preflightResult.message);
    }

    if (input.captures.size() != frozenRun.captures.size()) {
        return failure(RecordingCommitPlanCode::InvalidInput,
                       "capture and asset counts do not match");
    }

    std::unordered_map<std::string, const RecordingCommitCaptureInput*>
        inputsByCapture;
    for (const RecordingCommitCaptureInput& captureInput : input.captures) {
        if (!canonicalUuid(captureInput.captureId) ||
            !inputsByCapture.emplace(captureInput.captureId, &captureInput)
                 .second) {
            return failure(RecordingCommitPlanCode::InvalidInput,
                           "capture input identity is invalid or duplicated");
        }
    }

    std::vector<const CloudRecordingCapture*> captures;
    captures.reserve(frozenRun.captures.size());
    for (const CloudRecordingCapture& capture : frozenRun.captures) {
        if (!inputsByCapture.contains(capture.captureId)) {
            return failure(RecordingCommitPlanCode::InvalidInput,
                           "uploaded asset is not bound to a frozen capture");
        }
        captures.push_back(&capture);
    }
    std::sort(captures.begin(), captures.end(),
              [](const CloudRecordingCapture* left,
                 const CloudRecordingCapture* right) {
                  return left->trackId < right->trackId;
              });

    auto batch = std::make_shared<BatchCommand>();
    std::vector<RecordingLeaseClaim> canonicalLeases;
    canonicalLeases.reserve(captures.size());
    std::unordered_map<std::string, std::string> currentLeaseByTrack;
    currentLeaseByTrack.reserve(input.leases.size());
    for (const RecordingLeaseClaim& lease : input.leases)
        currentLeaseByTrack.emplace(lease.trackId, lease.leaseId);
    std::set<std::string> entityIds;

    const auto append = [&](ProjectCommand command) {
        if (batch->commands.size() >= kMaxProjectCommandBatchSize)
            return false;
        batch->commands.push_back(std::move(command));
        return true;
    };

    for (const CloudRecordingCapture* capturePointer : captures) {
        const CloudRecordingCapture& capture = *capturePointer;
        const RecordingCommitCaptureInput& captureInput =
            *inputsByCapture.at(capture.captureId);
        // The authority validates the leases present in the final command.
        // For a proven restart/rebind these are the freshly acquired current
        // lease ids, never the stale ids frozen with the original capture.
        canonicalLeases.push_back(
            {capture.trackId, currentLeaseByTrack.at(capture.trackId)});
        const CloudRecordingSemantics& semantics = capture.semantics;

        const AssetRef& asset = captureInput.uploadedAsset;
        const double tolerance = captureTolerance(capture);
        if (!completeUploadedAudio(asset) || asset.assetId != capture.assetId ||
            asset.channels != capture.channels ||
            asset.frames != capture.frames ||
            !nearlyEqual(asset.sampleRate, capture.sampleRate, tolerance) ||
            !nearlyEqual(double(asset.frames) / asset.sampleRate,
                         capture.durationSeconds, tolerance)) {
            return failure(RecordingCommitPlanCode::InvalidInput,
                           "uploaded AssetRef does not match the ready capture");
        }

        const TrackModel* track = snapshot.project.findTrack(capture.trackId);
        if (!track) {
            return failure(RecordingCommitPlanCode::SnapshotConflict,
                           "recording target is not a live audio track");
        }

        const std::string clipName =
            asset.originalName.empty() ? "Recording" : asset.originalName;
        if (semantics.mode == CloudRecordingMode::Overwrite) {
            if (!captureInput.containerClipId.empty() ||
                !captureInput.containerClipAfterId.empty() ||
                !captureInput.takes.empty() ||
                !captureInput.compSegments.empty() ||
                captureInput.clips.size() != capture.passes.size()) {
                return failure(RecordingCommitPlanCode::InvalidInput,
                               "overwrite identities do not match its passes");
            }
            const std::string firstAnchor =
                captureInput.clips.empty()
                    ? std::string()
                    : captureInput.clips.front().afterId;
            if (!optionalCanonicalUuid(firstAnchor) ||
                !liveClipAnchor(*track, firstAnchor) ||
                !validAnchoredChain(captureInput.clips, firstAnchor,
                                    entityIds)) {
                return failure(RecordingCommitPlanCode::InvalidInput,
                               "overwrite clip ids or anchors are invalid");
            }

            for (std::size_t index = 0; index < capture.passes.size();
                 ++index) {
                const CloudRecordingPass& pass = capture.passes[index];
                const RecordingCommitAnchoredId& ids =
                    captureInput.clips[index];
                if (!append(child(AddClip{capture.trackId, ids.id,
                                          ClipKind::Audio, clipName,
                                          pass.startSeconds,
                                          pass.endSeconds - pass.startSeconds,
                                          track->color, ids.afterId}))) {
                    return failure(RecordingCommitPlanCode::UnsupportedSemantics,
                                   "recording commit exceeds batch limit");
                }
                if (pass.captureOffsetSeconds > 0.0 &&
                    !append(child(SetClipProperty{
                        capture.trackId, ids.id,
                        ClipProperty::OffsetSeconds,
                        pass.captureOffsetSeconds}))) {
                    return failure(RecordingCommitPlanCode::UnsupportedSemantics,
                                   "recording commit exceeds batch limit");
                }
                if (!append(child(SetClipAsset{capture.trackId, ids.id,
                                               asset}))) {
                    return failure(RecordingCommitPlanCode::UnsupportedSemantics,
                                   "recording commit exceeds batch limit");
                }
            }
            continue;
        }

        if (!captureInput.clips.empty() ||
            !canonicalUuid(captureInput.containerClipId) ||
            !optionalCanonicalUuid(captureInput.containerClipAfterId) ||
            !liveClipAnchor(*track, captureInput.containerClipAfterId) ||
            !reserveEntityId(captureInput.containerClipId, entityIds) ||
            captureInput.takes.size() != capture.passes.size() ||
            !validAnchoredChain(captureInput.takes, {}, entityIds)) {
            return failure(RecordingCommitPlanCode::InvalidInput,
                           "layer container, take ids or anchors are invalid");
        }

        double runStart = std::numeric_limits<double>::max();
        double runEnd = 0.0;
        for (const CloudRecordingPass& pass : capture.passes) {
            runStart = std::min(runStart, pass.startSeconds);
            runEnd = std::max(runEnd, pass.endSeconds);
        }
        const std::vector<CompPiece> pieces =
            finalCompPieces(capture, runStart, runEnd);
        if (pieces.empty() ||
            captureInput.compSegments.size() != pieces.size() ||
            !validAnchoredChain(captureInput.compSegments, {}, entityIds)) {
            return failure(RecordingCommitPlanCode::InvalidInput,
                           "layer comp ids or anchors do not match final comp");
        }

        // A layers clip is a source-less container: its canonical channel
        // count remains zero, while every cloud-native TakeModel below owns
        // the validated channel metadata used by playback/serialization.
        if (!append(child(AddClip{capture.trackId,
                                  captureInput.containerClipId,
                                  ClipKind::Audio, clipName, runStart,
                                  runEnd - runStart, track->color,
                                  captureInput.containerClipAfterId}))) {
            return failure(RecordingCommitPlanCode::UnsupportedSemantics,
                           "recording commit exceeds batch limit");
        }
        if (!append(child(SetClipProperty{
                capture.trackId, captureInput.containerClipId,
                ClipProperty::CompCrossfadeMs,
                semantics.compCrossfadeMs}))) {
            return failure(RecordingCommitPlanCode::UnsupportedSemantics,
                           "recording commit exceeds batch limit");
        }
        for (std::size_t index = 0; index < capture.passes.size(); ++index) {
            const CloudRecordingPass& pass = capture.passes[index];
            const RecordingCommitAnchoredId& ids = captureInput.takes[index];
            TakeModel take;
            take.id = ids.id;
            take.name = "Take " + std::to_string(index + 1);
            take.offsetSeconds = pass.captureOffsetSeconds;
            take.lengthSeconds = pass.endSeconds - pass.startSeconds;
            take.clipOffsetSeconds = pass.startSeconds - runStart;
            take.gain = 1.0f;
            take.channels = int(capture.channels);
            take.color = track->color;
            take.asset = asset;
            if (!append(child(AddTake{capture.trackId,
                                      captureInput.containerClipId,
                                      std::move(take), ids.afterId}))) {
                return failure(RecordingCommitPlanCode::UnsupportedSemantics,
                               "recording commit exceeds batch limit");
            }
        }
        for (std::size_t index = 0; index < pieces.size(); ++index) {
            const CompPiece& piece = pieces[index];
            const RecordingCommitAnchoredId& ids =
                captureInput.compSegments[index];
            CompSegment segment;
            segment.id = ids.id;
            segment.takeId = captureInput.takes[piece.passIndex].id;
            segment.startSeconds = piece.start;
            segment.endSeconds = piece.end;
            if (!append(child(UpsertCompSegment{
                    capture.trackId, captureInput.containerClipId,
                    std::move(segment), ids.afterId}))) {
                return failure(RecordingCommitPlanCode::UnsupportedSemantics,
                               "recording commit exceeds batch limit");
            }
        }
    }

    ProjectCommand command;
    command.meta = input.meta;
    command.body = RecordingCommit{std::move(canonicalLeases),
                                   std::move(batch)};
    std::string idError;
    if (commandKind(command) != "recording.commit" ||
        !commandHasValidIds(command, &idError)) {
        return failure(RecordingCommitPlanCode::InvalidInput,
                       idError.empty() ? "planned command is not canonical"
                                       : std::move(idError));
    }

    SharedProjectDocument candidate = snapshot;
    const ApplyResult applied = ProjectReducer::apply(candidate, command);
    if (!applied.changed()) {
        return failure(RecordingCommitPlanCode::SnapshotConflict,
                       "planned recording command is not reducible: " +
                           applied.message);
    }

    RecordingCommitPlanResult result;
    result.code = RecordingCommitPlanCode::Planned;
    result.command = std::move(command);
    return result;
}

} // namespace daw::collab
