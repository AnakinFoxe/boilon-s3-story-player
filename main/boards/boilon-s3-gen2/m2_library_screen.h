#pragma once
#include <cstdint>
#include <string>

namespace boilon::local_library {
// Invalid UTF-8 consumes one byte; truncation never reads beyond the string.
inline uint32_t NextCodepoint(const std::string& text, size_t& offset) {
    if (offset >= text.size())
        return 0;
    const auto first = static_cast<unsigned char>(text[offset++]);
    if (first < 0x80)
        return first;
    const int extra = first >= 0xc2 && first <= 0xdf   ? 1
                      : first >= 0xe0 && first <= 0xef ? 2
                      : first >= 0xf0 && first <= 0xf4 ? 3
                                                       : 0;
    if (!extra || text.size() - offset < static_cast<size_t>(extra))
        return '?';
    uint32_t value = first & (0x7f >> (extra + 1));
    for (int i = 0; i < extra; ++i) {
        const auto byte = static_cast<unsigned char>(text[offset + i]);
        if ((byte & 0xc0) != 0x80)
            return '?';
        value = (value << 6) | (byte & 0x3f);
    }
    if (value < (extra == 1   ? 0x80u
                 : extra == 2 ? 0x800u
                              : 0x10000u) ||
        value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
        return '?';
    offset += extra;
    return value;
}

}  // namespace boilon::local_library
