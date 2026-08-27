#pragma once

#include <memory>
#include <string>
#include <vector>

#include "daw/model/Track.h"
#include "daw/time/TempoMap.h"

namespace daw::model {

class Session {
public:
    Session() = default;

    explicit Session(double sampleRate)
        : tempoMap_(std::make_shared<daw::time::TempoMap>(sampleRate)) {}

    int addTrack(std::shared_ptr<Track> track)
    {
        int idx = static_cast<int>(tracks_.size());
        tracks_.push_back(std::move(track));
        return idx;
    }

    // Вставка/удаление дорожки по индексу — для команд AddTrack/RemoveTrack
    // (undo/redo). Индекс вне диапазона — no-op / nullptr.
    bool insertTrack(int index, std::shared_ptr<Track> track)
    {
        if (index < 0 || index > static_cast<int>(tracks_.size()))
            return false;
        tracks_.insert(tracks_.begin() + index, std::move(track));
        return true;
    }

    std::shared_ptr<Track> removeTrack(int index)
    {
        if (index < 0 || index >= static_cast<int>(tracks_.size()))
            return nullptr;
        auto track = std::move(tracks_[index]);
        tracks_.erase(tracks_.begin() + index);
        return track;
    }

    Track* track(int index) noexcept
    {
        return (index >= 0 && index < static_cast<int>(tracks_.size()))
             ? tracks_[index].get() : nullptr;
    }

    const Track* track(int index) const noexcept
    {
        return (index >= 0 && index < static_cast<int>(tracks_.size()))
             ? tracks_[index].get() : nullptr;
    }

    int trackCount() const noexcept { return static_cast<int>(tracks_.size()); }

    const std::vector<std::shared_ptr<Track>>& tracks() const noexcept
    {
        return tracks_;
    }

    void setTempoMap(std::shared_ptr<daw::time::TempoMap> map)
    {
        tempoMap_ = std::move(map);
    }

    std::shared_ptr<daw::time::TempoMap> tempoMap() const noexcept
    {
        return tempoMap_;
    }

private:
    std::vector<std::shared_ptr<Track>> tracks_;
    std::shared_ptr<daw::time::TempoMap> tempoMap_;
};

} // namespace daw::model
