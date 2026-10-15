"""Run the production board runtime with virtual keys and a fake decoder."""

import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class M2LibraryRuntimeHostTests(unittest.TestCase):
    def test_production_runtime(self):
        configured = os.environ.get("CXX")
        compiler = shlex.split(configured) if configured else [shutil.which("c++")]
        self.assertTrue(compiler[0], "A native C++ compiler is required")
        with tempfile.TemporaryDirectory(prefix="m2-library-runtime-") as directory:
            executable = Path(directory) / "runtime-test"
            command = compiler + [
                "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                "-I", str(ROOT / "main"),
                str(ROOT / "main/media/media_catalog.cc"),
                str(ROOT / "main/media/player_controller.cc"),
                str(ROOT / "main/media/player_gestures.cc"),
                str(ROOT / "scripts/tests/cpp/m2_library_runtime_test.cc"),
                "-o", str(executable),
            ]
            result = subprocess.run(command, capture_output=True, text=True,
                                    stdin=subprocess.DEVNULL, timeout=30, cwd=ROOT)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True,
                                    stdin=subprocess.DEVNULL, timeout=10, cwd=directory)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            print(result.stdout, end="")


if __name__ == "__main__":
    unittest.main()
