#pragma once
//
// Конкретные команды правки проекта (§11). Каждая знает, как применить и
// откатить одно конкретное изменение модели. Публикацией аудио-графа занимается
// Engine::execute/undo/redo — команда трогает только Session/Track/Clip.
//
// Координаты клипов — абсолютные сэмплы таймлайна (как в Clip::timelinePosition).
// Musical/linear (§6.2) — атрибут дорожки, для самой команды неразличимы: что
// хранит дорожка, то и двигаем.
//

#include <cstdint>
#include <memory>

#include "daw/model/Clip.h"
#include "daw/model/Command.h"
#include "daw/model/Track.h"

namespace daw::model {

class Session;

// Переместить клип на новую позицию таймлайна. Склеивает поток микродвижений
// при перетаскивании: если следующая MoveClipCommand того же (track, clip) —
// поглощает её, удерживая первоначальную позицию oldPos_.
class MoveClipCommand final : public Command {
public:
    MoveClipCommand(int trackIndex, int clipIndex,
                    std::int64_t newPos) noexcept
        : trackIndex_(trackIndex), clipIndex_(clipIndex),
          newPos_(newPos) {}

    void redo(Session& s) override;
    void undo(Session& s) override;

    bool coalesceWith(const Command& next) override;
    const char* name() const noexcept override { return "Переместить клип"; }
    std::size_t byteSize() const noexcept override { return sizeof(*this); }

    int trackIndex() const noexcept { return trackIndex_; }
    int clipIndex() const noexcept { return clipIndex_; }

private:
    int           trackIndex_ = 0;
    int           clipIndex_ = 0;
    std::int64_t  newPos_ = 0;
    std::int64_t  oldPos_ = -1;   // заполняется первым redo
};

// Разрезать клип в заданной абсолютной позиции таймлайна. Второй клип
// ссылается на тот же Source (бесплатно по памяти, §11), окно Source
// сдвигается на длину первого клипа. Undo — убрать второй, восстановить длину.
class SplitClipCommand final : public Command {
public:
    SplitClipCommand(int trackIndex, int clipIndex,
                     std::int64_t splitAtAbsolute) noexcept
        : trackIndex_(trackIndex), clipIndex_(clipIndex),
          splitAtAbsolute_(splitAtAbsolute) {}

    void redo(Session& s) override;
    void undo(Session& s) override;

    bool coalesceWith(const Command&) override { return false; }
    const char* name() const noexcept override { return "Разрезать клип"; }
    std::size_t byteSize() const noexcept override { return sizeof(*this); }

private:
    int           trackIndex_ = 0;
    int           clipIndex_ = 0;
    std::int64_t  splitAtAbsolute_ = 0;

    // Заполняется redo: индекс вставки второго клипа и его длина — нужно,
    // чтобы undo вынул именно этот клип и восстановил окно первого.
    int           secondClipIndex_ = -1;
    std::int64_t  originalLength_ = 0;
    std::int64_t  originalOffset_ = 0;
    std::int64_t  firstLength_ = 0;
};

// Добавить дорожку в сессию. undo — вынуть её. Используется, в частности,
// для открытия аудиофайла: теперь это правка в истории, а не замена сессии.
class AddTrackCommand final : public Command {
public:
    AddTrackCommand(std::shared_ptr<Track> track, int insertIndex = -1) noexcept
        : track_(std::move(track)), insertIndex_(insertIndex) {}

    void redo(Session& s) override;
    void undo(Session& s) override;

    bool coalesceWith(const Command&) override { return false; }
    const char* name() const noexcept override { return "Добавить дорожку"; }
    std::size_t byteSize() const noexcept override;

    Track* track() const noexcept { return track_.get(); }

private:
    std::shared_ptr<Track> track_;
    int  insertIndex_ = -1;   // -1 = в конец
    int  actualIndex_ = -1;   // заполняется redo
};

} // namespace daw::model
