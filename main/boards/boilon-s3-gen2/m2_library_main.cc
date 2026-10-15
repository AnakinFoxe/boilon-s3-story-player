#include "sdkconfig.h"
#if CONFIG_BOILON_S3_GEN2_LOCAL_LIBRARY
#include <driver/rtc_io.h>
#include <driver/sdmmc_host.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_vfs_fat.h>
#include <hal/usb_serial_jtag_ll.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include "audio/audio_service.h"
#include "audio/local_mp3_player.h"
#include "config.h"
#include "m2_battery_adc.h"
#include "m2_library_lcd.h"
#include "m2_library_runtime.h"
#include "m2_local_codec.h"
#include "m2_resume_worker.h"
#include "m5_power_policy.h"
#include "m6_release.h"
#include "p1_5_sd_readonly.h"
#include "p5_1_ui_view.h"

#ifndef BOILON_M2_RESUME
#define BOILON_M2_RESUME 1
#endif

namespace {
constexpr char kTag[] = "BoilonLibrary";
portMUX_TYPE power_lock = portMUX_INITIALIZER_UNLOCKED;
bool shutting_down = false;
bool backlight_ready = false;
std::string mounted_library;
std::atomic<uint64_t> owner_progress_ms{0};
uint64_t NowMs();
uint8_t Pressed();
bool RequestedShutdown() {
    static media::PlayerGestures gestures;
    return gestures.Update(Pressed(), NowMs()) == media::GestureAction::kShutdownRequest;
}
int SetAmplifier(bool enable) {
    portENTER_CRITICAL(&power_lock);
    int result = enable && shutting_down ? ESP_ERR_INVALID_STATE
                                         : gpio_set_level(GPIO_NUM_42, enable ? 1 : 0);
    portEXIT_CRITICAL(&power_lock);
    return result;
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
[[noreturn]] void PowerOff(const char* reason, int code = 0) {
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
void Check(int result, const char* reason) {
    if (result != 0)
        PowerOff(reason, result);
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
// P5-8: catalog names are small heap blocks, and the player, the bookmarks and
// the resume policy each keep a copy. Build them in PSRAM, not internal RAM;
// only the scan and the copies run inside the window.
void CatalogInPsram(bool on) {
#if CONFIG_SPIRAM_USE_MALLOC
    heap_caps_malloc_extmem_enable(on ? 0 : CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
#endif
}
[[noreturn]] void EmptyLibrary(boilon::local_library::LibraryLcd& lcd, const char* state,
                               int code = 0) {
    Check(SetAmplifier(false), "PA_OFF");
    const bool unavailable = std::strcmp(state, "SD_UNAVAILABLE") == 0;
    ShowPage(lcd, unavailable ? boilon::ui::Page::kNoCard : boilon::ui::Page::kEmpty);
    ESP_LOGI(kTag, "Media idle state=%s code=%d", state, code);
    // No playback/NVS/remount. The same long-press shutdown works without media,
    // and three minutes without a key also power off (M5 batch 5).
    boilon::power::Hold idle(boilon::power::kIdleMs);
    while (true) {
        owner_progress_ms = NowMs();
        if (RequestedShutdown())
            PowerOff("USER");
        if (idle.Update(!Pressed(), NowMs()))
            PowerOff("USER");
        lcd.Poll(esp_timer_get_time() / 1000);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
// Expected media faults stay silent and visible briefly. Only the owner task
// renders here; hardware failures and the independent deadline still cut power.
[[noreturn]] void MediaError(boilon::local_library::LibraryLcd& lcd, const char* reason,
                             int code = 0) {
    Check(SetAmplifier(false), "PA_OFF");
    boilon::ui::Model model;
    model.page = boilon::ui::Page::kCardError;
    const int64_t deadline = esp_timer_get_time() + 3000000;
    while (esp_timer_get_time() < deadline) {
        const uint64_t now_ms = esp_timer_get_time() / 1000;
        lcd.Poll(now_ms);
        if (lcd.Ready(now_ms))
            lcd.Show(model, now_ms);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    PowerOff(reason, code);
}
media::MediaCatalog MountLibrary(boilon::local_library::LibraryLcd& lcd) {
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
        EmptyLibrary(lcd, "SD_UNAVAILABLE", card_result);
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
        MediaError(lcd, "SD_MOUNT", mount_result);
    if (filesystem->fs_type != FS_FAT32)
        MediaError(lcd, "SD_NOT_FAT32");
    ESP_LOGI(kTag, "Startup stage: FAT32_MOUNTED");
    std::string library =
        "sd-" + std::to_string(card.cid.mfg_id) + "-" + std::to_string(card.cid.serial);
    mounted_library = library;
    CatalogInPsram(true);
    auto result = boilon::local_library::Scan("/sdcard", library);
    CatalogInPsram(false);
    if (result.error == boilon::local_library::ScanError::kEmpty)
        EmptyLibrary(lcd, "NO_MUSIC");
    if (!result.Ok())
        MediaError(lcd, "CATALOG_SCAN", static_cast<int>(result.error));
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
}  // namespace

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
    ESP_LOGI(kTag,
             "Daily offline Beta; read-only FAT32, network/mic off, NVS resume=%d, "
             "shutdown by long press, low battery or 3 idle min",
             BOILON_M2_RESUME);
    ESP_LOGI(kTag, "Top short: pause/resume; middle/bottom hold 0.8s: previous/next");
    Check(gpio_input_enable(GPIO_NUM_3), "TOP_INPUT");
    for (auto pin : {GPIO_NUM_6, GPIO_NUM_7}) {
        Check(gpio_input_enable(pin), "BUTTON_INPUT");
        Check(gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY), "BUTTON_PULLUP");
    }
    static boilon::local_library::LibraryLcd lcd([](const char* reason) { PowerOff(reason); },
                                                 SetBacklight);
    lcd.Initialize();
    portENTER_CRITICAL(&power_lock);
    backlight_ready = true;
    portEXIT_CRITICAL(&power_lock);
    ShowPage(lcd, boilon::ui::Page::kChecking);
    ESP_LOGI(kTag, "Startup stage: LCD_READY");
    static boilon::local_library::BatteryAdc battery;
    battery.Initialize();
    auto catalog = MountLibrary(lcd);
    static boilon::local_player::LocalCodec codec([](const char* reason) { PowerOff(reason); },
                                                  SetAmplifier);
    static AudioService audio;
    if (!audio.InitializePlaybackOnly(&codec))
        PowerOff("AUDIO_INIT");
    audio.Start();
    static LocalMp3Player player(audio);
    boilon::local_library::ResumePort* resume = nullptr;
    boilon::local_library::ResumeWorker* persistence = nullptr;
#if BOILON_M2_RESUME
    CatalogInPsram(true);
    static boilon::local_library::ResumeWorker resume_worker(catalog, mounted_library, &battery);
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
        std::move(catalog), player, [] { return audio.IsPlaybackIdle(); }, observe, resume,
        [](int volume) { codec.SetOutputVolume(volume); });
    using boilon::local_player::kDacGainDb, boilon::local_player::kMaxVolume;
    ESP_LOGI(kTag, "Trace config rate=%d dac_db=%.1f volume=10 volume_max=%d step_db=%.1f",
             audio.LocalOutputSampleRate(), kDacGainDb, kMaxVolume,
             5 * boilon::local_player::kDbPerVolume);
    if (!runtime.Start(NowMs()))
        PowerOff("PLAY_START");
    bool finished = false;
    uint64_t last_progress = NowMs();
    uint64_t last_battery = 0;
    boilon::power::PowerWatch power;
    uint32_t battery_samples = 0;
    while (true) {
        owner_progress_ms = NowMs();
        if (RequestedShutdown())
            NormalShutdown(lcd, player, runtime, persistence);
        if (!runtime.Tick(Pressed(), NowMs()))
            MediaError(lcd, "PLAY_CONTROL");
        auto gesture = runtime.LastGesture();
        using Action = media::GestureAction;
        if (gesture != Action::kNone) {
            bool supported =
                gesture == Action::kTogglePlayback || gesture == Action::kNextTrack ||
                gesture == Action::kPreviousTrack || gesture == Action::kOpenDirectory ||
                gesture == Action::kBrowseNext || gesture == Action::kBrowsePrevious ||
                gesture == Action::kConfirmCollection || gesture == Action::kVolumeUp ||
                gesture == Action::kVolumeDown || gesture == Action::kOfflineHint;
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
            ESP_LOGI(kTag, "Collection finished; long-press top to power off or select another track");
        }
        finished = runtime.Finished();
        const auto now = NowMs();
        if (persistence && now - last_battery >= 5000) {
            last_battery = now;
            persistence->SampleBattery();
        }
        // M5 batch 5: like a long press when the battery runs out or after
        // three minutes with nothing to hear.
        std::optional<int> percent;  // A reading the worker finished since the last tick.
        if (persistence && persistence->BatterySamples() != battery_samples) {
            battery_samples = persistence->BatterySamples();
            percent = persistence->BatteryPercent();
        }
        const bool idle =
            !Pressed() && runtime.Controller().State() != media::PlaybackState::kPlaying;
        if (const auto off = power.Update(now, percent, idle, false);
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
            auto model = boilon::ui::PlayerModel(runtime.View(now), runtime.PositionMs(),
                                                 persistence ? persistence->BatteryPercent() : -1);
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
#endif
