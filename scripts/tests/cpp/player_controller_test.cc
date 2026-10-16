#include "media/player_controller.h"

#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using media::ControlStatus;
using media::PlaybackAction;
using media::PlaybackState;
using media::PlayMode;

int failure_count = 0;

void Check(bool condition, const char* expression, const char* file, int line) {
    if (!condition) {
        std::cerr << file << ':' << line << ": check failed: " << expression << '\n';
        ++failure_count;
    }
}

#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __FILE__, __LINE__)

void CheckResult(media::ControlResult result, ControlStatus status, PlaybackAction action,
                 const char* expression, const char* file, int line) {
    Check(result.status == status && result.action == action, expression, file, line);
}

#define CHECK_RESULT(expression, status, action) \
    CheckResult(expression, ControlStatus::status, PlaybackAction::action, #expression, __FILE__, \
                __LINE__)

media::MediaCatalog Fixture() {
    media::MediaCatalogBuilder builder("synthetic-card");
    for (const auto& file : {
             media::RawFile{u8"2_故事", u8"第10集.mp3", 110, 10},
             media::RawFile{u8"2_故事", u8"第2集.mp3", 102, 2},
             media::RawFile{u8"2_故事", u8"第1集.mp3", 101, 1},
             media::RawFile{"", u8"故事10.mp3", 210, 10},
             media::RawFile{"", u8"故事2.mp3", 202, 2},
             media::RawFile{u8"10_单曲", "only.mp3", 301, 1},
         }) {
        CHECK(builder.Add(file) == media::AddResult::kAdded);
    }
    auto result = builder.Finish();
    CHECK(result.Ok());
    return std::move(result.catalog);
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

void CheckCurrent(const media::PlayerController& controller, PlaybackState state,
                  const std::string& collection, const std::string& file) {
    CHECK(controller.State() == state);
    const auto current = controller.CurrentTrack();
    CHECK(current.has_value());
    if (current) {
        CHECK(current->directory_name == collection);
        CHECK(current->collection_is_root == collection.empty());
        CHECK(current->track.file_name == file);
    }
}

void TestEmptyAndNoSelection() {
    for (const auto& catalog : {media::MediaCatalog{}, Fixture()}) {
        media::PlayerController controller(catalog);
        CHECK(controller.State() == PlaybackState::kStopped);
        CHECK(controller.Mode() == PlayMode::kOrder);
        CHECK_RESULT(controller.SelectTrack(0), kNoSelection, kNone);
        CHECK_RESULT(controller.TrackFailed(), kNoSelection, kNone);
        CHECK(!controller.CurrentTrack());
        CHECK_RESULT(controller.Play(), kNoSelection, kNone);
        CHECK_RESULT(controller.Pause(), kNoSelection, kNone);
        CHECK_RESULT(controller.Previous(), kNoSelection, kNone);
        CHECK_RESULT(controller.Next(), kNoSelection, kNone);
        CHECK_RESULT(controller.TrackFinished(), kNoSelection, kNone);
        CHECK_RESULT(controller.SelectCollection({}), kUnknownCollection, kNone);
        CHECK_RESULT(controller.SelectCollection({1, 0}), kUnknownCollection, kNone);
        CHECK(controller.State() == PlaybackState::kStopped);
        CHECK(!controller.CurrentTrack());
    }
}

void TestPlayPauseAndNaturalNavigation() {
    auto catalog = Fixture();
    const auto series = CollectionId(catalog, u8"2_故事");
    media::PlayerController controller(std::move(catalog));
    CHECK_RESULT(controller.SelectCollection(series), kOk, kNone);
    CheckCurrent(controller, PlaybackState::kStopped, u8"2_故事", u8"第1集.mp3");
    CHECK_RESULT(controller.Pause(), kIgnored, kNone);
    CHECK_RESULT(controller.Play(), kOk, kStart);
    CHECK_RESULT(controller.Play(), kIgnored, kNone);
    CHECK_RESULT(controller.Pause(), kOk, kPause);
    CHECK_RESULT(controller.Pause(), kIgnored, kNone);
    CheckCurrent(controller, PlaybackState::kPaused, u8"2_故事", u8"第1集.mp3");
    CHECK_RESULT(controller.TrackFinished(), kIgnored, kNone);
    CHECK_RESULT(controller.Play(), kOk, kResume);
    CHECK_RESULT(controller.Next(), kOk, kStart);
    CheckCurrent(controller, PlaybackState::kPlaying, u8"2_故事", u8"第2集.mp3");
    CHECK_RESULT(controller.Next(), kOk, kStart);
    CheckCurrent(controller, PlaybackState::kPlaying, u8"2_故事", u8"第10集.mp3");
    CHECK_RESULT(controller.Previous(), kOk, kStart);
    CheckCurrent(controller, PlaybackState::kPlaying, u8"2_故事", u8"第2集.mp3");
}

void TestPausedAndStoppedNavigation() {
    const auto catalog = Fixture();
    const auto series = CollectionId(catalog, u8"2_故事");
    media::PlayerController controller(catalog);
    CHECK_RESULT(controller.SelectCollection(series), kOk, kNone);
    CHECK_RESULT(controller.Next(), kOk, kNone);
    CheckCurrent(controller, PlaybackState::kStopped, u8"2_故事", u8"第2集.mp3");
    CHECK_RESULT(controller.Previous(), kOk, kNone);
    CHECK_RESULT(controller.Play(), kOk, kStart);
    CHECK_RESULT(controller.Pause(), kOk, kPause);
    CHECK_RESULT(controller.Previous(), kAtBoundary, kNone);
    CHECK_RESULT(controller.Play(), kOk, kResume);
    CHECK_RESULT(controller.Pause(), kOk, kPause);

    // Every valid paused movement releases the old stream, including a second
    // movement before Play. Returning to an earlier track cannot resume it.
    CHECK_RESULT(controller.Next(), kOk, kStop);
    CheckCurrent(controller, PlaybackState::kPaused, u8"2_故事", u8"第2集.mp3");
    CHECK_RESULT(controller.Next(), kOk, kStop);
    CheckCurrent(controller, PlaybackState::kPaused, u8"2_故事", u8"第10集.mp3");
    CHECK_RESULT(controller.Next(), kAtBoundary, kNone);
    CHECK_RESULT(controller.Previous(), kOk, kStop);
    CHECK_RESULT(controller.Previous(), kOk, kStop);
    CheckCurrent(controller, PlaybackState::kPaused, u8"2_故事", u8"第1集.mp3");
    CHECK_RESULT(controller.TrackFinished(), kIgnored, kNone);
    CHECK_RESULT(controller.Play(), kOk, kStart);
}

void TestCollectionSelectionResetsStream() {
    const auto catalog = Fixture();
    const auto root = CollectionId(catalog, "");
    const auto series = CollectionId(catalog, u8"2_故事");
    media::PlayerController controller(catalog);
    CHECK_RESULT(controller.SelectCollection(root), kOk, kNone);
    CheckCurrent(controller, PlaybackState::kStopped, "", u8"故事2.mp3");
    CHECK_RESULT(controller.Play(), kOk, kStart);
    CHECK_RESULT(controller.SelectCollection(series), kOk, kStop);
    CheckCurrent(controller, PlaybackState::kStopped, u8"2_故事", u8"第1集.mp3");
    CHECK_RESULT(controller.Play(), kOk, kStart);
    CHECK_RESULT(controller.Next(), kOk, kStart);
    CHECK_RESULT(controller.SelectCollection(series), kOk, kStop);
    CheckCurrent(controller, PlaybackState::kStopped, u8"2_故事", u8"第1集.mp3");
    CHECK_RESULT(controller.Play(), kOk, kStart);
    CHECK_RESULT(controller.Pause(), kOk, kPause);
    CHECK_RESULT(controller.SelectCollection(root), kOk, kStop);
    CHECK_RESULT(controller.Play(), kOk, kStart);
    CHECK_RESULT(controller.Next(), kOk, kStart);
    CHECK_RESULT(controller.Pause(), kOk, kPause);
    CHECK_RESULT(controller.SelectCollection(root), kOk, kStop);
    CheckCurrent(controller, PlaybackState::kStopped, "", u8"故事2.mp3");
    CHECK_RESULT(controller.Next(), kOk, kNone);
    CHECK_RESULT(controller.SelectCollection(root), kOk, kNone);
    CheckCurrent(controller, PlaybackState::kStopped, "", u8"故事2.mp3");
    CHECK_RESULT(controller.Play(), kOk, kStart);
}

size_t CollectionIndex(const media::MediaCatalog& catalog, const std::string& name) {
    const auto& collections = catalog.Collections();
    for (size_t index = 0; index < collections.size(); ++index) {
        if (collections[index].directory_name == name) {
            return index;
        }
    }
    CHECK(false);
    return 0;
}

void TestNaturalEndPolicies() {
    const auto catalog = Fixture();
    const auto series = CollectionId(catalog, u8"2_故事");
    const auto index = CollectionIndex(catalog, u8"2_故事");
    media::PlayerController controller(catalog);
    CHECK_RESULT(controller.SelectCollection(series), kOk, kNone);
    CHECK_RESULT(controller.TrackFinished(), kIgnored, kNone);
    CHECK_RESULT(controller.Play(), kOk, kStart);
    CHECK_RESULT(controller.TrackFinished(), kOk, kStart);
    CheckCurrent(controller, PlaybackState::kPlaying, u8"2_故事", u8"第2集.mp3");
    CHECK_RESULT(controller.TrackFinished(), kOk, kStart);
    CheckCurrent(controller, PlaybackState::kPlaying, u8"2_故事", u8"第10集.mp3");
    CHECK_RESULT(controller.TrackFinished(), kOk, kStop);
    CheckCurrent(controller, PlaybackState::kStopped, u8"2_故事", u8"第10集.mp3");
    CHECK_RESULT(controller.TrackFinished(), kIgnored, kNone);
    CHECK_RESULT(controller.Play(), kOk, kStart);
    controller.SetMode(index, PlayMode::kLoop);
    CHECK(controller.Mode() == PlayMode::kLoop && controller.Mode(index) == PlayMode::kLoop);
    CheckCurrent(controller, PlaybackState::kPlaying, u8"2_故事", u8"第10集.mp3");
    const auto wrapped = controller.TrackFinished();  // Back to the first, by its bookmark.
    CHECK(wrapped.action == PlaybackAction::kStart && !wrapped.from_start);
    CheckCurrent(controller, PlaybackState::kPlaying, u8"2_故事", u8"第1集.mp3");
    CHECK_RESULT(controller.TrackFinished(), kOk, kStart);
    CHECK_RESULT(controller.TrackFinished(), kOk, kStart);
    CHECK_RESULT(controller.TrackFinished(), kOk, kStart);
    CheckCurrent(controller, PlaybackState::kPlaying, u8"2_故事", u8"第1集.mp3");
    controller.SetMode(index, PlayMode::kOrder);
    CHECK(controller.Mode() == PlayMode::kOrder);
    CHECK_RESULT(controller.TrackFinished(), kOk, kStart);
    CHECK_RESULT(controller.TrackFinished(), kOk, kStart);
    CHECK_RESULT(controller.TrackFinished(), kOk, kStop);
    // In order a failed track is skipped like a finished one.
    CHECK_RESULT(controller.Play(), kOk, kStart);
    CHECK_RESULT(controller.TrackFailed(), kOk, kStop);
}

void TestManualBoundariesAndSingleTrack() {
    const auto catalog = Fixture();
    const auto series = CollectionId(catalog, u8"2_故事");
    for (const auto state : {PlaybackState::kStopped, PlaybackState::kPlaying,
                             PlaybackState::kPaused}) {
        media::PlayerController controller(catalog);
        // Another collection's mode leaves this one in order.
        controller.SetMode(CollectionIndex(catalog, ""), PlayMode::kLoop);
        CHECK_RESULT(controller.SelectCollection(series), kOk, kNone);
        if (state != PlaybackState::kStopped) {
            CHECK_RESULT(controller.Play(), kOk, kStart);
        }
        if (state == PlaybackState::kPaused) {
            CHECK_RESULT(controller.Pause(), kOk, kPause);
        }
        CHECK_RESULT(controller.Previous(), kAtBoundary, kNone);
        CheckCurrent(controller, state, u8"2_故事", u8"第1集.mp3");
        const auto move_action = state == PlaybackState::kPlaying ? PlaybackAction::kStart
                               : state == PlaybackState::kPaused ? PlaybackAction::kStop
                                                                 : PlaybackAction::kNone;
        const auto second = controller.Next();
        CHECK(second.status == ControlStatus::kOk && second.action == move_action);
        const auto third = controller.Next();
        CHECK(third.status == ControlStatus::kOk && third.action == move_action);
        CHECK_RESULT(controller.Next(), kAtBoundary, kNone);
        CheckCurrent(controller, state, u8"2_故事", u8"第10集.mp3");
    }

    const auto only = CollectionIndex(catalog, u8"10_单曲");
    media::PlayerController single(catalog);
    CHECK_RESULT(single.SelectCollection(CollectionId(catalog, u8"10_单曲")), kOk, kNone);
    CHECK_RESULT(single.Previous(), kAtBoundary, kNone);
    CHECK_RESULT(single.Next(), kAtBoundary, kNone);
    CHECK_RESULT(single.Play(), kOk, kStart);
    CHECK_RESULT(single.TrackFinished(), kOk, kStop);
    CheckCurrent(single, PlaybackState::kStopped, u8"10_单曲", "only.mp3");
    // One track: every other mode plays it again and the keys stay put.
    for (const auto mode : {PlayMode::kLoop, PlayMode::kSingle, PlayMode::kShuffle}) {
        single.SetMode(only, mode);
        CHECK_RESULT(single.Play(), kOk, kStart);
        const auto again = single.TrackFinished();
        CHECK(again.action == PlaybackAction::kStart &&
              again.from_start == (mode != PlayMode::kLoop));
        CHECK_RESULT(single.TrackFinished(), kOk, kStart);
        CHECK_RESULT(single.TrackFailed(), kOk, kStart);
        CHECK_RESULT(single.Previous(), kAtBoundary, kNone);
        CHECK_RESULT(single.Next(), kAtBoundary, kNone);
        CheckCurrent(single, PlaybackState::kPlaying, u8"10_单曲", "only.mp3");
        CHECK_RESULT(single.Pause(), kOk, kPause);
        CHECK_RESULT(single.SelectCollection(CollectionId(catalog, u8"10_单曲")), kOk, kStop);
    }
}

void TestLoopAndSingleWrapTheKeys() {
    const auto catalog = Fixture();
    const auto series = CollectionId(catalog, u8"2_故事");
    const auto index = CollectionIndex(catalog, u8"2_故事");
    for (const auto mode : {PlayMode::kLoop, PlayMode::kSingle}) {
        for (const auto state :
             {PlaybackState::kStopped, PlaybackState::kPlaying, PlaybackState::kPaused}) {
            media::PlayerController controller(catalog);
            controller.SetMode(index, mode);
            CHECK_RESULT(controller.SelectCollection(series), kOk, kNone);
            if (state != PlaybackState::kStopped) {
                CHECK_RESULT(controller.Play(), kOk, kStart);
            }
            if (state == PlaybackState::kPaused) {
                CHECK_RESULT(controller.Pause(), kOk, kPause);
            }
            const auto action = state == PlaybackState::kPlaying  ? PlaybackAction::kStart
                                : state == PlaybackState::kPaused ? PlaybackAction::kStop
                                                                  : PlaybackAction::kNone;
            auto moved = controller.Previous();  // First to last...
            CHECK(moved.status == ControlStatus::kOk && moved.action == action &&
                  !moved.from_start);
            CheckCurrent(controller, state, u8"2_故事", u8"第10集.mp3");
            moved = controller.Next();  // ...and back.
            CHECK(moved.status == ControlStatus::kOk && moved.action == action);
            CheckCurrent(controller, state, u8"2_故事", u8"第1集.mp3");
            moved = controller.Next();
            CHECK(moved.status == ControlStatus::kOk && moved.action == action);
            CheckCurrent(controller, state, u8"2_故事", u8"第2集.mp3");
            if (state != PlaybackState::kPlaying) {
                // A key move follows the bookmark: the next Start is not forced to zero.
                const auto started = controller.Play();
                CHECK(started.action == PlaybackAction::kStart && !started.from_start);
            }
        }
    }
}

void TestSingleRepeatsAndSkipsAFailedTrack() {
    const auto catalog = Fixture();
    const auto series = CollectionId(catalog, u8"2_故事");
    media::PlayerController controller(catalog);
    controller.SetMode(CollectionIndex(catalog, u8"2_故事"), PlayMode::kSingle);
    CHECK(controller.Mode() == PlayMode::kOrder);  // Nothing selected yet.
    CHECK_RESULT(controller.SelectCollection(series), kOk, kNone);
    CHECK(controller.Mode() == PlayMode::kSingle);
    CHECK_RESULT(controller.Play(), kOk, kStart);
    for (int round = 0; round < 3; ++round) {
        const auto again = controller.TrackFinished();
        CHECK(again.status == ControlStatus::kOk && again.action == PlaybackAction::kStart &&
              again.from_start);
        CheckCurrent(controller, PlaybackState::kPlaying, u8"2_故事", u8"第1集.mp3");
    }
    // A track that cannot play is not retried: the next one repeats instead.
    CHECK_RESULT(controller.TrackFailed(), kOk, kStart);
    CheckCurrent(controller, PlaybackState::kPlaying, u8"2_故事", u8"第2集.mp3");
    CHECK_RESULT(controller.TrackFailed(), kOk, kStart);
    CHECK_RESULT(controller.TrackFailed(), kOk, kStart);  // Wraps around.
    CheckCurrent(controller, PlaybackState::kPlaying, u8"2_故事", u8"第1集.mp3");
    CHECK_RESULT(controller.TrackFinished(), kOk, kStart);
    CheckCurrent(controller, PlaybackState::kPlaying, u8"2_故事", u8"第1集.mp3");
    CHECK_RESULT(controller.Pause(), kOk, kPause);
    CHECK_RESULT(controller.TrackFinished(), kIgnored, kNone);
    CHECK_RESULT(controller.TrackFailed(), kIgnored, kNone);
}

media::MediaCatalog Songs(size_t count) {
    media::MediaCatalogBuilder builder("synthetic-songs");
    for (size_t index = 1; index <= count; ++index) {
        CHECK(builder.Add({"songs", "song" + std::to_string(index) + ".mp3", 100 + index, 1}) ==
              media::AddResult::kAdded);
    }
    CHECK(builder.Add({"story", "part1.mp3", 1, 1}) == media::AddResult::kAdded);
    auto result = builder.Finish();
    CHECK(result.Ok());
    return std::move(result.catalog);
}

size_t Current(const media::PlayerController& controller) {
    return controller.TrackNumber().first - 1;
}

// Every finished track moves through a random order of the others; the next
// round holds every track, never starting with the one just heard.
std::vector<size_t> Rounds(media::PlayerController& controller, size_t count, int rounds) {
    std::vector<size_t> heard{Current(controller)};
    for (int round = 0; round < rounds; ++round) {
        std::vector<int> times(count, 0);
        if (round == 0) {
            times[heard.front()] = 1;
        }
        const size_t steps = round == 0 ? count - 1 : count;
        for (size_t step = 0; step < steps; ++step) {
            const auto moved = controller.TrackFinished();
            CHECK(moved.status == ControlStatus::kOk && moved.action == PlaybackAction::kStart &&
                  moved.from_start);
            CHECK(Current(controller) != heard.back() || count == 1);
            heard.push_back(Current(controller));
            ++times[Current(controller)];
        }
        for (const int time : times) {
            CHECK(time == 1);
        }
    }
    return heard;
}

void TestShuffleRoundsAndKeys() {
    constexpr size_t kCount = 6;
    const auto catalog = Songs(kCount);
    const auto songs = CollectionId(catalog, "songs");
    const auto index = CollectionIndex(catalog, "songs");
    std::vector<std::vector<size_t>> orders;
    for (const uint32_t seed : {1u, 2u, 0u, 12345u}) {
        media::PlayerController controller(catalog);
        controller.Seed(seed);
        CHECK_RESULT(controller.SelectCollection(songs), kOk, kNone);
        CHECK_RESULT(controller.Next(), kOk, kNone);
        CHECK_RESULT(controller.Play(), kOk, kStart);
        controller.SetMode(index, PlayMode::kShuffle);  // The round starts at the second song.
        CHECK(Current(controller) == 1);
        const auto heard = Rounds(controller, kCount, 3);
        orders.push_back(heard);
        // The same seed gives the same order.
        media::PlayerController again(catalog);
        again.Seed(seed);
        CHECK_RESULT(again.SelectCollection(songs), kOk, kNone);
        CHECK_RESULT(again.SelectTrack(1), kOk, kNone);
        CHECK_RESULT(again.Play(), kOk, kStart);
        again.SetMode(index, PlayMode::kShuffle);
        CHECK(Rounds(again, kCount, 3) == heard);

        // Previous walks back through what was heard, across the round's start,
        // each from its beginning; Next walks forward the same way.
        for (size_t back = 1; back <= kCount; ++back) {
            const auto moved = controller.Previous();
            CHECK(moved.action == PlaybackAction::kStart && moved.from_start);
            CHECK(Current(controller) == heard[heard.size() - 1 - back]);
        }
        CHECK_RESULT(controller.Previous(), kAtBoundary, kNone);
        CHECK(Current(controller) == heard[heard.size() - 1 - kCount]);
        for (size_t forward = kCount; forward-- > 0;) {
            CHECK_RESULT(controller.Next(), kOk, kStart);
            CHECK(Current(controller) == heard[heard.size() - 1 - forward]);
        }
        // Past the round's end Next starts a new round, like a finished track.
        const auto last = Current(controller);
        CHECK_RESULT(controller.Next(), kOk, kStart);
        CHECK(Current(controller) != last);
        // A failed track is skipped to another random one.
        const auto failed = Current(controller);
        const auto skipped = controller.TrackFailed();
        CHECK(skipped.action == PlaybackAction::kStart && skipped.from_start);
        CHECK(Current(controller) != failed);
        // Paused or stopped, a move waits and the next Start begins at zero.
        CHECK_RESULT(controller.Pause(), kOk, kPause);
        CHECK_RESULT(controller.Next(), kOk, kStop);
        const auto resumed = controller.Play();
        CHECK(resumed.action == PlaybackAction::kStart && resumed.from_start);
        CHECK_RESULT(controller.Pause(), kOk, kPause);
        CHECK_RESULT(controller.Play(), kOk, kResume);
        CHECK_RESULT(controller.Pause(), kOk, kPause);
        CHECK_RESULT(controller.Next(), kOk, kStop);  // A shuffle move waits for Play...
        CHECK_RESULT(controller.SelectCollection(songs), kOk, kStop);  // ...but selecting wins.
        const auto first = controller.Play();  // Selecting is not a shuffle move.
        CHECK(first.action == PlaybackAction::kStart && !first.from_start);
        CHECK(Current(controller) == 0);
        Rounds(controller, kCount, 1);
        // Back to in order: the next track follows the current one.
        const auto at = Current(controller);
        controller.SetMode(index, PlayMode::kOrder);
        const auto next = controller.TrackFinished();
        CHECK(!next.from_start);
        if (at + 1 < kCount) {
            CHECK(next.action == PlaybackAction::kStart && Current(controller) == at + 1);
        } else {
            CHECK(next.action == PlaybackAction::kStop);
        }
    }
    CHECK(orders[0] != orders[1] || orders[0] != orders[3]);

    // A track chosen while shuffling starts a new round there.
    media::PlayerController controller(catalog);
    controller.SetMode(index, PlayMode::kShuffle);
    CHECK_RESULT(controller.SelectCollection(songs), kOk, kNone);
    CHECK_RESULT(controller.Play(), kOk, kStart);
    CHECK_RESULT(controller.TrackFinished(), kOk, kStart);
    CHECK_RESULT(controller.SelectTrack(4), kOk, kStart);
    CHECK(Current(controller) == 4);
    CHECK_RESULT(controller.Previous(), kAtBoundary, kNone);
    CHECK_RESULT(controller.Next(), kOk, kStart);  // Even the track playing now starts a round.
    const auto again = Current(controller);
    CHECK_RESULT(controller.SelectTrack(again), kOk, kStart);
    CHECK_RESULT(controller.Previous(), kAtBoundary, kNone);
    CHECK_RESULT(controller.SelectTrack(4), kOk, kStart);
    Rounds(controller, kCount, 2);
    // Another collection keeps its own (default) mode.
    CHECK_RESULT(controller.SelectCollection(CollectionId(catalog, "story")), kOk, kStop);
    CHECK(controller.Mode() == PlayMode::kOrder && controller.Mode(index) == PlayMode::kShuffle);
    CHECK(controller.Mode(99) == PlayMode::kOrder);
    controller.SetMode(99, PlayMode::kLoop);  // Ignored.
    CHECK(controller.Mode(99) == PlayMode::kOrder);
}

void TestSelectTrack() {
    const auto catalog = Fixture();
    const auto series = CollectionId(catalog, u8"2_故事");
    media::PlayerController controller(catalog);
    CHECK_RESULT(controller.SelectCollection(series), kOk, kNone);
    CHECK_RESULT(controller.SelectTrack(2), kOk, kNone);
    CheckCurrent(controller, PlaybackState::kStopped, u8"2_故事", u8"第10集.mp3");
    CHECK_RESULT(controller.SelectTrack(3), kAtBoundary, kNone);
    CheckCurrent(controller, PlaybackState::kStopped, u8"2_故事", u8"第10集.mp3");
    CHECK_RESULT(controller.Play(), kOk, kStart);
    CHECK_RESULT(controller.SelectTrack(1), kOk, kStart);
    CheckCurrent(controller, PlaybackState::kPlaying, u8"2_故事", u8"第2集.mp3");
    CHECK_RESULT(controller.Pause(), kOk, kPause);
    CHECK_RESULT(controller.SelectTrack(0), kOk, kStop);
    CheckCurrent(controller, PlaybackState::kPaused, u8"2_故事", u8"第1集.mp3");
    const auto started = controller.Play();
    CHECK(started.action == PlaybackAction::kStart && !started.from_start);
}

void TestInvalidIdsPreserveSelectionAndIntent() {
    const auto catalog = Fixture();
    const auto series = CollectionId(catalog, u8"2_故事");
    const auto track_id = catalog.Collections().front().tracks.front().id;
    for (const auto state : {PlaybackState::kStopped, PlaybackState::kPlaying,
                             PlaybackState::kPaused}) {
        media::PlayerController controller(catalog);
        CHECK_RESULT(controller.SelectCollection(series), kOk, kNone);
        CHECK_RESULT(controller.Next(), kOk, kNone);
        if (state != PlaybackState::kStopped) {
            CHECK_RESULT(controller.Play(), kOk, kStart);
        }
        if (state == PlaybackState::kPaused) {
            CHECK_RESULT(controller.Pause(), kOk, kPause);
        }
        for (const auto invalid : {media::MediaId{}, media::MediaId{1, 0}, track_id,
                                   media::MediaId{2, series.value}}) {
            CHECK_RESULT(controller.SelectCollection(invalid), kUnknownCollection, kNone);
            CheckCurrent(controller, state, u8"2_故事", u8"第2集.mp3");
        }
        if (state == PlaybackState::kPaused) {
            CHECK_RESULT(controller.Play(), kOk, kResume);
        } else if (state == PlaybackState::kStopped) {
            CHECK_RESULT(controller.Play(), kOk, kStart);
        } else {
            CHECK_RESULT(controller.Play(), kIgnored, kNone);
        }
    }
}

void TestCatalogAndTrackSnapshotOwnership() {
    std::optional<media::ResolvedTrack> saved;
    {
        auto source = Fixture();
        const auto series = CollectionId(source, u8"2_故事");
        media::PlayerController controller(source);
        source = media::MediaCatalog{};
        CHECK_RESULT(controller.SelectCollection(series), kOk, kNone);
        saved = controller.CurrentTrack();
        auto edited_snapshot = controller.CurrentTrack();
        CHECK(edited_snapshot.has_value());
        if (edited_snapshot) {
            edited_snapshot->directory_name = "edited";
            edited_snapshot->track.file_name = "edited.mp3";
        }
        CheckCurrent(controller, PlaybackState::kStopped, u8"2_故事", u8"第1集.mp3");
        CHECK_RESULT(controller.Next(), kOk, kNone);
        CheckCurrent(controller, PlaybackState::kStopped, u8"2_故事", u8"第2集.mp3");
    }
    CHECK(saved.has_value());
    if (saved) {
        CHECK(saved->directory_name == u8"2_故事");
        CHECK(saved->track.file_name == u8"第1集.mp3");
        CHECK(saved->track.size_bytes == 101);
        CHECK(saved->track.modified_time == 1);
    }
}

}  // namespace

int main() {
    TestEmptyAndNoSelection();
    TestPlayPauseAndNaturalNavigation();
    TestPausedAndStoppedNavigation();
    TestCollectionSelectionResetsStream();
    TestNaturalEndPolicies();
    TestManualBoundariesAndSingleTrack();
    TestLoopAndSingleWrapTheKeys();
    TestSingleRepeatsAndSkipsAFailedTrack();
    TestShuffleRoundsAndKeys();
    TestSelectTrack();
    TestInvalidIdsPreserveSelectionAndIntent();
    TestCatalogAndTrackSnapshotOwnership();
    if (failure_count != 0) {
        std::cerr << failure_count << " player controller test(s) failed\n";
        return 1;
    }
    std::cout << "player controller host tests passed\n";
    return 0;
}
