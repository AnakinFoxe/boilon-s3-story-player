"""Native production blob/session tests; NVS is a closed synthetic API fake."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class M2ResumeStoreTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ["CXX"]) if "CXX" in os.environ else [shutil.which("c++")]
        if not compiler or not compiler[0]:
            raise AssertionError("existing native compiler required; no download")
        cls.temporary = tempfile.TemporaryDirectory(prefix="m2-resume-store-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.executable = Path(cls.temporary.name) / "test"
        command = compiler + ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-I", str(ROOT / "scripts/tests/cpp/m2_resume_fakes"), "-I", str(ROOT / "main"),
            str(ROOT / "scripts/tests/cpp/m2_resume_store_test.cc"),
            str(ROOT / "main/media/media_catalog.cc"), str(ROOT / "main/media/playback_resume.cc"),
            "-o", str(cls.executable)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=45, stdin=subprocess.DEVNULL)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def scenario(self, mode):
        result = subprocess.run([str(self.executable), mode], capture_output=True, text=True,
                                timeout=10, stdin=subprocess.DEVNULL)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(mode + " PASS", result.stdout)

    def test_serialized_restart_capacity_integrity_and_failed_write(self):
        self.scenario("store")

    def test_real_resume_policy_rewind_and_session_start(self):
        self.scenario("session")

    def test_nvs_exact_calls_failures_close_and_no_erase(self):
        self.scenario("nvs")


if __name__ == "__main__":
    unittest.main()
