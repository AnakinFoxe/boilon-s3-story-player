import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/boilon/collect_licenses.py"
APACHE = "Apache License\nVersion 2.0\n"


def write(path: Path, text: str) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    return path


class BoilonCollectLicensesTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        base = Path(self.directory.name)
        idf, project, self.build = base / "idf", base / "project", base / "project/build"
        write(idf / "LICENSE", APACHE)
        write(idf / "docs/en/COPYRIGHT.rst", "FatFs is Copyright ChaN.\n")
        write(idf / "components/lwip/lwip/COPYING", "lwIP BSD text\n")
        write(idf / "components/fatfs/ff.c", "/* header-only license */\n")
        write(idf / "components/esp_wifi/lib/esp32s3/libnet80211.a", "")
        managed = project / "managed_components/espressif__thing"
        write(managed / "idf_component.yml", "version: 1.2.3\nlicense: Apache-2.0\n")
        write(managed / "LICENSE", APACHE)
        write(managed / "tests/LICENSE", "must be skipped\n")
        write(project / "managed_components/78__nolicense/idf_component.yml",
              "version: 0.4.0\nlicense: Apache-2.0\n")
        for name in ("LICENSE", "main/boards/boilon-s3-gen2/m2_library_common_font.SOURCE.txt",
                     "main/boards/boilon-s3-gen2/m2_library_glyphs.OFL.txt"):
            write(project / name, f"{name} text\n")
        write(self.build / "esp-idf/lwip/sub/liblwipextra.a", "")

        def component(directory, library):
            return {"dir": str(directory), "file": str(self.build / f"esp-idf/x/{library}")}

        description = {
            "project_name": "xiaozhi", "app_elf": "xiaozhi.elf", "idf_path": str(idf),
            "project_path": str(project), "version": "v6.0.2",
            "build_component_info": {
                "main": component(project / "main", "libmain.a"),
                "lwip": component(idf / "components/lwip", "liblwip.a"),
                "fatfs": component(idf / "components/fatfs", "libfatfs.a"),
                "esp_wifi": component(idf / "components/esp_wifi", "libesp_wifi.a"),
                "espressif__thing": component(managed, "libespressif__thing.a"),
                "78__nolicense": component(project / "managed_components/78__nolicense", "lib78__nolicense.a"),
                "unused": component(idf / "components/unused", "libunused.a"),
            },
        }
        write(self.build / "project_description.json", json.dumps(description))
        libraries = ["libmain.a", "liblwip.a", "liblwipextra.a", "libfatfs.a", "libnet80211.a",
                     "libespressif__thing.a", "libgcc.a", "libstdc++.a"]
        write(self.build / "xiaozhi.map", "".join(f"/x/{name}(obj.o)\n" for name in libraries))
        self.offline_map = write(base / "offline.map", "/x/lib78__nolicense.a(obj.o)\n")

    def run_script(self, *extra):
        output = Path(self.directory.name) / "licenses.txt"
        result = subprocess.run([sys.executable, "-B", str(SCRIPT), str(self.build), str(output), *extra],
                                capture_output=True, text=True)
        return result, output

    def test_collects_every_linked_component_once(self):
        result, output = self.run_script("--map", str(self.offline_map))
        self.assertEqual(result.returncode, 0, result.stderr)
        text = output.read_text()
        for expected in ("LICENSE text", "m2_library_glyphs.OFL.txt text", "FatFs is Copyright ChaN.",
                         "lwIP BSD text", "espressif__thing 1.2.3 (managed_components/espressif__thing)",
                         "78__nolicense 0.4.0", "Declared license: Apache-2.0",
                         "GCC Runtime Library Exception"):
            self.assertIn(expected, text)
        # The component's Apache text matches ESP-IDF's and is printed once.
        self.assertEqual(text.count("Version 2.0"), 1)
        self.assertIn("espressif__thing/LICENSE: same text as esp-idf/LICENSE above", text)
        # Prebuilt and in-tree libraries are traced to their components; unused ones are left out.
        self.assertIn("esp_wifi (esp-idf/components/esp_wifi)", text)
        self.assertIn("No separate license file", text)
        self.assertNotIn("unused", text)
        self.assertNotIn("must be skipped", text)

    def test_unknown_library_stops(self):
        with (self.build / "xiaozhi.map").open("a") as stream:
            stream.write("/x/libmystery.a(obj.o)\n")
        result, output = self.run_script()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("libmystery.a", result.stderr)
        self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
