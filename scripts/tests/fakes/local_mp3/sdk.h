#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

[[noreturn]] inline void esp_system_abort(const char*) { std::abort(); }

using TaskHandle_t = void*;
using EventGroupHandle_t = void*;
using esp_timer_handle_t = void*;
using esp_ae_rate_cvt_handle_t = void*;
struct srmodel_list_t {};
class AudioEngine {};
class AudioDebugger {};
struct AudioStreamPacket {
    int sample_rate = 16000;
    int frame_duration = 60;
    uint32_t timestamp = 0;
    std::vector<uint8_t> payload;
};

class AudioCodec {
public:
    int output_sample_rate() const { return rate; }
    bool output_enabled() const { return true; }
    void EnableOutput(bool) {}
    void OutputData(std::vector<int16_t>& pcm) {
        output.push_back(pcm);
        if (on_output) on_output();
    }
    std::vector<std::vector<int16_t>> output;
    std::function<void()> on_output;
    int rate = 24000;
};

inline int esp_timer_stop(esp_timer_handle_t) { return 0; }
inline int esp_timer_start_periodic(esp_timer_handle_t, uint64_t) { return 0; }
namespace fake_timer {
inline std::atomic<bool> enabled{false};
inline std::atomic<int64_t> now_us{0};
inline void advance(uint64_t us) {
    if (enabled)
        now_us += us;
}
}  // namespace fake_timer
inline int64_t esp_timer_get_time() {
    if (fake_timer::enabled)
        return fake_timer::now_us.load();
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
inline unsigned event_bits = 0;
inline unsigned xEventGroupSetBits(EventGroupHandle_t, unsigned bits) { return event_bits |= bits; }
inline unsigned xEventGroupClearBits(EventGroupHandle_t, unsigned bits) { return event_bits &= ~bits; }
inline unsigned xEventGroupGetBits(EventGroupHandle_t) { return event_bits; }
inline int esp_opus_dec_reset(void*) { return 0; }
#define ESP_LOGW(...) ((void)0)
inline void FakeLog(const char*, const char*, ...);
#define ESP_LOGI(tag, ...) FakeLog("", __VA_ARGS__)
#define ESP_LOGE(...) ((void)0)

constexpr int pdPASS = 1;
#define pdMS_TO_TICKS(ms) (ms)
using esp_audio_err_t = int;
constexpr int ESP_AUDIO_ERR_OK = 0, ESP_AUDIO_ERR_FAIL = -1, ESP_AE_ERR_OK = 0;
constexpr int ESP_AUDIO_SIMPLE_DEC_TYPE_MP3 = 1, ESP_AE_RATE_CVT_PERF_TYPE_SPEED = 1;
using esp_audio_simple_dec_handle_t = void*;
struct esp_audio_simple_dec_cfg_t {
    int dec_type = 0;
    bool use_frame_dec = false;
};
struct esp_audio_simple_dec_raw_t {
    uint8_t* buffer = nullptr;
    uint32_t len = 0, consumed = 0;
    bool eos = false;
};
struct esp_audio_simple_dec_out_t {
    uint8_t* buffer = nullptr;
    uint32_t len = 0, decoded_size = 0;
};
struct esp_audio_simple_dec_info_t {
    uint32_t sample_rate = 24000, channel = 2, bits_per_sample = 16;
};
struct esp_ae_rate_cvt_cfg_t {
    uint32_t src_rate = 0, dest_rate = 0, channel = 0, bits_per_sample = 0;
    int complexity = 0, perf_type = 0;
};

namespace fake {
enum class Decode { normal, error, oversized, no_progress, overconsumed, output_only, buffered, odd };
enum class Convert { normal, open_error, bound_error, oversized_bound, process_error, oversized_output };
enum class FileFault { none, read, lookahead };
inline Decode decode = Decode::normal;
inline Convert convert = Convert::normal;
inline FileFault file_fault = FileFault::none;
inline unsigned file_fault_call = 0, file_read_calls = 0, file_getc_calls = 0, file_close_calls = 0;
inline FILE* file_error = nullptr;
inline unsigned file_open_calls = 0, file_seek_calls = 0, file_tell_calls = 0;
inline size_t file_buffer_bytes = 0;  // The worker's setvbuf request.
inline unsigned seek_error_call = 0;
inline bool file_open_error = false;
inline std::array<uint64_t, 4> io_time_us{};  // open, seek, tell, read
inline uint64_t delay_time_us = 0;
inline std::function<void()> on_read;
inline std::vector<std::string> scan_logs;
inline int decode_error_call = 0, empty_output_call = 0;
inline esp_audio_simple_dec_info_t info;
inline esp_ae_rate_cvt_cfg_t conversion;
inline bool open_error = false, info_error = false, format_change = false, task_error = false;
inline bool sample_ramp = false;
inline bool frame_mode = false;
inline int decode_calls = 0, open_calls = 0, close_calls = 0, converter_open_calls = 0;
inline int converter_close_calls = 0, converter_process_calls = 0, delay_calls = 0;
inline int task_creates = 0, task_deletes = 0;
inline std::vector<size_t> reads;
inline std::vector<bool> eos;
inline std::function<void()> on_delay;
inline void (*pending_task)(void*) = nullptr;
inline void* pending_argument = nullptr;

inline void reset() {
    assert(pending_task == nullptr);
    decode = Decode::normal;
    convert = Convert::normal;
    file_fault = FileFault::none;
    file_fault_call = file_read_calls = file_getc_calls = file_close_calls = 0;
    file_error = nullptr;
    file_open_calls = file_seek_calls = file_tell_calls = seek_error_call = 0;
    file_buffer_bytes = 0;
    file_open_error = false;
    io_time_us = {};
    delay_time_us = 0;
    on_read = {};
    scan_logs.clear();
    fake_timer::enabled = false;
    fake_timer::now_us = 0;
    decode_error_call = empty_output_call = 0;
    info = {};
    conversion = {};
    open_error = info_error = format_change = task_error = false;
    sample_ramp = frame_mode = false;
    decode_calls = open_calls = close_calls = converter_open_calls = 0;
    converter_close_calls = converter_process_calls = delay_calls = task_creates = task_deletes = 0;
    reads.clear(); eos.clear(); on_delay = {};
    event_bits = 0;
}
inline void run_task() {
    assert(pending_task);
    auto task = pending_task;
    auto argument = pending_argument;
    pending_task = nullptr;
    pending_argument = nullptr;
    task(argument);
}

inline size_t ReadFile(void* bytes, size_t size, size_t count, FILE* file) {
    fake_timer::advance(io_time_us[3]);
    if (on_read)
        on_read();
    if (++file_read_calls == file_fault_call && file_fault == FileFault::read) {
        file_error = file;
        errno = EIO;
        return 0;
    }
    return std::fread(bytes, size, count, file);
}
inline FILE* OpenFile(const char* path, const char* mode) {
    ++file_open_calls;
    fake_timer::advance(io_time_us[0]);
    return file_open_error ? nullptr : std::fopen(path, mode);
}
inline int SeekFile(FILE* file, long offset, int whence) {
    fake_timer::advance(io_time_us[1]);
    if (++file_seek_calls == seek_error_call) {
        errno = EIO;
        return -1;
    }
    return std::fseek(file, offset, whence);
}
inline int BufferFile(FILE* file, char* buffer, int mode, size_t size) {
    assert(mode == _IOFBF && !buffer);
    file_buffer_bytes = size;
    return std::setvbuf(file, buffer, mode, size);
}
inline long TellFile(FILE* file) {
    ++file_tell_calls;
    fake_timer::advance(io_time_us[2]);
    return std::ftell(file);
}
inline int GetFileChar(FILE* file) {
    if (++file_getc_calls == file_fault_call && file_fault == FileFault::lookahead) {
        file_error = file;
        errno = EIO;
        return EOF;
    }
    return std::fgetc(file);
}
inline int FileError(FILE* file) { return file_error == file ? 1 : std::ferror(file); }
inline int CloseFile(FILE* file) {
    ++file_close_calls;
    if (file_error == file)
        file_error = nullptr;
    return std::fclose(file);
}
}  // namespace fake

inline void FakeLog(const char*, const char* format, ...) {
    if (!std::strstr(format, "[DEBUG-resume-scan-v1]"))
        return;
    char bytes[512];
    va_list args;
    va_start(args, format);
    const int count = std::vsnprintf(bytes, sizeof(bytes), format, args);
    va_end(args);
    assert(count >= 0 && static_cast<size_t>(count) < sizeof(bytes));
    fake::scan_logs.emplace_back(bytes);
}

inline int xTaskCreate(void (*task)(void*), const char* name, unsigned stack,
                       void* argument, unsigned priority, TaskHandle_t*) {
    assert(std::string(name) == "local_mp3" && stack == 8192 && priority == 3);
    ++fake::task_creates;
    if (fake::task_error) return 0;
    assert(fake::pending_task == nullptr);
    fake::pending_task = task;
    fake::pending_argument = argument;
    return pdPASS;
}
inline void vTaskDelete(void*) { ++fake::task_deletes; }
inline void vTaskDelay(unsigned ticks) {
    fake_timer::advance(fake::delay_time_us);
    assert(ticks > 0 && ticks <= 10);
    assert(++fake::delay_calls < 2000);  // A bug must fail rather than hang the suite.
    if (fake::on_delay) fake::on_delay();
}
inline int esp_mp3_dec_register() { return ESP_AUDIO_ERR_OK; }
inline int esp_audio_simple_dec_open(const esp_audio_simple_dec_cfg_t* config,
                                     esp_audio_simple_dec_handle_t* handle) {
    assert(config->dec_type == ESP_AUDIO_SIMPLE_DEC_TYPE_MP3);
    fake::frame_mode = config->use_frame_dec;
    ++fake::open_calls;
    if (fake::open_error) return ESP_AUDIO_ERR_FAIL;
    *handle = &fake::decode_calls;
    return ESP_AUDIO_ERR_OK;
}
inline int esp_audio_simple_dec_process(esp_audio_simple_dec_handle_t handle,
                                        esp_audio_simple_dec_raw_t* raw,
                                        esp_audio_simple_dec_out_t* frame) {
    assert(handle && raw->buffer && raw->len > 0 && raw->len <= (fake::frame_mode ? 2048U : 512U));
    assert(frame->buffer && frame->len == 4608);
    ++fake::decode_calls;
    fake::reads.push_back(raw->len);
    fake::eos.push_back(raw->eos);
    if (fake::decode == fake::Decode::error || fake::decode_calls == fake::decode_error_call)
        return ESP_AUDIO_ERR_FAIL;
    raw->consumed = raw->len;
    if (fake::decode == fake::Decode::oversized) {
        frame->decoded_size = frame->len + 1;
        return ESP_AUDIO_ERR_OK;
    }
    if (fake::decode == fake::Decode::overconsumed) {
        raw->consumed = raw->len + 1;
        return ESP_AUDIO_ERR_OK;
    }
    if (fake::decode == fake::Decode::no_progress) {
        raw->consumed = 0;
        return ESP_AUDIO_ERR_OK;
    }
    if ((fake::decode == fake::Decode::buffered && fake::decode_calls == 1) ||
        fake::decode_calls == fake::empty_output_call) {
        return ESP_AUDIO_ERR_OK;
    }
    const size_t samples = fake::decode == fake::Decode::output_only              ? 1
                           : fake::frame_mode && ((raw->buffer[1] >> 3) & 3) != 3 ? 576
                                                                                  : 1152;
    if (fake::decode == fake::Decode::output_only) raw->consumed = 0;
    const auto channels = fake::info.channel == 1 ? 1 : 2;
    uint64_t sample_base = (fake::decode_calls - 1) * 1152;
    if (fake::frame_mode) {
        assert(raw->len >= 44 && raw->buffer[0] == 0xff);
        sample_base = ((uint32_t{raw->buffer[40]} << 24) | (uint32_t{raw->buffer[41]} << 16) |
                       (uint32_t{raw->buffer[42]} << 8) | raw->buffer[43]) *
                      uint64_t{samples};
    }
    auto* pcm = reinterpret_cast<int16_t*>(frame->buffer);
    for (size_t i = 0; i < samples; ++i) {
        if (channels == 1)
            pcm[i] = fake::sample_ramp ? static_cast<int16_t>(sample_base + i) : 500;
        else { pcm[2 * i] = 32000; pcm[2 * i + 1] = -31000; }
    }
    frame->decoded_size = samples * channels * sizeof(int16_t);
    if (fake::decode == fake::Decode::odd) --frame->decoded_size;
    return ESP_AUDIO_ERR_OK;
}
inline int esp_audio_simple_dec_get_info(esp_audio_simple_dec_handle_t handle,
                                         esp_audio_simple_dec_info_t* info) {
    assert(handle);
    if (fake::info_error) return ESP_AUDIO_ERR_FAIL;
    *info = fake::info;
    if (fake::format_change && fake::decode_calls > 1) info->sample_rate = 16000;
    return ESP_AUDIO_ERR_OK;
}
inline void esp_audio_simple_dec_close(esp_audio_simple_dec_handle_t handle) {
    assert(handle); ++fake::close_calls;
}
inline int esp_ae_rate_cvt_open(const esp_ae_rate_cvt_cfg_t* config,
                               esp_ae_rate_cvt_handle_t* handle) {
    assert(config->channel == 1 && config->bits_per_sample == 16);
    ++fake::converter_open_calls;
    if (fake::convert == fake::Convert::open_error) return -1;
    fake::conversion = *config;
    *handle = &fake::conversion;
    return 0;
}
inline int esp_ae_rate_cvt_get_max_out_sample_num(esp_ae_rate_cvt_handle_t handle,
                                                uint32_t input, uint32_t* needed) {
    assert(handle && input <= 1152);
    if (fake::convert == fake::Convert::bound_error) return -1;
    *needed = fake::convert == fake::Convert::oversized_bound ? 8193 :
        input * fake::conversion.dest_rate / fake::conversion.src_rate;
    return 0;
}
inline int esp_ae_rate_cvt_process(esp_ae_rate_cvt_handle_t handle, const int16_t* input,
                                  uint32_t samples, int16_t* output, uint32_t* count) {
    assert(handle && *count == 8192 && samples <= 1152);
    ++fake::converter_process_calls;
    if (!fake::sample_ramp)
        for (uint32_t i = 0; i < samples; ++i) assert(input[i] == 500);
    if (fake::convert == fake::Convert::process_error) return -1;
    if (fake::convert == fake::Convert::oversized_output) { *count = 8193; return 0; }
    *count = samples * fake::conversion.dest_rate / fake::conversion.src_rate;
    assert(*count <= 8192);
    for (uint32_t i = 0; i < *count; ++i)
        output[i] = input[i * fake::conversion.src_rate / fake::conversion.dest_rate];
    return 0;
}
inline void esp_ae_rate_cvt_close(esp_ae_rate_cvt_handle_t handle) {
    assert(handle); ++fake::converter_close_calls;
}
