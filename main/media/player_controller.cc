#include "media/player_controller.h"

#include <utility>

namespace media {

PlayerController::PlayerController(MediaCatalog catalog) : catalog_(std::move(catalog)) {}

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
        return {ControlStatus::kOk, action};
    }
    return {ControlStatus::kUnknownCollection, PlaybackAction::kNone};
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
    return {ControlStatus::kOk, action};
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

ControlResult PlayerController::Move(bool forward) {
    const auto* collection = CurrentCollection();
    if (!collection) {
        return {ControlStatus::kNoSelection, PlaybackAction::kNone};
    }
    if ((forward && track_index_ + 1 == collection->tracks.size()) ||
        (!forward && track_index_ == 0)) {
        return {ControlStatus::kAtBoundary, PlaybackAction::kNone};
    }
    track_index_ = forward ? track_index_ + 1 : track_index_ - 1;
    can_resume_ = state_ == PlaybackState::kPlaying;
    const auto action = can_resume_ ? PlaybackAction::kStart
                                   : (state_ == PlaybackState::kPaused ? PlaybackAction::kStop
                                                                      : PlaybackAction::kNone);
    return {ControlStatus::kOk, action};
}

ControlResult PlayerController::Previous() { return Move(false); }

ControlResult PlayerController::Next() { return Move(true); }

ControlResult PlayerController::TrackFinished() {
    const auto* collection = CurrentCollection();
    if (!collection) {
        return {ControlStatus::kNoSelection, PlaybackAction::kNone};
    }
    if (state_ != PlaybackState::kPlaying) {
        return {ControlStatus::kIgnored, PlaybackAction::kNone};
    }
    if (track_index_ + 1 < collection->tracks.size()) {
        ++track_index_;
        return {ControlStatus::kOk, PlaybackAction::kStart};
    }
    if (end_policy_ == EndPolicy::kRepeatCollection) {
        track_index_ = 0;
        return {ControlStatus::kOk, PlaybackAction::kStart};
    }
    state_ = PlaybackState::kStopped;
    can_resume_ = false;
    return {ControlStatus::kOk, PlaybackAction::kStop};
}

}  // namespace media
