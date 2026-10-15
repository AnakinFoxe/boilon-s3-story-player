"""Run production startup/manual shutdown functions with host-only effects."""
import unittest
from scripts.tests import boilon_cpp_helpers as helpers


class LibraryStartupTests(unittest.TestCase):
    compile_and_run = helpers.compile_and_run

    def test_failure_report_is_bounded_and_user_poweroff_is_not_a_failure(self):
        source = (helpers.BOARD / "m2_library_main.cc").read_text(encoding="utf-8")
        functions = "\n".join(helpers.function(source, signature) for signature in (
            "void ReportFailure(", "[[noreturn]] void PowerOff(", "void Check("))
        self.compile_and_run(r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
struct Stopped {};
int power_lock = 0;
bool shutting_down = false, writable = true, pa_fail = false;
constexpr int GPIO_NUM_42 = 42, BOILON_S3_GEN2_POWER_ENABLE_GPIO = 15, ESP_OK = 0;
constexpr char kTag[] = "BoilonLibrary";
int64_t now = 0;
std::string wire;
std::vector<int> events;
#define portENTER_CRITICAL(p) static_cast<void>(p)
#define portEXIT_CRITICAL(p) static_cast<void>(p)
#define pdMS_TO_TICKS(x) (x)
#define ESP_LOGI(...) static_cast<void>(kTag)
int gpio_set_level(int pin, int level) {
    assert(pin == 42 && level == 0); events.push_back(42); return pa_fail ? -1 : 0;
}
int rtc_gpio_set_level(int pin, int level) {
    assert(pin == 15 && level == 0); events.push_back(15); return 0;
}
int64_t esp_timer_get_time() { now += 1000; return now; }
unsigned esp_log_timestamp() { return 10096; }
void usb_serial_jtag_ll_txfifo_flush() {}
void StopBacklight() {}
bool usb_serial_jtag_ll_txfifo_writable() { return writable; }
size_t usb_serial_jtag_ll_write_txfifo(const uint8_t* data, size_t len) {
    assert(len <= 64); assert(events.back() == 42);
    wire.append(reinterpret_cast<const char*>(data), len); return len;
}
void vTaskDelay(int ticks) { assert(ticks == 1000); throw Stopped{}; }
''' + functions + r'''
int main() {
    for (int scenario = 0; scenario < 4; ++scenario) {
        events.clear(); wire.clear(); now = 0; shutting_down = false;
        writable = scenario != 1; pa_fail = scenario == 3;
        try {
            if (scenario == 2) PowerOff("USER");
            else Check(261, "SD_CARD");
        } catch (const Stopped&) {}
        assert(shutting_down && (events == std::vector<int>{42, 15}));
        assert(now <= 101000);
        if (scenario == 0) {
            assert(wire.find("reason=SD_CARD code=261") != std::string::npos);
        } else assert(wire.empty());
        if (scenario >= 2) assert(now == 0);
    }
}
''')

    def test_sd_stage_logs_preserve_latch_without_startup_wait_or_normal_deadline(self):
        source = (helpers.BOARD / "m2_library_main.cc").read_text(encoding="utf-8")
        main = helpers.function(source, 'extern "C" void app_main(')
        self.assertIn('MediaError(lcd, "CATALOG_SCAN", static_cast<int>(result.error))', source)
        self.assertIn('Startup stage: SD_HOST', source)
        self.assertIn('Startup stage: SD_CARD_READY', source)
        self.assertIn('Startup stage: FAT32_MOUNTED', source)
        for old in ('vTaskDelay(pdMS_TO_TICKS(180000))',
                    'vTaskDelay(pdMS_TO_TICKS(10000))', 'NowMs() >= 175000',
                    'PowerOff("DEADLINE")'):
            self.assertNotIn(old, main)
        self.assertIn('NowMs() - owner_progress_ms.load() >= 30000', main)
        self.assertIn('PowerOff("OWNER_TIMEOUT")', main)
        self.assertLess(main.index('NormalShutdown(lcd, player, runtime, persistence)'),
                        main.index('runtime.Tick(Pressed(), NowMs())'))
        self.assertIn('RTC_GPIO_MODE_OUTPUT_ONLY', source)

    def test_media_error_turns_pa_off_before_bounded_lcd_and_preserves_error_poweroff(self):
        source = (helpers.BOARD / "m2_library_main.cc").read_text(encoding="utf-8")
        functions = "\n".join(helpers.function(source, signature) for signature in (
            "void Check(", "[[noreturn]] void MediaError("))
        self.compile_and_run(r'''
#include "p5_1_ui_screens.h"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
struct Stopped {};
int64_t now_us = 0;
bool pa_off = false, pa_fail = false, ready = true;
int power_code = 0;
std::string power_reason;
std::vector<std::string> events;
#define pdMS_TO_TICKS(value) (value)
int64_t esp_timer_get_time() { return now_us; }
int SetAmplifier(bool enable) {
    assert(!enable); events.push_back("PA_OFF"); pa_off = !pa_fail;
    return pa_fail ? -7 : 0;
}
[[noreturn]] void PowerOff(const char* reason, int code = 0) {
    events.push_back("POWER_OFF"); power_reason = reason; power_code = code;
    throw Stopped{};
}
void vTaskDelay(int ticks) {
    assert(ticks == 10); now_us += int64_t(ticks) * 1000;
}
namespace boilon::local_library {
class LibraryLcd {
public:
    int polls = 0, begins = 0;
    void Poll(uint64_t now) {
        assert(pa_off && events.front() == "PA_OFF");
        assert(now == uint64_t(now_us / 1000)); ++polls; events.push_back("POLL");
    }
    bool Ready(uint64_t now) {
        assert(pa_off && polls > 0 && now == uint64_t(now_us / 1000)); return ready;
    }
    void Show(const boilon::ui::Model& model, uint64_t now) {
        assert(pa_off && polls > 0 && now == uint64_t(now_us / 1000));
        assert(model.page == boilon::ui::Page::kCardError);
        ++begins; events.push_back("SHOW");
    }
};
}
''' + functions + r'''
int main() {
    for (int scenario = 0; scenario < 3; ++scenario) {
        now_us = 190000000;  // The retired three-minute timer must not own this path.
        const auto started = now_us;
        events.clear(); power_reason.clear(); power_code = 0;
        pa_off = false; pa_fail = scenario == 2; ready = scenario != 1;
        boilon::local_library::LibraryLcd lcd;
        bool stopped = false;
        try { MediaError(lcd, "SD_CARD", 261); } catch (const Stopped&) { stopped = true; }
        assert(stopped && events.front() == "PA_OFF" && events.back() == "POWER_OFF");
        assert(now_us - started <= 3000000);
        if (scenario == 2) {
            assert(power_reason == "PA_OFF" && power_code == -7);
            assert(lcd.polls == 0 && lcd.begins == 0 && now_us == started);
        } else {
            assert(lcd.polls > 0 && lcd.polls <= 300);
            assert(lcd.begins == (ready ? lcd.polls : 0));
            assert(power_reason == "SD_CARD" && power_code == 261);
            assert(now_us - started == 3000000);
        }
    }
    // A hardware-stage Check failure does not attempt media-error/LCD handling.
    events.clear(); power_reason.clear(); power_code = 0; now_us = 0;
    try { Check(17, "SD_SLOT"); } catch (const Stopped&) {}
    assert(events == std::vector<std::string>{"POWER_OFF"});
    assert(power_reason == "SD_SLOT" && power_code == 17 && now_us == 0);
}
''')

    def test_lcd_precedes_mount_and_only_expected_media_errors_use_error_page(self):
        source = (helpers.BOARD / "m2_library_main.cc").read_text(encoding="utf-8")
        main = helpers.function(source, 'extern "C" void app_main(')
        self.assertLess(main.index("lcd.Initialize()"), main.index("MountLibrary(lcd)"))
        self.assertLess(main.index("backlight_ready = true"), main.index("MountLibrary(lcd)"))
        self.assertLess(main.index("backlight_ready = true"), main.index("ShowPage(lcd, boilon::ui::Page::kChecking)"))
        self.assertLess(main.index("ShowPage(lcd, boilon::ui::Page::kChecking)"), main.index("Startup stage: LCD_READY"))
        self.assertLess(main.index("Startup stage: LCD_READY"), main.index("MountLibrary(lcd)"))
        for later in ("static AudioService audio", "static LocalMp3Player player", "ResumeWorker resume_worker"):
            self.assertLess(main.index("MountLibrary(lcd)"), main.index(later))
        self.assertIn('MediaError(lcd, "PLAY_CONTROL")', main)
        mount = helpers.function(source, "media::MediaCatalog MountLibrary(")
        self.assertIn('Check(host.init(), "SD_HOST")', mount)
        self.assertIn('Check(sdmmc_host_init_slot(host.slot, &slot), "SD_SLOT")', mount)
        self.assertIn('EmptyLibrary(lcd, "SD_UNAVAILABLE", card_result)', mount)
        self.assertIn('if (result.error == boilon::local_library::ScanError::kEmpty)', mount)
        self.assertIn('EmptyLibrary(lcd, "NO_MUSIC")', mount)
        self.assertLess(mount.index('EmptyLibrary(lcd, "NO_MUSIC")'), mount.index("if (!result.Ok())"))
        self.assertIn("return std::move(result.catalog)", mount)
        for reason in ("SD_MOUNT", "SD_NOT_FAT32", "CATALOG_SCAN"):
            self.assertIn(f'MediaError(lcd, "{reason}"', mount)
        error = helpers.function(source, "[[noreturn]] void MediaError(")
        for forbidden in ("runtime.", "Checkpoint", "Shutdown", "xTaskCreate", "nvs_", "MountLibrary"):
            self.assertNotIn(forbidden, error)

    def test_actual_empty_idle_drains_boot_key_then_long_press_shuts_down(self):
        source = (helpers.BOARD / "m2_library_main.cc").read_text(encoding="utf-8")
        functions = "\n".join(helpers.function(source, signature) for signature in (
            "void Check(", "void ShowPage(", "uint8_t Pressed() {",
            "bool RequestedShutdown(", "[[noreturn]] void EmptyLibrary("))
        empty = helpers.function(source, "[[noreturn]] void EmptyLibrary(")
        for forbidden in ("AudioService", "LocalMp3Player", "ResumeWorker", "Checkpoint",
                          "xTaskCreate", "nvs_", "Scan(", "MountLibrary("):
            self.assertNotIn(forbidden, empty)
        self.assertIn('if (RequestedShutdown())', empty)
        self.assertIn('PowerOff("USER")', empty)
        self.assertEqual(empty.count("ShowPage("), 1)
        gesture_header = (helpers.ROOT / "main/media/player_gestures.h").read_text(encoding="utf-8")
        gesture_source = (helpers.ROOT / "main/media/player_gestures.cc").read_text(encoding="utf-8")
        gesture_source = gesture_source.replace('#include "media/player_gestures.h"', '')
        self.compile_and_run(r'''
#include "m5_power_policy.h"
#include "m6_release.h"
#include "p5_1_ui_screens.h"
#include <cassert>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
''' + gesture_header + gesture_source + r'''
struct Stopped {};
constexpr char kTag[] = "BoilonLibrary";
int64_t now_us = 0;
std::atomic<uint64_t> owner_progress_ms{0};
uint64_t idle_started = 0;
constexpr int GPIO_NUM_3 = 3, GPIO_NUM_6 = 6, GPIO_NUM_7 = 7;
bool pa_off = true, pa_fail = false, page_ready = false;
int power_code = 0, idle_code = 0;
std::string power_reason, idle_state;
std::vector<std::string> events;
#define pdMS_TO_TICKS(value) (value)
int64_t esp_timer_get_time() { return now_us; }
uint64_t NowMs() { return now_us / 1000; }
int gpio_get_level(int pin) {
    if (pin != GPIO_NUM_3) {
        assert(pin == GPIO_NUM_6 || pin == GPIO_NUM_7); return 1;
    }
    const uint64_t elapsed = NowMs() - idle_started;
    // Boot hold must drain; short press must not power off; a new 2s hold does.
    return elapsed < 2500 || (elapsed >= 2600 && elapsed < 2700) || elapsed >= 3400;
}
int SetAmplifier(bool enable) {
    assert(!enable); events.push_back("PA_OFF"); pa_off = !pa_fail;
    return pa_fail ? -7 : 0;
}
[[noreturn]] void PowerOff(const char* reason, int code = 0) {
    events.push_back("POWER_OFF"); power_reason = reason; power_code = code;
    throw Stopped{};
}
void vTaskDelay(int ticks) {
    assert(ticks == 10); now_us += int64_t(ticks) * 1000;
    assert(!idle_started || NowMs() - idle_started < 6000);
}
void RecordLog(const char* tag, const char* format, const char* state, int code) {
    assert(std::strcmp(tag, kTag) == 0 && std::strcmp(format, "Media idle state=%s code=%d") == 0);
    assert(pa_off && page_ready); idle_state = state; idle_code = code;
    idle_started = NowMs(); events.push_back("MEDIA_IDLE");
}
#define ESP_LOGI(tag, ...) RecordLog(tag, __VA_ARGS__)
namespace boilon::local_library {
class LibraryLcd {
public:
    int begins = 0, polls = 0;
    bool complete = false;
    int64_t ready_at = 0;
    boilon::ui::Page page = boilon::ui::Page::kPlayer;
    void Show(const boilon::ui::Model& model, uint64_t now) {
        assert(pa_off && now == NowMs() && begins == 0);
        page = model.page; ++begins; page_ready = complete = false; events.push_back("SHOW");
    }
    void Poll(uint64_t now) {
        assert(pa_off && begins == 1 && now == uint64_t(now_us / 1000)); ++polls;
        if (polls == 3) { complete = true; ready_at = now_us + 100000; }
    }
    bool Ready(uint64_t now) {
        assert(now == uint64_t(now_us / 1000));
        page_ready = complete && now_us >= ready_at;
        return page_ready;
    }
};
}
''' + functions + r'''
int main() {
    {
        now_us = 10000000; events.clear(); page_ready = false;
        boilon::local_library::LibraryLcd lcd;
        ShowPage(lcd, boilon::ui::Page::kChecking);
        assert(lcd.begins == 1 && lcd.polls >= 3 && lcd.complete && lcd.Ready(now_us / 1000));
        assert(now_us > 10000000 && now_us < 11000000);
        assert(lcd.page == boilon::ui::Page::kChecking);
        // app_main's source-order assertions below bind the real LCD_READY and SD_HOST flow.
        events.push_back("LCD_READY"); events.push_back("SD_HOST");
        assert(page_ready && events.front() == "SHOW");
    }
    for (int scenario = 0; scenario < 3; ++scenario) {
        now_us = 190000000 + scenario * 10000000;
        const auto started = now_us;
        idle_started = 0;
        events.clear(); idle_state.clear(); power_reason.clear(); idle_code = power_code = 0;
        pa_off = page_ready = false; pa_fail = scenario == 2;
        const char* state = scenario == 1 ? "NO_MUSIC" : "SD_UNAVAILABLE";
        const int code = scenario == 1 ? 0 : 261;
        boilon::local_library::LibraryLcd lcd;
        bool stopped = false;
        try { EmptyLibrary(lcd, state, code); } catch (const Stopped&) { stopped = true; }
        assert(stopped && events.front() == "PA_OFF" && events.back() == "POWER_OFF");
        if (scenario == 2) {
            assert(power_reason == "PA_OFF" && power_code == -7);
            assert(lcd.begins == 0 && lcd.polls == 0 && idle_state.empty() && now_us == started);
        } else {
            assert(power_reason == "USER" && power_code == 0 && now_us > 180000000);
            assert(NowMs() - idle_started == 5430);
            assert(owner_progress_ms.load() == NowMs());
            assert(lcd.begins == 1 && lcd.polls > 0);
            assert(idle_state == state && idle_code == code && lcd.polls > 300);
            assert(lcd.page == (scenario == 1 ? boilon::ui::Page::kEmpty : boilon::ui::Page::kNoCard));
        }
    }
}
''')

    def test_actual_normal_shutdown_snapshots_then_drains_or_times_out(self):
        source = (helpers.BOARD / "m2_library_main.cc").read_text(encoding="utf-8")
        shutdown = helpers.function(source, "[[noreturn]] void NormalShutdown(")
        for forbidden in ("runtime.Tick", "runtime.Start", "nvs_", "xTaskCreate", "ShowPage("):
            self.assertNotIn(forbidden, shutdown)
        self.compile_and_run(r'''
#include "p5_1_ui_screens.h"
#include <cassert>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
struct Stopped {};
constexpr char kTag[] = "BoilonLibrary";
std::atomic<uint64_t> owner_progress_ms{0};
uint64_t now_ms = 0, started_ms = 0;
bool queued = true, reported_saved = false;
unsigned reported_pending = 0;
std::vector<std::string> events;
uint64_t NowMs() { return now_ms; }
#define pdMS_TO_TICKS(value) (value)
void vTaskDelay(int ticks) {
    assert(ticks == 10); now_ms += ticks;
    assert(now_ms - started_ms <= 15000);
}
void RecordLog(const char* tag, const char* format, bool saved, unsigned pending) {
    assert(std::strcmp(tag, kTag) == 0);
    assert(std::strcmp(format, "Normal shutdown saved=%d pending=%u") == 0);
    reported_saved = saved; reported_pending = pending; events.push_back("LOG");
}
#define ESP_LOGI(tag, ...) RecordLog(tag, __VA_ARGS__)
[[noreturn]] void PowerOff(const char* reason) {
    assert(std::strcmp(reason, "USER") == 0); events.push_back("OFF"); throw Stopped{};
}
class LocalMp3Player {
public:
    bool initially_running = true, stopped = false, blocked = false;
    uint64_t position = 21530, stop_done_at = 20;
    bool Running() const { return stopped ? blocked || NowMs() < stop_done_at : initially_running; }
    bool Pause(bool pause) {
        assert(pause && !stopped && initially_running); events.push_back("PAUSE");
        position = 0; return true;
    }
    void Stop() { events.push_back("STOP"); stopped = true; position = 0; }
};
namespace media {
enum class SaveStatus { kSaved, kUnchanged, kStorageError };
}
namespace boilon::local_library {
class LibraryLcd {
public:
    bool never_ready = false, begun = false;
    unsigned polls = 0, begins = 0;
    uint64_t ready_at = 0;
    void Poll(uint64_t now) { assert(now == NowMs()); ++polls; }
    bool Ready(uint64_t now) const { assert(now == NowMs()); return !never_ready && now >= ready_at; }
    boilon::ui::Page shown = boilon::ui::Page::kPlayer;
    void Show(const boilon::ui::Model& model, uint64_t now) {
        assert(now == NowMs() && !begun);
        shown = model.page; begun = true; ++begins; ready_at = NowMs() + 40;
    }
};
class ResumeWorker {
public:
    bool blocked = false;
    uint64_t drain_at = 30;
    media::SaveStatus status = media::SaveStatus::kSaved;
    unsigned PendingJobs() const { return blocked || NowMs() < drain_at ? 1 : 0; }
    media::SaveStatus LastSaveStatus() const { assert(PendingJobs() == 0); return status; }
    int BatteryPercent() const { return 42; }
};
template <typename Player> class Runtime {
public:
    explicit Runtime(Player& player) : player_(player) {}
    uint64_t snapshot = 0;
    unsigned shutdowns = 0;
    bool Shutdown(uint64_t now) {
        assert(now == NowMs() && events.empty());
        snapshot = player_.position; ++shutdowns; events.push_back("SNAPSHOT"); return queued;
    }
private:
    Player& player_;
};
}
''' + shutdown + r'''
int main() {
    for (int scenario = 0; scenario < 8; ++scenario) {
        now_ms = started_ms = 190000; events.clear(); owner_progress_ms = 0;
        queued = scenario != 3 && scenario != 7;
        reported_saved = false; reported_pending = 0;
        LocalMp3Player player;
        player.stop_done_at += now_ms;
        player.initially_running = scenario != 1;
        player.blocked = scenario == 5;
        boilon::local_library::LibraryLcd lcd;
        lcd.never_ready = scenario == 6;
        boilon::local_library::ResumeWorker resume;
        resume.drain_at += now_ms;
        resume.blocked = scenario == 4 || scenario == 6;
        if (scenario == 1) resume.status = media::SaveStatus::kUnchanged;
        if (scenario == 2) resume.status = media::SaveStatus::kStorageError;
        boilon::local_library::Runtime<LocalMp3Player> runtime(player);
        bool stopped = false;
        try { NormalShutdown(lcd, player, runtime, scenario == 7 ? nullptr : &resume); }
        catch (const Stopped&) { stopped = true; }
        assert(stopped && runtime.shutdowns == 1 && runtime.snapshot == 21530);
        const std::vector<std::string> expected = player.initially_running
            ? std::vector<std::string>{"SNAPSHOT", "PAUSE", "STOP", "LOG", "OFF"}
            : std::vector<std::string>{"SNAPSHOT", "STOP", "LOG", "OFF"};
        assert(events == expected && player.stopped);
        assert(lcd.polls > 0 && lcd.begins == (lcd.never_ready ? 0 : 1));
        assert(now_ms - started_ms <= 15000 && owner_progress_ms.load() <= now_ms);
        if (scenario == 4 || scenario == 5 || scenario == 6) {
            assert(now_ms - started_ms == 15000);
        } else {
            assert(now_ms - started_ms < 100);
        }
        if (scenario == 0 || scenario == 1) assert(reported_saved && reported_pending == 0);
        if (scenario == 2 || scenario == 3 || scenario == 7) assert(!reported_saved);
        if (scenario == 4 || scenario == 6) assert(!reported_saved && reported_pending == 1);
        assert(lcd.never_ready || lcd.shown == boilon::ui::Page::kGoodbye);
    }
    // M5 batch 5: the low-battery page stays about 3 s even when saving is quick.
    now_ms = started_ms = 190000; events.clear(); queued = true;
    LocalMp3Player player;
    player.stop_done_at += now_ms;
    boilon::local_library::LibraryLcd lcd;
    boilon::local_library::ResumeWorker resume;
    resume.drain_at += now_ms;
    boilon::local_library::Runtime<LocalMp3Player> runtime(player);
    bool stopped = false;
    try { NormalShutdown(lcd, player, runtime, &resume, boilon::ui::Page::kLowBattery); }
    catch (const Stopped&) { stopped = true; }
    assert(stopped && lcd.shown == boilon::ui::Page::kLowBattery && reported_saved);
    assert(now_ms - started_ms >= 3000 && now_ms - started_ms < 3100);
}
''')


if __name__ == "__main__":
    unittest.main()
