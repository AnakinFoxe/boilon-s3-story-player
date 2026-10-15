#ifndef MEDIA_PLAYER_CONTROLLER_H_
#define MEDIA_PLAYER_CONTROLLER_H_

#include "media/media_catalog.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace media {

enum class PlaybackState { kStopped, kPlaying, kPaused };
// How a collection goes on after a track: in order and stop after the last
// (the default), loop the collection, repeat the track, or shuffle without end.
enum class PlayMode : uint8_t { kOrder, kLoop, kSingle, kShuffle };
enum class PlaybackAction { kNone, kStart, kPause, kResume, kStop };
enum class ControlStatus { kOk, kNoSelection, kUnknownCollection, kAtBoundary, kIgnored };

struct ControlResult {
    ControlStatus status = ControlStatus::kOk;
    PlaybackAction action = PlaybackAction::kNone;
    // A Start that begins the track at zero, not where it was last heard: a
    // shuffled or repeated track.
    bool from_start = false;
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
    // Select a track of the selected collection by its place, like Next or
    // Previous would; kAtBoundary past its end.
    ControlResult SelectTrack(size_t index);
    ControlResult Play();
    ControlResult Pause();
    // Stay within the collection, preserving playing/paused/stopped intent.
    // Loop and single wrap around; shuffle moves through its random order.
    ControlResult Previous();
    ControlResult Next();
    // Caller supplies only the current playing track's synchronous end event.
    // Paused/stopped notifications are ignored; this is not an async callback.
    ControlResult TrackFinished();
    // Like TrackFinished for a track that could not play: single repeat moves
    // on instead of retrying it.
    ControlResult TrackFailed();

    // Each collection keeps its own mode, kOrder until set; a change applies
    // from the next move. Shuffling starts a new round at the current track.
    void SetMode(size_t collection, PlayMode mode);
    PlayMode Mode(size_t collection) const;
    // The selected collection's mode; kOrder without a selection.
    PlayMode Mode() const;
    // The shuffle's random numbers; the board seeds it from hardware.
    void Seed(uint32_t seed) { random_ = seed ? seed : kDefaultSeed; }
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
    static constexpr uint32_t kDefaultSeed = 0x9e3779b9u;
    const Collection* CurrentCollection() const;
    ControlResult Move(bool forward);
    ControlResult Advance(bool failed);
    ControlResult Go(size_t index, bool from_start);
    // A new shuffle round after the current track; with repeat the round holds
    // every track (the current one never first), otherwise the others.
    void Shuffle(bool repeat);
    // Starts a round at the current track unless the order already holds it.
    void Reorder();
    // The track after the current one, with a new round after the last.
    size_t NextShuffled();
    uint32_t Random(uint32_t bound);

    MediaCatalog catalog_;
    std::optional<size_t> collection_index_;
    size_t track_index_ = 0;
    PlaybackState state_ = PlaybackState::kStopped;
    bool can_resume_ = false;
    // The next Start begins at zero (a shuffle move made while not playing).
    bool from_start_ = false;
    std::vector<PlayMode> modes_;
    // Shuffle: the track before this round, then the round; order_[position_]
    // is the current track.
    std::vector<uint16_t> order_;
    size_t position_ = 0;
    uint32_t random_ = kDefaultSeed;
};

}  // namespace media

#endif  // MEDIA_PLAYER_CONTROLLER_H_
