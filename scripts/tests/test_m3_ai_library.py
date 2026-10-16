"""Run the P5-2 card lookup (m3_ai_library.h) unchanged against a built catalog.

The two MCP tools only forward to this code: the lists, the search, how a
directory name is matched, what a pick says and the pick handed to the player.
A real MediaCatalog is built from synthetic files; this proves the replies and
the hand-over, not how a model or a server uses them.
"""

import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/boilon-s3-gen2"

SOURCE = r'''
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>
#include "m3_ai_library.h"
using namespace boilon::ai::card;

namespace {
bool Has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}
// Every byte belongs to a whole UTF-8 character.
bool WholeUtf8(const std::string& text) {
    for (size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        const size_t length = lead < 0x80 ? 1 : lead >> 5 == 6 ? 2 : lead >> 4 == 14 ? 3 : 4;
        if (i + length > text.size())
            return false;
        for (size_t j = 1; j < length; ++j)
            if ((static_cast<unsigned char>(text[i + j]) & 0xC0) != 0x80)
                return false;
        i += length;
    }
    return true;
}
std::string Repeat(const std::string& text, int times) {
    std::string out;
    while (times--)
        out += text;
    return out;
}
const std::string kLongDirectory = Repeat("长", 25) + "目录";
const std::string kLongTitle = Repeat("很", 30);

media::MediaCatalog Library() {
    media::MediaCatalogBuilder builder("card");
    auto add = [&](const std::string& directory, const std::string& file) {
        assert(builder.Add({directory, file, 1000, 1}) == media::AddResult::kAdded);
    };
    add("", "Hello Song.mp3");
    char name[64];
    for (int i = 1; i <= 25; ++i) {
        const char* title = i == 7 ? "三打白骨精（上）" : i == 8 ? "三打白骨精（下）" : "故事";
        std::snprintf(name, sizeof(name), "%03d %s.mp3", i, title);
        add("凯叔西游记", name);
    }
    for (const char* file : {"01 桃园结义.mp3", "02 三顾茅庐.mp3", "03 草船借箭.mp3"})
        add("钱儿爸超级三国", file);
    add("西游记儿歌", "猴哥.mp3");
    add("西游记儿歌", "取经路上.mp3");
    add("Bedtime Songs", "Twinkle Twinkle Little Star.mp3");
    add("Bedtime Songs", kLongTitle + ".mp3");
    add(kLongDirectory, "a.mp3");
    auto built = builder.Finish();
    assert(built.Ok());
    return std::move(built.catalog);
}
size_t Index(const media::MediaCatalog& catalog, const std::string& name) {
    const auto& collections = catalog.Collections();
    for (size_t i = 0; i < collections.size(); ++i)
        if (DirectoryName(collections[i]) == name)
            return i;
    assert(false);
    return 0;
}

void Helpers() {
    assert(Title("Hello Song.mp3") == "Hello Song" && Title("noext") == "noext");
    assert(Title(".mp3") == ".mp3" && Title("a.longext") == "a.longext");
    assert(Shorten("short") == "short");
    assert(Shorten(Repeat("a", 70)) == Repeat("a", 60) + "…");
    // A cut never splits a character: "a" then 30 three-byte characters.
    const auto cut = Shorten("a" + kLongTitle);
    assert(cut == "a" + Repeat("很", 19) + "…" && WholeUtf8(cut));
    std::string json;
    AppendString(json, std::string("a\"b\\c\n\x01", 7));
    assert(json == "\"a\\\"b\\\\c\\u000a\\u0001\"");
    assert(Contains("Twinkle Twinkle", "TWINKLE") && Contains("007 三打白骨精（上）", "白骨精"));
    assert(!Contains("三打白骨", "白骨精") && Contains("anything", ""));
    assert(Words("西游  白骨精 ") == std::vector<std::string>({"西游", "白骨精"}));
    assert(Words("西游\xE3\x80\x80白骨精") == std::vector<std::string>({"西游", "白骨精"}));
    assert(Words(" \xE3\x80\x80 ").empty());
}

void NoCard() {
    Card card;
    assert(!card.List("", "", 1).ok && card.List("", "", 1).text == kNoCard);
    assert(!card.Play("凯叔西游记", 1).ok && !card.Take());
}

void Lists() {
    const auto catalog = Library();
    const auto xiyou = Index(catalog, "凯叔西游记");
    Card card;
    card.Publish(&catalog);
    auto reply = card.List("", "", 1);
    assert(reply.ok && Has(reply.text, R"({"name":"凯叔西游记","files":25})"));
    assert(Has(reply.text, R"({"name":"未分类","files":1})"));
    assert(Has(reply.text, R"("page":1,"pages":1)") && !Has(reply.text, "\"now\""));
    card.SetNow(std::make_pair(xiyou, size_t{7}));
    reply = card.List("", "", 1);
    assert(Has(reply.text, R"("now":{"directory":"凯叔西游记","number":7,)"
                           R"("title":"007 三打白骨精（上）","mode":"order"})"));
    card.SetNow(std::make_pair(xiyou, size_t{26}));  // Not a file: no "now".
    assert(!Has(card.List("", "", 1).text, "\"now\""));
    card.SetNow(std::nullopt);
    assert(!Has(card.List("", "", 1).text, "\"now\""));
    assert(!card.List("", "", 2).ok && card.List("", "", 2).text ==
                                           "Page 2 does not exist; there are 1 pages.");

    reply = card.List("凯叔西游记", "", 1);
    assert(reply.ok && Has(reply.text, R"({"directory":"凯叔西游记","files":25,"page":1,"pages":2,)"));
    assert(Has(reply.text, R"("items":[{"number":1,"title":"001 故事"},)"));
    assert(Has(reply.text, R"({"number":20,"title":"020 故事"}]})") && !Has(reply.text, "021"));
    reply = card.List("凯叔", "", 2);  // The only directory containing the name.
    assert(reply.ok && Has(reply.text, R"("page":2,"pages":2,"items":[{"number":21,)"));
    assert(Has(reply.text, R"({"number":25,"title":"025 故事"}]})"));
    assert(!card.List("凯叔西游记", "", 3).ok && !card.List("凯叔西游记", "", 0).ok);
    assert(card.List("凯叔西游记第7集", "", 1).ok);  // The name contains one directory.
    reply = card.List("西游记", "", 1);  // Two directories contain it.
    assert(!reply.ok && Has(reply.text, "No single directory matches \"西游记\". Directories:"));
    assert(Has(reply.text, "\"西游记儿歌\"") && Has(reply.text, "\"凯叔西游记\""));
    assert(card.List("bedtime songs", "", 1).ok);  // English letters ignore case.

    reply = card.List("Bedtime Songs", "", 1);  // Long titles are shortened, never split.
    assert(Has(reply.text, R"("title":")" + Repeat("很", 20) + "…\"") && WholeUtf8(reply.text));
    reply = card.List("", "", 1);  // So are long directory names...
    const std::string shortened = Repeat("长", 20) + "…";
    assert(Has(reply.text, "{\"name\":\"" + shortened + "\",\"files\":1}"));
    reply = card.List(shortened, "", 1);  // ...and the shortened name still finds it.
    assert(reply.ok && Has(reply.text, R"({"number":1,"title":"a"})"));
}

void Search() {
    const auto catalog = Library();
    Card card;
    card.Publish(&catalog);
    auto reply = card.List("", "白骨精", 1);
    assert(reply.ok);
    assert(reply.text ==
           R"({"keyword":"白骨精","matches":[)"
           R"({"directory":"凯叔西游记","number":7,"title":"007 三打白骨精（上）"},)"
           R"({"directory":"凯叔西游记","number":8,"title":"008 三打白骨精（下）"}],"more":false})");
    reply = card.List("", "西游 下", 1);  // Every word, in the title or the directory name.
    assert(Has(reply.text, R"("number":8,)") && !Has(reply.text, R"("number":7,)"));
    assert(!Has(reply.text, "猴哥") && !Has(reply.text, "取经路上"));
    reply = card.List("", "西游 猴哥", 1);
    assert(Has(reply.text, R"({"directory":"西游记儿歌","number":)") &&
           Has(reply.text, R"(,"title":"猴哥"}],"more":false})"));
    assert(Has(card.List("", "TWINKLE", 1).text, "Twinkle Twinkle Little Star"));
    reply = card.List("", "故事", 1);  // 23 matches: the first 20, and more.
    assert(Has(reply.text, R"("number":22,)") && !Has(reply.text, R"("number":23,)"));
    assert(Has(reply.text, R"("more":true})"));
    reply = card.List("钱儿爸", "三顾", 1);  // Within one directory.
    assert(Has(reply.text, R"({"directory":"钱儿爸超级三国","number":2,"title":"02 三顾茅庐"})"));
    assert(!Has(card.List("钱儿爸", "白骨精", 1).text, "number"));
    assert(card.List("", "不存在的故事", 1).text ==
           R"({"keyword":"不存在的故事","matches":[],"more":false})");
    assert(!card.List("没有这个", "白骨精", 1).ok);
}

void Picks() {
    const auto catalog = Library();
    const auto xiyou = Index(catalog, "凯叔西游记"), sanguo = Index(catalog, "钱儿爸超级三国");
    Card card;
    card.Publish(&catalog);
    auto reply = card.Play("凯叔西游记", 7);
    assert(reply.ok);
    assert(reply.text ==
           R"({"playing":{"directory":"凯叔西游记","number":7,"title":"007 三打白骨精（上）"},)"
           R"("from":"its beginning","starts":"when you finish this answer"})");
    auto pick = card.Take();
    assert(pick && pick->directory == xiyou && pick->number == 7 && !card.Take());
    reply = card.Play("钱儿爸超级三国", 0);
    assert(reply.text == R"({"playing":{"directory":"钱儿爸超级三国"},)"
                         R"("from":"where it was last heard","starts":"when you finish this answer"})");
    assert(card.Play("凯叔", 3).ok);  // The later pick replaces one not taken yet.
    pick = card.Take();
    assert(pick && pick->directory == xiyou && pick->number == 3 && !card.Take());
    reply = card.Play("凯叔西游记", 26);
    assert(!reply.ok && reply.text == "\"凯叔西游记\" has 25 files: number must be 1 to 25, or 0 "
                                      "to continue where it was last heard.");
    assert(!card.Play("凯叔西游记", -1).ok && !card.Play("西游记", 1).ok && !card.Play("", 1).ok);
    assert(!card.Take());  // Refusals leave nothing for the player.
    assert(card.Play("未分类", 1).ok);
    pick = card.Take();
    assert(pick && pick->directory == Index(catalog, "未分类") && pick->number == 1);
    assert(card.Play("钱儿爸超级三国", 3).ok);
    card.Publish(nullptr);  // A card error: the tools say so, a pick already left still counts.
    assert(card.List("", "", 1).text == kNoCard && card.Play("凯叔西游记", 1).text == kNoCard);
    pick = card.Take();
    assert(pick && pick->directory == sanguo && pick->number == 3);
}

// 2026-10-15: each directory's play mode, what NVS keeps and what the tools say.
void Modes() {
    using media::PlayMode;
    assert(std::string(ModeName(PlayMode::kOrder)) == "order" &&
           std::string(ModeName(PlayMode::kShuffle)) == "shuffle");
    assert(ParseMode("loop") == PlayMode::kLoop && ParseMode("single") == PlayMode::kSingle);
    assert(!ParseMode("Loop") && !ParseMode("") && !ParseMode("random"));
    // The saved format: byte 1, then 8-byte IDs (little-endian) and mode bytes.
    const std::vector<Saved> two = {{0x0102030405060708ULL, PlayMode::kShuffle},
                                    {42, PlayMode::kSingle}};
    const auto bytes = EncodeModes(two);
    assert(bytes.size() == 19 && bytes[0] == 1 && bytes[1] == 0x08 && bytes[8] == 0x01 &&
           bytes[9] == 3 && bytes[10] == 42 && bytes[18] == 2);
    auto back = DecodeModes(bytes);
    assert(back.size() == 2 && back[0].directory == two[0].directory &&
           back[0].mode == PlayMode::kShuffle && back[1].directory == 42 &&
           back[1].mode == PlayMode::kSingle);
    assert(DecodeModes({}).empty() && DecodeModes({1}).empty() && EncodeModes({}) ==
                                                                   std::vector<uint8_t>{1});
    auto other = bytes;
    other[0] = 2;  // Another format: nothing.
    assert(DecodeModes(other).empty());
    other = bytes;
    other.pop_back();  // Not whole entries: nothing.
    assert(DecodeModes(other).empty());
    other = bytes;
    other[9] = 0;  // A bad mode drops only that entry, and so does a repeated ID.
    assert(DecodeModes(other).size() == 1 && DecodeModes(other)[0].directory == 42);
    other = bytes;
    other.insert(other.end(), bytes.begin() + 10, bytes.end());
    assert(DecodeModes(other).size() == 2);
    std::vector<Saved> full(64, {0, PlayMode::kLoop});
    for (size_t i = 0; i < full.size(); ++i)
        full[i].directory = i + 1;
    assert(DecodeModes(EncodeModes(full)).size() == 64);
    full.push_back({99, PlayMode::kLoop});  // More than the limit: nothing.
    assert(DecodeModes(EncodeModes(full)).empty());

    const auto catalog = Library();
    const auto xiyou = Index(catalog, "凯叔西游记"), songs = Index(catalog, "西游记儿歌");
    Card card;
    std::vector<uint8_t> saved = {9};
    assert(!card.SetMode("凯叔西游记", "loop", saved).ok && saved.empty());  // No card.
    card.LoadModes(EncodeModes({{catalog.Collections()[songs].id.value, PlayMode::kShuffle},
                                {12345, PlayMode::kLoop}}));
    auto modes = card.Modes(catalog);
    assert(modes.size() == catalog.Collections().size() && modes[songs] == PlayMode::kShuffle &&
           modes[xiyou] == PlayMode::kOrder);
    card.Publish(&catalog);
    assert(!card.TakeModes());  // Loading is not a change.
    auto reply = card.List("", "", 1);
    assert(Has(reply.text, R"({"name":"西游记儿歌","files":2,"mode":"shuffle"})"));
    assert(Has(reply.text, R"({"name":"凯叔西游记","files":25})"));
    card.SetNow(std::make_pair(xiyou, size_t{7}));
    assert(Has(card.List("", "", 1).text,
               R"("now":{"directory":"凯叔西游记","number":7,"title":"007 三打白骨精（上）","mode":"order"})"));

    assert(!card.SetMode("", "random", saved).ok && saved.empty());
    reply = card.SetMode("西游记", "loop", saved);  // Two directories contain it.
    assert(!reply.ok && Has(reply.text, "No single directory matches") && saved.empty());
    reply = card.SetMode("", "single", saved);  // The selected directory.
    assert(reply.ok && reply.text == R"({"directory":"凯叔西游记","mode":"single"})");
    back = DecodeModes(saved);  // Added after what was loaded.
    assert(back.size() == 3 && back[2].directory == catalog.Collections()[xiyou].id.value &&
           back[2].mode == PlayMode::kSingle && back[1].directory == 12345);
    modes = *card.TakeModes();
    assert(modes[xiyou] == PlayMode::kSingle && modes[songs] == PlayMode::kShuffle);
    assert(!card.TakeModes());
    reply = card.SetMode("凯叔", "single", saved);  // Unchanged: nothing to save or apply.
    assert(reply.ok && saved.empty() && !card.TakeModes());
    assert(Has(card.List("", "", 1).text, R"(,"mode":"single"})"));
    // A pick not taken yet is what "this one" means; then the selection again.
    assert(card.Play("钱儿爸超级三国", 2).ok);
    reply = card.SetMode("", "loop", saved);
    assert(reply.ok && Has(reply.text, R"("directory":"钱儿爸超级三国","mode":"loop")"));
    assert(card.Take() && card.SetMode("", "order", saved).ok);
    back = DecodeModes(saved);  // Back in order: the directory leaves the saved list.
    assert(saved.size() == 1 + 3 * 9 && back.size() == 3 && back[2].directory == catalog.Collections()[Index(catalog, "钱儿爸超级三国")].id.value);
    assert(card.TakeModes()->at(xiyou) == PlayMode::kOrder);
    back = DecodeModes(saved);
    assert(std::none_of(back.begin(), back.end(), [&](const Saved& entry) {
        return entry.directory == catalog.Collections()[xiyou].id.value;
    }));
    card.SetNow(std::nullopt);
    reply = card.SetMode("", "loop", saved);
    assert(!reply.ok && reply.text == "Nothing is selected: name the directory." && saved.empty());
    // The oldest entry goes when the list is full; the newest stays.
    std::vector<Saved> many;
    for (uint64_t i = 1; i <= 64; ++i)
        many.push_back({i, PlayMode::kLoop});
    card.LoadModes(EncodeModes(many));
    assert(card.SetMode("Bedtime Songs", "shuffle", saved).ok);
    back = DecodeModes(saved);
    assert(back.size() == 64 && back.front().directory == 2 &&
           back.back().directory == catalog.Collections()[Index(catalog, "Bedtime Songs")].id.value);
    card.Publish(nullptr);
    assert(card.SetMode("凯叔西游记", "loop", saved).text == kNoCard && !card.TakeModes());
}

// 2026-10-16: what self.story.forget_progress leaves for the player and says.
void Forgets() {
    const auto catalog = Library();
    const auto xiyou = Index(catalog, "凯叔西游记"), sanguo = Index(catalog, "钱儿爸超级三国");
    Card card;
    assert(card.ForgetProgress("凯叔西游记", false).text == kNoCard && !card.TakeForget());
    card.Publish(&catalog);
    auto reply = card.ForgetProgress("", false);  // Nothing selected: name it, or all.
    assert(!reply.ok && reply.text == "Nothing is selected: name the directory.");
    reply = card.ForgetProgress("", true);
    assert(reply.ok && reply.text == R"({"forgot":"all","conversation":"goes on"})");
    auto taken = card.TakeForget();
    assert(taken && !taken->directory && !card.TakeForget());
    // The selected directory, or all: the conversation ends; another one: it goes on.
    card.SetNow(std::make_pair(xiyou, size_t{7}));
    reply = card.ForgetProgress("", false);
    assert(reply.text ==
           R"({"forgot":"凯叔西游记","conversation":"ends when you finish this answer"})");
    taken = card.TakeForget();
    assert(taken && taken->directory == xiyou);
    reply = card.ForgetProgress("钱儿爸", false);
    assert(reply.text == R"({"forgot":"钱儿爸超级三国","conversation":"goes on"})");
    assert(card.TakeForget()->directory == sanguo);
    reply = card.ForgetProgress("", true);
    assert(reply.text == R"({"forgot":"all","conversation":"ends when you finish this answer"})");
    assert(!card.TakeForget()->directory);
    assert(Has(card.ForgetProgress("凯叔西游记", true).text, R"("forgot":"all")"));
    assert(!card.TakeForget()->directory);
    // A pick not taken yet is "this one"; the forget leaves the pick to the player.
    assert(card.Play("钱儿爸超级三国", 2).ok);
    reply = card.ForgetProgress("", false);
    assert(reply.text ==
           R"({"forgot":"钱儿爸超级三国","conversation":"ends when you finish this answer"})");
    assert(card.Take() && card.TakeForget()->directory == sanguo);
    assert(!card.ForgetProgress("西游记", false).ok && !card.ForgetProgress("没有这个", false).ok);
    assert(!card.TakeForget());
    // One request waits for the player at a time: the same one or all merge.
    assert(card.ForgetProgress("凯叔西游记", false).ok && card.ForgetProgress("凯叔", false).ok);
    reply = card.ForgetProgress("钱儿爸超级三国", false);
    assert(!reply.ok && reply.text == "Still forgetting the last directory: try again in a moment.");
    assert(card.ForgetProgress("钱儿爸超级三国", true).ok);
    taken = card.TakeForget();
    assert(taken && !taken->directory && !card.TakeForget());
    assert(card.ForgetProgress("", true).ok && card.ForgetProgress("钱儿爸超级三国", false).ok);
    assert(!card.TakeForget()->directory && !card.TakeForget());
    card.Publish(nullptr);
    assert(card.ForgetProgress("", true).text == kNoCard && !card.TakeForget());
}

// The most a card may hold, with long names: every reply stays near 4 KB.
void Bounded() {
    media::MediaCatalogBuilder builder("full");
    char prefix[16];
    for (int d = 0; d < 64; ++d)
        for (int f = 0; f < 32; ++f) {
            std::snprintf(prefix, sizeof(prefix), "%02d", d);
            const std::string directory = prefix + Repeat("目", 100);
            std::snprintf(prefix, sizeof(prefix), "%02d", f);
            assert(builder.Add({directory, prefix + Repeat("名", 32) + ".mp3", 1000, 1}) ==
                   media::AddResult::kAdded);
        }
    auto built = builder.Finish();
    assert(built.Ok() && built.catalog.Collections().size() == 64);
    Card card;
    card.Publish(&built.catalog);
    card.SetNow(std::make_pair(size_t{63}, size_t{32}));
    const std::string first = DirectoryName(built.catalog.Collections()[0]);
    std::vector<Reply> replies;
    for (int page = 1; page <= 4; ++page)
        replies.push_back(card.List("", "", page));
    replies.push_back(card.List(first, "", 1));
    replies.push_back(card.List(first, "", 2));
    replies.push_back(card.List("", "名", 1));
    replies.push_back(card.List("", Repeat("名", 200), 1));
    replies.push_back(card.List("没有这个", "", 1));
    replies.push_back(card.Play(first, 32));
    std::vector<Saved> all;  // Every directory shows a mode.
    for (const auto& collection : built.catalog.Collections())
        all.push_back({collection.id.value, media::PlayMode::kShuffle});
    card.LoadModes(EncodeModes(all));
    for (int page = 1; page <= 4; ++page)
        replies.push_back(card.List("", "", page));
    std::vector<uint8_t> saved;
    replies.push_back(card.SetMode(first, "loop", saved));
    assert(replies.back().ok && saved.size() == 1 + 64 * 9);
    replies.push_back(card.ForgetProgress(first, false));
    assert(replies.back().ok && card.TakeForget()->directory == size_t{0});
    for (const auto& reply : replies)
        assert(reply.text.size() <= 4096 && WholeUtf8(reply.text));
    assert(!card.List("", "", 5).ok && Has(replies[8].text, " and 44 more."));
}
}  // namespace

int main() {
    Helpers();
    NoCard();
    Lists();
    Search();
    Picks();
    Modes();
    Forgets();
    Bounded();
}
'''


class M3AiLibraryHostTests(unittest.TestCase):
    def test_card_lookup_and_pick_with_a_built_catalog(self):
        compiler = shutil.which("c++")
        self.assertTrue(compiler, "Native C++ compiler required; do not silently skip")
        with tempfile.TemporaryDirectory(prefix="m3-ai-library-") as temporary:
            executable = Path(temporary) / "m3-ai-library-test"
            compiled = subprocess.run(
                [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                 "-I", str(BOARD), "-I", str(ROOT / "main"), "-x", "c++", "-",
                 str(ROOT / "main/media/media_catalog.cc"), "-o", str(executable)],
                input=SOURCE, text=True, capture_output=True, timeout=60, cwd=ROOT)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            tested = subprocess.run([str(executable)], text=True, capture_output=True,
                                    timeout=10, cwd=temporary)
            self.assertEqual(tested.returncode, 0, tested.stdout + tested.stderr)


    def test_production_wiring(self):
        board = (BOARD / "m3_ai_board.cc").read_text(encoding="utf-8")
        main = (BOARD / "m3_ai_main.cc").read_text(encoding="utf-8")
        runtime = (BOARD / "m2_library_runtime.h").read_text(encoding="utf-8")
        # Two board tools with unique names that only forward to the shared card.
        names = re.findall(r'AddTool\(\s*"(self\.[a-z_.]+)"', board)
        self.assertEqual(len(names), len(set(names)))
        self.assertIn("self.story.list", names)
        self.assertIn("self.story.play", names)
        self.assertNotIn("self.story.", (ROOT / "main/mcp_server.cc").read_text(encoding="utf-8"))
        for name, call in (("self.story.list", "Card::Shared().List("),
                           ("self.story.play", "Card::Shared().Play(")):
            self.assertIn(call, board.split(f'"{name}"', 1)[1].split("});", 1)[0])
        # Published once the story plays; the no-story page withdraws it.
        run = main.split("[[noreturn]] void RunPlayer() {", 1)[1]
        self.assertLess(run.index("runtime.Start(NowMs())"),
                        run.index("card.Publish(&runtime.Catalog());"))
        no_story = main.split("[[noreturn]] void NoStory(", 1)[1].split("\n}\n", 1)[0]
        self.assertIn("Card::Shared().Publish(nullptr);", no_story)
        # In a conversation the pick waits for the answer; outside one it plays now.
        self.assertIn("card.SetNow(focus.Picked() ? focus.Picked() : runtime.Selected());", run)
        take = run.split("if (const auto pick = card.Take()) {", 1)[1].split("\n        }\n", 1)[0]
        self.assertLess(take.index("focus.Choose(*pick);"), take.index("session.EndAfterReply("))
        self.assertIn("runtime.Choose(pick->directory, pick->number, NowMs())", take)
        resume = main.split("bool ResumeStory(uint64_t now_ms) override {", 1)[1]
        resume = resume.split("\n    }\n", 1)[0]
        self.assertLess(resume.index("runtime_->Choose("), resume.index("runtime_->Unpark("))
        # 2026-10-15: the play mode, pause and power-off tools, each forwarding.
        for name, call in (("self.story.set_play_mode", "Card::Shared().SetMode("),
                           ("self.story.pause", "story_end = boilon::ai::StoryEnd::kPause;"),
                           ("self.story.power_off", "story_end = boilon::ai::StoryEnd::kPowerOff;")):
            self.assertIn(name, names)
            self.assertIn(call, board.split(f'"{name}"', 1)[1].split("});", 1)[0])
        mode_tool = board.split('"self.story.set_play_mode"', 1)[1].split("});", 1)[0]
        self.assertIn("bytes.empty() ? -1 : SavePlayModes(bytes)", mode_tool)  # Only a change.
        pause_tool = board.split('"self.story.pause"', 1)[1].split("});", 1)[0]
        self.assertLess(pause_tool.index("Published()"), pause_tool.index("story_end ="))
        # Saved modes apply before the boot resume plays; the shuffle seed is random.
        start = run.index("runtime.Start(NowMs())")
        for call in ("runtime.Seed(esp_random());", "card.LoadModes(boilon::ai::ReadPlayModes());",
                     "ApplyModes(runtime, card.Modes(runtime.Catalog()));"):
            self.assertLess(run.index(call), start)
        self.assertIn("if (const auto modes = card.TakeModes())\n            ApplyModes(runtime, *modes);",
                      run)
        # In a conversation pause and power-off wait for the answer; outside one, at once.
        end = run.split("if (const auto end = boilon::ai::TakeStoryEnd();", 1)[1]
        end = end.split("\n        }\n", 1)[0]
        self.assertLess(end.index("focus.End(end);"), end.index("session.EndAfterReply(NowMs());"))
        self.assertIn("NormalShutdown(lcd, player, runtime, persistence);", end)
        self.assertIn("runtime.Pause(NowMs())", end)
        self.assertIn("if (focus.PowerOffDue() && !session.Active())\n            NormalShutdown(",
                      run)
        self.assertLess(resume.index("StoryEnd::kPowerOff"), resume.index("runtime_->StayPaused();"))
        self.assertLess(resume.index("runtime_->StayPaused();"), resume.index("runtime_->Choose("))
        self.assertIn("page.EndAfterReply(now);", no_story)
        self.assertIn('if (power_off && !page.Conversation().Active())\n            PowerOff("USER");',
                      no_story)
        # 2026-10-16: forgetting progress forwards to the card, asks to confirm
        # first, and the player applies it after the picks and before pause or
        # power-off, ending the conversation when it includes what is selected.
        self.assertIn("self.story.forget_progress", names)
        forget_tool = board.split('"self.story.forget_progress"', 1)[1].split("});", 1)[0]
        self.assertIn("Card::Shared().ForgetProgress(", forget_tool)
        description = "".join(re.findall(r'"([^"]*)"', forget_tool.split("PropertyList(", 1)[0]))
        self.assertIn("call this only after the user agrees", description)
        self.assertNotIn("nvs_", forget_tool)
        self.assertLess(run.index("if (const auto pick = card.Take()) {"),
                        run.index("forgetting = card.TakeForget();"))
        self.assertLess(run.index("forgetting = card.TakeForget();"),
                        run.index("if (const auto end = boilon::ai::TakeStoryEnd();"))
        forget = run.split("forgetting = card.TakeForget();", 1)[1].split("\n        }\n", 1)[0]
        self.assertIn("const auto now = focus.Picked() ? focus.Picked() : runtime.Selected();",
                      forget)
        self.assertLess(forget.index("if (runtime.Forget(forgetting->directory, NowMs())) {"),
                        forget.index("session.EndAfterReply(NowMs());"))
        self.assertLess(forget.index("session.EndAfterReply(NowMs());"),
                        forget.index("forgetting.reset();"))
        # The previous key no longer restarts the current file first.
        previous = runtime.split("case media::GestureAction::kPreviousTrack:", 1)[1]
        self.assertNotIn("RestartAt", previous.split("default:", 1)[0])


if __name__ == "__main__":
    unittest.main()
