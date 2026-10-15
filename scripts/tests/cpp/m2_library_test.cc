#include "boards/boilon-s3-gen2/m2_library_policy.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace library = boilon::local_library;

int failures = 0;

void Check(bool condition, const char* expression, int line) {
    if (!condition) {
        std::cerr << "line " << line << ": " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

void File(const fs::path& path) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << "synthetic fixture; not an MP3 bitstream";
    CHECK(output.good());
}

struct FakePlayer {
    bool running = false;
    bool accept_start = true;
    bool accept_pause = true;
    int stops = 0;
    std::vector<std::string> starts;
    std::vector<bool> pauses;

    bool Start(const std::string& path) {
        CHECK(!running);
        starts.push_back(path);
        running = accept_start;
        return accept_start;
    }
    bool Pause(bool value) {
        pauses.push_back(value);
        return accept_pause;
    }
    void Stop() { ++stops; }
    bool Running() const { return running; }
};

void TestScanAndStart(const fs::path& directory) {
    const auto root = directory / "minimal";
    File(root / "track10.mp3");
    File(root / "track2.mp3");
    auto scanned = library::Scan(root.string(), "synthetic-library");
    CHECK(scanned.Ok());
    const auto& collections = scanned.catalog.Collections();
    CHECK(collections.size() == 1);
    if (collections.size() != 1) {
        return;
    }
    CHECK(collections[0].tracks.size() == 2);
    if (collections[0].tracks.size() != 2) {
        return;
    }
    CHECK(collections[0].tracks[0].file_name == "track2.mp3");
    CHECK(collections[0].tracks[1].file_name == "track10.mp3");
    media::PlayerController controller(scanned.catalog);
    FakePlayer player;
    library::Switcher<FakePlayer> switcher(controller, player, root.string(), [] { return true; });
    CHECK(switcher.Submit(controller.SelectCollection(collections[0].id), 0));
    CHECK(switcher.Submit(controller.Play(), 10));
    CHECK(switcher.Poll(10));
    CHECK(player.starts.size() == 1);
    if (!player.starts.empty()) {
        CHECK(player.starts[0] == (root / "track2.mp3").string());
    }
    CHECK(!switcher.Pending());
    CHECK(!switcher.Failed());
    std::cout << "scan: track2.mp3, track10.mp3; start: track2.mp3\n";
}

media::MediaId CollectionId(const media::MediaCatalog& catalog, const std::string& name) {
    for (const auto& collection : catalog.Collections()) {
        if (collection.directory_name == name) {
            return collection.id;
        }
    }
    CHECK(false);
    return {};
}

media::MediaCatalog Catalog() {
    media::MediaCatalogBuilder builder("synthetic-library");
    for (const auto* name : {"track10.mp3", "track2.mp3", "track1.mp3"}) {
        CHECK(builder.Add({"", name, 10, 1}) == media::AddResult::kAdded);
    }
    CHECK(builder.Add({"series", "episode1.mp3", 10, 1}) == media::AddResult::kAdded);
    auto built = builder.Finish();
    CHECK(built.Ok());
    return std::move(built.catalog);
}

void CheckEmptyFailure(const library::ScanResult& result, library::ScanError expected) {
    CHECK(!result.Ok());
    CHECK(result.error == expected);
    CHECK(result.catalog.Collections().empty());
}

void TestScanFiltering(const fs::path& directory) {
    const auto root = directory / "filtering";
    File(root / "root10.mp3");
    File(root / "root2.MP3");
    File(root / u8"2_故事" / u8"第10集.mp3");
    File(root / u8"2_故事" / u8"第2集.mp3");
    File(root / u8"2_故事" / u8"第3集.wav");  // F08: WAV sorts among the MP3s by name.
    File(root / "10_series" / "track1.mp3");
    File(root / ".hidden.mp3");
    File(root / "audio.flac");
    File(root / "wrong.mp3.backup");
    File(root / "bad\\name.mp3");
    File(root / ".hidden" / "hidden.mp3");
    File(root / "System Volume Information" / "system.mp3");
    File(root / "$RECYCLE.BIN" / "discarded.mp3");
    File(root / "ReCyClEd" / "discarded.mp3");
    File(root / "lost+found" / "system.mp3");
    File(root / u8"2_故事" / "nested" / "too-deep.mp3");
    File(root / u8"2_故事" / ".hidden.mp3");
    File(root / "empty.mp3");
    fs::resize_file(root / "empty.mp3", 0);
    fs::create_directories(root / "directory.mp3");
    fs::create_symlink(root / "root2.MP3", root / "link.mp3");
    fs::create_directory_symlink(root / u8"2_故事", root / "linked-series");

    auto scanned = library::Scan(root.string(), "filter-library");
    CHECK(scanned.Ok());
    CHECK(scanned.catalog.Collections().size() == 3);
    const auto& collections = scanned.catalog.Collections();
    if (collections.size() == 3) {
        CHECK(collections[0].is_root);
        CHECK(collections[1].directory_name == u8"2_故事");
        CHECK(collections[2].directory_name == "10_series");
        CHECK(collections[0].tracks.size() == 2);
        CHECK(collections[1].tracks.size() == 3);
        CHECK(collections[2].tracks.size() == 1);
        if (collections[0].tracks.size() == 2 && collections[1].tracks.size() == 3) {
            CHECK(collections[0].tracks[0].file_name == "root2.MP3");
            CHECK(collections[0].tracks[1].file_name == "root10.mp3");
            CHECK(collections[1].tracks[0].file_name == u8"第2集.mp3");
            CHECK(collections[1].tracks[1].file_name == u8"第3集.wav");
            CHECK(collections[1].tracks[2].file_name == u8"第10集.mp3");
            CHECK(collections[0].tracks[0].size_bytes == fs::file_size(root / "root2.MP3"));
        }
    }
    // Traversal order and mount path must not become media identity.
    auto repeated = library::Scan(root.string(), "filter-library");
    CHECK(repeated.Ok());
    CHECK(CollectionId(scanned.catalog, "") == CollectionId(repeated.catalog, ""));
    CHECK(scanned.entries > 5);  // Skipped entries still consume the scan budget.
}

void TestScanFailures(const fs::path& directory) {
    const auto root = directory / "failures";
    File(root / "track1.mp3");
    File(root / "track2.mp3");
    for (const auto& path : {std::string(), std::string("relative"), root.string() + "/",
                             root.string() + "/../failures", root.string() + "/./child",
                             root.string() + "//child", root.string() + std::string("\0x", 2)}) {
        CheckEmptyFailure(library::Scan(path, "fixture"), library::ScanError::kPath);
    }
    CheckEmptyFailure(library::Scan(root.string(), "fixture", 0), library::ScanError::kPath);
    auto limited = library::Scan(root.string(), "fixture", 1);
    CheckEmptyFailure(limited, library::ScanError::kLimit);
    CHECK(limited.entries == 2);
    CHECK(library::Scan(root.string(), "fixture", 2).Ok());
    const auto skipped = directory / "skipped-limit";
    File(skipped / ".hidden.mp3");
    File(skipped / "ignored.flac");
    CheckEmptyFailure(library::Scan(skipped.string(), "fixture", 1),
                      library::ScanError::kLimit);
    media::CatalogLimits limits;
    limits.max_tracks_total = 1;
    CheckEmptyFailure(library::Scan(root.string(), "fixture", 4096, limits),
                      library::ScanError::kCatalog);
    CheckEmptyFailure(library::Scan(root.string(), ""), library::ScanError::kCatalog);
    CheckEmptyFailure(library::Scan((directory / "absent").string(), "fixture"),
                      library::ScanError::kIo);
    CheckEmptyFailure(library::Scan((root / "track1.mp3").string(), "fixture"),
                      library::ScanError::kIo);
    fs::create_directory_symlink(root, directory / "root-link");
    CheckEmptyFailure(library::Scan((directory / "root-link").string(), "fixture"),
                      library::ScanError::kIo);
    fs::create_directories(directory / "empty");
    File(directory / "empty" / "ignored.flac");
    CheckEmptyFailure(library::Scan((directory / "empty").string(), "fixture"),
                      library::ScanError::kEmpty);
    // One accepted name pushes the full selected file path beyond the player bound.
    const auto long_root = directory / std::string(100, 'a') / std::string(100, 'b') /
                           std::string(100, 'c');
    File(long_root / (std::string(220, 'd') + ".mp3"));
    CheckEmptyFailure(library::Scan(long_root.string(), "fixture"), library::ScanError::kPath);
}

void TestSwitchWaitAndLatestIntent() {
    const auto catalog = Catalog();
    media::PlayerController controller(catalog);
    FakePlayer player;
    library::Switcher<FakePlayer> switcher(controller, player, "/synthetic", [] { return true; });
    CHECK(switcher.Submit(controller.SelectCollection(CollectionId(catalog, "")), 0));
    CHECK(switcher.Submit(controller.Play(), 1));
    CHECK(switcher.Submit(controller.Next(), 10));
    CHECK(switcher.Pending());
    CHECK(player.stops == 1);
    CHECK(player.starts.size() == 1);
    CHECK(switcher.Poll(11));
    CHECK(switcher.Submit(controller.Next(), 12));
    CHECK(player.stops == 1);
    CHECK(player.starts.size() == 1);
    player.running = false;  // Old decoder acknowledges its asynchronous Stop.
    CHECK(switcher.Poll(13));
    CHECK(!switcher.Pending());
    CHECK(player.starts.size() == 2);
    CHECK(player.starts.back() == "/synthetic/track10.mp3");
    const auto before = player.starts.size();
    CHECK(switcher.Submit(controller.Next(), 14));  // Boundary.
    CHECK(switcher.Submit(controller.Play(), 15));  // Already playing.
    CHECK(player.starts.size() == before);
    CHECK(player.stops == 1);
    std::cout << "switch: Stop once; old worker still running -> wait; latest track10 starts "
                 "after old worker exits\n";
}

void TestPausedSelectionAndPendingPause() {
    const auto catalog = Catalog();
    for (const bool pause_before_move : {true, false}) {
        media::PlayerController controller(catalog);
        FakePlayer player;
        library::Switcher<FakePlayer> switcher(controller, player, "/synthetic", [] { return true; });
        CHECK(switcher.Submit(controller.SelectCollection(CollectionId(catalog, "")), 0));
        CHECK(switcher.Submit(controller.Play(), 1));
        if (pause_before_move) {
            CHECK(switcher.Submit(controller.Pause(), 2));
            CHECK(player.pauses == std::vector<bool>{true});
        }
        CHECK(switcher.Submit(controller.Next(), 3));
        if (!pause_before_move) {
            CHECK(switcher.Submit(controller.Pause(), 4));
            CHECK(player.pauses.empty());
        }
        CHECK(player.stops == 1);
        CHECK(player.starts.size() == 1);
        player.running = false;
        CHECK(switcher.Poll(5));
        CHECK(player.starts.size() == 1);
        CHECK(controller.State() == media::PlaybackState::kPaused);
        CHECK(switcher.Submit(controller.Play(), 6));
        CHECK(player.starts.size() == 2);
        CHECK(player.starts.back() == "/synthetic/track2.mp3");
        CHECK(!switcher.Pending());
    }
    std::cout << "pause: selecting another track stays silent; Play starts the selected track\n";
}

void TestPauseResumeAndCollectionReset() {
    const auto catalog = Catalog();
    media::PlayerController controller(catalog);
    FakePlayer player;
    library::Switcher<FakePlayer> switcher(controller, player, "/synthetic", [] { return true; });
    CHECK(switcher.Submit(controller.SelectCollection(CollectionId(catalog, "")), 0));
    CHECK(switcher.Submit(controller.Play(), 1));
    CHECK(switcher.Submit(controller.Pause(), 2));
    CHECK(switcher.Submit(controller.Play(), 3));
    CHECK(player.pauses == (std::vector<bool>{true, false}));
    CHECK(player.starts.size() == 1);
    CHECK(player.stops == 0);
    CHECK(switcher.Submit(controller.Next(), 4));
    CHECK(switcher.Submit(controller.SelectCollection(CollectionId(catalog, "series")), 5));
    player.running = false;
    CHECK(switcher.Poll(6));
    CHECK(player.starts.size() == 1);
    CHECK(!switcher.Pending());
    CHECK(switcher.Submit(controller.Play(), 7));
    CHECK(player.starts.back() == "/synthetic/series/episode1.mp3");
}

void TestTimeoutAndTerminalFailures() {
    const auto catalog = Catalog();
    {
        media::PlayerController controller(catalog);
        FakePlayer player;
        library::Switcher<FakePlayer> switcher(controller, player, "/synthetic", [] { return true; });
        CHECK(switcher.Submit(controller.SelectCollection(CollectionId(catalog, "")), 0));
        CHECK(switcher.Submit(controller.Play(), 1));
        CHECK(switcher.Submit(controller.Next(), 10));
        CHECK(switcher.Poll(2009));
        CHECK(switcher.Submit(controller.Next(), 2009));
        CHECK(!switcher.Poll(2010));  // New intent never extends the original stop deadline.
        CHECK(switcher.Failed());
        CHECK(!switcher.Pending());
        CHECK(player.stops == 1);
        player.running = false;
        CHECK(!switcher.Poll(2011));
        CHECK(!switcher.Submit(controller.Previous(), 2012));
        CHECK(player.starts.size() == 1);
        CHECK(player.stops == 1);
    }
    for (const bool reject_start : {true, false}) {
        media::PlayerController controller(catalog);
        FakePlayer player;
        player.accept_start = !reject_start;
        library::Switcher<FakePlayer> switcher(controller, player, "/synthetic", [] { return true; });
        CHECK(switcher.Submit(controller.SelectCollection(CollectionId(catalog, "")), 0));
        if (reject_start) {
            CHECK(!switcher.Submit(controller.Play(), 1));
        } else {
            CHECK(switcher.Submit(controller.Play(), 1));
            player.accept_pause = false;
            CHECK(!switcher.Submit(controller.Pause(), 2));
        }
        CHECK(switcher.Failed());
        CHECK(player.stops == 1);
        CHECK(!switcher.Poll(3));
        CHECK(player.starts.size() == 1);
        CHECK(player.stops == 1);
    }
    {
        media::PlayerController controller(catalog);
        FakePlayer player;
        library::Switcher<FakePlayer> switcher(controller, player, "/synthetic/../bad",
                                              [] { return true; });
        CHECK(switcher.Submit(controller.SelectCollection(CollectionId(catalog, "")), 0));
        CHECK(!switcher.Submit(controller.Play(), 1));
        CHECK(switcher.Failed());
        CHECK(player.starts.empty());
    }
    std::cout << "failure: 2000 ms stop timeout and player rejection stop permanently; no retry\n";
}

void TestOutputMustDrainWithinOriginalDeadline() {
    const auto catalog = Catalog();
    for (const bool release_before_deadline : {true, false}) {
        media::PlayerController controller(catalog);
        FakePlayer player;
        bool output_idle = true;
        library::Switcher<FakePlayer> switcher(controller, player, "/synthetic",
                                              [&] { return output_idle; });
        CHECK(switcher.Submit(controller.SelectCollection(CollectionId(catalog, "")), 0));
        CHECK(switcher.Submit(controller.Play(), 1));
        output_idle = false;
        CHECK(switcher.Submit(controller.Next(), 10));
        player.running = false;
        CHECK(switcher.Poll(11));
        CHECK(switcher.Pending());
        CHECK(player.starts.size() == 1);
        CHECK(switcher.Poll(2009));
        CHECK(player.starts.size() == 1);
        if (release_before_deadline) {
            output_idle = true;
            CHECK(switcher.Poll(2009));
            CHECK(player.starts.size() == 2);
            CHECK(player.starts.back() == "/synthetic/track2.mp3");
            CHECK(!switcher.Pending());
        } else {
            CHECK(!switcher.Poll(2010));
            CHECK(switcher.Failed());
            output_idle = true;
            CHECK(!switcher.Poll(2011));
            CHECK(player.starts.size() == 1);
        }
    }
    std::cout << "output: worker exit alone does not start the new track; queued output must drain\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        return 2;
    }
    TestScanAndStart(argv[1]);
    TestScanFiltering(argv[1]);
    TestScanFailures(argv[1]);
    TestSwitchWaitAndLatestIntent();
    TestPausedSelectionAndPendingPause();
    TestPauseResumeAndCollectionReset();
    TestTimeoutAndTerminalFailures();
    TestOutputMustDrainWithinOriginalDeadline();
    if (failures != 0) {
        std::cerr << failures << " failed checks\n";
        return 1;
    }
    std::cout << "M2 library scanner and switcher checks passed\n";
}
