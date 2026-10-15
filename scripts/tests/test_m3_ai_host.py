"""Run unchanged AudioService methods in the M3 AI variant's full mode.

Application initializes the full voice pipeline; the board's offline player
then uses only the local PCM session API. SDK, codec and decoders are fakes:
this proves control flow, not audible playback or device memory use.
"""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from scripts.tests.test_local_mp3_host import method, without_includes


ROOT = Path(__file__).resolve().parents[2]
AUDIO = ROOT / "main/audio"
FAKES = ROOT / "scripts/tests/fakes"


class M3AiFullModeHostTests(unittest.TestCase):
    def test_full_mode_local_playback(self):
        compiler = shutil.which("c++")
        self.assertTrue(compiler, "Native C++ compiler required; do not silently skip")
        production = (AUDIO / "audio_service.cc").read_text()
        selected = ["AudioService", "~AudioService", "Initialize", "CreateAudioPowerTimer",
                    "Start", "Stop", "AudioOutputTask", "CheckAndUpdateAudioPowerState",
                    "BeginLocalPlayback", "SubmitLocalPcm", "PauseLocalPlayback",
                    "EndLocalPlayback", "IsLocalPlaybackActive", "LocalPlaybackSamples",
                    "IsPlaybackDrainedLocked", "MarkPlaybackDrainedLocked",
                    "PushPacketToDecodeQueue", "PlaySound"]
        # The production initializer uses two local preprocessor configuration helpers.
        macros = production[production.index("#define RATE_CVT_CFG"):production.index("#if CONFIG_IDF_TARGET")]
        source = (FAKES / "playback_only/sdk.h").read_text().replace("#pragma once", "")
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
        source += (FAKES / "m3_ai/checks.cc").read_text()
        with tempfile.TemporaryDirectory(prefix="m3-ai-host-") as temporary:
            executable = Path(temporary) / "m3-ai-test"
            # The AI variant enables the audio processor; see the build guard test.
            compiled = subprocess.run(
                [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
                 "-DCONFIG_IDF_TARGET_ESP32S3=1", "-DCONFIG_USE_AUDIO_PROCESSOR=1",
                 "-x", "c++", "-", "-o", str(executable)],
                input=source, text=True, capture_output=True, timeout=30, cwd=ROOT)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            tested = subprocess.run([str(executable)], text=True, capture_output=True,
                                    timeout=10, cwd=temporary)
            self.assertEqual(tested.returncode, 0, tested.stdout + tested.stderr)


if __name__ == "__main__":
    unittest.main()
