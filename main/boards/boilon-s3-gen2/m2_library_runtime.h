#pragma once

#include <algorithm>
#include <tuple>
#include "m2_file_bookmarks.h"
#include "m2_library_gestures.h"
#include "m2_library_policy.h"
#include "m2_local_policy.h"
#include "m2_resume_runtime.h"
#include "media/player_gestures.h"

namespace boilon::local_library {
// P5-5: files in a row that may fail without sound before the player stops.
// When the card could not open or read one of them (for example it was pulled)
// the player stops with an error; bad files alone leave it paused on 「播放错误」
// with the directory list, the AI's picks and the keys still working.
constexpr int kMaxFailedFiles = 3;
enum class PlayerState { kPlaying, kPaused, kLocating, kSwitching, kFinished, kFailed, kEmpty };
struct PlayerView {
    std::string title, collection;
    std::vector<std::string> directories;
    std::vector<size_t> directory_files;  // files in each directory
    size_t highlighted = 0;
    int volume = 10;
    uint64_t duration_ms = 0;
    bool directory_open = false, volume_visible = false, failed = false;
    // P5-1: the player's state, the track's place in its directory (1-based,
    // 0 without a track), the ±30 s total while it shows, the time left before
    // the directory list closes itself, and the AI-offline hint.
    PlayerState state = PlayerState::kEmpty;
    size_t track_number = 0, track_count = 0;
    int seek_seconds = 0;
    uint32_t directory_left_ms = 0;
    bool offline_hint = false;
    media::PlayMode mode = media::PlayMode::kOrder;  // The selected directory's.
};
// Board-local polling adapter; storage effects belong to the optional ResumePort.
template <typename Player>
class Runtime {
public:
    using Observer = std::function<void(const char*, const media::PlayerController&)>;
    Runtime(media::MediaCatalog catalog, Player& player, std::function<bool()> output_idle,
            Observer observer = {}, ResumePort* resume = nullptr,
            std::function<void(int)> restore_volume = {})
        : directories_(Directories(catalog)),
          directory_files_(Files(catalog)),
          controller_(SelectFirst(std::move(catalog))),
          player_(player),
          resume_(resume),
          resume_player_(player),
          restore_volume_(std::move(restore_volume)),
          observer_(std::move(observer)),
          bridge_(
              controller_, resume_player_, "/sdcard",
              [this, idle = std::move(output_idle)] {
                  // Poll invokes this only after Running is false: the terminal
                  // result is now stable. Do not overwrite a decoder error with
                  // Start before the next Tick has skipped the failed file.
                  return (player_.LastResult() != Player::Result::kError || error_handled_) &&
                         idle && idle();
              },
              [this] {
                  error_handled_ = false;
                  active_ = controller_.CurrentTrack();
                  Notify("START");
                  Checkpoint(media::CheckpointReason::kTrackChange, now_);
              }) {}

    bool Start(uint64_t now_ms) {
        if (started_)
            return Fail();
        started_ = true;
        now_ = periodic_ = now_ms;
        if (resume_) {
            if constexpr (!HasResume<Player>::value)
                return Fail();
            restoring_ = resume_->Restore({}, media::ResumeMode::kBoot);
            return restoring_ || Fail();
        }
        return Submit(controller_.Play(), now_ms);
    }
    bool Tick(uint8_t pressed_mask, uint64_t now_ms) {
        if (!started_ || failed_)
            return Fail();
        now_ = now_ms;
        last_gesture_ = media::GestureAction::kNone;
        if (restoring_) {
            if (!bridge_.Poll(now_ms))
                return Fail();
            media::ResumePlan plan;
            if (!resume_->TakeRestore(plan))
                return true;
            restoring_ = false;
            if (!ApplyResume(plan) || !Submit(controller_.Play(), now_ms))
                return Fail();
        }
        if (waiting_bookmark_) {
            const auto track = controller_.CurrentTrack();
            const auto offset = track && resume_ ? resume_->KnownPosition(track->track.id)
                                                 : std::optional<uint64_t>{0};
            if (!offset)
                return true;
            waiting_bookmark_ = false;
            starting_offset_ = offset;
            if (!Submit({media::ControlStatus::kOk, media::PlaybackAction::kStart}, now_ms))
                return false;
        }
        // Another owner ended our output session (a conversation or a server
        // push): keep what the child heard and wait for the speaker to come back.
        if (!parked_ && active_ && !bridge_.Pending() && !player_.Running() &&
            player_.LastResult() == Player::Result::kStopped &&
            controller_.State() != media::PlaybackState::kStopped)
            Release(PositionMs());
        // Check before Poll can replace the old decoder's terminal result.
        if (DecoderFailed())
            return SkipFailed(now_ms);
        if constexpr (HasResume<Player>::value) {
            // Only a running decoder's output replaces the target; a stopped one
            // still reports the previous track's last position.
            if (seek_target_ && !bridge_.Pending() && player_.Running() && player_.PositionMs() > 0)
                seek_target_.reset();
            if (awaiting_output_ && player_.PositionMs() > 0) {
                awaiting_output_ = false;
                Checkpoint(media::CheckpointReason::kTrackChange, now_ms);
            }
            if (player_.Running() && player_.PositionMs() > 0) {
                failed_files_ = 0;  // This file makes sound.
                card_failed_ = false;
            }
        }
        if (!bridge_.Poll(now_ms))
            return Fail();
        if (DecoderFailed())
            return SkipFailed(now_ms);
        if (Ended() && !eof_consumed_ && controller_.State() == media::PlaybackState::kPlaying) {
            failed_files_ = 0;
            card_failed_ = false;
            Checkpoint(media::CheckpointReason::kTrackChange, now_ms);
            Notify("EOF");  // Preserve the old track and final output position before advancing.
            eof_consumed_ = true;
            if (!Submit(controller_.TrackFinished(), now_ms))
                return false;
        }
        if (now_ms >= periodic_ && now_ms - periodic_ >= media::PlaybackResume::kCheckpointMs) {
            periodic_ = now_ms;
            Checkpoint(media::CheckpointReason::kPeriodic, now_ms);
        }
        if (directory_open_) {
            if (pressed_mask)
                directory_activity_ = now_ms;
            else if (now_ms - directory_activity_ >= 5000)
                directory_open_ = false;
        }
        const auto mode = ai_active_        ? media::GestureMode::kAi
                          : directory_open_ ? media::GestureMode::kDirectory
                                            : media::GestureMode::kPlayer;
        const auto request = gestures_.Update(pressed_mask, now_ms, {mode, ai_ready_});
        last_gesture_ = request.action;
        if (request.seek_seconds)
            return Seek(request.seek_seconds, now_ms);
        switch (last_gesture_) {
            case media::GestureAction::kOfflineHint:
                offline_hint_ = now_ms;
                return true;  // Visual feedback only: never pause or restart the story.
            case media::GestureAction::kVolumeUp:
            case media::GestureAction::kVolumeDown:
                volume_ = std::clamp(
                    volume_ + (last_gesture_ == media::GestureAction::kVolumeUp ? 5 : -5), 0,
                    local_player::kMaxVolume);
                volume_changed_ = now_ms;
                return true;  // The board adapter applies the setting to its codec.
            case media::GestureAction::kOpenDirectory: {
                const auto current = controller_.CurrentTrack();
                for (size_t i = 0; current && i < directories_.size(); ++i)
                    if (directories_[i].first == current->collection_id)
                        highlighted_ = i;
                directory_open_ = !directories_.empty();
                directory_activity_ = now_ms;
                return true;
            }
            case media::GestureAction::kBrowseNext:
            case media::GestureAction::kBrowsePrevious:
                if (directory_open_ && !directories_.empty()) {
                    if (last_gesture_ == media::GestureAction::kBrowseNext)
                        highlighted_ = (highlighted_ + 1) % directories_.size();
                    else
                        highlighted_ =
                            (highlighted_ + directories_.size() - 1) % directories_.size();
                    directory_activity_ = now_ms;
                }
                return true;
            case media::GestureAction::kConfirmCollection:
                if (directory_open_ && highlighted_ < directories_.size()) {
                    directory_open_ = false;
                    Checkpoint(media::CheckpointReason::kCollectionChange, now_ms);
                    if (!Submit(controller_.SelectCollection(directories_[highlighted_].first),
                                now_ms))
                        return false;
                    if (resume_) {
                        restoring_ = resume_->Restore(directories_[highlighted_].first,
                                                      media::ResumeMode::kSavedExact);
                        return restoring_ || Fail();
                    }
                    return Submit(controller_.Play(), now_ms);
                }
                return true;
            case media::GestureAction::kTogglePlayback: {
                if (controller_.State() == media::PlaybackState::kPlaying)
                    Checkpoint(media::CheckpointReason::kPause, now_ms);
                auto result = controller_.State() == media::PlaybackState::kPlaying
                                  ? controller_.Pause()
                                  : controller_.Play();
                // A parked story has no decoder left, nor one that failed while
                // paused: start again where it stopped.
                if (result.action == media::PlaybackAction::kResume &&
                    (parked_ || (error_handled_ && !player_.Running() && !bridge_.Pending())))
                    result.action = media::PlaybackAction::kStart;
                // A stream that ended while paused cannot resume a dead decoder.
                if (result.action == media::PlaybackAction::kResume && Ended()) {
                    Notify("EOF");
                    eof_consumed_ = true;
                    result = controller_.TrackFinished();
                }
                return Submit(result, now_ms);
            }
            case media::GestureAction::kNextTrack:
                Checkpoint(media::CheckpointReason::kTrackChange, now_ms);
                return Submit(controller_.Next(), now_ms);
            case media::GestureAction::kPreviousTrack:  // P5-2: no "back to the start" first.
                Checkpoint(media::CheckpointReason::kTrackChange, now_ms);
                return Submit(controller_.Previous(), now_ms);
            default:
                return true;  // Other requests are diagnostic only in this variant.
        }
    }
    const media::PlayerController& Controller() const { return controller_; }
    int Volume() const { return volume_; }
    // A volume set outside the keys (XiaoZhi, AI variant): shown like a key
    // press, saved with the next checkpoint, and the keys continue from it.
    void AdoptVolume(int volume, uint64_t now_ms) {
        volume_ = std::clamp(volume, 0, local_player::kMaxVolume);
        volume_changed_ = now_ms;
    }
    // AI focus (AI variant). The board switches the buttons to AI gestures while
    // a conversation owns the speaker, and says whether a double-click may open one.
    void SetAi(bool active, bool ready) {
        ai_active_ = active;
        ai_ready_ = ready;
    }
    // Hands the speaker over: remembers what the child heard and whether the
    // story was playing, then stops the decoder. False while starting, switching,
    // locating or between tracks; true with nothing to do when no decoder holds
    // the speaker (stopped, or paused after the stream ended).
    bool Park(uint64_t now_ms) {
        if (!started_ || failed_ || restoring_ || waiting_bookmark_ || bridge_.Pending())
            return false;
        if (parked_ || !player_.Running())
            return controller_.State() != media::PlaybackState::kPlaying;
        if (Locating())
            return false;
        // The pause checkpoint saves and remembers what the child heard.
        const bool playing = controller_.State() == media::PlaybackState::kPlaying;
        if (playing) {
            Checkpoint(media::CheckpointReason::kPause, now_ms);
            controller_.Pause();
        }
        const uint64_t duration = DurationMs();
        if (!Submit({media::ControlStatus::kOk, media::PlaybackAction::kStop}, now_ms))
            return false;
        parked_ = true;
        park_playing_ = playing;
        if (duration)
            seek_duration_ = duration;
        return true;
    }
    bool Parked() const { return parked_; }
    bool ParkedWhilePlaying() const { return parked_ && park_playing_; }
    // The decoder has stopped and nothing is pending: the speaker is free.
    bool Released() const { return !player_.Running() && !bridge_.Pending(); }
    // Takes the speaker back. A story that was playing starts about five seconds
    // before what the child heard (at most to the start); a paused one stays paused.
    bool Unpark(uint64_t now_ms) {
        if (!parked_ || !park_playing_)
            return true;
        park_playing_ = false;
        if (!controller_.CurrentTrack())
            return true;
        const uint64_t heard = PositionMs();
        starting_offset_ = heard > 5000 ? heard - 5000 : 0;
        controller_.Play();
        return Submit({media::ControlStatus::kOk, media::PlaybackAction::kStart}, now_ms);
    }
    // P5-2: the child's pick through the AI replaces the parked story. Number 0
    // continues the directory from its bookmark, like the directory list; a
    // number starts that file from its beginning. False changes nothing.
    bool Choose(size_t directory, size_t number, uint64_t now_ms) {
        if (!started_ || failed_ || restoring_ || waiting_bookmark_ || bridge_.Pending() ||
            directory >= directories_.size() || number > directory_files_[directory])
            return false;
        directory_open_ = false;
        Checkpoint(media::CheckpointReason::kCollectionChange, now_ms);  // Keeps the old place.
        parked_ = park_playing_ = false;  // The pick wins: never resume the old story.
        const auto id = directories_[directory].first;
        if (!Submit(controller_.SelectCollection(id), now_ms))
            return false;
        if (!number && resume_) {
            restoring_ = resume_->Restore(id, media::ResumeMode::kSavedExact);
            return restoring_ || Fail();
        }
        if (number) {
            controller_.SelectTrack(number - 1);
            starting_offset_ = 0;
        }
        return Submit(controller_.Play(), now_ms);
    }
    // How each directory goes on after a file (AI variant); see PlayerController.
    void SetMode(size_t directory, media::PlayMode mode) { controller_.SetMode(directory, mode); }
    void Seed(uint32_t seed) { controller_.Seed(seed); }
    // The AI asked to pause outside a conversation: like the top key while playing,
    // and a story another owner holds stays paused. False while switching, which
    // changes nothing.
    bool Pause(uint64_t now_ms) {
        if (parked_) {
            StayPaused();
            return true;
        }
        if (!started_ || failed_ || controller_.State() != media::PlaybackState::kPlaying)
            return true;
        if (restoring_ || waiting_bookmark_ || bridge_.Pending())
            return false;
        Checkpoint(media::CheckpointReason::kPause, now_ms);
        return Submit(controller_.Pause(), now_ms);
    }
    // The AI asked the story to stay paused when the conversation ends: a story
    // parked while playing keeps its place, about five seconds back like Unpark.
    void StayPaused() {
        if (!parked_ || !park_playing_)
            return;
        park_playing_ = false;
        if (const auto current = controller_.CurrentTrack()) {
            const uint64_t heard = PositionMs();
            seek_target_ = heard > 5000 ? heard - 5000 : 0;
            Remember(current->track.id, *seek_target_);
        }
    }
    // The selected directory's place in the directory list and the file's
    // 1-based place in it; nothing without a selection.
    std::optional<std::pair<size_t, size_t>> Selected() const {
        const auto index = controller_.CollectionIndex();
        if (!index)
            return std::nullopt;
        return std::make_pair(*index, controller_.TrackNumber().first);
    }
    const media::MediaCatalog& Catalog() const { return controller_.Catalog(); }
    bool Shutdown(uint64_t now_ms) {
        return Checkpoint(media::CheckpointReason::kShutdown, now_ms);
    }
    bool Finished() const {
        return started_ && !failed_ && !restoring_ && !bridge_.Pending() && !player_.Running() &&
               controller_.State() == media::PlaybackState::kStopped;
    }
    media::GestureAction LastGesture() const { return last_gesture_; }
    uint64_t PositionMs() const {
        if (seek_target_)
            return *seek_target_;
        if constexpr (HasResume<Player>::value)
            return player_.PositionMs() ? player_.PositionMs() : resume_player_.StartedOffset();
        return 0;
    }
    uint64_t DurationMs() const {
        if (seek_duration_)
            return *seek_duration_;
        const auto current = controller_.CurrentTrack();
        if (!current)
            return 0;
        if constexpr (HasResume<Player>::value) {
            if (!bridge_.Pending() && active_ && active_->track.id == current->track.id &&
                player_.DurationMs())
                return player_.DurationMs();
        }
        return resume_ ? resume_->KnownDuration(current->track.id) : 0;
    }
    PlayerView View(uint64_t now_ms) const {
        PlayerView view;
        view.directory_open = directory_open_;
        view.highlighted = highlighted_;
        view.volume = volume_;
        view.duration_ms = DurationMs();
        view.volume_visible =
            volume_changed_ && now_ms >= *volume_changed_ && now_ms - *volume_changed_ < 2000;
        view.failed = failed_;
        for (const auto& directory : directories_)
            view.directories.push_back(directory.second);
        view.directory_files = directory_files_;
        if (directory_open_)
            view.directory_left_ms =
                static_cast<uint32_t>(5000 - std::min<uint64_t>(5000, now_ms - directory_activity_));
        std::tie(view.track_number, view.track_count) = controller_.TrackNumber();
        if (seek_shown_ && now_ms >= *seek_shown_ && now_ms - *seek_shown_ < 2000)
            view.seek_seconds = seek_total_;
        const auto current = controller_.CurrentTrack();
        view.title = current ? current->track.file_name : "没有可播放的故事";
        view.collection = current
                              ? (current->collection_is_root ? "未分类" : current->directory_name)
                              : "请检查 SD 卡内容";
        const bool locating = Locating();
        using S = PlayerState;
        view.state = failed_ || unplayable_            ? S::kFailed
                     : !current                        ? S::kEmpty
                     : restoring_ || bridge_.Pending() ? S::kSwitching
                     : controller_.State() == media::PlaybackState::kPlaying
                         ? (locating ? S::kLocating : S::kPlaying)
                     : controller_.State() == media::PlaybackState::kPaused ? S::kPaused
                                                                            : S::kFinished;
        view.offline_hint = !view.failed && !view.directory_open && offline_hint_ &&
                            now_ms >= *offline_hint_ && now_ms - *offline_hint_ < 2000;
        view.mode = controller_.Mode();
        return view;
    }

private:
    bool Locating() const {
        if constexpr (HasResume<Player>::value)
            return resume_player_.StartedOffset() && player_.Running() && !player_.PositionMs();
        return false;
    }
    // The output session ended without us: hold the position and intent.
    void Release(uint64_t position) {
        const uint64_t duration = DurationMs();
        Remember(active_->track.id, position);
        park_playing_ = controller_.State() == media::PlaybackState::kPlaying;
        if (park_playing_)
            controller_.Pause();
        parked_ = true;
        seek_target_ = position;
        if (duration)
            seek_duration_ = duration;
        active_.reset();
        awaiting_output_ = false;
        Notify("RELEASED");
    }
    bool Seek(int seconds, uint64_t now_ms) {
        const int step = seconds > 0 ? 30 : -30;  // Each chord press moves 30 s.
        seek_total_ = seek_shown_ && now_ms - *seek_shown_ < 2000 ? seek_total_ + step : step;
        seek_shown_ = now_ms;
        const uint64_t position = PositionMs();
        const uint64_t duration = DurationMs();
        const uint64_t limit = duration ? duration : 24ULL * 60 * 60 * 1000;
        const uint64_t target = seconds > 0 ? std::min(position + 30000, limit)
                                            : (position > 30000 ? position - 30000 : 0);
        return RestartAt(target, now_ms);
    }
    bool RestartAt(uint64_t target, uint64_t now_ms) {
        if constexpr (!HasResume<Player>::value)
            return true;
        if (!controller_.CurrentTrack())
            return true;
        const auto track = controller_.CurrentTrack();
        const auto duration = DurationMs();
        Remember(track->track.id, target);
        starting_offset_ = target;
        if (!Submit({media::ControlStatus::kOk, media::PlaybackAction::kStart}, now_ms))
            return false;
        seek_duration_ = duration ? std::optional<uint64_t>{duration} : std::nullopt;
        explicit_seek_ = true;
        if (resume_ && (!target || (duration && target <= duration)))
            resume_->Checkpoint(*track, target, duration, volume_, now_ms,
                                media::CheckpointReason::kTrackChange);
        Notify("SEEK");
        return true;
    }
    bool ApplyResume(const media::ResumePlan& plan) {
        if (plan.status != media::ResumeStatus::kReady || !plan.target) {
            Notify("RESTORE_UNAVAILABLE");
            return true;  // Play selected first track; never clean or overwrite bad storage here.
        }
        if (controller_.SelectCollection(plan.target->collection_id).status !=
            media::ControlStatus::kOk)
            return false;
        const auto& tracks =
            controller_.Catalog().Collections()[*controller_.CollectionIndex()].tracks;
        for (size_t i = 0; i < tracks.size(); ++i) {
            if (tracks[i].id != plan.target->track.id)
                continue;
            controller_.SelectTrack(i);
            starting_offset_ = plan.position_ms;
            Remember(tracks[i].id, plan.position_ms);
            if (plan.volume_percent) {
                // Records from before the 2026-10 rescale may hold up to 200.
                volume_ = std::clamp(*plan.volume_percent, 0, local_player::kMaxVolume);
                if (restore_volume_)
                    restore_volume_(volume_);
            }
            return true;
        }
        return false;
    }
    bool Checkpoint(media::CheckpointReason reason, uint64_t now_ms) {
        if constexpr (HasResume<Player>::value) {
            const auto track = active_ ? active_
                                       : ((explicit_seek_ || parked_) && seek_target_
                                              ? controller_.CurrentTrack()
                                              : std::nullopt);
            if (track) {
                const auto position = active_ ? player_.PositionMs() : PositionMs();
                // StartAt reports zero until actual completed output. Never
                // overwrite the retained point with that temporary zero.
                if (active_ && !position && resume_player_.StartedOffset()) {
                    if (reason == media::CheckpointReason::kTrackChange)
                        awaiting_output_ = true;
                    return false;
                }
                Remember(track->track.id, Ended() ? 0 : position);
                if (!resume_)
                    return false;
                const bool queued =
                    resume_->Checkpoint(*track, position, DurationMs(), volume_, now_ms, reason);
                if (!queued)
                    Notify("CHECKPOINT_NOT_QUEUED");
                return queued;
            }
        }
        return false;
    }
    void Remember(media::MediaId track, uint64_t position) {
        visited_.erase(std::remove_if(visited_.begin(), visited_.end(),
                                      [&](const auto& item) { return item.first == track; }),
                       visited_.end());
        visited_.emplace_back(track, position);
        if (visited_.size() > BOILON_S3_GEN2_BOOKMARK_CAPACITY)
            visited_.erase(visited_.begin());
    }
    std::optional<uint64_t> Remembered(media::MediaId track) const {
        for (const auto& item : visited_)
            if (item.first == track)
                return item.second;
        return {};
    }
    void Notify(const char* event) const {
        if (observer_)
            observer_(event, controller_);
    }
    static std::vector<std::pair<media::MediaId, std::string>> Directories(
        const media::MediaCatalog& catalog) {
        std::vector<std::pair<media::MediaId, std::string>> result;
        for (const auto& collection : catalog.Collections())
            result.emplace_back(collection.id,
                                collection.is_root ? "未分类" : collection.directory_name);
        return result;
    }
    static std::vector<size_t> Files(const media::MediaCatalog& catalog) {
        std::vector<size_t> result;
        for (const auto& collection : catalog.Collections())
            result.push_back(collection.tracks.size());
        return result;
    }
    static media::PlayerController SelectFirst(media::MediaCatalog catalog) {
        auto first = catalog.Collections().empty() ? std::optional<media::MediaId>{}
                                                   : catalog.Collections().front().id;
        media::PlayerController controller(std::move(catalog));
        if (first)
            controller.SelectCollection(*first);
        return controller;
    }
    bool Ended() const {
        return !bridge_.Pending() && !player_.Running() &&
               player_.LastResult() == Player::Result::kFinished;
    }
    // The decoder stopped on an error (open, read, decode) not yet handled.
    bool DecoderFailed() const {
        return !error_handled_ && !player_.Running() &&
               player_.LastResult() == Player::Result::kError;
    }
    // P5-5: skip the failed file like a finished one; at the directory's end
    // the player stops. kMaxFailedFiles in a row without sound stop it: with an
    // error if the card failed to open or read one of them, otherwise paused on
    // 「播放错误」 and still usable. A switch already under way replaces the decoder.
    bool SkipFailed(uint64_t now_ms) {
        error_handled_ = true;
        if (bridge_.Pending())
            return true;
        Checkpoint(media::CheckpointReason::kTrackChange, now_ms);
        Notify("SKIP");
        card_failed_ = card_failed_ || player_.ReadFailed();
        if (++failed_files_ < kMaxFailedFiles)
            return Submit(controller_.TrackFailed(), now_ms);
        if (card_failed_)
            return Fail();
        // Bad files only: Play tries again from this one; any other action
        // clears the 「播放错误」 mark.
        failed_files_ = 0;
        controller_.Pause();
        if (!Submit({media::ControlStatus::kOk, media::PlaybackAction::kStop}, now_ms))
            return false;
        unplayable_ = true;
        return true;
    }
    bool Submit(media::ControlResult result, uint64_t now_ms) {
        if (result.action != media::PlaybackAction::kNone)
            unplayable_ = false;
        // Reset at each requested Start, including a start queued behind drain.
        // The next very short track may finish before a tick observes Running.
        if (result.action == media::PlaybackAction::kStart) {
            parked_ = park_playing_ = false;
            explicit_seek_ = false;
            seek_duration_.reset();
            const auto current = controller_.CurrentTrack();
            if (result.from_start)  // A shuffled or repeated file: never its bookmark.
                starting_offset_ = 0;
            auto offset = std::exchange(starting_offset_, {});
            if (!offset && current)
                offset = Remembered(current->track.id);
            if (!offset && current && resume_)
                offset = resume_->KnownPosition(current->track.id);
            if (!offset && resume_) {
                waiting_bookmark_ = true;
                active_.reset();
                return bridge_.Submit({media::ControlStatus::kOk, media::PlaybackAction::kStop},
                                      now_ms);
            }
            seek_target_ = offset.value_or(0);
            resume_player_.Offset(*seek_target_);
            active_.reset();
            awaiting_output_ = false;
            eof_consumed_ = false;
            Notify("REQUEST");
        }
        if (!bridge_.Submit(result, now_ms))
            return Fail();
        using Action = media::PlaybackAction;
        if (result.action == Action::kPause)
            Notify("PAUSE");
        else if (result.action == Action::kResume)
            Notify("RESUME");
        else if (result.action == Action::kStop)
            Notify("STOP");
        if (result.action == Action::kStop) {
            seek_duration_.reset();
            active_.reset();
            awaiting_output_ = false;
            const auto current = controller_.CurrentTrack();
            if (controller_.State() == media::PlaybackState::kStopped)
                seek_target_.reset();
            else if (current) {
                auto offset = Remembered(current->track.id);
                if (!offset && resume_)
                    offset = resume_->KnownPosition(current->track.id);
                waiting_bookmark_ = !offset && resume_;
                seek_target_ = offset.value_or(0);
            }
        }
        return true;
    }
    bool Fail() {
        if (!failed_ && !bridge_.Failed())
            player_.Stop();
        failed_ = true;
        return false;
    }
    std::vector<std::pair<media::MediaId, std::string>> directories_;
    std::vector<size_t> directory_files_;
    media::PlayerController controller_;
    Player& player_;
    ResumePort* resume_;
    ResumePlayer<Player> resume_player_;
    std::function<void(int)> restore_volume_;
    Observer observer_;
    Switcher<ResumePlayer<Player>> bridge_;
    LibraryGestures gestures_;
    std::optional<uint64_t> seek_target_;
    std::optional<uint64_t> seek_duration_;
    std::optional<uint64_t> starting_offset_;
    std::vector<std::pair<media::MediaId, uint64_t>> visited_;
    bool waiting_bookmark_ = false, explicit_seek_ = false;
    media::GestureAction last_gesture_ = media::GestureAction::kNone;
    bool started_ = false, eof_consumed_ = false, failed_ = false, error_handled_ = false;
    int failed_files_ = 0;
    bool card_failed_ = false, unplayable_ = false;
    bool directory_open_ = false;
    size_t highlighted_ = 0;
    int volume_ = 10;
    uint64_t directory_activity_ = 0;
    std::optional<uint64_t> volume_changed_;
    std::optional<uint64_t> offline_hint_;
    std::optional<uint64_t> seek_shown_;
    int seek_total_ = 0;
    std::optional<media::ResolvedTrack> active_;
    bool restoring_ = false;
    bool awaiting_output_ = false;
    bool ai_active_ = false, ai_ready_ = false, parked_ = false, park_playing_ = false;
    uint64_t now_ = 0, periodic_ = 0;
};
}  // namespace boilon::local_library
