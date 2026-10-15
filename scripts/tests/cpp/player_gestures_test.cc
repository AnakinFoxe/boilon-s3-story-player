#include "media/player_gestures.h"

#include <initializer_list>
#include <iostream>
#include <vector>

namespace {

using media::GestureAction;
using media::GestureContext;
using media::GestureMode;
using media::PlayerGestures;

constexpr uint8_t kPower = PlayerGestures::kPower;
constexpr uint8_t kUp = PlayerGestures::kUp;
constexpr uint8_t kDown = PlayerGestures::kDown;
constexpr uint8_t kChord = kUp | kDown;
int failure_count = 0;

// Every elapsed virtual millisecond supplies a raw sample. Edge helpers wait
// through the documented 30ms debounce; no tests assume dropped physical edges.
class Rig {
public:
    explicit Rig(GestureContext initial = {}, uint8_t pressed = 0) : context(initial), mask(pressed) {
        Sample();
        Advance(30);
    }

    void Sample() {
        const auto action = gestures.Update(mask, now, context);
        if (action != GestureAction::kNone) {
            actions.push_back(action);
        }
    }

    void Advance(uint64_t milliseconds) {
        for (uint64_t elapsed = 0; elapsed < milliseconds; ++elapsed) {
            ++now;
            Sample();
        }
    }

    void Raw(uint8_t pressed) {
        mask = pressed;
        Sample();
    }

    void Edge(uint8_t pressed) {
        Raw(pressed);
        Advance(30);
    }

    void Tap(uint8_t pressed) {
        Edge(pressed);
        Advance(70);
        Edge(0);
    }

    void HoldAndRelease(uint8_t pressed, uint64_t accepted_duration) {
        Edge(pressed);
        Advance(accepted_duration - 30);
        Edge(0);
    }

    void Expect(std::initializer_list<GestureAction> expected, const char* file, int line) const {
        if (actions != std::vector<GestureAction>(expected)) {
            std::cerr << file << ':' << line << ": action sequence mismatch; expected";
            for (const auto action : expected) {
                std::cerr << ' ' << static_cast<int>(action);
            }
            std::cerr << "; got";
            for (const auto action : actions) {
                std::cerr << ' ' << static_cast<int>(action);
            }
            std::cerr << '\n';
            ++failure_count;
        }
    }

    GestureContext context;
    uint8_t mask = 0;
    uint64_t now = 0;

private:
    PlayerGestures gestures;
    std::vector<GestureAction> actions;
};

#define EXPECT(rig, ...) (rig).Expect({__VA_ARGS__}, __FILE__, __LINE__)
#define EXPECT_NONE(rig) (rig).Expect({}, __FILE__, __LINE__)

void TestDebounceAndSingleClick() {
    Rig rig;
    rig.Raw(kPower);
    rig.Advance(29);
    rig.Raw(0);
    rig.Advance(500);
    EXPECT_NONE(rig);
    rig.Tap(kPower);
    rig.Advance(399);
    EXPECT_NONE(rig);
    rig.Advance(1);
    EXPECT(rig, GestureAction::kTogglePlayback);
    rig.Advance(1000);
    EXPECT(rig, GestureAction::kTogglePlayback);

    Rig bounced;
    bounced.Edge(kUp);
    bounced.Raw(0);
    bounced.Advance(29);
    bounced.Raw(kUp);
    bounced.Advance(30);
    EXPECT_NONE(bounced);
    bounced.Edge(0);
    bounced.Advance(500);
    EXPECT(bounced, GestureAction::kVolumeUp);
}

void TestDoubleClickBoundaryAndReadiness() {
    for (const uint64_t gap : {399, 400, 401}) {
        Rig rig({GestureMode::kPlayer, true});
        rig.Tap(kPower);
        rig.Advance(gap - 30);
        rig.Edge(kPower);
        rig.Advance(70);
        rig.Edge(0);
        rig.Advance(500);
        if (gap <= 400) {
            EXPECT(rig, GestureAction::kOpenAi);
        } else {
            EXPECT(rig, GestureAction::kTogglePlayback, GestureAction::kTogglePlayback);
        }
    }
    for (const bool ready_at_end : {false, true}) {
        Rig rig({GestureMode::kPlayer, !ready_at_end});
        rig.Tap(kPower);
        rig.Edge(kPower);
        rig.context.ai_ready = ready_at_end;
        rig.Advance(70);
        rig.Edge(0);
        rig.Advance(1000);
        EXPECT(rig, ready_at_end ? GestureAction::kOpenAi : GestureAction::kOfflineHint);
    }

    Rig second_long({GestureMode::kPlayer, true});
    second_long.Tap(kPower);
    second_long.Edge(kPower);
    second_long.Advance(2000);
    EXPECT(second_long, GestureAction::kShutdownRequest);
    second_long.Advance(5000);
    second_long.Edge(0);
    second_long.Advance(500);
    EXPECT(second_long, GestureAction::kShutdownRequest);
}

void TestVolumeClicksAndHoldBoundaries() {
    for (const uint8_t key : {kUp, kDown}) {
        const auto volume = key == kUp ? GestureAction::kVolumeUp : GestureAction::kVolumeDown;
        const auto track = key == kUp ? GestureAction::kNextTrack : GestureAction::kPreviousTrack;
        Rig clicks;
        clicks.Tap(key);
        clicks.Tap(key);
        clicks.Tap(key);
        clicks.Advance(1000);
        EXPECT(clicks, volume, volume, volume);
        for (const uint64_t duration : {799, 800, 801}) {
            Rig boundary;
            boundary.HoldAndRelease(key, duration);
            boundary.Advance(1000);
            EXPECT(boundary, duration < 800 ? volume : track);
        }
        Rig held;
        held.Edge(key);
        held.Advance(799);
        EXPECT_NONE(held);
        held.Advance(1);
        EXPECT(held, track);
        held.Advance(6000);
        held.Edge(0);
        EXPECT(held, track);
    }
}

void TestPowerHoldBoundariesAndModes() {
    for (const auto mode : {GestureMode::kPlayer, GestureMode::kDirectory, GestureMode::kAi}) {
        const auto short_action = mode == GestureMode::kPlayer ? GestureAction::kTogglePlayback
                                : mode == GestureMode::kDirectory ? GestureAction::kConfirmCollection
                                                                  : GestureAction::kCancelAi;
        for (const uint64_t duration : {1999, 2000, 2001}) {
            Rig boundary({mode, true});
            boundary.HoldAndRelease(kPower, duration);
            boundary.Advance(500);
            EXPECT(boundary, duration < 2000 ? short_action : GestureAction::kShutdownRequest);
        }
        Rig held({mode, true});
        held.Edge(kPower);
        held.Advance(1999);
        EXPECT_NONE(held);
        held.Advance(1);
        EXPECT(held, GestureAction::kShutdownRequest);
        held.Advance(6000);
        held.Edge(0);
        held.Advance(500);
        EXPECT(held, GestureAction::kShutdownRequest);
    }
}

void TestChordGapAndReleaseTail() {
    for (const uint8_t first : {kUp, kDown}) {
        for (const uint64_t gap : {199, 200, 201}) {
            Rig rig;
            rig.Edge(first);
            rig.Advance(gap - 30);
            rig.Edge(kChord);
            rig.Advance(500);
            EXPECT_NONE(rig);
            rig.Edge(first);
            if (gap <= 200) {
                EXPECT(rig, GestureAction::kOpenDirectory);
            } else {
                EXPECT_NONE(rig);
            }
            rig.Advance(6000);
            rig.Edge(0);
            rig.Advance(500);
            if (gap <= 200) {
                EXPECT(rig, GestureAction::kOpenDirectory);
            } else {
                EXPECT_NONE(rig);
            }
            rig.Tap(kUp);
            if (gap <= 200) {
                EXPECT(rig, GestureAction::kOpenDirectory, GestureAction::kVolumeUp);
            } else {
                EXPECT(rig, GestureAction::kVolumeUp);
            }
        }
    }
    for (const uint64_t duration : {799, 800, 801, 4999, 5000, 5001}) {
        Rig boundary;
        boundary.HoldAndRelease(kChord, duration);
        boundary.Advance(1000);
        if (duration < 800) {
            EXPECT(boundary, GestureAction::kOpenDirectory);
        } else if (duration < 5000) {
            EXPECT_NONE(boundary);
        } else {
            EXPECT(boundary, GestureAction::kParentPageRequest);
        }
    }
    Rig held;
    held.Edge(kUp);
    held.Advance(170);
    held.Edge(kChord);
    held.Advance(4999);
    EXPECT_NONE(held);
    held.Advance(1);
    EXPECT(held, GestureAction::kParentPageRequest);
    held.Advance(6000);
    held.Edge(kDown);
    held.Advance(6000);
    held.Edge(0);
    EXPECT(held, GestureAction::kParentPageRequest);
}

void TestInvalidOverlapAndPendingClick() {
    for (const uint8_t overlap : {uint8_t(kPower | kUp), uint8_t(kPower | kDown), uint8_t(7)}) {
        Rig rig;
        rig.Edge(kPower);
        rig.Advance(100);
        rig.Edge(overlap);
        rig.Advance(6000);
        rig.Edge(kPower);
        rig.Advance(6000);
        rig.Edge(0);
        rig.Advance(500);
        EXPECT_NONE(rig);
        rig.Tap(kDown);
        EXPECT(rig, GestureAction::kVolumeDown);
    }
    Rig canceled;
    canceled.Tap(kPower);
    canceled.Tap(kUp);
    canceled.Advance(1000);
    EXPECT(canceled, GestureAction::kVolumeUp);

    Rig expired;
    expired.Tap(kPower);
    expired.Advance(371);
    expired.Edge(kUp);
    EXPECT(expired, GestureAction::kTogglePlayback);
    expired.Edge(0);
    expired.Advance(500);
    EXPECT(expired, GestureAction::kTogglePlayback, GestureAction::kVolumeUp);
}

void TestDirectoryAndAiRouting() {
    for (const auto mode : {GestureMode::kDirectory, GestureMode::kAi}) {
        Rig power({mode, true});
        power.Tap(kPower);
        power.Tap(kPower);
        power.Advance(500);
        const auto action = mode == GestureMode::kDirectory ? GestureAction::kConfirmCollection
                                                           : GestureAction::kCancelAi;
        EXPECT(power, action, action);
        for (const uint8_t key : {kUp, kDown}) {
            Rig volume({mode, true});
            volume.Tap(key);
            volume.Edge(key);
            volume.Advance(800);
            volume.Advance(5000);
            volume.Edge(0);
            volume.Advance(500);
            if (mode == GestureMode::kDirectory) {
                const auto browse = key == kUp ? GestureAction::kBrowseNext
                                              : GestureAction::kBrowsePrevious;
                EXPECT(volume, browse, browse);
            } else {
                EXPECT(volume, key == kUp ? GestureAction::kVolumeUp : GestureAction::kVolumeDown);
            }
        }
        Rig chord({mode, true});
        chord.Tap(kChord);
        EXPECT_NONE(chord);
        chord.Edge(kChord);
        chord.Advance(6000);
        EXPECT(chord, GestureAction::kParentPageRequest);
        chord.Edge(kUp);
        chord.Advance(6000);
        chord.Edge(0);
        EXPECT(chord, GestureAction::kParentPageRequest);
    }
}

void CheckDrainAndRecovery(Rig& rig) {
    rig.Advance(6000);
    rig.Raw(0);
    rig.Advance(29);
    rig.Edge(kUp);
    rig.Advance(6000);
    EXPECT_NONE(rig);
    rig.Edge(0);
    EXPECT_NONE(rig);
    rig.Tap(kDown);
    EXPECT(rig, GestureAction::kVolumeDown);
}

void TestStartupAndCancellationDrain() {
    Rig startup({}, kPower);
    CheckDrainAndRecovery(startup);

    Rig changed;
    changed.Tap(kPower);
    changed.context.mode = GestureMode::kAi;
    changed.Raw(kPower);
    CheckDrainAndRecovery(changed);

    Rig reversed;
    reversed.Tap(kPower);
    reversed.now -= 1;
    reversed.Raw(kPower);
    CheckDrainAndRecovery(reversed);

    Rig unknown;
    unknown.Tap(kPower);
    unknown.Raw(uint8_t(kPower | 8));
    unknown.Edge(kPower);
    CheckDrainAndRecovery(unknown);
}

}  // namespace

int main() {
    TestDebounceAndSingleClick();
    TestDoubleClickBoundaryAndReadiness();
    TestVolumeClicksAndHoldBoundaries();
    TestPowerHoldBoundariesAndModes();
    TestChordGapAndReleaseTail();
    TestInvalidOverlapAndPendingClick();
    TestDirectoryAndAiRouting();
    TestStartupAndCancellationDrain();
    if (failure_count != 0) {
        std::cerr << failure_count << " player gesture checks failed\n";
        return 1;
    }
    std::cout << "player gestures: 8 scenario groups passed\n";
    return 0;
}
