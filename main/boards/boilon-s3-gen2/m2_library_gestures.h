#pragma once
#include "media/player_gestures.h"

namespace boilon::local_library {
// Board-only seek chords. The ordinary recognizer retains all other gestures.
class LibraryGestures {
public:
    struct Request {
        media::GestureAction action = media::GestureAction::kNone;
        int seek_seconds = 0;
    };
    Request Update(uint8_t pressed, uint64_t now, media::GestureContext context = {}) {
        using G = media::PlayerGestures;
        if (!initialized_ || now < last_ || context.mode != mode_ || (pressed & ~uint8_t{7})) {
            ordinary_ = G{};
            raw_ = stable_ = pressed;
            changed_ = now;
            chord_ = false;
            invalid_ = false;
            armed_ = false;
            initialized_ = true;
        }
        last_ = now;
        mode_ = context.mode;
        if (pressed != raw_) {
            raw_ = pressed;
            changed_ = now;
        }
        if (!pressed && now - changed_ >= G::kDebounceMs)
            armed_ = true;
        // Latch before debounce so the power hold cannot leak through a chord.
        if (armed_ && context.mode == media::GestureMode::kPlayer && (pressed & G::kPower) &&
            (pressed & (G::kUp | G::kDown)) && !chord_) {
            chord_ = true;
            ordinary_ = G{};
        }
        const auto action = ordinary_.Update(pressed, now, context);
        if (chord_ && (pressed == 7 || (pressed && !(pressed & G::kPower))))
            invalid_ = true;
        int seek = 0;
        if (raw_ != stable_ && now - changed_ >= G::kDebounceMs) {
            stable_ = raw_;
            if (chord_ && !invalid_ && stable_ == (G::kPower | G::kUp))
                seek = -30;
            else if (chord_ && !invalid_ && stable_ == (G::kPower | G::kDown))
                seek = 30;
        }
        if (chord_) {
            if (!stable_ && !raw_ && now - changed_ >= G::kDebounceMs) {
                chord_ = false;
                invalid_ = false;
            }
            return {media::GestureAction::kNone, seek};
        }
        // Physical middle/bottom symbols are previous/next: the middle key also
        // moves up the directory list, the bottom key down. Volume keeps up/down.
        using A = media::GestureAction;
        switch (action) {
            case A::kNextTrack:
                return {A::kPreviousTrack, 0};
            case A::kPreviousTrack:
                return {A::kNextTrack, 0};
            case A::kBrowseNext:
                return {A::kBrowsePrevious, 0};
            case A::kBrowsePrevious:
                return {A::kBrowseNext, 0};
            default:
                return {action, 0};
        }
    }

private:
    media::PlayerGestures ordinary_;
    media::GestureMode mode_ = media::GestureMode::kPlayer;
    uint8_t raw_ = 0, stable_ = 0;
    uint64_t changed_ = 0, last_ = 0;
    bool initialized_ = false, chord_ = false, armed_ = false, invalid_ = false;
};
}  // namespace boilon::local_library
