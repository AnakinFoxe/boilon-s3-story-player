"""Run the M7 SD-update rules (m7_sd_update.h) unchanged, and check where the AI variant calls them.

A newer AI release in the card's root is written into the other slot only when the
running firmware is confirmed, the battery is not low and the image did not fail to
start before; the image must carry its own release name and the AI variant tag.
"""

import re
import unittest

from scripts.tests.boilon_cpp_helpers import BOARD, compile_and_run

SOURCE = r'''
#include <cassert>
#include <string>
#include <vector>
#include "m6_release.h"
#include "m7_sd_update.h"
using namespace boilon::sd_update;

static bool Same(const std::optional<Version>& v, int a, int b, int c) {
    return v && v->major == a && v->minor == b && v->patch == c;
}

static void Versions() {
    assert(Same(ParseNumbers("1.2.3"), 1, 2, 3) && Same(ParseNumbers("0.0.0"), 0, 0, 0));
    assert(Same(ParseNumbers("10.20.999"), 10, 20, 999));
    for (const char* bad : {"", "1", "1.2", "1.2.3.4", "01.2.3", "1.02.3", "1000.0.0", "1.2.3x",
                            "a.b.c", "1..3", ".1.2", "1.2.", "-1.2.3", " 1.2.3", "1.2.3 "})
        assert(!ParseNumbers(bad));
    assert(FromReleaseName(boilon::release::kName));  // the version m6_release.h names
    // The boot page's version is the release name's version, "v1.2.3".
    const std::string name = boilon::release::kName, version = boilon::release::kVersion;
    assert(version[0] == 'v' && name == "boilon-ai-player " + version);
    assert(Same(FromReleaseName("boilon-ai-player v1.1.0"), 1, 1, 0));
    assert(!FromReleaseName("boilon-ai-player 1.1.0") && !FromReleaseName("xiaozhi v1.1.0"));
    assert(Same(FromFileName("boilon-ai-player-v1.1.1.bin"), 1, 1, 1));
    for (const char* other : {"boilon-ai-player-v1.1.1-offline.bin", "boilon-ai-player-v1.1.1.BIN",
                              "._boilon-ai-player-v1.1.1.bin", "boilon-ai-player-v1.1.1.bin.bin",
                              "boilon-ai-player v1.1.1.bin", "boilon-ai-player-v.bin", "xiaozhi.bin",
                              "boilon-ai-player-v1.1.1"})
        assert(!FromFileName(other));
    assert(ReleaseName(Version{1, 1, 1}) == "boilon-ai-player v1.1.1");
    assert(Newer({1, 1, 1}, {1, 1, 0}) && Newer({1, 2, 0}, {1, 1, 9}) && Newer({2, 0, 0}, {1, 9, 9}));
    assert(!Newer({1, 1, 0}, {1, 1, 0}) && !Newer({1, 0, 9}, {1, 1, 0}));
}

static void Picking() {
    const Version running{1, 1, 0};
    assert(!PickNewer({}, running));
    assert(!PickNewer({"boilon-ai-player-v1.1.0.bin", "boilon-ai-player-v1.0.9.bin"}, running));
    const auto pick = PickNewer({"boilon-ai-player-v1.1.1.bin", "boilon-ai-player-v1.2.0-offline.bin",
                                 "boilon-ai-player-v1.1.2.bin", "notes.txt", "boilon-ai-player-v1.0.0.bin"},
                                running);
    assert(pick && pick->name == "boilon-ai-player-v1.1.2.bin" && pick->version.patch == 2);
}

static void Markers() {
    const std::string image = std::string(5000, 'x') + "boilon-ai-player v1.1.1" + std::string(3, 'y');
    const auto bytes = reinterpret_cast<const uint8_t*>(image.data());
    for (size_t chunk : {size_t{1}, size_t{2}, size_t{7}, size_t{4096}, image.size()}) {
        Marker name("boilon-ai-player v1.1.1"), other("boilon-ai-player v1.1.2");
        for (size_t at = 0; at < image.size(); at += chunk) {
            const size_t size = std::min(chunk, image.size() - at);
            name.Feed(bytes + at, size);
            other.Feed(bytes + at, size);
        }
        assert(name.Found() && !other.Found());
    }
    for (size_t split = 0; split <= 23; ++split) {  // every boundary inside the name
        Marker name("boilon-ai-player v1.1.1");
        const std::string text = "..boilon-ai-player v1.1.1..";
        name.Feed(reinterpret_cast<const uint8_t*>(text.data()), split + 2);
        name.Feed(reinterpret_cast<const uint8_t*>(text.data()) + split + 2, text.size() - split - 2);
        assert(name.Found());
    }
    Marker prefix("boilon-ai-player v1.1.1");
    const std::string near = "boilon-ai-player v1.1.";
    prefix.Feed(reinterpret_cast<const uint8_t*>(near.data()), near.size());
    assert(!prefix.Found());
}

static void Decisions() {
    Situation s;
    assert(Decide(s) == Skip::kNone);  // unknown states and battery do not block
    s.running = SlotState::kPendingVerify;
    assert(Decide(s) == Skip::kUnconfirmed);
    for (auto state : {SlotState::kValid, SlotState::kUndefined, SlotState::kNew}) {
        s.running = state;
        assert(Decide(s) == Skip::kNone);
    }
    s.battery = kMinBattery - 1;
    assert(Decide(s) == Skip::kLowBattery);
    s.battery = kMinBattery;
    assert(Decide(s) == Skip::kNone);
    s.target_holds_this_image = true;
    for (auto state : {SlotState::kAborted, SlotState::kInvalid}) {
        s.target = state;
        assert(Decide(s) == Skip::kFailedBefore);
    }
    for (auto state : {SlotState::kValid, SlotState::kNew, SlotState::kUndefined}) {
        s.target = state;  // the same image that did start: writing it again is harmless
        assert(Decide(s) == Skip::kNone);
    }
    s.target = SlotState::kAborted;
    s.target_holds_this_image = false;  // a newer image over a failed one
    assert(Decide(s) == Skip::kNone);
    assert(ShouldConfirm(SlotState::kPendingVerify));
    for (auto state : {SlotState::kNew, SlotState::kValid, SlotState::kUndefined, SlotState::kAborted,
                       SlotState::kInvalid})
        assert(!ShouldConfirm(state));
    assert(Progress(0, 0) == 0 && Progress(1, 3) == 33 && Progress(3, 3) == 100);
}

int main() {
    Versions();
    Picking();
    Markers();
    Decisions();
}
'''


class SdUpdateRulesTests(unittest.TestCase):
    def test_rules(self):
        compile_and_run(self, SOURCE)


class SdUpdateWiringTests(unittest.TestCase):
    def test_the_ai_variant_checks_the_card_before_the_scan_and_confirms_after_startup(self):
        source = (BOARD / "m3_ai_main.cc").read_text(encoding="utf-8")
        self.assertEqual(source.count('#include "m7_sd_update_esp.h"'), 1)
        mount = source[source.index("media::MediaCatalog MountLibrary("):]
        self.assertLess(mount.index('"Startup stage: FAT32_MOUNTED"'),
                        mount.index("boilon::sd_update::Run(\n        \"/sdcard\""))
        self.assertLess(mount.index("boilon::sd_update::Run("), mount.index("boilon::local_library::Scan("))
        player = source[source.index("[[noreturn]] void RunPlayer() {"):]
        self.assertLess(player.index("if (!runtime.Start(NowMs()))"),
                        player.index("boilon::sd_update::ConfirmIfPending();"))
        no_story = source[source.index("[[noreturn]] void NoStory("):]
        self.assertLess(no_story.index("boilon::sd_update::ConfirmIfPending();"),
                        no_story.index("while (true)"))
        main = source[source.index('extern "C" void app_main()'):]
        self.assertLess(main.index("boilon::release::kName"), main.index("boilon::sd_update::LogBoot();"))
        # The first boot after an update says which version now runs, before the scan.
        self.assertLess(mount.index("boilon::sd_update::Run("), mount.index("if (boilon::sd_update::JustUpdated())"))
        self.assertLess(mount.index("boilon::ui::Page::kUpdated"), mount.index("boilon::local_library::Scan("))

    def test_both_variants_put_the_version_on_their_pages(self):
        for name in ("m3_ai_main.cc", "m2_library_main.cc"):
            with self.subTest(name):
                source = (BOARD / name).read_text(encoding="utf-8")
                show = source[source.index("void ShowPage("):]
                show = show[:show.index("\n}") + 2]
                self.assertIn("model.version = boilon::release::kVersion;", show)

    def test_only_the_ai_variant_updates_from_the_card(self):
        self.assertNotIn("m7_sd_update", (BOARD / "m2_library_main.cc").read_text(encoding="utf-8"))

    def test_the_file_name_is_the_release_asset_name(self):
        rules = (BOARD / "m7_sd_update.h").read_text(encoding="utf-8")
        name = re.search(r'kName\[\] = "([^"]*)";', (BOARD / "m6_release.h").read_text(encoding="utf-8"))[1]
        prefix = re.search(r'kFilePrefix = "([^"]*)";', rules)[1]
        self.assertEqual(name.replace(" ", "-").rsplit("v", 1)[0] + "v", prefix)


if __name__ == "__main__":
    unittest.main()
