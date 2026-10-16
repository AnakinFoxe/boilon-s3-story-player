"""P5-8: the player builds the catalog's three copies in PSRAM.

Heap placement cannot be measured on the host, so this checks the production
wiring: two short windows (the scan, the resume worker's copies) lower the
malloc internal limit and restore it before anything else runs. Since
2026-10-15 the AI variant also restarts the minimum-free count at app_main, so
its "minimal sram" log line includes ESP-IDF 6.1's DMA pool again.
"""

import json
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/boilon-s3-gen2"
ON, OFF = "    CatalogInPsram(true);\n", "    CatalogInPsram(false);\n"
SCAN = '    auto result = boilon::local_library::Scan("/sdcard", library);\n'
WORKERS = {
    "m3_ai_main.cc": "    static boilon::local_library::ResumeWorker resume_worker("
                     "catalog, mounted_library, &Battery());\n",
}


class CatalogPsramTests(unittest.TestCase):
    def test_the_player_wraps_only_the_scan_and_the_copies(self):
        for name, worker in WORKERS.items():
            with self.subTest(variant=name):
                source = (BOARD / name).read_text(encoding="utf-8")
                helper = source.split("void CatalogInPsram(bool on) {", 1)[1].split("\n}\n", 1)[0]
                self.assertIn("#if CONFIG_SPIRAM_USE_MALLOC", helper)
                self.assertIn("heap_caps_malloc_extmem_enable(on ? 0 : "
                              "CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);", helper)
                self.assertEqual(source.count(ON), 2)
                self.assertEqual(source.count(OFF), 2)
                self.assertIn(ON + SCAN + OFF, source)
                self.assertIn(ON + worker + OFF, source)
                # Restored before the empty-card and error pages after the scan,
                # and before the worker's task and the player start.
                scan_end = source.index(SCAN + OFF) + len(SCAN + OFF)
                self.assertLess(scan_end, source.index("ScanError::kEmpty", scan_end))
                worker_end = source.index(worker + OFF) + len(worker + OFF)
                self.assertLess(worker_end, source.index("resume_worker.Start()", worker_end))
                self.assertLess(worker_end, source.index(
                    "boilon::local_library::Runtime<LocalMp3Player> runtime(", worker_end))

    def test_the_restored_limit_is_the_configured_one(self):
        config = json.loads((BOARD / "config.json").read_text(encoding="utf-8"))
        for build in config["builds"]:
            with self.subTest(build=build["name"]):
                self.assertIn("CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=2048", build["sdkconfig_append"])

    def test_ai_variant_counts_the_dma_pool_in_minimal_sram(self):
        source = (BOARD / "m3_ai_main.cc").read_text(encoding="utf-8")
        main = source.split('extern "C" void app_main() {', 1)[1]
        call = "    heap_caps_monitor_local_minimum_free_size_start();\n"
        self.assertEqual(source.count(call), 1)
        # After the power latch and the amplifier, before anything allocates.
        self.assertLess(main.index('"PA_DIRECTION");'), main.index(call))
        self.assertLess(main.index(call), main.index("xTaskCreate("))


if __name__ == "__main__":
    unittest.main()
