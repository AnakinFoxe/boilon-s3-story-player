#ifndef BOILON_S3_GEN2_CONFIG_H_
#define BOILON_S3_GEN2_CONFIG_H_

#include <driver/gpio.h>

// Confirmed on the BOILON S3 second-generation device by a reversible
// RTCIO latch experiment. Do not substitute the Gen1/Jiuchuan GPIO5 mapping.
#define BOILON_S3_GEN2_POWER_ENABLE_GPIO GPIO_NUM_15

#endif  // BOILON_S3_GEN2_CONFIG_H_
