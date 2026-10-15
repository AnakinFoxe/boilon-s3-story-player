#pragma once

namespace boilon::local_library {
struct BatteryReading {
    int raw = -1;
    int adc_pad_mv = -1;  // ADC pad voltage, not battery voltage or percentage.
};
class BatterySource {
public:
    virtual ~BatterySource() = default;
    virtual BatteryReading Read() = 0;  // Existing storage worker only.
};
}  // namespace boilon::local_library
