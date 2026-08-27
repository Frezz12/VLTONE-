#pragma once

#include <cstdint>
#include <memory>

#include "daw/model/Source.h"

namespace daw::model {

class Clip {
public:
    // offset            — начало окна внутри иcходника (в cэмплах);
    // length            — длина окна (cколько cэмплов иcходника играем);
    // timelinePosition  — где клип cтоит на таймлайне аранжировки (в cэмплах).
    Clip(std::shared_ptr<Source> source, std::int64_t offset, std::int64_t length,
         std::int64_t timelinePosition = 0)
        : source_(std::move(source)),
          offset_(offset),
          length_(length),
          timelinePosition_(timelinePosition) {}

    const Source* source() const noexcept { return source_.get(); }
    std::shared_ptr<Source> sharedSource() const noexcept { return source_; }

    std::int64_t offset() const noexcept { return offset_; }
    std::int64_t length() const noexcept { return length_; }

    std::int64_t sourceEnd() const noexcept
    {
        return offset_ + length_;
    }

    // Позиция на таймлайне: начало и конец клипа в абcолютных cэмплах проекта.
    std::int64_t timelinePosition() const noexcept { return timelinePosition_; }
    void setTimelinePosition(std::int64_t pos) noexcept { timelinePosition_ = pos; }

    std::int64_t timelineEnd() const noexcept
    {
        return timelinePosition_ + length_;
    }

    // Окно внутри Source: начало и длина проигрываемого фрагмента. Меняется
    // при split/trim — сам аудиоматериал не трогается, клип только ссылается
    // на Source (§11), поэтому правка мгновенна и бесплатна по памяти.
    void setOffset(std::int64_t offset) noexcept { offset_ = offset; }
    void setLength(std::int64_t length) noexcept { length_ = length; }

    bool isValid() const noexcept
    {
        return source_ && source_->isValid();
    }

private:
    std::shared_ptr<Source> source_;
    std::int64_t offset_ = 0;
    std::int64_t length_ = 0;
    std::int64_t timelinePosition_ = 0;
};

} // namespace daw::model
