#include "sdkconfig.h"
#if CONFIG_BOILON_S3_GEN2_AI
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#include <esp_log.h>
#include <esp_network.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>
#include <ssid_manager.h>
#include <wifi_manager.h>
#include <atomic>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "display/display.h"
#include "m2_local_codec.h"
#include "m3_ai_device.h"
#include "m3_ai_echo.h"
#include "m3_ai_library.h"
#include "m3_ai_network.h"
#include "mcp_server.h"
#include "settings.h"

namespace boilon::ai {
// m3_ai_main.cc owns power and the amplifier for the whole session.
[[noreturn]] void PowerOff(const char* reason, int code);
int SetAmplifier(bool enable);
int BatteryLevel();  // The player's estimate; -1 while unknown.
}  // namespace boilon::ai

namespace {
constexpr char kTag[] = "BoilonAi";
// Set by InspectStorage() in app_main before the board exists.
bool storage_ready = false;
bool ota_url_set = false;
boilon::ai::NetworkController* network = nullptr;  // The board's, once it exists.
std::atomic<bool> back_to_story{false};  // Set by the MCP tool on the main task.
// 2026-10-15: what the pause and power_off tools asked for; the player takes it.
std::atomic<boilon::ai::StoryEnd> story_end{boilon::ai::StoryEnd::kNone};
// From WifiManager's events, so AiReady() needs no lock on every player tick.
std::atomic<bool> wifi_connected{false};
// P5-3: the barge-in switch the user asked for; applied between conversations.
std::atomic<bool> barge_in{boilon::ai::echo::kDefaultBargeIn};
int barge_in_byte = -1;  // What NVS holds (-1: nothing); app_main, then the main task.

boilon::local_player::LocalCodec& Codec() {
    // The ES7210 microphone joins I2S0.
    static boilon::local_player::LocalCodec codec(
        [](const char* reason) { boilon::ai::PowerOff(reason, 0); }, boilon::ai::SetAmplifier,
        true);
    return codec;
}

std::string Read(const char* name_space, const char* key) {
    return Settings(name_space, false).GetString(key);  // Read only: never aborts.
}

// P5-3: one byte, written only when it changes; false when it was not saved.
bool SaveBargeIn(bool enabled) {
    namespace echo = boilon::ai::echo;
    const int value = enabled ? 1 : 0;
    if (barge_in_byte == value)
        return true;
    nvs_handle_t handle;
    bool saved = storage_ready && nvs_open(echo::kNamespace, NVS_READWRITE, &handle) == ESP_OK;
    if (saved) {
        saved = nvs_set_u8(handle, echo::kKey, value) == ESP_OK && nvs_commit(handle) == ESP_OK;
        nvs_close(handle);
    }
    if (saved)
        barge_in_byte = value;
    return saved;
}

// 2026-10-15: each directory's play mode, written only when one changed;
// false when it was not saved (the mode still applies until a restart).
bool SavePlayModes(const std::vector<uint8_t>& bytes) {
    namespace card = boilon::ai::card;
    nvs_handle_t handle;
    bool saved = storage_ready && nvs_open(card::kModesNamespace, NVS_READWRITE, &handle) == ESP_OK;
    if (saved) {
        saved = nvs_set_blob(handle, card::kModesKey, bytes.data(), bytes.size()) == ESP_OK &&
                nvs_commit(handle) == ESP_OK;
        nvs_close(handle);
    }
    return saved;
}

// Records upstream's messages. The activation prompt goes to the network
// controller; the conversation's turns, words and emotion to the AI page.
class AiDisplay final : public Display {
public:
    explicit AiDisplay(boilon::ai::NetworkController& controller) : controller_(controller) {}
    using Display::ShowNotification;
    void SetStatus(const char* status) override {
        ESP_LOGI(kTag, "UI status: %s", status);
        activation_ = std::strcmp(status, Lang::Strings::ACTIVATION) == 0;
        if (std::strcmp(status, Lang::Strings::ERROR) == 0) {
            controller_.Activation(nullptr);
            boilon::ai::CurrentChat().Failed();
        }
        // Upstream connects at the start of every conversation it opens.
        if (std::strcmp(status, Lang::Strings::CONNECTING) == 0)
            boilon::ai::CurrentChat().Begin();
        if (std::strcmp(status, Lang::Strings::LISTENING) == 0)
            boilon::ai::CurrentChat().Listening();
        if (std::strcmp(status, Lang::Strings::SPEAKING) == 0)
            boilon::ai::CurrentChat().Speaking();
    }
    void ShowNotification(const char* text, int) override { ESP_LOGI(kTag, "UI notice: %s", text); }
    void SetEmotion(const char* emotion) override {
        ESP_LOGI(kTag, "UI emotion: %s", emotion);
        boilon::ai::CurrentChat().Emotion(emotion);
    }
    void SetChatMessage(const char* role, const char* content) override {
        if (!*content)
            return;
        // The child's words and the AI's answers stay out of the log: only the
        // screen shows them.
        if (std::strcmp(role, "user") == 0)
            boilon::ai::CurrentChat().Heard(content);
        if (std::strcmp(role, "assistant") == 0)
            boilon::ai::CurrentChat().Said(content);
        if (std::strcmp(role, "system") == 0)
            ESP_LOGI(kTag, "UI %s: %s", role, content);
        if (activation_ && std::strcmp(role, "system") == 0)
            controller_.Activation(content);
    }

private:
    bool Lock(int) override { return true; }
    void Unlock() override {}
    boilon::ai::NetworkController& controller_;
    std::atomic<bool> activation_{false};
};

// Wi-Fi through upstream WifiManager's public API only. Called from the
// network task, the main task (Schedule) and Wi-Fi event callbacks.
class WifiPort final : public boilon::ai::NetworkPort {
public:
    WifiPort(std::string& uuid, std::function<std::string()> generate_uuid)
        : uuid_(uuid), generate_uuid_(std::move(generate_uuid)) {}
    NetworkEventCallback forward;  // Application's; set before any network start.
    boilon::ai::NetworkController* controller = nullptr;

    size_t SavedWifi() override { return SsidManager::GetInstance().GetSsidList().size(); }
    bool InitWifi() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ready_)
            return true;
        if (uuid_.empty() && !SaveIdentity())
            return false;
        WifiManagerConfig config;
        config.ssid_prefix = "Boilon";
        config.language = Lang::CODE;
        config.show_ota_config = false;  // Only the built-in official server.
        auto& wifi = WifiManager::GetInstance();
        wifi.SetEventCallback(
            [this](WifiEvent event, const std::string& data) { OnWifiEvent(event, data); });
        // Callers checked storage_ready: NVS is initialized, so this never erases it.
        ready_ = wifi.Initialize(config);
        if (!ready_)
            ESP_LOGE(kTag, "Wi-Fi init failed; network stays off");
        return ready_;
    }
    void StartStation() override { WifiManager::GetInstance().StartStation(); }
    bool CanConfigure() override {
        const auto state = Application::GetInstance().GetDeviceState();
        return state == kDeviceStateStarting || state == kDeviceStateIdle ||
               state == kDeviceStateWifiConfiguring;
    }
    void OpenHotspot() override {
        // On the main task; upstream WifiBoard also resets the protocol first.
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() == kDeviceStateIdle)
            app.ResetProtocol();
        app.SetDeviceState(kDeviceStateWifiConfiguring);
        WifiManager::GetInstance().StartConfigAp();
    }
    void CloseHotspot() override {
        Application::GetInstance().Schedule([] { WifiManager::GetInstance().StopConfigAp(); });
    }
    bool HotspotOpen() override { return WifiManager::GetInstance().IsConfigMode(); }
    std::string HotspotName() override { return WifiManager::GetInstance().GetApSsid(); }
    bool Activating() override {
        return Application::GetInstance().GetDeviceState() == kDeviceStateActivating;
    }
    bool OtaUrlSet() override {
        const bool set = !Read("wifi", "ota_url").empty();
        if (set)
            ESP_LOGW(kTag, "wifi/ota_url saved; Wi-Fi stays off");
        return set;
    }

private:
    // Upstream Board()'s key and generator, but a failed write keeps the
    // network off instead of aborting.
    bool SaveIdentity() {
        const auto uuid = generate_uuid_();
        nvs_handle_t handle;
        bool saved = nvs_open("board", NVS_READWRITE, &handle) == ESP_OK;
        if (saved) {
            saved = nvs_set_str(handle, "uuid", uuid.c_str()) == ESP_OK &&
                    nvs_commit(handle) == ESP_OK;
            nvs_close(handle);
        }
        if (saved)
            uuid_ = uuid;
        ESP_LOGI(kTag, "Cloud identity %s", saved ? "created" : "not saved; network stays off");
        return saved;
    }
    void OnWifiEvent(WifiEvent event, const std::string& data) {
        // WifiEvent and NetworkEvent list the Wi-Fi events in the same order.
        static_assert(static_cast<int>(WifiEvent::ConfigModeExit) == 5 &&
                      static_cast<int>(NetworkEvent::WifiConfigModeExit) == 5);
        static constexpr NetworkEvent kEvents[] = {
            NetworkEvent::Scanning, NetworkEvent::Connecting, NetworkEvent::Connected,
            NetworkEvent::Disconnected, NetworkEvent::WifiConfigModeEnter,
            NetworkEvent::WifiConfigModeExit};
        const bool disconnected = event == WifiEvent::Disconnected;
        if (disconnected)
            ESP_LOGW(kTag, "Wi-Fi disconnected %s", data.c_str());
        // Connected follows the IP; every way off Wi-Fi reports Disconnected.
        if (disconnected || event == WifiEvent::Connected)
            wifi_connected = !disconnected;
        if (disconnected)
            boilon::ai::CurrentChat().Failed();  // A conversation now ends with the error face.
        if (forward)
            forward(kEvents[static_cast<int>(event)], disconnected ? std::string() : data);
        // Unlike WifiBoard there is no connect timer: a failed connection
        // keeps retrying in the background and never reopens the hotspot.
        if (event == WifiEvent::ConfigModeExit && controller)
            controller->HotspotClosed();
    }

    std::string& uuid_;
    std::function<std::string()> generate_uuid_;
    std::mutex mutex_;
    bool ready_ = false;
};
}  // namespace

// M3 batch 2: XiaoZhi's Application beside the offline player, with Wi-Fi only
// after the parent saved a network through the gesture-opened hotspot. Not a
// WifiBoard: its Board() aborts creating the identity when NVS failed, and it
// opens the hotspot by itself when a connection fails.
class BoilonS3Gen2AiBoard final : public Board {
public:
    BoilonS3Gen2AiBoard() noexcept
        : Board(TransientIdentityTag{}),
          port_(uuid_, [this] { return GenerateUuid(); }),
          controller_(port_, storage_ready, ota_url_set) {
        port_.controller = network = &controller_;
        // Read only; a missing identity is created when Wi-Fi first starts.
        if (storage_ready)
            uuid_ = Read("board", "uuid");
        // Application logs what the child says and what the AI answers at info level.
        esp_log_level_set("Application", ESP_LOG_WARN);
        McpServer::GetInstance().AddTool(
            "self.story.back_to_story",
            "End this conversation and go back to the story player. Call it when the child "
            "wants to listen to the story again or to stop chatting (for example: 我要听故事, "
            "回到故事, 不聊了).",
            PropertyList(), [](const PropertyList&) -> ReturnValue {
                back_to_story = true;
                return true;
            });
        // P5-1: try the other palette, layout or subtitles without a new build; not saved.
        McpServer::GetInstance().AddTool(
            "self.story.set_screen_style",
            "Change how the story player's screen looks. Call it only when the user asks to "
            "change the screen style (for example: 换成夜灯, 换成暖纸, 换成陪听, 换成故事卡, "
            "打开字幕, 关掉字幕). palette: paper (warm, the default) or night (dark). layout: card "
            "(the story card, the default) or cat (the cat listens along). subtitles: on or off "
            "(the default); on shows what the child said and the sentence you are speaking. Leave "
            "one empty to keep it. The change lasts until the device restarts.",
            PropertyList({Property("palette", kPropertyTypeString, std::string()),
                          Property("layout", kPropertyTypeString, std::string()),
                          Property("subtitles", kPropertyTypeString, std::string())}),
            [](const PropertyList& properties) -> ToolResult {
                auto& style = boilon::ui::CurrentStyle();
                if (!boilon::ui::ApplyStyle(style, properties["palette"].value<std::string>(),
                                            properties["layout"].value<std::string>(),
                                            properties["subtitles"].value<std::string>()))
                    return std::unexpected(std::string(
                        "palette must be paper or night, layout card or cat, subtitles on or off"));
                ESP_LOGI(kTag, "Screen style %s", boilon::ui::DescribeStyle(style).c_str());
                return boilon::ui::DescribeStyle(style);
            });
        // P5-3: off until the user asks; remembered, and applied from the next conversation.
        McpServer::GetInstance().AddTool(
            "self.story.set_barge_in",
            "Turn on or off letting the child interrupt you while you are speaking (回答中插话). "
            "Call it only when the user asks (for example: 打开回答中插话, 打开插话, 关掉插话, "
            "不要插话). enabled: true turns it on, false turns it off (the default). The change "
            "starts with the next conversation and is remembered after a restart, so tell the "
            "user it works from next time.",
            PropertyList({Property("enabled", kPropertyTypeBoolean)}),
            [](const PropertyList& properties) -> ToolResult {
                const bool enabled = properties["enabled"].value<bool>();
                if (enabled && !Codec().ReferenceReady())
                    return std::unexpected(std::string(
                        "barge-in is unavailable: no playback reference was found at startup"));
                barge_in = enabled;
                const bool saved = SaveBargeIn(enabled);
                ESP_LOGI(kTag, "Barge-in %s from the next conversation, saved=%d",
                         enabled ? "on" : "off", saved);
                return std::string(R"({"barge_in":")") + (enabled ? "on" : "off") +
                       R"(","starts":"next conversation","saved":)" + (saved ? "true" : "false") +
                       "}";
            });
        // P5-2: find and play what is on the card; the player switches when 喵喵
        // finishes the answer.
        McpServer::GetInstance().AddTool(
            "self.story.list",
            "Look up the stories and songs on the story player's card. With no arguments it lists "
            "the directories (each a story series or a song collection) with how many files each "
            "has, and what is selected now. directory: that directory's files with their numbers, "
            "20 per page (page counts from 1). keyword: search the directory and file names; "
            "words separated by spaces must all appear; up to 20 matches with their directory and "
            "number. File names usually carry the episode's title, so search for what the child "
            "mentions (for example 三打白骨精) and try a shorter keyword (白骨精) when nothing "
            "matches.",
            PropertyList({Property("directory", kPropertyTypeString, std::string()),
                          Property("keyword", kPropertyTypeString, std::string()),
                          Property("page", kPropertyTypeInteger, 1, 1, 100)}),
            [](const PropertyList& properties) -> ToolResult {
                const auto reply = boilon::ai::card::Card::Shared().List(
                    properties["directory"].value<std::string>(),
                    properties["keyword"].value<std::string>(), properties["page"].value<int>());
                if (!reply.ok)
                    return std::unexpected(reply.text);
                return reply.text;
            });
        McpServer::GetInstance().AddTool(
            "self.story.play",
            "Play a story or song from the card, then go back to the story player. Call it only "
            "when the child asks to hear something on the card (for example: 放凯叔西游记, "
            "放第三集, 从三打白骨精那里开始); for anything else, say it is not on the card. "
            "directory: a directory name from self.story.list. number: the file's number in that "
            "directory, played from its beginning; 0 continues the directory where it was last "
            "heard. Find the file with self.story.list first. If the names do not show which file "
            "the child means, guess from the usual order of the story, tell the child it is a "
            "guess and call this only after the child agrees. After calling, say one short "
            "sentence: the conversation ends when you finish, the file plays and the next ones "
            "follow.",
            PropertyList({Property("directory", kPropertyTypeString),
                          Property("number", kPropertyTypeInteger, 0, 0, 512)}),
            [](const PropertyList& properties) -> ToolResult {
                const auto reply = boilon::ai::card::Card::Shared().Play(
                    properties["directory"].value<std::string>(),
                    properties["number"].value<int>());
                ESP_LOGI(kTag, "Play request %s", reply.ok ? "accepted" : "refused");
                if (!reply.ok)
                    return std::unexpected(reply.text);
                return reply.text;
            });
        AddPlayerTools();
    }

    std::string GetBoardType() override { return "wifi"; }
    AudioCodec* GetAudioCodec() override { return &Codec(); }
    Display* GetDisplay() override {
        static AiDisplay display(controller_);
        return &display;
    }
    NetworkInterface* GetNetwork() override {
        static EspNetwork esp_network;
        return &esp_network;
    }
    void StartNetwork() override {
        // Let the story start first; Start() never opens the hotspot.
        if (xTaskCreate(
                [](void* controller) {
                    vTaskDelay(pdMS_TO_TICKS(3000));
                    const auto start =
                        static_cast<boilon::ai::NetworkController*>(controller)->Start();
                    ESP_LOGI(kTag, "Network start: %s",
                             boilon::ai::kNetworkStartNames[static_cast<int>(start)]);
                    vTaskDelete(nullptr);
                },
                "boilon_net", 6144, &controller_, 1, nullptr) != pdPASS)
            ESP_LOGE(kTag, "Network task not created; network stays off");
    }
    void SetNetworkEventCallback(NetworkEventCallback callback) override {
        port_.forward = std::move(callback);
    }
    const char* GetNetworkStateIcon() override { return ""; }
    void SetPowerSaveLevel(PowerSaveLevel level) override {
        // Both enums list the same three levels; a no-op until the station runs.
        static_assert(static_cast<int>(WifiPowerSaveLevel::PERFORMANCE) == 2);
        WifiManager::GetInstance().SetPowerSaveLevel(static_cast<WifiPowerSaveLevel>(level));
    }
    std::string GetBoardJson() override {
        return std::string(R"({"type":")") + BOARD_TYPE + R"(","name":")" + BOARD_NAME + R"("})";
    }
    // For the set_volume tool, which tells the model to read this first.
    std::string GetDeviceStatusJson() override {
        return boilon::ai::DeviceStatusJson(Codec().OutputVolume(), boilon::ai::BatteryLevel());
    }

private:
    // 2026-10-15: the play mode, pause and power-off tools; 2026-10-16 forgetting progress.
    void AddPlayerTools() {
        McpServer::GetInstance().AddTool(
            "self.story.set_play_mode",
            "Set how a directory of the story player goes on after each file (播放模式), when "
            "the user asks (for example: 随机播放, 单曲循环, 循环播放, 顺序播放). mode: order "
            "(顺序播放, stops after the last file; the default), loop (循环播放, the whole "
            "directory without end), single (单曲循环, the same file again and again), shuffle "
            "(随机播放, random order without end). directory: a name from self.story.list; "
            "empty means the one playing now, or the one just picked with self.story.play. Each "
            "directory keeps its mode, also after a restart; self.story.list shows the modes. "
            "The conversation goes on.",
            PropertyList({Property("mode", kPropertyTypeString),
                          Property("directory", kPropertyTypeString, std::string())}),
            [](const PropertyList& properties) -> ToolResult {
                std::vector<uint8_t> bytes;
                const auto reply = boilon::ai::card::Card::Shared().SetMode(
                    properties["directory"].value<std::string>(),
                    properties["mode"].value<std::string>(), bytes);
                if (!reply.ok)
                    return std::unexpected(reply.text);
                ESP_LOGI(kTag, "Play mode %s, saved=%d",
                         properties["mode"].value<std::string>().c_str(),
                         bytes.empty() ? -1 : SavePlayModes(bytes));
                return reply.text;
            });
        McpServer::GetInstance().AddTool(
            "self.story.pause",
            "Stop the story for now when the child asks (for example: 先别放了, 暂停, 停一下, "
            "不听了). The conversation ends when you finish this answer and the story stays "
            "paused until the child presses the top key, so say one short sentence.",
            PropertyList(), [](const PropertyList&) -> ToolResult {
                if (!boilon::ai::card::Card::Shared().Published())
                    return std::unexpected(std::string(boilon::ai::card::kNoCard));
                story_end = boilon::ai::StoryEnd::kPause;
                return std::string(
                    R"({"story":"stays paused","starts":"when you finish this answer"})");
            });
        McpServer::GetInstance().AddTool(
            "self.story.power_off",
            "Turn the story player off when the user asks (for example: 关机, 关掉故事机). "
            "Progress is saved and it turns off when you finish this answer, so say goodbye in "
            "one short sentence.",
            PropertyList(), [](const PropertyList&) -> ToolResult {
                story_end = boilon::ai::StoryEnd::kPowerOff;
                return std::string(R"({"power":"off","starts":"when you finish this answer"})");
            });
        // 2026-10-16: the player deletes the bookmarks at once; the story playing
        // now, if among them, starts again from its first file after the answer.
        McpServer::GetInstance().AddTool(
            "self.story.forget_progress",
            "Forget where the stories were last heard (播放进度), so they start again from the "
            "beginning, when the user asks (for example: 把西游记的进度清掉, 清除所有播放进度). "
            "Forgotten progress cannot come back: first say exactly what will be forgotten, a "
            "directory's name or all progress on this story player, and call this only after "
            "the user agrees. directory: a name from self.story.list; empty means the one playing "
            "now, or the one just picked with self.story.play. all: true forgets every "
            "directory's progress. The volume and the play modes stay. When it includes the "
            "directory playing now, the conversation ends when you finish this answer and that "
            "directory plays from its first file's beginning (or what you just picked plays); "
            "otherwise the conversation goes on. To hear one file from its beginning, use "
            "self.story.play with its number instead.",
            PropertyList({Property("directory", kPropertyTypeString, std::string()),
                          Property("all", kPropertyTypeBoolean, false)}),
            [](const PropertyList& properties) -> ToolResult {
                const auto reply = boilon::ai::card::Card::Shared().ForgetProgress(
                    properties["directory"].value<std::string>(), properties["all"].value<bool>());
                ESP_LOGI(kTag, "Forget request %s", reply.ok ? "accepted" : "refused");
                if (!reply.ok)
                    return std::unexpected(reply.text);
                return reply.text;
            });
    }

    WifiPort port_;
    boilon::ai::NetworkController controller_;
};

DECLARE_BOARD(BoilonS3Gen2AiBoard);

namespace boilon::ai {
void InspectStorage(esp_err_t nvs_result) {
    storage_ready = nvs_result == ESP_OK;
    if (!storage_ready) {
        ESP_LOGW(kTag, "Cloud identity: NVS unavailable (0x%x); network stays off", nvs_result);
        return;
    }
    ota_url_set = !Read("wifi", "ota_url").empty();
    uint8_t serial = 0;  // Same test as Ota: a serial selects activation version 2.
    esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, &serial, 8);
    nvs_stats_t stats = {};
    const bool counted = nvs_get_stats(nullptr, &stats) == ESP_OK;
    ESP_LOGI(kTag,
             "Cloud identity: uuid=%d wifi_saved=%u ota_url=%d efuse_serial=%d nvs_used=%u "
             "nvs_free=%u nvs_available=%u",
             !Read("board", "uuid").empty(),
             static_cast<unsigned>(SsidManager::GetInstance().GetSsidList().size()), ota_url_set,
             serial != 0, static_cast<unsigned>(stats.used_entries),
             static_cast<unsigned>(stats.free_entries),
             static_cast<unsigned>(stats.available_entries));
    // Same path as an NVS error: Wi-Fi and the hotspot stay off this boot.
    if (!counted || !NvsRoomForNetwork(stats.available_entries)) {
        storage_ready = false;
        ESP_LOGW(kTag, "NVS nearly full or unreadable; network stays off");
    }
    nvs_handle_t handle;
    uint8_t value = 0;
    if (nvs_open(echo::kNamespace, NVS_READONLY, &handle) == ESP_OK) {
        if (nvs_get_u8(handle, echo::kKey, &value) == ESP_OK)
            barge_in_byte = value;
        nvs_close(handle);
    }
    barge_in = echo::Decode(barge_in_byte >= 0, value);
    ESP_LOGI(kTag, "Barge-in switch: %s (%s)", barge_in ? "on" : "off",
             barge_in_byte >= 0 ? "saved" : "default");
}

// Before every conversation the player starts, on its task. Upstream's
// SetAecMode() closes an open voice channel, so the main task switches only
// while idle; the WakeWordInvoke() scheduled next then listens in the new
// mode: realtime with device AEC, or stopping while 喵喵 speaks, as before.
void ApplyBargeIn() {
    Application::GetInstance().Schedule([] {
        auto& app = Application::GetInstance();
        const bool aec_on = app.GetAecMode() == kAecOnDeviceSide;
        if (!echo::ShouldSwitch(app.GetDeviceState() == kDeviceStateIdle, Codec().ReferenceReady(),
                                barge_in, aec_on))
            return;
        ESP_LOGI(kTag, "Barge-in %s from this conversation", aec_on ? "off" : "on");
        app.SetAecMode(aec_on ? kAecOff : kAecOnDeviceSide);
    });
}

void RequestHotspot() {
    Application::GetInstance().Schedule([] {
        const bool opened = network->OpenHotspot(esp_timer_get_time() / 1000);
        ESP_LOGI(kTag, "Hotspot request %s: storage=%d ota_url=%d state=%d",
                 opened ? "accepted" : "refused", storage_ready, ota_url_set,
                 static_cast<int>(Application::GetInstance().GetDeviceState()));
    });
}

std::optional<Notice> PollNetwork(uint64_t now_ms) { return network->Poll(now_ms); }

bool AiReady() {
    return Application::GetInstance().GetDeviceState() == kDeviceStateIdle && wifi_connected &&
           Codec().MicrophoneReady();
}

bool TakeBackToStory() { return back_to_story.exchange(false); }

StoryEnd TakeStoryEnd() { return story_end.exchange(StoryEnd::kNone); }

// What NVS holds for the play modes; nothing when it cannot be read.
std::vector<uint8_t> ReadPlayModes() {
    std::vector<uint8_t> bytes;
    nvs_handle_t handle;
    if (nvs_open(card::kModesNamespace, NVS_READONLY, &handle) != ESP_OK)
        return bytes;
    size_t size = 0;
    if (nvs_get_blob(handle, card::kModesKey, nullptr, &size) == ESP_OK &&
        size <= 1 + 9 * card::kMaxSaved) {
        bytes.resize(size);
        if (nvs_get_blob(handle, card::kModesKey, bytes.data(), &size) != ESP_OK ||
            size != bytes.size())
            bytes.clear();
    }
    nvs_close(handle);
    return bytes;
}
}  // namespace boilon::ai
#endif
