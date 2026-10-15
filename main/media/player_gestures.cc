#include "media/player_gestures.h"

namespace media {

void PlayerGestures::Cancel(uint8_t pressed, uint64_t now_ms, GestureContext context) {
    context_ = context;
    phase_ = Phase::kConsumed;
    raw_ = stable_ = pressed;
    last_ms_ = raw_since_ = now_ms;
    pending_click_ = second_click_ = false;
    initialized_ = true;
}

uint64_t PlayerGestures::HoldTime() const {
    if (phase_ == Phase::kPower) {
        return kPowerHoldMs;
    }
    return phase_ == Phase::kChord ? kParentHoldMs : kTrackHoldMs;
}

GestureAction PlayerGestures::LongAction() const {
    if (phase_ == Phase::kPower) {
        return GestureAction::kShutdownRequest;
    }
    if (phase_ == Phase::kChord) {
        // The future executor exits AI first; recognizing this request does
        // not perform that transition or open a page here.
        return GestureAction::kParentPageRequest;
    }
    const bool up = phase_ == Phase::kUp;
    if (context_.mode == GestureMode::kDirectory) {
        return up ? GestureAction::kBrowseNext : GestureAction::kBrowsePrevious;
    }
    if (context_.mode == GestureMode::kAi) {
        return GestureAction::kNone;
    }
    return up ? GestureAction::kNextTrack : GestureAction::kPreviousTrack;
}

GestureAction PlayerGestures::Finish(uint64_t now_ms) {
    if (now_ms - started_ >= HoldTime()) {
        return LongAction();
    }
    if (phase_ == Phase::kPower) {
        if (context_.mode == GestureMode::kDirectory) {
            return GestureAction::kConfirmCollection;
        }
        if (context_.mode == GestureMode::kAi) {
            return GestureAction::kCancelAi;
        }
        if (second_click_) {
            return context_.ai_ready ? GestureAction::kOpenAi : GestureAction::kOfflineHint;
        }
        pending_click_ = true;
        released_ = now_ms;
        return GestureAction::kNone;
    }
    if (phase_ == Phase::kChord) {
        // Once this is a long-hold attempt, early release cancels it instead
        // of accidentally opening the collection picker (DESIGN parent gate).
        if (now_ms - started_ >= kTrackHoldMs) {
            return GestureAction::kNone;
        }
        return context_.mode == GestureMode::kPlayer ? GestureAction::kOpenDirectory
                                                    : GestureAction::kNone;
    }
    const bool up = phase_ == Phase::kUp;
    if (context_.mode == GestureMode::kDirectory) {
        return up ? GestureAction::kBrowseNext : GestureAction::kBrowsePrevious;
    }
    return up ? GestureAction::kVolumeUp : GestureAction::kVolumeDown;
}

GestureAction PlayerGestures::Edge(uint8_t pressed, uint64_t now_ms) {
    if (phase_ == Phase::kConsumed) {
        if (pressed == 0) {
            phase_ = Phase::kIdle;
        }
        return GestureAction::kNone;
    }
    if (phase_ == Phase::kIdle) {
        if (pressed == 0) {
            return GestureAction::kNone;
        }
        second_click_ = pending_click_ && pressed == kPower && now_ms - released_ <= kDoubleMs;
        pending_click_ = false;
        started_ = now_ms;
        switch (pressed) {
            case kPower:
                phase_ = Phase::kPower;
                break;
            case kUp:
                phase_ = Phase::kUp;
                break;
            case kDown:
                phase_ = Phase::kDown;
                break;
            case kUp | kDown:
                phase_ = Phase::kChord;
                break;
            default:
                phase_ = Phase::kConsumed;
                break;
        }
        return GestureAction::kNone;
    }
    if ((phase_ == Phase::kUp || phase_ == Phase::kDown) && pressed == (kUp | kDown) &&
        now_ms - started_ <= kChordGapMs) {
        phase_ = Phase::kChord;
        started_ = now_ms;
        return GestureAction::kNone;
    }
    if (pressed == 0 || (phase_ == Phase::kChord && (pressed == kUp || pressed == kDown))) {
        const auto result = Finish(now_ms);
        phase_ = pressed == 0 ? Phase::kIdle : Phase::kConsumed;
        return result;
    }
    // Invalid overlaps never fall back to an individual key action.
    phase_ = Phase::kConsumed;
    pending_click_ = false;
    return GestureAction::kNone;
}

GestureAction PlayerGestures::Update(uint8_t pressed, uint64_t now_ms, GestureContext context) {
    if (!initialized_ || (pressed & ~uint8_t{7}) != 0 || now_ms < last_ms_ ||
        context.mode != context_.mode) {
        Cancel(pressed, now_ms, context);
        return GestureAction::kNone;
    }
    last_ms_ = now_ms;
    context_ = context;
    if (pressed != raw_) {
        raw_ = pressed;
        raw_since_ = now_ms;
    }
    auto result = GestureAction::kNone;
    if (pending_click_ && phase_ == Phase::kIdle && now_ms - released_ >= kDoubleMs &&
        !(raw_ == kPower && now_ms - released_ == kDoubleMs)) {
        pending_click_ = false;
        result = GestureAction::kTogglePlayback;
    }
    if (raw_ != stable_ && now_ms - raw_since_ >= kDebounceMs) {
        stable_ = raw_;
        const auto edge = Edge(stable_, now_ms);
        return result == GestureAction::kNone ? edge : result;
    }
    if (raw_ == stable_ && now_ms - raw_since_ >= kDebounceMs) {
        if (phase_ == Phase::kConsumed && stable_ == 0) {
            phase_ = Phase::kIdle;
        } else if (phase_ != Phase::kIdle && phase_ != Phase::kConsumed &&
                   now_ms - started_ >= HoldTime()) {
            const auto action = LongAction();
            phase_ = Phase::kConsumed;
            return action;
        }
    }
    return result;
}

}  // namespace media
