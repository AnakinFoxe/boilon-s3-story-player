import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CATALOG_SOURCE = ROOT / "main" / "media" / "media_catalog.cc"
CONTROLLER_SOURCE = ROOT / "main" / "media" / "player_controller.cc"
TEST_SOURCE = ROOT / "scripts" / "tests" / "cpp" / "player_controller_test.cc"


class PlayerControllerHostTest(unittest.TestCase):
    def _compiler(self):
        configured = os.environ.get("CXX")
        if configured:
            return shlex.split(configured)
        compiler = shutil.which("c++")
        self.assertIsNotNone(compiler, "A host C++ compiler is required for player controller tests")
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

    def test_player_controller_cpp_suite(self):
        with tempfile.TemporaryDirectory(prefix="player-controller-test-") as directory:
            output_directory = Path(directory)
            for char_flag, variant in (
                ("-fsigned-char", "signed-char"),
                ("-funsigned-char", "unsigned-char"),
            ):
                executable = output_directory / variant
                compile_command = self._compiler() + [
                    "-std=c++17",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-pedantic",
                    char_flag,
                    "-I",
                    str(ROOT / "main"),
                    str(CATALOG_SOURCE),
                    str(CONTROLLER_SOURCE),
                    str(TEST_SOURCE),
                    "-o",
                    str(executable),
                ]
                self._run(compile_command)
                self._run([str(executable)], timeout=10)

    def test_fixed_host_demo(self):
        output = self._run(
            [sys.executable, "-B", str(ROOT / "scripts/boilon/run_player_controller_demo.py")],
            timeout=90,
        )
        self.assertIn("第1集.mp3 第2集.mp3 第10集.mp3", output)
        checks = (
            ("继续同一首", "state=Playing action=Resume", "第1集.mp3"),
            ("暂停时切到第2集", "state=Paused action=Stop", "第2集.mp3"),
            ("从新曲开头开始", "state=Playing action=Start", "第2集.mp3"),
            ("目录播完：默认停止", "state=Stopped action=Stop", "第10集.mp3"),
            ("目录播完：循环到第1集", "state=Playing action=Start", "第1集.mp3"),
        )
        for command, state_action, track in checks:
            with self.subTest(command=command):
                lines = [line for line in output.splitlines() if line.startswith(command)]
                self.assertEqual(len(lines), 1)
                self.assertIn(state_action, lines[0])
                self.assertIn(track, lines[0])


if __name__ == "__main__":
    unittest.main()
