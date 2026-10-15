#include "media/player_controller.h"
#include "media/player_gestures.h"
#include "media/playback_resume.h"
#include "resume_memory_store.h"

#include <iostream>
#include <utility>

namespace {

const char* StateName(media::PlaybackState state) {
    switch (state) {
        case media::PlaybackState::kStopped:
            return "Stopped";
        case media::PlaybackState::kPlaying:
            return "Playing";
        case media::PlaybackState::kPaused:
            return "Paused";
    }
    return "Unknown";
}

const char* ActionName(media::PlaybackAction action) {
    switch (action) {
        case media::PlaybackAction::kNone:
            return "None";
        case media::PlaybackAction::kStart:
            return "Start";
        case media::PlaybackAction::kPause:
            return "Pause";
        case media::PlaybackAction::kResume:
            return "Resume";
        case media::PlaybackAction::kStop:
            return "Stop";
    }
    return "Unknown";
}

void Show(const media::PlayerController& player, const char* command, media::ControlResult result) {
    const auto current = player.CurrentTrack();
    std::cout << command << " | state=" << StateName(player.State())
              << " action=" << ActionName(result.action);
    if (current) {
        std::cout << " | " << (current->collection_is_root ? "未分类" : current->directory_name)
                  << " / " << current->track.file_name;
    }
    std::cout << '\n';
}

void DemonstrateGestures(media::PlayerController& player, media::MediaId stories) {
    std::cout << "三键手势→生产控制器：虚拟时间，无需按键；其他功能只打印请求。\n";
    player.SelectCollection(stories);
    player.Play();
    media::PlayerGestures gestures;
    uint64_t now_ms = 0;
    const auto feed = [&](uint8_t pressed, uint64_t duration_ms) {
        static constexpr const char* names[] = {
            "None", "TogglePlayback", "OpenAi", "OfflineHint", "ShutdownRequest",
            "VolumeUp", "VolumeDown", "NextTrack", "PreviousTrack", "OpenDirectory",
            "ParentPageRequest", "BrowseNext", "BrowsePrevious", "ConfirmCollection", "CancelAi",
        };
        for (uint64_t elapsed = 0; elapsed < duration_ms; elapsed += 10, now_ms += 10) {
            const auto action = gestures.Update(pressed, now_ms);
            if (action == media::GestureAction::kNone) {
                continue;
            }
            std::cout << "gesture=" << names[static_cast<size_t>(action)] << ' ';
            if (action == media::GestureAction::kTogglePlayback) {
                Show(player, "三键", player.State() == media::PlaybackState::kPlaying
                                          ? player.Pause() : player.Play());
            } else if (action == media::GestureAction::kNextTrack) {
                Show(player, "三键", player.Next());
            } else if (action == media::GestureAction::kPreviousTrack) {
                Show(player, "三键", player.Previous());
            } else {
                std::cout << "state=" << StateName(player.State()) << " request_only\n";
            }
        }
    };
    feed(0, 100);
    feed(media::PlayerGestures::kPower, 100);
    feed(0, 600);
    feed(media::PlayerGestures::kPower, 100);
    feed(0, 600);
    feed(media::PlayerGestures::kUp, 1000);
    feed(0, 100);
    feed(media::PlayerGestures::kDown, 1000);
    feed(0, 100);
    feed(media::PlayerGestures::kPower, 100);
    feed(0, 120);
    feed(media::PlayerGestures::kPower, 100);
    feed(0, 600);
    feed(media::PlayerGestures::kUp, 100);
    feed(media::PlayerGestures::kUp | media::PlayerGestures::kDown, 600);
    feed(0, 100);
    feed(media::PlayerGestures::kUp | media::PlayerGestures::kDown, 5200);
    feed(0, 100);
    feed(media::PlayerGestures::kPower, 2200);
    feed(0, 100);
}

void ShowResume(const char* label, const media::ResumePlan& plan) {
    static constexpr const char* names[] = {
        "Ready", "Missing", "InvalidRecord", "StaleMedia", "StorageError",
    };
    std::cout << "resume=" << label << " status=" << names[static_cast<size_t>(plan.status)]
              << " position_ms=" << plan.position_ms << " auto_play=" << plan.auto_play;
    if (plan.target) {
        std::cout << " track=" << plan.target->track.file_name;
    }
    std::cout << " volume=" << (plan.volume_percent ? std::to_string(*plan.volume_percent)
                                                   : "keep_current") << '\n';
}

bool DemonstrateResume(const media::MediaCatalog& catalog, media::MediaId stories, media::MediaId root) {
    std::cout << "断点续播演示：只用内存存储和合成可听位置，不读取音频，不写NVS。\n";
    media::MediaId story_track;
    media::MediaId music_track;
    for (const auto& collection : catalog.Collections()) {
        if (collection.id == stories) {
            story_track = collection.tracks.front().id;
        } else if (collection.id == root) {
            music_track = collection.tracks.front().id;
        }
    }
    MemoryResumeStore store;
    media::PlaybackResume resume(catalog, "synthetic-demo-card", store);
    using Reason = media::CheckpointReason;
    using Saved = media::SaveStatus;
    if (resume.Save(story_track, 60000, 600000, 35, 0, Reason::kPeriodic) != Saved::kSaved ||
        resume.Save(story_track, 89999, 600000, 35, 29999, Reason::kPeriodic) != Saved::kNotDue ||
        resume.Save(story_track, 90000, 600000, 35, 30000, Reason::kPeriodic) != Saved::kSaved ||
        resume.Save(story_track, 91000, 600000, 35, 31000, Reason::kPause) != Saved::kSaved ||
        resume.Save(music_track, 45000, 300000, 40, 31001, Reason::kCollectionChange) != Saved::kSaved) {
        return false;
    }
    std::cout << "checkpoint=periodic_and_pause PASS writes=" << store.write_attempts << '\n';
    ShowResume("return_story", resume.Restore(stories, media::ResumeMode::kSavedExact));
    media::PlaybackResume reboot(catalog, "synthetic-demo-card", store);
    ShowResume("boot_last", reboot.Restore(std::nullopt, media::ResumeMode::kBoot));
    store.write_error = true;
    if (resume.Save(music_track, 75000, 300000, 40, 61001, Reason::kPeriodic) != Saved::kStorageError ||
        resume.LastSuccessfulSaveMs() != std::optional<uint64_t>{31001}) {
        return false;
    }
    std::cout << "checkpoint=write_failed old_success_ms=31001; 35秒上限此时不能保证\n";
    ShowResume("after_failed_save", reboot.Restore(std::nullopt, media::ResumeMode::kBoot));
    // A changed fingerprint is rejected; this modifies only the host fake.
    ++store.records.back().media.track.modified_time;
    ShowResume("changed_fingerprint", reboot.Restore(std::nullopt, media::ResumeMode::kBoot));
    return true;
}

}  // namespace

int main() {
    std::cout << "播放控制演示：仅生产核心的状态与请求，不解码、不发声、不访问设备。\n";
    media::MediaCatalogBuilder builder("synthetic-demo-card");
    for (const auto* name : {"第10集.mp3", "第2集.mp3", "第1集.mp3"}) {
        if (builder.Add({"睡前故事", name, 100, 1}) != media::AddResult::kAdded) {
            return 1;
        }
    }
    if (builder.Add({"", "音乐.mp3", 100, 1}) != media::AddResult::kAdded) {
        return 1;
    }
    auto built = builder.Finish();
    if (!built.Ok()) {
        return 1;
    }
    media::MediaId stories;
    media::MediaId root;
    for (const auto& collection : built.catalog.Collections()) {
        (collection.is_root ? root : stories) = collection.id;
        std::cout << "目录 " << (collection.is_root ? "未分类" : collection.directory_name) << ':';
        for (const auto& track : collection.tracks) {
            std::cout << ' ' << track.file_name;
        }
        std::cout << '\n';
    }
    if (!stories.IsValid() || !root.IsValid()) {
        return 1;
    }
    if (!DemonstrateResume(built.catalog, stories, root)) {
        return 1;
    }
    media::PlayerController player(std::move(built.catalog));
    const auto& collections = player.Catalog().Collections();
    size_t stories_index = 0;
    while (stories_index < collections.size() && collections[stories_index].id != stories) {
        ++stories_index;
    }
    Show(player, "选择故事目录", player.SelectCollection(stories));
    Show(player, "开始播放", player.Play());
    Show(player, "暂停", player.Pause());
    Show(player, "继续同一首", player.Play());
    Show(player, "下一集", player.Next());
    Show(player, "上一集", player.Previous());
    Show(player, "第1集结束", player.TrackFinished());
    Show(player, "第2集结束", player.TrackFinished());
    Show(player, "目录播完：默认停止", player.TrackFinished());
    player.SetMode(stories_index, media::PlayMode::kLoop);
    Show(player, "重新播放最后一首", player.Play());
    Show(player, "目录播完：循环到第1集", player.TrackFinished());
    Show(player, "暂停", player.Pause());
    Show(player, "暂停时切到第2集", player.Next());
    Show(player, "从新曲开头开始（不是Resume）", player.Play());
    Show(player, "切换到未分类目录", player.SelectCollection(root));
    Show(player, "播放音乐", player.Play());  // 每个目录各有模式：未分类还是顺序播放。
    Show(player, "单曲目录结束", player.TrackFinished());
    DemonstrateGestures(player, stories);
    std::cout << "演示结束。真实音频、物理按键、NVS续播和固件集成尚未接入。\n";
    return 0;
}
