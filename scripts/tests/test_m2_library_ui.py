"""Exercise board-local volume and LCD helpers using native host fixtures."""

import os
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class M2LibraryUiHostTests(unittest.TestCase):
    def production_method(self, filename, name):
        source = (ROOT / "main/boards/boilon-s3-gen2" / filename).read_text()
        start = re.search(r"^    (?:static )?(?:bool|void|int)\s+" + re.escape(name) + r"\(",
                          source, re.MULTILINE)
        self.assertIsNotNone(start, f"Missing production {filename}::{name}")
        opening = source.index("{", start.start())
        depth = 0
        # The selected methods have no braces inside string literals/comments.
        for position in range(opening, len(source)):
            depth += (source[position] == "{") - (source[position] == "}")
            if depth == 0:
                return source[start.start():position + 1]
        self.fail(f"Unclosed production {filename}::{name}")

    def compile_and_run(self, source):
        configured = os.environ.get("CXX")
        compiler = shlex.split(configured) if configured else [shutil.which("c++")]
        self.assertTrue(compiler and compiler[0], "A native C++ compiler is required")
        with tempfile.TemporaryDirectory(prefix="m2-library-ui-") as directory:
            executable = Path(directory) / "ui-test"
            compiled = subprocess.run(
                compiler + ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                            "-I", str(ROOT / "main"), "-x", "c++", "-",
                            "-o", str(executable)],
                input=source, capture_output=True, text=True,
                timeout=30, cwd=ROOT,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            checked = subprocess.run(
                [str(executable)], capture_output=True, text=True,
                stdin=subprocess.DEVNULL, timeout=10, cwd=directory,
            )
            self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)

    def test_volume_mapping_all_int16_samples(self):
        self.compile_and_run(r'''
#include "boards/boilon-s3-gen2/m2_local_policy.h"
#include <cassert>
#include <limits>

int main() {
    using boilon::local_player::VolumeSample;
    const auto minimum = std::numeric_limits<int16_t>::min();
    const auto maximum = std::numeric_limits<int16_t>::max();
    assert(VolumeSample(minimum, 0) == 0);
    assert(VolumeSample(maximum, 0) == 0);
    assert(VolumeSample(minimum, 100) == minimum);
    assert(VolumeSample(maximum, 100) == maximum);

    for (int value = minimum; value <= maximum; ++value) {
        const auto sample = static_cast<int16_t>(value);
        assert(VolumeSample(sample, 0) == 0);
        assert(VolumeSample(sample, -1) == 0);
        assert(VolumeSample(sample, std::numeric_limits<int>::min()) == 0);
        assert(VolumeSample(sample, 101) == sample);
        assert(VolumeSample(sample, 200) == sample);
        assert(VolumeSample(sample, std::numeric_limits<int>::max()) == sample);
        int previous_magnitude = 0;
        for (int volume = 0; volume <= 100; ++volume) {
            const int output = VolumeSample(sample, volume);
            assert(value < 0 ? output <= 0 && output >= value : output >= 0 && output <= value);
            const int magnitude = output < 0 ? -output : output;
            assert(magnitude >= previous_magnitude);
            previous_magnitude = magnitude;
        }
    }
}
''')

    def test_utf8_decoder_preserves_scalars_and_advances_on_invalid_bytes(self):
        self.compile_and_run(r'''
#include "boards/boilon-s3-gen2/m2_library_screen.h"
#include <cassert>
#include <vector>

int main() {
    using boilon::local_library::NextCodepoint;
    const std::string valid = u8"A¢中🙂";
    const std::vector<uint32_t> expected = {0x41, 0xa2, 0x4e2d, 0x1f642};
    size_t offset = 0;
    for (const auto codepoint : expected) {
        const auto before = offset;
        assert(NextCodepoint(valid, offset) == codepoint);
        assert(offset > before && offset <= valid.size());
    }
    assert(offset == valid.size());
    assert(NextCodepoint(valid, offset) == 0 && offset == valid.size());
    ++offset;
    assert(NextCodepoint(valid, offset) == 0 && offset == valid.size() + 1);
    offset = 0;
    assert(NextCodepoint("", offset) == 0 && offset == 0);

    const std::vector<std::string> malformed = {
        "\x80", "\xff", "\xc0\x80", "\xc1\xbf", "\xc2", "\xe2\x82",
        "\xe0\x80\x80", "\xed\xa0\x80", "\xf0\x80\x80\x80",
        "\xf4\x90\x80\x80", "\xf5\x80\x80\x80", "\xe2(\xa1",
    };
    for (const auto& text : malformed) {
        offset = 0;
        assert(NextCodepoint(text, offset) == '?' && offset == 1);
        while (offset < text.size()) {
            const auto before = offset;
            const auto codepoint = NextCodepoint(text, offset);
            assert(offset > before && offset <= text.size());
            assert(codepoint <= 0x10ffff && !(codepoint >= 0xd800 && codepoint <= 0xdfff));
        }
        assert(offset == text.size());
    }
    for (const auto& fixture : std::vector<std::pair<std::string, uint32_t>>{
             {"\xed\x9f\xbf", 0xd7ff}, {"\xee\x80\x80", 0xe000},
             {"\xf4\x8f\xbf\xbf", 0x10ffff}}) {
        offset = 0;
        assert(NextCodepoint(fixture.first, offset) == fixture.second);
        assert(offset == fixture.first.size());
    }
    const std::string nul("\0A", 2);
    offset = 0;
    assert(NextCodepoint(nul, offset) == 0 && offset == 1);
    assert(NextCodepoint(nul, offset) == 'A' && offset == 2);
}
''')

    def test_library_main_connects_short_volume_gestures_to_codec(self):
        source = (ROOT / "main/boards/boilon-s3-gen2/m2_library_main.cc").read_text()
        branch = re.search(
            r"if\s*\(gesture == Action::kVolumeUp \|\| gesture == Action::kVolumeDown\)\s*\{([^}]+)\}",
            source,
        )
        self.assertIsNotNone(branch, "Both short volume actions need a codec update")
        self.assertRegex(branch[1], r"const int volume\s*=\s*runtime\.View\(NowMs\(\)\)\.volume;")
        self.assertRegex(branch[1], r"codec\.SetOutputVolume\(volume\);")

    def test_production_codec_pcm_chunks_snapshot_volume_outside_send(self):
        source = r'''
#include "boards/boilon-s3-gen2/m2_local_policy.h"
#include <cassert>
#include <functional>
#include <limits>
#include <vector>

struct portMUX_TYPE { bool held = false; int enters = 0, exits = 0; };
void portENTER_CRITICAL(portMUX_TYPE* lock) {
    assert(!lock->held);
    lock->held = true;
    ++lock->enters;
}
void portEXIT_CRITICAL(portMUX_TYPE* lock) {
    assert(lock->held);
    lock->held = false;
    ++lock->exits;
}
struct CodecBase {
    virtual void SetOutputVolume(int) = 0;
    virtual int Write(const int16_t*, int) = 0;
};
namespace boilon::local_player {
class LocalCodec : public CodecBase {
public:
    void Send(const int16_t* data, size_t bytes) {
        assert(!volume_lock_.held && volume_lock_.enters == volume_lock_.exits);
        assert(bytes > 0 && bytes <= 640 && bytes % 4 == 0);
        blocks.emplace_back(data, data + bytes / sizeof(int16_t));
        if (on_send) on_send();
    }
    portMUX_TYPE volume_lock_;
    int volume_ = 10;
    std::vector<std::vector<int16_t>> blocks;
    std::function<void()> on_send;
'''
        source += self.production_method("m2_local_codec.h", "SetOutputVolume") + "\n"
        source += self.production_method("m2_local_codec.h", "Write") + "\n};\n}\n"
        source += r'''
void CheckBlocks(const boilon::local_player::LocalCodec& codec,
                  const std::vector<int16_t>& input, const std::vector<int>& volumes) {
    assert(codec.blocks.size() == 3 && volumes.size() == 3);
    assert(codec.blocks[0].size() == 320 && codec.blocks[1].size() == 320 &&
           codec.blocks[2].size() == 2);
    size_t offset = 0;
    for (size_t block = 0; block < codec.blocks.size(); ++block)
        for (size_t sample = 0; sample < codec.blocks[block].size() / 2; ++sample, ++offset) {
            const auto expected = boilon::local_player::VolumeSample(input[offset], volumes[block]);
            assert(codec.blocks[block][sample * 2] == expected);
            assert(codec.blocks[block][sample * 2 + 1] == expected);
        }
    assert(offset == input.size());
    assert(!codec.volume_lock_.held && codec.volume_lock_.enters == codec.volume_lock_.exits);
}

int main() {
    using boilon::local_player::LocalCodec;
    const int16_t pattern[] = {-32768, 32767, -320, 320, 0, -31, 31, -1, 1};
    std::vector<int16_t> input(321);
    for (size_t i = 0; i < input.size(); ++i) input[i] = pattern[i % 9];

    LocalCodec changing;
    assert(changing.volume_ == 10);
    changing.on_send = [&changing] {
        if (changing.blocks.size() == 1) changing.SetOutputVolume(100);
        if (changing.blocks.size() == 2) changing.SetOutputVolume(0);
    };
    assert(changing.Write(input.data(), 321) == 321);
    CheckBlocks(changing, input, {10, 100, 0});
    assert(changing.volume_ == 0 && changing.volume_lock_.enters == 5);

    for (const int requested : {std::numeric_limits<int>::min(), -1, 0, 5, 10, 100, 105, 200, 201,
                                std::numeric_limits<int>::max()}) {
        LocalCodec codec;
        codec.SetOutputVolume(requested);
        const int expected = requested < 0 ? 0 : requested > 100 ? 100 : requested;
        assert(codec.volume_ == expected);
        assert(codec.Write(input.data(), 321) == 321);
        CheckBlocks(codec, input, {expected, expected, expected});
        assert(codec.volume_lock_.enters == 4);
    }
    LocalCodec empty;
    assert(empty.Write(nullptr, 0) == 0 && empty.blocks.empty() && empty.volume_lock_.enters == 0);
}
'''
        self.compile_and_run(source)


if __name__ == "__main__":
    unittest.main()
