#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

static std::string synthetic_root;
static int reads = 0;
static bool read_error = false, pumping = false;
static bool sdk_fd_mtime = false, fd_size_changed = false, fd_mode_changed = false,
            open_error = false, close_error = false;
static int fd_calls = 0, fd_failure_at = 0, path_calls = 0, path_failure_at = 0;
static std::string MapPath(const char* path) {
    const std::string value(path);
    if (value == "/sdcard" || value.rfind("/sdcard/", 0) == 0) {
        assert(pumping && !synthetic_root.empty());
        return synthetic_root + value.substr(7);
    }
    return value;
}
[[maybe_unused]] static int FakeStat(const char* path, struct stat* info) {
    if (++path_calls == path_failure_at) {
        errno = EIO;
        return -1;
    }
    return stat(MapPath(path).c_str(), info);
}
static int FakeFstat(int fd, struct stat* info) {
    if (++fd_calls == fd_failure_at) {
        errno = EIO;
        return -1;
    }
    const int result = fstat(fd, info);
    if (result == 0) {
        if (sdk_fd_mtime)
            info->st_mtime = 0;  // IDF FatFs fstat exposes mode/size, not the path's FAT mtime.
        if (fd_size_changed)
            ++info->st_size;
        if (fd_mode_changed)
            info->st_mode = S_IFDIR;
    }
    return result;
}
static FILE* FakeOpen(const char* path, const char* mode) {
    if (open_error) {
        errno = EACCES;
        return nullptr;
    }
    return fopen(MapPath(path).c_str(), mode);
}
static int FakeClose(FILE* file) {
    const int result = fclose(file);
    if (close_error) {
        errno = EIO;
        return EOF;
    }
    return result;
}
static size_t FakeRead(void* out, size_t width, size_t count, FILE* file) {
    assert(width == 1 && count <= 8192);
    ++reads;
    if (read_error) {
        errno = EIO;
        return 0;
    }
    return fread(out, width, count, file);
}
#ifndef M2_RESUME_FILE_ONLY
#define ESP_PLATFORM
#endif
#define stat(...) FakeStat(__VA_ARGS__)
#define fstat FakeFstat
#define fopen FakeOpen
#define fclose FakeClose
#define fread FakeRead
#include "boards/boilon-s3-gen2/m2_resume_worker.h"
#undef stat
#undef fstat
#undef fopen
#undef fclose
#undef fread

using namespace boilon::local_library;
using namespace media;

#ifndef M2_RESUME_FILE_ONLY
struct FakeQueue {
    unsigned capacity;
    std::deque<void*> jobs;
};
struct QueueDrained {};
static bool queue_failure = false, task_failure = false, init_failure = false,
            commit_failure = false;
static int created = 0, deleted = 0, initialized = 0, erased = 0, opens = 0, sets = 0, commits = 0,
           delays = 0;
static int64_t timer = 0, delay_step = 1000;
static void (*task)(void*) = nullptr;
static void* task_self = nullptr;
static std::vector<uint8_t> persisted, pending, legacy_persisted;
static std::vector<FakeQueue*> queues;
static ResumeWorker* observed_worker = nullptr;
static std::vector<uint32_t> sent_pending, received_pending, write_pending, commit_pending,
    close_pending;
static std::function<void()> commit_hook;

QueueHandle_t xQueueCreate(unsigned capacity, size_t item) {
    assert(capacity == 2 && item == sizeof(void*));
    ++created;
    if (queue_failure)
        return nullptr;
    auto* queue = new FakeQueue{capacity, {}};
    queues.push_back(queue);
    return queue;
}
void vQueueDelete(QueueHandle_t handle) {
    ++deleted;
    auto* queue = static_cast<FakeQueue*>(handle);
    assert(queue->jobs.empty());
    const auto found = std::find(queues.begin(), queues.end(), queue);
    assert(found != queues.end());
    queues.erase(found);
    delete queue;
}
int xQueueSend(QueueHandle_t handle, const void* item, unsigned wait) {
    assert(wait == 0 && !pumping);
    auto* queue = static_cast<FakeQueue*>(handle);
    if (observed_worker) {
        sent_pending.push_back(observed_worker->PendingJobs());
        assert(observed_worker->PendingJobs() == queue->jobs.size() + 1);
    }
    if (queue->jobs.size() == queue->capacity)
        return 0;
    queue->jobs.push_back(*static_cast<void* const*>(item));
    return pdTRUE;
}
int xQueueReceive(QueueHandle_t handle, void* item, unsigned wait) {
    assert(pumping && wait == portMAX_DELAY);
    auto* queue = static_cast<FakeQueue*>(handle);
    if (queue->jobs.empty()) {
        if (observed_worker)
            assert(observed_worker->PendingJobs() == 0);
        throw QueueDrained{};
    }
    if (observed_worker) {
        received_pending.push_back(observed_worker->PendingJobs());
        assert(observed_worker->PendingJobs() == queue->jobs.size());
    }
    *static_cast<void**>(item) = queue->jobs.front();
    queue->jobs.pop_front();
    return pdTRUE;
}
int xTaskCreate(void (*callback)(void*), const char* name, unsigned stack, void* self,
                unsigned priority, void* output) {
    assert(std::string(name) == "local_resume" && stack == 12288 && priority == 2 && !output);
    if (task_failure)
        return 0;
    task = callback;
    task_self = self;
    return pdPASS;
}
void vTaskDelay(unsigned ticks) {
    assert(pumping && ticks == 1);
    ++delays;
    timer += delay_step;
}
int64_t esp_timer_get_time() {
    assert(pumping);
    return timer;
}
int nvs_flash_init() {
    assert(pumping);
    ++initialized;
    return init_failure ? 99 : ESP_OK;
}
int nvs_flash_erase() {
    ++erased;
    return ESP_OK;
}
int nvs_open(const char* ns, int access, nvs_handle_t* handle) {
    assert(pumping && std::string(ns) == "bl_resume");
    assert(access == NVS_READONLY || access == NVS_READWRITE);
    ++opens;
    *handle = 42;
    return access == NVS_READONLY && persisted.empty() && legacy_persisted.empty()
               ? ESP_ERR_NVS_NOT_FOUND : ESP_OK;
}
int nvs_get_blob(nvs_handle_t handle, const char* key, void* data, size_t* size) {
    assert(pumping && handle == 42);
    const std::string name(key);
    assert(name == "state_v2" || name == "state_v1");
    const auto& bytes = name == "state_v2" ? persisted : legacy_persisted;
    if (bytes.empty())
        return ESP_ERR_NVS_NOT_FOUND;
    if (!data) {
        *size = bytes.size();
        return ESP_OK;
    }
    assert(*size == bytes.size());
    std::memcpy(data, bytes.data(), *size);
    return ESP_OK;
}
int nvs_set_blob(nvs_handle_t handle, const char* key, const void* data, size_t size) {
    assert(pumping && handle == 42 && std::string(key) == "state_v2");
    ++sets;
    if (observed_worker) {
        write_pending.push_back(observed_worker->PendingJobs());
        assert(observed_worker->PendingJobs() > 0);
    }
    const auto* bytes = static_cast<const uint8_t*>(data);
    pending.assign(bytes, bytes + size);
    return ESP_OK;
}
int nvs_commit(nvs_handle_t handle) {
    assert(pumping && handle == 42);
    ++commits;
    if (observed_worker) {
        commit_pending.push_back(observed_worker->PendingJobs());
        assert(observed_worker->PendingJobs() > 0);
    }
    if (commit_hook)
        commit_hook();
    if (commit_failure)
        return 99;
    persisted = pending;
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) {
    assert(pumping && handle == 42);
    if (observed_worker) {
        close_pending.push_back(observed_worker->PendingJobs());
        assert(observed_worker->PendingJobs() > 0);
    }
}
void Pump() {
    assert(task && !pumping);
    pumping = true;
    try {
        task(task_self);
        assert(false);
    } catch (const QueueDrained&) {
    }
    pumping = false;
}
void Reset() {
    observed_worker = nullptr;
    commit_hook = {};
    sent_pending.clear();
    received_pending.clear();
    write_pending.clear();
    commit_pending.clear();
    close_pending.clear();
    for (auto* queue : queues) {
        assert(queue->jobs.empty());
        delete queue;
    }
    queues.clear();
    queue_failure = task_failure = init_failure = commit_failure = read_error = false;
    sdk_fd_mtime = fd_size_changed = fd_mode_changed = open_error = close_error = false;
    fd_calls = fd_failure_at = path_calls = path_failure_at = 0;
    created = deleted = initialized = erased = opens = sets = commits = delays = reads = 0;
    timer = 0;
    delay_step = 1000;
    task = nullptr;
    task_self = nullptr;
    persisted.clear();
    pending.clear();
    legacy_persisted.clear();
}
#endif
void WriteMp3(const std::string& path, int count = 65, bool truncated = false) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    assert(file);
    std::vector<uint8_t> frame(417);
    frame[0] = 0xff;
    frame[1] = 0xfb;
    frame[2] = 0x90;
    for (int i = 0; i < count; ++i)
        file.write(reinterpret_cast<const char*>(frame.data()),
                   truncated && i + 1 == count ? 416 : 417);
    file.close();
    assert(file);
}
MediaCatalog Catalog(const std::string& root) {
    struct stat info{};
    assert(stat((root + "/track.mp3").c_str(), &info) == 0);
    MediaCatalogBuilder builder("synthetic-card");
    assert(builder.Add({"", "track.mp3", uint64_t(info.st_size), uint64_t(info.st_mtime)}) ==
           AddResult::kAdded);
    auto built = builder.Finish();
    assert(built.Ok());
    return std::move(built.catalog);
}
ResolvedTrack FixtureTrack(const MediaCatalog& catalog) {
    return *catalog.Resolve(catalog.Collections().front().tracks.front().id);
}
void FileCases(const std::string& root) {
    const auto path = root + "/track.mp3";
    WriteMp3(path);
    auto track = FixtureTrack(Catalog(root));
    int steps = 0;
    assert(FileDuration(track, root, [&] {
               ++steps;
               return true;
           }) == 65ULL * 1152 * 1000 / 44100);
    assert(steps == 6 && reads >= 4);  // One admission check before metadata reads.
    auto bad = track;
    ++bad.track.size_bytes;
    assert(FileDuration(bad, root, [] {
               assert(false);
               return true;
           }) == 0);
    bad = track;
    ++bad.track.modified_time;
    assert(FileDuration(bad, root, [] {
               assert(false);
               return true;
           }) == 0);
    for (const auto& invalid : {std::string("relative"), root + "/..", root + "/"})
        assert(FileDuration(track, invalid, [] {
                   assert(false);
                   return true;
               }) == 0);
    bad = track;
    bad.track.file_name = "../track.mp3";
    assert(FileDuration(bad, root, [] {
               assert(false);
               return true;
           }) == 0);
    bad = track;
    bad.collection_is_root = false;
    bad.directory_name = "..";
    assert(FileDuration(bad, root, [] {
               assert(false);
               return true;
           }) == 0);
    steps = 0;
    assert(FileDuration(track, root, [&] { return ++steps < 2; }) == 0 && steps == 2);
    read_error = true;
    assert(FileDuration(track, root, [] { return true; }) == 0);
    read_error = false;
    steps = 0;
    assert(FileDuration(track, root, [&] {
               if (++steps == 2) {
                   const utimbuf changed{time_t(track.track.modified_time + 2),
                                         time_t(track.track.modified_time + 2)};
                   assert(utime(path.c_str(), &changed) == 0);
               }
               return true;
           }) == 0);
    WriteMp3(path);
    track = FixtureTrack(Catalog(root));
    steps = 0;
    assert(FileDuration(track, root, [&] {
               if (++steps == 2)
                   assert(truncate(path.c_str(), 417) == 0);
               return true;
           }) == 0);
    WriteMp3(path, 65, true);
    track = FixtureTrack(Catalog(root));
    assert(FileDuration(track, root, [] { return true; }) == 0);
}
// F08: a WAV's length comes from its header, in one admission step.
void WavCases(const std::string& root) {
    const auto path = root + "/track.wav";
    auto write = [&](uint16_t tag, uint32_t frames) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        auto put = [&](uint32_t value, int width) {
            for (int i = 0; i < width; ++i)
                file.put(static_cast<char>(value >> (8 * i)));
        };
        file.write("RIFF", 4);
        put(36 + frames * 4, 4);
        file.write("WAVEfmt ", 8);
        put(16, 4);
        put(tag, 2);
        put(2, 2);
        put(44100, 4);
        put(44100 * 4, 4);
        put(4, 2);
        put(16, 2);
        file.write("data", 4);
        put(frames * 4, 4);
        for (uint32_t i = 0; i < frames * 4; ++i)
            file.put(0);
        file.close();
        assert(file);
    };
    auto track_of = [&]() {
        struct stat info{};
        assert(stat(path.c_str(), &info) == 0);
        MediaCatalogBuilder builder("synthetic-card");
        assert(builder.Add({"", "track.wav", uint64_t(info.st_size), uint64_t(info.st_mtime)}) ==
               AddResult::kAdded);
        auto built = builder.Finish();
        assert(built.Ok());
        return FixtureTrack(built.catalog);
    };
    write(1, 44100 * 3 / 2);
    int steps = 0;
    DurationDiagnostic diagnostic;
    assert(FileDuration(
               track_of(), root,
               [&] {
                   ++steps;
                   return true;
               },
               &diagnostic) == 1500);
    assert(steps == 1 && diagnostic.status == DurationStatus::kKnown);
    write(0x11, 100);  // IMA ADPCM: no PCM length, and no MP3 scan either.
    steps = 0;
    assert(FileDuration(
               track_of(), root,
               [&] {
                   ++steps;
                   return true;
               },
               &diagnostic) == 0);
    assert(steps == 1 && diagnostic.status == DurationStatus::kFormat);
    assert(std::remove(path.c_str()) == 0);
}
void SdkStatCases(const std::string& root) {
    const auto path = root + "/track.mp3";
    WriteMp3(path);
    const utimbuf timestamp{1234567890, 1234567890};
    assert(utime(path.c_str(), &timestamp) == 0);
    const auto track = FixtureTrack(Catalog(root));
    assert(track.track.modified_time == 1234567890);
    sdk_fd_mtime = true;
    assert(FileDuration(track, root, [] { return true; }) == 65ULL * 1152 * 1000 / 44100);
    sdk_fd_mtime = false;
}
void DurationDiagnostics(const std::string& root) {
    const auto path = root + "/track.mp3";
    WriteMp3(path);
    const auto track = FixtureTrack(Catalog(root));
    auto reset = [&] {
        sdk_fd_mtime = fd_size_changed = fd_mode_changed = open_error = close_error = read_error =
            false;
        fd_calls = fd_failure_at = path_calls = path_failure_at = reads = 0;
    };
    DurationDiagnostic diagnostic;
    reset();
    sdk_fd_mtime = true;
    assert(FileDuration(track, root, [] { return true; }, &diagnostic) > 0);
    assert(diagnostic.status == DurationStatus::kKnown && diagnostic.error == 0 && fd_calls == 2);
    reset();
    assert(FileDuration(track, "relative", [] { return true; }, &diagnostic) == 0);
    assert(diagnostic.status == DurationStatus::kPath && fd_calls == 0 && reads == 0);
    reset();
    path_failure_at = 1;
    assert(FileDuration(track, root, [] { return true; }, &diagnostic) == 0);
    assert(diagnostic.status == DurationStatus::kPathStat && diagnostic.error == EIO &&
           fd_calls == 0);
    reset();
    auto mismatched = track;
    ++mismatched.track.modified_time;
    assert(FileDuration(mismatched, root, [] { return true; }, &diagnostic) == 0);
    assert(diagnostic.status == DurationStatus::kMetadata && fd_calls == 0);
    reset();
    open_error = true;
    assert(FileDuration(track, root, [] { return true; }, &diagnostic) == 0);
    assert(diagnostic.status == DurationStatus::kOpen && diagnostic.error == EACCES);
    reset();
    fd_failure_at = 1;
    assert(FileDuration(track, root, [] { return true; }, &diagnostic) == 0);
    assert(diagnostic.status == DurationStatus::kFdStat && diagnostic.error == EIO && reads == 0);
    reset();
    fd_size_changed = true;
    assert(FileDuration(track, root, [] { return true; }, &diagnostic) == 0 && reads == 0);
    assert(diagnostic.status == DurationStatus::kFdStat);
    reset();
    fd_mode_changed = true;
    assert(FileDuration(track, root, [] { return true; }, &diagnostic) == 0 && reads == 0);
    assert(diagnostic.status == DurationStatus::kFdStat);
    reset();
    read_error = true;
    assert(FileDuration(track, root, [] { return true; }, &diagnostic) == 0);
    assert(diagnostic.status == DurationStatus::kRead);
    reset();
    assert(FileDuration(track, root, [] { return false; }, &diagnostic) == 0);
    assert(diagnostic.status == DurationStatus::kTimeout && reads == 0);
    reset();
    fd_failure_at = 2;
    assert(FileDuration(track, root, [] { return true; }, &diagnostic) == 0);
    assert(diagnostic.status == DurationStatus::kFdStat && diagnostic.error == EIO && reads > 0);
    reset();
    path_failure_at = 2;
    assert(FileDuration(track, root, [] { return true; }, &diagnostic) == 0);
    assert(diagnostic.status == DurationStatus::kChanged && reads > 0);
    reset();
    int steps = 0;
    assert(FileDuration(
               track, root,
               [&] {
                   if (++steps == 2)
                       fd_size_changed = true;
                   return true;
               },
               &diagnostic) == 0);
    assert(diagnostic.status == DurationStatus::kChanged);
    reset();
    steps = 0;
    assert(FileDuration(
               track, root,
               [&] {
                   if (++steps == 2) {
                       const utimbuf changed{time_t(track.track.modified_time + 2),
                                             time_t(track.track.modified_time + 2)};
                       assert(utime(path.c_str(), &changed) == 0);
                   }
                   return true;
               },
               &diagnostic) == 0);
    assert(diagnostic.status == DurationStatus::kChanged);
    WriteMp3(path);
    const auto fresh = FixtureTrack(Catalog(root));
    reset();
    close_error = true;
    assert(FileDuration(fresh, root, [] { return true; }, &diagnostic) == 0);
    assert(diagnostic.status == DurationStatus::kClose && diagnostic.error == EIO);
    reset();
    WriteMp3(path, 65, true);
    const auto truncated = FixtureTrack(Catalog(root));
    assert(FileDuration(truncated, root, [] { return true; }, &diagnostic) == 0);
    assert(diagnostic.status == DurationStatus::kFormat);
    reset();
}
#ifndef M2_RESUME_FILE_ONLY
void WorkerCases(const std::string& root) {
    Reset();
    WriteMp3(root + "/track.mp3");
    const auto catalog = Catalog(root);
    auto track = FixtureTrack(catalog);
    const auto original_id = track.track.id;
    ResumeWorker worker(catalog, "synthetic-card");
    assert(initialized == 0 && opens == 0 && worker.Start());
    assert(initialized == 0 && opens == 0 && reads == 0);
    assert(worker.Checkpoint(track, 500, 0, 35, 100, CheckpointReason::kPause));
    track.track.id = {};
    track.track.file_name = "changed.mp3";
    assert(worker.Restore({}, ResumeMode::kBoot));
    assert(!worker.Checkpoint(track, 100, 1000, 20, 200, CheckpointReason::kPause));
    assert(sets == 0 && opens == 0 && reads == 0 && persisted.empty());
    ResumePlan result;
    assert(!worker.TakeRestore(result));
    sdk_fd_mtime = true;
    Pump();
    assert(initialized == 1 && sets == 1 && commits == 1 && erased == 0 && delays == 0 &&
           reads >= 4);
    assert(!worker.Restore({}, ResumeMode::kBoot));
    assert(worker.TakeRestore(result) && result.status == ResumeStatus::kReady);
    assert(result.target->track.id == original_id && result.position_ms == 0 &&
           result.volume_percent == 35);
    assert(!worker.TakeRestore(result));
    Reset();
}
void FailureCases(const std::string& root) {
    WriteMp3(root + "/track.mp3");
    const auto catalog = Catalog(root);
    const auto track = FixtureTrack(catalog);
    Reset();
    queue_failure = true;
    {
        ResumeWorker worker(catalog, "synthetic-card");
        assert(!worker.Start());
        assert(!worker.Checkpoint(track, 500, 1000, 35, 100, CheckpointReason::kPause));
    }
    assert(created == 1 && deleted == 0 && initialized == 0);
    Reset();
    task_failure = true;
    {
        ResumeWorker worker(catalog, "synthetic-card");
        assert(!worker.Start());
        assert(!worker.Restore({}, ResumeMode::kBoot));
    }
    assert(created == 1 && deleted == 1 && initialized == 0);
    Reset();
    init_failure = true;
    {
        ResumeWorker worker(catalog, "synthetic-card");
        assert(worker.Start());
        assert(worker.Checkpoint(track, 500, 1000, 35, 100, CheckpointReason::kPause));
        assert(worker.Restore({}, ResumeMode::kBoot));
        Pump();
        ResumePlan result;
        assert(worker.TakeRestore(result) && result.status == ResumeStatus::kStorageError);
    }
    assert(initialized == 1 && opens == 0 && sets == 0 && erased == 0 && reads == 0);
    Reset();
    commit_failure = true;
    {
        ResumeWorker worker(catalog, "synthetic-card");
        assert(worker.Start());
        assert(worker.Checkpoint(track, 500, 1000, 35, 100, CheckpointReason::kPause));
        assert(worker.Restore({}, ResumeMode::kBoot));
        Pump();
        ResumePlan result;
        assert(worker.TakeRestore(result) && result.status == ResumeStatus::kMissing);
    }
    assert(initialized == 1 && sets == 1 && commits == 1 && erased == 0 && persisted.empty());
    Reset();
    delay_step = 11000000;
    WriteMp3(root + "/track.mp3", 1025);
    const auto long_catalog = Catalog(root);
    const auto long_track = FixtureTrack(long_catalog);
    {
        ResumeWorker worker(long_catalog, "synthetic-card");
        assert(worker.Start());
        assert(worker.Checkpoint(long_track, 500, 0, 35, 100, CheckpointReason::kPause));
        assert(worker.Restore({}, ResumeMode::kBoot));
        Pump();
        ResumePlan result;
        assert(worker.TakeRestore(result) && result.status == ResumeStatus::kMissing);
    }
    assert(delays == 1 && reads > 0 && sets == 0 && erased == 0);
    Reset();
}
void CompletionCases(const std::string& root) {
    Reset();
    WriteMp3(root + "/track.mp3");
    const auto catalog = Catalog(root);
    const auto track = FixtureTrack(catalog);
    ResumeWorker worker(catalog, "synthetic-card");
    assert(worker.PendingJobs() == 0 && worker.Start());
    assert(worker.LastSaveStatus() != SaveStatus::kSaved &&
           worker.LastSaveStatus() != SaveStatus::kUnchanged);
    observed_worker = &worker;
    const auto initial_status = worker.LastSaveStatus();
    assert(worker.Checkpoint(track, 500, 1000, 35, 100, CheckpointReason::kShutdown));
    assert(worker.PendingJobs() == 1 && persisted.empty() && commits == 0);
    assert(worker.Checkpoint(track, 600, 1000, 40, 101, CheckpointReason::kPause));
    assert(worker.PendingJobs() == 2);
    assert(!worker.Checkpoint(track, 700, 1000, 45, 102, CheckpointReason::kShutdown));
    assert(worker.PendingJobs() == 2 && worker.LastSaveStatus() == initial_status);
    commit_hook = [&] {
        // Dequeue is not completion: this exact job is still counted inside nvs_commit.
        assert(worker.PendingJobs() == (commits == 1 ? 2U : 1U));
        assert(worker.LastSaveStatus() == (commits == 1 ? initial_status : SaveStatus::kSaved));
        if (commits == 1)
            assert(persisted.empty());
    };
    Pump();
    commit_hook = {};
    assert(worker.PendingJobs() == 0 && worker.LastSaveStatus() == SaveStatus::kSaved);
    assert(sent_pending == (std::vector<uint32_t>{1, 2, 3}));
    assert(received_pending == (std::vector<uint32_t>{2, 1}));
    assert(write_pending == (std::vector<uint32_t>{2, 1}));
    assert(commit_pending == (std::vector<uint32_t>{2, 1}));
    // Blob reads may also close NVS handles; none may publish a drained worker early.
    assert(close_pending.size() >= 2 && close_pending.front() == 2 && close_pending.back() == 1);
    assert(sets == 2 && commits == 2 && !persisted.empty());
    const auto saved = persisted;
    assert(worker.Checkpoint(track, 600, 1000, 40, 103, CheckpointReason::kShutdown));
    assert(worker.PendingJobs() == 1);
    Pump();
    assert(worker.PendingJobs() == 0 && worker.LastSaveStatus() == SaveStatus::kUnchanged);
    assert(sets == 2 && commits == 2 && persisted == saved);
    assert(worker.Restore({}, ResumeMode::kBoot) && worker.PendingJobs() == 1);
    Pump();
    ResumePlan plan;
    assert(worker.PendingJobs() == 0 && worker.LastSaveStatus() == SaveStatus::kUnchanged);
    assert(worker.TakeRestore(plan) && plan.status == ResumeStatus::kReady &&
           plan.target->track.id == track.track.id && plan.volume_percent == 40);
    commit_failure = true;
    assert(worker.Checkpoint(track, 700, 1000, 45, 104, CheckpointReason::kShutdown));
    Pump();
    assert(worker.PendingJobs() == 0 && worker.LastSaveStatus() == SaveStatus::kStorageError);
    assert(commits == 3 && persisted == saved && erased == 0);
    commit_failure = false;
    assert(worker.Checkpoint(track, 1001, 1000, 45, 105, CheckpointReason::kShutdown));
    Pump();
    assert(worker.PendingJobs() == 0 && worker.LastSaveStatus() == SaveStatus::kInvalidInput);
    assert(commits == 3 && persisted == saved);
    Reset();
    init_failure = true;
    ResumeWorker uninitialized(catalog, "synthetic-card");
    assert(uninitialized.Start());
    observed_worker = &uninitialized;
    assert(uninitialized.Checkpoint(track, 500, 1000, 35, 100, CheckpointReason::kShutdown));
    assert(uninitialized.PendingJobs() == 1);
    Pump();
    assert(uninitialized.PendingJobs() == 0 &&
           uninitialized.LastSaveStatus() == SaveStatus::kStorageError);
    assert(sets == 0 && commits == 0 && erased == 0);
    Reset();
}

void BoostCases(const std::string& root) {
    Reset();
    WriteMp3(root + "/track.mp3");
    const auto catalog = Catalog(root);
    const auto track = FixtureTrack(catalog);
    ResumeWorker worker(catalog, "synthetic-card");
    assert(worker.Start());
    assert(worker.Checkpoint(track, 500, 1000, 105, 100, CheckpointReason::kPause));
    Pump();
    assert(worker.LastSaveStatus() == SaveStatus::kSaved && commits == 1);
    assert(worker.Checkpoint(track, 500, 1000, 110, 101, CheckpointReason::kShutdown));
    Pump();
    assert(worker.LastSaveStatus() == SaveStatus::kSaved && commits == 2);
    const auto accepted = persisted;
    ResumeWorker restarted(catalog, "synthetic-card");
    assert(restarted.Start() && restarted.Restore({}, ResumeMode::kBoot));
    Pump();
    ResumePlan plan;
    assert(restarted.TakeRestore(plan) && plan.status == ResumeStatus::kReady &&
           plan.volume_percent == 110 && plan.target->track.id == track.track.id);
    commit_failure = true;
    assert(restarted.Checkpoint(track, 500, 1000, 200, 102, CheckpointReason::kShutdown));
    Pump();
    assert(restarted.LastSaveStatus() == SaveStatus::kStorageError && persisted == accepted &&
           erased == 0);
    commit_failure = false;
    assert(restarted.Restore({}, ResumeMode::kBoot));
    Pump();
    assert(restarted.TakeRestore(plan) && plan.volume_percent == 110);
    Reset();

    // Old v1 data keeps the original logical volume; it is never erased/replaced.
    struct MemoryBlob : ResumeBlob {
        std::vector<uint8_t> bytes;
        ResumeRead Read(std::vector<uint8_t>& out) const override {
            if (bytes.empty()) return ResumeRead::kMissing;
            out = bytes; return ResumeRead::kFound;
        }
        bool Write(const std::vector<uint8_t>& value) override { bytes = value; return true; }
    } old;
    BlobResumeStore legacy(old);
    assert(legacy.Write({1, "synthetic-card", track, 500, 1000, 35}) == ResumeWrite::kOk);
    legacy_persisted = old.bytes;
    ResumeWorker migrated(catalog, "synthetic-card");
    assert(migrated.Start() && migrated.Restore({}, ResumeMode::kBoot));
    Pump();
    assert(migrated.TakeRestore(plan) && plan.status == ResumeStatus::kReady &&
           plan.volume_percent == 35);
    assert(migrated.Checkpoint(track, 500, 1000, 100, 103, CheckpointReason::kShutdown));
    Pump();
    assert(migrated.LastSaveStatus() == SaveStatus::kSaved && legacy_persisted == old.bytes);
    assert(migrated.Restore({}, ResumeMode::kBoot));
    Pump();
    assert(migrated.TakeRestore(plan) && plan.volume_percent == 100 && erased == 0);
    Reset();
}

void BatteryCases(const std::string& root) {
    Reset();
    WriteMp3(root + "/track.mp3");
    const auto catalog = Catalog(root);
    const auto track = FixtureTrack(catalog);
    struct Source : BatterySource {
        int calls = 0, raw = 2000, pad_mv = 1400;
        BatteryReading Read() override {
            assert(pumping);
            ++calls;
            return {raw, pad_mv};
        }
    } source;
    ResumeWorker worker(catalog, "synthetic-card", &source);
    assert(worker.BatteryPercent() == -1 && worker.BatterySamples() == 0);
    assert(worker.Start() && source.calls == 0 && worker.BatteryPercent() == -1);
    assert(worker.SampleBattery() && worker.PendingJobs() == 1 && source.calls == 0);
    assert(!worker.SampleBattery() && worker.PendingJobs() == 1);
    assert(worker.Checkpoint(track, 500, 1000, 35, 100, CheckpointReason::kShutdown));
    assert(worker.PendingJobs() == 2 && source.calls == 0);
    assert(!worker.SampleBattery() && worker.PendingJobs() == 2);
    Pump();
    assert(source.calls == 1 && worker.PendingJobs() == 0 &&
           worker.LastSaveStatus() == SaveStatus::kSaved && commits == 1 &&
           worker.BatteryPercent() == 100 && worker.BatterySamples() == 1);
    source.pad_mv = 1274;
    assert(worker.SampleBattery() && source.calls == 1 && worker.BatteryPercent() == 100);
    Pump();
    assert(source.calls == 2 && worker.BatteryPercent() == 92);
    source.pad_mv = 1400;
    assert(worker.SampleBattery());
    Pump();
    assert(source.calls == 3 && worker.BatteryPercent() == 93);
    source.pad_mv = -1;  // Raw ADC may succeed while calibration is unavailable.
    assert(worker.SampleBattery());
    Pump();
    assert(source.calls == 4 && worker.BatteryPercent() == -1);
    source.pad_mv = 1400;
    assert(worker.SampleBattery());
    Pump();
    assert(source.calls == 5 && worker.BatteryPercent() == 100 && worker.BatterySamples() == 5);
    assert(worker.Checkpoint(track, 600, 1000, 40, 101, CheckpointReason::kShutdown));
    assert(!worker.SampleBattery() && worker.PendingJobs() == 1 && source.calls == 5);
    Pump();
    source.raw = -1;
    assert(worker.SampleBattery() && source.calls == 5);
    Pump();
    assert(source.calls == 6 && worker.PendingJobs() == 0 && worker.BatteryPercent() == -1);
    assert(!worker.SampleBattery() && source.calls == 6 && worker.PendingJobs() == 0);
    assert(worker.Checkpoint(track, 700, 1000, 45, 102, CheckpointReason::kShutdown));
    Pump();
    assert(source.calls == 6 && worker.LastSaveStatus() == SaveStatus::kSaved && commits == 3 &&
           erased == 0 && worker.BatteryPercent() == -1);
    Reset();
}
#endif
int main(int argc, char** argv) {
    assert(argc == 3);
    const std::string mode(argv[1]);
    synthetic_root = argv[2];
    if (mode == "files") {
        FileCases(synthetic_root);
        WavCases(synthetic_root);
    } else if (mode == "sdkstat")
        SdkStatCases(synthetic_root);
    else if (mode == "diagnostics")
        DurationDiagnostics(synthetic_root);
#ifndef M2_RESUME_FILE_ONLY
    else if (mode == "worker")
        WorkerCases(synthetic_root);
    else if (mode == "failures")
        FailureCases(synthetic_root);
    else if (mode == "completion")
        CompletionCases(synthetic_root);
    else if (mode == "boost")
        BoostCases(synthetic_root);
    else if (mode == "battery")
        BatteryCases(synthetic_root);
#endif
    else
        return 2;
    std::cout << mode << " PASS\n";
}
