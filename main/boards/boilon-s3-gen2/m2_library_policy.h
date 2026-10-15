#pragma once

#include "media/player_controller.h"
#include <cerrno>
#include <dirent.h>
#include <functional>
#include <memory>
#include <sys/stat.h>
#include <utility>

// Board-internal preparation; not yet called by the accepted fixed-file firmware.
namespace boilon::local_library {
enum class ScanError { kNone, kPath, kIo, kLimit, kCatalog, kEmpty };
struct ScanResult {
    ScanError error = ScanError::kNone;
    media::MediaCatalog catalog;
    size_t entries = 0;
    bool Ok() const { return error == ScanError::kNone; }
};

inline bool Segment(const std::string& value) {
    std::string folded = value;
    for (char& ch : folded)
        if (ch >= 'A' && ch <= 'Z')
            ch += 'a' - 'A';
    return !value.empty() && value.front() != '.' && media::IsValidUtf8(value) &&
           value.find_first_of("/\\") == std::string::npos &&
           value.find('\0') == std::string::npos &&
           folded != "system volume information" && folded != "$recycle.bin" &&
           folded != "recycled" && folded != "lost+found";
}

inline bool Root(const std::string& path) {
    if (path.size() < 2 || path.size() > 500 || path.front() != '/' || path.back() == '/' ||
        path.find('\0') != std::string::npos || path.find('\\') != std::string::npos)
        return false;
    size_t begin = 1;
    while (begin < path.size()) {
        const auto end = path.find('/', begin);
        const auto part = path.substr(begin, end == std::string::npos ? end : end - begin);
        if (part.empty() || part == "." || part == "..")
            return false;
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    return true;
}

inline int EntryStat(const char* path, struct stat* info) {
#ifdef ESP_PLATFORM
    // This board adapter is only for the mounted FAT32 card (no symlinks).
    // ESP-IDF VFS does not provide lstat. Host fixtures must not follow links.
    return stat(path, info);
#else
    return lstat(path, info);
#endif
}

inline ScanResult Scan(const std::string& root, const std::string& library_key,
                       size_t max_entries = 4096, media::CatalogLimits limits = {}) {
    ScanResult result;
    if (!Root(root) || max_entries == 0) {
        result.error = ScanError::kPath;
        return result;
    }
    struct stat root_info = {};
    if (EntryStat(root.c_str(), &root_info) != 0 || !S_ISDIR(root_info.st_mode)) {
        result.error = ScanError::kIo;
        return result;
    }
    media::MediaCatalogBuilder builder(library_key, limits);
    auto scan_directory = [&](auto&& self, const std::string& collection) -> bool {
        const auto directory = collection.empty() ? root : root + "/" + collection;
        std::unique_ptr<DIR, decltype(&closedir)> stream(opendir(directory.c_str()), closedir);
        if (!stream) {
            result.error = ScanError::kIo;
            return false;
        }
        while (true) {
            errno = 0;
            const auto* entry = readdir(stream.get());
            if (!entry) {
                const int read_error = errno;
                const int close_error = closedir(stream.release());
                if (read_error || close_error) {
                    result.error = ScanError::kIo;
                    return false;
                }
                return true;
            }
            const std::string name(entry->d_name);
            if (name == "." || name == "..")
                continue;
            if (++result.entries > max_entries) {
                result.error = ScanError::kLimit;
                return false;
            }
            if (!Segment(name))
                continue;
            const auto path = directory + "/" + name;
            struct stat info = {};
            if (EntryStat(path.c_str(), &info) != 0) {
                result.error = ScanError::kIo;
                return false;
            }
            if (S_ISDIR(info.st_mode)) {
                if (collection.empty() && !self(self, name))
                    return false;
                continue;
            }
            if (!S_ISREG(info.st_mode) || !media::IsPlayableAudioName(name) || info.st_size <= 0)
                continue;
            if (path.size() > 512 || info.st_mtime < 0) {
                result.error = ScanError::kPath;
                return false;
            }
            const auto added = builder.Add({collection, name, static_cast<uint64_t>(info.st_size),
                                            static_cast<uint64_t>(info.st_mtime)});
            if (added == media::AddResult::kLimitExceeded || added == media::AddResult::kIdCollision ||
                added == media::AddResult::kConflictingDuplicate ||
                added == media::AddResult::kBuilderFailed || added == media::AddResult::kBuilderFinished) {
                result.error = ScanError::kCatalog;
                return false;
            }
        }
    };
    if (!scan_directory(scan_directory, ""))
        return result;
    auto built = builder.Finish();
    if (!built.Ok())
        result.error = ScanError::kCatalog;
    else if (built.catalog.Collections().empty())
        result.error = ScanError::kEmpty;
    else
        result.catalog = std::move(built.catalog);
    return result;
}

// One owner calls Submit immediately after each Controller action, then Poll
// periodically. No sleeps, new tasks, Start retries or stale completion callbacks.
template <typename Player>
class Switcher {
public:
    // output_idle must bind the existing AudioService::IsPlaybackIdle(); decoder
    // exit alone does not prove the last Codec write has drained.
    Switcher(media::PlayerController& controller, Player& player, std::string root,
             std::function<bool()> output_idle, std::function<void()> on_started = {})
        : controller_(controller), player_(player), root_(std::move(root)),
          output_idle_(std::move(output_idle)), on_started_(std::move(on_started)) {}

    bool Submit(media::ControlResult result, uint64_t now_ms) {
        if (!Clock(now_ms))
            return false;
        using Action = media::PlaybackAction;
        if (result.action == Action::kNone) {
            if (result.status == media::ControlStatus::kOk ||
                result.status == media::ControlStatus::kIgnored ||
                result.status == media::ControlStatus::kAtBoundary)
                return Poll(now_ms);
            return Fail();
        }
        if (result.status != media::ControlStatus::kOk)
            return Fail();
        if (result.action == Action::kStart) {
            const auto track = controller_.CurrentTrack();
            if (!track || !Root(root_) || !Segment(track->track.file_name) ||
                !media::IsPlayableAudioName(track->track.file_name) ||
                (!track->collection_is_root && !Segment(track->directory_name)))
                return Fail();
            path_ = root_ + "/" + (track->collection_is_root ? "" : track->directory_name + "/") +
                    track->track.file_name;
            if (path_.size() > 512)
                return Fail();
            start_pending_ = true;
            Drain(now_ms);
        } else if (result.action == Action::kStop) {
            start_pending_ = false;
            Drain(now_ms);
        } else if (result.action == Action::kPause) {
            if (!draining_ && !start_pending_ && !player_.Pause(true))
                return Fail();
        } else if (result.action == Action::kResume) {
            if (start_pending_)
                Drain(now_ms);
            else if (!player_.Pause(false))
                return Fail();
        } else {
            return Fail();
        }
        return Poll(now_ms);
    }

    bool Poll(uint64_t now_ms) {
        if (!Clock(now_ms))
            return false;
        if (!draining_)
            return true;
        if (now_ms - drain_started_ >= 2000)
            return Fail();
        if (!output_idle_)
            return Fail();
        if (player_.Running() || !output_idle_())
            return true;
        draining_ = false;
        if (start_pending_ && controller_.State() == media::PlaybackState::kPlaying) {
            start_pending_ = false;
            stop_sent_ = false;
            if (!player_.Start(path_))
                return Fail();
            if (on_started_)
                on_started_();  // Decoder accepted Start; not proof of audible output.
        }
        return true;
    }

    bool Pending() const { return draining_ || start_pending_; }
    bool Failed() const { return failed_; }

private:
    bool Fail() {
        if (!failed_ && !stop_sent_)
            player_.Stop();
        failed_ = true;
        draining_ = start_pending_ = false;
        return false;
    }
    bool Clock(uint64_t now_ms) {
        if (failed_)
            return false;
        if (now_ms < last_ms_)
            return Fail();
        last_ms_ = now_ms;
        return true;
    }
    void Drain(uint64_t now_ms) {
        if (!draining_) {
            draining_ = true;
            drain_started_ = now_ms;
        }
        if (player_.Running() && !stop_sent_) {
            player_.Stop();
            stop_sent_ = true;
        }
    }
    media::PlayerController& controller_;
    Player& player_;
    std::string root_, path_;
    std::function<bool()> output_idle_;
    std::function<void()> on_started_;
    uint64_t last_ms_ = 0, drain_started_ = 0;
    bool draining_ = false, start_pending_ = false, stop_sent_ = false, failed_ = false;
};
}  // namespace boilon::local_library
