"""Run production MP3 control flow with deterministic host SDK fakes.

The ESP MP3 decoder is target-only: these tests prove worker/queue behavior,
not bitstream decoding quality or audible hardware playback.
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
AUDIO = ROOT / "main/audio"
FAKES = ROOT / "scripts/tests/fakes/local_mp3"


def without_includes(source):
    return re.sub(r'^\s*#include[^\n]*$', '', source, flags=re.MULTILINE)


def method(source, name):
    """Extract one complete, unchanged production method, including its body."""
    start = re.search(r'^[^\n;{}]*\bAudioService::' + re.escape(name) + r'\(',
                      source, flags=re.MULTILINE)
    if start is None:
        raise AssertionError(f"Missing production method: AudioService::{name}")
    opening = source.index('{', start.start())
    depth = 0
    # The selected methods do not contain braces in string literals/comments.
    for position in range(opening, len(source)):
        depth += (source[position] == '{') - (source[position] == '}')
        if depth == 0:
            return source[start.start():position + 1]
    raise AssertionError(f"Unclosed production method: {name}")


class LocalMp3HostTests(unittest.TestCase):
    def test_production_local_audio_control(self):
        compiler = shlex.split(os.environ["CXX"]) if "CXX" in os.environ else [shutil.which("c++")]
        self.assertTrue(compiler and compiler[0], "A native C++ compiler is required")
        service_source = (AUDIO / "audio_service.cc").read_text()
        selected = ["Stop", "ResetDecoder", "PushPacketToDecodeQueue", "IsPlaybackIdle",
                    "IsPlaybackDrainedLocked", "MarkPlaybackDrainedLocked", "AudioOutputTask",
                    "BeginLocalPlayback", "SubmitLocalPcm", "PauseLocalPlayback",
                    "EndLocalPlayback", "IsLocalPlaybackActive", "LocalOutputSampleRate", "LocalPlaybackSamples",
                    "EnableAudioTesting"]
        source = (FAKES / "sdk.h").read_text().replace('#pragma once', '')
        source += '\n#include "boards/boilon-s3-gen2/m2_resume_store.h"\n'
        source += '\n#include "boards/boilon-s3-gen2/m2_resume_session.h"\n'
        source += '\n#include "media/wav_format.h"\n'
        source += '\n' + without_includes((AUDIO / "fixed_queue.h").read_text())
        source += '\n#define private public\n'
        source += without_includes((AUDIO / "audio_service.h").read_text())
        source += '\n#undef private\n'
        source += '\nAudioService::AudioService() { event_group_ = nullptr; }\nAudioService::~AudioService() = default;\n'
        source += '\n'.join(method(service_source, name) for name in selected)
        source += '\n' + without_includes((AUDIO / "local_mp3_player.h").read_text()).replace('#pragma once', '')
        player_source = without_includes((AUDIO / "local_mp3_player.cc").read_text())
        # Redirect only the extracted worker's stdio calls. The wrappers still
        # use real host stdio, outside these macros, so they cannot recurse.
        stdio_calls = {"fread": "ReadFile", "fgetc": "GetFileChar",
                       "ferror": "FileError", "fclose": "CloseFile",
                       "fopen": "OpenFile", "fseek": "SeekFile", "ftell": "TellFile",
                       "setvbuf": "BufferFile"}
        for name, wrapper in stdio_calls.items():
            macro = "BOILON_TEST_" + name.upper()
            player_source = player_source.replace("std::" + name + "(", macro + "(")
            source += "\n#define " + macro + " fake::" + wrapper
        source += '\n' + player_source
        for name in stdio_calls:
            source += "\n#undef BOILON_TEST_" + name.upper()
        source += '\n' + (ROOT / "scripts/tests/cpp/local_mp3_test.cc").read_text()
        with tempfile.TemporaryDirectory(prefix="local-mp3-host-") as directory:
            executable = Path(directory) / "local-mp3-test"
            result = subprocess.run(
                compiler + ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
                 "-I", str(ROOT / "main"),
                 str(ROOT / "main/media/media_catalog.cc"), str(ROOT / "main/media/player_controller.cc"),
                 str(ROOT / "main/media/playback_resume.cc"),
                 "-x", "c++", "-", "-o", str(executable)], input=source,
                capture_output=True, text=True, timeout=30, cwd=ROOT)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable), str(Path(directory) / "synthetic.mp3")],
                                    capture_output=True, text=True,
                                    timeout=10, cwd=directory)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            print(result.stdout, end="")


if __name__ == "__main__":
    unittest.main()
