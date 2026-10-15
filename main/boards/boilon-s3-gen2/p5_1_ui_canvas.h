#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "m2_library_screen.h"
#include "p5_1_ui_font.h"

// P5-1: the 240x296 screen as 8-bit color indices, and the drawing the pages
// need. A port of docs/boilon/p5_1_prototype/proto.js; no ESP-IDF headers, so
// test_p5_1_ui renders this exact code on the host and compares every pixel
// with the prototype.
namespace boilon::ui {
constexpr int kWidth = 240, kHeight = 296;

struct Glyph {
    int adv = 0, w = 0, h = 0, x = 0, y = 0;
    const uint8_t* bits = nullptr;  // w*h bits, MSB first, rows not padded
};

// The xiaozhi-fonts CBIN: a 32-bit little-endian dump of LVGL's lv_font_t and
// lv_font_fmt_txt_dsc_t with offsets in place of pointers, built with
// LV_FONT_FMT_TXT_LARGE (16-byte glyph descriptors), 1 bpp, plain bitmaps.
// Glyph lookup follows LVGL's lv_font_get_glyph_dsc_fmt_txt without kerning.
class CbinFont {
public:
    explicit CbinFont(const uint8_t* bin) : bin_(bin) {
        line_height = static_cast<int>(U32(12));
        base_line = static_cast<int>(U32(16));
        base_ = U32(24);
        bitmaps_ = base_ + U32(base_);
        glyphs_ = base_ + U32(base_ + 4);
        cmaps_ = base_ + U32(base_ + 8);
        const uint16_t bits = U16(base_ + 18);
        cmap_count_ = bits & 0x1ff;
        valid_ = ((bits >> 9) & 0xf) == 1 && ((bits >> 14) & 3) == 0;
    }
    bool Valid() const { return valid_; }
    bool Get(uint32_t codepoint, Glyph& out) const {
        uint32_t id = 0;
        for (uint32_t i = 0; i < cmap_count_ && !id; ++i)
            id = GlyphId(cmaps_ + 20 * i, codepoint);
        if (!id)
            return false;
        const uint32_t at = glyphs_ + 16 * id;
        out.adv = static_cast<int>((U32(at + 4) + 8) >> 4);
        out.w = U16(at + 8);
        out.h = U16(at + 10);
        out.x = static_cast<int16_t>(U16(at + 12));
        out.y = static_cast<int16_t>(U16(at + 14));
        out.bits = bin_ + bitmaps_ + U32(at);
        return true;
    }
    int line_height = 0, base_line = 0;

private:
    uint32_t U32(uint32_t at) const {
        uint32_t value;
        std::memcpy(&value, bin_ + at, 4);
        return value;
    }
    uint16_t U16(uint32_t at) const {
        uint16_t value;
        std::memcpy(&value, bin_ + at, 2);
        return value;
    }
    uint32_t GlyphId(uint32_t cmap, uint32_t codepoint) const {
        const uint32_t start = U32(cmap), length = U16(cmap + 4), first = U16(cmap + 6);
        const uint32_t unicodes = cmaps_ + U32(cmap + 8), offsets = cmaps_ + U32(cmap + 12);
        const uint32_t count = U16(cmap + 16), type = bin_[cmap + 18];
        if (codepoint < start || codepoint - start >= length)
            return 0;
        const uint32_t relative = codepoint - start;
        if (type == 2)  // FORMAT0_TINY
            return first + relative;
        if (type == 0)  // FORMAT0_FULL
            return first + bin_[offsets + relative];
        uint32_t low = 0, high = count;  // SPARSE: sorted 16-bit offsets
        while (low < high) {
            const uint32_t middle = (low + high) / 2;
            const uint32_t value = U16(unicodes + 2 * middle);
            if (value == relative)
                return first + (type == 3 ? middle : U16(offsets + 2 * middle));
            if (value < relative)
                low = middle + 1;
            else
                high = middle;
        }
        return 0;
    }
    const uint8_t* bin_;
    uint32_t base_ = 0, bitmaps_ = 0, glyphs_ = 0, cmaps_ = 0, cmap_count_ = 0;
    bool valid_ = false;
};

// The two faces: Noto (CBIN above) for Chinese, unscii-8 for pixel digits.
struct Font {
    const CbinFont* cbin = nullptr;  // nullptr: unscii-8
    int LineHeight() const { return cbin ? cbin->line_height : unscii::kLineHeight; }
    int BaseLine() const { return cbin ? cbin->base_line : unscii::kBaseLine; }
    bool Get(uint32_t codepoint, Glyph& out) const {
        if (cbin)
            return cbin->Get(codepoint, out);
        if (codepoint < 32 || codepoint > 126)
            return false;
        const auto& box = unscii::kBoxes[codepoint - 32];
        out = {box.adv, box.w, box.h, box.x, box.y, unscii::kBits + box.offset};
        return true;
    }
};

// Missing Chinese characters draw as an empty 10x12 box; missing pixel-font
// characters as its '?'.
inline Glyph Lookup(const Font& font, uint32_t codepoint) {
    static constexpr uint8_t kTofu[] = {0xff, 0xe0, 0x18, 0x06, 0x01, 0x80, 0x60, 0x18,
                                        0x06, 0x01, 0x80, 0x60, 0x18, 0x07, 0xff};
    Glyph glyph;
    if (font.Get(codepoint, glyph))
        return glyph;
    if (font.cbin)
        return {14, 10, 12, 2, 0, kTofu};
    font.Get('?', glyph);
    return glyph;
}

enum class Align : uint8_t { kLeft, kCenter, kRight };
struct TextStyle {
    const Font* font = nullptr;
    uint8_t color = 0;
    int scale = 1, max_width = 1000000000, lines = 1, line_pitch = 0;
    Align align = Align::kLeft;
};

inline std::vector<uint32_t> Codepoints(const std::string& text) {
    std::vector<uint32_t> result;
    for (size_t offset = 0; offset < text.size();)
        result.push_back(local_library::NextCodepoint(text, offset));
    return result;
}
inline int Measure(const std::vector<uint32_t>& text, const Font& font, int scale) {
    int width = 0;
    for (const uint32_t c : text)
        width += Lookup(font, c).adv * scale;
    return width;
}
// Breaks at any character; the last allowed line ends with an ellipsis.
inline std::vector<std::vector<uint32_t>> Wrap(const std::string& text, const TextStyle& style) {
    std::vector<std::vector<uint32_t>> lines;
    std::vector<uint32_t> line;
    int width = 0;
    for (const uint32_t c : Codepoints(text)) {
        const int advance = Lookup(*style.font, c).adv * style.scale;
        if (!line.empty() && width + advance > style.max_width) {
            lines.push_back(std::move(line));
            line.clear();
            width = 0;
        }
        line.push_back(c);
        width += advance;
    }
    if (!line.empty())
        lines.push_back(std::move(line));
    if (static_cast<int>(lines.size()) > style.lines) {
        lines.resize(style.lines);
        auto& last = lines.back();
        const int ellipsis = Lookup(*style.font, 0x2026).adv * style.scale;
        while (!last.empty() &&
               Measure(last, *style.font, style.scale) + ellipsis > style.max_width)
            last.pop_back();
        last.push_back(0x2026);
    }
    return lines;
}

// A changed part of the screen: columns [x0, x1) of rows [y0, y1).
struct Band {
    int x0 = 0, x1 = 0, y0 = 0, y1 = 0;
};
// Groups the rows that differ into at most max_bands bands; changed rows at
// most 8 rows apart share a band, so an animation and the clock stay separate.
inline int ChangedBands(const uint8_t* before, const uint8_t* after, Band* bands, int max_bands) {
    int count = 0;
    for (int y = 0; y < kHeight; ++y) {
        const uint8_t* a = before + y * kWidth;
        const uint8_t* b = after + y * kWidth;
        if (std::memcmp(a, b, kWidth) == 0)
            continue;
        int left = 0, right = kWidth;
        while (a[left] == b[left])
            ++left;
        while (a[right - 1] == b[right - 1])
            --right;
        if (count && (y - bands[count - 1].y1 <= 8 || count == max_bands)) {
            Band& band = bands[count - 1];
            band.x0 = std::min(band.x0, left);
            band.x1 = std::max(band.x1, right);
            band.y1 = y + 1;
        } else {
            bands[count++] = {left, right, y, y + 1};
        }
    }
    return count;
}

class Canvas {
public:
    explicit Canvas(uint8_t* pixels) : pixels_(pixels) {}
    uint8_t* Pixels() { return pixels_; }
    void Clear(uint8_t color) { std::memset(pixels_, color, kWidth * kHeight); }
    void Dot(int x, int y, uint8_t color) {
        if (x >= 0 && y >= 0 && x < kWidth && y < kHeight)
            pixels_[y * kWidth + x] = color;
    }
    void Rect(int x, int y, int w, int h, uint8_t color) {
        const int x0 = std::max(0, x), y0 = std::max(0, y);
        const int x1 = std::min(kWidth, x + w), y1 = std::min(kHeight, y + h);
        if (x1 <= x0)
            return;
        for (int row = y0; row < y1; ++row)
            std::memset(pixels_ + row * kWidth + x0, color, x1 - x0);
    }
    // A rectangle with its r x r corners left alone: one-step pixel rounding.
    void Round(int x, int y, int w, int h, uint8_t color, int r = 2) {
        Rect(x + r, y, w - 2 * r, h, color);
        Rect(x, y + r, r, h - 2 * r, color);
        Rect(x + w - r, y + r, r, h - 2 * r, color);
    }
    // 2 px border and a hard shadow 2 px to the lower right.
    void Panel(int x, int y, int w, int h, uint8_t fill, uint8_t line, uint8_t shadow) {
        Round(x + 2, y + 2, w, h, shadow);
        Round(x, y, w, h, line);
        Rect(x + 2, y + 2, w - 4, h - 4, fill);
    }
    void Dash(int x, int y, int w, uint8_t color) {
        for (int i = 0; i < w; i += 4)
            Rect(x + i, y, std::min(2, w - i), 2, color);
    }
    // rows[r][c] picks a color through map; '.' is transparent.
    template <typename Map>
    void Sprite(const char* const* rows, int count, Map map, int x, int y, int scale) {
        for (int r = 0; r < count; ++r)
            for (int c = 0; rows[r][c]; ++c)
                if (rows[r][c] != '.' && rows[r][c] != ' ')
                    Rect(x + c * scale, y + r * scale, scale, scale, map(rows[r][c]));
    }
    // Rows start at top + (line_height - base_line - box_h - ofs_y), as LVGL draws them.
    int Text(const std::string& text, int x, int y, const TextStyle& style) {
        const Font& font = *style.font;
        const int s = style.scale;
        const int pitch = style.line_pitch ? style.line_pitch
                          : font.cbin       ? (s == 1 ? 20 : 30)
                                            : 9 * s + 4;
        const auto lines = Wrap(text, style);
        for (size_t index = 0; index < lines.size(); ++index) {
            const int width = Measure(lines[index], font, s);
            int cx = style.align == Align::kCenter  ? x - width / 2
                     : style.align == Align::kRight ? x - width
                                                    : x;
            const int top = y + static_cast<int>(index) * pitch;
            for (const uint32_t c : lines[index]) {
                const Glyph g = Lookup(font, c);
                const int oy = top + (font.LineHeight() - font.BaseLine() - g.h - g.y) * s;
                for (int r = 0; r < g.h; ++r)
                    for (int col = 0; col < g.w; ++col) {
                        const int bit = r * g.w + col;
                        if (g.bits[bit / 8] & (0x80 >> (bit % 8)))
                            Rect(cx + (g.x + col) * s, oy + r * s, s, s, style.color);
                    }
                cx += g.adv * s;
            }
        }
        return static_cast<int>(lines.size());
    }

private:
    uint8_t* pixels_;
};
}  // namespace boilon::ui
