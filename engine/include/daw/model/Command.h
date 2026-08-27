#pragma once
//
// Команды правки проекта и стек undo/redo (§11).
//
// Любое изменение модели проекта (Session/Track/Clip) проходит через команду.
// Команда знает, как применить правку (redo) и как её откатить (undo), и
// хранит ровно то, что нужно для отката — не снимок всей сессии.
//
// Команда мутирует только модель. Перестройку аудио-графа делает слой,
// которому доступен Engine (см. Engine::execute / undo / redo): команда не
// знает про граф и RT-поток. Так модель и undo тестируются без Qt и без звука.
//
// coalesceWith склеивает поток мелких правок (перетаскивание клипа мышью
// генерирует десятки движений) в одну запись истории. Окно склейки открывает
// UI: setCoalesceWindow(true) на drag-start, false — на release.
//
// Лимит истории — по памяти, не по числу шагов (§11). При превышении бюджета
// низ undo-стека (самые старые записи) отбрасывается.
//

#include <cstddef>
#include <memory>
#include <vector>

namespace daw::model {

class Session;

class Command {
public:
    virtual ~Command() = default;

    virtual void redo(Session& s) = 0;
    virtual void undo(Session& s) = 0;

    // Вернуть true, если next — продолжение этой же правки и его можно
    // поглотить вместо отдельной записи. По умолчанию правки не склеиваются.
    virtual bool coalesceWith(const Command& next) = 0;

    // Имя для меню «Отменить: …» / «Вернуть: …». Строковый литерал, владения
    // нет — не освобождается.
    virtual const char* name() const noexcept = 0;

    // Оценка веса команды в памяти для лимита истории (§11). По умолчанию —
    // sizeof конкретного типа; команды, хранящие shared_ptr на Source,
    // переопределяют и учитывают его. Точная цифра не нужна — важна пропорция.
    virtual std::size_t byteSize() const noexcept = 0;
};

class UndoStack {
public:
    UndoStack() = default;

    // Применить правку: вызвать cmd->redo(s) и положить команду в историю.
    // При открытом окне склейки, если верхняя запись undo_ поглощает cmd,
    // redo новой не зовётся (правка уже применена «вживую» во время drag) —
    // поглощённая команда просто заменяет новую на вершине.
    void execute(std::unique_ptr<Command> cmd, Session& s);

    void undo(Session& s);
    void redo(Session& s);

    bool canUndo() const noexcept { return !undo_.empty(); }
    bool canRedo() const noexcept { return !redo_.empty(); }

    const char* nextUndoName() const noexcept;
    const char* nextRedoName() const noexcept;

    // Любая новая правка или переход к другому проекту очищает историю.
    void clear() noexcept;

    // Окно склейки: открывается UI на начало перетаскивания. Вне окна каждая
    // правка — отдельная запись, даже если coalesceWith вернул бы true.
    void setCoalesceWindow(bool on) noexcept { coalesce_ = on; }
    bool coalesceWindow() const noexcept { return coalesce_; }

    std::size_t size() const noexcept { return undo_.size(); }
    std::size_t redoSize() const noexcept { return redo_.size(); }

    // Бюджет истории в байтах (эвристика по sizeof команды). При превышении
    // низ undo-стека отбрасывается. 0 — без лимита.
    void setMemoryBudget(std::size_t bytes) noexcept { budget_ = bytes; }
    std::size_t memoryBudget() const noexcept { return budget_; }

private:
    void enforceBudget();

    std::vector<std::unique_ptr<Command>> undo_;
    std::vector<std::unique_ptr<Command>> redo_;
    bool         coalesce_ = false;
    std::size_t  budget_   = 0;   // 0 = без лимита
    std::size_t  usedBytes_ = 0;
};

} // namespace daw::model
