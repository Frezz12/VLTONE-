#pragma once

#include "collaboration/CollaborationState.hpp"
#include "Core/Result.hpp"

#include <string>
#include <string_view>

namespace daw::collab {

inline constexpr int kSharedProjectSnapshotSchemaVersion = 1;
inline constexpr std::size_t kMaximumSharedProjectSnapshotBytes =
    128u * 1024u * 1024u;

/// Canonical cloud snapshot for the reducer, not merely ProjectModel. Delete
/// tombstones and field writers are required so replay and conditional undo
/// have the same meaning on a freshly joined client as on the host that wrote
/// the snapshot. Local paths/UI/session state must already have been projected
/// away by the caller.
audio::Result serializeSharedProjectSnapshot(
    const SharedProjectDocument& document, std::string& bytes);

audio::Result deserializeSharedProjectSnapshot(
    SharedProjectDocument& document, std::string_view bytes);

} // namespace daw::collab
