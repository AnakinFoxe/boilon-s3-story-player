"""Factory voltage model and filtering in the board-local battery helper."""

import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class M2BatteryHostTests(unittest.TestCase):
    def compile_and_run(self, body):
        compiler = shlex.split(os.environ["CXX"]) if "CXX" in os.environ else [shutil.which("c++")]
        self.assertTrue(compiler and compiler[0], "An existing native C++ compiler is required")
        source = r'''
#include "boards/boilon-s3-gen2/m2_battery_estimate.h"
#include <cassert>
#include <cmath>
#include <limits>
using namespace boilon::local_library;
int main() {
''' + body + "\n}\n"
        with tempfile.TemporaryDirectory(prefix="m2-battery-") as directory:
            executable = Path(directory) / "battery-test"
            result = subprocess.run(
                compiler + ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                            "-I", str(ROOT / "main"), "-x", "c++", "-", "-o", str(executable)],
                input=source, capture_output=True, text=True, timeout=30, cwd=ROOT,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], stdin=subprocess.DEVNULL,
                                    capture_output=True, text=True, timeout=10, cwd=directory)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_factory_divider_requires_calibrated_voltage_and_cannot_overflow(self):
        self.compile_and_run(r'''
    assert(BatteryEstimate::Millivolts({1640, 1385}) == 4155);
    assert(BatteryEstimate::Millivolts({0, 1}) == 3);
    assert(BatteryEstimate::Millivolts({-1, 1385}) == -1);
    assert(BatteryEstimate::Millivolts({1640, -1}) == -1);
    assert(BatteryEstimate::Millivolts({1640, 0}) == -1);
    constexpr int maximum = std::numeric_limits<int>::max();
    assert(BatteryEstimate::Millivolts({1640, maximum / 3}) == maximum / 3 * 3);
    assert(BatteryEstimate::Millivolts({1640, maximum / 3 + 1}) == -1);
    assert(BatteryEstimate::Millivolts({1640, maximum}) == -1);
''')

    def test_factory_seven_point_curve_interpolates_and_bounds_voltage(self):
        self.compile_and_run(r'''
    const auto close = [](float actual, float expected) {
        assert(std::fabs(actual - expected) < 0.001f);
    };
    close(BatteryEstimate::Capacity(4200), 100);
    close(BatteryEstimate::Capacity(4060), 80);
    close(BatteryEstimate::Capacity(3820), 60);
    close(BatteryEstimate::Capacity(3580), 40);
    close(BatteryEstimate::Capacity(3340), 20);
    close(BatteryEstimate::Capacity(3100), 0);
    close(BatteryEstimate::Capacity(3000), -10);
    close(BatteryEstimate::Capacity(4130), 90);
    close(BatteryEstimate::Capacity(3940), 70);
    close(BatteryEstimate::Capacity(3700), 50);
    close(BatteryEstimate::Capacity(3460), 30);
    close(BatteryEstimate::Capacity(3220), 10);
    close(BatteryEstimate::Capacity(3050), -5);
    close(BatteryEstimate::Capacity(5000), 100);
    close(BatteryEstimate::Capacity(2000), -10);
    close(BatteryEstimate::Capacity(std::numeric_limits<int>::max()), 100);
    close(BatteryEstimate::Capacity(std::numeric_limits<int>::min()), -10);
''')

    def test_first_sample_filter_updates_both_directions_and_invalid_resets(self):
        self.compile_and_run(r'''
    BatteryEstimate estimate;
    assert(estimate.Update({1640, 1400}) == 100); // First sample has no ramp from zero.
    assert(estimate.Update({1640, 1274}) == 92);  // 100 -> 60.1667, alpha 0.2.
    assert(estimate.Update({1640, 1400}) == 93);  // Charging detection is unavailable: rises allowed.
    assert(estimate.Update({1640, -1}) == -1);
    assert(estimate.Update({1640, 1274}) == 60);  // Valid again: reset, no stale 93%.
    assert(estimate.Update({-1, 1400}) == -1);
    assert(estimate.Update({1640, 1400}) == 100);
    assert(estimate.Update({1640, std::numeric_limits<int>::max()}) == -1);
    assert(estimate.Update({1640, 1000}) == 0);   // Factory's -10 endpoint is not shown to users.
    assert(estimate.Update({1640, 0}) == -1);
    assert(estimate.Update({1640, 2000}) == 100); // Public result is bounded to 0..100.
''')


if __name__ == "__main__":
    unittest.main()
