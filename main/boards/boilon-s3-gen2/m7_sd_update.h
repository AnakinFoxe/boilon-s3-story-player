#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// M7: the rules for updating the AI variant from the SD card. The ESP side
// (m7_sd_update_esp.h) lists the card's root, writes the other OTA slot and
// asks these functions what to do; they are host tested.
namespace boilon::sd_update {

struct Version {
    int major = 0, minor = 0, patch = 0;
};
inline bool Newer(const Version& a, const Version& b) {
    if (a.major != b.major)
        return a.major > b.major;
    if (a.minor != b.minor)
        return a.minor > b.minor;
    return a.patch > b.patch;
}
inline std::string Numbers(const Version& v) {
    return std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.patch);
}

constexpr std::string_view kReleasePrefix = "boilon-ai-player v";  // m6_release.h kName
constexpr std::string_view kFilePrefix = "boilon-ai-player-v";     // the Release asset
constexpr std::string_view kFileSuffix = ".bin";
constexpr int kMinBattery = 20;  // percent; an unknown level does not block

// "1.2.3" exactly: three numbers below 1000 without leading zeros.
inline std::optional<Version> ParseNumbers(std::string_view text) {
    int parts[3] = {};
    for (int i = 0; i < 3; ++i) {
        if (i > 0) {
            if (text.empty() || text[0] != '.')
                return std::nullopt;
            text.remove_prefix(1);
        }
        size_t digits = 0;
        while (digits < text.size() && digits < 4 && text[digits] >= '0' && text[digits] <= '9')
            parts[i] = parts[i] * 10 + (text[digits++] - '0');
        if (digits == 0 || digits > 3 || (digits > 1 && text[0] == '0'))
            return std::nullopt;
        text.remove_prefix(digits);
    }
    if (!text.empty())
        return std::nullopt;
    return Version{parts[0], parts[1], parts[2]};
}
// The running firmware's release name, "boilon-ai-player v1.2.3".
inline std::optional<Version> FromReleaseName(std::string_view name) {
    if (name.substr(0, kReleasePrefix.size()) != kReleasePrefix)
        return std::nullopt;
    return ParseNumbers(name.substr(kReleasePrefix.size()));
}
// An AI release in the card's root, "boilon-ai-player-v1.2.3.bin"; the offline
// variant ("-offline.bin") and every other name are not candidates.
inline std::optional<Version> FromFileName(std::string_view name) {
    if (name.size() <= kFilePrefix.size() + kFileSuffix.size() ||
        name.substr(0, kFilePrefix.size()) != kFilePrefix ||
        name.substr(name.size() - kFileSuffix.size()) != kFileSuffix)
        return std::nullopt;
    return ParseNumbers(
        name.substr(kFilePrefix.size(), name.size() - kFilePrefix.size() - kFileSuffix.size()));
}
// The release name the image itself must carry, "boilon-ai-player v1.2.3".
inline std::string ReleaseName(const Version& v) {
    return std::string(kReleasePrefix) + Numbers(v);
}

struct Candidate {
    std::string name;
    Version version;
};
// The newest release file among the names, when it is newer than the running one.
inline std::optional<Candidate> PickNewer(const std::vector<std::string>& names,
                                          const Version& running) {
    std::optional<Candidate> best;
    for (const auto& name : names) {
        const auto version = FromFileName(name);
        if (version && Newer(*version, running) && (!best || Newer(*version, best->version)))
            best = Candidate{name, *version};
    }
    return best;
}

// One exact byte string, found across chunk boundaries.
class Marker {
public:
    explicit Marker(std::string text) : text_(std::move(text)) {}
    void Feed(const uint8_t* data, size_t size) {
        if (found_ || text_.empty())
            return;
        std::string window = tail_;
        window.append(reinterpret_cast<const char*>(data), size);
        found_ = window.find(text_) != std::string::npos;
        const size_t keep = std::min(window.size(), text_.size() - 1);
        tail_ = window.substr(window.size() - keep);
    }
    bool Found() const { return found_; }

private:
    std::string text_, tail_;
    bool found_ = false;
};

// The OTA states of esp_ota_img_states_t, for logs and decisions.
enum class SlotState : uint8_t { kNew, kPendingVerify, kValid, kInvalid, kAborted, kUndefined };
inline const char* Name(SlotState s) {
    constexpr const char* kNames[] = {"NEW", "PENDING_VERIFY", "VALID", "INVALID", "ABORTED",
                                      "UNDEFINED"};
    return kNames[static_cast<int>(s)];
}
// A new firmware confirms itself once startup completes, and only while a
// rollback-capable bootloader waits for it; other states are left untouched
// so the USB install records keep matching.
inline bool ShouldConfirm(SlotState running) { return running == SlotState::kPendingVerify; }

enum class Skip : uint8_t { kNone, kUnconfirmed, kLowBattery, kFailedBefore };
inline const char* Name(Skip s) {
    constexpr const char* kNames[] = {"none", "running app not confirmed", "battery low",
                                      "this image failed to start before"};
    return kNames[static_cast<int>(s)];
}
struct Situation {
    SlotState running = SlotState::kUndefined;
    int battery = -1;  // percent; -1 unknown
    SlotState target = SlotState::kUndefined;
    bool target_holds_this_image = false;  // same app_elf_sha256 as the card's file
};
inline Skip Decide(const Situation& s) {
    if (s.running == SlotState::kPendingVerify)  // esp_ota_begin would refuse anyway
        return Skip::kUnconfirmed;
    if (s.battery >= 0 && s.battery < kMinBattery)
        return Skip::kLowBattery;
    // After a rollback the failed image stays in the other slot: never loop on it.
    if (s.target_holds_this_image &&
        (s.target == SlotState::kAborted || s.target == SlotState::kInvalid))
        return Skip::kFailedBefore;
    return Skip::kNone;
}

// Percent of a known total; the page shows whole steps only.
inline int Progress(size_t done, size_t total) {
    return total ? static_cast<int>(done * 100 / total) : 0;
}
}  // namespace boilon::sd_update
