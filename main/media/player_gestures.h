#ifndef MEDIA_PLAYER_GESTURES_H_
#define MEDIA_PLAYER_GESTURES_H_

#include <cstdint>

namespace media {

enum class GestureMode { kPlayer, kDirectory, kAi };
struct GestureContext {
    GestureMode mode = GestureMode::kPlayer;
    bool ai_ready = false;
};

enum class GestureAction {
    kNone,
    kTogglePlayback,
    kOpenAi,
    kOfflineHint,
    kShutdownRequest,
    kVolumeUp,
    kVolumeDown,
    kNextTrack,
    kPreviousTrack,
    kOpenDirectory,
    kParentPageRequest,
    kBrowseNext,
    kBrowsePrevious,
    kConfirmCollection,
    kCancelAi,
};

// Raw pressed masks, not GPIO numbers. Synchronous sampling, normally <=10ms.
// Outputs requests only: never connect ShutdownRequest directly to hardware.
// Start/mode changes/invalid input drain held keys before accepting gestures.
class PlayerGestures {
public:
    static constexpr uint8_t kPower = 1;
    static constexpr uint8_t kUp = 2;
    static constexpr uint8_t kDown = 4;
    static constexpr uint64_t kDebounceMs = 30;
    static constexpr uint64_t kDoubleMs = 400;
    static constexpr uint64_t kChordGapMs = 200;
    static constexpr uint64_t kTrackHoldMs = 800;
    static constexpr uint64_t kPowerHoldMs = 2000;
    static constexpr uint64_t kParentHoldMs = 5000;

    GestureAction Update(uint8_t pressed, uint64_t now_ms, GestureContext context = {});

private:
    enum class Phase { kIdle, kPower, kUp, kDown, kChord, kConsumed };
    void Cancel(uint8_t pressed, uint64_t now_ms, GestureContext context);
    GestureAction Edge(uint8_t pressed, uint64_t now_ms);
    GestureAction Finish(uint64_t now_ms);
    GestureAction LongAction() const;
    uint64_t HoldTime() const;

    GestureContext context_;
    Phase phase_ = Phase::kConsumed;
    uint8_t raw_ = 0;
    uint8_t stable_ = 0;
    uint64_t last_ms_ = 0;
    uint64_t raw_since_ = 0;
    uint64_t started_ = 0;
    uint64_t released_ = 0;
    bool initialized_ = false;
    bool pending_click_ = false;
    bool second_click_ = false;
};

}  // namespace media

#endif  // MEDIA_PLAYER_GESTURES_H_
