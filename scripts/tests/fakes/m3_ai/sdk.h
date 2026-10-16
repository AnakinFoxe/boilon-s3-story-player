#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

[[noreturn]] inline void esp_system_abort(const char*) { std::abort(); }

using TaskHandle_t = void*;
using EventGroupHandle_t = void*;
using esp_timer_handle_t = void*;
using esp_ae_rate_cvt_handle_t = void*;
using esp_ae_sample_t = int16_t*;
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_TIMER_TASK = 0, pdPASS = 1;
constexpr int ESP_AUDIO_SAMPLE_RATE_16K = 16000, ESP_AUDIO_MONO = 1, ESP_AUDIO_BIT16 = 16;
constexpr int ESP_OPUS_BITRATE_AUTO = 0, ESP_OPUS_ENC_APPLICATION_AUDIO = 0;
constexpr int ESP_AE_RATE_CVT_PERF_TYPE_SPEED = 0;
using esp_opus_enc_frame_duration_t = int;
using esp_opus_dec_frame_duration_t = int;
constexpr int ESP_OPUS_ENC_FRAME_DURATION_5_MS = 5, ESP_OPUS_ENC_FRAME_DURATION_10_MS = 10;
constexpr int ESP_OPUS_ENC_FRAME_DURATION_20_MS = 20, ESP_OPUS_ENC_FRAME_DURATION_40_MS = 40;
constexpr int ESP_OPUS_ENC_FRAME_DURATION_60_MS = 60, ESP_OPUS_ENC_FRAME_DURATION_80_MS = 80;
constexpr int ESP_OPUS_ENC_FRAME_DURATION_100_MS = 100, ESP_OPUS_ENC_FRAME_DURATION_120_MS = 120;
struct srmodel_list_t {};
class AudioDebugger {};
struct AudioStreamPacket {
    int sample_rate = 16000, frame_duration = 60;
    uint32_t timestamp = 0;
    std::vector<uint8_t> payload;
};
struct esp_timer_create_args_t {
    void (*callback)(void*); void* arg; int dispatch_method;
    const char* name; bool skip_unhandled_events;
};
struct esp_opus_dec_cfg_t { uint32_t sample_rate; int channel, frame_duration; bool self_delimited; };
struct esp_opus_enc_config_t {
    int sample_rate, channel, bits_per_sample, bitrate, frame_duration, application_mode, complexity;
    bool enable_fec, enable_dtx, enable_vbr;
};
struct esp_ae_rate_cvt_cfg_t {
    uint32_t src_rate, dest_rate; uint8_t channel; int bits_per_sample, complexity, perf_type;
};
namespace fake {
struct Task { void (*entry)(void*); std::string name; void* argument; unsigned stack = 0; };
inline bool event_error = false, timer_error = false, timer_start_error = false, task_error = false;
inline int event_token, timer_token;
inline unsigned event_bits = 0;
inline int timers_created = 0, timers_started = 0, timers_stopped = 0, timers_deleted = 0;
inline int codec_starts = 0, opus_dec_opened = 0, opus_enc_opened = 0, resamplers_opened = 0;
inline int engines_constructed = 0, input_runs = 0, opus_runs = 0, tasks_deleted = 0;
inline int task_attempts = 0, codec_reads = 0, demuxers_constructed = 0;
inline std::vector<Task> tasks;
inline void reset() {
    event_error = timer_error = timer_start_error = task_error = false;
    event_bits = 0;
    timers_created = timers_started = timers_stopped = timers_deleted = 0;
    codec_starts = opus_dec_opened = opus_enc_opened = resamplers_opened = 0;
    engines_constructed = input_runs = opus_runs = tasks_deleted = 0;
    task_attempts = codec_reads = demuxers_constructed = 0;
    tasks.clear();
}
template <typename... Args> void log(Args&&...) {}
}
#define ESP_LOGE(...) fake::log(__VA_ARGS__)
#define ESP_LOGW(...) fake::log(__VA_ARGS__)
#define ESP_LOGI(...) fake::log(__VA_ARGS__)
#define ESP_LOGD(...) fake::log(__VA_ARGS__)
#define TAG "AudioService"

class AudioCodec {
public:
    int rate = 16000, input_rate = 16000, channels = 1;
    bool output = false, input = false;
    std::vector<std::vector<int16_t>> writes;
    std::function<void()> on_output;
    int output_sample_rate() const { return rate; }
    int output_channels() const { return channels; }
    int input_sample_rate() const { return input_rate; }
    int input_channels() const { return 1; }
    bool output_enabled() const { return output; }
    bool input_enabled() const { return input; }
    bool duplex() const { return false; }
    void Start() { ++fake::codec_starts; }
    void EnableOutput(bool enabled) { output = enabled; }
    void EnableInput(bool enabled) { input = enabled; }
    void OutputData(std::vector<int16_t>& pcm) { writes.push_back(pcm); if (on_output) on_output(); }
    bool InputData(std::vector<int16_t>& pcm) { ++fake::codec_reads; std::fill(pcm.begin(), pcm.end(), 1); return true; }
};
class AudioEngine {
public:
    virtual ~AudioEngine() = default;
    void OnOutput(std::function<void(std::vector<int16_t>&&)>) {}
    void OnVadStateChange(std::function<void(bool)>) {}
    void OnWakeWordDetected(std::function<void(const std::string&)>) {}
};
class AfeAudioEngine : public AudioEngine { public: AfeAudioEngine() { ++fake::engines_constructed; } };
class LiteAudioEngine : public AudioEngine { public: LiteAudioEngine() { ++fake::engines_constructed; } };
class OggDemuxer {
public:
    OggDemuxer() { ++fake::demuxers_constructed; }
    void OnPacket(std::function<void(const uint8_t*, int, int, size_t)>) {}
    void Reset() {}
    void Process(const uint8_t*, size_t) {}
};

inline EventGroupHandle_t xEventGroupCreate() { return fake::event_error ? nullptr : &fake::event_token; }
inline void vEventGroupDelete(EventGroupHandle_t) {}
inline unsigned xEventGroupSetBits(EventGroupHandle_t handle, unsigned bits) {
    assert(handle); return fake::event_bits |= bits;
}
inline unsigned xEventGroupClearBits(EventGroupHandle_t handle, unsigned bits) {
    assert(handle); return fake::event_bits &= ~bits;
}
inline unsigned xEventGroupGetBits(EventGroupHandle_t handle) { assert(handle); return fake::event_bits; }
inline int esp_timer_create(const esp_timer_create_args_t* cfg, esp_timer_handle_t* handle) {
    assert(cfg->callback && cfg->arg && std::string(cfg->name) == "audio_power_timer");
    ++fake::timers_created;
    if (fake::timer_error) return -1;
    *handle = &fake::timer_token;
    return ESP_OK;
}
inline int esp_timer_start_periodic(esp_timer_handle_t, uint64_t) {
    ++fake::timers_started; return fake::timer_start_error ? -1 : ESP_OK;
}
inline int esp_timer_stop(esp_timer_handle_t) { ++fake::timers_stopped; return ESP_OK; }
inline int esp_timer_delete(esp_timer_handle_t) { ++fake::timers_deleted; return ESP_OK; }
inline int xTaskCreate(void (*entry)(void*), const char* name, unsigned stack, void* argument,
                       unsigned, TaskHandle_t* handle) {
    ++fake::task_attempts;
    if (fake::task_error) return 0;
    fake::tasks.push_back({entry, name, argument, stack});
    *handle = &fake::event_token;
    return pdPASS;
}
inline int xTaskCreatePinnedToCore(void (*entry)(void*), const char* name, unsigned stack,
                                 void* arg, unsigned priority, TaskHandle_t* handle, int) {
    return xTaskCreate(entry, name, stack, arg, priority, handle);
}
inline void vTaskDelete(void*) { ++fake::tasks_deleted; }
inline int esp_opus_dec_open(const esp_opus_dec_cfg_t*, size_t, void** handle) {
    ++fake::opus_dec_opened; *handle = &fake::event_token; return ESP_OK;
}
inline int esp_opus_enc_open(const esp_opus_enc_config_t*, size_t, void** handle) {
    ++fake::opus_enc_opened; *handle = &fake::event_token; return ESP_OK;
}
inline int esp_opus_enc_get_frame_size(void*, int* frame, int* size) { *frame = 1920; *size = 256; return 0; }
inline void esp_opus_enc_close(void*) {}
inline void esp_opus_dec_close(void*) {}
inline int esp_ae_rate_cvt_open(const esp_ae_rate_cvt_cfg_t*, esp_ae_rate_cvt_handle_t* handle) {
    ++fake::resamplers_opened; *handle = &fake::event_token; return ESP_OK;
}
inline void esp_ae_rate_cvt_close(void*) {}
inline int esp_ae_rate_cvt_get_max_out_sample_num(void*, uint32_t input, uint32_t* output) {
    *output = input; return 0;
}
inline int esp_ae_rate_cvt_process(void*, esp_ae_sample_t input, uint32_t samples,
                                 esp_ae_sample_t output, uint32_t* actual) {
    std::copy_n(input, samples, output); *actual = samples; return 0;
}
