#pragma once
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_log.h>
#include "m2_battery_source.h"

namespace boilon::local_library {
// Same GPIO4/ADC1_CH3 settings as the accepted passive probe; model converts pad mV.
class BatteryAdc final : public BatterySource {
public:
    void Initialize() {
        adc_unit_t unit;
        adc_channel_t channel;
        if (adc_oneshot_io_to_channel(4, &unit, &channel) != ESP_OK || unit != ADC_UNIT_1 ||
            channel != ADC_CHANNEL_3)
            return;
        adc_oneshot_unit_init_cfg_t config{};
        config.unit_id = ADC_UNIT_1;
        config.ulp_mode = ADC_ULP_MODE_DISABLE;
        if (adc_oneshot_new_unit(&config, &adc_) != ESP_OK)
            return;
        adc_oneshot_chan_cfg_t input{};
        input.atten = ADC_ATTEN_DB_12;
        input.bitwidth = ADC_BITWIDTH_12;
        if (adc_oneshot_config_channel(adc_, ADC_CHANNEL_3, &input) != ESP_OK) {
            adc_oneshot_del_unit(adc_);
            adc_ = nullptr;
            return;
        }
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        adc_cali_curve_fitting_config_t calibration{};
        calibration.unit_id = ADC_UNIT_1;
        calibration.chan = ADC_CHANNEL_3;
        calibration.atten = ADC_ATTEN_DB_12;
        calibration.bitwidth = ADC_BITWIDTH_12;
        if (adc_cali_create_scheme_curve_fitting(&calibration, &calibration_) != ESP_OK)
            calibration_ = nullptr;  // Raw remains useful; no fabricated pad voltage.
#endif
        available_ = true;
    }
    BatteryReading Read() override {
        BatteryReading result;
        if (!available_)
            return result;
        if (adc_oneshot_read(adc_, ADC_CHANNEL_3, &result.raw) != ESP_OK ||
            (calibration_ &&
             adc_cali_raw_to_voltage(calibration_, result.raw, &result.adc_pad_mv) != ESP_OK)) {
            available_ = false;
            return {};  // Disable optional sampling for this boot; no retry.
        }
        return result;
    }

private:
    adc_oneshot_unit_handle_t adc_ = nullptr;
    adc_cali_handle_t calibration_ = nullptr;
    bool available_ = false;
};
}  // namespace boilon::local_library
