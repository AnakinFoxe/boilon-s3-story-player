"""Exercise actual LocalCodec startup against the installed codec_dev API.

Run after an IDF build installs managed components, including in the CI build job.
Hardware I2C/I2S and the ES8311 implementation are faked; this checks the adapter,
constructor precondition and failure path, not audible playback.
"""

import os
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CODEC = ROOT / "managed_components/espressif__esp_codec_dev"


class M2LocalCodecHostTests(unittest.TestCase):
    def test_actual_start_forwards_info_and_stops_before_pa_on_info_failure(self):
        self.assertTrue((CODEC / "interface/audio_codec_ctrl_if.h").is_file(),
                        "Build the board first to install the locked codec_dev headers")
        compiler = shlex.split(os.environ["CXX"]) if "CXX" in os.environ else [shutil.which("c++")]
        self.assertTrue(compiler and compiler[0], "A native C++ compiler is required")
        actual = (ROOT / "main/boards/boilon-s3-gen2/m2_local_codec.h").read_text()
        # Keep the entire production class; replace only SDK include resolution.
        actual = re.sub(r'^\s*#(?:include|pragma once)[^\n]*$', '', actual, flags=re.MULTILINE)
        source = (ROOT / "scripts/tests/fakes/m2_local_codec/sdk.h").read_text().replace("#pragma once", "")
        source += ('\n#include <memory>\n#include <new>\n'
                   '#include "boards/boilon-s3-gen2/m2_local_policy.h"\n'
                   '#include "boards/boilon-s3-gen2/m3_ai_echo.h"\n') + actual
        source += '\n' + (ROOT / "scripts/tests/cpp/m2_local_codec_test.cc").read_text()
        with tempfile.TemporaryDirectory(prefix="m2-local-codec-") as directory:
            path = Path(directory)
            (path / "esp_err.h").write_text("#pragma once\n#define ESP_CODEC_DEV_TEST_ERR 0x102\n#define ESP_ERR_INVALID_ARG ESP_CODEC_DEV_TEST_ERR\n")
            executable = path / "codec-test"
            compiled = subprocess.run(
                compiler + ["-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-DCONFIG_BOILON_S3_GEN2_AI=1",  # The microphone path.
                            "-I", str(path), "-I", str(ROOT / "main"),
                            "-I", str(CODEC / "include"), "-I", str(CODEC / "interface"),
                            "-I", str(CODEC / "device/include"),
                            "-x", "c++", "-", "-o", str(executable)],
                input=source, capture_output=True, text=True, timeout=30, cwd=ROOT,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            checked = subprocess.run([str(executable)], capture_output=True, text=True,
                                     stdin=subprocess.DEVNULL, timeout=10, cwd=directory)
            self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
            print(checked.stdout, end="")


if __name__ == "__main__":
    unittest.main()
