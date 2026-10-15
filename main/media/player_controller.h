#ifndef MEDIA_PLAYER_CONTROLLER_H_
#define MEDIA_PLAYER_CONTROLLER_H_

#include "media/media_catalog.h"

#include <cstddef>
#include <optional>
#include <utility>

namespace media {

enum class PlaybackState { kStopped, kPlaying, kPaused };
enum class EndPolicy { kStop, kRepeatCollection };
enum class PlaybackAction { kNone, kStart, kPause, kResume, kStop };
enum class ControlStatus { kOk, kNoSelection, kUnknownCollection, kAtBoundary, kIgnored };

struct ControlResult {
    ControlStatus status = ControlStatus::kOk;
    PlaybackAction action = PlaybackAction::kNone;
};

// Synchronous, single-owner decision core; no audio, storage or device effects.
// State describes requested playback, not successful decoder/codec execution.
// The caller handles each returned action immediately, reading CurrentTrack()
// for Start/Resume. Start replaces any old stream; Stop releases even a paused
// stream. Real async audio integration must separately bind active sessions.
class PlayerController {
public:
    explicit PlayerController(MediaCatalog catalog);

    // Select the first track, stopped. Even reselecting resets the selection.
    ControlResult SelectCollection(MediaId id);
    ControlResult Play();
    ControlResult Pause();
    // Stay within the collection, preserving playing/paused/stopped intent.
    ControlResult Previous();
    ControlResult Next();
    // Caller supplies only the current playing track's synchronous end event.
    // Paused/stopped notifications are ignored; this is not an async callback.
    ControlResult TrackFinished();

    void SetEndPolicy(EndPolicy policy) { end_policy_ = policy; }
    EndPolicy GetEndPolicy() const { return end_policy_; }
    PlaybackState State() const { return state_; }
    std::optional<ResolvedTrack> CurrentTrack() const;
    // The current track's 1-based place in its collection and the collection's
    // size; {0, 0} without a selection.
    std::pair<size_t, size_t> TrackNumber() const;
    // The selected collection's place in Catalog().Collections(), if any.
    std::optional<size_t> CollectionIndex() const { return collection_index_; }
    // Never changes after construction, so another task may read it.
    const MediaCatalog& Catalog() const { return catalog_; }

private:
    const Collection* CurrentCollection() const;
    ControlResult Move(bool forward);

    MediaCatalog catalog_;
    std::optional<size_t> collection_index_;
    size_t track_index_ = 0;
    PlaybackState state_ = PlaybackState::kStopped;
    EndPolicy end_policy_ = EndPolicy::kStop;
    bool can_resume_ = false;
};

}  // namespace media

#endif  // MEDIA_PLAYER_CONTROLLER_H_
