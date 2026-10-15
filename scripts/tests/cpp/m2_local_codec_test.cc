#include <iostream>
#include <string>

namespace {
audio_codec_ctrl_if_t raw = {};
audio_codec_if_t dac = {};
const audio_codec_ctrl_if_t* wrapper = nullptr;
audio_codec_ctrl_info_t* expected_info = nullptr;
int info_result = 0, info_calls = 0, pa_enables = 0;
bool muted = true;
float dac_db = 0;  // What the codec asked the DAC for; the register follows it.
int RawInfo(const audio_codec_ctrl_if_t* control, audio_codec_ctrl_info_t* info) {
    assert(control == &raw && info == expected_info);
    ++info_calls;
    info->type = AUDIO_CODEC_CTRL_I2C;
    info->i2c.addr = ES8311_CODEC_DEFAULT_ADDR;
    info->i2c.port = I2C_NUM_0;
    return info_result;
}
void Fail(const char* reason) { throw std::string(reason); }
int Amplifier(bool enable) {
    pa_enables += enable;
    return 0;
}
}  // namespace

audio_codec_ctrl_if_t adc_control = {};
audio_codec_if_t adc = {};
float adc_gain = 0, adc_channel_gain = -1;
int adc_channel_mask = 0;
bool adc_muted = false, adc_powered = false;

const audio_codec_ctrl_if_t* audio_codec_new_i2c_ctrl(const audio_codec_i2c_cfg_t* config) {
    assert(config->port == I2C_NUM_0);
    if (config->addr == 0x82)  // ES7210 at 7-bit 0x41 (P1-5).
        return &adc_control;
    assert(config->addr == ES8311_CODEC_DEFAULT_ADDR);
    raw.is_open = [](const audio_codec_ctrl_if_t* control) {
        assert(control == &raw);
        return true;
    };
    raw.read_reg = [](const audio_codec_ctrl_if_t* control, int, int, void*, int) {
        assert(control == &raw);
        return 0;
    };
    raw.write_reg = raw.read_reg;
    raw.get_info = RawInfo;
    return &raw;
}

// Model the locked ES8311 constructor's required get_info gate using its real API headers.
const audio_codec_if_t* es8311_codec_new(es8311_codec_cfg_t* config) {
    wrapper = config->ctrl_if;
    assert(wrapper != &raw && wrapper->is_open(wrapper));
    if (!wrapper->get_info)
        return nullptr;
    audio_codec_ctrl_info_t info = {};
    expected_info = &info;
    if (wrapper->get_info(wrapper, &info) != ESP_CODEC_DEV_OK)
        return nullptr;
    assert(info.type == AUDIO_CODEC_CTRL_I2C);
    assert(info.i2c.addr == ES8311_CODEC_DEFAULT_ADDR && info.i2c.port == I2C_NUM_0);
    dac.set_fs = [](const audio_codec_if_t*, esp_codec_dev_sample_info_t*) { return 0; };
    dac.enable = [](const audio_codec_if_t*, bool) { return 0; };
    dac.mute = [](const audio_codec_if_t*, bool value) {
        muted = value;
        return 0;
    };
    dac.set_vol = [](const audio_codec_if_t*, float db) {
        dac_db = db;
        return 0;
    };
    dac.get_reg = [](const audio_codec_if_t*, int reg, int* value) {
        assert(reg == 0x31 || reg == 0x32);
        // esp_codec_dev ES8311: 0.5 dB per register step from -95.5 dB.
        *value = reg == 0x31 ? (muted ? 0x60 : 0) : static_cast<int>((dac_db + 95.5f) * 2);
        return 0;
    };
    return &dac;
}

const audio_codec_if_t* es7210_codec_new(es7210_codec_cfg_t* config) {
    assert(config->ctrl_if == &adc_control && !config->master_mode && config->mclk_div == 256);
    assert(config->mic_selected ==
           (ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4));
    adc.set_fs = [](const audio_codec_if_t*, esp_codec_dev_sample_info_t* format) {
        assert(format->channel == 4 && format->bits_per_sample == 16);
        assert(format->sample_rate == 16000 && format->mclk_multiple == 256);
        fake_codec::events.push_back("adc format");
        return 0;
    };
    adc.enable = [](const audio_codec_if_t*, bool enable) {
        assert(enable != adc_powered);
        adc_powered = enable;
        if (enable)
            adc_gain = 0;  // The vendor enable resets the gain.
        fake_codec::events.push_back(enable ? "adc enable" : "adc disable");
        return 0;
    };
    adc.set_mic_gain = [](const audio_codec_if_t*, float db) {
        adc_gain = db;
        return 0;
    };
    adc.set_mic_channel_gain = [](const audio_codec_if_t*, uint16_t mask, float db) {
        adc_channel_mask = mask;
        adc_channel_gain = db;
        return 0;
    };
    adc.mute_mic = [](const audio_codec_if_t*, bool mute) {
        adc_muted = mute;
        return 0;
    };
    return &adc;
}

// The AI board's codec: the ES7210 joins I2S0 as a full-duplex receiver.
void CheckMicrophone() {
    using boilon::local_player::LocalCodec;
    fake_codec::events.clear();
    LocalCodec codec(Fail, Amplifier, true);
    codec.Start();
    assert(codec.duplex() && codec.MicrophoneReady() && !codec.input_enabled());
    // Both frames are 64 BCLK: 16-bit samples in 32-bit transmit slots.
    assert(fake_codec::tx_config.slot_cfg.data_bit_width == 16);
    assert(fake_codec::tx_config.slot_cfg.slot_bit_width == 32);
    assert(fake_codec::tx_config.slot_cfg.ws_width == 32);
    const auto& rx = fake_codec::rx_config;
    assert(rx.slot_cfg.data_bit_width == 16 && rx.slot_cfg.total_slot == 4 &&
           rx.slot_cfg.slot_mask == 15 && rx.clk_cfg.sample_rate_hz == 16000);
    assert(rx.clk_cfg.mclk_multiple == 256 && rx.gpio_cfg.din == GPIO_NUM_11);
    assert(rx.gpio_cfg.bclk == GPIO_NUM_14 && rx.gpio_cfg.ws == GPIO_NUM_13);
    assert(rx.gpio_cfg.dout == I2S_GPIO_UNUSED);
    // The receiver is configured before the shared clock starts; the ADC after the DAC.
    // The ADC is configured once, powered for the echo check at 0 dB, then stays
    // powered down until a conversation. No slot follows the tone here.
    const std::vector<std::string> order = {"probe dac", "create duplex", "init tx", "init rx",
                                            "enable tx", "probe adc", "adc format",
                                            "adc enable", "adc disable", "enable rx",
                                            "adc enable", "adc disable"};
    assert(fake_codec::events == order);
    assert(adc_gain == 0.0f && adc_muted && !adc_powered);
    assert(!codec.ReferenceReady() && codec.input_channels() == 1 && !codec.input_reference());
    assert(fake_codec::logs.back().find("Echo check") != std::string::npos);
    int16_t pcm[400];
    assert(codec.ReadForTest(pcm, 400) == 0);  // Muted and not capturing.
    codec.EnableInput(true);  // Powered up with its gain set again.
    assert(codec.input_enabled() && !adc_muted && adc_powered && adc_gain == 30.0f);
    fake_codec::next_sample = 100;
    assert(codec.ReadForTest(pcm, 400) == 400);  // Slot 0 only, across 160-frame reads.
    for (int i = 0; i < 400; ++i)
        assert(pcm[i] == 100 + i);
    codec.EnableInput(false);
    assert(!codec.input_enabled() && adc_muted && !adc_powered);
    codec.EnableInput(true);  // A second conversation powers it up the same way.
    assert(codec.input_enabled() && !adc_muted && adc_powered && adc_gain == 30.0f);
    codec.EnableInput(false);
    assert(!codec.input_enabled() && adc_muted && !adc_powered);
    assert(fake_codec::logs.back().find("Input off frames=") != std::string::npos);

    // No ADC answer: playback starts as usual and the microphone stays off.
    fake_codec::events.clear();
    fake_codec::adc_ack = false;
    LocalCodec silent(Fail, Amplifier, true);
    silent.Start();
    assert(!silent.MicrophoneReady() && !silent.ReferenceReady());
    assert(fake_codec::logs.back().find("Microphone unavailable") != std::string::npos);
    silent.EnableInput(true);
    assert(!silent.input_enabled() && silent.ReadForTest(pcm, 160) == 0);
    fake_codec::adc_ack = true;
}

// P5-3: a slot wired back from the DAC follows the boot check's tone. It
// becomes the echo reference: capture turns into [voice, reference] pairs and
// its MIC input gets its own gain after the voice's 30 dB.
void CheckReference(int slot) {
    using boilon::local_player::LocalCodec;
    fake_codec::loopback_slot = slot;
    adc_channel_mask = 0;
    LocalCodec codec(Fail, Amplifier, true);
    codec.Start();
    assert(codec.MicrophoneReady() && codec.ReferenceReady());
    assert(codec.input_channels() == 2 && codec.input_reference());
    codec.EnableInput(true);
    // Slot 1 is MIC3 and slot 3 is MIC4; -26 dBFS at 0 dB needs no more gain.
    assert(adc_gain == 30.0f && adc_channel_mask == (slot == 1 ? 4 : 8));
    assert(adc_channel_gain == 0.0f && !adc_muted && adc_powered);
    int16_t pcm[400];
    fake_codec::next_sample = 100;
    assert(codec.ReadForTest(pcm, 400) == 400);  // 200 pairs across 160-frame reads.
    for (int i = 0; i < 200; ++i)
        assert(pcm[2 * i] == 100 + i);
    codec.EnableInput(false);
    assert(fake_codec::logs.back().find("ref_rms=") != std::string::npos);
    fake_codec::loopback_slot = -1;
}

int main() {
    using boilon::local_player::LocalCodec;
    try {
        LocalCodec codec(Fail, Amplifier);
        codec.Start();
        assert(info_calls == 1 && pa_enables == 1);
        // The jiuchuan-s3 cap: -6.5 dB, register 0xB2, read back before the PA turns on.
        assert(dac_db == -6.5f && boilon::local_player::kDacVolumeRegister == 0xB2);
        audio_codec_ctrl_info_t info = {};
        expected_info = &info;
        info_result = ESP_CODEC_DEV_INVALID_ARG;
        assert(wrapper->get_info(wrapper, &info) == info_result);
        assert(info_calls == 2 && info.type == AUDIO_CODEC_CTRL_I2C);
        assert(info.i2c.addr == ES8311_CODEC_DEFAULT_ADDR && info.i2c.port == I2C_NUM_0);
    } catch (const std::string& reason) {
        std::cerr << "Startup rejected: " << reason << '\n';
        return 1;
    }
    info_calls = pa_enables = 0;
    LocalCodec failing(Fail, Amplifier);
    bool stopped = false;
    try {
        failing.Start();
    } catch (const std::string& reason) {
        assert(reason == "DAC_CREATE");
        stopped = true;
    }
    assert(stopped && info_calls == 1 && pa_enables == 0);
    info_result = 0;
    fake_codec::events.clear();
    LocalCodec playback(Fail, Amplifier);  // The library codec never touches the ADC.
    playback.Start();
    assert(!playback.duplex() && !playback.MicrophoneReady());
    // The keys and XiaoZhi's set_volume land in one value, clamped to the scale.
    assert(playback.OutputVolume() == 10);
    playback.SetOutputVolume(35);
    assert(playback.OutputVolume() == 35);
    playback.SetOutputVolume(150);
    assert(playback.OutputVolume() == 100);
    playback.SetOutputVolume(-5);
    assert(playback.OutputVolume() == 0);
    assert(fake_codec::tx_config.slot_cfg.slot_bit_width == 0);
    assert(fake_codec::events ==
           (std::vector<std::string>{"probe dac", "create tx", "init tx", "enable tx"}));
    CheckMicrophone();
    CheckReference(1);
    CheckReference(3);
    std::cout << "Actual LocalCodec startup, control identity/info, and failure gate passed\n";
}
