#pragma once
#include <algorithm>
#include "m2_library_runtime.h"
#include "p5_1_ui_screens.h"

// P5-1: the player page (or the directory list) from the runtime's view. The
// caller adds the AI badge, notices and the conversation.
namespace boilon::ui {
inline Model PlayerModel(const local_library::PlayerView& view, uint64_t position_ms,
                         int battery) {
    Model m;
    m.battery = battery < 0 ? -1 : std::min(battery, 100);
    if (view.directory_open) {
        m.page = Page::kDirectory;
        for (size_t i = 0; i < view.directories.size(); ++i)
            m.directories.emplace_back(
                view.directories[i], i < view.directory_files.size() ? view.directory_files[i] : 0);
        m.highlighted = view.highlighted;
        m.directory_left_ms = view.directory_left_ms;
        return m;
    }
    static constexpr Play kPlay[] = {Play::kPlaying,  Play::kPaused, Play::kLocating,
                                     Play::kSwitching, Play::kFinished, Play::kError,
                                     Play::kFinished};
    m.play = kPlay[static_cast<int>(view.state)];
    m.title = view.title;
    m.collection = view.collection;
    m.index = view.track_number;
    m.count = view.track_count;
    m.position_ms = position_ms;
    m.duration_ms = view.duration_ms;
    m.volume = view.volume;
    m.show_volume = view.volume_visible;
    m.seek_seconds = view.seek_seconds;
    m.seek_target_ms = position_ms;
    static_assert(static_cast<int>(media::PlayMode::kShuffle) ==
                  static_cast<int>(PlayMode::kShuffle));
    m.mode = static_cast<PlayMode>(view.mode);
    if (view.offline_hint)
        m.toast = Toast::kAiOffline;
    return m;
}
}  // namespace boilon::ui
