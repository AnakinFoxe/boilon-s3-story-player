#include "media/player_controller.h"

#include <utility>

namespace media {

PlayerController::PlayerController(MediaCatalog catalog)
    : catalog_(std::move(catalog)), modes_(catalog_.Collections().size(), PlayMode::kOrder) {}

const Collection* PlayerController::CurrentCollection() const {
    if (!collection_index_) {
        return nullptr;
    }
    return &catalog_.Collections()[*collection_index_];
}

std::optional<ResolvedTrack> PlayerController::CurrentTrack() const {
    const auto* collection = CurrentCollection();
    if (!collection) {
        return std::nullopt;
    }
    return ResolvedTrack{collection->id, collection->is_root, collection->directory_name,
                         collection->tracks[track_index_]};
}

std::pair<size_t, size_t> PlayerController::TrackNumber() const {
    const auto* collection = CurrentCollection();
    if (!collection) {
        return {0, 0};
    }
    return {track_index_ + 1, collection->tracks.size()};
}

void PlayerController::SetMode(size_t collection, PlayMode mode) {
    if (collection >= modes_.size() || modes_[collection] == mode) {
        return;
    }
    modes_[collection] = mode;
    if (mode == PlayMode::kShuffle && collection_index_ == collection) {
        Shuffle(false);
    }
}

PlayMode PlayerController::Mode(size_t collection) const {
    return collection < modes_.size() ? modes_[collection] : PlayMode::kOrder;
}

PlayMode PlayerController::Mode() const {
    return collection_index_ ? Mode(*collection_index_) : PlayMode::kOrder;
}

ControlResult PlayerController::SelectCollection(MediaId id) {
    const auto& collections = catalog_.Collections();
    for (size_t index = 0; index < collections.size(); ++index) {
        if (collections[index].id != id || collections[index].tracks.empty()) {
            continue;
        }
        const auto action = state_ == PlaybackState::kStopped ? PlaybackAction::kNone
                                                             : PlaybackAction::kStop;
        collection_index_ = index;
        track_index_ = 0;
        state_ = PlaybackState::kStopped;
        can_resume_ = false;
        from_start_ = false;
        if (Mode() == PlayMode::kShuffle) {
            Shuffle(false);
        }
        return {ControlStatus::kOk, action};
    }
    return {ControlStatus::kUnknownCollection, PlaybackAction::kNone};
}

ControlResult PlayerController::SelectTrack(size_t index) {
    const auto* collection = CurrentCollection();
    if (!collection) {
        return {ControlStatus::kNoSelection, PlaybackAction::kNone};
    }
    if (index >= collection->tracks.size()) {
        return {ControlStatus::kAtBoundary, PlaybackAction::kNone};
    }
    const auto result = Go(index, false);
    if (Mode() == PlayMode::kShuffle) {
        Shuffle(false);
    }
    return result;
}

ControlResult PlayerController::Play() {
    if (!CurrentCollection()) {
        return {ControlStatus::kNoSelection, PlaybackAction::kNone};
    }
    if (state_ == PlaybackState::kPlaying) {
        return {ControlStatus::kIgnored, PlaybackAction::kNone};
    }
    const auto action = can_resume_ ? PlaybackAction::kResume : PlaybackAction::kStart;
    state_ = PlaybackState::kPlaying;
    can_resume_ = true;
    return {ControlStatus::kOk, action,
            action == PlaybackAction::kStart && std::exchange(from_start_, false)};
}

ControlResult PlayerController::Pause() {
    if (!CurrentCollection()) {
        return {ControlStatus::kNoSelection, PlaybackAction::kNone};
    }
    if (state_ != PlaybackState::kPlaying) {
        return {ControlStatus::kIgnored, PlaybackAction::kNone};
    }
    state_ = PlaybackState::kPaused;
    return {ControlStatus::kOk, PlaybackAction::kPause};
}

ControlResult PlayerController::Go(size_t index, bool from_start) {
    track_index_ = index;
    can_resume_ = state_ == PlaybackState::kPlaying;
    // Not playing: the next Play starts this track and honors from_start then.
    from_start_ = from_start && !can_resume_;
    const auto action = can_resume_ ? PlaybackAction::kStart
                                    : (state_ == PlaybackState::kPaused ? PlaybackAction::kStop
                                                                        : PlaybackAction::kNone);
    return {ControlStatus::kOk, action, from_start && can_resume_};
}

ControlResult PlayerController::Move(bool forward) {
    const auto* collection = CurrentCollection();
    if (!collection) {
        return {ControlStatus::kNoSelection, PlaybackAction::kNone};
    }
    const size_t count = collection->tracks.size();
    const auto mode = Mode();
    if (mode == PlayMode::kShuffle) {
        if (count > 1 && forward) {
            return Go(NextShuffled(), true);
        }
        Reorder();
        if (count > 1 && position_ > 0) {
            return Go(order_[--position_], true);
        }
        return {ControlStatus::kAtBoundary, PlaybackAction::kNone};
    }
    if (forward ? track_index_ + 1 < count : track_index_ > 0) {
        return Go(forward ? track_index_ + 1 : track_index_ - 1, false);
    }
    if (mode != PlayMode::kOrder && count > 1) {
        return Go(forward ? 0 : count - 1, false);
    }
    return {ControlStatus::kAtBoundary, PlaybackAction::kNone};
}

ControlResult PlayerController::Previous() { return Move(false); }

ControlResult PlayerController::Next() { return Move(true); }

ControlResult PlayerController::TrackFinished() { return Advance(false); }

ControlResult PlayerController::TrackFailed() { return Advance(true); }

ControlResult PlayerController::Advance(bool failed) {
    const auto* collection = CurrentCollection();
    if (!collection) {
        return {ControlStatus::kNoSelection, PlaybackAction::kNone};
    }
    if (state_ != PlaybackState::kPlaying) {
        return {ControlStatus::kIgnored, PlaybackAction::kNone};
    }
    const auto mode = Mode();
    if (mode == PlayMode::kSingle && !failed) {
        return {ControlStatus::kOk, PlaybackAction::kStart, true};
    }
    if (mode == PlayMode::kShuffle) {
        track_index_ = NextShuffled();
        return {ControlStatus::kOk, PlaybackAction::kStart, true};
    }
    if (track_index_ + 1 < collection->tracks.size()) {
        ++track_index_;
        return {ControlStatus::kOk, PlaybackAction::kStart};
    }
    if (mode != PlayMode::kOrder) {
        track_index_ = 0;
        return {ControlStatus::kOk, PlaybackAction::kStart};
    }
    state_ = PlaybackState::kStopped;
    can_resume_ = false;
    return {ControlStatus::kOk, PlaybackAction::kStop};
}

// Every way into shuffling starts a round; this only guards the order's index.
void PlayerController::Reorder() {
    if (position_ >= order_.size() || order_[position_] != track_index_) {
        Shuffle(false);
    }
}

size_t PlayerController::NextShuffled() {
    Reorder();
    if (position_ + 1 == order_.size()) {
        Shuffle(true);
    }
    return order_[++position_];
}

void PlayerController::Shuffle(bool repeat) {
    const size_t count = CurrentCollection()->tracks.size();
    order_.assign(1, static_cast<uint16_t>(track_index_));
    for (size_t index = 0; index < count; ++index) {
        if (repeat || index != track_index_) {
            order_.push_back(static_cast<uint16_t>(index));
        }
    }
    // Fisher-Yates over the round, which starts at order_[1].
    for (size_t index = order_.size() - 1; index > 1; --index) {
        std::swap(order_[index], order_[1 + Random(static_cast<uint32_t>(index))]);
    }
    if (repeat && order_.size() > 2 && order_[1] == track_index_) {
        std::swap(order_[1], order_[2 + Random(static_cast<uint32_t>(order_.size() - 2))]);
    }
    position_ = 0;
}

uint32_t PlayerController::Random(uint32_t bound) {
    random_ ^= random_ << 13;  // xorshift32
    random_ ^= random_ >> 17;
    random_ ^= random_ << 5;
    return random_ % bound;
}

}  // namespace media
