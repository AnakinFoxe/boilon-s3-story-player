#pragma once
#include <cerrno>
#include <climits>
#include <cstdio>
#include "m2_battery_estimate.h"
#include "m2_battery_source.h"
#include "m2_library_policy.h"
#include "m2_mp3_duration.h"
#include "m2_resume_runtime.h"
#include "media/wav_format.h"

namespace boilon::local_library {
enum class DurationStatus {
    kKnown,
    kPath,
    kPathStat,
    kMetadata,
    kOpen,
    kFdStat,
    kRead,
    kTimeout,
    kFormat,
    kChanged,
    kClose
};
struct DurationDiagnostic {
    DurationStatus status = DurationStatus::kKnown;
    int error = 0;
};
// Worker-only read-only scan, with a bounded 8 KiB cache. Yield/budget is supplied
// by the caller. Catalog size/mtime must match both before and after scanning.
template <typename Continue>
uint64_t FileDuration(const media::ResolvedTrack& track, const std::string& root, Continue proceed,
                      DurationDiagnostic* diagnostic = nullptr) {
    auto fail = [&](DurationStatus status, int error = 0) -> uint64_t {
        if (diagnostic)
            *diagnostic = {status, error};
        return 0;
    };
    if (diagnostic)
        *diagnostic = {};
    if (!Root(root) || !Segment(track.track.file_name) ||
        (!track.collection_is_root && !Segment(track.directory_name)))
        return fail(DurationStatus::kPath);
    const auto path = root + "/" + (track.collection_is_root ? "" : track.directory_name + "/") +
                      track.track.file_name;
    auto matches = [&](const struct stat& info) {
        return S_ISREG(info.st_mode) && info.st_size > 0 && info.st_mtime >= 0 &&
               uint64_t(info.st_size) == track.track.size_bytes &&
               uint64_t(info.st_mtime) == track.track.modified_time;
    };
    struct stat info{};
    if (path.size() > 512)
        return fail(DurationStatus::kPath);
    if (EntryStat(path.c_str(), &info) != 0)
        return fail(DurationStatus::kPathStat, errno);
    if (!matches(info))
        return fail(DurationStatus::kMetadata);
    std::unique_ptr<FILE, decltype(&fclose)> file(fopen(path.c_str(), "rb"), fclose);
    if (!file)
        return fail(DurationStatus::kOpen, errno);
    // ESP-IDF FAT fstat deliberately reports mtime=0. Only path stat has the
    // actual FAT timestamp; check it before AND after the scan, not on the FD.
    auto fd_matches = [&](const struct stat& value) {
        return S_ISREG(value.st_mode) && value.st_size > 0 &&
               uint64_t(value.st_size) == track.track.size_bytes;
    };
    if (fstat(fileno(file.get()), &info) != 0)
        return fail(DurationStatus::kFdStat, errno);
    if (!fd_matches(info))
        return fail(DurationStatus::kFdStat);
    std::vector<uint8_t> cache(8192);
    uint64_t begin = 0;
    size_t cached = 0;
    Mp3Duration duration(track.track.size_bytes);
    bool cancelled = false, read_failed = false;
    int read_error = 0;
    auto read = [&](uint64_t offset, uint8_t* out, size_t count) {
            if (offset > LONG_MAX || offset > track.track.size_bytes ||
                count > track.track.size_bytes - offset)
                return false;
            if (!cached || offset < begin || offset + count > begin + cached) {
                if (fseek(file.get(), static_cast<long>(offset), SEEK_SET) != 0) {
                    read_failed = true;
                    read_error = errno;
                    return false;
                }
                begin = offset;
                cached = fread(cache.data(), 1,
                               std::min<uint64_t>(cache.size(), track.track.size_bytes - offset),
                               file.get());
                if (ferror(file.get()) || cached < count) {
                    read_failed = true;
                    read_error = ferror(file.get()) ? errno : 0;
                    return false;
                }
            }
            std::memcpy(out, cache.data() + (offset - begin), count);
            return true;
    };
    const bool permitted = proceed();
    // F08: a WAV's header gives its length; an MP3 may carry it in a Xing/Info frame.
    const bool is_wav = media::IsWavName(track.track.file_name);
    media::WavFormat wav;
    const uint64_t header_duration =
        !permitted ? 0
        : is_wav   ? (media::ParseWav(track.track.size_bytes, read, wav) ? wav.DurationMs() : 0)
                   : duration.HeaderDuration(read);
    if (!permitted) {
        duration.Cancel();
        cancelled = true;
    }
    while (!cancelled && !header_duration && !is_wav &&
           duration.State() == Mp3Duration::Status::kScanning) {
        if (!proceed()) {
            duration.Cancel();
            cancelled = true;
            break;
        }
        duration.Step(read);
    }
    const int fd_result = fstat(fileno(file.get()), &info);
    const int fd_error = fd_result ? errno : 0;
    const bool unchanged =
        fd_result == 0 && fd_matches(info) && EntryStat(path.c_str(), &info) == 0 && matches(info);
    const int closed = fclose(file.release());
    if (fd_result != 0)
        return fail(DurationStatus::kFdStat, fd_error);
    if (!unchanged)
        return fail(DurationStatus::kChanged);
    if (closed != 0)
        return fail(DurationStatus::kClose, errno);
    if (cancelled)
        return fail(DurationStatus::kTimeout);
    if (read_failed)
        return fail(DurationStatus::kRead, read_error);
    if (!header_duration && !duration.DurationMs())
        return fail(DurationStatus::kFormat);
    return header_duration ? header_duration : duration.DurationMs();
}
}  // namespace boilon::local_library

#ifdef ESP_PLATFORM
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <nvs_flash.h>
#include <atomic>
#include <mutex>
#include <new>
#include "m2_file_bookmarks.h"
#include "m2_resume_nvs.h"

namespace boilon::local_library {
// Explicit opt-in only. One active + two queued snapshots; no UI/audio I/O.
// Objects outlive the task. OFF remains independent of this worker, even if an
// SDK storage call blocks. Successful enqueue is never reported as a saved point.
class ResumeWorker final : public ResumePort {
public:
    ResumeWorker(media::MediaCatalog catalog, std::string library, BatterySource* battery = nullptr)
        : battery_(battery),
          library_(library),
          store_(blob_, legacy_, catalog),
          resume_(std::move(catalog), std::move(library), store_) {}
    uint32_t PendingJobs() const { return pending_.load(); }
    media::SaveStatus LastSaveStatus() const { return last_save_.load(); }
    int BatteryPercent() const { return battery_percent_.load(); }
    // Counts finished samples, so the owner can tell a new reading from an old one.
    uint32_t BatterySamples() const { return battery_samples_.load(); }
    bool SampleBattery() {
        // Only sample when idle; leave queue space for shutdown/save.
        if (!battery_ || battery_failed_.load() || pending_.load() != 0)
            return false;
        auto job = std::unique_ptr<Job>(new (std::nothrow) Job);
        if (!job)
            return false;
        job->sample_battery = true;
        return Enqueue(std::move(job));
    }
    bool Start() {
        queue_ = xQueueCreate(2, sizeof(Job*));
        if (!queue_)
            return false;
        if (xTaskCreate([](void* self) { static_cast<ResumeWorker*>(self)->Run(); }, "local_resume",
                        12288, this, 2, nullptr) == pdPASS)
            return true;
        vQueueDelete(queue_);
        queue_ = nullptr;
        return false;
    }
    bool Restore(std::optional<media::MediaId> collection, media::ResumeMode mode) override {
        std::unique_lock<std::mutex> lock(result_lock_, std::try_to_lock);
        if (!lock || ready_)
            return false;
        auto job = std::unique_ptr<Job>(new (std::nothrow) Job);
        if (!job)
            return false;
        job->collection = collection;
        job->mode = mode;
        return Enqueue(std::move(job));
    }
    bool TakeRestore(media::ResumePlan& plan) override {
        std::unique_lock<std::mutex> lock(result_lock_, std::try_to_lock);
        if (!lock || !ready_)
            return false;
        plan = std::move(result_);
        ready_ = false;
        return true;
    }
    bool Checkpoint(const media::ResolvedTrack& track, uint64_t position, uint64_t duration,
                    int volume, uint64_t now, media::CheckpointReason reason) override {
        auto job = std::unique_ptr<Job>(new (std::nothrow) Job);
        if (!job)
            return false;
        job->track = track;
        job->position = position;
        job->duration = duration;
        job->volume = volume;
        job->now = now;
        job->reason = reason;
        return Enqueue(std::move(job));
    }
    // 2026-10-16: deletes one collection's bookmarks on this card, or (none)
    // every bookmark on the device, after the jobs already queued. Until then
    // KnownPosition answers "not yet", so no forgotten place is used again.
    bool Forget(std::optional<media::MediaId> collection) override {
        auto job = std::unique_ptr<Job>(new (std::nothrow) Job);
        if (!job)
            return false;
        job->forget = true;
        job->collection = collection;
        ++forgetting_;
        if (Enqueue(std::move(job)))
            return true;
        --forgetting_;
        return false;
    }

    uint64_t KnownDuration(media::MediaId track) override {
        std::unique_lock<std::mutex> lock(result_lock_, std::try_to_lock);
        return lock && duration_track_ && *duration_track_ == track ? duration_ms_ : 0;
    }
    std::optional<uint64_t> KnownPosition(media::MediaId track) override {
        if (forgetting_.load())
            return std::nullopt;  // Asked again once the bookmarks are gone.
        std::unique_lock<std::mutex> lock(result_lock_, std::try_to_lock);
        if (lock)
            for (const auto& item : positions_)
                if (item.first == track)
                    return item.second;
        return lock ? std::optional<uint64_t>{0} : std::nullopt;
    }

private:
    struct Job {
        bool sample_battery = false, forget = false;
        std::optional<media::ResolvedTrack> track;
        std::optional<media::MediaId> collection;
        media::ResumeMode mode = media::ResumeMode::kBoot;
        uint64_t position = 0, duration = 0, now = 0;
        int volume = 0;
        media::CheckpointReason reason = media::CheckpointReason::kPeriodic;
    };
    bool Enqueue(std::unique_ptr<Job> job) {
        auto* raw = job.get();
        ++pending_;  // Count in-flight work, not just queued pointers.
        if (!queue_ || xQueueSend(queue_, &raw, 0) != pdTRUE) {
            --pending_;
            return false;
        }
        job.release();
        return true;
    }
    void Run() {
        // An enabled candidate initializes the existing partition once; any
        // error stays an error. In particular, NEVER erase on init failure.
        const bool initialized = nvs_flash_init() == ESP_OK;
        std::optional<media::MediaId> cached_track;
        uint64_t cached_duration = 0;
        uint64_t scan_elapsed_ms = 0;
        DurationDiagnostic scan_diagnostic;
        while (true) {
            Job* raw = nullptr;
            if (xQueueReceive(queue_, &raw, portMAX_DELAY) != pdTRUE)
                continue;
            std::unique_ptr<Job> job(raw);
            if (job->sample_battery) {
                const auto reading = battery_->Read();
                battery_failed_ = reading.raw < 0;
                const int percent = battery_estimate_.Update(reading);
                battery_percent_ = percent;
                ++battery_samples_;
                ESP_LOGI("BoilonBattery",
                         "raw=%d adc_pad_mv=%d battery_mv=%d divider=3 percent=%d "
                         "model=factory_estimate",
                         reading.raw, reading.adc_pad_mv, BatteryEstimate::Millivolts(reading),
                         percent);
            } else if (job->forget) {
                const auto status = initialized ? store_.Forget(library_, job->collection)
                                                : media::ResumeWrite::kError;
                resume_.ForgetLastSave();
                auto positions = store_.Positions(library_);
                {
                    std::lock_guard<std::mutex> lock(result_lock_);
                    positions_ = std::move(positions);
                }
                --forgetting_;
                ESP_LOGI("BoilonResume", "Forget all=%d status=%d", !job->collection,
                         static_cast<int>(status));
            } else if (!job->track) {
                auto plan =
                    initialized ? resume_.Restore(job->collection, job->mode) : media::ResumePlan{};
                if (!initialized)
                    plan.status = media::ResumeStatus::kStorageError;
                if (plan.status == media::ResumeStatus::kReady &&
                    job->mode == media::ResumeMode::kBoot)
                    plan.volume_percent = store_.LastReadVolume();
                std::lock_guard<std::mutex> lock(result_lock_);
                positions_ = store_.Positions(library_);
                result_ = std::move(plan);
                ready_ = true;
            } else if (initialized) {
                if (!job->duration && (!cached_track || *cached_track != job->track->track.id)) {
                    const int64_t scan_begin = esp_timer_get_time();
                    const int64_t deadline = scan_begin + 10000000;
                    unsigned steps = 0;
                    cached_track = job->track->track.id;
                    cached_duration = FileDuration(
                        *job->track, "/sdcard",
                        [&] {
                            // 512 frame headers per yield, not a tick every 16.
                            if (++steps % 32 == 0)
                                vTaskDelay(1);
                            return esp_timer_get_time() < deadline;
                        },
                        &scan_diagnostic);
                    scan_elapsed_ms = (esp_timer_get_time() - scan_begin) / 1000;
                }
                const auto duration = job->duration ? job->duration : cached_duration;
                {
                    std::lock_guard<std::mutex> lock(result_lock_);
                    duration_track_ = job->track->track.id;
                    duration_ms_ = duration;
                }
                // Five-point UI steps map distinctly into the unchanged shared 0–100 policy.
                // The board store persists/restores the original logical value.
                const auto status =
                    store_.WriteVolume(job->volume)
                        ? resume_.Save(job->track->track.id, job->position, duration,
                                       job->volume / 2, job->now, job->reason)
                        : media::SaveStatus::kInvalidInput;
                {
                    auto positions = store_.Positions(library_);
                    std::lock_guard<std::mutex> lock(result_lock_);
                    positions_ = std::move(positions);
                }
                last_save_ = status;  // Save includes the actual NVS commit.
                ESP_LOGI("BoilonResume",
                         "Checkpoint reason=%d position_ms=%llu duration_ms=%llu scan=%d error=%d "
                         "elapsed_ms=%llu",
                         static_cast<int>(job->reason),
                         static_cast<unsigned long long>(job->position),
                         static_cast<unsigned long long>(duration),
                         static_cast<int>(job->duration ? DurationStatus::kKnown
                                                        : scan_diagnostic.status),
                         job->duration ? 0 : scan_diagnostic.error,
                         static_cast<unsigned long long>(job->duration ? 0 : scan_elapsed_ms));
                ESP_LOGI("BoilonResume", "Checkpoint status=%d", static_cast<int>(status));
            } else {
                last_save_ = media::SaveStatus::kStorageError;
                ESP_LOGW("BoilonResume", "Checkpoint rejected: NVS init failed; no erase/retry");
            }
            --pending_;
        }
    }
    BatterySource* battery_;
    BatteryEstimate battery_estimate_;
    std::atomic<int> battery_percent_{-1};
    std::atomic<uint32_t> battery_samples_{0};
    std::atomic<bool> battery_failed_{false};
    std::string library_;
    std::vector<std::pair<media::MediaId, uint64_t>> positions_;
    NvsResumeBlob blob_{"state_v2"}, legacy_blob_;
    BlobResumeStore legacy_{legacy_blob_};
    FileBookmarks store_;
    media::PlaybackResume resume_;
    QueueHandle_t queue_ = nullptr;
    std::mutex result_lock_;
    media::ResumePlan result_;
    std::optional<media::MediaId> duration_track_;
    uint64_t duration_ms_ = 0;
    bool ready_ = false;
    std::atomic<uint32_t> pending_{0}, forgetting_{0};
    std::atomic<media::SaveStatus> last_save_{media::SaveStatus::kStorageError};
};
}  // namespace boilon::local_library
#endif
