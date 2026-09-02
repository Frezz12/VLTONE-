#pragma once

#include "collaboration/ProjectReducer.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace daw::collab {

struct PreparedHistoryCommand {
    std::uint64_t token = 0;
    ProjectCommand command;
    std::string label;
};

/// Actor-local undo/redo. It never rewinds a snapshot: reducer-produced inverse
/// commands carry field-writer preconditions and are submitted like any
/// other durable operation. Stack movement happens only after apply/ack success.
class ConditionalUndoHistory {
public:
    void record(const ProjectCommand& forward, const ApplyResult& applied,
                std::string label);

    std::optional<PreparedHistoryCommand> prepareUndo(CommandMeta freshMeta);
    std::optional<PreparedHistoryCommand> prepareRedo(CommandMeta freshMeta);

    /// `applied` must be the result of the prepared command. On conflict the
    /// entry remains on its original stack and can be retried or discarded.
    bool complete(std::uint64_t token, const ApplyResult& applied);
    void cancel(std::uint64_t token);

    bool canUndo() const noexcept;
    bool canRedo() const noexcept;
    std::size_t undoDepth() const noexcept { return m_undo.size(); }
    std::size_t redoDepth() const noexcept { return m_redo.size(); }
    void clear();

private:
    struct Entry {
        std::string label;
        ProjectCommand undoCommand;
        std::optional<ProjectCommand> redoCommand;
    };
    enum class Direction : std::uint8_t { Undo, Redo };
    struct Pending {
        std::uint64_t token = 0;
        Direction direction = Direction::Undo;
    };

    static ProjectCommand withFreshMeta(const ProjectCommand& command,
                                        CommandMeta freshMeta);
    std::uint64_t m_nextToken = 1;
    std::vector<Entry> m_undo;
    std::vector<Entry> m_redo;
    std::optional<Pending> m_pending;
};

} // namespace daw::collab
