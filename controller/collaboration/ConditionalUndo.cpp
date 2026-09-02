#include "collaboration/ConditionalUndo.hpp"

namespace daw::collab {

void ConditionalUndoHistory::record(const ProjectCommand& forward,
                                    const ApplyResult& applied,
                                    std::string label) {
    if (!applied.changed() || !applied.inverse || m_pending) return;
    Entry entry;
    entry.label = std::move(label);
    entry.undoCommand = *applied.inverse;
    // Preserve the original transaction as a diagnostic even though redo will
    // be regenerated from the successful undo's typed inverse.
    if (entry.undoCommand.meta.transactionId.empty())
        entry.undoCommand.meta.transactionId = forward.meta.operationId;
    m_undo.push_back(std::move(entry));
    m_redo.clear();
}

ProjectCommand ConditionalUndoHistory::withFreshMeta(
    const ProjectCommand& command, CommandMeta freshMeta) {
    ProjectCommand prepared = command;
    const std::string originatingTransaction = command.meta.transactionId;
    prepared.meta = std::move(freshMeta);
    prepared.meta.schemaVersion = kProjectCommandSchemaVersion;
    if (prepared.meta.transactionId.empty())
        prepared.meta.transactionId = originatingTransaction;
    return prepared;
}

std::optional<PreparedHistoryCommand>
ConditionalUndoHistory::prepareUndo(CommandMeta freshMeta) {
    if (m_pending || m_undo.empty()) return std::nullopt;
    const std::uint64_t token = m_nextToken++;
    m_pending = Pending{token, Direction::Undo};
    return PreparedHistoryCommand{
        token, withFreshMeta(m_undo.back().undoCommand, std::move(freshMeta)),
        m_undo.back().label};
}

std::optional<PreparedHistoryCommand>
ConditionalUndoHistory::prepareRedo(CommandMeta freshMeta) {
    if (m_pending || m_redo.empty() || !m_redo.back().redoCommand)
        return std::nullopt;
    const std::uint64_t token = m_nextToken++;
    m_pending = Pending{token, Direction::Redo};
    return PreparedHistoryCommand{
        token, withFreshMeta(*m_redo.back().redoCommand, std::move(freshMeta)),
        m_redo.back().label};
}

bool ConditionalUndoHistory::complete(std::uint64_t token,
                                      const ApplyResult& applied) {
    if (!m_pending || m_pending->token != token) return false;
    const Pending pending = *m_pending;
    m_pending.reset();
    if (!applied.changed() || !applied.inverse) return false;

    if (pending.direction == Direction::Undo) {
        if (m_undo.empty()) return false;
        Entry entry = std::move(m_undo.back());
        m_undo.pop_back();
        entry.redoCommand = *applied.inverse;
        m_redo.push_back(std::move(entry));
    } else {
        if (m_redo.empty()) return false;
        Entry entry = std::move(m_redo.back());
        m_redo.pop_back();
        entry.undoCommand = *applied.inverse;
        entry.redoCommand.reset();
        m_undo.push_back(std::move(entry));
    }
    return true;
}

void ConditionalUndoHistory::cancel(std::uint64_t token) {
    if (m_pending && m_pending->token == token) m_pending.reset();
}

bool ConditionalUndoHistory::canUndo() const noexcept {
    return !m_pending && !m_undo.empty();
}

bool ConditionalUndoHistory::canRedo() const noexcept {
    return !m_pending && !m_redo.empty() && m_redo.back().redoCommand.has_value();
}

void ConditionalUndoHistory::clear() {
    m_pending.reset();
    m_undo.clear();
    m_redo.clear();
}

} // namespace daw::collab
