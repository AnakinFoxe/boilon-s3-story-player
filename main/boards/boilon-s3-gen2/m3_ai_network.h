#pragma once
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

// M3 batch 2: when the AI variant may use Wi-Fi. No ESP-IDF headers, so
// test_m3_ai_network runs this exact code against a fake port.
namespace boilon::ai {
enum class NetworkStart { kStorageError, kServerOverride, kNoSavedWifi, kConnect };
constexpr const char* kNetworkStartNames[] = {"storage_error", "ota_url_override",
                                              "no_saved_wifi", "connect"};
constexpr uint64_t kHotspotMs = 10 * 60 * 1000, kNoticeMs = 5000;

// M5 batch 5: upstream aborts when an NVS write fails, and Wi-Fi brings its
// writes (the identity, server settings, saved networks). Below one page
// (126 entries) of room the network stays off; the story only needs its own
// checked writes.
constexpr size_t kMinNvsEntries = 126;
inline bool NvsRoomForNetwork(size_t available_entries) {
    return available_entries >= kMinNvsEntries;
}

// A failing NVS must never reach Wi-Fi init, which erases it; a saved ota_url
// would send the identity to a server other than the built-in official one.
inline NetworkStart DecideNetworkStart(bool storage_ready, bool ota_url_set, size_t saved_wifi) {
    if (!storage_ready)
        return NetworkStart::kStorageError;
    if (ota_url_set)
        return NetworkStart::kServerOverride;
    return saved_wifi ? NetworkStart::kConnect : NetworkStart::kNoSavedWifi;
}

// The six-digit code inside the server's activation prompt, or empty.
inline std::string ActivationCode(const std::string& prompt) {
    for (size_t start = 0, end = 0; start < prompt.size(); start = end > start ? end : start + 1) {
        for (end = start; end < prompt.size() && prompt[end] >= '0' && prompt[end] <= '9';)
            ++end;
        if (end - start == 6)
            return prompt.substr(start, 6);
    }
    return {};
}

// Board services; never called while the controller holds its lock.
struct NetworkPort {
    virtual ~NetworkPort() = default;
    virtual size_t SavedWifi() = 0;
    virtual bool InitWifi() = 0;  // Idempotent; persists the identity first.
    virtual void StartStation() = 0;
    virtual bool CanConfigure() = 0;  // Application is starting, idle or configuring.
    virtual void OpenHotspot() = 0;
    virtual void CloseHotspot() = 0;
    virtual bool HotspotOpen() = 0;
    virtual std::string HotspotName() = 0;
    virtual bool Activating() = 0;
    virtual bool OtaUrlSet() = 0;  // Read now: the hotspot page can save one.
};

struct Notice {
    // P5-1: what the screen shows; status and title stay for the logs.
    enum class Kind { kRefused, kHotspot, kActivating, kActivated };
    Notice(std::string status_text, std::string title_text, Kind notice_kind = Kind::kActivated,
           std::string notice_detail = {}, uint64_t remaining = 0)
        : status(std::move(status_text)),
          title(std::move(title_text)),
          kind(notice_kind),
          detail(std::move(notice_detail)),
          remaining_ms(remaining) {}
    std::string status, title;
    Kind kind;
    std::string detail;     // the hotspot name, or the six-digit code
    uint64_t remaining_ms;  // until the hotspot closes
};

class NetworkController {
public:
    NetworkController(NetworkPort& port, bool storage_ready, bool ota_url_set)
        : port_(port), storage_ready_(storage_ready), ota_url_set_(ota_url_set) {}

    // Background connection at boot; nothing here opens the hotspot.
    NetworkStart Start() {
        const auto start = DecideNetworkStart(storage_ready_, ota_url_set_, port_.SavedWifi());
        if (start == NetworkStart::kConnect && port_.InitWifi())
            port_.StartStation();
        return start;
    }

    // The 5 s volume-key hold is the only way to open the hotspot.
    bool OpenHotspot(uint64_t now_ms) {
        const bool open = port_.HotspotOpen();
        const bool allowed = open || (storage_ready_ && !ota_url_set_ && port_.CanConfigure() &&
                                      port_.InitWifi());
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (allowed)
                deadline_ms_ = now_ms + kHotspotMs;
            else
                refused_ms_ = now_ms;
        }
        if (allowed && !open)
            port_.OpenHotspot();
        return allowed;
    }

    // The web page, the timeout or a station start closed the hotspot. The page
    // may have saved a server address; then the station stays off.
    void HotspotClosed() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            deadline_ms_.reset();
        }
        if (port_.SavedWifi() && !port_.OtaUrlSet())
            port_.StartStation();
    }

    // From the display: an activation prompt, or nullptr for an error alert.
    void Activation(const char* prompt) {
        std::lock_guard<std::mutex> lock(mutex_);
        prompt_failed_ = !prompt && !prompt_.empty();
        if (prompt)
            prompt_ = prompt;
    }

    // From the player loop: closes an expired hotspot and returns the text
    // that temporarily replaces the player's status and title.
    std::optional<Notice> Poll(uint64_t now_ms) {
        const bool open = port_.HotspotOpen(), activating = port_.Activating();
        const std::string name = open ? port_.HotspotName() : std::string();
        std::optional<Notice> notice;
        bool close = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            close = open && deadline_ms_ && now_ms >= *deadline_ms_;
            if (close)
                deadline_ms_.reset();
            if (!prompt_.empty() && !activating) {  // Activation ended.
                if (!prompt_failed_)
                    activated_ms_ = now_ms;
                prompt_.clear();
            }
            const auto code = ActivationCode(prompt_);
            using Kind = Notice::Kind;
            if (refused_ms_ && now_ms - *refused_ms_ < kNoticeMs)
                notice = Notice{"配网", "现在不能配网", Kind::kRefused};
            else if (open && !close)
                notice = Notice{"配网", "手机连接热点 " + name + "，再打开 192.168.4.1",
                                Kind::kHotspot, name,
                                deadline_ms_ && *deadline_ms_ > now_ms ? *deadline_ms_ - now_ms : 0};
            else if (!prompt_.empty())
                notice = code.empty() ? Notice{"激活", prompt_, Kind::kActivating}
                                      : Notice{code, "请在 xiaozhi.me 添加设备，输入上面的激活码",
                                               Kind::kActivating, code};
            else if (activated_ms_ && now_ms - *activated_ms_ < kNoticeMs)
                notice = Notice{"已激活", "", Kind::kActivated};
        }
        if (close)
            port_.CloseHotspot();
        return notice;
    }

private:
    NetworkPort& port_;
    const bool storage_ready_, ota_url_set_;
    std::mutex mutex_;
    std::optional<uint64_t> deadline_ms_, refused_ms_, activated_ms_;
    std::string prompt_;
    bool prompt_failed_ = false;
};
}  // namespace boilon::ai
