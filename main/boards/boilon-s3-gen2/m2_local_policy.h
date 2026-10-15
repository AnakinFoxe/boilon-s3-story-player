#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace boilon::local_player {
// Volume 0 mutes; each step of 5 adds 2.5 dB up to 100, which plays PCM unscaled.
// kDacGainDb alone sets how loud 100 is: the cap of the jiuchuan-s3 reference
// board (its volume 80). Lowering it lowers every step; saved volumes stay valid.
constexpr int kMaxVolume = 100;
constexpr float kDacGainDb = -6.5f;
constexpr float kDbPerVolume = 0.5f;
// ES8311 DAC volume register as esp_codec_dev sets it: 0.5 dB steps from -95.5 dB.
constexpr int kDacVolumeRegister = static_cast<int>((kDacGainDb + 95.5f) * 2);
// Q15 gain per volume; 32768 is unity.
inline const std::array<int32_t, kMaxVolume + 1> kVolumeGains = [] {
    std::array<int32_t, kMaxVolume + 1> gains{};
    for (int volume = 1; volume <= kMaxVolume; ++volume)
        gains[volume] = static_cast<int32_t>(
            std::lround(32768 * std::pow(10.0, (volume - kMaxVolume) * kDbPerVolume / 20)));
    return gains;
}();
inline int16_t VolumeSample(int16_t sample, int volume) {
    return static_cast<int16_t>(static_cast<int32_t>(sample) *
                                kVolumeGains[std::clamp(volume, 0, kMaxVolume)] / 32768);
}
}  // namespace boilon::local_player
