#include "boards/boilon-s3-gen2/m2_library_runtime.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace {

namespace library = boilon::local_library;
int failures = 0;

void Check(bool condition, const char* expression, int line) {
    if (!condition) {
        std::cerr << "line " << line << ": " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

struct FakePlayer {
    enum class Result { kIdle, kRunning, kFinished, kStopped, kError };
    Result result = Result::kIdle;
    bool running = false;
    bool accept_start = true;
    bool accept_pause = true;
    bool read_failed = false;
    int stops = 0;
    std::vector<std::string> starts;
    std::vector<bool> pauses;

    bool Start(const std::string& path) {
        CHECK(!running);
        starts.push_back(path);
        running = accept_start;
        result = accept_start ? Result::kRunning : Result::kError;
        read_failed = false;
        return accept_start;
    }
    bool Pause(bool value) {
        CHECK(running);
        pauses.push_back(value);
        return accept_pause;
    }
    void Stop() { ++stops; }
    bool Running() const { return running; }
    Result LastResult() const { return result; }
    bool ReadFailed() const { return read_failed; }
    // read: the card could not open or read the file (kError only).
    void End(Result value = Result::kFinished, bool read = false) {
        running = false;
        result = value;
        read_failed = read;
    }
};

struct WorkerInterleave : FakePlayer {
    mutable bool finish_error_after_running_read = false;
    bool Running() const {
        bool observed = running;
        if (finish_error_after_running_read) {
            finish_error_after_running_read = false;
            const_cast<WorkerInterleave*>(this)->End(Result::kError);
        }
        return observed;
    }
};

media::MediaCatalog Catalog() {
    media::MediaCatalogBuilder builder("synthetic-library");
    for (const auto* name : {"track10.mp3", "track2.mp3", "track1.mp3"}) {
        CHECK(builder.Add({"", name, 10, 1}) == media::AddResult::kAdded);
    }
    CHECK(builder.Add({"series", "episode1.mp3", 10, 1}) == media::AddResult::kAdded);
    auto result = builder.Finish();
    CHECK(result.Ok());
    return std::move(result.catalog);
}

void TestAutoplayAndNaturalEnd() {
    {
        WorkerInterleave player;
        library::Runtime<WorkerInterleave> runtime(Catalog(), player, [] { return true; });
        CHECK(runtime.Start(0));
        CHECK(runtime.Tick(0, 0));
        CHECK(runtime.Tick(0, 30));
        CHECK(runtime.Tick(media::PlayerGestures::kDown, 31));
        CHECK(runtime.Tick(media::PlayerGestures::kDown, 61));
        CHECK(runtime.Tick(media::PlayerGestures::kDown, 861));
        CHECK(player.stops == 1);
        player.finish_error_after_running_read = true;
        // Old worker publishes error between the two Running observations: the
        // switch already asked for still starts the chosen track, never the one after.
        CHECK(runtime.Tick(0, 862));
        CHECK(runtime.Tick(0, 863));
        CHECK(player.starts.size() == 2 && player.starts.back() == "/sdcard/track2.mp3");
        CHECK(runtime.Controller().CurrentTrack()->track.file_name == "track2.mp3");
    }
    FakePlayer player;
    library::Runtime<FakePlayer> runtime(Catalog(), player, [] { return true; });
    CHECK(runtime.Start(0));
    CHECK(player.starts.size() == 1);
    CHECK(runtime.Controller().CurrentTrack()->track.file_name == "track1.mp3");
    player.End();
    CHECK(runtime.Tick(0, 1));
    CHECK(player.starts.size() == 2);
    CHECK(runtime.Controller().CurrentTrack()->track.file_name == "track2.mp3");
    CHECK(!runtime.Finished());
    CHECK(runtime.Tick(0, 2));
    CHECK(player.starts.size() == 2);  // One EOF cannot advance twice.
    player.End();
    CHECK(runtime.Tick(0, 3));
    CHECK(player.starts.size() == 3);
    player.End();
    CHECK(runtime.Tick(0, 4));
    CHECK(runtime.Finished());
    CHECK(player.starts.size() == 3);  // Does not cross into the next collection.
    std::cout << "runtime: autoplay track1.mp3 -> natural EOF -> track2.mp3\n";
}

class Rig {
public:
    Rig()
        : runtime(
              Catalog(), player, [&] { return output_idle; },
              [&](const char* event, const media::PlayerController& controller) {
                  const auto track = controller.CurrentTrack();
                  events.push_back(std::string(event) + ":" +
                                   (track ? track->track.file_name : ""));
              }) {
        CHECK(runtime.Start(now));
        CHECK(runtime.Tick(mask, now));
        Advance(30);
    }
    void Advance(uint64_t count) {
        for (uint64_t elapsed = 0; elapsed < count; ++elapsed) {
            CHECK(runtime.Tick(mask, ++now));
        }
    }
    void Edge(uint8_t next) {
        mask = next;
        CHECK(runtime.Tick(mask, now));
        Advance(30);
    }
    void TapPower() {
        Edge(media::PlayerGestures::kPower);
        Advance(70);
        Edge(0);
        Advance(media::PlayerGestures::kDoubleMs + 1);
    }
    void DoublePower() {
        Edge(media::PlayerGestures::kPower);
        Advance(70);
        Edge(0);
        Advance(70);
        Edge(media::PlayerGestures::kPower);
        Advance(70);
        Edge(0);
        CHECK(runtime.LastGesture() == media::GestureAction::kOfflineHint);
    }
    void LongTrack(uint8_t key) {
        Edge(key);
        Advance(media::PlayerGestures::kTrackHoldMs);
        Edge(0);
    }

    FakePlayer player;
    bool output_idle = true;
    std::vector<std::string> events;
    library::Runtime<FakePlayer> runtime;
    uint64_t now = 0;
    uint8_t mask = 0;
};

void TestVirtualPauseAndResume() {
    Rig rig;
    rig.TapPower();
    CHECK(rig.runtime.Controller().State() == media::PlaybackState::kPaused);
    CHECK(rig.player.pauses == std::vector<bool>{true});
    rig.TapPower();
    CHECK(rig.runtime.Controller().State() == media::PlaybackState::kPlaying);
    CHECK(rig.player.pauses == (std::vector<bool>{true, false}));
    CHECK(rig.player.starts.size() == 1);
    CHECK(rig.player.stops == 0);
    CHECK(rig.events == (std::vector<std::string>{"REQUEST:track1.mp3", "START:track1.mp3",
                                                  "PAUSE:track1.mp3", "RESUME:track1.mp3"}));
    std::cout << "keys: top short press pauses; another short press resumes the same stream\n";
}

void TestOfflineHintPreservesPlaybackAndExpires() {
    for (const bool paused : {false, true}) {
        Rig rig;
        if (paused)
            rig.TapPower();
        const auto before = rig.runtime.View(rig.now);
        const auto state = rig.runtime.Controller().State();
        const auto player = rig.player;
        const auto events = rig.events;
        rig.DoublePower();
        const auto hinted = rig.runtime.View(rig.now);
        CHECK(hinted.offline_hint);
        CHECK(hinted.title == before.title && hinted.collection == before.collection);
        CHECK(hinted.directory_open == before.directory_open &&
              hinted.highlighted == before.highlighted && hinted.directories == before.directories);
        CHECK(rig.runtime.Controller().State() == state);
        CHECK(rig.player.starts == player.starts && rig.player.pauses == player.pauses &&
              rig.player.stops == player.stops && rig.events == events);
        rig.Advance(1999);
        CHECK(rig.runtime.View(rig.now).offline_hint);
        rig.Advance(1);
        CHECK(!rig.runtime.View(rig.now).offline_hint &&
              rig.runtime.View(rig.now).state ==
                  (paused ? library::PlayerState::kPaused : library::PlayerState::kPlaying));
        CHECK(rig.runtime.Controller().State() == state);
        CHECK(rig.player.starts == player.starts && rig.player.pauses == player.pauses &&
              rig.player.stops == player.stops && rig.events == events);
    }
}

void TestOfflineHintRefreshes() {
    Rig rig;
    rig.DoublePower();
    const auto first = rig.now;
    rig.Advance(500);
    rig.DoublePower();
    const auto second = rig.now;
    CHECK(second > first && second < first + 2000);
    rig.Advance(first + 2000 - rig.now);
    CHECK(rig.runtime.View(rig.now).offline_hint);
    rig.Advance(second + 1999 - rig.now);
    CHECK(rig.runtime.View(rig.now).offline_hint);
    rig.Advance(1);
    CHECK(!rig.runtime.View(rig.now).offline_hint &&
          rig.runtime.View(rig.now).state == library::PlayerState::kPlaying);
    CHECK(rig.player.starts.size() == 1 && rig.player.pauses.empty() && rig.player.stops == 0);
    CHECK(rig.events == (std::vector<std::string>{"REQUEST:track1.mp3", "START:track1.mp3"}));
}

void TestOfflineHintDoesNotCoverDirectoryOrFailure() {
    {
        Rig rig;
        rig.DoublePower();
        const auto hint = rig.now;
        rig.Edge(media::PlayerGestures::kUp | media::PlayerGestures::kDown);
        rig.Advance(70);
        rig.Edge(0);
        const auto before = rig.runtime.View(rig.now);
        CHECK(before.directory_open && !before.offline_hint &&
              before.state == library::PlayerState::kPlaying);
        rig.Advance(100);
        const auto visible = rig.runtime.View(rig.now);
        CHECK(rig.now < hint + 2000);  // Directory wins even while the hint is still active.
        CHECK(visible.directory_open && !visible.offline_hint &&
              visible.state == library::PlayerState::kPlaying);
        CHECK(visible.highlighted == before.highlighted &&
              visible.directories == before.directories);
        CHECK(visible.title == before.title && visible.collection == before.collection);
        CHECK(rig.player.starts.size() == 1 && rig.player.pauses.empty() && rig.player.stops == 0);
        CHECK(rig.events == (std::vector<std::string>{"REQUEST:track1.mp3", "START:track1.mp3"}));
    }
    {
        Rig rig;  // The card fails: the error view, never the hint.
        rig.DoublePower();
        for (int file = 1; file < library::kMaxFailedFiles; ++file) {
            rig.player.End(FakePlayer::Result::kError, true);
            CHECK(rig.runtime.Tick(0, ++rig.now));
        }
        rig.player.End(FakePlayer::Result::kError, true);
        CHECK(!rig.runtime.Tick(0, ++rig.now));
        const auto failed = rig.runtime.View(rig.now);
        CHECK(failed.failed && failed.state == library::PlayerState::kFailed);
        CHECK(failed.title == "track10.mp3" && rig.player.starts.size() == 3);
    }
}

void TestObservedManualSwitches() {
    Rig rig;
    CHECK(rig.events == (std::vector<std::string>{"REQUEST:track1.mp3", "START:track1.mp3"}));
    rig.LongTrack(media::PlayerGestures::kUp);  // First-track boundary.
    CHECK(rig.events.size() == 2);
    rig.LongTrack(media::PlayerGestures::kDown);
    CHECK(rig.events.back() == "REQUEST:track2.mp3");
    CHECK(rig.events.size() == 3);  // A request is not a successful Start.
    rig.player.End();               // Cancelled old worker races with natural EOF.
    rig.Advance(1);
    CHECK(rig.events.back() == "START:track2.mp3");
    CHECK(rig.events.size() == 4);  // Cancelled EOF is not current-track EOF.
    rig.LongTrack(media::PlayerGestures::kUp);
    CHECK(rig.events.back() == "REQUEST:track1.mp3");
    rig.player.End(FakePlayer::Result::kStopped);
    rig.Advance(1);
    CHECK(rig.events ==
          (std::vector<std::string>{"REQUEST:track1.mp3", "START:track1.mp3", "REQUEST:track2.mp3",
                                    "START:track2.mp3", "REQUEST:track1.mp3", "START:track1.mp3"}));
    rig.Advance(10);
    CHECK(rig.events.size() == 6);
}

void TestObservedNaturalEofAndBoundaries() {
    Rig rig;
    rig.player.End();
    rig.Advance(1);
    CHECK(rig.events ==
          (std::vector<std::string>{"REQUEST:track1.mp3", "START:track1.mp3", "EOF:track1.mp3",
                                    "REQUEST:track2.mp3", "START:track2.mp3"}));
    rig.Advance(10);
    CHECK(rig.events.size() == 5);
    rig.player.End();
    rig.Advance(1);
    CHECK(rig.events.size() == 8 && rig.events[5] == "EOF:track2.mp3" &&
          rig.events[6] == "REQUEST:track10.mp3" && rig.events[7] == "START:track10.mp3");
    rig.LongTrack(media::PlayerGestures::kDown);  // Last-track boundary.
    CHECK(rig.events.size() == 8);
    rig.player.End();
    rig.Advance(1);
    CHECK(rig.runtime.Finished());
    CHECK(rig.events.size() == 10 && rig.events[8] == "EOF:track10.mp3" &&
          rig.events[9] == "STOP:track10.mp3");
    rig.Advance(10);
    CHECK(rig.events.size() == 10);
}

void TestAsyncSwitchAndCancelledEof() {
    Rig rig;
    rig.LongTrack(media::PlayerGestures::kDown);
    CHECK(rig.runtime.Controller().CurrentTrack()->track.file_name == "track2.mp3");
    CHECK(rig.player.starts.size() == 1);
    CHECK(rig.player.stops == 1);
    rig.LongTrack(media::PlayerGestures::kDown);
    CHECK(rig.runtime.Controller().CurrentTrack()->track.file_name == "track10.mp3");
    CHECK(rig.player.stops == 1);
    rig.player.End();  // Old worker raced cancellation and reported EOF.
    rig.Advance(1);
    CHECK(rig.player.starts.size() == 2);
    CHECK(rig.runtime.Controller().CurrentTrack()->track.file_name == "track10.mp3");
    CHECK(!rig.runtime.Finished());
    rig.LongTrack(media::PlayerGestures::kUp);
    rig.player.End(FakePlayer::Result::kStopped);
    rig.Advance(1);
    CHECK(rig.runtime.Controller().CurrentTrack()->track.file_name == "track2.mp3");
    CHECK(rig.player.starts.size() == 3);
    std::cout
        << "keys: long up/down selects tracks; cancelled old EOF does not skip another track\n";
}

void TestPauseWhileSwitchingAndOutputDrain() {
    Rig rig;
    rig.output_idle = false;
    rig.LongTrack(media::PlayerGestures::kDown);
    rig.TapPower();
    CHECK(rig.runtime.Controller().State() == media::PlaybackState::kPaused);
    CHECK(rig.player.pauses.empty());
    rig.player.End(FakePlayer::Result::kStopped);
    rig.Advance(1);
    CHECK(rig.player.starts.size() == 1);
    rig.output_idle = true;
    rig.Advance(1);
    CHECK(rig.player.starts.size() == 1);
    rig.TapPower();
    CHECK(rig.player.starts.size() == 2);
    CHECK(rig.runtime.Controller().CurrentTrack()->track.file_name == "track2.mp3");
    CHECK(rig.player.pauses.empty());
}

void TestPausedEndDoesNotResumeDeadDecoder() {
    Rig rig;
    rig.TapPower();
    rig.player.End();
    rig.Advance(1);
    CHECK(rig.runtime.Controller().State() == media::PlaybackState::kPaused);
    CHECK(rig.player.starts.size() == 1);
    rig.TapPower();
    CHECK(rig.runtime.Controller().State() == media::PlaybackState::kPlaying);
    CHECK(rig.runtime.Controller().CurrentTrack()->track.file_name == "track2.mp3");
    CHECK(rig.player.starts.size() == 2);
    CHECK(rig.player.pauses == std::vector<bool>{true});
    rig.player.End();
    rig.Advance(1);
    CHECK(rig.runtime.Controller().CurrentTrack()->track.file_name == "track10.mp3");
    rig.TapPower();
    rig.player.End();
    rig.Advance(1);
    rig.TapPower();
    CHECK(rig.runtime.Finished());
    CHECK(rig.player.starts.size() == 3);
    CHECK(rig.player.pauses == (std::vector<bool>{true, true}));
    std::cout << "EOF while paused: Play advances or ends; never resumes the exited decoder\n";
}

// P5-5: a file the decoder cannot play is skipped like a finished one.
void TestFailedFilesAreSkipped() {
    {
        Rig rig;  // One bad file: the next one plays.
        rig.player.End(FakePlayer::Result::kError);
        rig.Advance(1);
        CHECK(rig.player.starts.size() == 2 && rig.player.starts.back() == "/sdcard/track2.mp3");
        CHECK(rig.runtime.Controller().State() == media::PlaybackState::kPlaying);
        CHECK(rig.events ==
              (std::vector<std::string>{"REQUEST:track1.mp3", "START:track1.mp3", "SKIP:track1.mp3",
                                        "REQUEST:track2.mp3", "START:track2.mp3"}));
        // A file that plays to its end clears the count; a bad last file then ends
        // the directory like a finished one, without an error.
        rig.player.End();
        rig.Advance(1);
        CHECK(rig.player.starts.back() == "/sdcard/track10.mp3");
        rig.player.End(FakePlayer::Result::kError);
        rig.Advance(1);
        CHECK(rig.runtime.Finished() && !rig.runtime.View(rig.now).failed);
        CHECK(rig.player.starts.size() == 3 && rig.events.back() == "STOP:track10.mp3");
    }
    {
        Rig rig;  // Paused when it failed: Play starts that file again, then it is skipped.
        rig.TapPower();
        rig.player.End(FakePlayer::Result::kError);
        rig.Advance(1);
        CHECK(rig.runtime.Controller().State() == media::PlaybackState::kPaused);
        CHECK(rig.player.starts.size() == 1 && !rig.runtime.View(rig.now).failed);
        rig.TapPower();
        CHECK(rig.player.starts.size() == 2 && rig.player.starts.back() == "/sdcard/track1.mp3");
        rig.player.End(FakePlayer::Result::kError);
        rig.Advance(1);
        CHECK(rig.player.starts.size() == 3 && rig.player.starts.back() == "/sdcard/track2.mp3");
    }
    {
        Rig rig;  // Failed while a switch waits for the output: the chosen track starts.
        rig.output_idle = false;
        rig.LongTrack(media::PlayerGestures::kDown);
        CHECK(rig.player.stops == 1);
        rig.player.End(FakePlayer::Result::kError);
        rig.Advance(10);
        CHECK(rig.player.starts.size() == 1);
        rig.output_idle = true;
        rig.Advance(1);
        CHECK(rig.player.starts.size() == 2 && rig.player.starts.back() == "/sdcard/track2.mp3");
        CHECK(std::find(rig.events.begin(), rig.events.end(), "SKIP:track1.mp3") ==
              rig.events.end());
    }
    {
        // kMaxFailedFiles bad files in a row (2026-10-13, after a directory of
        // bad files locked the player on the device): paused on 「播放错误」,
        // and the keys, the directory list and the AI's picks still work.
        Rig rig;
        for (int file = 1; file < library::kMaxFailedFiles; ++file) {
            rig.player.End(FakePlayer::Result::kError);
            rig.Advance(1);
        }
        const auto stops = rig.player.stops;
        rig.player.End(FakePlayer::Result::kError);
        CHECK(rig.runtime.Tick(0, ++rig.now));
        // The decoder has already ended: nothing to stop.
        CHECK(rig.player.starts.size() == 3 && rig.player.stops == stops);
        CHECK(rig.events.size() >= 2 && rig.events[rig.events.size() - 2] == "SKIP:track10.mp3" &&
              rig.events.back() == "STOP:track10.mp3");
        CHECK(rig.runtime.Controller().State() == media::PlaybackState::kPaused);
        auto view = rig.runtime.View(rig.now);
        CHECK(!view.failed && view.state == library::PlayerState::kFailed);
        CHECK(view.title == "track10.mp3" && !rig.runtime.Finished());
        rig.Advance(3000);  // Nothing retries by itself.
        CHECK(rig.player.starts.size() == 3);
        CHECK(rig.runtime.View(rig.now).state == library::PlayerState::kFailed);
        // Play tries that file again with a fresh count: a bad last file ends
        // the directory like a finished one.
        rig.TapPower();
        CHECK(rig.player.starts.size() == 4 && rig.player.starts.back() == "/sdcard/track10.mp3");
        CHECK(rig.runtime.View(rig.now).state == library::PlayerState::kPlaying);
        rig.player.End(FakePlayer::Result::kError);
        rig.Advance(1);
        CHECK(rig.runtime.Finished() &&
              rig.runtime.View(rig.now).state == library::PlayerState::kFinished);
        // The directory list opens and the AI's pick plays.
        rig.Edge(static_cast<uint8_t>(media::PlayerGestures::kUp | media::PlayerGestures::kDown));
        rig.Advance(70);
        rig.Edge(0);
        CHECK(rig.runtime.View(rig.now).directory_open);
        view = rig.runtime.View(rig.now);
        const auto series = static_cast<size_t>(
            std::find(view.directories.begin(), view.directories.end(), "series") -
            view.directories.begin());
        CHECK(series < view.directories.size());
        CHECK(rig.runtime.Choose(series, 0, rig.now));
        rig.Advance(1);
        CHECK(rig.player.starts.back() == "/sdcard/series/episode1.mp3");
    }
    {
        Rig rig;  // A step to another file clears the mark without playing.
        for (int file = 0; file < library::kMaxFailedFiles; ++file) {
            rig.player.End(FakePlayer::Result::kError);
            rig.Advance(1);
        }
        CHECK(rig.runtime.View(rig.now).state == library::PlayerState::kFailed);
        rig.LongTrack(media::PlayerGestures::kUp);  // Previous
        CHECK(rig.runtime.Controller().CurrentTrack()->track.file_name == "track2.mp3");
        CHECK(rig.runtime.View(rig.now).state == library::PlayerState::kPaused);
        CHECK(rig.player.starts.size() == 3);
    }
    {
        // A file played to its end also clears a failure the card caused.
        Rig rig;
        rig.player.End(FakePlayer::Result::kError, true);  // track1
        rig.Advance(1);
        rig.player.End();  // track2 plays to its end.
        rig.Advance(1);
        for (int file = 0; file < library::kMaxFailedFiles; ++file) {
            rig.player.End(FakePlayer::Result::kError);  // track10, then twice again
            CHECK(rig.runtime.Tick(0, ++rig.now));
            if (file + 1 < library::kMaxFailedFiles) {
                CHECK(rig.runtime.Finished());
                rig.TapPower();
            }
        }
        const auto view = rig.runtime.View(rig.now);
        CHECK(!view.failed && view.state == library::PlayerState::kFailed);
        CHECK(rig.player.starts.size() == 5 && rig.player.starts.back() == "/sdcard/track10.mp3");
    }
    for (int read = 0; read < library::kMaxFailedFiles; ++read) {
        // One file the card could not open or read is enough for the error.
        Rig rig;
        for (int file = 0; file < library::kMaxFailedFiles; ++file) {
            rig.player.End(FakePlayer::Result::kError, file == read);
            CHECK(rig.runtime.Tick(0, ++rig.now) == (file + 1 < library::kMaxFailedFiles));
        }
        CHECK(rig.runtime.View(rig.now).failed);
    }
    {
        // kMaxFailedFiles in a row the card could not read (a pulled card)
        // stop the player for good.
        Rig rig;
        for (int file = 1; file < library::kMaxFailedFiles; ++file) {
            rig.player.End(FakePlayer::Result::kError, true);
            rig.Advance(1);
        }
        CHECK(rig.player.starts.size() == 3);
        const auto starts = rig.player.starts;
        const auto pauses = rig.player.pauses;
        const auto stops = rig.player.stops;
        const auto track = rig.runtime.Controller().CurrentTrack()->track.id;
        const auto before = rig.runtime.View(rig.now);
        rig.player.End(FakePlayer::Result::kError, true);
        CHECK(!rig.runtime.Tick(0, ++rig.now));
        CHECK(rig.events.back() == "SKIP:track10.mp3");
        const auto events = rig.events;
        const auto error_stops = rig.player.stops;
        CHECK(error_stops == stops + 1);
        // Even held keys and elapsed drain/gesture deadlines cannot revive a failed runtime.
        for (const auto key :
             {uint8_t{0}, media::PlayerGestures::kPower, media::PlayerGestures::kUp,
              media::PlayerGestures::kDown,
              static_cast<uint8_t>(media::PlayerGestures::kUp | media::PlayerGestures::kDown),
              uint8_t{0}}) {
            CHECK(!rig.runtime.Tick(key, ++rig.now));
            rig.now += 3000;
            CHECK(!rig.runtime.Tick(key, rig.now));
            CHECK(rig.player.stops == error_stops);
            CHECK(rig.player.starts == starts);
            CHECK(rig.player.pauses == pauses);
            CHECK(rig.events == events);  // No EOF, REQUEST, START or playback notification.
            CHECK(rig.runtime.Controller().CurrentTrack()->track.id == track);
            CHECK(rig.runtime.LastGesture() == media::GestureAction::kNone);
            CHECK(!rig.runtime.Finished());
            const auto failed_view = rig.runtime.View(rig.now);
            CHECK(failed_view.failed && failed_view.state == library::PlayerState::kFailed);
            CHECK(failed_view.title == before.title && failed_view.collection == before.collection);
            CHECK(failed_view.volume == before.volume);
        }
    }
    // A refused start is not a bad file: it stops at once.
    FakePlayer refusing;
    refusing.accept_start = false;
    library::Runtime<FakePlayer> failed(Catalog(), refusing, [] { return true; });
    CHECK(!failed.Start(0));
    CHECK(!failed.Tick(0, 1));
    CHECK(refusing.starts.size() == 1);
    std::cout << "bad files: skipped like EOF; 3 in a row pause on 播放错误, still usable; "
                 "a card that cannot read stops; a refused start stops at once\n";
}

void TestUnavailableGesturesLeavePlaybackAlone() {
    Rig rig;
    // Volume and directory gestures leave playback alone; long power is diagnostic only.
    for (const auto key :
         {media::PlayerGestures::kUp, media::PlayerGestures::kDown,
          static_cast<uint8_t>(media::PlayerGestures::kUp | media::PlayerGestures::kDown)}) {
        rig.Edge(key);
        rig.Advance(70);
        rig.Edge(0);
    }
    rig.Edge(media::PlayerGestures::kPower);
    rig.Advance(media::PlayerGestures::kPowerHoldMs);
    rig.Edge(0);
    CHECK(rig.player.starts.size() == 1);
    CHECK(rig.player.pauses.empty());
    CHECK(rig.player.stops == 0);
    CHECK(!rig.runtime.Finished());
}

}  // namespace

// P5-1: the view's screen details follow the runtime.
void TestScreenViewFields() {
    using G = media::PlayerGestures;
    Rig rig;
    auto view = rig.runtime.View(rig.now);
    CHECK(view.state == library::PlayerState::kPlaying && !view.offline_hint);
    CHECK(view.track_number == 1 && view.track_count == 3);
    CHECK(view.directory_files == (std::vector<size_t>{3, 1}) && view.seek_seconds == 0);
    rig.DoublePower();
    CHECK(rig.runtime.View(rig.now).offline_hint);
    rig.Advance(2000);
    CHECK(!rig.runtime.View(rig.now).offline_hint);
    // Hold the top key and tap the middle key twice: back 30 s, then 60 s in total.
    rig.Edge(G::kPower);
    rig.Edge(G::kPower | G::kUp);
    rig.Edge(G::kPower);
    CHECK(rig.runtime.View(rig.now).seek_seconds == -30);
    rig.Edge(G::kPower | G::kUp);
    rig.Edge(G::kPower);
    rig.Edge(0);
    CHECK(rig.runtime.View(rig.now).seek_seconds == -60);
    rig.Advance(2000);
    CHECK(rig.runtime.View(rig.now).seek_seconds == 0);
    rig.TapPower();
    CHECK(rig.runtime.View(rig.now).state == library::PlayerState::kPaused);
    // The directory list counts down to its own close.
    rig.Edge(G::kUp | G::kDown);
    rig.Advance(70);
    rig.Edge(0);
    view = rig.runtime.View(rig.now);
    CHECK(view.directory_open && view.directory_left_ms > 4800 && view.directory_left_ms <= 5000);
    rig.Advance(4000);
    view = rig.runtime.View(rig.now);
    CHECK(view.directory_open && view.directory_left_ms <= 1000);
    std::cout << "screen: state, track number, files, seek total and list countdown follow the runtime\n";
}

int main() {
    TestAutoplayAndNaturalEnd();
    TestVirtualPauseAndResume();
    TestOfflineHintPreservesPlaybackAndExpires();
    TestOfflineHintRefreshes();
    TestOfflineHintDoesNotCoverDirectoryOrFailure();
    TestObservedManualSwitches();
    TestObservedNaturalEofAndBoundaries();
    TestAsyncSwitchAndCancelledEof();
    TestPauseWhileSwitchingAndOutputDrain();
    TestPausedEndDoesNotResumeDeadDecoder();
    TestFailedFilesAreSkipped();
    TestUnavailableGesturesLeavePlaybackAlone();
    TestScreenViewFields();
    if (failures) {
        std::cerr << failures << " failed checks\n";
        return 1;
    }
    std::cout << "M2 library runtime checks passed\n";
}
