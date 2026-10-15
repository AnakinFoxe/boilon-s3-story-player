"""Run the WAV header parser (main/media/wav_format.h) on hostile headers.

The player test plays real WAV files through it; this one feeds the headers a
card could hold by mistake or damage: wrong sizes, chunks past the end, junk.
"""

import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]

SOURCE = r'''
#include "media/wav_format.h"
#include <cassert>
#include <cstring>
#include <vector>

struct Bytes {
    std::vector<uint8_t> b;
    void Put(uint64_t value, int width) {
        for (int i = 0; i < width; ++i) b.push_back(static_cast<uint8_t>(value >> (8 * i)));
    }
    void Tag(const char* id) { b.insert(b.end(), id, id + 4); }
    bool Parse(media::WavFormat& out, uint64_t size = 0) const {
        const uint64_t file = size ? size : b.size();
        return media::ParseWav(file, [&](uint64_t at, uint8_t* to, size_t count) {
            if (at > b.size() || count > b.size() - at) return false;
            std::memcpy(to, b.data() + at, count);
            return true;
        }, out);
    }
};

// RIFF/WAVE with a 16-byte fmt chunk and a data chunk claiming `claimed` bytes.
Bytes Wav(uint16_t channels, uint32_t rate, uint16_t bits, uint32_t claimed, size_t held,
          uint16_t block = 0) {
    Bytes w;
    w.Tag("RIFF"); w.Put(0, 4); w.Tag("WAVE");
    w.Tag("fmt "); w.Put(16, 4); w.Put(1, 2); w.Put(channels, 2); w.Put(rate, 4);
    w.Put(rate * channels * bits / 8, 4); w.Put(block ? block : channels * bits / 8, 2);
    w.Put(bits, 2);
    w.Tag("data"); w.Put(claimed, 4);
    w.b.insert(w.b.end(), held, 0);
    return w;
}

int main() {
    media::WavFormat f;
    assert(Wav(2, 44100, 16, 44100 * 4, 44100 * 4).Parse(f));
    assert(f.rate == 44100 && f.channels == 2 && f.bits == 16 && !f.is_float);
    assert(f.data_offset == 44 && f.Frames() == 44100 && f.DurationMs() == 1000);
    // Half a frame at the end is not a frame; a claim past the end is cut back.
    assert(Wav(2, 8000, 16, 4003, 4003).Parse(f) && f.data_bytes == 4000);
    assert(Wav(1, 8000, 16, 0xffffffff, 800).Parse(f) && f.Frames() == 400);
    // Wrong or impossible sizes.
    assert(!Wav(2, 8000, 16, 0, 0).Parse(f));                    // No frames.
    assert(!Wav(2, 8000, 16, 1, 1).Parse(f));                    // Less than a frame.
    assert(!Wav(2, 8000, 16, 400, 400, 2).Parse(f));             // Block align lies.
    assert(!Wav(1, 7999, 16, 400, 400).Parse(f));                // Rate out of range.
    assert(!Wav(1, 8000, 20, 400, 400).Parse(f));                // No 20-bit PCM.
    Bytes junk = Wav(1, 8000, 16, 400, 400);
    junk.b[0] = 'R'; junk.b[1] = 'I'; junk.b[2] = 'F'; junk.b[3] = 'X';
    assert(!junk.Parse(f));
    Bytes cut = Wav(1, 8000, 16, 400, 400);
    assert(!cut.Parse(f, 30));                                    // fmt runs past the file.
    Bytes runaway;
    runaway.Tag("RIFF"); runaway.Put(0, 4); runaway.Tag("WAVE");
    runaway.Tag("LIST"); runaway.Put(0xfffffff0, 4);              // A chunk past the end.
    assert(!runaway.Parse(f));
    Bytes short_fmt;
    short_fmt.Tag("RIFF"); short_fmt.Put(0, 4); short_fmt.Tag("WAVE");
    short_fmt.Tag("fmt "); short_fmt.Put(14, 4); short_fmt.b.insert(short_fmt.b.end(), 14, 0);
    assert(!short_fmt.Parse(f));
    Bytes no_data;
    no_data.Tag("RIFF"); no_data.Put(0, 4); no_data.Tag("WAVE");
    for (int i = 0; i < 70; ++i) { no_data.Tag("junk"); no_data.Put(0, 4); }
    assert(!no_data.Parse(f));                                    // Bounded chunk walk.
    assert(!Bytes{}.Parse(f));
    // At most 24 hours, like an MP3. Only headers are read, so a large file size
    // stands in for 8 kHz mono 8-bit data that is never built.
    assert(Wav(1, 8000, 8, 8000 * 86399, 0).Parse(f, 44 + 8000ULL * 86399));
    assert(f.DurationMs() == 86399000);
    assert(!Wav(1, 8000, 8, 8000 * 86401, 0).Parse(f, 44 + 8000ULL * 86401));
    assert(media::IsWavName("a.wav") && media::IsWavName("A.WAV") && media::IsWavName("x.WaV"));
    assert(!media::IsWavName(".wav") && !media::IsWavName("a.wave") && !media::IsWavName("a.mp3") &&
           !media::IsWavName("wav"));
}
'''


class WavFormatHostTests(unittest.TestCase):
    def test_hostile_headers(self):
        compiler = shlex.split(os.environ["CXX"]) if "CXX" in os.environ else [shutil.which("c++")]
        self.assertTrue(compiler[0], "Native C++ compiler required; do not silently skip")
        with tempfile.TemporaryDirectory(prefix="wav-format-") as temporary:
            executable = Path(temporary) / "wav-format-test"
            compiled = subprocess.run(
                compiler + ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                            "-I", str(ROOT / "main"), "-x", "c++", "-", "-o", str(executable)],
                input=SOURCE, text=True, capture_output=True, timeout=60, cwd=ROOT)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            tested = subprocess.run([str(executable)], text=True, capture_output=True,
                                    timeout=10, cwd=temporary)
            self.assertEqual(tested.returncode, 0, tested.stdout + tested.stderr)


if __name__ == "__main__":
    unittest.main()
