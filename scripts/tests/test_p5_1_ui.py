"""P5-1: the firmware's drawing code paints every page like the prototype.

docs/boilon/p5_1_prototype/cases.json lists the pages; reference.js renders
them with the prototype and stores the SHA-256 of each RGB565 frame in
reference.json. This test builds the same pages with the production headers
and must match every frame exactly. On a mismatch it writes the firmware's
PNGs to a temporary directory; `node reference.js --png <dir>` writes the
prototype's for comparison.
"""

import hashlib
import json
import os
import shlex
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PROTOTYPE = ROOT / "docs/boilon/p5_1_prototype"
WIDTH, HEIGHT = 240, 296
FRAME = WIDTH * HEIGHT * 2

PLAY = {"playing": "kPlaying", "paused": "kPaused", "locating": "kLocating",
        "switching": "kSwitching", "finished": "kFinished", "error": "kError"}
PAGE = {"player": "kPlayer", "directory": "kDirectory", "checking": "kChecking",
        "nocard": "kNoCard", "empty": "kEmpty", "error": "kCardError", "goodbye": "kGoodbye",
        "hotspot": "kHotspot", "activation": "kActivation", "ai": "kAi",
        "lowbattery": "kLowBattery", "updating": "kUpdating", "updated": "kUpdated"}
TOAST = {"ai_offline": "kAiOffline", "activated": "kActivated", "refused": "kHotspotRefused",
         "low_battery": "kLowBattery"}
STAGE = {"connecting": "kConnecting", "listening": "kListening", "thinking": "kThinking",
         "speaking": "kSpeaking", "returning": "kReturning"}
FACE = {"happy": "kHappy", "surprised": "kSurprised", "sad": "kSad", "sleepy": "kSleepy",
        "thinking": "kThinking"}
MODE = {"order": "kOrder", "loop": "kLoop", "single": "kSingle", "shuffle": "kShuffle"}


def cpp_string(text):
    return '"' + "".join(c if c.isascii() and c not in '"\\' else
                         "".join(f"\\x{b:02x}" for b in c.encode()) + '""' for c in text) + '"'


def model_code(case):
    lines = [f"m.page = Page::{PAGE[case['page']]};"]
    if "play" in case:
        lines.append(f"m.play = Play::{PLAY[case['play']]};")
    for key, field in (("title", "title"), ("collection", "collection"), ("hotspot", "hotspot"),
                       ("code", "code"), ("heard", "child"), ("said", "reply"), ("version", "version")):
        if key in case:
            lines.append(f"m.{field} = {cpp_string(case[key])};")
    for key, field in (("index", "index"), ("count", "count"), ("vol", "volume"),
                       ("bat", "battery"), ("selected", "highlighted"),
                       ("directory_left_ms", "directory_left_ms"),
                       ("hotspot_left_s", "hotspot_left_s"), ("seek", "seek_seconds"),
                       ("progress", "progress")):
        if key in case:
            lines.append(f"m.{field} = {case[key]};")
    if "pos" in case:
        lines.append(f"m.position_ms = {case['pos']}000;")
    if "dur" in case:
        lines.append(f"m.duration_ms = {case['dur']}000;")
    if "seek_target" in case:
        lines.append(f"m.seek_target_ms = {case['seek_target']}000;")
    lines.append(f"m.ai_ready = {'true' if case.get('ai_ready') else 'false'};")
    lines.append(f"m.show_volume = {'true' if case.get('overlay') == 'volume' else 'false'};")
    if case.get("overlay") != "seek":
        lines.append("m.seek_seconds = 0;")
    if "toast" in case:
        lines.append(f"m.toast = Toast::{TOAST[case['toast']]};")
    if "mode" in case:
        lines.append(f"m.mode = PlayMode::{MODE[case['mode']]};")
    for name, count in case.get("directories", []):
        lines.append(f"m.directories.push_back({{{cpp_string(name)}, {count}}});")
    if "ai" in case:
        lines.append(f"m.ai = AiStage::{STAGE[case['ai']]};")
        lines.append(f"m.interrupted = {'true' if case['interrupted'] else 'false'};")
        lines.append(f"m.no_story = {'true' if case.get('no_story') else 'false'};")
        lines.append(f"m.failed = {'true' if case.get('failed') else 'false'};")
        lines.append(f"m.subtitles = {'true' if case.get('subs') else 'false'};")
        if "face" in case:
            lines.append(f"m.face = Face::{FACE[case['face']]};")
    palette = "Palette::kPaper" if case["palette"] == "paper" else "Palette::kNight"
    layout = "Layout::kCat" if case["layout"] == "cat" else "Layout::kCard"
    return ("    {\n        Model m;\n        " + "\n        ".join(lines) +
            f"\n        Emit(m, {palette}, {layout}, {case['t']!r});\n    }}\n")


def png(frame, path):
    rows = bytearray()
    for y in range(HEIGHT):
        rows.append(0)
        for x in range(WIDTH):
            (p,) = struct.unpack_from("<H", frame, (y * WIDTH + x) * 2)
            rows += bytes((round((p >> 11) * 255 / 31), round(((p >> 5) & 63) * 255 / 63),
                           round((p & 31) * 255 / 31)))

    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data +
                struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff))
    path.write_bytes(b"\x89PNG\r\n\x1a\n" +
                     chunk(b"IHDR", struct.pack(">IIBBBBB", WIDTH, HEIGHT, 8, 2, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(bytes(rows))) + chunk(b"IEND", b""))


class P51UiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        configured = os.environ.get("CXX")
        cls.compiler = shlex.split(configured) if configured else [shutil.which("c++")]
        if not cls.compiler or not cls.compiler[0]:
            raise AssertionError("A native C++ compiler is required")
        cls.temporary = tempfile.TemporaryDirectory(prefix="p5-1-ui-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.cases = json.loads((PROTOTYPE / "cases.json").read_text(encoding="utf-8"))

    def run_program(self, body, name, prelude=""):
        source = Path(self.temporary.name) / f"{name}.cc"
        source.write_text(r'''
#include "boards/boilon-s3-gen2/p5_1_ui_refresh.h"
#include <array>
#include <cassert>
#include <cstdio>
namespace font {
#include "boards/boilon-s3-gen2/m2_library_common_font.inc"
}
using namespace boilon::ui;
static uint8_t pixels[kWidth * kHeight];
static const CbinFont cbin(font::kCommonFont);
static const Fonts fonts{Font{&cbin}, Font{}};
// One frame as little-endian RGB565 through the chosen palette.
static void Emit(const Model& m, Palette palette, Layout layout, double t) {
    Canvas canvas(pixels);
    Draw(canvas, fonts, m, layout, t);
    const uint32_t* rgb = palette == Palette::kPaper ? kPaperRgb : kNightRgb;
    for (const uint8_t index : pixels) {
        assert(index < kColorCount);
        const uint16_t value = Rgb565(rgb[index]);
        const uint8_t bytes[2] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8)};
        std::fwrite(bytes, 1, 2, stdout);
    }
}
''' + prelude + r'''
int main() {
    assert(cbin.Valid());
''' + body + "    (void)Emit;\n    return 0;\n}\n", encoding="utf-8")
        executable = Path(self.temporary.name) / name
        compiled = subprocess.run(
            self.compiler + ["-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror", "-pedantic",
                             "-I", str(ROOT / "main"), str(source), "-o", str(executable)],
            capture_output=True, text=True, timeout=180, cwd=ROOT)
        self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
        result = subprocess.run([str(executable)], capture_output=True, timeout=60,
                                stdin=subprocess.DEVNULL)
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
        return result.stdout

    def test_every_page_matches_the_prototype_pixel_for_pixel(self):
        reference = json.loads((PROTOTYPE / "reference.json").read_text(encoding="utf-8"))
        self.assertEqual(sorted(reference), sorted(case["id"] for case in self.cases))
        output = self.run_program("".join(model_code(case) for case in self.cases), "pages")
        self.assertEqual(len(output), FRAME * len(self.cases))
        mismatched = []
        for index, case in enumerate(self.cases):
            frame = output[index * FRAME:(index + 1) * FRAME]
            if hashlib.sha256(frame).hexdigest() != reference[case["id"]]:
                mismatched.append(case["id"])
                png(frame, Path(self.temporary.name) / f"{case['id']}.png")
        if mismatched:
            keep = Path(tempfile.mkdtemp(prefix="p5-1-ui-mismatch-"))
            for name in mismatched:
                shutil.copy(Path(self.temporary.name) / f"{name}.png", keep)
            self.fail(f"{len(mismatched)} pages differ from the prototype: {mismatched}; "
                      f"firmware PNGs in {keep}")

    def test_missing_characters_draw_a_box_and_long_text_an_ellipsis(self):
        self.run_program(r'''
    Canvas canvas(pixels);
    TextStyle style;
    style.font = &fonts.noto;
    style.color = kText;
    canvas.Clear(kBg);
    // U+9955 is not in the 6845-character font: a 10x12 box at x+2, rows top+2..13.
    canvas.Text("\xe9\xa5\x95", 10, 10, style);
    for (int y = 0; y < 40; ++y)
        for (int x = 0; x < 40; ++x) {
            const bool box = x >= 12 && x < 22 && y >= 12 && y < 24 &&
                             (x == 12 || x == 21 || y == 12 || y == 23);
            assert((pixels[y * kWidth + x] == kText) == box);
        }
    style.max_width = 3 * 14;
    const auto lines = Wrap("\xe4\xb8\x80\xe4\xba\x8c\xe4\xb8\x89\xe5\x9b\x9b", style);
    assert(lines.size() == 1 && lines[0].size() == 3 && lines[0][2] == 0x2026);
    style.lines = 2;
    assert(Wrap("\xe4\xb8\x80\xe4\xba\x8c\xe4\xb8\x89\xe5\x9b\x9b", style).size() == 2);
''', "glyphs")

    def test_titles_shrink_only_as_far_as_needed(self):
        def fit(name, text):
            return f"    const title::Fit {name} = title::FitTitle(fonts.noto, {cpp_string(text)});\n"
        self.run_program(
            fit("brief", "第2集：小猫寻找月亮") +
            fit("longer", "第10集：穿过星光闪烁的森林寻找失踪的朋友") +
            fit("orphan", "第23回：孙悟空三打白骨精，唐僧误会把他赶走了") +
            fit("comma", "十万个为什么：为什么天空是蓝色的，而傍晚的晚霞却是红色的呢") +
            fit("english", "The Little Red Hen S01E05 - Who Will Bake the Bread") +
            fit("longest", "【睡前故事】小熊阿布和他的好朋友们：大森林里的春天、夏天、秋天和冬天，"
                "一年四季都有好玩的事情发生，小猪、小鹿和小狐狸都来了，大家一起过了快乐的一年") + r'''
    // Sizes scale every 14 px measure, rounding down below zero as well.
    assert(title::Scale(14, 21) == 21 && title::Scale(8, 23) == 13);
    assert(title::Scale(-1, 28) == -2 && title::Scale(-1, 21) == -1);
    assert(title::Lines(28) == 3 && title::Lines(24) == 3 && title::Lines(23) == 4);
    assert(title::Lines(19) == 4 && title::Lines(18) == 5);
    // The biggest size that shows everything; 28 px is the old 2x.
    assert(brief.size == 28 && brief.lines.size() == 2);
    assert(longer.size == 26 && longer.lines.size() == 3);
    assert(comma.size == 22 && comma.lines.size() == 4);
    for (const auto& line : comma.lines)
        assert(line.front() != 0xff0c);  // no line starts with a comma
    assert(orphan.size == 23 && orphan.lines.size() == 4 && orphan.lines.back().size() == 2);
    // Every English line break took the place of a space: no word is cut.
    assert(english.size == 23 && english.lines.size() == 4);
    std::vector<uint32_t> joined;
    for (const auto& line : english.lines) {
        assert(line.front() != ' ' && line.back() != ' ');
        if (!joined.empty())
            joined.push_back(' ');
        joined.insert(joined.end(), line.begin(), line.end());
    }
    assert(joined == Codepoints("The Little Red Hen S01E05 - Who Will Bake the Bread"));
    // Past 18 px: five lines and an ellipsis, no comma before it.
    assert(longest.size == 18 && longest.lines.size() == 5);
    const auto& last = longest.lines.back();
    assert(last.back() == 0x2026 && last[last.size() - 2] != 0xff0c);
    // Doubling every pixel or none; otherwise each boundary stays within 1 px.
    int cost[14] = {}, start[15];
    title::Stretch(cost, 14, 28, -1, start);
    for (int c = 0; c <= 14; ++c)
        assert(start[c] == 2 * c);
    title::Stretch(cost, 14, 14, -1, start);
    for (int c = 0; c <= 14; ++c)
        assert(start[c] == c);
    cost[3] = cost[7] = 5;
    title::Stretch(cost, 14, 21, 9, start);
    assert(start[0] == 0 && start[14] == 21 && start[4] - start[3] == 1 && start[8] - start[7] == 1);
    assert(2 * std::abs(14 * start[9] - 9 * 21) <= 14);
    for (int c = 0; c < 14; ++c)
        assert(start[c + 1] - start[c] >= 1 && start[c + 1] - start[c] <= 2 &&
               std::abs(14 * start[c] - c * 21) < 14);
    // At 28 px every glyph is the old 2x drawing, the missing-character box too.
    static uint8_t old_pixels[kWidth * kHeight];
    Canvas fitted(pixels), old(old_pixels);
    TextStyle style;
    style.font = &fonts.noto;
    style.color = kText;
    style.scale = 2;
    for (uint32_t c = 0x4e00; c < 0x5200; ++c) {
        fitted.Clear(kBg);
        old.Clear(kBg);
        title::DrawGlyph(fitted, fonts.noto, Lookup(fonts.noto, c), 100, 100, 28, kText);
        const char utf8[] = {static_cast<char>(0xe0 | (c >> 12)),
                             static_cast<char>(0x80 | ((c >> 6) & 63)),
                             static_cast<char>(0x80 | (c & 63)), 0};
        old.Text(utf8, 100, 100, style);
        assert(std::memcmp(pixels, old_pixels, sizeof(old_pixels)) == 0);
    }
''', "title", prelude="#include <cstdlib>\n#include <cstring>\n")

    def test_refresh_sends_only_what_changed_one_strip_at_a_time(self):
        self.run_program(r'''
    FakePanel panel;
    Refresh<FakePanel> refresh(panel, fonts);
    static uint8_t front[kWidth * kHeight], back[kWidth * kHeight];
    static uint16_t strip[Refresh<FakePanel>::kStripPixels];
    refresh.Attach(front, back, strip);
    Model m;
    m.title = "\xe5\xb0\x8f\xe7\x8c\xab";
    m.collection = "A";
    m.index = 2;
    m.count = 24;
    m.position_ms = 402000;
    m.duration_ms = 1085000;
    m.volume = 60;
    m.battery = 87;
    uint64_t now = 0;
    // The first page goes out whole, in 37 full-width strips, then the light.
    assert(refresh.Ready(now));
    refresh.Show(m, Palette::kPaper, Layout::kCard, now);
    now = Pump(refresh, panel, now);
    assert(panel.sends == 37 && panel.lit && panel.Matches(m, Palette::kPaper, Layout::kCard));
    // An unchanged page sends nothing, and the next page waits for the frame time.
    now += 125;
    panel.sends = 0;
    refresh.Show(m, Palette::kPaper, Layout::kCard, now);
    refresh.Poll(now);
    assert(panel.sends == 0 && !refresh.Ready(now + 124) && refresh.Ready(now + 125));
    now += 125;
    // A new volume touches only the time row: narrow strips at its rows.
    panel.windows.clear();
    m.volume = 65;
    now += 125;
    refresh.Show(m, Palette::kPaper, Layout::kCard, now);
    now = Pump(refresh, panel, now);
    assert(panel.sends > 0 && panel.sends <= 3 && panel.Matches(m, Palette::kPaper, Layout::kCard));
    for (const auto& w : panel.windows)
        assert(w[1] >= 236 && w[3] <= 252 && w[2] - w[0] < kWidth);
    // The cat's animation and the clock change in separate bands.
    m.play = Play::kPlaying;
    now += 125;
    refresh.Show(m, Palette::kPaper, Layout::kCat, now);
    now = Pump(refresh, panel, now);
    panel.windows.clear();
    panel.sends = 0;
    m.position_ms += 1000;
    now = 1500;  // floor(t * 2) % 2 flips: the cat moves 2 px.
    refresh.Show(m, Palette::kPaper, Layout::kCat, now);
    now = Pump(refresh, panel, now);
    assert(panel.Matches(m, Palette::kPaper, Layout::kCat, 1.5));
    bool cat = false, clock = false;
    for (const auto& w : panel.windows) {
        assert((w[2] - w[0]) * (w[3] - w[1]) <= Refresh<FakePanel>::kStripPixels);
        cat = cat || (w[1] < 140 && w[3] <= 140);
        clock = clock || w[1] >= 230;
    }
    assert(cat && clock);
    // Another palette: same indices, so everything goes again.
    now += 125;
    panel.sends = 0;
    const uint64_t shown = now;
    refresh.Show(m, Palette::kNight, Layout::kCat, now);
    now = Pump(refresh, panel, now);
    assert(panel.sends == 37 && panel.Matches(m, Palette::kNight, Layout::kCat, shown / 1000.0));
    // The AI page animates in the cat's rows only: here the ear starts to twitch.
    Model ai;
    ai.page = Page::kAi;
    ai.ai = AiStage::kListening;
    ai.battery = 87;
    const uint64_t twitch = (now / 2000 + 2) * 2000;  // t = 2k s: f8 % 16 == 0
    now = twitch - 1000;
    refresh.Show(ai, Palette::kNight, Layout::kCat, now);
    now = Pump(refresh, panel, now);
    panel.windows.clear();
    panel.sends = 0;
    now = twitch;
    refresh.Show(ai, Palette::kNight, Layout::kCat, now);
    now = Pump(refresh, panel, now);
    assert(panel.Matches(ai, Palette::kNight, Layout::kCat, twitch / 1000.0));
    assert(panel.sends > 0 && panel.sends < 37);
    for (const auto& w : panel.windows)
        assert(w[1] >= 40 && w[3] <= 40 + kCatHeight * 3);
    // A strip that never finishes stops the device after 500 ms; no second strip.
    m.volume = 70;
    now += 125;
    refresh.Show(m, Palette::kNight, Layout::kCat, now);
    refresh.Poll(now);
    const int sent = panel.sends;
    refresh.Poll(now + 499);
    assert(!panel.failed && panel.sends == sent);
    refresh.Poll(now + 500);
    assert(panel.failed && std::string(panel.failed) == "LCD_DMA_TIMEOUT" && panel.sends == sent);
''', "refresh", prelude=r'''
// The panel: one strip in flight at a time; Done() never blocks.
struct FakePanel {
    bool in_flight = false, finished = false, lit = false;
    int sends = 0;
    const char* failed = nullptr;
    std::vector<std::array<int, 4>> windows;
    uint16_t glass[kWidth * kHeight] = {};
    bool Done() {
        if (!finished)
            return false;
        finished = in_flight = false;
        return true;
    }
    void Send(int x0, int y0, int x1, int y1, const uint16_t* pixels) {
        assert(!in_flight && x0 >= 0 && y0 >= 0 && x1 <= kWidth && y1 <= kHeight);
        assert(x0 < x1 && y0 < y1 && (x1 - x0) * (y1 - y0) <= kWidth * 8);
        in_flight = true;
        ++sends;
        windows.push_back({x0, y0, x1, y1});
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x)
                glass[y * kWidth + x] = *pixels++;
    }
    void BacklightOn() {
        assert(!lit);
        lit = true;
    }
    void Fail(const char* reason) { failed = reason; }
    // What the glass shows equals the page drawn directly, high byte first.
    bool Matches(const Model& m, Palette palette, Layout layout, double t = 0) {
        static uint8_t expected[kWidth * kHeight];
        Canvas canvas(expected);
        Draw(canvas, fonts, m, layout, t);
        const uint32_t* rgb = palette == Palette::kPaper ? kPaperRgb : kNightRgb;
        for (int i = 0; i < kWidth * kHeight; ++i) {
            const uint16_t value = Rgb565(rgb[expected[i]]);
            if (glass[i] != static_cast<uint16_t>(value << 8 | value >> 8))
                return false;
        }
        return true;
    }
};
// The player loop: one Poll every 10 ms, the strip finishing in between.
static uint64_t Pump(Refresh<FakePanel>& refresh, FakePanel& panel, uint64_t now) {
    for (int i = 0; i < 1000 && !(refresh.Ready(now) && !panel.in_flight); ++i) {
        refresh.Poll(now);
        if (panel.in_flight)
            panel.finished = true;
        now += 10;
    }
    return now;
}
''')

    def test_player_view_notices_and_conversation_map_to_pages(self):
        self.run_program(r'''
    namespace library = boilon::local_library;
    namespace ai = boilon::ai;
    library::PlayerView view;
    view.title = "t.mp3";
    view.collection = "c";
    view.state = library::PlayerState::kLocating;
    view.track_number = 2;
    view.track_count = 24;
    view.duration_ms = 9000;
    view.volume = 35;
    view.volume_visible = true;
    view.seek_seconds = -60;
    view.offline_hint = true;
    Model m = PlayerModel(view, 4000, 130);
    assert(m.page == Page::kPlayer && m.play == Play::kLocating && m.title == "t.mp3");
    assert(m.index == 2 && m.count == 24 && m.position_ms == 4000 && m.duration_ms == 9000);
    assert(m.volume == 35 && m.show_volume && m.seek_seconds == -60 && m.seek_target_ms == 4000);
    assert(m.toast == Toast::kAiOffline && m.battery == 100 && !m.ai_ready);
    assert(m.mode == PlayMode::kOrder);
    for (const auto mode : {media::PlayMode::kLoop, media::PlayMode::kSingle,
                            media::PlayMode::kShuffle}) {
        view.mode = mode;  // The directory's play mode, for the status bar.
        assert(static_cast<int>(PlayerModel(view, 0, 50).mode) == static_cast<int>(mode));
    }
    assert(PlayerModel(view, 0, -7).battery == -1);
    view.state = library::PlayerState::kFailed;
    assert(PlayerModel(view, 0, 50).play == Play::kError);
    view.directory_open = true;
    view.directories = {"a", "b"};
    view.directory_files = {4, 9};
    view.highlighted = 1;
    view.directory_left_ms = 3200;
    m = PlayerModel(view, 0, 50);
    assert(m.page == Page::kDirectory && m.directories.size() == 2 && m.directories[1].second == 9);
    assert(m.highlighted == 1 && m.directory_left_ms == 3200 && m.toast == Toast::kNone);
    // Network notices: two take over the page, two are short notices.
    using Kind = ai::Notice::Kind;
    m = Model();
    ai::ShowNotice(m, ai::Notice("配网", "x", Kind::kHotspot, "Boilon-7F3A", 599001));
    assert(m.page == Page::kHotspot && m.hotspot == "Boilon-7F3A" && m.hotspot_left_s == 600);
    m = Model();
    ai::ShowNotice(m, ai::Notice("123456", "x", Kind::kActivating, "123456"));
    assert(m.page == Page::kActivation && m.code == "123456");
    m = Model();
    ai::ShowNotice(m, ai::Notice("激活", "请稍候", Kind::kActivating));
    assert(m.page == Page::kActivation && m.code.empty() && m.prompt == "请稍候");
    m = Model();
    ai::ShowNotice(m, ai::Notice("配网", "现在不能配网", Kind::kRefused));
    assert(m.page == Page::kPlayer && m.toast == Toast::kHotspotRefused);
    m = Model();
    ai::ShowNotice(m, ai::Notice("已激活", "", Kind::kActivated));
    assert(m.page == Page::kPlayer && m.toast == Toast::kActivated);
    m = Model();
    ai::ShowNotice(m, std::nullopt);
    assert(m.page == Page::kPlayer && m.toast == Toast::kNone);
    // The conversation's stage and whether it interrupted the story.
    FakeChat port;
    ai::Session session(port);
    ai::ChatView chat;
    port.chat = ai::Chat::kListening;
    m = Model();
    ai::ShowConversation(m, session, true, false, chat, false);
    assert(m.page == Page::kAi && m.ai == AiStage::kListening && m.interrupted && !m.no_story);
    assert(!m.failed && !m.subtitles && m.face == Face::kDefault);
    // Thinking: still listening, but the child's words arrived this turn.
    chat.heard = true;
    chat.child = "c";
    chat.reply = "r";
    chat.face = Face::kSad;
    ai::ShowConversation(m, session, true, false, chat, true);
    assert(m.ai == AiStage::kThinking && m.subtitles && m.child == "c" && m.reply == "r");
    assert(m.face == Face::kSad);
    port.chat = ai::Chat::kSpeaking;
    ai::ShowConversation(m, session, false, true, chat, false);
    assert(m.ai == AiStage::kSpeaking && !m.interrupted && m.no_story && !m.subtitles);
    port.chat = ai::Chat::kQuiet;
    assert(session.Open(0));
    ai::ShowConversation(m, session, false, false, chat, false);
    assert(m.ai == AiStage::kConnecting && !m.failed);
    session.Close(ai::EndReason::kButton, 1);
    ai::ShowConversation(m, session, false, false, chat, false);
    assert(m.ai == AiStage::kReturning && !m.failed);
    // The error face: an error or a Wi-Fi drop, or a conversation that never started.
    chat.failed = true;
    ai::ShowConversation(m, session, false, false, chat, false);
    assert(m.failed);
    chat.failed = false;
    ai::Session silent(port);
    assert(silent.Open(0) && silent.Poll(0) == ai::Event::kChatRequested);
    assert(silent.Poll(ai::kStartMs) == ai::Event::kEnded);
    ai::ShowConversation(m, silent, false, false, chat, false);
    assert(m.ai == AiStage::kReturning && m.failed);
''', "mapping", prelude=r'''
#include "boards/boilon-s3-gen2/m3_ai_device.h"
#include "boards/boilon-s3-gen2/p5_1_ui_view.h"
struct FakeChat final : boilon::ai::SessionPort {
    boilon::ai::Chat chat = boilon::ai::Chat::kQuiet;
    boilon::ai::Chat App() override { return chat; }
    bool ParkStory(uint64_t) override { return true; }
    bool StoryReleased() override { return true; }
    bool StoryWaiting() override { return false; }
    bool ResumeStory(uint64_t) override { return true; }
    bool OutputIdle() override { return true; }
    void StartChat() override {}
    void CloseChat() override {}
    void DropAiAudio() override {}
};
''')

    def test_chat_screen_follows_turns_words_and_emotions(self):
        self.run_program(r'''
    namespace ai = boilon::ai;
    ai::ChatScreen chat;
    ai::ChatView v = chat.View();
    assert(!v.heard && v.child.empty() && v.reply.empty() && v.face == Face::kDefault && !v.failed);
    // One turn: the child's words (thinking), the emotion, then the reply.
    chat.Listening();
    chat.Heard("\xe4\xbd\xa0\xe5\xa5\xbd");
    chat.Emotion("laughing");
    v = chat.View();
    assert(v.heard && v.child == "\xe4\xbd\xa0\xe5\xa5\xbd" && v.face == Face::kHappy);
    chat.Speaking();
    chat.Said("hi");
    v = chat.View();
    assert(!v.heard && !v.child.empty() && v.reply == "hi" && v.face == Face::kHappy);
    // The next turn starts without words; upstream resets the emotion itself.
    chat.Listening();
    v = chat.View();
    assert(!v.heard && v.child.empty() && v.reply.empty());
    chat.Failed();
    assert(chat.View().failed);
    chat.Begin();  // a new conversation forgets everything
    v = chat.View();
    assert(!v.failed && !v.heard && v.face == Face::kDefault);
    assert(&ai::CurrentChat() == &ai::CurrentChat());
    // The emotion words of P5_1_PIXEL_UI.md; anything else is the default face.
    const std::pair<const char*, Face> words[] = {
        {"happy", Face::kHappy}, {"laughing", Face::kHappy}, {"funny", Face::kHappy},
        {"loving", Face::kHappy}, {"winking", Face::kHappy}, {"relaxed", Face::kHappy},
        {"delicious", Face::kHappy}, {"kissy", Face::kHappy}, {"confident", Face::kHappy},
        {"cool", Face::kHappy}, {"silly", Face::kHappy}, {"surprised", Face::kSurprised},
        {"shocked", Face::kSurprised}, {"sad", Face::kSad}, {"crying", Face::kSad},
        {"sleepy", Face::kSleepy}, {"thinking", Face::kThinking}, {"confused", Face::kThinking},
        {"neutral", Face::kDefault}, {"angry", Face::kDefault}, {"embarrassed", Face::kDefault},
        {"cancel", Face::kDefault}, {"Happy", Face::kDefault}, {"", Face::kDefault}};
    for (const auto& word : words) {
        chat.Emotion(word.first);
        assert(chat.View().face == word.second);
    }
    // Long words keep at most kMaxText bytes and never cut a character.
    std::string moon;
    for (int i = 0; i < 100; ++i)
        moon += "\xe6\x9c\x88";
    chat.Said(moon.c_str());
    assert(chat.View().reply == moon.substr(0, 240) && ai::ChatScreen::kMaxText == 240);
    chat.Heard(("a" + moon).c_str());
    assert(chat.View().child == ("a" + moon).substr(0, 238));
''', "chat", prelude=r'''
#include "boards/boilon-s3-gen2/m3_ai_device.h"
''')

    def test_style_switch_accepts_known_values_and_keeps_others(self):
        self.run_program(r'''
    Style style;
    assert(style.palette == kDefaultPalette && style.layout == kDefaultLayout);
    assert(style.subtitles == kDefaultSubtitles && !kDefaultSubtitles);
    assert(ApplyStyle(style, "night", ""));
    assert(style.palette == Palette::kNight && style.layout == kDefaultLayout);
    assert(ApplyStyle(style, "", "cat"));
    assert(style.palette == Palette::kNight && style.layout == Layout::kCat);
    assert(!ApplyStyle(style, "dark", "card") && style.layout == Layout::kCat);
    assert(!ApplyStyle(style, "paper", "list") && style.palette == Palette::kNight);
    assert(ApplyStyle(style, "", "", "on") && style.subtitles && style.layout == Layout::kCat);
    assert(!ApplyStyle(style, "", "", "yes") && style.subtitles);
    assert(!ApplyStyle(style, "day", "", "off") && style.subtitles);
    assert(ApplyStyle(style, "paper", "card"));
    assert(DescribeStyle(style) == R"({"palette":"paper","layout":"card","subtitles":"on"})");
    assert(ApplyStyle(style, "", "", "off") && !style.subtitles);
    assert(kDefaultPalette == Palette::kPaper && kDefaultLayout == Layout::kCard);
''', "style")


if __name__ == "__main__":
    unittest.main()
