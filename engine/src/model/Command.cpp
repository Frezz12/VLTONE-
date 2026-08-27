#include "daw/model/Command.h"
#include "daw/model/Session.h"

#include <utility>

namespace daw::model {

void UndoStack::execute(std::unique_ptr<Command> cmd, Session& s)
{
    if (!cmd)
        return;

    // Окно склейки открыто и верхняя запись готова поглотить новую правку:
    // redo новой НЕ зовём — её эффект уже применён в модель во время drag.
    if (coalesce_ && !undo_.empty() && undo_.back()->coalesceWith(*cmd))
        return;

    cmd->redo(s);

    // Любая новая правка обрубает ветку redo: вернуть отброшенное нельзя.
    redo_.clear();

    usedBytes_ += cmd->byteSize();
    undo_.push_back(std::move(cmd));
    enforceBudget();
}

void UndoStack::undo(Session& s)
{
    if (undo_.empty())
        return;

    auto cmd = std::move(undo_.back());
    undo_.pop_back();
    usedBytes_ -= cmd->byteSize();

    cmd->undo(s);
    redo_.push_back(std::move(cmd));
}

void UndoStack::redo(Session& s)
{
    if (redo_.empty())
        return;

    auto cmd = std::move(redo_.back());
    redo_.pop_back();

    cmd->redo(s);

    usedBytes_ += cmd->byteSize();
    undo_.push_back(std::move(cmd));
    enforceBudget();
}

const char* UndoStack::nextUndoName() const noexcept
{
    return undo_.empty() ? "" : undo_.back()->name();
}

const char* UndoStack::nextRedoName() const noexcept
{
    return redo_.empty() ? "" : redo_.back()->name();
}

void UndoStack::clear() noexcept
{
    undo_.clear();
    redo_.clear();
    usedBytes_ = 0;
}

void UndoStack::enforceBudget()
{
    // Лимит по памяти: выбрасываем самые старые записи, пока не уложимся.
    // 0 — без лимита.
    while (budget_ > 0 && usedBytes_ > budget_ && undo_.size() > 1) {
        usedBytes_ -= undo_.front()->byteSize();
        undo_.erase(undo_.begin());
    }
}

} // namespace daw::model
