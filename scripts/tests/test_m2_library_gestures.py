"""Boilon seek chords with the real ordinary and shutdown recognizers."""

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

SOURCE = r'''
#include "boards/boilon-s3-gen2/m2_library_gestures.h"

#include <cassert>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

using media::GestureAction;
using media::GestureContext;
using media::GestureMode;
using media::PlayerGestures;
constexpr uint8_t P = PlayerGestures::kPower;
constexpr uint8_t U = PlayerGestures::kUp;
constexpr uint8_t D = PlayerGestures::kDown;

// Main's independent RequestedShutdown recognizer samples before Runtime.
// Both receive every virtual millisecond, including raw bounce and release.
struct Rig {
    explicit Rig(uint8_t held = 0, GestureMode mode = GestureMode::kPlayer)
        : context{mode, false}, mask(held) {
        Sample();
        Advance(30);
    }
    void Sample() {
        auto shutdown = main.Update(mask, now);
        if (shutdown != GestureAction::kNone)
            main_actions.push_back(shutdown);
        const auto request = runtime.Update(mask, now, context);
        if (request.action != GestureAction::kNone)
            actions.push_back(request.action);
        if (request.seek_seconds)
            seeks.push_back(request.seek_seconds);
    }
    void Advance(uint64_t elapsed) {
        while (elapsed--) {
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
    void QuietChord() const {
        assert(actions.empty());
        assert(main_actions.empty());
    }
    PlayerGestures main;
    boilon::local_library::LibraryGestures runtime;
    GestureContext context;
    uint8_t mask = 0;
    uint64_t now = 0;
    std::vector<GestureAction> actions, main_actions;
    std::vector<int> seeks;
};

void Repeated() {
    Rig rig;
    rig.Edge(P);
    for (int step = 1; step <= 3; ++step) {
        rig.Edge(P | U);
        assert(rig.seeks.size() == static_cast<size_t>(step));
        rig.Edge(P);
    }
    rig.Edge(P | D);
    rig.Edge(P);
    assert((rig.seeks == std::vector<int>{-30, -30, -30, 30}));
    assert(std::accumulate(rig.seeks.begin(), rig.seeks.end(), 0) == -60);
    rig.Advance(2500);
    rig.QuietChord();
    rig.Edge(0);
    rig.Advance(500);
    rig.QuietChord();
}

void HeldAuxiliary() {
    for (uint8_t auxiliary : {U, D}) {
        Rig rig;
        rig.Edge(P);
        rig.Edge(P | auxiliary);
        rig.Advance(6000);
        assert(rig.seeks.size() == 1);
        assert(rig.seeks[0] == (auxiliary == U ? -30 : 30));
        rig.Edge(P);
        rig.Advance(2500);
        rig.QuietChord();
        rig.Edge(0);
        rig.Advance(500);
        rig.QuietChord();
    }
}

void FreshShutdown() {
    Rig rig;
    rig.Edge(P);
    rig.Edge(P | U);
    rig.Edge(P);
    rig.Advance(2500);
    rig.Edge(0);
    rig.Advance(500);
    rig.QuietChord();
    rig.Edge(P);
    rig.Advance(2000);
    assert((rig.actions == std::vector<GestureAction>{GestureAction::kShutdownRequest}));
    assert((rig.main_actions == std::vector<GestureAction>{GestureAction::kShutdownRequest}));
    rig.Advance(2000);
    assert(rig.actions.size() == 1 && rig.main_actions.size() == 1);
    assert(rig.seeks.size() == 1);
}

// The list runs top to bottom: the middle key moves up, the bottom key down.
void Directory() {
    Rig rig(0, GestureMode::kDirectory);
    rig.Tap(U);
    rig.Tap(D);
    rig.Tap(P);
    rig.Advance(500);
    assert((rig.actions == std::vector<GestureAction>{
        GestureAction::kBrowsePrevious, GestureAction::kBrowseNext,
        GestureAction::kConfirmCollection}));
    rig.Edge(P);
    rig.Edge(P | U);
    rig.Advance(2500);
    rig.Edge(0);
    rig.Advance(500);
    assert(rig.seeks.empty());
    assert(rig.actions.size() == 3);
    rig.Edge(U);
    rig.Advance(PlayerGestures::kTrackHoldMs);
    rig.Edge(0);
    rig.Edge(D);
    rig.Advance(PlayerGestures::kTrackHoldMs);
    rig.Edge(0);
    assert((rig.actions == std::vector<GestureAction>{
        GestureAction::kBrowsePrevious, GestureAction::kBrowseNext,
        GestureAction::kConfirmCollection, GestureAction::kBrowsePrevious,
        GestureAction::kBrowseNext}));
}

void PlayerDirections() {
    Rig rig;
    rig.Tap(U);
    rig.Tap(D);
    assert((rig.actions == std::vector<GestureAction>{
        GestureAction::kVolumeUp, GestureAction::kVolumeDown}));
    rig.Edge(U);
    rig.Advance(PlayerGestures::kTrackHoldMs);
    assert(rig.actions.back() == GestureAction::kPreviousTrack);
    rig.Advance(2000);
    assert(rig.actions.size() == 3);
    rig.Edge(0);
    rig.Edge(D);
    rig.Advance(PlayerGestures::kTrackHoldMs);
    assert(rig.actions.back() == GestureAction::kNextTrack);
    rig.Advance(2000);
    rig.Edge(0);
    assert(rig.actions.size() == 4 && rig.seeks.empty());
}

void InitialHeld() {
    Rig rig(P | U);
    rig.Advance(2500);
    rig.Edge(P);
    rig.Edge(P | D);
    rig.Advance(2500);
    rig.QuietChord();
    assert(rig.seeks.empty());
    rig.Edge(0);
    rig.Edge(P);
    rig.Edge(P | U);
    assert((rig.seeks == std::vector<int>{-30}));
}

void ResetDrain() {
    for (bool clock_back : {false, true}) {
        Rig rig;
        rig.Edge(P);
        rig.Edge(P | U);
        assert(rig.seeks.size() == 1);
        if (clock_back) {
            rig.now = 1;
            rig.Sample();
        } else {
            rig.context.mode = GestureMode::kDirectory;
            rig.Sample();
            rig.context.mode = GestureMode::kPlayer;
            rig.Sample();
        }
        rig.Edge(P);
        rig.Edge(P | D);
        rig.Advance(2500);
        rig.QuietChord();
        assert(rig.seeks.size() == 1);
        rig.Edge(0);
        rig.Edge(P);
        rig.Edge(P | D);
        assert((rig.seeks == std::vector<int>{-30, 30}));
    }
}

void InvalidDrain() {
    {
        Rig rig;
        rig.Edge(P);
        rig.Edge(P | U | D);
        rig.Edge(P | U);
        rig.Edge(P);
        rig.Edge(P | D);
        rig.Advance(2500);
        rig.QuietChord();
        assert(rig.seeks.empty());
        rig.Edge(0);
        rig.Edge(P);
        rig.Edge(P | U);
        assert((rig.seeks == std::vector<int>{-30}));
    }
    {
        Rig rig;
        rig.Edge(P);
        rig.Edge(P | U);
        rig.Edge(U);  // Top released first: the entire sequence must drain.
        rig.Edge(P | U);
        rig.Edge(P);
        rig.Edge(P | D);
        rig.Advance(2500);
        rig.QuietChord();
        assert((rig.seeks == std::vector<int>{-30}));
        rig.Edge(0);
        rig.Edge(P);
        rig.Edge(P | D);
        assert((rig.seeks == std::vector<int>{-30, 30}));
    }
}

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string scenario = argv[1];
    if (scenario == "repeated") Repeated();
    else if (scenario == "held") HeldAuxiliary();
    else if (scenario == "shutdown") FreshShutdown();
    else if (scenario == "directory") Directory();
    else if (scenario == "directions") PlayerDirections();
    else if (scenario == "initial") InitialHeld();
    else if (scenario == "reset") ResetDrain();
    else if (scenario == "invalid") InvalidDrain();
    else return 2;
    std::cout << scenario << " PASS\n";
}
'''


class M2LibraryGesturesTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ["CXX"]) if "CXX" in os.environ else [shutil.which("c++")]
        if not compiler or not compiler[0]:
            raise AssertionError("existing native C++ compiler required; no download")
        cls.temporary = tempfile.TemporaryDirectory(prefix="m2-library-gestures-")
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        source = directory / "gestures.cc"
        source.write_text(SOURCE, encoding="utf-8")
        cls.executable = directory / "gestures-test"
        command = compiler + ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                              "-I", str(ROOT / "main"), str(source),
                              str(ROOT / "main/media/player_gestures.cc"),
                              "-o", str(cls.executable)]
        result = subprocess.run(command, capture_output=True, text=True,
                                stdin=subprocess.DEVNULL, timeout=30, cwd=ROOT)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def scenario(self, name):
        result = subprocess.run([str(self.executable), name], capture_output=True, text=True,
                                stdin=subprocess.DEVNULL, timeout=10, cwd=ROOT)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(name + " PASS", result.stdout)

    def test_repeated_steps_accumulate_and_suppress_ordinary_actions(self):
        self.scenario("repeated")

    def test_auxiliary_hold_only_steps_once(self):
        self.scenario("held")

    def test_independent_top_hold_after_full_release_still_shuts_down(self):
        self.scenario("shutdown")

    def test_directory_browse_and_confirmation_preserve_old_actions(self):
        self.scenario("directory")

    def test_player_long_directions_match_marks_and_short_volume_is_unchanged(self):
        self.scenario("directions")

    def test_initial_held_keys_require_full_release(self):
        self.scenario("initial")

    def test_clock_reversal_and_mode_change_drain_current_hold(self):
        self.scenario("reset")

    def test_three_key_overlap_and_top_first_release_drain_until_full_release(self):
        self.scenario("invalid")


if __name__ == "__main__":
    unittest.main()
