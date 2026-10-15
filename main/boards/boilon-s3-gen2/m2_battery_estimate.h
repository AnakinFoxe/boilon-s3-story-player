#pragma once
#include <algorithm>
#include <limits>
#include "m2_battery_source.h"

namespace boilon::local_library {
// Factory 2.0.5 divider/curve; see docs/boilon/M2_BATTERY_BASIS.md.
// Voltage-based estimate only. No charging state is inferred from USB.
class BatteryEstimate {
public:
    static int Millivolts(const BatteryReading& reading) {
        if (reading.raw < 0 || reading.adc_pad_mv <= 0 ||
            reading.adc_pad_mv > std::numeric_limits<int>::max() / 3)
            return -1;
        return reading.adc_pad_mv * 3;  // Upper 200k / lower 100k.
    }
    static float Capacity(int millivolts) {
        struct Point {
            int mv, percent;
        };
        constexpr Point points[] = {{4200, 100}, {4060, 80}, {3820, 60}, {3580, 40},
                                    {3340, 20},  {3100, 0},  {3000, -10}};
        if (millivolts >= points[0].mv)
            return points[0].percent;
        for (size_t i = 1; i < sizeof(points) / sizeof(points[0]); ++i) {
            if (millivolts >= points[i].mv) {
                const auto& low = points[i];
                const auto& high = points[i - 1];
                return low.percent + static_cast<float>(millivolts - low.mv) *
                                         (high.percent - low.percent) / (high.mv - low.mv);
            }
        }
        return points[6].percent;
    }
    int Update(const BatteryReading& reading) {
        const int millivolts = Millivolts(reading);
        if (millivolts < 0) {
            known_ = false;
            return -1;
        }
        const float current = Capacity(millivolts);
        filtered_ = known_ ? 0.2f * current + 0.8f * filtered_ : current;
        known_ = true;
        // Factory negative low-voltage sentinel is never displayed as a percentage.
        return std::clamp(static_cast<int>(filtered_), 0, 100);
    }

private:
    float filtered_ = 0;
    bool known_ = false;
};
}  // namespace boilon::local_library
