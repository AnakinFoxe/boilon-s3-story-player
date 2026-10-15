import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "main" / "media" / "media_catalog.cc"
TEST_SOURCE = ROOT / "scripts" / "tests" / "cpp" / "media_catalog_test.cc"


class MediaCatalogHostTest(unittest.TestCase):
    def _compiler(self):
        configured = os.environ.get("CXX")
        if configured:
            return shlex.split(configured)
        compiler = shutil.which("c++")
        self.assertIsNotNone(compiler, "A host C++ compiler is required for media catalog tests")
        return [compiler]

    def _run(self, command):
        result = subprocess.run(
            command,
            cwd=ROOT,
            check=False,
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            self.fail(
                f"command failed with exit code {result.returncode}:\n"
                f"{' '.join(map(str, command))}\n"
                f"stdout:\n{result.stdout}\n"
                f"stderr:\n{result.stderr}"
            )

    def test_media_catalog_cpp_suite(self):
        with tempfile.TemporaryDirectory(prefix="media-catalog-test-") as directory:
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
                    str(SOURCE),
                    str(TEST_SOURCE),
                    "-o",
                    str(executable),
                ]
                self._run(compile_command)
                self._run([str(executable)])


if __name__ == "__main__":
    unittest.main()
