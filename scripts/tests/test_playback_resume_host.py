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
RESUME_SOURCE = ROOT / "main" / "media" / "playback_resume.cc"
TEST_SOURCE = ROOT / "scripts" / "tests" / "cpp" / "playback_resume_test.cc"


class PlaybackResumeHostTest(unittest.TestCase):
    def _compiler(self):
        configured = os.environ.get("CXX")
        if configured:
            return shlex.split(configured)
        compiler = shutil.which("c++")
        self.assertIsNotNone(compiler, "A host C++ compiler is required for playback resume tests")
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

    def test_playback_resume_cpp_suite(self):
        with tempfile.TemporaryDirectory(prefix="playback-resume-test-") as directory:
            executable = Path(directory) / "playback-resume-test"
            self._run(self._compiler() + [
                "-std=c++17",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-pedantic",
                "-I",
                str(ROOT / "main"),
                str(CATALOG_SOURCE),
                str(RESUME_SOURCE),
                str(TEST_SOURCE),
                "-o",
                str(executable),
            ])
            output = self._run([str(executable)], timeout=10)
            self.assertIn("playback resume: 10 scenario groups passed", output)

    def test_resume_host_demo(self):
        output = self._run(
            [sys.executable, "-B", str(ROOT / "scripts/boilon/run_player_controller_demo.py")],
            timeout=90,
        )
        self.assertIn("checkpoint=periodic_and_pause PASS writes=4", output)
        self.assertIn("checkpoint=write_failed old_success_ms=31001", output)
        expected = {
            "return_story": ("Ready", "91000", "0", "keep_current", "第1集.mp3"),
            "boot_last": ("Ready", "40000", "1", "40", "音乐.mp3"),
            "after_failed_save": ("Ready", "40000", "1", "40", "音乐.mp3"),
            "changed_fingerprint": ("StaleMedia", "0", "0", "keep_current", None),
        }
        for label, (status, position, auto_play, volume, track) in expected.items():
            with self.subTest(label=label):
                lines = [line for line in output.splitlines() if line.startswith(f"resume={label} ")]
                self.assertEqual(len(lines), 1)
                fields = dict(field.split("=", 1) for field in lines[0].split())
                self.assertEqual(fields["status"], status)
                self.assertEqual(fields["position_ms"], position)
                self.assertEqual(fields["auto_play"], auto_play)
                self.assertEqual(fields["volume"], volume)
                self.assertEqual(fields.get("track"), track)


if __name__ == "__main__":
    unittest.main()
