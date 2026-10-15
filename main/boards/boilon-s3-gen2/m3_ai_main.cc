#include "sdkconfig.h"
#if CONFIG_BOILON_S3_GEN2_AI
#include <driver/rtc_io.h>
#include <driver/sdmmc_host.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_vfs_fat.h>
#include <hal/usb_serial_jtag_ll.h>
#include <nvs_flash.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include "application.h"
#include "audio/audio_service.h"
#include "audio/local_mp3_player.h"
#include "board.h"
#include "config.h"
#include "m2_battery_adc.h"
#include "m2_battery_estimate.h"
#include "m2_library_lcd.h"
#include "m2_library_runtime.h"
#include "m2_local_codec.h"
#include "m2_resume_worker.h"
#include "m3_ai_device.h"
#include "m3_ai_library.h"
#include "m3_ai_network.h"
#include "m3_ai_session.h"
#include "m5_power_policy.h"
#include "m6_release.h"
#include "m7_sd_update_esp.h"
#include "p1_5_sd_readonly.h"
#include "p5_1_ui_view.h"

// XiaoZhi's Application owns the main task and the offline player runs in its
// own task. The player orchestration below mirrors m2_library_main.cc (the
// offline-only variant); change the two together.

#ifndef BOILON_M2_RESUME
#define BOILON_M2_RESUME 1
#endif

namespace boilon::ai {
[[noreturn]] void PowerOff(const char* reason, int code = 0);
int SetAmplifier(bool enable);
// The player only asks for the hotspot and shows network notices.
void InspectStorage(esp_err_t nvs_result);
void RequestHotspot();
std::optional<Notice> PollNetwork(uint64_t now_ms);
// Application is idle, Wi-Fi is up and the microphone works.
bool AiReady();
bool TakeBackToStory();  // The "back to story" MCP tool was called.
void ApplyBargeIn();     // Before a conversation: the saved barge-in switch (P5-3).
int BatteryLevel();      // For self.get_device_status; -1 while unknown.
}  // namespace boilon::ai

namespace {
using boilon::ai::PowerOff;
using boilon::ai::SetAmplifier;
// Keep the accepted log tag so existing capture summaries still apply.
constexpr char kTag[] = "BoilonLibrary";
portMUX_TYPE power_lock = portMUX_INITIALIZER_UNLOCKED;
bool shutting_down = false;
bool backlight_ready = false;
std::string mounted_library;
std::atomic<uint64_t> owner_progress_ms{0};
std::atomic<int> battery_level{-1};
uint64_t NowMs();
uint8_t Pressed();
bool RequestedShutdown() {
    static media::PlayerGestures gestures;
    return gestures.Update(Pressed(), NowMs()) == media::GestureAction::kShutdownRequest;
}
// Same bounded, nonblocking failure report as the accepted microphone probe.
// Do not use the logging/VFS lock on the path that must remove power.
void ReportFailure(const char* reason, int code) {
    const int64_t deadline = esp_timer_get_time() + 100000;
    char line[192];
    const int length = std::snprintf(
        line, sizeof(line),
        "\nI (%lu) BoilonLibrary: Startup/runtime error reason=%s code=%d report_budget_ms=100\n",
        static_cast<unsigned long>(esp_log_timestamp()), reason, code);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(line))
        return;
    size_t sent = 0;
    bool final_flush = false;
    usb_serial_jtag_ll_txfifo_flush();
    while (esp_timer_get_time() < deadline) {
        if (usb_serial_jtag_ll_txfifo_writable()) {
            if (sent < static_cast<size_t>(length)) {
                const size_t remaining = static_cast<size_t>(length) - sent;
                sent += usb_serial_jtag_ll_write_txfifo(
                    reinterpret_cast<const uint8_t*>(line + sent), remaining < 64 ? remaining : 64);
                usb_serial_jtag_ll_txfifo_flush();
            } else if (!final_flush) {
                usb_serial_jtag_ll_txfifo_flush();
                final_flush = true;
            }
        }
    }
}
void StopBacklight() {
    portENTER_CRITICAL(&power_lock);
    if (backlight_ready)
        ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    portEXIT_CRITICAL(&power_lock);
}
void SetBacklight(bool enable) {
    portENTER_CRITICAL(&power_lock);
    if (!shutting_down && backlight_ready) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0,
                      enable ? 1023 * boilon::lcd_cal::kBrightnessPercent / 100 : 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    }
    portEXIT_CRITICAL(&power_lock);
}
void Check(int result, const char* reason) {
    if (result != 0)
        PowerOff(reason, result);
}
boilon::local_library::LibraryLcd& Lcd() {
    static boilon::local_library::LibraryLcd lcd([](const char* reason) { PowerOff(reason); },
                                                 SetBacklight);
    return lcd;
}
// The battery ADC: the storage worker samples it while there is a card; the
// no-story page samples it itself.
boilon::local_library::BatteryAdc& Battery() {
    static boilon::local_library::BatteryAdc battery;
    return battery;
}
// M5 batch 5: one watch for the whole boot, so the 10% notice shows once even
// when a card error moves the player to the no-story page.
boilon::power::PowerWatch& Power() {
    static boilon::power::PowerWatch watch;
    return watch;
}
// Complete the first visible frame before entering synchronous SD preparation.
void ShowPage(boilon::local_library::LibraryLcd& lcd, boilon::ui::Page page) {
    boilon::ui::Model model;
    model.page = page;
    model.version = boilon::release::kVersion;
    lcd.Show(model, NowMs());
    do {
        owner_progress_ms = NowMs();
        lcd.Poll(esp_timer_get_time() / 1000);
        vTaskDelay(pdMS_TO_TICKS(10));
    } while (!lcd.Ready(esp_timer_get_time() / 1000));
}
// M7: the SD update's page; a failure stays about 3 s, then the boot goes on.
// kUpdated says which version now runs, also about 3 s.
void ShowUpdate(boilon::local_library::LibraryLcd& lcd, const std::string& version, int percent,
                boilon::ui::Page page = boilon::ui::Page::kUpdating) {
    boilon::ui::Model model;
    model.page = page;
    model.title = version;
    model.progress = percent;
    lcd.Show(model, NowMs());
    const uint64_t until =
        NowMs() + (percent < 0 || page == boilon::ui::Page::kUpdated ? 3000 : 0);
    do {
        owner_progress_ms = NowMs();
        lcd.Poll(esp_timer_get_time() / 1000);
        vTaskDelay(pdMS_TO_TICKS(10));
    } while (!lcd.Ready(esp_timer_get_time() / 1000) || NowMs() < until);
}
// P5-8: catalog names are small heap blocks, and the player, the bookmarks and
// the resume policy each keep a copy. Build them in PSRAM, not internal RAM;
// only the scan and the copies run inside the window.
void CatalogInPsram(bool on) {
#if CONFIG_SPIRAM_USE_MALLOC
    heap_caps_malloc_extmem_enable(on ? 0 : CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
#endif
}
// The no-story page (below) replaces the old idle and error pages.
[[noreturn]] void EmptyLibrary(boilon::local_library::LibraryLcd& lcd, AudioService& audio,
                               boilon::local_player::LocalCodec& codec, const char* state,
                               int code = 0);
[[noreturn]] void MediaError(boilon::local_library::LibraryLcd& lcd, AudioService& audio,
                             boilon::local_player::LocalCodec& codec, const char* reason,
                             int code = 0, int volume = boilon::ai::NoStoryPage::kStartVolume);
media::MediaCatalog MountLibrary(boilon::local_library::LibraryLcd& lcd, AudioService& audio,
                                 boilon::local_player::LocalCodec& codec) {
    namespace sd = boilon::sd_probe;
    static sdmmc_card_t card;
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags &= ~(SDMMC_HOST_FLAG_4BIT | SDMMC_HOST_FLAG_8BIT | SDMMC_HOST_FLAG_DDR);
    // Card identification still uses the driver's probing clock before data transfer.
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    host.command_timeout_ms = 1000;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = GPIO_NUM_47;
    slot.cmd = GPIO_NUM_48;
    slot.d0 = GPIO_NUM_21;
    slot.d1 = slot.d2 = slot.d3 = slot.d4 = slot.d5 = slot.d6 = slot.d7 = GPIO_NUM_NC;
    slot.cd = SDMMC_SLOT_NO_CD;
    slot.wp = SDMMC_SLOT_NO_WP;
    ESP_LOGI(kTag, "Startup stage: SD_HOST");
    Check(host.init(), "SD_HOST");
    Check(sdmmc_host_init_slot(host.slot, &slot), "SD_SLOT");
    const int card_result = sdmmc_card_init(&host, &card);
    if (card_result != ESP_OK)
        EmptyLibrary(lcd, audio, codec, "SD_UNAVAILABLE", card_result);
    ESP_LOGI(kTag, "Startup stage: SD_CARD_READY");
    Check(ff_diskio_get_drive(&sd::drive), "SD_DRIVE");
    sd::card = &card;
    ff_diskio_register(sd::drive, &sd::kReadOnly);
    char volume[5];
    snprintf(volume, sizeof(volume), "%u:", sd::drive);
    esp_vfs_fat_conf_t vfs = {"/sdcard", volume, 4};
    FATFS* filesystem = nullptr;
    Check(esp_vfs_fat_register(&vfs, &filesystem), "SD_VFS");
    const int mount_result = f_mount(filesystem, volume, 1);
    if (mount_result != FR_OK)
        MediaError(lcd, audio, codec, "SD_MOUNT", mount_result);
    if (filesystem->fs_type != FS_FAT32)
        MediaError(lcd, audio, codec, "SD_NOT_FAT32");
    ESP_LOGI(kTag, "Startup stage: FAT32_MOUNTED");
    // M7: a newer release in the card's root is installed now and restarts (AI variant only;
    // the offline variant stays a USB-installed fallback).
    boilon::sd_update::Run(
        "/sdcard", {[&lcd](const std::string& version, int percent) {
                        ShowUpdate(lcd, version, percent);
                        if (percent < 0)
                            ShowPage(lcd, boilon::ui::Page::kChecking);
                    },
                    [] {
                        boilon::local_library::BatteryEstimate estimate;
                        return estimate.Update(Battery().Read());
                    }});
    if (boilon::sd_update::JustUpdated()) {
        ShowUpdate(lcd, boilon::release::kVersion, 100, boilon::ui::Page::kUpdated);
        ShowPage(lcd, boilon::ui::Page::kChecking);
    }
    std::string library =
        "sd-" + std::to_string(card.cid.mfg_id) + "-" + std::to_string(card.cid.serial);
    mounted_library = library;
    CatalogInPsram(true);
    auto result = boilon::local_library::Scan("/sdcard", library);
    CatalogInPsram(false);
    if (result.error == boilon::local_library::ScanError::kEmpty)
        EmptyLibrary(lcd, audio, codec, "NO_MUSIC");
    if (!result.Ok())
        MediaError(lcd, audio, codec, "CATALOG_SCAN", static_cast<int>(result.error));
    ESP_LOGI(kTag, "Scanned %u entries, %u collections; playing first collection",
             static_cast<unsigned>(result.entries),
             static_cast<unsigned>(result.catalog.Collections().size()));
    return std::move(result.catalog);
}
uint64_t NowMs() { return esp_timer_get_time() / 1000; }
void Trace(const char* event, const std::optional<media::ResolvedTrack>& track,
           LocalMp3Player& player) {
    if (!track)
        return;
    // Bounded hex avoids newlines/control characters in FAT file names.
    char name[193];
    const auto& file = track->track.file_name;
    const size_t count = std::min<size_t>(file.size(), 96);
    constexpr char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < count; ++i) {
        const auto byte = static_cast<unsigned char>(file[i]);
        name[2 * i] = hex[byte >> 4];
        name[2 * i + 1] = hex[byte & 15];
    }
    name[2 * count] = 0;
    const bool request = std::strcmp(event, "REQUEST") == 0;
    ESP_LOGI(kTag,
             "Trace event=%s track=%llu name_hex=%s truncated=%d output_ms=%llu duration_ms=%llu",
             event, static_cast<unsigned long long>(track->track.id.value), name,
             file.size() > count ? 1 : 0,
             static_cast<unsigned long long>(request ? 0 : player.PositionMs()),
             static_cast<unsigned long long>(request ? 0 : player.DurationMs()));
}
uint8_t Pressed() {
    using G = media::PlayerGestures;
    // Accepted Gen2 polarity: top GPIO3 is HIGH, middle/bottom are LOW.
    return (gpio_get_level(GPIO_NUM_3) ? G::kPower : 0) |
           (gpio_get_level(GPIO_NUM_6) ? 0 : G::kUp) | (gpio_get_level(GPIO_NUM_7) ? 0 : G::kDown);
}
// Application and the story runtime behind the session policy: only one of
// them owns the speaker at a time. Without a runtime (the no-story page)
// there is nothing to hand over or resume.
class StoryFocus final : public boilon::ai::SessionPort {
public:
    StoryFocus(boilon::local_library::Runtime<LocalMp3Player>* runtime, AudioService& audio)
        : runtime_(runtime), audio_(audio) {}
    boilon::ai::Chat App() override {
        using boilon::ai::Chat;
        const auto state = Application::GetInstance().GetDeviceState();
        upgrading_ = state == kDeviceStateUpgrading;
        switch (state) {
            case kDeviceStateConnecting:
                return Chat::kConnecting;
            case kDeviceStateListening:
                return Chat::kListening;
            case kDeviceStateSpeaking:
                return Chat::kSpeaking;
            case kDeviceStateNotifying:
            case kDeviceStateUpgrading:
            case kDeviceStateAudioTesting:
                return Chat::kBusy;
            default:
                return Chat::kQuiet;
        }
    }
    bool ParkStory(uint64_t now_ms) override { return !runtime_ || runtime_->Park(now_ms); }
    bool StoryReleased() override { return !runtime_ || runtime_->Released(); }
    bool StoryWaiting() override { return runtime_ && runtime_->ParkedWhilePlaying(); }
    bool ResumeStory(uint64_t now_ms) override {
        if (!runtime_)
            return true;
        if (const auto pick = std::exchange(pick_, std::nullopt))
            return runtime_->Choose(pick->directory, pick->number, now_ms);
        return runtime_->Unpark(now_ms);
    }
    // P5-2: played instead of the parked story when the conversation ends.
    void Choose(boilon::ai::card::Pick pick) { pick_ = pick; }
    // M5 batch 5: upstream was in its upgrade state at the last App() read.
    bool Upgrading() const { return upgrading_; }
    bool OutputIdle() override { return audio_.IsPlaybackIdle(); }
    // Like a wake word: Application plays its prompt sound, then listens.
    void StartChat() override {
        boilon::ai::ApplyBargeIn();
        Application::GetInstance().WakeWordInvoke("double_click");
    }
    void CloseChat() override { Application::GetInstance().CloseConversation(); }
    void DropAiAudio() override { audio_.ResetDecoder(); }

private:
    boilon::local_library::Runtime<LocalMp3Player>* runtime_;
    AudioService& audio_;
    std::optional<boilon::ai::card::Pick> pick_;
    bool upgrading_ = false;
};
// M5 batch 5: the no-story page powers off by itself. There is nothing to save:
// the low-battery page shows about 3 s, the other reasons go straight off.
[[noreturn]] void NoStoryOff(boilon::local_library::LibraryLcd& lcd,
                             boilon::power::PowerWatch::Off off) {
    ESP_LOGI(kTag, "Automatic shutdown reason=%s without a story",
             boilon::power::PowerWatch::kOffNames[static_cast<int>(off)]);
    if (off == boilon::power::PowerWatch::Off::kLowBattery) {
        ShowPage(lcd, boilon::ui::Page::kLowBattery);
        for (const uint64_t until = NowMs() + 3000; NowMs() < until;) {
            owner_progress_ms = NowMs();
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    PowerOff("USER");
}
// No card, an empty card or a card error: the page says why, and XiaoZhi, the
// volume keys and the hotspot gesture still work. A long top press powers off,
// and so do low battery, three idle minutes and a stuck upgrade (M5 batch 5).
// The volume is not saved: there is no card to save it to.
[[noreturn]] void NoStory(boilon::local_library::LibraryLcd& lcd, AudioService& audio,
                          boilon::local_player::LocalCodec& codec, const char* state, int code,
                          int volume, boilon::ui::Page screen) {
    boilon::ai::card::Card::Shared().Publish(nullptr);  // The tools now say there is no card.
    boilon::sd_update::ConfirmIfPending();  // M7: startup completed on a no-story page.
    codec.EnableOutput(false);  // AudioService turns it back on for XiaoZhi.
    codec.SetOutputVolume(volume);
    ESP_LOGI(kTag, "Media idle state=%s code=%d volume=%d", state, code, volume);
    StoryFocus focus(nullptr, audio);
    boilon::ai::NoStoryPage page(focus, volume);
    uint64_t opened_ms = 0;
    // Same cadence and estimate as the storage worker; the first sample waits a
    // full period, so it never overlaps a worker sample still in flight.
    boilon::local_library::BatteryEstimate battery;
    uint64_t sampled_ms = NowMs();
    auto& power = Power();
    bool network_page = false;  // The hotspot or activation page is up.
    while (true) {
        const uint64_t now = NowMs();
        owner_progress_ms = now;
        const bool ai_ready = boilon::ai::AiReady();
        const auto tick = page.Poll(Pressed(), now, ai_ready, codec.OutputVolume(),
                                    boilon::ai::TakeBackToStory());
        using Action = media::GestureAction;
        if (tick.gesture == Action::kShutdownRequest)
            PowerOff("USER");
        if (tick.gesture == Action::kParentPageRequest)
            boilon::ai::RequestHotspot();
        if (tick.gesture == Action::kOpenAi) {
            opened_ms = now;
            ESP_LOGI(kTag, "AI open %s without a story",
                     page.Conversation().Active() ? "accepted" : "refused");
        }
        if (tick.volume_changed) {
            codec.SetOutputVolume(page.Volume());
            ESP_LOGI(kTag, "Output volume=%d without a story", page.Volume());
        }
        if (tick.event != boilon::ai::Event::kNone)
            ESP_LOGI(kTag, "AI event=%d reason=%s since_open_ms=%llu", static_cast<int>(tick.event),
                     boilon::ai::kEndReasonNames[static_cast<int>(page.Conversation().Reason())],
                     static_cast<unsigned long long>(now - opened_ms));
        std::optional<int> percent;  // A new reading this tick.
        if (now - sampled_ms >= 5000) {
            sampled_ms = now;
            const auto reading = Battery().Read();
            battery_level = battery.Update(reading);
            percent = battery_level.load();
            ESP_LOGI("BoilonBattery",
                     "raw=%d adc_pad_mv=%d battery_mv=%d divider=3 percent=%d "
                     "model=factory_estimate",
                     reading.raw, reading.adc_pad_mv,
                     boilon::local_library::BatteryEstimate::Millivolts(reading),
                     battery_level.load());
        }
        const bool idle = !Pressed() && !page.Conversation().OwnsKeys() && !network_page;
        if (const auto off = power.Update(now, percent, idle, focus.Upgrading());
            off != boilon::power::PowerWatch::Off::kNone)
            NoStoryOff(lcd, off);
        lcd.Poll(now);
        if (lcd.Ready(now)) {
            boilon::ui::Model model;
            model.page = screen;
            model.ai_ready = ai_ready;
            model.battery = battery_level;
            const auto notice = boilon::ai::PollNetwork(now);  // Closes an expired hotspot.
            network_page = notice && (notice->kind == boilon::ai::Notice::Kind::kHotspot ||
                                      notice->kind == boilon::ai::Notice::Kind::kActivating);
            boilon::ai::ShowNotice(model, notice);
            if (page.Conversation().Active())
                boilon::ai::ShowConversation(model, page.Conversation(), false, true,
                                             boilon::ai::CurrentChat().View(),
                                             boilon::ui::CurrentStyle().subtitles);
            else if (page.Refused(now))
                model.toast = boilon::ui::Toast::kAiOffline;
            if (power.ShowWarning(model.page == screen && model.toast == boilon::ui::Toast::kNone,
                                  now))
                model.toast = boilon::ui::Toast::kLowBattery;
            lcd.Show(model, now);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
[[noreturn]] void EmptyLibrary(boilon::local_library::LibraryLcd& lcd, AudioService& audio,
                               boilon::local_player::LocalCodec& codec, const char* state,
                               int code) {
    const bool unavailable = std::strcmp(state, "SD_UNAVAILABLE") == 0;
    NoStory(lcd, audio, codec, state, code, boilon::ai::NoStoryPage::kStartVolume,
            unavailable ? boilon::ui::Page::kNoCard : boilon::ui::Page::kEmpty);
}
// Media faults no longer power off: the page stays until a long top press.
// Hardware failures and the independent deadline still cut power.
[[noreturn]] void MediaError(boilon::local_library::LibraryLcd& lcd, AudioService& audio,
                             boilon::local_player::LocalCodec& codec, const char* reason,
                             int code, int volume) {
    NoStory(lcd, audio, codec, reason, code, volume, boilon::ui::Page::kCardError);
}
[[noreturn]] void NormalShutdown(boilon::local_library::LibraryLcd& lcd, LocalMp3Player& player,
                                 boilon::local_library::Runtime<LocalMp3Player>& runtime,
                                 boilon::local_library::ResumeWorker* resume,
                                 boilon::ui::Page page = boilon::ui::Page::kGoodbye) {
    // Snapshot before controls can invalidate position; no further Runtime.Tick.
    const bool queued = runtime.Shutdown(NowMs());
    if (player.Running())
        player.Pause(true);
    player.Stop();
    const uint64_t deadline = NowMs() + 15000;
    uint64_t shown_until = 0;  // The low-battery page stays long enough to read.
    bool page_started = false;
    while (NowMs() < deadline && (!page_started || !lcd.Ready(NowMs()) || NowMs() < shown_until ||
                                  player.Running() || (resume && resume->PendingJobs() != 0))) {
        owner_progress_ms = NowMs();
        lcd.Poll(NowMs());
        if (!page_started && lcd.Ready(NowMs())) {
            boilon::ui::Model model;
            model.page = page;
            lcd.Show(model, NowMs());
            page_started = true;
            if (page == boilon::ui::Page::kLowBattery)
                shown_until = NowMs() + 3000;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    const bool saved = queued && resume && resume->PendingJobs() == 0 &&
                       (resume->LastSaveStatus() == media::SaveStatus::kSaved ||
                        resume->LastSaveStatus() == media::SaveStatus::kUnchanged);
    ESP_LOGI(kTag, "Normal shutdown saved=%d pending=%u", saved,
             resume ? static_cast<unsigned>(resume->PendingJobs()) : 0);
    PowerOff("USER");  // A blocked storage call must never prevent power-off.
}
// The player depends only on AudioService's local PCM interface and the board
// codec, never on Application's state: it must keep working while Application
// stays in Starting without a network.
[[noreturn]] void RunPlayer() {
    auto& lcd = Lcd();
    Battery().Initialize();
    auto& audio = Application::GetInstance().GetAudioService();
    auto& codec =
        *static_cast<boilon::local_player::LocalCodec*>(Board::GetInstance().GetAudioCodec());
    auto catalog = MountLibrary(lcd, audio, codec);
    static LocalMp3Player player(audio);
    boilon::local_library::ResumePort* resume = nullptr;
    boilon::local_library::ResumeWorker* persistence = nullptr;
#if BOILON_M2_RESUME
    CatalogInPsram(true);
    static boilon::local_library::ResumeWorker resume_worker(catalog, mounted_library, &Battery());
    CatalogInPsram(false);
    if (!resume_worker.Start())
        PowerOff("RESUME_TASK");
    resume = &resume_worker;
    persistence = &resume_worker;
#endif
    std::optional<media::ResolvedTrack> active_track;
    auto observe = [&](const char* event, const media::PlayerController& controller) {
        const auto selected = controller.CurrentTrack();
        if (std::strcmp(event, "START") == 0)
            active_track = selected;
        Trace(event, std::strcmp(event, "REQUEST") == 0 ? selected : active_track, player);
    };
    boilon::local_library::Runtime<LocalMp3Player> runtime(
        std::move(catalog), player, [&audio] { return audio.IsPlaybackIdle(); }, observe, resume,
        [&codec](int volume) { codec.SetOutputVolume(volume); });
    using boilon::local_player::kDacGainDb, boilon::local_player::kMaxVolume;
    ESP_LOGI(kTag, "Trace config rate=%d dac_db=%.1f volume=10 volume_max=%d step_db=%.1f",
             audio.LocalOutputSampleRate(), kDacGainDb, kMaxVolume,
             5 * boilon::local_player::kDbPerVolume);
    if (!runtime.Start(NowMs()))
        PowerOff("PLAY_START");
    boilon::sd_update::ConfirmIfPending();  // M7: startup completed with a story.
    auto& card = boilon::ai::card::Card::Shared();
    card.Publish(&runtime.Catalog());  // Unchanged from here on; the tools only read it.
    StoryFocus focus(&runtime, audio);
    boilon::ai::Session session(focus);
    std::optional<uint64_t> refused_ms;  // A double-click the story could not hand over.
    uint64_t opened_ms = 0;
    bool finished = false;
    uint64_t last_progress = NowMs();
    uint64_t last_battery = 0;
    auto& power = Power();
    uint32_t battery_samples = 0;
    bool network_page = false;  // The hotspot or activation page is up.
    while (true) {
        owner_progress_ms = NowMs();
        if (RequestedShutdown())
            NormalShutdown(lcd, player, runtime, persistence);
        runtime.SetAi(session.OwnsKeys(), boilon::ai::AiReady());
        // XiaoZhi's set_volume tool changes the codec directly: adopt its value
        // like a key press, so the screen, the keys and the next save follow.
        if (const int volume = codec.OutputVolume(); volume != runtime.Volume()) {
            runtime.AdoptVolume(volume, NowMs());
            ESP_LOGI(kTag, "Output volume=%d set by XiaoZhi", volume);
        }
        if (!runtime.Tick(Pressed(), NowMs()))
            MediaError(lcd, audio, codec, "PLAY_CONTROL", 0, runtime.Volume());
        auto gesture = runtime.LastGesture();
        using Action = media::GestureAction;
        using boilon::ai::EndReason;
        if (gesture == Action::kOpenAi) {
            const auto state = runtime.Controller().State();
            if (session.Open(NowMs()))
                opened_ms = NowMs();
            else
                refused_ms = NowMs();
            ESP_LOGI(kTag, "AI open %s story_state=%d", session.Active() ? "accepted" : "refused",
                     static_cast<int>(state));
        }
        if (gesture == Action::kCancelAi)
            session.Close(EndReason::kButton, NowMs());
        if (boilon::ai::TakeBackToStory())
            session.Close(EndReason::kTool, NowMs());
        // P5-2: in a conversation the pick plays once 喵喵 finishes the answer;
        // outside one (the server called the tool) it plays now.
        card.SetNow(runtime.Selected());
        if (const auto pick = card.Take()) {
            ESP_LOGI(kTag, "AI play directory=%u number=%u in_chat=%d",
                     static_cast<unsigned>(pick->directory), static_cast<unsigned>(pick->number),
                     session.Active());
            if (session.Active()) {
                focus.Choose(*pick);
                session.EndAfterReply(NowMs());
            } else if (!runtime.Choose(pick->directory, pick->number, NowMs())) {
                ESP_LOGW(kTag, "AI play refused: the player is switching");
            }
        }
        // Events: 1 chat requested, 2 ended, 3 story resumed (delay 0 after a push or
        // late reply), 4 closed a conversation nobody opened.
        if (const auto event = session.Poll(NowMs()); event != boilon::ai::Event::kNone)
            ESP_LOGI(kTag, "AI event=%d reason=%s since_open_ms=%llu resume_delay_ms=%llu",
                     static_cast<int>(event),
                     boilon::ai::kEndReasonNames[static_cast<int>(session.Reason())],
                     static_cast<unsigned long long>(NowMs() - opened_ms),
                     static_cast<unsigned long long>(session.ResumeDelayMs()));
        if (gesture != Action::kNone) {
            bool supported =
                gesture == Action::kTogglePlayback || gesture == Action::kNextTrack ||
                gesture == Action::kPreviousTrack || gesture == Action::kOpenDirectory ||
                gesture == Action::kBrowseNext || gesture == Action::kBrowsePrevious ||
                gesture == Action::kConfirmCollection || gesture == Action::kVolumeUp ||
                gesture == Action::kVolumeDown || gesture == Action::kOfflineHint ||
                gesture == Action::kParentPageRequest || gesture == Action::kOpenAi ||
                gesture == Action::kCancelAi;
            if (gesture == Action::kParentPageRequest)
                boilon::ai::RequestHotspot();
            if (gesture == Action::kVolumeUp || gesture == Action::kVolumeDown) {
                const int volume = runtime.View(NowMs()).volume;
                codec.SetOutputVolume(volume);
                ESP_LOGI(kTag, "Output volume=%d; DAC remains %.1f dB", volume, kDacGainDb);
            }
            ESP_LOGI(kTag, "Gesture=%d %s; requested playback state=%d", static_cast<int>(gesture),
                     supported ? "handled" : "not connected in this variant",
                     static_cast<int>(runtime.Controller().State()));
        }
        if (runtime.Finished() && !finished) {
            codec.EnableOutput(false);
            ESP_LOGI(kTag,
                     "Collection finished; long-press top to power off or select another track");
        }
        finished = runtime.Finished();
        const auto now = NowMs();
        if (persistence && now - last_battery >= 5000) {
            last_battery = now;
            persistence->SampleBattery();
        }
        // M5 batch 5: like a long press when the battery runs out, after three
        // minutes with nothing to hear, or when upstream sits in its upgrade state.
        std::optional<int> percent;  // A reading the worker finished since the last tick.
        if (persistence && persistence->BatterySamples() != battery_samples) {
            battery_samples = persistence->BatterySamples();
            percent = persistence->BatteryPercent();
        }
        const bool idle = !Pressed() && !session.OwnsKeys() && !network_page &&
                          runtime.Controller().State() != media::PlaybackState::kPlaying;
        if (const auto off = power.Update(now, percent, idle, focus.Upgrading());
            off != boilon::power::PowerWatch::Off::kNone) {
            ESP_LOGI(kTag, "Automatic shutdown reason=%s battery=%d",
                     boilon::power::PowerWatch::kOffNames[static_cast<int>(off)],
                     persistence ? persistence->BatteryPercent() : -1);
            NormalShutdown(lcd, player, runtime, persistence,
                           off == boilon::power::PowerWatch::Off::kLowBattery
                               ? boilon::ui::Page::kLowBattery
                               : boilon::ui::Page::kGoodbye);
        }
        lcd.Poll(now);
        if (lcd.Ready(now)) {
            const auto view = runtime.View(now);
            battery_level = persistence ? persistence->BatteryPercent() : -1;
            auto model = boilon::ui::PlayerModel(view, runtime.PositionMs(), battery_level);
            model.ai_ready = boilon::ai::AiReady();
            // Polling also closes an expired hotspot.
            const auto notice = boilon::ai::PollNetwork(now);
            network_page = notice && (notice->kind == boilon::ai::Notice::Kind::kHotspot ||
                                      notice->kind == boilon::ai::Notice::Kind::kActivating);
            if (!view.directory_open && !view.failed)
                boilon::ai::ShowNotice(model, notice);
            if (session.Active() && !view.failed) {
                // "Story paused" only when the conversation interrupted playback.
                boilon::ai::ShowConversation(model, session, runtime.ParkedWhilePlaying(), false,
                                             boilon::ai::CurrentChat().View(),
                                             boilon::ui::CurrentStyle().subtitles);
            } else if (refused_ms && now - *refused_ms < 2000) {
                model.toast = boilon::ui::Toast::kAiOffline;
            }
            if (power.ShowWarning(model.page == boilon::ui::Page::kPlayer &&
                                      model.toast == boilon::ui::Toast::kNone,
                                  now))
                model.toast = boilon::ui::Toast::kLowBattery;
            lcd.Show(model, now);
        }
        if (player.Running() && now - last_progress >= 5000) {
            Trace("PROGRESS", active_track, player);
            last_progress = now;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
}  // namespace

namespace boilon::ai {
int BatteryLevel() { return battery_level.load(); }
int SetAmplifier(bool enable) {
    portENTER_CRITICAL(&power_lock);
    int result = enable && shutting_down ? ESP_ERR_INVALID_STATE
                                         : gpio_set_level(GPIO_NUM_42, enable ? 1 : 0);
    portEXIT_CRITICAL(&power_lock);
    return result;
}
[[noreturn]] void PowerOff(const char* reason, int code) {
    portENTER_CRITICAL(&power_lock);
    shutting_down = true;
    const bool pa_off = gpio_set_level(GPIO_NUM_42, 0) == ESP_OK;
    portEXIT_CRITICAL(&power_lock);
    StopBacklight();
    if (pa_off && std::strcmp(reason, "DEADLINE") != 0 && std::strcmp(reason, "USER") != 0)
        ReportFailure(reason, code);
    rtc_gpio_set_level(BOILON_S3_GEN2_POWER_ENABLE_GPIO, 0);
    ESP_LOGI(kTag, "Power off: %s", reason);
    while (true)
        vTaskDelay(pdMS_TO_TICKS(1000));
}
}  // namespace boilon::ai

extern "C" void app_main() {
    Check(rtc_gpio_set_level(BOILON_S3_GEN2_POWER_ENABLE_GPIO, 1), "LATCH_PRELOAD");
    Check(rtc_gpio_init(BOILON_S3_GEN2_POWER_ENABLE_GPIO), "LATCH_INIT");
    Check(rtc_gpio_set_level(BOILON_S3_GEN2_POWER_ENABLE_GPIO, 1), "LATCH_LEVEL");
    Check(rtc_gpio_set_direction(BOILON_S3_GEN2_POWER_ENABLE_GPIO, RTC_GPIO_MODE_OUTPUT_ONLY),
          "LATCH_DIRECTION");
    Check(gpio_reset_pin(GPIO_NUM_42), "PA_RESET");
    Check(gpio_set_level(GPIO_NUM_42, 0), "PA_PRELOAD");
    Check(gpio_set_direction(GPIO_NUM_42, GPIO_MODE_OUTPUT), "PA_DIRECTION");
    if (xTaskCreate(
            [](void*) {
                while (true) {
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    if (NowMs() - owner_progress_ms.load() >= 30000)
                        PowerOff("OWNER_TIMEOUT");
                }
            },
            "local_power_off", 2048, nullptr, 5, nullptr) != pdPASS)
        PowerOff("DEADLINE_TASK");
    ESP_LOGI(kTag, "%s", boilon::release::kName);
    boilon::sd_update::LogBoot();
    ESP_LOGI(kTag,
             "M3 AI variant; XiaoZhi Application, Wi-Fi once saved, microphone in conversations, "
             "read-only FAT32, NVS resume=%d, shutdown by long press, low battery or 3 idle min",
             BOILON_M2_RESUME);
    ESP_LOGI(kTag, "Top short: pause/resume; middle/bottom hold 0.8s: previous/next");
    Check(gpio_input_enable(GPIO_NUM_3), "TOP_INPUT");
    for (auto pin : {GPIO_NUM_6, GPIO_NUM_7}) {
        Check(gpio_input_enable(pin), "BUTTON_INPUT");
        Check(gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY), "BUTTON_PULLUP");
    }
    auto& lcd = Lcd();
    lcd.Initialize();
    portENTER_CRITICAL(&power_lock);
    backlight_ready = true;
    portEXIT_CRITICAL(&power_lock);
    ShowPage(lcd, boilon::ui::Page::kChecking);
    ESP_LOGI(kTag, "Startup stage: LCD_READY");
    // Application settings and the resume worker share the default partition.
    // Never erase on failure: playback continues and resume reports the error.
    const esp_err_t nvs = nvs_flash_init();
    if (nvs != ESP_OK)
        ESP_LOGE(kTag, "NVS init failed (0x%x); recovery erase is disabled", nvs);
    boilon::ai::InspectStorage(nvs);  // Before the board decides whether Wi-Fi may start.
    auto& app = Application::GetInstance();
    app.Initialize();  // Starts the board codec and AudioService; Wi-Fi starts later.
    owner_progress_ms = NowMs();
    // The player keeps the accepted main-task stack and CPU0 placement.
    if (xTaskCreatePinnedToCore([](void*) { RunPlayer(); }, "story_player", 8192, nullptr, 1,
                                nullptr, 0) != pdPASS)
        PowerOff("PLAYER_TASK");
    app.Run();  // Never returns.
}
#endif
