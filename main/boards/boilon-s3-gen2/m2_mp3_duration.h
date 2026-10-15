#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace boilon::local_library {
// Encoding-frame time, not gapless/decoded/audible time. Never infer from file
// size or average bitrate. Caller supplies exact reads from one stable file and
// performs I/O outside the player/UI owner. Step has a fixed header-read budget.
class Mp3Duration {
public:
    enum class Status { kScanning, kKnown, kUnknown };
    explicit Mp3Duration(uint64_t size) : size_(size) {}
    Status State() const { return status_; }
    uint64_t DurationMs() const { return status_ == Status::kKnown ? samples_ * 1000 / rate_ : 0; }
    // Xing/Info stores an explicit frame count. Use it for long-file checkpoints
    // without competing with playback for an entire SD scan. This is metadata
    // time, not proof that every frame decodes or that a truncated file is good.
    template <typename Read>
    uint64_t HeaderDuration(Read read) const {
        std::array<uint8_t, 12> bytes{};
        if (size_ < 10 || !read(0, bytes.data(), 10))
            return 0;
        uint64_t offset = 0;
        if (std::memcmp(bytes.data(), "ID3", 3) == 0) {
            if (bytes[3] < 2 || bytes[3] > 4 || bytes[4] == 0xff)
                return 0;
            uint64_t tag = 0;
            for (int i = 6; i < 10; ++i) {
                if (bytes[i] & 0x80)
                    return 0;
                tag = (tag << 7) | bytes[i];
            }
            offset = 10 + tag + (bytes[3] == 4 && (bytes[5] & 0x10) ? 10 : 0);
        }
        if (offset > size_ || size_ - offset < 4 || !read(offset, bytes.data(), 4))
            return 0;
        const uint32_t header = (uint32_t{bytes[0]} << 24) | (uint32_t{bytes[1]} << 16) |
                                (uint32_t{bytes[2]} << 8) | bytes[3];
        const unsigned version = (header >> 19) & 3, frequency = (header >> 10) & 3;
        const unsigned bitrate = (header >> 12) & 15;
        if ((header & 0xffe00000) != 0xffe00000 || version == 1 || ((header >> 17) & 3) != 1 ||
            !bitrate || bitrate == 15 || frequency == 3 || (header & 3) == 2)
            return 0;
        const bool mono = ((header >> 6) & 3) == 3;
        const uint64_t marker = offset + 4 + ((header & 0x10000) ? 0 : 2) +
                                (version == 3 ? (mono ? 17 : 32) : (mono ? 9 : 17));
        if (marker > size_ || size_ - marker < bytes.size() || !read(marker, bytes.data(), bytes.size()) ||
            (std::memcmp(bytes.data(), "Xing", 4) != 0 && std::memcmp(bytes.data(), "Info", 4) != 0) ||
            !(bytes[7] & 1))
            return 0;
        const uint64_t frames = (uint32_t{bytes[8]} << 24) | (uint32_t{bytes[9]} << 16) |
                                (uint32_t{bytes[10]} << 8) | bytes[11];
        static constexpr unsigned rates[] = {44100, 48000, 32000};
        const unsigned rate = rates[frequency] / (version == 3 ? 1 : version == 2 ? 2 : 4);
        const uint64_t duration = frames * (version == 3 ? 1152 : 576) * 1000 / rate;
        return frames && frames <= (size_ - offset) / 4 && duration <= 24ULL * 60 * 60 * 1000
                   ? duration : 0;
    }
    template <typename Read>
    Status Step(Read read) {
        if (status_ != Status::kScanning)
            return status_;
        std::array<uint8_t, 10> bytes{};
        if (!initialized_) {
            initialized_ = true;
            if (size_ < 4 || !read(0, bytes.data(), std::min<uint64_t>(size_, bytes.size())))
                return Unknown();
            if (std::memcmp(bytes.data(), "ID3", 3) == 0) {
                if (size_ < 10 || bytes[3] < 2 || bytes[3] > 4 || bytes[4] == 0xff)
                    return Unknown();
                uint64_t tag_size = 0;
                for (int i = 6; i < 10; ++i) {
                    if (bytes[i] & 0x80)
                        return Unknown();
                    tag_size = (tag_size << 7) | bytes[i];
                }
                offset_ = 10 + tag_size + (bytes[3] == 4 && (bytes[5] & 0x10) ? 10 : 0);
                if (offset_ > size_)
                    return Unknown();
            }
        }
        for (int budget = 0; budget < 16; ++budget) {
            if (offset_ == size_) {
                status_ = samples_ && rate_ ? Status::kKnown : Status::kUnknown;
                return status_;
            }
            if (size_ - offset_ == 128) {
                if (!read(offset_, bytes.data(), 3))
                    return Unknown();
                if (std::memcmp(bytes.data(), "TAG", 3) == 0) {
                    offset_ = size_;
                    continue;
                }
            }
            if (size_ - offset_ < 4 || !read(offset_, bytes.data(), 4))
                return Unknown();
            const uint32_t header = (uint32_t{bytes[0]} << 24) | (uint32_t{bytes[1]} << 16) |
                                    (uint32_t{bytes[2]} << 8) | bytes[3];
            const unsigned version = (header >> 19) & 3, layer = (header >> 17) & 3;
            const unsigned bitrate = (header >> 12) & 15, frequency = (header >> 10) & 3;
            if ((header & 0xffe00000) != 0xffe00000 || version == 1 || layer != 1 || bitrate == 0 ||
                bitrate == 15 || frequency == 3 || (header & 3) == 2)
                return Unknown();
            static constexpr unsigned rates[] = {44100, 48000, 32000};
            static constexpr unsigned mpeg1[] = {0,   32,  40,  48,  56,  64,  80, 96,
                                                 112, 128, 160, 192, 224, 256, 320};
            static constexpr unsigned mpeg2[] = {0,  8,  16, 24,  32,  40,  48, 56,
                                                 64, 80, 96, 112, 128, 144, 160};
            const unsigned rate = rates[frequency] / (version == 3 ? 1 : version == 2 ? 2 : 4);
            const unsigned channels = ((header >> 6) & 3) == 3 ? 1 : 2;
            if (rate_ && (rate_ != rate || version_ != version || channels_ != channels))
                return Unknown();
            rate_ = rate;
            version_ = version;
            channels_ = channels;
            const unsigned length =
                (version == 3 ? 144000 * mpeg1[bitrate] : 72000 * mpeg2[bitrate]) / rate +
                ((header >> 9) & 1);
            if (length < 4 || length > size_ - offset_)
                return Unknown();
            // Confirm the frame's last byte exists, not only its header.
            if (!read(offset_ + length - 1, bytes.data() + 4, 1))
                return Unknown();
            samples_ += version == 3 ? 1152 : 576;
            if (samples_ * 1000 / rate_ > 24ULL * 60 * 60 * 1000)
                return Unknown();
            offset_ += length;
        }
        return status_;
    }
    void Cancel() { Unknown(); }

private:
    Status Unknown() {
        status_ = Status::kUnknown;
        return status_;
    }
    uint64_t size_, offset_ = 0, samples_ = 0;
    unsigned rate_ = 0, version_ = 0, channels_ = 0;
    bool initialized_ = false;
    Status status_ = Status::kScanning;
};
}  // namespace boilon::local_library
