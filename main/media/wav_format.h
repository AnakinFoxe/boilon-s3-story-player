#ifndef MEDIA_WAV_FORMAT_H_
#define MEDIA_WAV_FORMAT_H_

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace media {

// F08: what a WAV file's header says, shared by the player and the bookmark
// worker. Integer PCM of 8, 16, 24 or 32 bits or 32-bit float, one or two
// channels, 8-48 kHz, plain or WAVE_FORMAT_EXTENSIBLE. The header is untrusted:
// every size is checked against the file, never believed.
struct WavFormat {
    uint32_t rate = 0;
    uint16_t channels = 0, bits = 0;
    bool is_float = false;
    uint64_t data_offset = 0;
    uint64_t data_bytes = 0;  // Whole frames that are actually in the file.

    uint32_t FrameBytes() const { return channels * (bits / 8u); }
    uint64_t Frames() const { return FrameBytes() ? data_bytes / FrameBytes() : 0; }
    uint64_t DurationMs() const { return rate ? Frames() * 1000 / rate : 0; }
};

inline bool IsWavName(std::string_view name) {
    if (name.size() <= 4)
        return false;
    const auto tail = name.substr(name.size() - 4);
    return tail[0] == '.' && (tail[1] | 0x20) == 'w' && (tail[2] | 0x20) == 'a' &&
           (tail[3] | 0x20) == 'v';
}

// read(offset, bytes, count) reads exactly count bytes or returns false.
template <typename Read>
bool ParseWav(uint64_t file_size, Read read, WavFormat& out) {
    auto u16 = [](const uint8_t* b) { return static_cast<uint32_t>(b[0] | b[1] << 8); };
    auto u32 = [](const uint8_t* b) {
        return uint32_t{b[0]} | uint32_t{b[1]} << 8 | uint32_t{b[2]} << 16 | uint32_t{b[3]} << 24;
    };
    std::array<uint8_t, 40> b{};
    if (file_size < 12 || !read(0, b.data(), 12) || std::memcmp(b.data(), "RIFF", 4) != 0 ||
        std::memcmp(b.data() + 8, "WAVE", 4) != 0)
        return false;
    WavFormat format;
    bool have_format = false;
    uint64_t offset = 12;
    for (int chunk = 0; chunk < 64 && file_size - offset >= 8; ++chunk) {
        if (!read(offset, b.data(), 8))
            return false;
        const uint64_t size = u32(b.data() + 4), body = offset + 8;
        if (std::memcmp(b.data(), "data", 4) == 0) {
            if (!have_format)
                return false;
            format.data_offset = body;
            // Streamed or cut-off files claim more than they hold: keep what is there.
            const uint64_t held = file_size - body < size ? file_size - body : size;
            format.data_bytes = held - held % format.FrameBytes();
            if (!format.Frames() || format.DurationMs() > 24ULL * 60 * 60 * 1000)
                return false;
            out = format;
            return true;
        }
        if (std::memcmp(b.data(), "fmt ", 4) == 0) {
            if (size < 16 || size > file_size - body ||
                !read(body, b.data(), static_cast<size_t>(size < 40 ? size : 40)))
                return false;
            uint32_t tag = u16(b.data());
            if (tag == 0xfffe) {  // WAVE_FORMAT_EXTENSIBLE: the real tag opens the subformat GUID.
                static constexpr uint8_t kGuidTail[] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80,
                                                        0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};
                if (size < 40 || std::memcmp(b.data() + 26, kGuidTail, sizeof(kGuidTail)) != 0)
                    return false;
                tag = u16(b.data() + 24);
            }
            format.channels = static_cast<uint16_t>(u16(b.data() + 2));
            format.rate = u32(b.data() + 4);
            format.bits = static_cast<uint16_t>(u16(b.data() + 14));
            format.is_float = tag == 3;
            const bool pcm = tag == 1 && (format.bits == 8 || format.bits == 16 ||
                                          format.bits == 24 || format.bits == 32);
            if (!(pcm || (format.is_float && format.bits == 32)) || format.channels < 1 ||
                format.channels > 2 || format.rate < 8000 || format.rate > 48000 ||
                u16(b.data() + 12) != format.FrameBytes())
                return false;
            have_format = true;
        }
        // Chunks are word aligned: an odd size has one pad byte.
        const uint64_t next = size + (size & 1);
        if (next > file_size - body)
            return false;
        offset = body + next;
    }
    return false;
}

}  // namespace media

#endif  // MEDIA_WAV_FORMAT_H_
