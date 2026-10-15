"""Production frame scanner: synthetic boundaries and fresh encoder-generated files."""

from fractions import Fraction
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class M2Mp3DurationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ["CXX"]) if "CXX" in os.environ else [shutil.which("c++")]
        if not compiler or not compiler[0]:
            raise AssertionError("existing native C++ compiler required; no download")
        cls.temporary = tempfile.TemporaryDirectory(prefix="m2-mp3-duration-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.executable = Path(cls.temporary.name) / "duration-test"
        command = compiler + ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                              "-I", str(ROOT / "main"),
                              str(ROOT / "scripts/tests/cpp/m2_mp3_duration_test.cc"),
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

    def test_cbr_mpeg1_2_and_2_5_exact_frame_counts(self):
        self.scenario("cbr")

    def test_vbr_mpeg1_2_and_2_5_with_padding(self):
        self.scenario("vbr")

    def test_id3v2_versions_footer_and_id3v1_tail(self):
        self.scenario("tags")

    def test_truncation_reserved_fields_format_changes_and_read_errors(self):
        self.scenario("invalid")

    def test_sixteen_frame_step_budget_and_terminal_cancel(self):
        self.scenario("budget")

    def test_fresh_real_cbr_vbr_matches_ffprobe_packet_time(self):
        ffmpeg, ffprobe = shutil.which("ffmpeg"), shutil.which("ffprobe")
        if not ffmpeg or not ffprobe:
            self.skipTest("existing ffmpeg/ffprobe unavailable; no download")
        with tempfile.TemporaryDirectory(prefix="m2-duration-encoder-") as directory:
            for rate in (44100, 22050, 11025):
                for variable in (False, True):
                    with self.subTest(rate=rate, variable=variable):
                        path = Path(directory) / f"{rate}-{'vbr' if variable else 'cbr'}.mp3"
                        command = [ffmpeg, "-nostdin", "-v", "error", "-f", "lavfi", "-i",
                                   f"sine=frequency=631:sample_rate={rate}:duration=0.4",
                                   "-ac", "2", "-c:a", "libmp3lame"]
                        command += ["-q:a", "4"] if variable else ["-b:a", "64k"]
                        # Disable the extra Xing frame so packet count is the scanner's frame count.
                        command += ["-write_xing", "0", "-id3v2_version", "3", str(path)]
                        encoded = subprocess.run(command, capture_output=True, text=True,
                                                 stdin=subprocess.DEVNULL, timeout=15)
                        self.assertEqual(encoded.returncode, 0, encoded.stderr)
                        probed = subprocess.run([ffprobe, "-v", "error", "-select_streams", "a:0",
                                                 "-show_packets", "-show_streams", "-show_entries",
                                                 "packet=duration:stream=sample_rate,time_base", "-of",
                                                 "json", str(path)], capture_output=True, text=True,
                                                stdin=subprocess.DEVNULL, timeout=15)
                        self.assertEqual(probed.returncode, 0, probed.stderr)
                        data = json.loads(probed.stdout)
                        stream = data["streams"][0]
                        self.assertEqual(int(stream["sample_rate"]), rate)
                        packet_time = sum(int(packet["duration"]) for packet in data["packets"])
                        expected = int(packet_time * Fraction(stream["time_base"]) * 1000)
                        samples_per_frame = 1152 if rate == 44100 else 576
                        self.assertEqual(expected, len(data["packets"]) * samples_per_frame * 1000 // rate)
                        scanned = subprocess.run([str(self.executable), "file", str(path)],
                                                 capture_output=True, text=True,
                                                 stdin=subprocess.DEVNULL, timeout=10)
                        self.assertEqual(scanned.returncode, 0, scanned.stdout + scanned.stderr)
                        self.assertEqual(int(scanned.stdout.strip()), expected)


if __name__ == "__main__":
    unittest.main()
