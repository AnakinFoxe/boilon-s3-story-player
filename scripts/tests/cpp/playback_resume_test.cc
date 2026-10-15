#include "media/playback_resume.h"
#include "resume_memory_store.h"

#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using media::CheckpointReason;
using media::PlaybackResume;
using media::ResumeMode;
using media::ResumeStatus;
using media::SaveStatus;

int failure_count = 0;

void Check(bool condition, const char* expression, const char* file, int line) {
    if (!condition) {
        std::cerr << file << ':' << line << ": check failed: " << expression << '\n';
        ++failure_count;
    }
}

#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __FILE__, __LINE__)

media::MediaCatalog Fixture(const std::string& library = "synthetic-card") {
    media::MediaCatalogBuilder builder(library);
    for (const auto& file : {
             media::RawFile{"", "root.mp3", 100, 1},
             media::RawFile{u8"故事", u8"第2集.mp3", 202, 2},
             media::RawFile{u8"故事", u8"第10集.mp3", 210, 10},
             media::RawFile{u8"音乐", "song.mp3", 300, 3},
         }) {
        CHECK(builder.Add(file) == media::AddResult::kAdded);
    }
    auto result = builder.Finish();
    CHECK(result.Ok());
    return std::move(result.catalog);
}

media::ResolvedTrack Track(const media::MediaCatalog& catalog, const std::string& directory) {
    for (const auto& collection : catalog.Collections()) {
        if (collection.directory_name == directory && !collection.tracks.empty()) {
            return *catalog.Resolve(collection.tracks.front().id);
        }
    }
    CHECK(false);
    return {};
}

media::ResumeRecord Record(const media::MediaCatalog& catalog) {
    return {1, "synthetic-card", Track(catalog, u8"故事"), 42000, 180000, 35};
}

void CheckRejected(const media::ResumePlan& plan, ResumeStatus status) {
    CHECK(plan.status == status);
    CHECK(!plan.target);
    CHECK(!plan.volume_percent);
    CHECK(!plan.auto_play);
    CHECK(plan.position_ms == 0);
}

void TestPeriodicBoundary() {
    const auto catalog = Fixture();
    const auto track = Track(catalog, u8"故事").track.id;
    MemoryResumeStore store;
    PlaybackResume resume(catalog, "synthetic-card", store);
    CHECK(!resume.LastSuccessfulSaveMs());
    CHECK(resume.Save(track, 1000, 180000, 30, 0, CheckpointReason::kPeriodic) == SaveStatus::kSaved);
    CHECK(resume.LastSuccessfulSaveMs() == std::optional<uint64_t>(0));
    CHECK(resume.Save(track, 30999, 180000, 30, 29999, CheckpointReason::kPeriodic) ==
          SaveStatus::kNotDue);
    CHECK(store.write_attempts == 1);
    CHECK(resume.Save(track, 31000, 180000, 30, 30000, CheckpointReason::kPeriodic) ==
          SaveStatus::kSaved);
    CHECK(store.write_attempts == 2);
    CHECK(store.records.size() == 1);
    CHECK(store.records.front().position_ms == 31000);
    CHECK(resume.LastSuccessfulSaveMs() == std::optional<uint64_t>(30000));
}

void TestForcedSavesAndUnchanged() {
    const auto catalog = Fixture();
    const auto track = Track(catalog, u8"故事").track.id;
    MemoryResumeStore store;
    PlaybackResume resume(catalog, "synthetic-card", store);
    CHECK(resume.Save(track, 0, 180000, 30, 0, CheckpointReason::kPeriodic) == SaveStatus::kSaved);
    uint64_t now = 0;
    for (const auto reason : {CheckpointReason::kPause, CheckpointReason::kTrackChange,
                              CheckpointReason::kCollectionChange, CheckpointReason::kShutdown}) {
        ++now;
        CHECK(resume.Save(track, now * 1000, 180000, 30, now, reason) == SaveStatus::kSaved);
        const auto attempts = store.write_attempts;
        CHECK(resume.Save(track, now * 1000, 180000, 30, now, reason) == SaveStatus::kUnchanged);
        CHECK(store.write_attempts == attempts);
        CHECK(resume.LastSuccessfulSaveMs() == std::optional<uint64_t>(now));
    }
    CHECK(store.write_attempts == 5);
    CHECK(resume.Save(track, 4000, 180000, 30, 30004, CheckpointReason::kPeriodic) ==
          SaveStatus::kUnchanged);
    CHECK(store.write_attempts == 5);
    CHECK(resume.LastSuccessfulSaveMs() == std::optional<uint64_t>(4));
}

void TestWriteFailureAndRateLimit() {
    const auto catalog = Fixture();
    const auto track = Track(catalog, u8"故事").track.id;
    MemoryResumeStore store;
    PlaybackResume resume(catalog, "synthetic-card", store);
    CHECK(resume.Save(track, 20000, 180000, 30, 0, CheckpointReason::kPeriodic) == SaveStatus::kSaved);
    store.write_error = true;
    CHECK(resume.Save(track, 50000, 180000, 30, 30000, CheckpointReason::kPeriodic) ==
          SaveStatus::kStorageError);
    CHECK(store.write_attempts == 2);
    CHECK(resume.LastSuccessfulSaveMs() == std::optional<uint64_t>(0));
    CHECK(store.records.size() == 1);
    CHECK(store.records.front().position_ms == 20000);
    const auto old_point = resume.Restore(std::nullopt, ResumeMode::kBoot);
    CHECK(old_point.status == ResumeStatus::kReady);
    CHECK(old_point.position_ms == 15000);
    CHECK(store.write_attempts == 2);
    CHECK(resume.Save(track, 50001, 180000, 30, 30001, CheckpointReason::kPeriodic) ==
          SaveStatus::kNotDue);
    CHECK(store.write_attempts == 2);
    store.write_error = false;
    CHECK(resume.Save(track, 50002, 180000, 30, 30002, CheckpointReason::kPause) == SaveStatus::kSaved);
    CHECK(store.write_attempts == 3);
    CHECK(resume.LastSuccessfulSaveMs() == std::optional<uint64_t>(30002));

    MemoryResumeStore initially_failing;
    initially_failing.write_error = true;
    PlaybackResume first(catalog, "synthetic-card", initially_failing);
    CHECK(first.Save(track, 0, 180000, 30, 0, CheckpointReason::kPeriodic) == SaveStatus::kStorageError);
    CHECK(!first.LastSuccessfulSaveMs());
    CHECK(first.Save(track, 29999, 180000, 30, 29999, CheckpointReason::kPeriodic) == SaveStatus::kNotDue);
    CHECK(initially_failing.write_attempts == 1);
    initially_failing.write_error = false;
    CHECK(first.Save(track, 30000, 180000, 30, 30000, CheckpointReason::kPeriodic) == SaveStatus::kSaved);
    CHECK(initially_failing.write_attempts == 2);
}

void TestReconstructionAndIndependentDirectories() {
    const auto catalog = Fixture();
    const auto story = Track(catalog, u8"故事");
    const auto music = Track(catalog, u8"音乐");
    const auto root = Track(catalog, "");
    MemoryResumeStore store;
    {
        PlaybackResume resume(catalog, "synthetic-card", store);
        CHECK(resume.Save(story.track.id, 65000, 180000, 20, 0, CheckpointReason::kCollectionChange) ==
              SaveStatus::kSaved);
        CHECK(resume.Save(root.track.id, 1000, 180000, 25, 1, CheckpointReason::kCollectionChange) ==
              SaveStatus::kSaved);
        CHECK(resume.Save(music.track.id, 42000, 180000, 60, 2, CheckpointReason::kPause) == SaveStatus::kSaved);
    }
    const auto other_catalog = Fixture("other-card");
    PlaybackResume other(other_catalog, "other-card", store);
    CHECK(other.Save(Track(other_catalog, "").track.id, 90000, 180000, 90, 0,
                     CheckpointReason::kShutdown) == SaveStatus::kSaved);

    PlaybackResume rebooted(catalog, "synthetic-card", store);
    const auto boot = rebooted.Restore(std::nullopt, ResumeMode::kBoot);
    CHECK(boot.status == ResumeStatus::kReady);
    CHECK(boot.target && boot.target->track.id == music.track.id);
    CHECK(boot.position_ms == 37000);
    CHECK(boot.volume_percent == std::optional<int>(60));
    CHECK(boot.auto_play);
    for (const auto& entry : std::vector<std::pair<media::ResolvedTrack, uint64_t>>{
             {story, 65000}, {root, 1000}, {music, 42000}}) {
        const auto exact = rebooted.Restore(entry.first.collection_id, ResumeMode::kSavedExact);
        CHECK(exact.status == ResumeStatus::kReady);
        CHECK(exact.target && exact.target->track.id == entry.first.track.id);
        CHECK(exact.position_ms == entry.second);
        CHECK(!exact.volume_percent);
        CHECK(!exact.auto_play);
    }
    CHECK(store.write_attempts == 4);
    CHECK(!rebooted.LastSuccessfulSaveMs());
}

void TestRewindFloorAndVolumeBoundaries() {
    const auto catalog = Fixture();
    MemoryResumeStore store;
    PlaybackResume resume(catalog, "synthetic-card", store);
    auto record = Record(catalog);
    for (const auto& example : std::vector<std::pair<uint64_t, uint64_t>>{
             {0, 0}, {4999, 0}, {5000, 0}, {5001, 1}, {180000, 175000}}) {
        for (const auto volume : {0, 100}) {
            record.position_ms = example.first;
            record.volume_percent = volume;
            store.records = {record};
            const auto boot = resume.Restore(std::nullopt, ResumeMode::kBoot);
            CHECK(boot.status == ResumeStatus::kReady);
            CHECK(boot.position_ms == example.second);
            CHECK(boot.volume_percent == std::optional<int>(volume));
            CHECK(boot.auto_play);
        }
    }
    CHECK(store.write_attempts == 0);
}

// Simulate a backend returning mismatched record fields, including an indexed
// library/collection mismatch that the normal memory fake would filter out.
class ReturnedRecordStore : public MemoryResumeStore {
public:
    media::ResumeRead Read(const std::string&, std::optional<media::MediaId>,
                           media::ResumeRecord& out) const override {
        out = records.front();
        return media::ResumeRead::kFound;
    }
};

void TestInvalidRecordFields() {
    const auto catalog = Fixture();
    const auto original = Record(catalog);
    ReturnedRecordStore store;
    PlaybackResume resume(catalog, "synthetic-card", store);
    const std::vector<std::function<void(media::ResumeRecord&)>> mutations = {
        [](auto& record) { record.version = 2; },
        [](auto& record) { record.library_key = "other-card"; },
        [](auto& record) { record.media.track.id.version = 2; },
        [](auto& record) { record.media.collection_id.version = 0; },
        [](auto& record) { record.duration_ms = 0; },
        [](auto& record) { record.position_ms = record.duration_ms + 1; },
        [](auto& record) { record.volume_percent = -1; },
        [](auto& record) { record.volume_percent = 101; },
    };
    for (const auto& mutate : mutations) {
        store.records = {original};
        mutate(store.records.front());
        CheckRejected(resume.Restore(std::nullopt, ResumeMode::kBoot), ResumeStatus::kInvalidRecord);
    }
    store.records = {original};
    CheckRejected(resume.Restore(Track(catalog, u8"音乐").collection_id, ResumeMode::kSavedExact),
                  ResumeStatus::kInvalidRecord);
    CHECK(store.write_attempts == 0);
}

void TestStaleIdentityAndFingerprint() {
    const auto catalog = Fixture();
    const auto original = Record(catalog);
    MemoryResumeStore store;
    PlaybackResume resume(catalog, "synthetic-card", store);
    const std::vector<std::function<void(media::ResumeRecord&)>> mutations = {
        [](auto& record) { record.media.track.file_name = "../other.mp3"; },
        [](auto& record) { record.media.directory_name = "/wrong"; },
        [](auto& record) { ++record.media.track.size_bytes; },
        [](auto& record) { ++record.media.track.modified_time; },
        [](auto& record) { record.media.collection_is_root = true; },
        [](auto& record) { record.media.track.id.value = 0; },
        [&](auto& record) { record.media.track.id = Track(catalog, u8"音乐").track.id; },
        [&](auto& record) { record.media.collection_id = Track(catalog, "").collection_id; },
    };
    for (const auto& mutate : mutations) {
        store.records = {original};
        mutate(store.records.front());
        CheckRejected(resume.Restore(std::nullopt, ResumeMode::kBoot), ResumeStatus::kStaleMedia);
    }
    store.records = {original};
    PlaybackResume empty(media::MediaCatalog{}, "synthetic-card", store);
    CheckRejected(empty.Restore(std::nullopt, ResumeMode::kBoot), ResumeStatus::kStaleMedia);
    media::MediaCatalogBuilder rescanned("synthetic-card");
    CHECK(rescanned.Add({original.media.directory_name, original.media.track.file_name,
                         original.media.track.size_bytes, original.media.track.modified_time + 1}) ==
          media::AddResult::kAdded);
    auto changed_catalog = rescanned.Finish();
    CHECK(changed_catalog.Ok());
    PlaybackResume changed(std::move(changed_catalog.catalog), "synthetic-card", store);
    CheckRejected(changed.Restore(std::nullopt, ResumeMode::kBoot), ResumeStatus::kStaleMedia);
    CHECK(store.records.front().position_ms == original.position_ms);
    CHECK(store.write_attempts == 0);
}

void TestMissingAndReadErrors() {
    const auto catalog = Fixture();
    MemoryResumeStore store;
    PlaybackResume resume(catalog, "synthetic-card", store);
    CheckRejected(resume.Restore(std::nullopt, ResumeMode::kBoot), ResumeStatus::kMissing);
    store.records = {Record(catalog)};
    CheckRejected(resume.Restore(Track(catalog, u8"音乐").collection_id, ResumeMode::kSavedExact),
                  ResumeStatus::kMissing);
    PlaybackResume other(catalog, "other-card", store);
    CheckRejected(other.Restore(std::nullopt, ResumeMode::kBoot), ResumeStatus::kMissing);
    store.read_error = true;
    CheckRejected(resume.Restore(std::nullopt, ResumeMode::kBoot), ResumeStatus::kStorageError);
    CHECK(store.records.size() == 1);
    CHECK(store.write_attempts == 0);
}

void TestInvalidSaveInputAndTime() {
    const auto catalog = Fixture();
    const auto track = Track(catalog, u8"故事").track.id;
    MemoryResumeStore store;
    PlaybackResume resume(catalog, "synthetic-card", store);
    CHECK(resume.Save({}, 0, 100, 30, 0, CheckpointReason::kPause) == SaveStatus::kInvalidInput);
    CHECK(resume.Save(track, 101, 100, 30, 0, CheckpointReason::kPause) == SaveStatus::kInvalidInput);
    CHECK(resume.Save(track, 0, 0, 30, 0, CheckpointReason::kPause) == SaveStatus::kInvalidInput);
    for (const auto volume : {-1, 101}) {
        CHECK(resume.Save(track, 0, 100, volume, 0, CheckpointReason::kPause) == SaveStatus::kInvalidInput);
    }
    for (const auto& key : {std::string{}, std::string(257, 'x')}) {
        PlaybackResume invalid(catalog, key, store);
        CHECK(invalid.Save(track, 0, 100, 30, 0, CheckpointReason::kPause) == SaveStatus::kInvalidInput);
        CheckRejected(invalid.Restore(std::nullopt, ResumeMode::kBoot), ResumeStatus::kInvalidRecord);
    }
    PlaybackResume empty(media::MediaCatalog{}, "synthetic-card", store);
    CHECK(empty.Save(track, 0, 100, 30, 0, CheckpointReason::kPause) == SaveStatus::kInvalidInput);
    CHECK(store.write_attempts == 0);
    CHECK(!resume.LastSuccessfulSaveMs());
    CHECK(resume.Save(track, 0, 100, 30, 100, CheckpointReason::kPause) == SaveStatus::kSaved);
    CHECK(resume.Save(track, 1, 100, 30, 99, CheckpointReason::kPause) == SaveStatus::kInvalidTime);
    CHECK(resume.Save(track, 20, 100, 30, 120, CheckpointReason::kPeriodic) == SaveStatus::kNotDue);
    CHECK(resume.Save(track, 19, 100, 30, 119, CheckpointReason::kPause) == SaveStatus::kInvalidTime);
    CHECK(store.write_attempts == 1);
    CHECK(resume.LastSuccessfulSaveMs() == std::optional<uint64_t>(100));
    CHECK(resume.Save(track, 20, 100, 30, 120, CheckpointReason::kPause) == SaveStatus::kSaved);
    CHECK(store.write_attempts == 2);
}

void TestBoundedStoreFullAndUpdate() {
    media::CatalogLimits limits;
    limits.max_collections = 65;
    media::MediaCatalogBuilder builder("synthetic-card", limits);
    for (int index = 0; index < 65; ++index) {
        CHECK(builder.Add({"series" + std::to_string(index), "only.mp3", 100, 1}) == media::AddResult::kAdded);
    }
    auto result = builder.Finish();
    CHECK(result.Ok());
    const auto& catalog = result.catalog;
    CHECK(catalog.Collections().size() == 65);
    MemoryResumeStore store;
    PlaybackResume resume(catalog, "synthetic-card", store);
    for (size_t index = 0; index < 64; ++index) {
        CHECK(resume.Save(catalog.Collections()[index].tracks.front().id, 0, 180000, 30, index,
                          CheckpointReason::kCollectionChange) == SaveStatus::kSaved);
    }
    CHECK(store.records.size() == 64);
    CHECK(resume.Save(catalog.Collections()[64].tracks.front().id, 0, 180000, 30, 64,
                      CheckpointReason::kCollectionChange) == SaveStatus::kFull);
    CHECK(store.write_attempts == 65);
    CHECK(store.records.size() == 64);
    CHECK(resume.LastSuccessfulSaveMs() == std::optional<uint64_t>(63));
    const auto previous = resume.Restore(std::nullopt, ResumeMode::kBoot);
    CHECK(previous.target && previous.target->collection_id == catalog.Collections()[63].id);
    CHECK(resume.Save(catalog.Collections()[0].tracks.front().id, 1000, 180000, 40, 65,
                      CheckpointReason::kCollectionChange) == SaveStatus::kSaved);
    CHECK(store.records.size() == 64);
    CHECK(store.write_attempts == 66);
    const auto updated = resume.Restore(std::nullopt, ResumeMode::kBoot);
    CHECK(updated.target && updated.target->collection_id == catalog.Collections()[0].id);
    CHECK(updated.volume_percent == std::optional<int>(40));
    CHECK(resume.Restore(catalog.Collections()[1].id, ResumeMode::kSavedExact).status == ResumeStatus::kReady);
}

}  // namespace

int main() {
    TestPeriodicBoundary();
    TestForcedSavesAndUnchanged();
    TestWriteFailureAndRateLimit();
    TestReconstructionAndIndependentDirectories();
    TestRewindFloorAndVolumeBoundaries();
    TestInvalidRecordFields();
    TestStaleIdentityAndFingerprint();
    TestMissingAndReadErrors();
    TestInvalidSaveInputAndTime();
    TestBoundedStoreFullAndUpdate();
    if (failure_count != 0) {
        std::cerr << failure_count << " failed checks\n";
        return 1;
    }
    std::cout << "playback resume: 10 scenario groups passed\n";
    return 0;
}
