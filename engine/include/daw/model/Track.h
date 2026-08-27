#pragma once

#include <memory>
#include <string>
#include <vector>

#include "daw/model/Clip.h"

namespace daw::model {

class Track {
public:
    enum class Type { Audio, Master };

    explicit Track(std::string name, Type type = Type::Audio)
        : name_(std::move(name)), type_(type) {}

    const std::string& name() const noexcept { return name_; }
    Type type() const noexcept { return type_; }

    void addClip(std::shared_ptr<Clip> clip)
    {
        clips_.push_back(std::move(clip));
    }

    // Вставка/удаление по индексу — для команд split и remove (undo/redo).
    // Индекс вне диапазона — no-op. Возвращает false, если операция не
    // выполнена (индекс невалиден), true — если применена.
    bool insertClip(int index, std::shared_ptr<Clip> clip)
    {
        if (index < 0 || index > static_cast<int>(clips_.size()))
            return false;
        clips_.insert(clips_.begin() + index, std::move(clip));
        return true;
    }

    std::shared_ptr<Clip> removeClip(int index)
    {
        if (index < 0 || index >= static_cast<int>(clips_.size()))
            return nullptr;
        auto clip = std::move(clips_[index]);
        clips_.erase(clips_.begin() + index);
        return clip;
    }

    int clipCount() const noexcept
    {
        return static_cast<int>(clips_.size());
    }

    const std::shared_ptr<Clip>& clip(int index) const
    {
        return clips_[index];
    }

    const std::vector<std::shared_ptr<Clip>>& clips() const noexcept
    {
        return clips_;
    }

    void setGain(float gain) noexcept { gain_ = gain; }
    float gain() const noexcept { return gain_; }

    void setMuted(bool muted) noexcept { muted_ = muted; }
    bool isMuted() const noexcept { return muted_; }

    void setSoloed(bool soloed) noexcept { soloed_ = soloed; }
    bool isSoloed() const noexcept { return soloed_; }

private:
    std::string name_;
    Type type_ = Type::Audio;
    std::vector<std::shared_ptr<Clip>> clips_;
    float gain_ = 1.0f;
    bool muted_ = false;
    bool soloed_ = false;
};

} // namespace daw::model
