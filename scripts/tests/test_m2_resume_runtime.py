"""Real board runtime resume calls through an explicitly pumped synthetic port."""

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class M2ResumeRuntimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ["CXX"]) if "CXX" in os.environ else [shutil.which("c++")]
        if not compiler or not compiler[0]:
            raise AssertionError("existing native C++ compiler required; no download")
        cls.temporary = tempfile.TemporaryDirectory(prefix="m2-resume-runtime-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.executable = Path(cls.temporary.name) / "runtime-test"
        command = compiler + ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                              "-I", str(ROOT / "main"),
                              str(ROOT / "scripts/tests/cpp/m2_resume_runtime_test.cc"),
                              str(ROOT / "main/media/media_catalog.cc"),
                              str(ROOT / "main/media/player_controller.cc"),
                              str(ROOT / "main/media/player_gestures.cc"),
                              str(ROOT / "main/media/playback_resume.cc"),
                              "-o", str(cls.executable)]
        result = subprocess.run(command, capture_output=True, text=True,
                                stdin=subprocess.DEVNULL, timeout=30, cwd=ROOT)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def scenario(self, mode):
        result = subprocess.run([str(self.executable), mode], capture_output=True, text=True,
                                stdin=subprocess.DEVNULL, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(mode + " PASS", result.stdout)

    def test_runtime_reconstruction_rewinds_restores_volume_before_start_and_preserves_pre_output(self):
        self.scenario("boot")

    def test_periodic_thirty_seconds_and_best_effort_shutdown(self):
        self.scenario("periodic")

    def test_shutdown_snapshot_survives_stop_mutations_and_reconstruction_without_fake_success(self):
        self.scenario("shutdown")

    def test_switch_and_eof_checkpoint_old_active_identity_before_controls(self):
        self.scenario("switch")

    def test_async_collection_restore_exact_cursor_and_output_drain(self):
        self.scenario("collection")

    def test_failed_save_queue_rejection_and_unknown_duration_do_not_fail_playback(self):
        self.scenario("failure")

    def test_failed_files_after_sound_never_stop_and_keep_what_played(self):
        self.scenario("skip")

    def test_default_disabled_supports_legacy_player_without_resume_methods(self):
        self.scenario("default")

    def test_previous_goes_straight_back_chord_accumulation_clamping_and_paused_seek(self):
        self.scenario("controls")

    def test_ai_park_saves_heard_position_and_restarts_five_seconds_back(self):
        self.scenario("ai_playing")

    def test_ai_park_while_paused_stays_paused_and_restarts_in_place(self):
        self.scenario("ai_paused")

    def test_output_ended_by_another_owner_holds_position_instead_of_failing(self):
        self.scenario("released")

    def test_park_refused_while_locating_or_switching(self):
        self.scenario("park_busy")

    def test_ai_pick_replaces_the_parked_story(self):
        self.scenario("ai_choose")


if __name__ == "__main__":
    unittest.main()
