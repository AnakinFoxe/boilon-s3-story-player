"""Compile unchanged AudioService lifecycle/output methods against SDK fakes.

No real SDK task, codec, card, network, or decoder is accessed. The normal-mode
task bodies are stubs: their production selection and initialization are tested.
"""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from scripts.tests.test_local_mp3_host import method, without_includes


ROOT = Path(__file__).resolve().parents[2]
AUDIO = ROOT / "main/audio"
FAKES = ROOT / "scripts/tests/fakes/playback_only"


class PlaybackOnlyHostTests(unittest.TestCase):
    def test_production_lifecycle_and_pcm(self):
        compiler = shutil.which("c++")
        self.assertTrue(compiler, "Native C++ compiler required; do not silently skip")
        production = (AUDIO / "audio_service.cc").read_text()
        selected = ["AudioService", "~AudioService", "Initialize", "InitializePlaybackOnly",
                    "CreateAudioPowerTimer", "Start", "Stop", "AudioOutputTask", "CheckAndUpdateAudioPowerState",
                    "BeginLocalPlayback", "SubmitLocalPcm", "EndLocalPlayback",
                    "IsLocalPlaybackActive", "IsPlaybackDrainedLocked", "MarkPlaybackDrainedLocked",
                    "ReadAudioData", "PushPacketToDecodeQueue", "EnableAudioTesting", "PlaySound"]
        # The production initializer uses two local preprocessor configuration helpers.
        macros = production[production.index("#define RATE_CVT_CFG"):production.index("#if CONFIG_IDF_TARGET")]
        source = (FAKES / "sdk.h").read_text().replace("#pragma once", "")
        source += "\n" + without_includes((AUDIO / "fixed_queue.h").read_text())
        source += "\n#define private public\n"
        source += without_includes((AUDIO / "audio_service.h").read_text())
        source += "\n#undef private\n" + macros
        source += "\n".join(method(production, name) for name in selected)
        source += """
void AudioService::AudioInputTask() { ++fake::input_runs; }
void AudioService::OpusCodecTask() { ++fake::opus_runs; }
void AudioService::PushTaskToEncodeQueue(AudioTaskType, std::vector<int16_t>&&) {}
"""
        source += (FAKES / "checks.cc").read_text()
        with tempfile.TemporaryDirectory(prefix="playback-only-host-") as temporary:
            executable = Path(temporary) / "playback-only-test"
            for audio_processor in (0, 1):
                with self.subTest(audio_processor=audio_processor):
                    compiled = subprocess.run(
                        [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
                         "-DCONFIG_IDF_TARGET_ESP32S3=1",
                         f"-DCONFIG_USE_AUDIO_PROCESSOR={audio_processor}",
                         "-x", "c++", "-", "-o", str(executable)],
                        input=source, text=True, capture_output=True, timeout=30, cwd=ROOT)
                    self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
                    tested = subprocess.run([str(executable)], text=True, capture_output=True,
                                            timeout=10, cwd=temporary)
                    self.assertEqual(tested.returncode, 0, tested.stdout + tested.stderr)


if __name__ == "__main__":
    unittest.main()
