#include "Commands.h"
#include <cassert>

namespace daw::model {

// MoveClipCommand
MoveClipCommand::MoveClipCommand(int trackIndex, int clipIndex,
                                 std::int64_t newPos) noexcept
    : trackIndex_(trackIndex), clipIndex_(clipIndex), newPos_(newPos) {}

void MoveClipCommand::redo(Session& s) {
    auto* track = s.track(trackIndex_);
    if (!track) return;
    auto& clip = track->clips()[clipIndex_];
    oldPos_ = clip->timelinePosition(); // store original position
    clip->setTimelinePosition(newPos_);
}

void MoveClipCommand::undo(Session& s) {
    auto* track = s.track(trackIndex_);
    if (!track) return;
    auto& clip = track->clips()[clipIndex_];
    clip->setTimelinePosition(oldPos_);
}

bool MoveClipCommand::coalesceWith(const Command& next) {
    if (const MoveClipCommand* nxt = dynamic_cast<const MoveClipCommand*>(&next)) {
        return trackIndex_ == nxt->trackIndex_
            && clipIndex_ == nxt->clipIndex_
            && newPos_ == nxt->newPos_;
    }
    return false;
}

const char* MoveClipCommand::name() const noexcept {
    return "Переместить клип";
}

std::size_t MoveClipCommand::byteSize() const noexcept {
    return sizeof(*this);
}

// AddTrackCommand
AddTrackCommand::AddTrackCommand(std::shared_ptr<Track> track, int insertIndex) noexcept
    : track_(std::move(track)), insertIndex_(insertIndex) {}

void AddTrackCommand::redo(Session& s) {
    if (insertIndex_ == -1) {
        // Append at end
        s.addTrack(track_);
        actualIndex_ = static_cast<int>(s.trackCount()) - 1;
    } else {
        // Insert at specified index
        s.insertTrack(insertIndex_, track_);
        actualIndex_ = insertIndex_;
    }
}

void AddTrackCommand::undo(Session& s) {
    if (actualIndex_ >= 0 && actualIndex_ < static_cast<int>(s.trackCount())) {
        s.removeTrack(actualIndex_);
    }
}

const char* AddTrackCommand::name() const noexcept {
    return "Добавить дорожку";
}

std::size_t AddTrackCommand::byteSize() const noexcept {
    return sizeof(*this);
}

// SplitClipCommand
SplitClipCommand::SplitClipCommand(int trackIndex, int clipIndex,
                                 std::int64_t splitAtAbsolute) noexcept
    : trackIndex_(trackIndex), clipIndex_(clipIndex), splitAtAbsolute_(splitAtAbsolute) {}

void SplitClipCommand::redo(Session& s) {
    auto* track = s.track(trackIndex_);
    if (!track) return;
    auto& clip = track->clips()[clipIndex_];
    // Validate split position
    std::int64_t relativePos = splitAtAbsolute_ - clip->timelinePosition();
    if (relativePos < 0 || relativePos > clip->length()) {
        // Invalid split position; do nothing
        return;
    }
    // Store original data
    originalLength_ = clip->length();
    originalOffset_ = clip->offset();
    firstLength_ = relativePos;

    // Trim the original clip
    clip->setLength(firstLength_);

    // Create second clip
    std::int64_t newOffset = originalOffset_ + relativePos;
    std::shared_ptr<Clip> secondClip = std::make_shared<Clip>(
        clip->sharedSource(),
        newOffset,
        originalLength_ - relativePos,
        splitAtAbsolute_
    );

    // Insert the new clip after the original
    track->insertClip(clipIndex_ + 1, secondClip);
    secondClipIndex_ = clipIndex_ + 1; // index of the newly inserted clip
}

void SplitClipCommand::undo(Session& s) {
    auto* track = s.track(trackIndex_);
    if (!track) return;
    // Remove the second clip
    if (secondClipIndex_ >= 0 && secondClipIndex_ < static_cast<int>(track->clips().size())) {
        track->removeClip(secondClipIndex_);
    }
    // Restore original clip
    auto& clip = track->clips()[clipIndex_];
    clip->setOffset(originalOffset_);
    clip->setLength(originalLength_);
}

bool SplitClipCommand::coalesceWith(const Command&) override {
    return false;
}

const char* SplitClipCommand::name() const noexcept {
    return "Разрезать клип";
}

std::size_t SplitClipCommand::byteSize() const noexcept {
    return sizeof(*this);
}

} // namespace daw::model