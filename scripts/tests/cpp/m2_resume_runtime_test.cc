#include "boards/boilon-s3-gen2/m2_resume_runtime.h"
#include "boards/boilon-s3-gen2/m2_library_runtime.h"
#include "boards/boilon-s3-gen2/m2_resume_store.h"

#include <algorithm>
#include <cassert>
#include <deque>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace library = boilon::local_library;
using Reason = media::CheckpointReason;

namespace {
media::MediaCatalog Catalog() {
    media::MediaCatalogBuilder builder("runtime-card");
    for (const auto* name : {"track1.mp3", "track2.mp3", "track10.mp3"})
        assert(builder.Add({"", name, 1000, 1}) == media::AddResult::kAdded);
    for (const auto* name : {"episode1.mp3", "episode2.mp3"})
        assert(builder.Add({"series", name, 2000, 2}) == media::AddResult::kAdded);
    auto built = builder.Finish();
    assert(built.Ok());
    return std::move(built.catalog);
}

struct MemoryBlob : library::ResumeBlob {
    std::vector<uint8_t> bytes;
    bool write_error = false;
    mutable int reads = 0;
    int writes = 0;
    media::ResumeRead Read(std::vector<uint8_t>& out) const override {
        ++reads;
        if (bytes.empty())
            return media::ResumeRead::kMissing;
        out = bytes;
        return media::ResumeRead::kFound;
    }
    bool Write(const std::vector<uint8_t>& in) override {
        ++writes;
        if (write_error)
            return false;
        bytes = in;
        return true;
    }
};

struct Checkpoint {
    media::ResolvedTrack track;
    uint64_t position, duration;
    int volume;
    uint64_t now;
    Reason reason;
};

// Only Pump touches storage/policy: Runtime gets an asynchronous, owner-safe port.
class PumpPort : public library::ResumePort {
public:
    explicit PumpPort(MemoryBlob& blob)
        : legacy(legacy_blob),
          store(blob, legacy, Catalog()),
          policy(Catalog(), "runtime-card", store) {}
    std::optional<uint64_t> KnownPosition(media::MediaId track) override {
        for (const auto& item : store.Positions("runtime-card"))
            if (item.first == track)
                return item.second;
        return uint64_t{0};
    }
    uint64_t KnownDuration(media::MediaId track) override {
        return known_track && *known_track == track ? known_duration : 0;
    }
    bool Restore(std::optional<media::MediaId> collection, media::ResumeMode mode) override {
        restores.emplace_back(collection, mode);
        queue.emplace_back(
            [this, collection, mode] { response = policy.Restore(collection, mode); });
        return true;
    }
    bool TakeRestore(media::ResumePlan& plan) override {
        if (!response)
            return false;
        plan = *response;
        response.reset();
        return true;
    }
    bool Checkpoint(const media::ResolvedTrack& track, uint64_t position, uint64_t duration,
                    int volume, uint64_t now, Reason reason) override {
        checkpoints.push_back({track, position, duration, volume, now, reason});
        if (!accept_checkpoints)
            return false;
        queue.emplace_back([this, track, position, duration, volume, now, reason] {
            known_track = track.track.id;
            known_duration = duration;
            saved.push_back(policy.Save(track.track.id, position, duration, volume, now, reason));
        });
        return true;
    }
    void Pump() {
        while (!queue.empty()) {
            auto next = std::move(queue.front());
            queue.pop_front();
            next();
        }
    }
    size_t Count(Reason reason) const {
        return std::count_if(checkpoints.begin(), checkpoints.end(),
                             [reason](const auto& item) { return item.reason == reason; });
    }
    const ::Checkpoint& Last(Reason reason) const {
        for (auto item = checkpoints.rbegin(); item != checkpoints.rend(); ++item)
            if (item->reason == reason)
                return *item;
        assert(false);
        return checkpoints.back();
    }
    MemoryBlob legacy_blob;
    library::BlobResumeStore legacy;
    library::FileBookmarks store;
    std::optional<media::MediaId> known_track;
    uint64_t known_duration = 0;
    media::PlaybackResume policy;
    bool accept_checkpoints = true;
    std::deque<std::function<void()>> queue;
    std::optional<media::ResumePlan> response;
    std::vector<std::pair<std::optional<media::MediaId>, media::ResumeMode>> restores;
    std::vector<::Checkpoint> checkpoints;
    std::vector<media::SaveStatus> saved;
};

struct FakePlayer {
    enum class Result { kIdle, kRunning, kFinished, kStopped, kError };
    Result result = Result::kIdle;
    bool running = false;
    bool transient_zero = false;
    uint64_t position = 0, duration = 0;
    int stops = 0, seeks = 0;
    std::vector<std::pair<std::string, uint64_t>> starts;
    std::vector<bool> pauses;
    std::function<void()> on_start;
    bool Start(const std::string& path) { return Launch(path, 0); }
    bool StartAt(const std::string& path, uint64_t offset) {
        ++seeks;
        return Launch(path, offset);
    }
    bool Launch(const std::string& path, uint64_t offset) {
        assert(!running);
        if (on_start)
            on_start();
        starts.emplace_back(path, offset);
        running = true;
        result = Result::kRunning;
        read_failed = false;
        position = transient_zero ? 0 : offset;
        duration = 0;
        return true;
    }
    bool Pause(bool value) {
        pauses.push_back(value);
        position = 0;  // Sentinel: checkpoint must snapshot before the control mutation.
        return true;
    }
    void Stop() {
        ++stops;
        position = 0;
    }
    bool Running() const { return running; }
    Result LastResult() const { return result; }
    bool ReadFailed() const { return read_failed; }
    uint64_t PositionMs() const { return position; }
    uint64_t DurationMs() const { return duration; }
    // read: the card could not open or read the file (kError only).
    void End(Result value = Result::kStopped, bool read = false) {
        running = false;
        result = value;
        read_failed = read;
    }
    bool read_failed = false;
};

class Rig {
public:
    explicit Rig(MemoryBlob& input, bool transient_zero = false)
        : blob(input),
          port(input),
          runtime(
              Catalog(), player, [&] { return output_idle; },
              [&](const char* event, const media::PlayerController&) {
                  events.emplace_back(event);
              },
              &port,
              [&](int volume) {
                  restored_volumes.push_back(volume);
                  order.emplace_back("VOLUME:" + std::to_string(volume));
              }) {
        player.transient_zero = transient_zero;
        player.on_start = [&] { order.emplace_back("START"); };
        const int reads = blob.reads, writes = blob.writes;
        assert(runtime.Start(now));
        assert(player.starts.empty() && blob.reads == reads && blob.writes == writes);
        Tick();
        assert(player.starts.empty() && port.restores.size() == 1);
        assert(!port.restores[0].first && port.restores[0].second == media::ResumeMode::kBoot);
        port.Pump();
        Tick();
        Advance(31);
        assert(player.starts.size() == 1);
    }
    void Tick() {
        const int reads = blob.reads, writes = blob.writes;
        assert(runtime.Tick(mask, now));
        assert(blob.reads == reads && blob.writes == writes);
        if (runtime.LastGesture() != media::GestureAction::kNone)
            gestures.push_back(runtime.LastGesture());
    }
    void Advance(uint64_t elapsed) {
        now += elapsed;
        Tick();
    }
    void Edge(uint8_t next) {
        mask = next;
        Tick();
        Advance(30);
    }
    void Tap(uint8_t key) {
        Edge(key);
        Advance(70);
        Edge(0);
    }
    void Pause() {
        Tap(media::PlayerGestures::kPower);
        Advance(media::PlayerGestures::kDoubleMs + 1);
    }
    void Long(uint8_t key) {
        Edge(key);
        Advance(media::PlayerGestures::kTrackHoldMs);
        Edge(0);
    }
    void Drain() {
        player.End();
        port.Pump();
        Advance(1);
        Advance(31);
    }
    void ChooseOther() {
        Tap(media::PlayerGestures::kUp | media::PlayerGestures::kDown);
        assert(runtime.View(now).directory_open);
        Advance(1);  // First observe the new context, then settle all-released debounce.
        Advance(30);
        const auto first = runtime.View(now).highlighted;
        Tap(media::PlayerGestures::kUp);
        assert(runtime.View(now).highlighted != first);
        Tap(media::PlayerGestures::kPower);
        assert(!runtime.View(now).directory_open);
    }
    MemoryBlob& blob;
    PumpPort port;
    FakePlayer player;
    bool output_idle = true;
    std::vector<std::string> events;
    std::vector<std::string> order;
    std::vector<int> restored_volumes;
    std::vector<media::GestureAction> gestures;
    library::Runtime<FakePlayer> runtime;
    uint64_t now = 0;
    uint8_t mask = 0;
};

void BootReconstruction() {
    MemoryBlob first;
    std::vector<uint8_t> persisted;
    {
        Rig rig(first);
        rig.Long(media::PlayerGestures::kDown);
        rig.Drain();
        assert(rig.runtime.View(rig.now).title == "track2.mp3");
        for (int i = 0; i < 5; ++i)
            rig.Tap(media::PlayerGestures::kUp);
        assert(rig.runtime.View(rig.now).volume == 35);
        rig.player.position = 42000;
        rig.player.duration = 180000;
        rig.Pause();
        const auto& checkpoint = rig.port.Last(Reason::kPause);
        assert(checkpoint.track.track.file_name == "track2.mp3" && checkpoint.position == 42000);
        assert(checkpoint.duration == 180000 && checkpoint.volume == 35);
        assert(first.writes == 0);
        rig.port.Pump();
        assert(rig.port.saved.back() == media::SaveStatus::kSaved);
        persisted = first.bytes;
    }
    MemoryBlob fresh;
    fresh.bytes =
        persisted;  // Only serialized bytes survive; no old policy/player/runtime objects.
    Rig boot(fresh, true);
    assert(boot.player.seeks == 1 && boot.player.starts.back().first == "/sdcard/track2.mp3");
    assert(boot.player.starts.back().second == 37000);
    assert(boot.runtime.View(boot.now).title == "track2.mp3" &&
           boot.runtime.View(boot.now).volume == 35);
    assert(boot.runtime.Controller().State() == media::PlaybackState::kPlaying);
    assert(fresh.writes == 0);
    assert(boot.restored_volumes == std::vector<int>{35});
    assert(boot.order == (std::vector<std::string>{"VOLUME:35", "START"}));
    assert(boot.player.PositionMs() == 0 && boot.port.checkpoints.empty());
    assert(boot.runtime.View(boot.now).state == library::PlayerState::kLocating);
    boot.player.duration =
        180000;  // Even trusted duration must not turn the transient zero into a save.
    boot.Advance(10);
    boot.port.Pump();
    assert(fresh.bytes == persisted && fresh.writes == 0 && boot.port.checkpoints.empty());
    boot.Pause();
    boot.port.Pump();
    assert(boot.runtime.Controller().State() == media::PlaybackState::kPaused);
    assert(boot.runtime.View(boot.now).state == library::PlayerState::kPaused);
    assert(fresh.bytes == persisted && fresh.writes == 0 && boot.port.checkpoints.empty());
    boot.Pause();
    assert(boot.runtime.View(boot.now).state == library::PlayerState::kLocating);
    boot.player.position = 37048;  // First completed output, rather than decoded/queued data.
    boot.Advance(1);
    assert(boot.port.Count(Reason::kTrackChange) == 1);
    assert(boot.port.Last(Reason::kTrackChange).position == 37048);
    assert(boot.runtime.View(boot.now).state == library::PlayerState::kPlaying);
    assert(fresh.bytes == persisted);  // Still asynchronous until Pump.
    boot.port.Pump();
    media::ResumeRecord updated;
    assert(boot.port.store.Read("runtime-card", {}, updated) == media::ResumeRead::kFound);
    assert(updated.position_ms == 37048 && updated.volume_percent == 35);
    boot.Advance(1);
    boot.port.Pump();
    assert(boot.port.Count(Reason::kTrackChange) == 1 && fresh.writes == 1);
}

void PeriodicAndShutdown() {
    MemoryBlob blob;
    Rig rig(blob);
    rig.player.position = 42000;
    rig.player.duration = 180000;
    rig.Advance(29968);
    assert(rig.port.Count(Reason::kPeriodic) == 0);
    rig.Advance(1);
    assert(rig.port.Count(Reason::kPeriodic) == 1);
    rig.port.Pump();
    assert(blob.writes == 1);
    rig.player.position = 43000;
    rig.Advance(29999);
    assert(rig.port.Count(Reason::kPeriodic) == 1);
    rig.Advance(1);
    assert(rig.port.Count(Reason::kPeriodic) == 2);
    rig.port.Pump();
    const auto starts = rig.player.starts.size();
    assert(rig.runtime.Shutdown(rig.now));
    assert(rig.port.Last(Reason::kShutdown).position == 43000);
    assert(rig.player.starts.size() == starts && rig.player.stops == 0 && rig.player.running);
    rig.port.Pump();
}

void ShutdownSnapshotSurvivesStop() {
    MemoryBlob original;
    std::vector<uint8_t> persisted;
    {
        Rig rig(original);
        rig.Long(media::PlayerGestures::kDown);
        rig.Drain();
        for (int i = 0; i < 5; ++i)
            rig.Tap(media::PlayerGestures::kUp);
        rig.port.Pump();  // Finish unrelated startup snapshots before this shutdown.
        assert(rig.runtime.View(rig.now).title == "track2.mp3");
        assert(rig.runtime.View(rig.now).volume == 35);
        rig.player.position = 43000;
        rig.player.duration = 180000;
        const auto track = rig.runtime.Controller().CurrentTrack()->track.id;
        const auto starts = rig.player.starts;
        const auto pauses = rig.player.pauses;
        const auto events = rig.events;
        const auto stops = rig.player.stops;
        const auto saved_count = rig.port.saved.size();
        const auto shutdown_now = rig.now + 1000;
        assert(rig.runtime.Shutdown(shutdown_now));
        const auto snapshot = rig.port.Last(Reason::kShutdown);
        assert(snapshot.track.track.id == track && snapshot.track.track.file_name == "track2.mp3");
        assert(snapshot.position == 43000 && snapshot.duration == 180000);
        assert(snapshot.volume == 35 && snapshot.now == shutdown_now);
        assert(rig.port.queue.size() == 1 && rig.port.saved.size() == saved_count);
        assert(original.bytes.empty() && original.writes == 0);
        assert(rig.player.starts == starts && rig.player.pauses == pauses);
        assert(rig.player.stops == stops && rig.player.running && rig.events == events);
        assert(rig.runtime.Controller().State() == media::PlaybackState::kPlaying);

        rig.player.Stop();  // The owner stops only after snapshot/enqueue; Stop zeros the cursor.
        assert(rig.player.position == 0);
        rig.Tap(media::PlayerGestures::kDown);
        assert(rig.runtime.View(rig.now).volume == 30);
        rig.player.position = 99000;
        rig.player.duration = 900000;
        rig.player.End();
        assert(rig.port.Count(Reason::kShutdown) == 1 && rig.port.queue.size() == 1);
        rig.port.Pump();
        assert(rig.port.saved.size() == saved_count + 1);
        assert(rig.port.saved.back() == media::SaveStatus::kSaved);
        assert(rig.port.policy.LastSuccessfulSaveMs() == shutdown_now);
        media::ResumeRecord stored;
        assert(rig.port.store.Read("runtime-card", {}, stored) == media::ResumeRead::kFound);
        assert(stored.media.track.id == track && stored.position_ms == 43000);
        assert(stored.duration_ms == 180000 && stored.volume_percent == 35);
        persisted = original.bytes;
    }
    MemoryBlob fresh;
    fresh.bytes = persisted;  // No old Runtime, player, store or policy survives reconstruction.
    Rig reboot(fresh, true);
    assert(reboot.player.starts.back() == std::make_pair(std::string("/sdcard/track2.mp3"), uint64_t{38000}));
    assert(reboot.restored_volumes == std::vector<int>{35});
    assert(reboot.runtime.View(reboot.now).volume == 35 && fresh.writes == 0);

    reboot.player.position = 53000;
    reboot.player.duration = 240000;
    reboot.port.accept_checkpoints = false;
    const auto starts = reboot.player.starts;
    const auto stops = reboot.player.stops;
    const auto saved_count = reboot.port.saved.size();
    assert(!reboot.runtime.Shutdown(reboot.now + 1000));
    assert(reboot.port.queue.empty() && reboot.port.saved.size() == saved_count);
    assert(reboot.events.back() == "CHECKPOINT_NOT_QUEUED");
    assert(reboot.player.starts == starts && reboot.player.stops == stops);
    assert(!reboot.runtime.View(reboot.now).failed);
    reboot.player.Stop();
    reboot.player.position = 123;
    reboot.player.duration = 456;
    reboot.port.Pump();
    assert(fresh.bytes == persisted && fresh.writes == 0);
    assert(reboot.port.saved.size() == saved_count);
}

void OldTrackAndEof() {
    MemoryBlob blob;
    Rig rig(blob);
    const auto old = rig.runtime.Controller().CurrentTrack()->track.id;
    rig.player.position = 42000;
    rig.player.duration = 180000;
    rig.Long(media::PlayerGestures::kDown);
    assert(rig.port.Last(Reason::kTrackChange).track.track.id == old);
    assert(rig.port.Last(Reason::kTrackChange).position == 42000 && rig.player.stops == 1);
    rig.Long(media::PlayerGestures::kDown);  // Controller moves again while the old worker drains.
    for (const auto& checkpoint : rig.port.checkpoints)
        assert(checkpoint.track.track.id == old);
    rig.Drain();
    assert(rig.runtime.View(rig.now).title == "track10.mp3");
    rig.player.position = 180000;
    rig.player.duration = 180000;
    rig.player.End(FakePlayer::Result::kFinished);
    rig.Advance(1);
    const auto& eof = rig.port.Last(Reason::kTrackChange);
    assert(eof.track.track.file_name == "track10.mp3" && eof.position == 180000);
    assert(rig.runtime.Finished());
    rig.port.Pump();
}

void CollectionRestore() {
    MemoryBlob blob;
    Rig rig(blob);
    rig.player.position = 12000;
    rig.player.duration = 180000;
    rig.ChooseOther();
    assert(rig.port.Last(Reason::kCollectionChange).track.track.file_name == "track1.mp3");
    assert(rig.port.Last(Reason::kCollectionChange).position == 12000);
    const auto request = rig.port.restores.back();
    assert(request.first && request.second == media::ResumeMode::kSavedExact);
    assert(rig.player.starts.size() == 1);
    rig.Drain();
    assert(rig.runtime.View(rig.now).collection == "series");
    rig.Long(media::PlayerGestures::kDown);
    rig.Drain();
    assert(rig.runtime.View(rig.now).title == "episode2.mp3");
    rig.player.position = 42000;
    rig.player.duration = 180000;
    rig.ChooseOther();
    rig.Drain();
    assert(rig.runtime.View(rig.now).collection == "未分类");
    rig.ChooseOther();
    const auto count = rig.player.starts.size();
    rig.output_idle = false;
    rig.player.End();
    rig.port.Pump();
    rig.Advance(1);
    assert(rig.player.starts.size() == count);
    rig.output_idle = true;
    rig.Advance(1);
    assert(rig.player.starts.size() == count + 1);
    assert(rig.player.starts.back().first == "/sdcard/series/episode2.mp3");
    assert(rig.player.starts.back().second == 42000);  // SavedExact is not boot rewind.
    assert(rig.runtime.View(rig.now).volume == 10);
    assert(
        rig.restored_volumes.empty());  // Collection restore never reapplies an old saved volume.
}

void FailuresAreBestEffort() {
    MemoryBlob blob;
    Rig rig(blob);
    rig.player.position = 42000;
    rig.player.duration = 180000;
    rig.Pause();
    rig.port.Pump();
    const auto retained = blob.bytes;
    rig.Pause();  // Resume remains ordinary playback despite storage failures below.
    blob.write_error = true;
    rig.player.position = 52000;
    rig.player.duration = 180000;
    rig.Pause();
    rig.port.Pump();
    assert(rig.runtime.Controller().State() == media::PlaybackState::kPaused);
    assert(rig.port.saved.back() == media::SaveStatus::kStorageError && blob.bytes == retained);
    assert(rig.runtime.View(rig.now).failed == false);
    rig.port.accept_checkpoints = false;
    rig.Pause();
    rig.player.position = 62000;
    rig.Pause();
    assert(rig.runtime.Controller().State() == media::PlaybackState::kPaused);
    assert(!rig.runtime.Shutdown(rig.now));
    blob.write_error = false;
    rig.port.accept_checkpoints = true;
    rig.player.position = 63000;
    rig.player.duration = 0;
    rig.port.known_duration = 0;
    assert(rig.runtime.Shutdown(rig.now));
    rig.port.Pump();
    assert(rig.port.saved.back() == media::SaveStatus::kInvalidInput && blob.bytes == retained);
}

void FileReturnAndSeek() {
    using G = media::PlayerGestures;
    MemoryBlob blob;
    Rig rig(blob);
    rig.player.position = 42000;
    rig.player.duration = 180000;
    rig.Long(G::kDown);
    rig.Drain();
    rig.player.position = 70000;
    rig.player.duration = 180000;
    rig.Long(G::kUp);  // P5-2: straight back to A at its kept place, even 70 s into B.
    rig.Drain();
    assert(rig.player.starts.back() ==
           std::make_pair(std::string("/sdcard/track1.mp3"), uint64_t{42000}));
    const auto first = rig.player.starts.size();
    rig.Long(G::kUp);  // The first file has nothing before it: nothing happens.
    assert(rig.player.starts.size() == first && rig.runtime.View(rig.now).title == "track1.mp3");
    rig.player.position = 50000;
    rig.player.duration = 100000;
    rig.Edge(G::kPower);
    rig.Edge(G::kPower | G::kDown);
    assert(rig.runtime.PositionMs() == 80000);
    rig.Edge(G::kPower);
    rig.Edge(G::kPower | G::kDown);  // Accumulate on target, not transient player zero.
    assert(rig.runtime.PositionMs() == 100000);
    rig.Edge(G::kPower);
    rig.Edge(G::kPower | G::kUp);
    assert(rig.runtime.PositionMs() == 70000);
    rig.Edge(0);
    rig.Drain();
    assert(rig.player.starts.back().second == 70000);
    rig.player.position = 70000;
    rig.player.duration = 100000;
    rig.Pause();
    rig.player.position = 70000;
    rig.Edge(G::kPower);
    rig.Edge(G::kPower | G::kDown);
    rig.Edge(0);
    const auto starts = rig.player.starts.size();
    rig.Drain();
    assert(rig.player.starts.size() == starts);
    assert(rig.runtime.Controller().State() == media::PlaybackState::kPaused);
    assert(rig.runtime.PositionMs() == 100000);
    rig.Pause();
    assert(rig.player.starts.back().second == 100000);
}

// P5-2: the AI's pick replaces a parked story. A directory alone continues from
// its bookmark, a number starts that file from its beginning, and refusals
// change nothing.
void AiChoose() {
    MemoryBlob blob;
    Rig rig(blob);
    assert(rig.runtime.View(rig.now).directories == std::vector<std::string>({"未分类", "series"}));
    assert(rig.runtime.Selected() == std::make_optional(std::make_pair(size_t{0}, size_t{1})));
    assert(&rig.runtime.Catalog() == &rig.runtime.Controller().Catalog());
    rig.player.position = 42000;
    rig.player.duration = 180000;
    assert(rig.runtime.Park(rig.now));  // A conversation takes the speaker.
    rig.player.End();
    rig.Advance(1);
    const auto starts = rig.player.starts.size();
    assert(!rig.runtime.Choose(2, 0, rig.now) && !rig.runtime.Choose(1, 3, rig.now));
    assert(rig.runtime.ParkedWhilePlaying() && rig.player.starts.size() == starts);
    assert(rig.runtime.Choose(1, 2, rig.now));  // series, second file, from its beginning.
    assert(!rig.runtime.ParkedWhilePlaying());
    assert(rig.port.Last(Reason::kCollectionChange).track.track.file_name == "track1.mp3");
    assert(rig.port.Last(Reason::kCollectionChange).position == 42000);  // The old place stays.
    assert(rig.player.starts.size() == starts + 1);
    assert(rig.player.starts.back() ==
           std::make_pair(std::string("/sdcard/series/episode2.mp3"), uint64_t{0}));
    assert(rig.runtime.Selected() == std::make_optional(std::make_pair(size_t{1}, size_t{2})));
    assert(rig.runtime.Unpark(rig.now) && rig.player.starts.size() == starts + 1);
    // 30 s into episode2 the next conversation picks 未分类's third file...
    rig.player.position = 30000;
    rig.player.duration = 180000;
    assert(rig.runtime.Park(rig.now));
    rig.player.End();
    rig.port.Pump();
    rig.Advance(1);
    assert(rig.runtime.Choose(0, 3, rig.now));
    assert(rig.player.starts.back() ==
           std::make_pair(std::string("/sdcard/track10.mp3"), uint64_t{0}));
    // ...and the one after picks series alone: it continues where it was heard.
    rig.player.position = 5000;
    rig.player.duration = 180000;  // Only a known length keeps a bookmark.
    assert(rig.runtime.Park(rig.now));
    rig.player.End();
    rig.port.Pump();
    rig.Advance(1);
    assert(rig.runtime.Choose(1, 0, rig.now));
    assert(!rig.runtime.ParkedWhilePlaying());  // Never resumes track10 while restoring.
    const auto before = rig.player.starts.size();
    assert(rig.runtime.Unpark(rig.now) && rig.player.starts.size() == before);
    assert(rig.port.restores.back().first &&
           rig.port.restores.back().second == media::ResumeMode::kSavedExact);
    assert(!rig.runtime.Choose(0, 1, rig.now));  // Still finding the bookmark: refused.
    rig.port.Pump();
    rig.Advance(1);
    assert(rig.player.starts.back() ==
           std::make_pair(std::string("/sdcard/series/episode2.mp3"), uint64_t{30000}));
    // A number still means the file's beginning, bookmark or not.
    rig.player.position = 31000;
    assert(rig.runtime.Park(rig.now));
    rig.player.End();
    rig.port.Pump();
    rig.Advance(1);
    assert(rig.runtime.Choose(1, 2, rig.now));
    assert(rig.player.starts.back() ==
           std::make_pair(std::string("/sdcard/series/episode2.mp3"), uint64_t{0}));
    // Without a conversation the pick plays at once, then the directory goes on.
    assert(rig.runtime.Choose(1, 1, rig.now));
    rig.Drain();
    assert(rig.player.starts.back() ==
           std::make_pair(std::string("/sdcard/series/episode1.mp3"), uint64_t{0}));
    rig.player.End(FakePlayer::Result::kFinished);
    rig.Advance(1);
    rig.Advance(31);
    assert(rig.player.starts.back().first == "/sdcard/series/episode2.mp3");
    // After the directory's last file the player stops; picks still work, one at a time.
    rig.player.End(FakePlayer::Result::kFinished);
    rig.Advance(1);
    assert(rig.runtime.Finished());
    assert(rig.runtime.Choose(0, 0, rig.now));
    rig.Advance(1);  // The switch settles; the bookmark is still being found.
    assert(!rig.runtime.Choose(1, 1, rig.now));  // So a second pick is refused.
    rig.port.Pump();
    rig.Advance(1);
    rig.Advance(31);
    assert(rig.player.starts.back() ==
           std::make_pair(std::string("/sdcard/track10.mp3"), uint64_t{5000}));
    // P5-5: a pick the decoder cannot play is skipped like any other file.
    assert(rig.runtime.Choose(1, 1, rig.now));
    rig.Drain();
    assert(rig.player.starts.back().first == "/sdcard/series/episode1.mp3");
    rig.player.End(FakePlayer::Result::kError);
    rig.Advance(1);
    assert(rig.player.starts.back().first == "/sdcard/series/episode2.mp3");
}

// AI takeover of a playing story: the heard position is saved and kept while
// the conversation owns the speaker, then the story restarts five seconds back.
void AiParkWhilePlaying() {
    using G = media::PlayerGestures;
    using A = media::GestureAction;
    MemoryBlob blob;
    Rig rig(blob);
    rig.player.position = 42000;
    rig.player.duration = 180000;
    rig.runtime.SetAi(false, true);
    rig.Tap(G::kPower);
    rig.Tap(G::kPower);  // Double-click while ready asks the board to open the AI.
    assert(rig.gestures.back() == A::kOpenAi);
    assert(rig.runtime.Controller().State() == media::PlaybackState::kPlaying);
    const auto stops = rig.player.stops;
    assert(rig.runtime.Park(rig.now));
    assert(rig.port.Last(Reason::kPause).position == 42000 && rig.player.stops == stops + 1);
    assert(rig.runtime.Controller().State() == media::PlaybackState::kPaused);
    assert(rig.runtime.ParkedWhilePlaying() && !rig.runtime.Released());
    rig.player.End();  // The decoder exits; the speaker is free.
    rig.runtime.SetAi(true, true);
    rig.Advance(1);  // First observe the new context, then settle all-released debounce.
    rig.Advance(30);
    assert(rig.runtime.Released() && rig.runtime.PositionMs() == 42000);
    assert(rig.runtime.View(rig.now).duration_ms == 180000);
    const auto starts = rig.player.starts.size();
    rig.Tap(G::kUp);  // AI gestures: volume still works,
    assert(rig.runtime.View(rig.now).volume == 15);
    rig.Long(G::kDown);  // track changes do not,
    rig.Tap(G::kPower);  // and the top key asks to end the conversation.
    assert(rig.gestures.back() == A::kCancelAi);
    assert(rig.player.starts.size() == starts);
    rig.port.Pump();
    rig.Advance(30000);  // Periodic and shutdown saves keep the heard position.
    assert(rig.port.Last(Reason::kPeriodic).position == 42000);
    assert(rig.runtime.Shutdown(rig.now) && rig.port.Last(Reason::kShutdown).position == 42000);
    rig.runtime.SetAi(false, true);
    assert(rig.runtime.Unpark(rig.now));
    assert(rig.player.starts.size() == starts + 1);
    assert(rig.player.starts.back() ==
           std::make_pair(std::string("/sdcard/track1.mp3"), uint64_t{37000}));
    assert(rig.runtime.Controller().State() == media::PlaybackState::kPlaying);
    assert(!rig.runtime.Parked() && rig.runtime.Unpark(rig.now));  // Nothing more to do.
    assert(rig.player.starts.size() == starts + 1);
    rig.player.position = 37500;
    rig.Advance(1);
    rig.player.position = 3000;  // Near the start, the rewind stops at zero.
    assert(rig.runtime.Park(rig.now));
    rig.player.End();
    rig.Advance(1);
    assert(rig.runtime.Unpark(rig.now) && rig.player.starts.back().second == 0);
}

// A paused story stays paused after the AI; play restarts exactly where it was.
void AiParkWhilePaused() {
    using G = media::PlayerGestures;
    MemoryBlob blob;
    Rig rig(blob);
    rig.player.position = 42000;
    rig.player.duration = 180000;
    rig.Pause();
    rig.player.position = 42000;
    const auto stops = rig.player.stops;
    assert(rig.runtime.Park(rig.now) && rig.player.stops == stops + 1);
    assert(rig.runtime.Parked() && !rig.runtime.ParkedWhilePlaying());
    rig.player.End();
    rig.Advance(1);
    const auto starts = rig.player.starts.size();
    assert(rig.runtime.Unpark(rig.now) && rig.player.starts.size() == starts);
    assert(rig.runtime.Controller().State() == media::PlaybackState::kPaused);
    assert(rig.runtime.View(rig.now).state == library::PlayerState::kPaused);
    rig.runtime.SetAi(false, false);
    rig.Pause();  // No decoder to unpause: a new one starts at the paused position.
    assert(rig.player.starts.size() == starts + 1 && rig.player.starts.back().second == 42000);
    assert(rig.runtime.Controller().State() == media::PlaybackState::kPlaying);
    rig.Tap(G::kPower);
    rig.Tap(G::kPower);  // Not ready: only the offline hint, the story keeps playing.
    assert(rig.gestures.back() == media::GestureAction::kOfflineHint);
    assert(rig.runtime.View(rig.now).offline_hint);
    assert(rig.runtime.Controller().State() == media::PlaybackState::kPlaying);
}

// 2026-10-15: each directory's play mode through the real runtime. Files the
// shuffle or a repeat moves to start from zero; every other start keeps the
// bookmark rules.
void PlayModes() {
    using G = media::PlayerGestures;
    using media::PlayMode;
    const auto finish = [](Rig& rig, FakePlayer::Result result = FakePlayer::Result::kFinished) {
        rig.player.position = rig.player.duration = 180000;
        rig.player.End(result);
        rig.Advance(1);
        rig.Advance(31);
    };
    const auto started = [](const Rig& rig, const char* path, uint64_t offset) {
        return rig.player.starts.back() == std::make_pair(std::string(path), offset);
    };
    MemoryBlob blob;
    Rig rig(blob);
    rig.runtime.Seed(7);
    assert(rig.runtime.View(rig.now).mode == PlayMode::kOrder);
    // Bookmarks: track1 at 20 s and track2 at 30 s; track10 plays.
    rig.player.position = 20000;
    rig.player.duration = 180000;
    rig.Long(G::kDown);
    rig.Drain();
    rig.player.position = 30000;
    rig.Long(G::kDown);
    rig.Drain();
    assert(started(rig, "/sdcard/track10.mp3", 0));
    // Loop: after the last file the first plays again, from its bookmark, and
    // the previous key wraps to the last file.
    rig.runtime.SetMode(0, PlayMode::kLoop);
    assert(rig.runtime.View(rig.now).mode == PlayMode::kLoop);
    finish(rig);
    assert(started(rig, "/sdcard/track1.mp3", 20000) && !rig.runtime.Finished());
    rig.player.position = 25000;
    rig.Long(G::kUp);
    rig.Drain();
    assert(started(rig, "/sdcard/track10.mp3", 0));
    // Single: the file plays again from zero; one that cannot play is not
    // retried, the next one (wrapping around) plays and repeats instead.
    rig.runtime.SetMode(0, PlayMode::kSingle);
    finish(rig);
    assert(started(rig, "/sdcard/track10.mp3", 0));
    rig.player.End(FakePlayer::Result::kError);
    rig.Advance(1);
    rig.Advance(31);
    assert(started(rig, "/sdcard/track1.mp3", 25000));
    finish(rig);
    assert(started(rig, "/sdcard/track1.mp3", 0));
    // Shuffle: the round after track1 holds the other two, each from zero,
    // even track2 with its 30 s bookmark.
    rig.runtime.SetMode(0, PlayMode::kShuffle);
    assert(rig.runtime.View(rig.now).mode == PlayMode::kShuffle);
    std::vector<std::string> round;
    for (int i = 0; i < 2; ++i) {
        finish(rig);
        assert(rig.player.starts.back().second == 0);
        round.push_back(rig.player.starts.back().first);
    }
    const auto last = round.back();
    std::sort(round.begin(), round.end());
    assert(round == (std::vector<std::string>{"/sdcard/track10.mp3", "/sdcard/track2.mp3"}));
    rig.player.position = 10000;
    rig.Long(G::kDown);  // A new round, never the file just heard first.
    rig.Drain();
    assert(rig.player.starts.back().first != last && rig.player.starts.back().second == 0);
    rig.player.position = 10000;
    rig.Long(G::kUp);  // Back to the file just heard, from zero.
    rig.Drain();
    assert(started(rig, last.c_str(), 0));
    // The AI's pick in a shuffled directory starts that file, then shuffles on.
    assert(rig.runtime.Choose(0, 2, rig.now));
    rig.Drain();
    assert(started(rig, "/sdcard/track2.mp3", 0));
    finish(rig);
    assert(rig.player.starts.back().first != "/sdcard/track2.mp3");
    assert(rig.player.starts.back().second == 0);
    // Another directory keeps its own mode; continuing it keeps the bookmark.
    rig.runtime.SetMode(1, PlayMode::kShuffle);
    assert(rig.runtime.Choose(1, 1, rig.now));
    rig.Drain();
    assert(started(rig, "/sdcard/series/episode1.mp3", 0));
    rig.player.position = 40000;
    assert(rig.runtime.Choose(0, 1, rig.now));
    rig.Drain();
    assert(started(rig, "/sdcard/track1.mp3", 0));
    assert(rig.runtime.View(rig.now).mode == PlayMode::kShuffle);
    rig.runtime.SetMode(0, PlayMode::kOrder);
    assert(rig.runtime.View(rig.now).mode == PlayMode::kOrder);
    rig.port.Pump();
    assert(rig.runtime.Choose(1, 0, rig.now));
    rig.Drain();
    assert(started(rig, "/sdcard/series/episode1.mp3", 40000));
    finish(rig);
    assert(started(rig, "/sdcard/series/episode2.mp3", 0));
    finish(rig);  // Two files: the shuffle goes back and forth.
    assert(started(rig, "/sdcard/series/episode1.mp3", 0) && !rig.runtime.Finished());
}

// The AI asked the story to stay paused: after the conversation it keeps its
// place about five seconds back, and the top key starts it there. Outside a
// conversation the request works like the top key.
void AiStayPaused() {
    MemoryBlob blob;
    Rig rig(blob);
    rig.player.position = 42000;
    rig.player.duration = 180000;
    rig.runtime.StayPaused();  // Not parked: nothing to do.
    assert(rig.runtime.Controller().State() == media::PlaybackState::kPlaying);
    assert(rig.runtime.Park(rig.now));
    rig.player.End();
    rig.Advance(1);
    assert(rig.runtime.ParkedWhilePlaying());
    rig.runtime.StayPaused();
    assert(!rig.runtime.ParkedWhilePlaying() && rig.runtime.PositionMs() == 37000);
    const auto starts = rig.player.starts.size();
    assert(rig.runtime.Unpark(rig.now) && rig.player.starts.size() == starts);
    assert(rig.runtime.View(rig.now).state == library::PlayerState::kPaused);
    rig.Pause();  // No decoder to unpause: a new one starts at the kept place.
    assert(rig.player.starts.size() == starts + 1);
    assert(rig.player.starts.back() ==
           std::make_pair(std::string("/sdcard/track1.mp3"), uint64_t{37000}));
    rig.player.position = 50000;
    const auto pauses = rig.player.pauses.size();
    assert(rig.runtime.Pause(rig.now));
    assert(rig.runtime.Controller().State() == media::PlaybackState::kPaused);
    assert(rig.player.pauses.size() == pauses + 1 && rig.player.pauses.back());
    assert(rig.port.Last(Reason::kPause).position == 50000);
    assert(rig.runtime.Pause(rig.now) && rig.player.pauses.size() == pauses + 1);
    rig.Pause();  // Playing again; then a server push takes the speaker.
    rig.player.position = 60000;
    rig.Advance(1);
    rig.player.End();
    rig.Advance(1);
    assert(rig.runtime.ParkedWhilePlaying());
    assert(rig.runtime.Pause(rig.now) && !rig.runtime.ParkedWhilePlaying());
    assert(rig.runtime.PositionMs() == 55000);
}

// Another owner ending the output session (a reply or a server push) no longer
// fails playback: the story holds its position until the board gives it back.
void ReleasedByAnotherOwner() {
    MemoryBlob blob;
    Rig rig(blob);
    rig.player.position = 42000;
    rig.player.duration = 180000;
    rig.Advance(1);
    rig.player.End();  // Not requested by the runtime.
    rig.Advance(1);
    assert(rig.events.back() == "RELEASED" && !rig.runtime.View(rig.now).failed);
    assert(rig.runtime.ParkedWhilePlaying() && rig.runtime.PositionMs() == 42000);
    assert(rig.runtime.Controller().State() == media::PlaybackState::kPaused);
    assert(rig.runtime.Shutdown(rig.now) && rig.port.Last(Reason::kShutdown).position == 42000);
    assert(rig.runtime.Unpark(rig.now) && rig.player.starts.back().second == 37000);
    rig.player.position = 50000;
    rig.Pause();  // Paused, then ended by another owner: play restarts in place.
    rig.player.position = 50000;
    rig.player.End();
    rig.Advance(1);
    assert(rig.runtime.Parked() && !rig.runtime.ParkedWhilePlaying());
    rig.Pause();
    assert(rig.player.starts.back().second == 50000);
}

// The story refuses to hand over the speaker while it is switching or locating.
void ParkRefusedWhileBusy() {
    using G = media::PlayerGestures;
    MemoryBlob blob;
    Rig rig(blob, true);  // A decoder reports zero until its first output.
    rig.player.position = 1000;
    rig.player.duration = 180000;
    rig.Advance(1);
    rig.Edge(G::kPower);
    rig.Edge(G::kPower | G::kDown);  // Seek +30 s: the old decoder drains first.
    rig.Edge(0);
    assert(!rig.runtime.Park(rig.now) && !rig.runtime.Parked());
    rig.Drain();  // The new decoder starts at 31 s and is still locating.
    assert(rig.player.starts.back().second == 31000);
    assert(!rig.runtime.Park(rig.now) &&
           rig.runtime.View(rig.now).state == library::PlayerState::kLocating);
    rig.player.position = 31048;
    rig.Advance(1);
    assert(rig.runtime.Park(rig.now) && rig.runtime.Parked());
}

struct LegacyPlayer {
    using Result = FakePlayer::Result;
    bool running = false;
    int starts = 0;
    bool Start(const std::string&) {
        running = true;
        ++starts;
        return true;
    }
    bool Pause(bool) { return true; }
    void Stop() { running = false; }
    bool Running() const { return running; }
    Result LastResult() const { return running ? Result::kRunning : Result::kIdle; }
    bool ReadFailed() const { return false; }
};
// P5-5: sound clears the failure count, so files that each play a while and
// then fail never stop the player. What a failed file played is kept; one that
// failed before any sound after a seek saves no zero.
void SkipAfterSound() {
    using G = media::PlayerGestures;
    MemoryBlob blob;
    Rig rig(blob, true);
    auto saved = [&](const char* name, uint64_t position) {
        return std::count_if(
            rig.port.checkpoints.begin(), rig.port.checkpoints.end(), [&](const auto& item) {
                return item.track.track.file_name == name && item.position == position;
            });
    };
    rig.player.position = 50000;
    rig.player.duration = 100000;
    rig.Edge(G::kPower);
    rig.Edge(G::kPower | G::kDown);  // +30 s: track1 starts again at 80 s...
    rig.Edge(0);
    rig.Drain();
    assert(rig.player.starts.back() ==
           std::make_pair(std::string("/sdcard/track1.mp3"), uint64_t{80000}));
    const auto zeros = saved("track1.mp3", 0);
    rig.player.End(FakePlayer::Result::kError);  // ...and fails before any sound.
    rig.Advance(1);
    assert(saved("track1.mp3", 0) == zeros);
    assert(rig.player.starts.back().first == "/sdcard/track2.mp3");
    for (const char* name : {"track2.mp3", "track10.mp3"}) {
        rig.player.position = 20000;  // It plays a while...
        rig.Advance(1);
        rig.player.End(FakePlayer::Result::kError);  // ...then fails.
        rig.Advance(1);
        assert(saved(name, 20000) == 1);
    }
    // Three failures, but sound cleared the count: the directory just ends.
    assert(std::count(rig.events.begin(), rig.events.end(), std::string("SKIP")) == 3);
    assert(rig.runtime.Finished() && !rig.runtime.View(rig.now).failed);
    rig.port.Pump();
}

// P5-5: sound also clears a failure the card caused, so bad files after it
// pause on 「播放错误」 like any bad files; Tick() never reports the error.
void SoundClearsCardFailure() {
    MemoryBlob blob;
    Rig rig(blob);
    rig.player.End(FakePlayer::Result::kError, true);  // The card could not read track1.
    rig.Advance(1);
    assert(rig.player.starts.back().first == "/sdcard/track2.mp3");
    rig.player.position = 20000;  // track2 plays a while...
    rig.Advance(1);
    rig.player.End(FakePlayer::Result::kError);  // ...then fails, and so does track10:
    rig.Advance(1);
    assert(rig.player.starts.back().first == "/sdcard/track10.mp3");
    rig.player.End(FakePlayer::Result::kError);
    rig.Advance(1);
    assert(rig.runtime.Finished());  // The directory ends; Play tries track10 again.
    rig.Pause();
    assert(rig.player.starts.back().first == "/sdcard/track10.mp3");
    rig.player.End(FakePlayer::Result::kError);  // Third bad file in a row.
    rig.Advance(1);
    const auto view = rig.runtime.View(rig.now);
    assert(!view.failed && view.state == library::PlayerState::kFailed);
    rig.port.Pump();
}

void DefaultDisabled() {
    LegacyPlayer player;
    library::Runtime<LegacyPlayer> runtime(Catalog(), player, [] { return true; });
    assert(runtime.Start(0) && player.starts == 1);
    assert(runtime.Tick(0, 0) && runtime.Tick(0, 30000));
    assert(player.starts == 1 && runtime.View(30000).volume == 10);
    runtime.Shutdown(30000);
}
}  // namespace

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string mode = argv[1];
    if (mode == "boot")
        BootReconstruction();
    else if (mode == "periodic")
        PeriodicAndShutdown();
    else if (mode == "shutdown")
        ShutdownSnapshotSurvivesStop();
    else if (mode == "switch")
        OldTrackAndEof();
    else if (mode == "collection")
        CollectionRestore();
    else if (mode == "failure")
        FailuresAreBestEffort();
    else if (mode == "default")
        DefaultDisabled();
    else if (mode == "controls")
        FileReturnAndSeek();
    else if (mode == "ai_playing")
        AiParkWhilePlaying();
    else if (mode == "ai_paused")
        AiParkWhilePaused();
    else if (mode == "released")
        ReleasedByAnotherOwner();
    else if (mode == "park_busy")
        ParkRefusedWhileBusy();
    else if (mode == "ai_choose")
        AiChoose();
    else if (mode == "modes")
        PlayModes();
    else if (mode == "ai_stay_paused")
        AiStayPaused();
    else if (mode == "skip") {
        SkipAfterSound();
        SoundClearsCardFailure();
    } else
        return 2;
    std::cout << mode << " PASS (real Runtime/resume policy/blob, virtual decoder and keys)\n";
}
