#ifndef MEDIA_PLAYBACK_RESUME_H_
#define MEDIA_PLAYBACK_RESUME_H_

#include "media/media_catalog.h"

#include <cstdint>
#include <optional>
#include <string>

namespace media {

// Typed logical data only, NOT a serialized NVS/blob format or struct ABI.
struct ResumeRecord {
    uint32_t version = 1;
    std::string library_key;
    ResolvedTrack media;
    uint64_t position_ms = 0;
    uint64_t duration_ms = 0;
    int volume_percent = 0;
};

enum class ResumeRead { kFound, kMissing, kError };
enum class ResumeWrite { kOk, kFull, kError };

// A bounded store: one record per (library, collection). Empty collection
// reads the library's last successful write. No erase/format operation.
// Only the host memory fake exists; real serialization/atomicity is deferred.
class ResumeStore {
public:
    virtual ~ResumeStore() = default;
    virtual ResumeRead Read(const std::string& library, std::optional<MediaId> collection,
                            ResumeRecord& record) const = 0;
    virtual ResumeWrite Write(const ResumeRecord& record) = 0;
};

enum class CheckpointReason { kPeriodic, kPause, kTrackChange, kCollectionChange, kShutdown };
enum class SaveStatus { kSaved, kUnchanged, kNotDue, kInvalidInput, kInvalidTime, kFull, kStorageError };
enum class ResumeMode { kSavedExact, kBoot };
enum class ResumeStatus { kReady, kMissing, kInvalidRecord, kStaleMedia, kStorageError };

struct ResumePlan {
    ResumeStatus status = ResumeStatus::kMissing;
    std::optional<ResolvedTrack> target;
    uint64_t position_ms = 0;
    std::optional<int> volume_percent;
    bool auto_play = false;
};

// Synchronous host-testable policy. Caller supplies the audible cursor (not
// decoded/queued position) and duration, and must not block an audio task with
// a future real store. Store must outlive this object; catalog is owned/static.
class PlaybackResume {
public:
    static constexpr uint64_t kCheckpointMs = 30000;
    static constexpr uint64_t kRewindMs = 5000;

    PlaybackResume(MediaCatalog catalog, std::string library_key, ResumeStore& store);
    SaveStatus Save(MediaId track, uint64_t position_ms, uint64_t duration_ms, int volume_percent,
                    uint64_t now_ms, CheckpointReason reason);
    ResumePlan Restore(std::optional<MediaId> collection, ResumeMode mode) const;
    std::optional<uint64_t> LastSuccessfulSaveMs() const { return last_success_ms_; }
    // The store forgot records behind this policy (2026-10-16): the next save
    // writes even when it repeats the last one.
    void ForgetLastSave() { confirmed_.reset(); }

private:
    bool ValidLibrary() const;

    MediaCatalog catalog_;
    std::string library_key_;
    ResumeStore& store_;
    std::optional<uint64_t> last_seen_ms_;
    std::optional<uint64_t> last_attempt_ms_;
    std::optional<uint64_t> last_success_ms_;
    std::optional<ResumeRecord> confirmed_;
};

}  // namespace media

#endif  // MEDIA_PLAYBACK_RESUME_H_
