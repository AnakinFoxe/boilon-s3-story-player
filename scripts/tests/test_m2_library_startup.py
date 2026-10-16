"""Run production startup/manual shutdown functions with host-only effects."""
import unittest
from scripts.tests import boilon_cpp_helpers as helpers

# The offline-only entry these tests first ran against was retired after v1.4.0;
# the AI entry has the same functions.
ENTRY = helpers.BOARD / "m3_ai_main.cc"


class LibraryStartupTests(unittest.TestCase):
    compile_and_run = helpers.compile_and_run

    def test_failure_report_is_bounded_and_user_poweroff_is_not_a_failure(self):
        source = ENTRY.read_text(encoding="utf-8")
        functions = "\n".join(helpers.function(source, signature) for signature in (
            "void ReportFailure(", "[[noreturn]] void PowerOff(const char* reason, int code) {",
            "void Check("))
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
[[noreturn]] void PowerOff(const char* reason, int code = 0);
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

    def test_first_page_completes_before_show_page_returns(self):
        source = ENTRY.read_text(encoding="utf-8")
        self.compile_and_run(r'''
#include "m6_release.h"
#include "p5_1_ui_screens.h"
#include <atomic>
#include <cassert>
#include <cstdint>
int64_t now_us = 0;
std::atomic<uint64_t> owner_progress_ms{0};
#define pdMS_TO_TICKS(value) (value)
int64_t esp_timer_get_time() { return now_us; }
uint64_t NowMs() { return now_us / 1000; }
void vTaskDelay(int ticks) {
    assert(ticks == 10); now_us += int64_t(ticks) * 1000;
}
namespace boilon::local_library {
class LibraryLcd {
public:
    int begins = 0, polls = 0;
    bool complete = false;
    int64_t ready_at = 0;
    boilon::ui::Page page = boilon::ui::Page::kPlayer;
    void Show(const boilon::ui::Model& model, uint64_t now) {
        assert(now == NowMs() && begins == 0);
        page = model.page; ++begins; complete = false;
    }
    void Poll(uint64_t now) {
        assert(begins == 1 && now == uint64_t(now_us / 1000)); ++polls;
        if (polls == 3) { complete = true; ready_at = now_us + 100000; }
    }
    bool Ready(uint64_t now) {
        assert(now == uint64_t(now_us / 1000));
        return complete && now_us >= ready_at;
    }
};
}
''' + helpers.function(source, "void ShowPage(") + r'''
int main() {
    now_us = 10000000;
    boilon::local_library::LibraryLcd lcd;
    ShowPage(lcd, boilon::ui::Page::kChecking);
    assert(lcd.begins == 1 && lcd.polls >= 3 && lcd.complete && lcd.Ready(now_us / 1000));
    assert(now_us > 10000000 && now_us < 11000000);
    assert(lcd.page == boilon::ui::Page::kChecking);
    // The owner watchdog stays fed while the page draws.
    assert(owner_progress_ms.load() + 10 == NowMs());
}
''')

    def test_actual_normal_shutdown_snapshots_then_drains_or_times_out(self):
        source = ENTRY.read_text(encoding="utf-8")
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
