#include "media/playback_resume.h"

#include <utility>

namespace media {
namespace {

bool SameMedia(const ResolvedTrack& left, const ResolvedTrack& right) {
    return left.collection_id == right.collection_id &&
           left.collection_is_root == right.collection_is_root &&
           left.directory_name == right.directory_name && left.track.id == right.track.id &&
           left.track.file_name == right.track.file_name &&
           left.track.size_bytes == right.track.size_bytes &&
           left.track.modified_time == right.track.modified_time;
}

bool SameRecord(const ResumeRecord& left, const ResumeRecord& right) {
    return left.version == right.version && left.library_key == right.library_key &&
           SameMedia(left.media, right.media) && left.position_ms == right.position_ms &&
           left.duration_ms == right.duration_ms && left.volume_percent == right.volume_percent;
}

bool ValidPosition(uint64_t position, uint64_t duration, int volume) {
    return duration > 0 && position <= duration && volume >= 0 && volume <= 100;
}

ResumePlan Rejected(ResumeStatus status) {
    ResumePlan result;
    result.status = status;
    return result;
}

}  // namespace

PlaybackResume::PlaybackResume(MediaCatalog catalog, std::string library_key, ResumeStore& store)
    : catalog_(std::move(catalog)), library_key_(std::move(library_key)), store_(store) {}

bool PlaybackResume::ValidLibrary() const { return !library_key_.empty() && library_key_.size() <= 256; }

SaveStatus PlaybackResume::Save(MediaId track, uint64_t position_ms, uint64_t duration_ms,
                                int volume_percent, uint64_t now_ms, CheckpointReason reason) {
    if (!ValidLibrary() || !ValidPosition(position_ms, duration_ms, volume_percent)) {
        return SaveStatus::kInvalidInput;
    }
    const auto media = catalog_.Resolve(track);
    if (!media) {
        return SaveStatus::kInvalidInput;
    }
    if (last_seen_ms_ && now_ms < *last_seen_ms_) {
        return SaveStatus::kInvalidTime;
    }
    last_seen_ms_ = now_ms;
    if (reason == CheckpointReason::kPeriodic && last_attempt_ms_ &&
        now_ms - *last_attempt_ms_ < kCheckpointMs) {
        return SaveStatus::kNotDue;
    }
    last_attempt_ms_ = now_ms;
    ResumeRecord record{1, library_key_, *media, position_ms, duration_ms, volume_percent};
    if (confirmed_ && SameRecord(*confirmed_, record)) {
        return SaveStatus::kUnchanged;
    }
    const auto written = store_.Write(record);
    if (written != ResumeWrite::kOk) {
        return written == ResumeWrite::kFull ? SaveStatus::kFull : SaveStatus::kStorageError;
    }
    confirmed_ = std::move(record);
    last_success_ms_ = now_ms;
    return SaveStatus::kSaved;
}

ResumePlan PlaybackResume::Restore(std::optional<MediaId> collection, ResumeMode mode) const {
    if (!ValidLibrary()) {
        return Rejected(ResumeStatus::kInvalidRecord);
    }
    ResumeRecord record;
    const auto read = store_.Read(library_key_, collection, record);
    if (read == ResumeRead::kMissing) {
        return Rejected(ResumeStatus::kMissing);
    }
    if (read != ResumeRead::kFound) {
        return Rejected(ResumeStatus::kStorageError);
    }
    if (record.version != 1 || record.library_key != library_key_ ||
        record.media.track.id.version != kMediaIdSchemeVersion ||
        record.media.collection_id.version != kMediaIdSchemeVersion ||
        (collection && record.media.collection_id != *collection) ||
        !ValidPosition(record.position_ms, record.duration_ms, record.volume_percent)) {
        return Rejected(ResumeStatus::kInvalidRecord);
    }
    const auto current = catalog_.Resolve(record.media.track.id);
    if (!current || !SameMedia(*current, record.media)) {
        return Rejected(ResumeStatus::kStaleMedia);
    }
    ResumePlan result;
    result.status = ResumeStatus::kReady;
    result.target = current;
    result.position_ms = record.position_ms;
    if (mode == ResumeMode::kBoot) {
        result.position_ms = record.position_ms > kRewindMs ? record.position_ms - kRewindMs : 0;
        result.volume_percent = record.volume_percent;
        result.auto_play = true;
    }
    return result;
}

}  // namespace media
