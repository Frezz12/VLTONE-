#pragma once

#include "collaboration/ProjectCommand.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace daw::collab {

/// Result of offering a document mutation to the collaboration command path.
///
/// The sink is deliberately non-owning and synchronous: it must copy any
/// string views it needs before returning.  EngineController keeps its legacy
/// local behaviour only for LocalFallback.  Submitted means the command path
/// owns the change (including its optimistic projection), while Blocked is a
/// deliberate read-only/offline refusal.
enum class SharedMutationResult {
    LocalFallback,
    Submitted,
    Blocked,
};

/// Only a real local fallback belongs to the legacy file dirty/undo path.
/// Cloud submissions are projected by the collaboration gateway, while a
/// blocked mutation deliberately leaves the materialized document unchanged.
constexpr bool marksLocalFileDirty(SharedMutationResult result) noexcept {
    return result == SharedMutationResult::LocalFallback;
}

struct SharedMutationRequest {
    CommandBody body;
    std::string undoLabel;
    std::optional<std::string> transactionId;
};

/// Narrow controller-side seam for shared document edits already represented
/// by the typed collaboration reducer.  Command metadata, UUIDs and transport
/// policy belong to the application bridge, not to EngineController.
class SharedMutationSink {
public:
    virtual ~SharedMutationSink() = default;

    virtual bool handlesCloudBinding() = 0;
    virtual SharedMutationResult submit(SharedMutationRequest request) = 0;

    virtual SharedMutationResult setTimeSignature(int numerator,
                                                  int denominator) = 0;
    virtual SharedMutationResult setProjectKey(int root,
                                               std::string_view scaleId) = 0;
    virtual SharedMutationResult setAiInstructions(std::string_view text) = 0;
    virtual SharedMutationResult renameTrack(std::string_view trackId,
                                             std::string_view name) = 0;
    virtual SharedMutationResult setTrackMuted(std::string_view trackId,
                                               bool muted) = 0;

    /// One call represents one atomic mute gesture.  IDs are stable document
    /// IDs, already expanded/deduplicated by EngineController; no vector
    /// indices cross this seam.
    virtual SharedMutationResult setTracksMuted(
        std::span<const std::string> trackIds, bool muted) = 0;

    /// One call represents one atomic batch.  The ids are exactly the tracks
    /// muted in the controller's current materialized document.
    virtual SharedMutationResult clearAllMutes(
        std::span<const std::string> mutedTrackIds) = 0;
};

} // namespace daw::collab
