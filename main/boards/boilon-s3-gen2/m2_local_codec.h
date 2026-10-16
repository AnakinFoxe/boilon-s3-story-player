#pragma once

#include <driver/i2c_master.h>
#include <driver/i2s_tdm.h>
#include <es7210_adc.h>
#include <esp_codec_dev_defaults.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <new>
#include "audio/audio_codec.h"
#include "m2_local_policy.h"
#include "m3_ai_echo.h"
#include "sdkconfig.h"

namespace boilon::local_player {
// Owned by the standalone app. Lifetime is the entire powered-on session.
// With `microphone`, the ES7210 (P1-5: 7-bit 0x41, voice in TDM slot 0) shares
// I2S0 as a full-duplex receiver; a microphone fault leaves playback untouched.
// When the boot self-check finds the slot wired back from the DAC, input is
// [voice, reference] pairs for the echo canceller (P5-3).
class LocalCodec final : public AudioCodec {
public:
    using Failure = void (*)(const char*);
    using Amplifier = int (*)(bool);
    LocalCodec(Failure failure, Amplifier amplifier, bool microphone = false)
        : failure_(failure), amplifier_(amplifier), microphone_(microphone) {
        input_sample_rate_ = output_sample_rate_ = 16000;
        output_volume_ = 10;
        duplex_ = microphone;
    }
    bool MicrophoneReady() const { return mic_ready_; }
    // Set once in Start(), before AudioService builds its echo canceller.
    bool ReferenceReady() const { return reference_slot_ >= 0; }
    void Start() override {
        i2c_master_bus_config_t bus_config = {};
        bus_config.i2c_port = I2C_NUM_0;
        bus_config.sda_io_num = GPIO_NUM_1;
        bus_config.scl_io_num = GPIO_NUM_2;
        bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
        bus_config.glitch_ignore_cnt = 7;
        bus_config.flags.enable_internal_pullup = true;
        Check(i2c_new_master_bus(&bus_config, &bus_), "I2C_BUS");
        Check(i2c_master_probe(bus_, ES8311_CODEC_DEFAULT_ADDR >> 1, 100), "DAC_NO_ACK");
        i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
        channel.dma_desc_num = 4;
        channel.dma_frame_num = 160;
        channel.auto_clear_after_cb = true;
        Check(i2s_new_channel(&channel, &tx_handle_, microphone_ ? &rx_handle_ : nullptr),
              "I2S_CREATE");
        i2s_std_config_t standard = {};
        standard.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000);
        standard.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
        standard.slot_cfg =
            I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
        standard.gpio_cfg.mclk = GPIO_NUM_38;
        standard.gpio_cfg.bclk = GPIO_NUM_14;
        standard.gpio_cfg.ws = GPIO_NUM_13;
        standard.gpio_cfg.dout = GPIO_NUM_12;
        standard.gpio_cfg.din = I2S_GPIO_UNUSED;
        if (microphone_) {
            // The receiver reads 4 x 16-bit TDM slots on the same BCLK/WS, so
            // 16-bit samples ride in 32-bit slots: both frames are 64 BCLK.
            standard.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
            standard.slot_cfg.ws_width = 32;
        }
        Check(i2s_channel_init_std_mode(tx_handle_, &standard), "I2S_CONFIG");
        if (microphone_)
            PrepareReceiver();
        Check(i2s_channel_enable(tx_handle_), "I2S_ENABLE");
        Silence();
        audio_codec_i2c_cfg_t control = {};
        control.port = I2C_NUM_0;
        control.addr = ES8311_CODEC_DEFAULT_ADDR;
        control.bus_handle = bus_;
        raw_ = audio_codec_new_i2c_ctrl(&control);
        if (!raw_)
            failure_("DAC_CONTROL");
        // Vendor operations occasionally discard transfer errors: stop inside
        // the transfer wrapper, before later initialization can enable the PA.
        checked_.owner = this;
        checked_.api.is_open = [](const audio_codec_ctrl_if_t* api) {
            auto* self = reinterpret_cast<const CheckedControl*>(api)->owner;
            return self->raw_->is_open(self->raw_);
        };
        checked_.api.get_info = [](const audio_codec_ctrl_if_t* api,
                                   audio_codec_ctrl_info_t* info) {
            auto* self = reinterpret_cast<const CheckedControl*>(api)->owner;
            return self->raw_->get_info(self->raw_, info);
        };
        checked_.api.read_reg = [](const audio_codec_ctrl_if_t* api, int reg, int width, void* data,
                                   int size) {
            auto* self = reinterpret_cast<const CheckedControl*>(api)->owner;
            self->Check(self->raw_->read_reg(self->raw_, reg, width, data, size), "I2C_READ");
            return 0;
        };
        checked_.api.write_reg = [](const audio_codec_ctrl_if_t* api, int reg, int width,
                                    void* data, int size) {
            auto* self = reinterpret_cast<const CheckedControl*>(api)->owner;
            self->Check(self->raw_->write_reg(self->raw_, reg, width, data, size), "I2C_WRITE");
            return 0;
        };
        uint8_t reset = 0x1f;
        checked_.api.write_reg(&checked_.api, 0, 1, &reset, 1);
        vTaskDelay(pdMS_TO_TICKS(5));
        es8311_codec_cfg_t config = {};
        config.ctrl_if = &checked_.api;
        config.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
        config.pa_pin = -1;
        config.use_mclk = true;
        config.no_dac_ref = true;
        config.hw_gain.pa_voltage = config.hw_gain.codec_dac_voltage = 3.3f;
        dac_ = es8311_codec_new(&config);
        if (!dac_)
            failure_("DAC_CREATE");
        esp_codec_dev_sample_info_t format = {};
        format.bits_per_sample = 16;
        format.channel = 2;
        format.sample_rate = 16000;
        format.mclk_multiple = 256;
        Check(dac_->set_fs(dac_, &format), "DAC_FORMAT");
        Check(dac_->enable(dac_, true), "DAC_ENABLE");
        Check(dac_->mute(dac_, true), "DAC_MUTE");
        Check(dac_->set_vol(dac_, kDacGainDb), "DAC_VOLUME");
        Verify(true);
        Silence();
        Check(amplifier_(true), "PA_ENABLE");
        Silence();
        Check(dac_->mute(dac_, false), "DAC_UNMUTE");
        Verify(false);
        // AudioService enables the amplifier when the first PCM block arrives.
        Check(amplifier_(false), "PA_IDLE");
        if (microphone_) {
            const char* failed = StartMicrophone();
            mic_ready_ = !failed;
            if (failed)
                ESP_LOGE("BoilonMic", "Microphone unavailable: %s; playback continues", failed);
            else
                ESP_LOGI("BoilonMic",
                         "Microphone ready: ES7210 slot 0, 16 kHz, %.0f dB, powered down",
                         kMicGainDb);
#if CONFIG_BOILON_S3_GEN2_AI
            if (!failed)
                FindReference();
#endif
        }
    }
    // Deliberately do not call base Start/SetOutputVolume: those access NVS.
    void SetOutputVolume(int volume) override {
        portENTER_CRITICAL(&volume_lock_);
        volume_ = std::clamp(volume, 0, kMaxVolume);
        portEXIT_CRITICAL(&volume_lock_);
    }
    // What the speaker plays at now, whoever set it (the keys or XiaoZhi).
    int OutputVolume() {
        portENTER_CRITICAL(&volume_lock_);
        const int volume = volume_;
        portEXIT_CRITICAL(&volume_lock_);
        return volume;
    }
    void SetInputGain(float) override {}
    // Powers the ADC only while AudioService captures (an AI conversation).
    void EnableInput(bool enable) override {
        if (!mic_ready_ || enable == input_enabled_)
            return;
        // enable() reselects the inputs and resets the gain; set the gains afterwards.
        if (enable
                ? mic_->enable(mic_, true) || mic_->set_mic_gain(mic_, kMicGainDb) ||
                      (ReferenceReady() && mic_->set_mic_channel_gain(
                                               mic_, boilon::ai::echo::InputMask(reference_slot_),
                                               reference_gain_db_)) ||
                      mic_->mute_mic(mic_, false)
                : mic_->mute_mic(mic_, true) || mic_->enable(mic_, false))
            ESP_LOGW("BoilonMic", "ADC power %s failed", enable ? "up" : "down");
        input_enabled_ = enable;
        if (!enable && mic_frames_) {
            // Level summary only; samples are never stored or logged.
            const double rms = std::sqrt(static_cast<double>(mic_squares_) / mic_frames_);
            const double ref_rms = std::sqrt(static_cast<double>(ref_squares_) / mic_frames_);
            ESP_LOGI("BoilonMic", "Input off frames=%llu rms=%u peak=%u ref_rms=%u ref_peak=%u",
                     static_cast<unsigned long long>(mic_frames_), static_cast<unsigned>(rms),
                     static_cast<unsigned>(mic_peak_), static_cast<unsigned>(ref_rms),
                     static_cast<unsigned>(ref_peak_));
            mic_frames_ = mic_squares_ = mic_peak_ = ref_squares_ = ref_peak_ = 0;
        }
    }
    void EnableOutput(bool enable) override {
        Check(amplifier_(enable), "PA_OUTPUT");
        output_enabled_ = enable;
    }

protected:
    // Slot 0 (ADC MIC1) as mono 16 kHz, or [voice, reference] pairs once the
    // self-check found the reference; called only by AudioService's input task.
    int Read(int16_t* dest, int samples) override {
        if (!mic_ready_ || !input_enabled_)
            return 0;
        const int channels = input_channels_;
        const int wanted = samples / channels;
        int done = 0;
        while (done < wanted) {
            const int frames = std::min(wanted - done, kMicFrames);
            size_t bytes = 0;
            if (i2s_channel_read(rx_handle_, mic_block_, frames * 4 * sizeof(int16_t), &bytes,
                                 100) != ESP_OK)
                break;
            const int got = static_cast<int>(bytes / (4 * sizeof(int16_t)));
            for (int i = 0; i < got; ++i) {
                Measure(mic_block_[4 * i], mic_squares_, mic_peak_);
                if (ReferenceReady())
                    Measure(mic_block_[4 * i + reference_slot_], ref_squares_, ref_peak_);
            }
            boilon::ai::echo::TakeChannels(mic_block_, got, reference_slot_,
                                           dest + done * channels);
            mic_frames_ += got;
            done += got;
            if (got < frames)
                break;
        }
        return done * channels;
    }
    int Write(const int16_t* data, int samples) override {
        int16_t stereo[320];
        for (int offset = 0; offset < samples; offset += 160) {
            int count = std::min(160, samples - offset);
            // S3's toolchain does not promise lock-free std::atomic<int>.
            // Hold the spinlock only for this scalar copy, never I2S or PCM work.
            portENTER_CRITICAL(&volume_lock_);
            const int volume = volume_;
            portEXIT_CRITICAL(&volume_lock_);
            for (int i = 0; i < count; ++i)
                stereo[2 * i] = stereo[2 * i + 1] = VolumeSample(data[offset + i], volume);
            Send(stereo, count * 2 * sizeof(int16_t));
        }
        return samples;
    }

private:
    static constexpr int kMicFrames = boilon::ai::echo::kBlockFrames;
    static constexpr float kMicGainDb = 30.0f;  // P1-5 used 24 dB; voice was quiet.
    struct CheckedControl {
        audio_codec_ctrl_if_t api = {};
        LocalCodec* owner = nullptr;
    } checked_;
    Failure failure_;
    Amplifier amplifier_;
    const bool microphone_;
    bool rx_ready_ = false, mic_ready_ = false;
    const audio_codec_if_t* mic_ = nullptr;
    int16_t mic_block_[kMicFrames * 4] = {};
    uint64_t mic_frames_ = 0, mic_squares_ = 0, mic_peak_ = 0;
    int reference_slot_ = -1;  // The ES7210 slot wired back from the DAC, if any.
    int reference_gain_db_ = 0;
    uint64_t ref_squares_ = 0, ref_peak_ = 0;
    portMUX_TYPE volume_lock_ = portMUX_INITIALIZER_UNLOCKED;
    int volume_ = 10;
    i2c_master_bus_handle_t bus_ = nullptr;
    const audio_codec_ctrl_if_t* raw_ = nullptr;
    const audio_codec_if_t* dac_ = nullptr;
    void Check(int result, const char* reason) {
        if (result != 0)
            failure_(reason);
    }
    // Before the transmitter starts, as both directions share its clock.
    void PrepareReceiver() {
        i2s_tdm_config_t tdm = {};
        tdm.clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(16000);
        tdm.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
        tdm.slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
            static_cast<i2s_tdm_slot_mask_t>(I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 |
                                             I2S_TDM_SLOT3));
        tdm.slot_cfg.total_slot = 4;
        tdm.gpio_cfg.mclk = GPIO_NUM_38;
        tdm.gpio_cfg.bclk = GPIO_NUM_14;
        tdm.gpio_cfg.ws = GPIO_NUM_13;
        tdm.gpio_cfg.dout = I2S_GPIO_UNUSED;
        tdm.gpio_cfg.din = GPIO_NUM_11;
        rx_ready_ = i2s_channel_init_tdm_mode(rx_handle_, &tdm) == ESP_OK;
    }
    // Same ADC setup as P1-5 (slave, 4 x 16-bit TDM), returning why it failed.
    const char* StartMicrophone() {
        if (!rx_ready_)
            return "I2S_RX";
        if (i2c_master_probe(bus_, 0x41, 100) != ESP_OK)
            return "NO_ACK";
#if CONFIG_BOILON_S3_GEN2_AI
        audio_codec_i2c_cfg_t control = {};
        control.port = I2C_NUM_0;
        control.addr = 0x82;  // 0x41 as a codec_dev address (ES7210_AD1_AD0_01).
        control.bus_handle = bus_;
        es7210_codec_cfg_t adc = {};
        adc.ctrl_if = audio_codec_new_i2c_ctrl(&control);
        adc.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4;
        adc.mclk_div = 256;
        mic_ = adc.ctrl_if ? es7210_codec_new(&adc) : nullptr;
#endif
        esp_codec_dev_sample_info_t format = {};
        format.bits_per_sample = 16;
        format.channel = 4;
        format.sample_rate = 16000;
        format.mclk_multiple = 256;
        // enable() reselects the inputs and resets gain; set the gain afterwards.
        // Configured once, then powered down until the first conversation.
        if (!mic_ || mic_->set_fs(mic_, &format) || mic_->enable(mic_, true) ||
            mic_->set_mic_gain(mic_, kMicGainDb) || mic_->mute_mic(mic_, true) ||
            mic_->enable(mic_, false))
            return "CONFIG";
        return i2s_channel_enable(rx_handle_) == ESP_OK ? nullptr : "I2S_ENABLE";
    }
#if CONFIG_BOILON_S3_GEN2_AI
    // P5-3: play a quiet tone with the amplifier still off and read all four
    // slots at 0 dB. The microphones hear nothing, so only an input wired back
    // from the DAC follows the tone; that slot becomes the echo reference.
    void FindReference() {
        namespace echo = boilon::ai::echo;
        // About 1.2 KB, so not on the 8 KB main-task stack; freed after boot.
        struct Scratch {
            float levels[echo::kSlots][echo::kProbeBlocks];
            int16_t tone[2 * kMicFrames];
        };
        std::unique_ptr<Scratch> scratch(new (std::nothrow) Scratch());
        if (!scratch) {
            ESP_LOGW("BoilonMic", "Echo check: no memory; no barge-in this boot");
            return;
        }
        auto& levels = scratch->levels;
        auto& tone = scratch->tone;
        bool read = !(mic_->enable(mic_, true) || mic_->set_mic_gain(mic_, 0) ||
                      mic_->mute_mic(mic_, false));
        for (int block = 0; read && block < echo::kProbeBlocks; ++block) {
            for (int i = 0; i < kMicFrames; ++i)
                tone[2 * i] = tone[2 * i + 1] = echo::ToneOn(block) ? echo::ToneSample(i) : 0;
            Send(tone, sizeof(tone));
            size_t bytes = 0;
            read = i2s_channel_read(rx_handle_, mic_block_, sizeof(mic_block_), &bytes, 100) ==
                       ESP_OK &&
                   bytes == sizeof(mic_block_);
            for (int slot = 0; read && slot < echo::kSlots; ++slot)
                levels[slot][block] = echo::ToneDb(mic_block_, slot);
        }
        // Powered down until the first conversation, as before. The last blocks
        // were silent, so the transmitter is left playing zeros.
        if (mic_->mute_mic(mic_, true) || mic_->enable(mic_, false))
            ESP_LOGW("BoilonMic", "ADC power down after the echo check failed");
        if (!read) {
            ESP_LOGW("BoilonMic", "Echo check failed; no barge-in this boot");
            return;
        }
        echo::SlotLevel slot_levels[echo::kSlots];
        for (int slot = 0; slot < echo::kSlots; ++slot)
            slot_levels[slot] = echo::Levels(levels[slot]);
        reference_slot_ = echo::PickReference(slot_levels);
        if (ReferenceReady()) {
            reference_gain_db_ = echo::ReferenceGainDb(slot_levels[reference_slot_].on);
            input_channels_ = 2;
            input_reference_ = true;
        }
        ESP_LOGI("BoilonMic",
                 "Echo check dB on/off: slot0 %.0f/%.0f slot1 %.0f/%.0f slot2 %.0f/%.0f "
                 "slot3 %.0f/%.0f; reference slot %d, gain %d dB",
                 slot_levels[0].on, slot_levels[0].off, slot_levels[1].on, slot_levels[1].off,
                 slot_levels[2].on, slot_levels[2].off, slot_levels[3].on, slot_levels[3].off,
                 reference_slot_, reference_gain_db_);
    }
#endif
    static void Measure(int16_t sample, uint64_t& squares, uint64_t& peak) {
        const uint32_t level = sample < 0 ? -static_cast<int32_t>(sample) : sample;
        squares += uint64_t{level} * level;
        peak = std::max<uint64_t>(peak, level);
    }
    void Send(const int16_t* pcm, size_t bytes) {
        size_t written = 0;
        Check(i2s_channel_write(tx_handle_, pcm, bytes, &written, 100), "I2S_WRITE");
        if (written != bytes)
            failure_("I2S_SHORT_WRITE");
    }
    void Silence() {
        const int16_t zeros[320] = {};
        for (int i = 0; i < 10; ++i)
            Send(zeros, sizeof(zeros));
    }
    void Verify(bool muted) {
        int mute = 0, volume = 0;
        Check(dac_->get_reg(dac_, 0x31, &mute), "DAC_MUTE_READ");
        Check(dac_->get_reg(dac_, 0x32, &volume), "DAC_VOLUME_READ");
        if ((mute & 0x60) != (muted ? 0x60 : 0) || volume != kDacVolumeRegister)
            failure_("DAC_READBACK");
    }
};
}  // namespace boilon::local_player
