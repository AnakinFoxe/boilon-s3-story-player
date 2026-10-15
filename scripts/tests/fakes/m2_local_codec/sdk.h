#pragma once
#include <es7210_adc.h>
#include <es8311_codec.h>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

constexpr int I2C_NUM_0 = 0, GPIO_NUM_1 = 1, GPIO_NUM_2 = 2, I2C_CLK_SRC_DEFAULT = 0;
constexpr int I2S_NUM_0 = 0, I2S_ROLE_MASTER = 0, I2S_MCLK_MULTIPLE_256 = 256;
constexpr int I2S_DATA_BIT_WIDTH_16BIT = 16, I2S_SLOT_MODE_STEREO = 2, I2S_GPIO_UNUSED = -1;
constexpr int GPIO_NUM_38 = 38, GPIO_NUM_14 = 14, GPIO_NUM_13 = 13, GPIO_NUM_12 = 12;
constexpr int GPIO_NUM_11 = 11, ESP_OK = 0, I2S_SLOT_BIT_WIDTH_32BIT = 32;
#define ESP_LOGI(tag, ...) fake_codec::Log(__VA_ARGS__)
#define ESP_LOGW(tag, ...) fake_codec::Log(__VA_ARGS__)
#define ESP_LOGE(tag, ...) fake_codec::Log(__VA_ARGS__)
namespace fake_codec {
inline std::vector<std::string> events, logs;
inline bool adc_ack = true;
inline int16_t next_sample = 0;
// P5-3: the slot wired back from the DAC (-1: none). It hears the transmitted
// left channel at -6 dB, three 160-frame blocks after it was written.
inline int loopback_slot = -1;
inline std::vector<int16_t> sent;
inline int16_t Heard(size_t frame, size_t frames) {
    const long index = static_cast<long>(sent.size()) - 3 * 160 - static_cast<long>(frames) +
                       static_cast<long>(frame);
    return index >= 0 && index < static_cast<long>(sent.size()) ? sent[index] / 2 : 0;
}
template <typename... Args>
void Log(const char* format, Args...) {
    logs.emplace_back(format);
}
}  // namespace fake_codec
using i2c_master_bus_handle_t = void*;
using i2s_chan_handle_t = void*;
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
#define pdMS_TO_TICKS(ms) (ms)
inline void vTaskDelay(int) {}
struct i2c_master_bus_config_t {
    int i2c_port, sda_io_num, scl_io_num, clk_source, glitch_ignore_cnt;
    struct {
        bool enable_internal_pullup;
    } flags;
};
inline int i2c_new_master_bus(const i2c_master_bus_config_t*, void** bus) {
    *bus = reinterpret_cast<void*>(1);
    return 0;
}
inline int i2c_master_probe(void*, int addr, int) {
    assert(addr == 0x18 || addr == 0x41);
    fake_codec::events.push_back(addr == 0x18 ? "probe dac" : "probe adc");
    return addr == 0x18 || fake_codec::adc_ack ? 0 : 0x107;
}
struct i2s_chan_config_t {
    int dma_desc_num, dma_frame_num;
    bool auto_clear_after_cb;
};
#define I2S_CHANNEL_DEFAULT_CONFIG(port, role) ((void)(port), (void)(role), i2s_chan_config_t{})
struct i2s_std_config_t {
    struct {
        int mclk_multiple;
    } clk_cfg;
    struct {
        int data_bit_width, slot_mode, slot_bit_width, ws_width;
    } slot_cfg;
    struct {
        int mclk, bclk, ws, dout, din;
    } gpio_cfg;
};
#define I2S_STD_CLK_DEFAULT_CONFIG(rate) \
    decltype(i2s_std_config_t::clk_cfg) {}
#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(width, mode) \
    decltype(i2s_std_config_t::slot_cfg) { (width), (mode), 0, (width) }
enum i2s_tdm_slot_mask_t { I2S_TDM_SLOT0 = 1, I2S_TDM_SLOT1 = 2, I2S_TDM_SLOT2 = 4, I2S_TDM_SLOT3 = 8 };
struct i2s_tdm_config_t {
    struct {
        int sample_rate_hz, mclk_multiple;
    } clk_cfg;
    struct {
        int data_bit_width, slot_mode, slot_mask, total_slot;
    } slot_cfg;
    struct {
        int mclk, bclk, ws, dout, din;
    } gpio_cfg;
};
#define I2S_TDM_CLK_DEFAULT_CONFIG(rate) \
    decltype(i2s_tdm_config_t::clk_cfg) { (rate), 0 }
#define I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(width, mode, mask) \
    decltype(i2s_tdm_config_t::slot_cfg) { (width), (mode), (mask), 0 }
namespace fake_codec {
inline i2s_std_config_t tx_config;
inline i2s_tdm_config_t rx_config;
}  // namespace fake_codec
inline int i2s_new_channel(const i2s_chan_config_t*, void** tx, void** rx) {
    *tx = reinterpret_cast<void*>(2);
    if (rx)
        *rx = reinterpret_cast<void*>(3);
    fake_codec::events.push_back(rx ? "create duplex" : "create tx");
    return 0;
}
inline int i2s_channel_init_std_mode(void*, const i2s_std_config_t* config) {
    fake_codec::tx_config = *config;
    fake_codec::events.push_back("init tx");
    return 0;
}
inline int i2s_channel_init_tdm_mode(void* rx, const i2s_tdm_config_t* config) {
    assert(rx == reinterpret_cast<void*>(3));
    fake_codec::rx_config = *config;
    fake_codec::events.push_back("init rx");
    return 0;
}
inline int i2s_channel_enable(void* channel) {
    fake_codec::events.push_back(channel == reinterpret_cast<void*>(3) ? "enable rx" : "enable tx");
    return 0;
}
// Four 16-bit TDM slots per frame: slot 0 carries a ramp, the others noise.
inline int i2s_channel_read(void* rx, void* data, size_t bytes, size_t* received, int) {
    assert(rx == reinterpret_cast<void*>(3) && bytes % 8 == 0 && bytes <= 160 * 8);
    auto* pcm = static_cast<int16_t*>(data);
    for (size_t frame = 0; frame < bytes / 8; ++frame) {
        pcm[4 * frame] = fake_codec::next_sample++;
        pcm[4 * frame + 1] = pcm[4 * frame + 2] = pcm[4 * frame + 3] = -7;
        if (fake_codec::loopback_slot >= 0)
            pcm[4 * frame + fake_codec::loopback_slot] = fake_codec::Heard(frame, bytes / 8);
    }
    *received = bytes;
    return 0;
}
inline int i2s_channel_write(void*, const void* data, size_t bytes, size_t* written, int) {
    const auto* stereo = static_cast<const int16_t*>(data);
    for (size_t frame = 0; frame < bytes / 4; ++frame)
        fake_codec::sent.push_back(stereo[2 * frame]);
    *written = bytes;
    return 0;
}
struct audio_codec_i2c_cfg_t {
    int port, addr;
    void* bus_handle;
};
const audio_codec_ctrl_if_t* audio_codec_new_i2c_ctrl(const audio_codec_i2c_cfg_t*);
class AudioCodec {
public:
    virtual ~AudioCodec() = default;
    virtual void Start() = 0;
    virtual void SetOutputVolume(int) = 0;
    virtual void SetInputGain(float) = 0;
    virtual void EnableInput(bool) = 0;
    virtual void EnableOutput(bool) = 0;

    bool input_enabled() const { return input_enabled_; }
    bool duplex() const { return duplex_; }
    int input_channels() const { return input_channels_; }
    bool input_reference() const { return input_reference_; }
    int ReadForTest(int16_t* dest, int samples) { return Read(dest, samples); }

protected:
    virtual int Read(int16_t*, int) = 0;
    virtual int Write(const int16_t*, int) = 0;
    int input_sample_rate_ = 0, output_sample_rate_ = 0, output_volume_ = 0;
    bool output_enabled_ = false, input_enabled_ = false, duplex_ = false;
    bool input_reference_ = false;
    int input_channels_ = 1;
    i2s_chan_handle_t tx_handle_ = nullptr, rx_handle_ = nullptr;
};
