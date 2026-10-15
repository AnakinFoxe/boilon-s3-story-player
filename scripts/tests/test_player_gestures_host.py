import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
GESTURES_SOURCE = ROOT / "main" / "media" / "player_gestures.cc"
TEST_SOURCE = ROOT / "scripts" / "tests" / "cpp" / "player_gestures_test.cc"


class PlayerGesturesHostTest(unittest.TestCase):
    def _compiler(self):
        configured = os.environ.get("CXX")
        if configured:
            return shlex.split(configured)
        compiler = shutil.which("c++")
        self.assertIsNotNone(compiler, "A host C++ compiler is required for player gesture tests")
        return [compiler]

    def _run(self, command, timeout=60):
        result = subprocess.run(
            command,
            cwd=ROOT,
            check=False,
            capture_output=True,
            text=True,
            stdin=subprocess.DEVNULL,
            timeout=timeout,
        )
        if result.returncode != 0:
            self.fail(
                f"command failed with exit code {result.returncode}:\n"
                f"{' '.join(map(str, command))}\n"
                f"stdout:\n{result.stdout}\n"
                f"stderr:\n{result.stderr}"
            )
        return result.stdout

    def test_player_gestures_cpp_suite(self):
        with tempfile.TemporaryDirectory(prefix="player-gestures-test-") as directory:
            executable = Path(directory) / "player-gestures-test"
            self._run(self._compiler() + [
                "-std=c++17",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-pedantic",
                "-I",
                str(ROOT / "main"),
                str(GESTURES_SOURCE),
                str(TEST_SOURCE),
                "-o",
                str(executable),
            ])
            output = self._run([str(executable)], timeout=10)
            self.assertIn("player gestures: 8 scenario groups passed", output)

    def test_gestures_drive_existing_controller_demo(self):
        output = self._run(
            [sys.executable, "-B", str(ROOT / "scripts/boilon/run_player_controller_demo.py")],
            timeout=90,
        )
        gestures = [line for line in output.splitlines() if line.startswith("gesture=")]
        expected = (
            ("TogglePlayback", "state=Paused action=Pause", "第1集.mp3"),
            ("TogglePlayback", "state=Playing action=Resume", "第1集.mp3"),
            ("NextTrack", "state=Playing action=Start", "第2集.mp3"),
            ("PreviousTrack", "state=Playing action=Start", "第1集.mp3"),
            ("OfflineHint", "state=Playing", "request_only"),
            ("OpenDirectory", "state=Playing", "request_only"),
            ("ParentPageRequest", "state=Playing", "request_only"),
            ("ShutdownRequest", "state=Playing", "request_only"),
        )
        self.assertEqual(len(gestures), len(expected))
        for line, (action, state, detail) in zip(gestures, expected):
            with self.subTest(action=action):
                self.assertTrue(line.startswith(f"gesture={action} "))
                self.assertIn(state, line)
                self.assertIn(detail, line)


if __name__ == "__main__":
    unittest.main()
