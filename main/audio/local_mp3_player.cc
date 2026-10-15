#include "local_mp3_player.h"

#include "audio_service.h"
#include "esp_audio_simple_dec.h"
#include "esp_log.h"
#include "esp_mp3_dec.h"
#include "esp_timer.h"
#include "media/wav_format.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <vector>

namespace {
// Frame headers seen in the current file, so a later start in the same file (a
// seek, or the story coming back after the AI) scans from a nearby frame instead
// of the file head. Only the worker uses it, and one worker runs at a time.
struct FrameIndex {
    struct Anchor {
        long offset = 0;
        uint64_t samples = 0;  // Source samples before this frame.
    };
    static constexpr unsigned kSpacing = 32;  // Frames between anchors (~0.8 s).
    std::string path;
    unsigned rate = 0, channels = 0, frame_samples = 0;
    // The latest 256 anchors added: about 3.5 minutes at 44.1 kHz.
    std::array<Anchor, 256> anchors{};
    size_t added = 0;

    void Use(const std::string& file) {
        if (file != path) {
            path = file;
            Clear();
        }
    }
    void Clear() { added = rate = channels = frame_samples = 0; }
    size_t Count() const { return std::min(added, anchors.size()); }
    void Add(long offset, uint64_t samples, unsigned frame_rate, unsigned frame_channels,
             unsigned per_frame) {
        if (samples % (uint64_t{kSpacing} * per_frame) ||
            (added && (frame_rate != rate || frame_channels != channels ||
                       per_frame != frame_samples)))
            return;
        rate = frame_rate;
        channels = frame_channels;
        frame_samples = per_frame;
        for (size_t i = 0; i < Count(); ++i)
            if (anchors[i].samples == samples)
                return;
        anchors[added++ % anchors.size()] = {offset, samples};
    }
    const Anchor* Before(uint64_t samples) const {
        const Anchor* latest = nullptr;
        for (size_t i = 0; i < Count(); ++i)
            if (anchors[i].samples <= samples && (!latest || anchors[i].samples > latest->samples))
                latest = &anchors[i];
        return latest;
    }
};
}  // namespace

struct LocalMp3Player::State {
    explicit State(AudioService& service) : audio(service) {}
    AudioService& audio;
    std::atomic<bool> running{false};
    std::atomic<bool> cancelled{false};
    std::atomic<bool> paused{false};
    std::atomic<uint64_t> session{0};
    std::atomic<Result> result{Result::kIdle};
    std::atomic<bool> read_failed{false};
    std::atomic<uint64_t> start_ms{0}, position_ms{0}, duration_ms{0};
    std::string path;
    FrameIndex index;
};

namespace {
struct DecoderBuffers {
    std::array<uint8_t, 2048> input{};    // Also fits a complete Layer III frame (max 1441).
    std::array<int16_t, 2304> decoded{};  // One 1152-sample stereo MP3 frame.
    std::array<int16_t, 1152> mono{};
    std::array<int16_t, 8192> converted{};  // Fixed 8k->48k upper bound + filter room.
};

void YieldWorker() { vTaskDelay(pdMS_TO_TICKS(10) ? pdMS_TO_TICKS(10) : 1); }
constexpr size_t kFileBufferBytes = 8192;

// F08: one WAV frame as a mono 16-bit sample. Wider integers keep their top 16
// bits; 8-bit PCM is unsigned; float is clipped (NaN reads as silence's floor).
int16_t WavMono(const uint8_t* frame, const media::WavFormat& wav) {
    const unsigned bytes = wav.bits / 8;
    int32_t sum = 0;
    for (unsigned channel = 0; channel < wav.channels; ++channel, frame += bytes) {
        if (wav.is_float) {
            float value;
            std::memcpy(&value, frame, sizeof(value));
            value = value > 1.0f ? 1.0f : value >= -1.0f ? value : -1.0f;
            sum += static_cast<int32_t>(value * 32767.0f);
        } else if (bytes == 1) {
            sum += (frame[0] - 128) * 256;
        } else {
            sum += static_cast<int16_t>(frame[bytes - 2] | frame[bytes - 1] << 8);
        }
    }
    return static_cast<int16_t>(sum / static_cast<int32_t>(wav.channels));
}

struct Mp3Frame {
    unsigned bytes = 0, samples = 0, rate = 0, channels = 0;
};
bool ParseMp3Frame(const uint8_t* bytes, Mp3Frame& frame) {
    const uint32_t header = (uint32_t{bytes[0]} << 24) | (uint32_t{bytes[1]} << 16) |
                            (uint32_t{bytes[2]} << 8) | bytes[3];
    const unsigned version = (header >> 19) & 3, bitrate = (header >> 12) & 15;
    const unsigned frequency = (header >> 10) & 3;
    if ((header & 0xffe00000) != 0xffe00000 || version == 1 || ((header >> 17) & 3) != 1 ||
        !bitrate || bitrate == 15 || frequency == 3 || (header & 3) == 2)
        return false;
    static constexpr unsigned rates[] = {44100, 48000, 32000};
    static constexpr unsigned mpeg1[] = {0,   32,  40,  48,  56,  64,  80, 96,
                                         112, 128, 160, 192, 224, 256, 320};
    static constexpr unsigned mpeg2[] = {0,  8,  16, 24,  32,  40,  48, 56,
                                         64, 80, 96, 112, 128, 144, 160};
    frame.rate = rates[frequency] / (version == 3 ? 1 : version == 2 ? 2 : 4);
    frame.channels = ((header >> 6) & 3) == 3 ? 1 : 2;
    frame.samples = version == 3 ? 1152 : 576;
    frame.bytes = (version == 3 ? 144000 * mpeg1[bitrate] : 72000 * mpeg2[bitrate]) / frame.rate +
                  ((header >> 9) & 1);
    return frame.bytes >= 4 && frame.bytes <= 2048;
}
// Offset of the first frame after an optional ID3v2 tag, from the file's first
// 10 bytes; false for tag layouts the frame scan does not handle.
bool FirstFrame(const uint8_t* bytes, uint64_t& offset) {
    offset = 0;
    if (bytes[0] != 'I' || bytes[1] != 'D' || bytes[2] != '3')
        return true;
    if (bytes[3] < 2 || bytes[3] > 4 || bytes[4] == 0xff)
        return false;
    uint64_t tag = 0;
    for (int i = 6; i < 10; ++i) {
        if (bytes[i] & 0x80)
            return false;
        tag = (tag << 7) | bytes[i];
    }
    offset = 10 + tag + (bytes[3] == 4 && (bytes[5] & 0x10) ? 10 : 0);
    return true;
}
// Follows frame headers through the stream decoder's sequential reads, so
// playback from the file head also leaves anchors. Stops at the first header it
// does not understand; the decoder itself is unaffected.
struct FrameWalker {
    uint64_t next = 0, samples = 0;
    std::array<uint8_t, 4> header{};
    size_t have = 0;
    unsigned rate = 0, channels = 0;
    bool started = false, valid = true;

    void Feed(const uint8_t* bytes, size_t count, uint64_t at, FrameIndex& index) {
        if (!valid || !count)
            return;
        if (!started) {
            valid = at == 0 && count >= 10 && FirstFrame(bytes, next);
            started = true;
        }
        // Reads are sequential, so the next header byte is never behind `at`.
        while (valid && next + have < at + count) {
            header[have] = bytes[next + have - at];
            if (++have < header.size())
                continue;
            have = 0;
            Mp3Frame frame;
            if (!ParseMp3Frame(header.data(), frame) ||
                (rate && (frame.rate != rate || frame.channels != channels))) {
                valid = false;
                return;
            }
            rate = frame.rate;
            channels = frame.channels;
            index.Add(static_cast<long>(next), samples, frame.rate, frame.channels, frame.samples);
            samples += frame.samples;
            next += frame.bytes;
        }
    }
};

struct ResumeFrame {
    long offset = 0, size = 0;
    uint64_t samples = 0;
    unsigned rate = 0, channels = 0;
};
// Temporary, fixed-size wall-clock accounting. No per-frame log or policy change.
struct ResumeScanProfile {
    enum Part { kOpen, kSeek, kTell, kRead, kReady, kYield, kParts };
    struct Cost {
        uint64_t calls = 0, total_us = 0, max_us = 0;
    };
    std::array<Cost, kParts> costs{};
    uint64_t frames = 0, read_bytes = 0;
    int64_t begin_us;
    bool enabled, finished = false;

    ResumeScanProfile(int64_t begin, bool active) : begin_us(begin), enabled(active) {}
    ~ResumeScanProfile() { Finish("early_exit"); }
    template <typename Action>
    auto Measure(Part part, Action action) {
        if (!enabled)
            return action();
        const auto begin = esp_timer_get_time();
        auto value = action();
        const auto elapsed = static_cast<uint64_t>(esp_timer_get_time() - begin);
        auto& cost = costs[part];
        ++cost.calls;
        cost.total_us += elapsed;
        cost.max_us = std::max(cost.max_us, elapsed);
        return value;
    }
    int Seek(FILE* file, long offset, int whence) {
        return Measure(kSeek, [&] { return std::fseek(file, offset, whence); });
    }
    size_t Read(FILE* file, uint8_t* bytes, size_t count) {
        const auto size = Measure(kRead, [&] { return std::fread(bytes, 1, count, file); });
        read_bytes += size;
        return size;
    }
    void Finish(const char* outcome) {
        if (!enabled || finished)
            return;
        finished = true;
        const auto total = static_cast<uint64_t>(esp_timer_get_time() - begin_us);
        uint64_t accounted = 0;
        for (const auto& cost : costs)
            accounted += cost.total_us;
        auto u = [](uint64_t value) { return static_cast<unsigned long long>(value); };
        ESP_LOGI("BoilonMp3",
                 "[DEBUG-resume-scan-v1] total_us=%llu accounted_us=%llu other_us=%llu "
                 "outcome=%s frames=%llu read_bytes=%llu",
                 u(total), u(accounted), u(total >= accounted ? total - accounted : 0), outcome,
                 u(frames), u(read_bytes));
        static constexpr const char* names[] = {"open", "seek", "tell", "read", "ready", "yield"};
        for (size_t i = 0; i < costs.size(); ++i)
            ESP_LOGI("BoilonMp3",
                     "[DEBUG-resume-scan-v1] part=%s calls=%llu total_us=%llu max_us=%llu",
                     names[i], u(costs[i].calls), u(costs[i].total_us), u(costs[i].max_us));
    }
};
// Scan actual frame lengths (including VBR/padding), never a bitrate estimate.
// The frame-decoder API supports starting midstream. Keep 32 frames of discarded
// preroll for the MP3 reservoir and synthesis state; any decoder error still stops.
// The scan starts at a remembered frame when one leaves room for that preroll.
// The cache holds one aligned block and blocks are read in file order: a frame
// never ends beyond the next block, so the scan seeks only to its first block.
template <typename Ready>
bool LocateResumeFrame(FILE* file, uint64_t target_ms, Ready ready, ResumeFrame& result,
                       std::array<uint8_t, 2048>& cache, bool& io_error,
                       ResumeScanProfile& profile, FrameIndex& index) {
    if (!target_ms)
        return false;
    if (profile.Seek(file, 0, SEEK_END) ||
        (result.size =
             profile.Measure(ResumeScanProfile::kTell, [&] { return std::ftell(file); })) < 0 ||
        profile.Seek(file, 0, SEEK_SET)) {
        io_error = true;
        return false;
    }
    if (result.size < 10)
        return false;
    std::array<uint8_t, 10> bytes{};
    if (profile.Read(file, bytes.data(), bytes.size()) != bytes.size()) {
        io_error = true;
        return false;
    }
    uint64_t cache_offset = 0;
    size_t cached = 0;
    auto read_header = [&](uint64_t at) {
        for (uint64_t want = at; want < at + 4; ++want) {
            if (!cached || want < cache_offset || want - cache_offset >= cached) {
                const uint64_t block = want - want % cache.size();
                const size_t count = std::min<uint64_t>(cache.size(), result.size - block);
                // The file position already follows the last block read.
                if ((!cached || block != cache_offset + cached) &&
                    profile.Seek(file, static_cast<long>(block), SEEK_SET)) {
                    io_error = true;
                    return false;
                }
                if ((cached = profile.Read(file, cache.data(), count)) != count) {
                    io_error = true;
                    return false;
                }
                cache_offset = block;
            }
            bytes[want - at] = cache[want - cache_offset];
        }
        return true;
    };
    uint64_t offset = 0, samples = 0;
    if (!FirstFrame(bytes.data(), offset))
        return false;
    if (index.Count()) {
        const uint64_t target = target_ms * index.rate / 1000;
        const uint64_t preroll = 32ULL * index.frame_samples;
        const auto* anchor = target >= preroll ? index.Before(target - preroll) : nullptr;
        if (anchor && anchor->offset + 4 <= result.size) {
            if (!read_header(anchor->offset))
                return false;
            Mp3Frame frame;
            if (ParseMp3Frame(bytes.data(), frame) && frame.rate == index.rate &&
                frame.channels == index.channels) {
                offset = anchor->offset;
                samples = anchor->samples;
            } else {
                index.Clear();  // The file no longer matches; scan from the head.
            }
        }
    }
    std::array<ResumeFrame, 33> recent{};
    unsigned rate = 0, channels = 0;
    uint64_t frames = 0;
    while (ready() && offset < static_cast<uint64_t>(result.size)) {
        if (static_cast<uint64_t>(result.size) - offset < 4 || !read_header(offset))
            return false;
        Mp3Frame frame;
        if (!ParseMp3Frame(bytes.data(), frame) ||
            frame.bytes > static_cast<uint64_t>(result.size) - offset ||
            (rate && (rate != frame.rate || channels != frame.channels)))
            return false;
        rate = frame.rate;
        channels = frame.channels;
        index.Add(static_cast<long>(offset), samples, rate, channels, frame.samples);
        ++profile.frames;
        recent[frames % recent.size()] = {static_cast<long>(offset), result.size, samples, rate,
                                          channels};
        if (samples + frame.samples >= target_ms * rate / 1000) {
            if (frames < 32)
                return false;  // Short seeks keep the original stream parser.
            result = recent[(frames - 32) % recent.size()];
            io_error = profile.Seek(file, result.offset, SEEK_SET) != 0;
            return !io_error;
        }
        samples += frame.samples;
        offset += frame.bytes;
        if (++frames % 256 == 0)
            profile.Measure(ResumeScanProfile::kYield, [] {
                vTaskDelay(1);
                return 0;
            });
    }
    return false;
}
}  // namespace

LocalMp3Player::LocalMp3Player(AudioService& audio) : state_(std::make_shared<State>(audio)) {}

LocalMp3Player::~LocalMp3Player() { Stop(); }

bool LocalMp3Player::Start(const std::string& path) { return StartAt(path, 0); }

bool LocalMp3Player::StartAt(const std::string& path, uint64_t position_ms) {
    if (path.empty() || path.size() > 512 || path.front() != '/' ||
        path.find('\0') != std::string::npos || position_ms > 24ULL * 60 * 60 * 1000) {
        return false;
    }
    bool expected = false;
    if (!state_->running.compare_exchange_strong(expected, true)) {
        return false;
    }
    const auto session = state_->audio.BeginLocalPlayback();
    if (!session) {
        state_->running = false;
        return false;
    }
    state_->session = session;
    state_->path = path;
    state_->start_ms = position_ms;
    state_->position_ms = state_->duration_ms = 0;
    state_->cancelled = state_->paused = state_->read_failed = false;
    state_->result = Result::kRunning;
    auto* argument = new (std::nothrow) std::shared_ptr<State>(state_);
    if (!argument || xTaskCreate(
                         [](void* opaque) {
                             {
                                 std::unique_ptr<std::shared_ptr<State>> owner(
                                     static_cast<std::shared_ptr<State>*>(opaque));
                                 Run(*owner);
                             }
                             vTaskDelete(nullptr);
                         },
                         "local_mp3", 8192, argument, 3, nullptr) != pdPASS) {
        delete argument;
        state_->audio.EndLocalPlayback(session);
        state_->result = Result::kError;
        state_->running = false;
        return false;
    }
    return true;
}

bool LocalMp3Player::Pause(bool paused) {
    if (!Running() || !state_->audio.PauseLocalPlayback(state_->session.load(), paused)) {
        return false;
    }
    state_->paused = paused;
    return true;
}

void LocalMp3Player::Stop() {
    state_->cancelled = true;
    state_->audio.EndLocalPlayback(state_->session.load());
}

bool LocalMp3Player::Running() const { return state_->running.load(); }

LocalMp3Player::Result LocalMp3Player::LastResult() const { return state_->result.load(); }

bool LocalMp3Player::ReadFailed() const { return state_->read_failed.load(); }

uint64_t LocalMp3Player::PositionMs() const {
    if (state_->running.load()) {
        const auto samples = state_->audio.LocalPlaybackSamples(state_->session.load());
        const auto rate = state_->audio.LocalOutputSampleRate();
        if (samples && rate > 0)
            state_->position_ms = state_->start_ms.load() + samples * 1000 / rate;
    }
    return state_->position_ms.load();
}

uint64_t LocalMp3Player::DurationMs() const { return state_->duration_ms.load(); }

void LocalMp3Player::Run(const std::shared_ptr<State>& state) {
    const uint64_t session = state->session.load();
    auto live = [&] {
        return !state->cancelled.load() && state->audio.IsLocalPlaybackActive(session);
    };
    auto ready = [&] {
        while (live() && state->paused.load()) {
            YieldWorker();
        }
        return live();
    };
    FILE* file = nullptr;
    esp_audio_simple_dec_handle_t decoder = nullptr;
    esp_ae_rate_cvt_handle_t converter = nullptr;
    auto buffers = std::unique_ptr<DecoderBuffers>(new (std::nothrow) DecoderBuffers);
    const auto decode_begin_us = esp_timer_get_time();
    uint64_t decoded_frames = 0;
    bool submitted_pcm = false;
    bool success = false;
    bool open_failed = false;
    const char* stage = "memory";  // Where decoding gave up, for the log.
    std::vector<int16_t> block;
    // The output runs at target_rate; another source rate gets a converter.
    auto open_converter = [&](uint32_t source_rate, int target_rate) {
        if (source_rate == static_cast<uint32_t>(target_rate))
            return true;
        esp_ae_rate_cvt_cfg_t rate{};
        rate.src_rate = source_rate;
        rate.dest_rate = target_rate;
        rate.channel = 1;
        rate.bits_per_sample = 16;
        rate.complexity = 2;
        rate.perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED;
        return esp_ae_rate_cvt_open(&rate, &converter) == ESP_AE_ERR_OK;
    };
    // Resamples the first `count` samples of buffers->mono and queues them for
    // the speaker in blocks of at most 960; false on an error or cancellation.
    auto emit = [&](size_t count) {
        const int16_t* pcm = buffers->mono.data();
        uint32_t output_samples = count;
        if (converter) {
            uint32_t needed = 0;
            if (esp_ae_rate_cvt_get_max_out_sample_num(converter, count, &needed) !=
                    ESP_AE_ERR_OK ||
                needed > buffers->converted.size()) {
                return false;
            }
            output_samples = buffers->converted.size();
            if (esp_ae_rate_cvt_process(converter, buffers->mono.data(), count,
                                        buffers->converted.data(),
                                        &output_samples) != ESP_AE_ERR_OK ||
                output_samples > buffers->converted.size()) {
                return false;
            }
            pcm = buffers->converted.data();
        }
        for (size_t offset = 0; offset < output_samples;) {
            const size_t length = std::min<size_t>(960, output_samples - offset);
            block.assign(pcm + offset, pcm + offset + length);
            while (ready()) {
                const auto submitted = state->audio.SubmitLocalPcm(session, block);
                if (submitted == LocalPcmResult::kAccepted) {
                    if (!submitted_pcm && state->start_ms.load())
                        ESP_LOGI("BoilonMp3",
                                 "Resume first_pcm_submitted elapsed_ms=%llu "
                                 "decoded_frames=%llu",
                                 static_cast<unsigned long long>(
                                     (esp_timer_get_time() - decode_begin_us) / 1000),
                                 static_cast<unsigned long long>(decoded_frames));
                    submitted_pcm = true;
                    break;
                }
                if (submitted != LocalPcmResult::kWouldBlock) {
                    return false;
                }
                YieldWorker();
            }
            if (!live()) {
                return false;
            }
            offset += length;
        }
        return true;
    };
    // F08: PCM straight from the file. The header gives the duration at once,
    // and a start position is a byte offset: no frame scan or preroll.
    auto play_wav = [&](const media::WavFormat& wav, int target_rate) {
        const uint32_t frame_bytes = wav.FrameBytes();
        const uint64_t frames = wav.Frames();
        const uint64_t first = std::min<uint64_t>(state->start_ms.load() * wav.rate / 1000, frames);
        state->duration_ms = wav.DurationMs();
        if (first == frames) {  // Sought to the end: a normal finish, as for MP3.
            state->start_ms = state->duration_ms.load();
            state->position_ms = state->duration_ms.load();
            return true;
        }
        const uint64_t offset = wav.data_offset + first * frame_bytes;
        stage = "read";
        if (offset > LONG_MAX || std::fseek(file, static_cast<long>(offset), SEEK_SET) != 0)
            return false;
        stage = "output";
        if (!open_converter(wav.rate, target_rate))
            return false;
        // The decoded-frame buffer holds the raw bytes; at most 1152 frames a step.
        auto* raw = reinterpret_cast<uint8_t*>(buffers->decoded.data());
        const size_t step =
            std::min<size_t>(buffers->mono.size(), sizeof(buffers->decoded) / frame_bytes);
        for (uint64_t left = frames - first; left;) {
            if (!ready())
                return false;
            const size_t count = static_cast<size_t>(std::min<uint64_t>(left, step));
            stage = "read";
            if (std::fread(raw, frame_bytes, count, file) != count)
                return false;
            for (size_t i = 0; i < count; ++i)
                buffers->mono[i] = WavMono(raw + i * frame_bytes, wav);
            ++decoded_frames;
            stage = "output";
            if (!emit(count))
                return false;
            left -= count;
        }
        return true;
    };
    auto decode_file = [&]() -> bool {
        ResumeScanProfile profile(decode_begin_us, state->start_ms.load() != 0);
        auto scan_ready = [&] { return profile.Measure(ResumeScanProfile::kReady, ready); };
        if (!buffers || !scan_ready()) {
            profile.Finish("not_ready");
            return false;
        }
        stage = "open";
        file = profile.Measure(ResumeScanProfile::kOpen,
                               [&] { return std::fopen(state->path.c_str(), "rb"); });
        if (!file) {
            open_failed = true;
            profile.Finish("open_error");
            return false;
        }
        // The libc's default stdio buffer is 128 bytes, which turned every 2 KB
        // read into single-sector SD reads. A larger one (heap, so PSRAM) gives
        // multi-sector reads; without it the default still works.
        if (std::setvbuf(file, nullptr, _IOFBF, kFileBufferBytes) != 0)
            ESP_LOGW("BoilonMp3", "File buffer stays at the libc default");
        const int target_rate = state->audio.LocalOutputSampleRate();
        if (target_rate < 8000 || target_rate > 48000) {
            return false;
        }
        if (media::IsWavName(state->path)) {
            profile.enabled = false;  // The MP3 resume scan profile does not apply.
            stage = "header";
            long size = -1;
            media::WavFormat wav;
            const bool parsed =
                std::fseek(file, 0, SEEK_END) == 0 && (size = std::ftell(file)) >= 0 &&
                media::ParseWav(
                    static_cast<uint64_t>(size),
                    [&](uint64_t at, uint8_t* bytes, size_t count) {
                        return at <= LONG_MAX &&
                               std::fseek(file, static_cast<long>(at), SEEK_SET) == 0 &&
                               std::fread(bytes, 1, count, file) == count;
                    },
                    wav);
            return parsed && play_wav(wav, target_rate);
        }
        state->index.Use(state->path);
        stage = "locate";
        ResumeFrame resume_frame;
        bool scan_io_error = false;
        const bool frame_mode =
            LocateResumeFrame(file, state->start_ms.load(), scan_ready, resume_frame,
                              buffers->input, scan_io_error, profile, state->index);
        if (!frame_mode && (scan_io_error || std::ferror(file) || !scan_ready() ||
                            profile.Seek(file, 0, SEEK_SET))) {
            profile.Finish("scan_error_or_cancel");
            return false;
        }
        profile.Finish(frame_mode ? "frame_mode" : "fallback");
        if (state->start_ms.load())
            ESP_LOGI(
                "BoilonMp3", "Resume frame_scan elapsed_ms=%llu frame_mode=%d preroll_ms=%llu",
                static_cast<unsigned long long>((esp_timer_get_time() - decode_begin_us) / 1000),
                frame_mode,
                static_cast<unsigned long long>(
                    frame_mode
                        ? state->start_ms.load() - resume_frame.samples * 1000 / resume_frame.rate
                        : 0));
        stage = "decoder";
        static std::once_flag registration;
        static esp_audio_err_t registered = ESP_AUDIO_ERR_FAIL;
        std::call_once(registration, [] { registered = esp_mp3_dec_register(); });
        esp_audio_simple_dec_cfg_t config{};
        config.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
        config.use_frame_dec = frame_mode;
        if (registered != ESP_AUDIO_ERR_OK ||
            esp_audio_simple_dec_open(&config, &decoder) != ESP_AUDIO_ERR_OK) {
            return false;
        }
        esp_audio_simple_dec_info_t format{};
        bool have_pcm = false;
        uint64_t source_samples = frame_mode ? resume_frame.samples : 0;
        uint64_t stream_offset = 0;
        FrameWalker walker;
        unsigned skipped_frames = 0;
        block.reserve(960);
        while (ready()) {
            size_t count = 0;
            Mp3Frame encoded;
            stage = "read";
            if (frame_mode) {
                const auto offset = std::ftell(file);
                if (offset < 0 || offset > resume_frame.size)
                    return false;
                count = std::fread(buffers->input.data(), 1, 4, file);
                if (count) {
                    if (count != 4)
                        return false;
                    if (resume_frame.size - offset == 128 && buffers->input[0] == 'T' &&
                        buffers->input[1] == 'A' && buffers->input[2] == 'G') {
                        count = 0;  // Optional ID3v1 trailer is not an audio frame.
                    } else {
                        if (!ParseMp3Frame(buffers->input.data(), encoded) ||
                            encoded.bytes > static_cast<uint64_t>(resume_frame.size - offset) ||
                            encoded.rate != resume_frame.rate ||
                            encoded.channels != resume_frame.channels ||
                            std::fread(buffers->input.data() + 4, 1, encoded.bytes - 4, file) !=
                                encoded.bytes - 4)
                            return false;
                        count = encoded.bytes;
                        state->index.Add(offset, source_samples, encoded.rate, encoded.channels,
                                         encoded.samples);
                    }
                }
            } else {
                count = std::fread(buffers->input.data(), 1, 512, file);
                walker.Feed(buffers->input.data(), count, stream_offset, state->index);
                stream_offset += count;
            }
            if (std::ferror(file)) {
                return false;
            }
            if (!count) {
                if (have_pcm) {
                    state->duration_ms = source_samples * 1000 / format.sample_rate;
                    if (!submitted_pcm && state->start_ms.load() >= state->duration_ms.load()) {
                        // A valid stream sought beyond EOF ends normally; corrupt/no-PCM
                        // streams still fail below. Unknown duration can occur during seek.
                        state->start_ms = state->duration_ms.load();
                        state->position_ms = state->duration_ms.load();
                    }
                }
                return have_pcm;
            }
            // Look ahead once so exact 512-byte multiples also signal final input.
            const int next = std::fgetc(file);
            if (std::ferror(file) || (next != EOF && std::ungetc(next, file) == EOF)) {
                return false;
            }
            esp_audio_simple_dec_raw_t raw{};
            raw.buffer = buffers->input.data();
            raw.len = count;
            raw.eos = next == EOF;
            unsigned no_input_progress = 0;
            stage = "decode";
            while (raw.len && ready()) {
                esp_audio_simple_dec_out_t frame{};
                frame.buffer = reinterpret_cast<uint8_t*>(buffers->decoded.data());
                frame.len = sizeof(buffers->decoded);
                raw.consumed = 0;
                if (esp_audio_simple_dec_process(decoder, &raw, &frame) != ESP_AUDIO_ERR_OK ||
                    raw.consumed > raw.len || frame.decoded_size > frame.len ||
                    (!raw.consumed && !frame.decoded_size)) {
                    return false;  // Includes oversized output; no unbounded realloc/retry.
                }
                if (!raw.consumed && ++no_input_progress > 64) {
                    return false;
                }
                if (raw.consumed) {
                    no_input_progress = 0;
                }
                raw.buffer += raw.consumed;
                raw.len -= raw.consumed;
                if (frame_mode && raw.consumed != count)
                    return false;
                if (!frame.decoded_size) {
                    if (frame_mode) {
                        if (source_samples + encoded.samples >
                            state->start_ms.load() * encoded.rate / 1000)
                            return false;  // Missing target PCM cannot count as a successful seek.
                        source_samples += encoded.samples;
                    }
                    continue;  // Parser buffered input, not decoded audio yet.
                }
                esp_audio_simple_dec_info_t info{};
                if (esp_audio_simple_dec_get_info(decoder, &info) != ESP_AUDIO_ERR_OK ||
                    info.bits_per_sample != 16 || (info.channel != 1 && info.channel != 2) ||
                    info.sample_rate < 8000 || info.sample_rate > 48000 ||
                    frame.decoded_size % (2 * info.channel)) {
                    return false;
                }
                if (have_pcm &&
                    (info.sample_rate != format.sample_rate || info.channel != format.channel)) {
                    return false;
                }
                if (!have_pcm && !open_converter(info.sample_rate, target_rate)) {
                    return false;
                }
                if (!have_pcm && state->start_ms.load())
                    ESP_LOGI("BoilonMp3", "Resume target_ms=%llu source_hz=%u output_hz=%d",
                             static_cast<unsigned long long>(state->start_ms.load()),
                             static_cast<unsigned>(info.sample_rate), target_rate);
                format = info;
                have_pcm = true;
                ++decoded_frames;
                const size_t samples = frame.decoded_size / (2 * info.channel);
                if (samples > buffers->mono.size() ||
                    (frame_mode &&
                     (samples != encoded.samples || info.sample_rate != encoded.rate ||
                      info.channel != encoded.channels))) {
                    return false;
                }
                // Discard preroll (or the entire prefix on the parser fallback)
                // without mixing/resampling samples that never reach the codec.
                // Count in the source clock, avoiding per-frame conversion rounding.
                const uint64_t skip_target = state->start_ms.load() * info.sample_rate / 1000;
                const size_t skipped = static_cast<size_t>(std::min<uint64_t>(
                    samples, source_samples < skip_target ? skip_target - source_samples : 0));
                source_samples += samples;
                if (skipped && ++skipped_frames % 256 == 0)
                    vTaskDelay(1);
                const size_t remaining = samples - skipped;
                if (!remaining)
                    continue;
                for (size_t i = 0; i < remaining; ++i) {
                    const size_t source = skipped + i;
                    buffers->mono[i] = info.channel == 1
                                           ? buffers->decoded[source]
                                           : (static_cast<int32_t>(buffers->decoded[2 * source]) +
                                              buffers->decoded[2 * source + 1]) /
                                                 2;
                }
                stage = "output";
                if (!emit(remaining)) {
                    return false;
                }
                stage = "decode";
            }
        }
        return false;
    };
    success = decode_file();
    // P5-5: the card failed (open or read), not the file's content.
    const bool read_failed = open_failed || (file && std::ferror(file));
    if (converter) {
        esp_ae_rate_cvt_close(converter);
    }
    if (decoder) {
        esp_audio_simple_dec_close(decoder);
    }
    if (file) {
        std::fclose(file);
    }
    // Natural EOF must drain submitted PCM; cancellation never waits on the codec.
    while (success && live() && !state->audio.IsPlaybackIdle()) {
        YieldWorker();
    }
    const bool stopped = !live();
    if (!success && !stopped)
        ESP_LOGW("BoilonMp3", "Decode failed stage=%s read_error=%d", stage, read_failed);
    const auto completed = state->audio.LocalPlaybackSamples(session);
    const int output_rate = state->audio.LocalOutputSampleRate();
    if (completed && output_rate > 0)
        state->position_ms = state->start_ms.load() + completed * 1000 / output_rate;
    state->audio.EndLocalPlayback(session);
    state->read_failed = !success && !stopped && read_failed;
    state->result = stopped ? Result::kStopped : (success ? Result::kFinished : Result::kError);
    state->running = false;
}
