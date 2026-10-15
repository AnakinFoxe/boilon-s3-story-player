#pragma once
#include <cstdint>
#include <optional>

// M5 batch 5: when the player powers itself off, like a long top press. No
// ESP-IDF headers, so test_m5_power_policy runs this exact code.
namespace boilon::power {
constexpr int kWarnPercent = 10, kOffPercent = 3, kOffReadings = 3;
constexpr uint64_t kWarningMs = 5000;        // How long the 10% notice shows.
constexpr uint64_t kIdleMs = 3 * 60 * 1000;  // Nothing to hear and no key.
constexpr uint64_t kUpgradeMs = 60 * 1000;   // Upstream stuck in its upgrade state.

// How long a condition has held without a break.
class Hold {
public:
    explicit Hold(uint64_t limit_ms) : limit_ms_(limit_ms) {}
    bool Update(bool condition, uint64_t now_ms) {
        if (!condition)
            since_.reset();
        else if (!since_)
            since_ = now_ms;
        return since_ && now_ms - *since_ >= limit_ms_;
    }

private:
    uint64_t limit_ms_;
    std::optional<uint64_t> since_;
};

class PowerWatch {
public:
    enum class Off { kNone, kLowBattery, kIdle, kUpgrade };
    static constexpr const char* kOffNames[] = {"none", "low_battery", "idle", "upgrade"};
    // Every loop tick. reading: the battery estimate (0-100, -1 unknown) when a
    // new one arrived; idle: nothing plays, no conversation or network page,
    // no key; upgrading: upstream is in its upgrade state.
    Off Update(uint64_t now_ms, std::optional<int> reading, bool idle, bool upgrading) {
        if (reading) {
            const bool known = *reading >= 0;
            low_readings_ = known && *reading <= kOffPercent ? low_readings_ + 1 : 0;
            if (known && *reading <= kWarnPercent && !warned_)
                warned_ = warning_due_ = true;
        }
        if (low_readings_ >= kOffReadings)
            return Off::kLowBattery;
        if (upgrade_.Update(upgrading, now_ms))
            return Off::kUpgrade;
        return idle_.Update(idle, now_ms) ? Off::kIdle : Off::kNone;
    }
    // Whether the 10% notice shows now: once a boot, for kWarningMs, starting
    // when a page that can show it (can_show) is up.
    bool ShowWarning(bool can_show, uint64_t now_ms) {
        if (warning_due_ && can_show) {
            warning_due_ = false;
            warning_until_ = now_ms + kWarningMs;
        }
        return can_show && warning_until_ && now_ms < *warning_until_;
    }

private:
    int low_readings_ = 0;
    bool warned_ = false, warning_due_ = false;
    std::optional<uint64_t> warning_until_;
    Hold idle_{kIdleMs}, upgrade_{kUpgradeMs};
};
}  // namespace boilon::power
