#pragma once
#include "m2_library_policy.h"
#include "media/playback_resume.h"

namespace boilon::local_library {
// Call from the single player owner, never an audio callback. A real caller must
// supply trusted duration metadata while EOF duration is still unknown.
template <typename Player>
media::SaveStatus SaveProgress(media::PlaybackResume& resume, const media::ResolvedTrack& track,
                               Player& player, uint64_t known_duration_ms, int volume,
                               uint64_t now, media::CheckpointReason reason) {
    return resume.Save(track.track.id, player.PositionMs(), known_duration_ms, volume, now, reason);
}

template <typename Player>
bool StartResume(const media::ResumePlan& plan, const std::string& root, Player& player) {
    if (plan.status != media::ResumeStatus::kReady || !plan.target || !plan.auto_play ||
        player.Running() || !Root(root)) return false;
    const auto& target = *plan.target;
    if (!Segment(target.track.file_name) || !media::IsPlayableAudioName(target.track.file_name) ||
        (!target.collection_is_root && !Segment(target.directory_name))) return false;
    const auto path = root + "/" + (target.collection_is_root ? "" : target.directory_name + "/") + target.track.file_name;
    return path.size() <= 512 && player.StartAt(path, plan.position_ms);
}
}  // namespace boilon::local_library
