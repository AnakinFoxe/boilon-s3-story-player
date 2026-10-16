"""Actual board worker and file scan, using fresh synthetic files and closed SDK fakes."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class M2ResumeWorkerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ["CXX"]) if "CXX" in os.environ else [shutil.which("c++")]
        if not compiler or not compiler[0]:
            raise AssertionError("existing native compiler required; no download")
        cls.temporary = tempfile.TemporaryDirectory(prefix="m2-resume-worker-")
        cls.addClassCleanup(cls.temporary.cleanup)
        temporary = Path(cls.temporary.name)
        sdk = temporary / "sdk"
        headers = {
            "esp_log.h": "#pragma once\ntemplate <typename... Args> void FakeLog(Args&&...) {}\n#define ESP_LOGI(...) FakeLog(__VA_ARGS__)\n#define ESP_LOGW(...) FakeLog(__VA_ARGS__)\n",
            "esp_timer.h": "#pragma once\n#include <cstdint>\nint64_t esp_timer_get_time();\n",
            "nvs_flash.h": "#pragma once\nint nvs_flash_init();\nint nvs_flash_erase();\n",
            "freertos/FreeRTOS.h": "#pragma once\n#include <cstddef>\nusing QueueHandle_t = void*;\nconstexpr int pdPASS=1, pdTRUE=1;\nconstexpr unsigned portMAX_DELAY=~0U;\n",
            "freertos/queue.h": "#pragma once\n#include <freertos/FreeRTOS.h>\nQueueHandle_t xQueueCreate(unsigned, size_t);\nvoid vQueueDelete(QueueHandle_t);\nint xQueueSend(QueueHandle_t, const void*, unsigned);\nint xQueueReceive(QueueHandle_t, void*, unsigned);\n",
            "freertos/task.h": "#pragma once\n#include <freertos/FreeRTOS.h>\nint xTaskCreate(void (*)(void*), const char*, unsigned, void*, unsigned, void*);\nvoid vTaskDelay(unsigned);\n",
        }
        for name, content in headers.items():
            path = sdk / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
        cls.executable = temporary / "test"
        command = compiler + ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-I", str(sdk), "-I", str(ROOT / "scripts/tests/cpp/m2_resume_fakes"),
            "-I", str(ROOT / "main"), str(ROOT / "scripts/tests/cpp/m2_resume_worker_test.cc"),
            str(ROOT / "main/media/media_catalog.cc"), str(ROOT / "main/media/playback_resume.cc"),
            "-o", str(cls.executable)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=45, stdin=subprocess.DEVNULL)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        cls.portable = temporary / "portable"
        command = command[:-2] + ["-DM2_RESUME_FILE_ONLY", "-o", str(cls.portable)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=45, stdin=subprocess.DEVNULL)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def scenario(self, mode):
        with tempfile.TemporaryDirectory(prefix="synthetic-", dir=self.temporary.name) as root:
            result = subprocess.run([str(self.executable), mode, root], capture_output=True, text=True,
                                    timeout=10, stdin=subprocess.DEVNULL)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(mode + " PASS", result.stdout)

    def test_file_scan_stability_cache_budget_and_cancel(self):
        self.scenario("files")
        with tempfile.TemporaryDirectory(prefix="portable-", dir=self.temporary.name) as root:
            result = subprocess.run([str(self.portable), "files", root], capture_output=True, text=True,
                                    timeout=10, stdin=subprocess.DEVNULL)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("files PASS", result.stdout)

    def test_actual_worker_bounded_queue_snapshot_and_one_shot_restore(self):
        self.scenario("worker")

    def test_sdk_fd_mtime_is_unavailable_but_path_metadata_is_retained(self):
        self.scenario("sdkstat")
        with tempfile.TemporaryDirectory(prefix="portable-sdk-", dir=self.temporary.name) as root:
            result = subprocess.run([str(self.portable), "sdkstat", root], capture_output=True,
                                    text=True, timeout=10, stdin=subprocess.DEVNULL)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("sdkstat PASS", result.stdout)

    def test_duration_scan_diagnostics_preserve_all_failure_boundaries(self):
        self.scenario("diagnostics")

    def test_worker_init_task_queue_and_write_failures_never_erase_or_retry(self):
        self.scenario("failures")

    def test_pending_counts_inflight_until_commit_and_reports_real_save_status(self):
        self.scenario("completion")

    def test_boost_volume_changes_persist_restore_and_keep_legacy_values_without_erase(self):
        self.scenario("boost")

    def test_battery_reads_only_on_worker_leave_shutdown_space_and_fail_once(self):
        self.scenario("battery")

    def test_forget_runs_after_queued_saves_and_positions_wait_for_it(self):
        self.scenario("forget")

    def test_daily_default_on_main_construction_guard(self):
        source = (ROOT / "main/boards/boilon-s3-gen2/m3_ai_main.cc").read_text()
        self.assertIn("#ifndef BOILON_M2_RESUME\n#define BOILON_M2_RESUME 1\n#endif", source)
        guard = source.split("#if BOILON_M2_RESUME\n", 1)[1].split("#endif", 1)[0]
        self.assertIn("ResumeWorker resume_worker", guard)
        self.assertIn("resume_worker.Start()", guard)
        self.assertIn("persistence = &resume_worker", guard)
        self.assertEqual(source.count("ResumeWorker resume_worker"), 1)


if __name__ == "__main__":
    unittest.main()
