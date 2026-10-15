"""Run the M5 batch 5 power-off rules (m5_power_policy.h) unchanged.

Low battery warns once at 10% and powers off after three readings in a row at
or below 3%; three minutes with nothing to hear powers off; so does a minute
in upstream's upgrade state. The board loops only feed these inputs.
"""

import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/boilon-s3-gen2"

SOURCE = r'''
#include <cassert>
#include <string>
#include "m5_power_policy.h"
using namespace boilon::power;
using Off = PowerWatch::Off;

static void Battery() {
    PowerWatch watch;
    uint64_t now = 0;
    auto feed = [&](int percent) { return watch.Update(now += 5000, percent, false, false); };
    assert(feed(-1) == Off::kNone && !watch.ShowWarning(true, now));  // Unknown: nothing.
    assert(feed(11) == Off::kNone && !watch.ShowWarning(true, now));
    assert(feed(10) == Off::kNone);
    // It waits for a page that can show it, then stays kWarningMs.
    assert(!watch.ShowWarning(false, now + 100));
    assert(watch.ShowWarning(true, now + 200));
    assert(!watch.ShowWarning(false, now + 300));  // Covered meanwhile: not drawn.
    assert(watch.ShowWarning(true, now + 199 + kWarningMs));
    assert(!watch.ShowWarning(true, now + 200 + kWarningMs));
    now += kWarningMs;
    assert(feed(9) == Off::kNone && !watch.ShowWarning(true, now));  // Once a boot.
    assert(feed(12) == Off::kNone && feed(10) == Off::kNone && !watch.ShowWarning(true, now));
    // Three readings in a row at or below 3%; a higher or unknown one starts over.
    assert(feed(3) == Off::kNone && feed(2) == Off::kNone);
    assert(feed(4) == Off::kNone);
    assert(feed(3) == Off::kNone && feed(3) == Off::kNone);
    assert(feed(-1) == Off::kNone);
    assert(feed(1) == Off::kNone && feed(0) == Off::kNone && feed(3) == Off::kLowBattery);
    // Ticks without a new reading do not count as readings.
    PowerWatch ticks;
    assert(ticks.Update(1, 2, false, false) == Off::kNone);
    for (uint64_t t = 2; t < 20000; t += 10)
        assert(ticks.Update(t, std::nullopt, false, false) == Off::kNone);
    assert(ticks.Update(20000, 2, false, false) == Off::kNone);
    assert(ticks.Update(25000, 2, false, false) == Off::kLowBattery);
    // Booting already empty warns at the first reading and powers off at the third.
    PowerWatch boot;
    assert(boot.Update(2000, 1, false, false) == Off::kNone && boot.ShowWarning(true, 2000));
    assert(boot.Update(7000, 1, false, false) == Off::kNone);
    assert(boot.Update(12000, 1, false, false) == Off::kLowBattery);
}

static void Idle() {
    PowerWatch watch;
    assert(watch.Update(1000, std::nullopt, true, false) == Off::kNone);
    assert(watch.Update(1000 + kIdleMs - 1, std::nullopt, true, false) == Off::kNone);
    assert(watch.Update(1000 + kIdleMs - 1 + 10, std::nullopt, false, false) == Off::kNone);  // A key.
    assert(watch.Update(1000 + kIdleMs + 20, std::nullopt, true, false) == Off::kNone);
    assert(watch.Update(1000 + 2 * kIdleMs + 19, std::nullopt, true, false) == Off::kNone);
    assert(watch.Update(1000 + 2 * kIdleMs + 20, std::nullopt, true, false) == Off::kIdle);
}

static void Upgrade() {
    PowerWatch watch;
    assert(watch.Update(500, std::nullopt, false, true) == Off::kNone);
    assert(watch.Update(500 + kUpgradeMs - 1, std::nullopt, false, true) == Off::kNone);
    assert(watch.Update(500 + kUpgradeMs, std::nullopt, false, false) == Off::kNone);  // Left it.
    assert(watch.Update(600 + kUpgradeMs, std::nullopt, false, true) == Off::kNone);
    assert(watch.Update(600 + 2 * kUpgradeMs, std::nullopt, false, true) == Off::kUpgrade);
    Hold hold(100);
    assert(!hold.Update(true, 0) && !hold.Update(true, 99) && hold.Update(true, 100));
    assert(!hold.Update(false, 101) && !hold.Update(true, 102) && hold.Update(true, 202));
}

int main() {
    assert(std::string(PowerWatch::kOffNames[static_cast<int>(Off::kUpgrade)]) == "upgrade");
    Battery();
    Idle();
    Upgrade();
}
'''


class M5PowerPolicyHostTests(unittest.TestCase):
    def test_power_off_rules(self):
        compiler = shlex.split(os.environ["CXX"]) if "CXX" in os.environ else [shutil.which("c++")]
        self.assertTrue(compiler[0], "Native C++ compiler required; do not silently skip")
        with tempfile.TemporaryDirectory(prefix="m5-power-") as temporary:
            executable = Path(temporary) / "m5-power-test"
            compiled = subprocess.run(
                compiler + ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                            "-I", str(BOARD), "-x", "c++", "-", "-o", str(executable)],
                input=SOURCE, text=True, capture_output=True, timeout=60, cwd=ROOT)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            tested = subprocess.run([str(executable)], text=True, capture_output=True,
                                    timeout=10, cwd=temporary)
            self.assertEqual(tested.returncode, 0, tested.stdout + tested.stderr)

    def test_both_variants_and_the_no_story_page_feed_the_watch(self):
        ai = (BOARD / "m3_ai_main.cc").read_text(encoding="utf-8")
        library = (BOARD / "m2_library_main.cc").read_text(encoding="utf-8")
        run = ai.split("[[noreturn]] void RunPlayer() {", 1)[1]
        no_story = ai.split("[[noreturn]] void NoStory(", 1)[1].split("\n}\n", 1)[0]
        loop = library.split('extern "C" void app_main() {', 1)[1]
        empty = library.split("[[noreturn]] void EmptyLibrary(", 1)[1].split("\n}\n", 1)[0]
        for body in (run, loop):
            # A reading counts once: only when the worker finished a new sample.
            self.assertIn("if (persistence && persistence->BatterySamples() != battery_samples) {",
                          body)
            self.assertIn("runtime.Controller().State() != media::PlaybackState::kPlaying", body)
            self.assertIn("? boilon::ui::Page::kLowBattery", body)
            self.assertIn("model.toast = boilon::ui::Toast::kLowBattery;", body)
            self.assertLess(body.index("power.Update("), body.index("lcd.Poll(now);"))
        self.assertIn("power.Update(now, percent, idle, focus.Upgrading())", run)
        self.assertIn("!session.OwnsKeys() && !network_page", run)
        self.assertIn("power.Update(now, percent, idle, false)", loop)
        self.assertIn("power.Update(now, percent, idle, focus.Upgrading())", no_story)
        self.assertIn("!page.Conversation().OwnsKeys() && !network_page", no_story)
        self.assertIn("NoStoryOff(lcd, off);", no_story)
        self.assertIn("percent = battery_level.load();", no_story)
        self.assertIn("if (idle.Update(!Pressed(), NowMs()))\n            PowerOff(\"USER\");", empty)


if __name__ == "__main__":
    unittest.main()
