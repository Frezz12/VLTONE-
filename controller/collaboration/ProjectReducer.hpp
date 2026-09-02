#pragma once

#include "collaboration/CollaborationState.hpp"
#include "collaboration/ProjectCommand.hpp"

#include <memory>
#include <set>
#include <string>

namespace daw::collab {

enum class ApplyCode : std::uint8_t {
    Applied,
    NoChange,
    Duplicate,
    InvalidCommand,
    PreconditionsFailed,
    MissingEntity,
    DeletedEntity,
    MissingAnchor,
    Unsupported,
};

struct ChangeImpact {
    bool fullProjection = false;
    bool documentChanged = false;
    bool graphRebuild = false;
    bool timelineChanged = false;
    bool transportProjectionChanged = false;
    bool masterGainChanged = false;
    std::set<std::string> trackIds;
    std::set<std::string> clipIds;
    std::set<std::string> noteIds;
    std::set<std::string> automationPointIds;
    std::set<std::string> controllerLaneIds;
    std::set<std::string> takeIds;
    std::set<std::string> compSegmentIds;
    std::set<std::string> sendIds;
    std::set<std::string> pluginInsertIds;
    std::set<std::string> fieldKeys;

    void merge(const ChangeImpact& other);
};

struct ApplyResult {
    ApplyCode code = ApplyCode::InvalidCommand;
    std::string message;
    ChangeImpact impact;
    /// A typed command guarded by generic field-writer preconditions. The caller
    /// supplies a fresh envelope before submitting it as an undo operation.
    std::shared_ptr<ProjectCommand> inverse;

    bool accepted() const noexcept {
        return code == ApplyCode::Applied || code == ApplyCode::NoChange ||
               code == ApplyCode::Duplicate;
    }
    bool changed() const noexcept { return code == ApplyCode::Applied; }
};

class ProjectReducer {
public:
    /// Pure deterministic mutation: no engine, filesystem, UUID generation or
    /// network access. A rejected batch leaves `state` byte-for-byte unchanged.
    static ApplyResult apply(SharedProjectDocument& state,
                             const ProjectCommand& command);

    static std::string projectFieldKey(ProjectScalar field);
    static std::string trackFieldKey(const std::string& trackId,
                                     TrackProperty property);
    static std::string trackPositionKey(const std::string& trackId);
    static std::string trackLifecycleKey(const std::string& trackId);
    static std::string clipFieldKey(const std::string& clipId,
                                    ClipProperty property);
    static std::string clipPositionKey(const std::string& clipId);
    static std::string clipLifecycleKey(const std::string& clipId);
    /// Coarse collection revision used only to guard operations that replace
    /// an entire clip. Descendant commands write it, but their own inverses
    /// remain guarded by their granular entity fields.
    static std::string clipDescendantsKey(const std::string& clipId);
    static std::string notePositionKey(const std::string& noteId);
    static std::string noteLifecycleKey(const std::string& noteId);
    static std::string automationPointPositionKey(const std::string& pointId);
    static std::string automationPointLifecycleKey(const std::string& pointId);
    static std::string controllerLanePositionKey(const std::string& laneId);
    static std::string controllerLaneLifecycleKey(const std::string& laneId);
    static std::string takePositionKey(const std::string& takeId);
    static std::string takeLifecycleKey(const std::string& takeId);
    static std::string compSegmentPositionKey(const std::string& segmentId);
    static std::string compSegmentLifecycleKey(const std::string& segmentId);
    static std::string sendPositionKey(const std::string& sendId);
    static std::string sendLifecycleKey(const std::string& sendId);
    static std::string pluginPositionKey(const std::string& insertId);
    static std::string pluginLifecycleKey(const std::string& insertId);
};

} // namespace daw::collab
