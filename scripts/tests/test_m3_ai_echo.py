"""P5-3 echo cancellation and barge-in rules (m3_ai_echo.h) and their wiring.

The rules run unchanged on synthetic TDM blocks: the boot self-check that
finds the playback reference slot, the reference gain, the [voice, reference]
pairs, the saved switch and when upstream's mode may change. This proves the
rules, not which slot the real board wires back, nor how the echo canceller or
xiaozhi.me behave; those need the device.
"""

import json
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/boilon-s3-gen2"

PRELUDE = r'''
#include "m3_ai_echo.h"
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>
using namespace boilon::ai::echo;
[[maybe_unused]] constexpr double kPi = 3.14159265358979323846;
inline bool Near(float actual, float expected, float tolerance = 0.3f) {
    return std::fabs(actual - expected) <= tolerance;
}
// One 10 ms block of 4-slot frames from per-slot sample functions.
template <class F>
std::vector<int16_t> Block(F sample) {
    std::vector<int16_t> tdm(kBlockFrames * kSlots);
    for (int i = 0; i < kBlockFrames; ++i)
        for (int slot = 0; slot < kSlots; ++slot)
            tdm[i * kSlots + slot] = sample(slot, i);
    return tdm;
}
inline SlotLevel Pattern(float on, float off, int lag) {
    float blocks[kProbeBlocks];
    for (int b = 0; b < kProbeBlocks; ++b)
        blocks[b] = b >= lag && ToneOn(b - lag) ? on : off;
    return Levels(blocks);
}
int main() {
'''


class M3AiEchoTests(unittest.TestCase):
    def run_cpp(self, body):
        compiler = shlex.split(os.environ["CXX"]) if "CXX" in os.environ else [shutil.which("c++")]
        self.assertTrue(compiler and compiler[0], "An existing native C++ compiler is required")
        with tempfile.TemporaryDirectory(prefix="m3-ai-echo-") as directory:
            executable = Path(directory) / "echo-test"
            result = subprocess.run(
                compiler + ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                            "-I", str(BOARD), "-x", "c++", "-", "-o", str(executable)],
                input=PRELUDE + body + "\n}\n", capture_output=True, text=True, timeout=30,
                cwd=ROOT,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], stdin=subprocess.DEVNULL,
                                    capture_output=True, text=True, timeout=10, cwd=directory)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_tone_is_minus_20_dbfs_and_measured_only_at_1_khz(self):
        self.run_cpp(r'''
    const auto tone = Block([](int slot, int i) { return slot == 1 ? ToneSample(i) : 0; });
    assert(Near(ToneDb(tone.data(), 1), kToneDbfs));
    assert(ToneDb(tone.data(), 0) < -109);  // Silence reads as the -110 dB floor.
    const auto full = Block([](int, int i) {
        return static_cast<int16_t>(std::lround(32767 * std::sin(2 * kPi * i / 16)));
    });
    assert(Near(ToneDb(full.data(), 2), 0));
    // 2 kHz (exactly twenty periods) and a DC offset fall outside the 1 kHz bin.
    const auto other = Block([](int slot, int i) {
        return static_cast<int16_t>(slot == 0 ? std::lround(30000 * std::sin(2 * kPi * i / 8))
                                              : 1000);
    });
    assert(ToneDb(other.data(), 0) < -80 && ToneDb(other.data(), 3) < -80);
    // Ten whole periods per block, so the tone continues across blocks.
    for (int n = 0; n < 16; ++n)
        assert(ToneSample(n + kBlockFrames) == ToneSample(n));
    // Silence first, then two bursts; the last blocks are silent again.
    int on = 0;
    for (int b = 0; b < kProbeBlocks; ++b)
        on += ToneOn(b);
    assert(on == 12 && !ToneOn(0) && !ToneOn(7) && ToneOn(8) && !ToneOn(kProbeBlocks - 1));
''')

    def test_levels_ignore_the_lag_and_the_power_up_transient(self):
        self.run_cpp(r'''
    for (int lag = 0; lag <= 10; ++lag) {
        const auto level = Pattern(-26, -95, lag);
        assert(Near(level.on, -26) && Near(level.off, -95));
    }
    // Four stale receive buffers and two blocks of ADC power-up transient,
    // all loud, change nothing.
    float blocks[kProbeBlocks];
    for (int b = 0; b < kProbeBlocks; ++b)
        blocks[b] = b < 6 ? -5 : (ToneOn(b - 8) && b >= 8 ? -30 : -90);
    const auto level = Levels(blocks);
    assert(Near(level.on, -30) && Near(level.off, -90));
    // A few clicks in a quiet slot are not a burst.
    for (int b = 0; b < kProbeBlocks; ++b)
        blocks[b] = b == 10 || b == 20 || b == 30 ? -10 : -95;
    assert(Near(Levels(blocks).on, -95));
''')

    def test_reference_is_exactly_one_of_slots_1_and_3_that_follows_the_tone(self):
        self.run_cpp(r'''
    const SlotLevel quiet{-95, -95}, follows{-26, -95};
    assert(PickReference({quiet, follows, quiet, quiet}) == 1);
    assert(PickReference({quiet, quiet, quiet, follows}) == 3);
    assert(PickReference({quiet, quiet, quiet, quiet}) == -1);
    // Two candidates is ambiguous; a voice slot that follows is not a candidate.
    assert(PickReference({quiet, follows, quiet, follows}) == -1);
    assert(PickReference({follows, quiet, follows, quiet}) == -1);
    assert(PickReference({follows, follows, follows, quiet}) == 1);
    // Crosstalk: too little contrast, or too weak to trust.
    assert(PickReference({quiet, SlotLevel{-75, -100}, quiet, quiet}) == -1);
    assert(PickReference({quiet, SlotLevel{-62, -110}, quiet, quiet}) == -1);
    assert(PickReference({quiet, SlotLevel{-50, -60}, quiet, quiet}) == -1);
    // The thresholds themselves pass.
    assert(PickReference({quiet, SlotLevel{-60, -90}, quiet, quiet}) == 1);
    // ES7210 gain masks use the MIC number: slots are MIC1, MIC3, MIC2, MIC4.
    assert(InputMask(0) == 1 && InputMask(1) == 4 && InputMask(2) == 2 && InputMask(3) == 8);
''')

    def test_reference_gain_keeps_full_scale_at_or_below_minus_6_dbfs(self):
        self.run_cpp(r'''
    assert(ReferenceGainDb(-26) == 0);   // Full scale would be -6 dBFS at 0 dB.
    assert(ReferenceGainDb(-20) == 0);   // Hotter than wanted: the lowest gain.
    assert(ReferenceGainDb(-35) == 9);
    assert(ReferenceGainDb(-34) == 6);   // Rounds down: never closer to clipping.
    assert(ReferenceGainDb(-50) == 24);
    assert(ReferenceGainDb(-60) == 30);  // At most 30 dB.
    for (float tone = -60; tone <= -20; tone += 0.5f) {
        const int gain = ReferenceGainDb(tone);
        assert(gain % 3 == 0 && gain >= 0 && gain <= 30);
        assert(gain == 0 || gain == 30 || tone + 20 + gain <= -6);
    }
''')

    def test_self_check_end_to_end_on_simulated_slots(self):
        self.run_cpp(r'''
    // The receiver lags the transmitter; the reference is the tone at -6 dB,
    // the microphones hear only room noise, and the first blocks are garbage.
    const auto run = [](int reference, int lag) {
        std::srand(7);
        float levels[kSlots][kProbeBlocks];
        for (int b = 0; b < kProbeBlocks; ++b) {
            const bool on = b >= lag && ToneOn(b - lag);
            const auto tdm = Block([&](int slot, int i) -> int16_t {
                const int noise = std::rand() % 41 - 20;
                if (b < 4)
                    return static_cast<int16_t>(std::rand() % 20001 - 10000);
                if (slot == reference && on)
                    return static_cast<int16_t>(ToneSample(i) / 2 + noise / 10);
                return static_cast<int16_t>(slot == 0 || slot == 2 ? noise : noise / 10);
            });
            for (int slot = 0; slot < kSlots; ++slot)
                levels[slot][b] = ToneDb(tdm.data(), slot);
        }
        SlotLevel slots[kSlots];
        for (int slot = 0; slot < kSlots; ++slot)
            slots[slot] = Levels(levels[slot]);
        const int found = PickReference(slots);
        assert(found < 0 || ReferenceGainDb(slots[found].on) == 0);
        return found;
    };
    for (int lag : {0, 4, 8}) {
        assert(run(1, lag) == 1);
        assert(run(3, lag) == 3);
        assert(run(-1, lag) == -1);
    }
''')

    def test_channels_are_the_voice_alone_or_voice_and_reference_pairs(self):
        self.run_cpp(r'''
    const auto tdm = Block([](int slot, int i) { return static_cast<int16_t>(i * 10 + slot); });
    int16_t mono[kBlockFrames], pairs[2 * kBlockFrames];
    TakeChannels(tdm.data(), kBlockFrames, -1, mono);
    for (int i = 0; i < kBlockFrames; ++i)
        assert(mono[i] == i * 10);
    for (int reference : {1, 3}) {
        TakeChannels(tdm.data(), kBlockFrames, reference, pairs);
        for (int i = 0; i < kBlockFrames; ++i)
            assert(pairs[2 * i] == i * 10 && pairs[2 * i + 1] == i * 10 + reference);
    }
''')

    def test_saved_switch_defaults_off_and_reads_only_0_or_1(self):
        self.run_cpp(r'''
    static_assert(!kDefaultBargeIn, "barge-in is off until the user asks");
    assert(!Decode(false, 0) && !Decode(false, 1));
    assert(Decode(true, 1) && !Decode(true, 0));
    assert(!Decode(true, 2) && !Decode(true, 255));
    assert(std::string(kNamespace) == "bl_ai" && std::string(kKey) == "barge_in");
''')

    def test_mode_changes_only_while_idle_and_only_when_the_setting_differs(self):
        self.run_cpp(r'''
    for (bool reference : {false, true})
        for (bool wanted : {false, true})
            for (bool aec_on : {false, true})
                assert(!ShouldSwitch(false, reference, wanted, aec_on));
    assert(ShouldSwitch(true, true, true, false));    // Turn on before the conversation.
    assert(!ShouldSwitch(true, true, true, true));    // Already on.
    assert(ShouldSwitch(true, true, false, true));    // Turned off by the user.
    assert(!ShouldSwitch(true, true, false, false));
    assert(!ShouldSwitch(true, false, true, false));  // No reference: stays off.
    assert(ShouldSwitch(true, false, true, true));
''')

    def test_production_wiring(self):
        codec = (BOARD / "m2_local_codec.h").read_text(encoding="utf-8")
        board = (BOARD / "m3_ai_board.cc").read_text(encoding="utf-8")
        main = (BOARD / "m3_ai_main.cc").read_text(encoding="utf-8")
        # The self-check is AI-only, runs after the microphone started, and
        # only a found reference turns the input into two channels.
        check = codec.split("void FindReference() {", 1)[1].split("\n    }\n#endif", 1)[0]
        self.assertIn("#if CONFIG_BOILON_S3_GEN2_AI\n    // P5-3", codec)
        self.assertIn("#if CONFIG_BOILON_S3_GEN2_AI\n            if (!failed)\n"
                      "                FindReference();", codec)
        self.assertIn("if (ReferenceReady()) {\n            reference_gain_db_", check)
        self.assertIn("input_channels_ = 2;\n            input_reference_ = true;", check)
        self.assertEqual(codec.count("input_channels_ = 2"), 1)
        # The switch: one board tool with a unique name, saved only to bl_ai.
        self.assertEqual(board.count('"self.story.set_barge_in"'), 1)
        self.assertNotIn("set_barge_in", (ROOT / "main/mcp_server.cc").read_text(encoding="utf-8"))
        self.assertIn("nvs_open(echo::kNamespace, NVS_READWRITE", board)
        self.assertEqual(board.count("NVS_READWRITE"), 2)  # The identity and this byte.
        apply = board.split("void ApplyBargeIn() {", 1)[1].split("\n}\n", 1)[0]
        self.assertIn("Application::GetInstance().Schedule(", apply)
        self.assertIn("echo::ShouldSwitch(app.GetDeviceState() == kDeviceStateIdle", apply)
        self.assertIn("app.SetAecMode(", apply)
        tool = board.split('"self.story.set_barge_in"', 1)[1].split("});", 1)[0]
        self.assertIn("if (enabled && !Codec().ReferenceReady())", tool)
        self.assertLess(tool.index("ReferenceReady()"), tool.index("SaveBargeIn(enabled)"))
        save = board.split("bool SaveBargeIn(bool enabled) {", 1)[1].split("\n}\n", 1)[0]
        self.assertLess(save.index("if (barge_in_byte == value)\n        return true;"),
                        save.index("nvs_open"))
        start = main.split("void StartChat() override {", 1)[1].split("}", 1)[0]
        self.assertLess(start.index("ApplyBargeIn()"), start.index("WakeWordInvoke"))
        # Runtime switching only: no build-time AEC option for either variant.
        config = json.loads((BOARD / "config.json").read_text(encoding="utf-8"))
        for variant in config["builds"]:
            for option in variant["sdkconfig_append"]:
                self.assertNotIn("_AEC", option)


if __name__ == "__main__":
    unittest.main()
