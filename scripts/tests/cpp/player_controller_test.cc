#include "media/player_controller.h"

#include <iostream>
#include <optional>
#include <string>
#include <utility>

namespace {

using media::ControlStatus;
using media::EndPolicy;
using media::PlaybackAction;
using media::PlaybackState;

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
        CHECK(controller.GetEndPolicy() == EndPolicy::kStop);
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

void TestNaturalEndPolicies() {
    const auto catalog = Fixture();
    const auto series = CollectionId(catalog, u8"2_故事");
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
    controller.SetEndPolicy(EndPolicy::kRepeatCollection);
    CHECK(controller.GetEndPolicy() == EndPolicy::kRepeatCollection);
    CheckCurrent(controller, PlaybackState::kPlaying, u8"2_故事", u8"第10集.mp3");
    CHECK_RESULT(controller.TrackFinished(), kOk, kStart);
    CheckCurrent(controller, PlaybackState::kPlaying, u8"2_故事", u8"第1集.mp3");
    CHECK_RESULT(controller.TrackFinished(), kOk, kStart);
    CHECK_RESULT(controller.TrackFinished(), kOk, kStart);
    CHECK_RESULT(controller.TrackFinished(), kOk, kStart);
    CheckCurrent(controller, PlaybackState::kPlaying, u8"2_故事", u8"第1集.mp3");
    controller.SetEndPolicy(EndPolicy::kStop);
    CHECK(controller.GetEndPolicy() == EndPolicy::kStop);
    CHECK_RESULT(controller.TrackFinished(), kOk, kStart);
    CHECK_RESULT(controller.TrackFinished(), kOk, kStart);
    CHECK_RESULT(controller.TrackFinished(), kOk, kStop);
}

void TestManualBoundariesAndSingleTrack() {
    const auto catalog = Fixture();
    const auto series = CollectionId(catalog, u8"2_故事");
    for (const auto state : {PlaybackState::kStopped, PlaybackState::kPlaying,
                             PlaybackState::kPaused}) {
        media::PlayerController controller(catalog);
        controller.SetEndPolicy(EndPolicy::kRepeatCollection);
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

    media::PlayerController single(catalog);
    CHECK_RESULT(single.SelectCollection(CollectionId(catalog, u8"10_单曲")), kOk, kNone);
    CHECK_RESULT(single.Previous(), kAtBoundary, kNone);
    CHECK_RESULT(single.Next(), kAtBoundary, kNone);
    CHECK_RESULT(single.Play(), kOk, kStart);
    CHECK_RESULT(single.TrackFinished(), kOk, kStop);
    CheckCurrent(single, PlaybackState::kStopped, u8"10_单曲", "only.mp3");
    single.SetEndPolicy(EndPolicy::kRepeatCollection);
    CHECK_RESULT(single.Play(), kOk, kStart);
    CHECK_RESULT(single.TrackFinished(), kOk, kStart);
    CHECK_RESULT(single.TrackFinished(), kOk, kStart);
    CHECK_RESULT(single.Previous(), kAtBoundary, kNone);
    CHECK_RESULT(single.Next(), kAtBoundary, kNone);
    CheckCurrent(single, PlaybackState::kPlaying, u8"10_单曲", "only.mp3");
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
    TestInvalidIdsPreserveSelectionAndIntent();
    TestCatalogAndTrackSnapshotOwnership();
    if (failure_count != 0) {
        std::cerr << failure_count << " player controller test(s) failed\n";
        return 1;
    }
    std::cout << "player controller host tests passed\n";
    return 0;
}
