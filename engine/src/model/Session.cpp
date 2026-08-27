#include "Session.h"
#include <cassert>

namespace daw::model {

// Вставка/удаление дорожки по индексу
bool Session::insertTrack(int index, std::shared_ptr<Track> track) {
    if (index < 0 || index > static_cast<int>(tracks_.size())) return false;
    tracks_.insert(tracks_.begin() + index, std::move(track));
    return true;
}

std::shared_ptr<Track> Session::removeTrack(int index) {
    if (index < 0 || index >= static_cast<int>(tracks_.size())) return nullptr;
    auto track = std::move(tracks_[index]);
    tracks_.erase(tracks_.begin() + index);
    return track;
}

} // namespace daw::model