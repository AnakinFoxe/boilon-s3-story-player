import hashlib
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/boilon/make_otadata.py"
# The otadata that selected ota_1 on the development device in 2026-08 and that
# every USB install there has written since.
DEVICE_SHA256 = "a1ed149677f974e7b1fd3b926c817a1db72ce01ca26b31929937d4ab01f5be22"


class BoilonOtadataTests(unittest.TestCase):
    def test_writes_the_image_that_boots_ota_1(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "otadata.bin"
            subprocess.run([sys.executable, "-B", str(SCRIPT), str(output)], check=True)
            data = output.read_bytes()
        self.assertEqual(len(data), 0x2000)
        self.assertEqual(hashlib.sha256(data).hexdigest(), DEVICE_SHA256)
        # The bootloader boots the higher valid sequence: 2 selects ota_1.
        self.assertEqual([int.from_bytes(data[o:o + 4], "little") for o in (0, 0x1000)], [1, 2])


if __name__ == "__main__":
    unittest.main()
