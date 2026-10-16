#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>
#include "p5_1_ui_canvas.h"

// Layout A's title (2026-10-16): the biggest size from 28 px (the old 2x) down
// to 18 px that shows it whole, else 18 px with an ellipsis. A size is the
// advance of a Chinese character; every 14 px measure scales by size/14,
// rounded. A port of the "fitted title" part of
// docs/boilon/p5_1_prototype/proto.js; integers only, so test_p5_1_ui can
// compare every pixel with the prototype.
namespace boilon::ui::title {
constexpr int kWidth = 182, kHeight = 108, kMax = 28, kMin = 18, kBold = 21;
// Cells of one glyph along an axis; the device font needs at most 18.
constexpr int kMaxCells = 24;

// v * size / 14, rounded half up, also below zero.
inline int Scale(int v, int size) {
    const int t = v * size + 7;
    return t >= 0 ? t / 14 : -((13 - t) / 14);
}
inline int Pitch(int size) { return std::max(size + 4, 20); }
inline int Lines(int size) {
    int n = 1;
    while (n * Pitch(size) + size + 2 <= kHeight)
        ++n;
    return n;
}
inline int Advance(const Font& font, uint32_t c, int size) {
    return Scale(Lookup(font, c).adv, size);
}
inline int Width(const Font& font, const std::vector<uint32_t>& line, int size) {
    int width = 0;
    for (const uint32_t c : line)
        width += Advance(font, c, size);
    return width;
}

// No line starts with a closing mark or ends with an opening one, and ASCII
// letters and digits stay together; spaces where a line breaks are dropped.
constexpr char32_t kNoStart[] = U"，。、；：！？）》」』】〉〕”’…·,.;:!?)]}%";
constexpr char32_t kNoEnd[] = U"（《「『【〈〔“‘([{";
constexpr char32_t kTrim[] = U" ，、。；：,.;:（《「『【〈〔“‘([{";  // dropped before an ellipsis
inline bool OneOf(uint32_t c, const char32_t* list) {
    for (; *list; ++list)
        if (*list == c)
            return true;
    return false;
}
inline bool Word(uint32_t c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           c == '\'' || c == '_';
}
inline bool CanBreak(uint32_t a, uint32_t b) {
    return !OneOf(b, kNoStart) && !OneOf(a, kNoEnd) && !(Word(a) && Word(b));
}
inline void TrimEnd(std::vector<uint32_t>& line) {
    while (line.size() > 1 && line.back() == ' ')
        line.pop_back();
}
inline std::vector<uint32_t> Tail(const std::vector<uint32_t>& line, size_t from) {
    return std::vector<uint32_t>(line.begin() + static_cast<std::ptrdiff_t>(from), line.end());
}
inline void TrimStart(std::vector<uint32_t>& line) {
    line.erase(line.begin(),
               std::find_if(line.begin(), line.end(), [](uint32_t c) { return c != ' '; }));
}

using TextLines = std::vector<std::vector<uint32_t>>;
// A character that does not fit starts a new line, with whatever must stay
// with it. Past `lines` lines the last one ends with an ellipsis and this
// returns false; a last line of one character takes the previous line's last
// character or word.
inline bool Wrap(const Font& font, const std::vector<uint32_t>& text, int size, int lines,
                 TextLines& out) {
    out.clear();
    std::vector<uint32_t> line;
    int width = 0;
    for (const uint32_t c : text) {
        const int advance = Advance(font, c, size);
        if (!line.empty() && width + advance > kWidth) {
            size_t at = line.size();
            if (!CanBreak(line.back(), c)) {
                --at;
                while (at > 0 && !CanBreak(line[at - 1], line[at]))
                    --at;
                if (at == 0 || Width(font, Tail(line, at), size) + advance > kWidth)
                    at = line.size();
            }
            std::vector<uint32_t> rest = Tail(line, at);
            line.resize(at);
            TrimEnd(line);
            out.push_back(std::move(line));
            TrimStart(rest);
            line = std::move(rest);
            width = Width(font, line, size);
            if (line.empty() && c == ' ')
                continue;
        }
        line.push_back(c);
        width += advance;
    }
    if (!line.empty())
        out.push_back(std::move(line));
    if (static_cast<int>(out.size()) > lines) {
        out.resize(lines);
        auto& last = out.back();
        const int ellipsis = Advance(font, 0x2026, size);
        while (!last.empty() &&
               (Width(font, last, size) + ellipsis > kWidth || OneOf(last.back(), kTrim)))
            last.pop_back();
        last.push_back(0x2026);
        return false;
    }
    const size_t n = out.size();
    if (n > 1 && out[n - 1].size() == 1) {
        auto& prev = out[n - 2];
        size_t k = prev.size() - 1;
        while (k > 0 && !CanBreak(prev[k - 1], prev[k]))
            --k;
        std::vector<uint32_t> moved = Tail(prev, k);
        moved.push_back(out[n - 1][0]);
        TrimStart(moved);
        if (k > 0 && Width(font, moved, size) <= kWidth) {
            prev.resize(k);
            TrimEnd(prev);
            out[n - 1] = std::move(moved);
        }
    }
    return true;
}

struct Fit {
    int size = kMax;
    TextLines lines;
};
inline Fit FitTitle(const Font& font, const std::string& text) {
    const std::vector<uint32_t> chars = Codepoints(text);
    Fit fit;
    while (!Wrap(font, chars, fit.size, Lines(fit.size), fit.lines) && fit.size > kMin)
        --fit.size;
    return fit;
}

// Which of n source pixels draw 2 px wide so that together they cover m
// (n <= m <= 2n): the cheapest set, every boundary less than 1 px from its
// exact place and boundary `pin` at its nearest. start gets where each source
// pixel begins (n + 1 values).
inline void Stretch(const int* cost, int n, int m, int pin, int* start) {
    constexpr int kInf = 1 << 30;
    const int d = m - n;
    int best[kMaxCells + 1], next[kMaxCells + 1];
    uint32_t doubled[kMaxCells] =
        {};  // bit k: the cheapest way to k doubled pixels doubles this one
    std::fill(best, best + d + 1, kInf);
    best[0] = 0;
    for (int c = 0; c < n; ++c) {
        std::fill(next, next + d + 1, kInf);
        for (int k = 0; k <= d; ++k)
            for (int two = 0; two < 2 && best[k] < kInf && k + two <= d; ++two) {
                const int off = std::abs(n * (c + 1 + k + two) - (c + 1) * m);
                if (c + 1 == pin ? 2 * off > n : off >= n)
                    continue;
                const int value = best[k] + two * cost[c] * 1000 + off;
                if (value < next[k + two]) {
                    next[k + two] = value;
                    doubled[c] = (doubled[c] & ~(1u << (k + two))) |
                                 (static_cast<uint32_t>(two) << (k + two));
                }
            }
        std::copy(next, next + d + 1, best);
    }
    start[n] = m;
    for (int c = n - 1, k = d; c >= 0; --c) {
        const int two = (doubled[c] >> k) & 1;
        start[c] = start[c + 1] - 1 - two;
        k -= two;
    }
}

// A 14 px glyph at `size`: some columns and rows draw 2 px wide. From kBold up
// the ones a stroke runs along go first (2 px strokes, like the old 2x); below
// it the empty ones (1 px strokes, like 1x). The baseline row stays where the
// line puts it.
inline void DrawGlyph(Canvas& canvas, const Font& font, const Glyph& g, int x, int top, int size,
                      uint8_t color) {
    const int y0 = font.LineHeight() - font.BaseLine() - g.h - g.y;
    const int c0 = std::min(0, g.x), c1 = std::max(g.adv, g.x + g.w);
    const int r0 = std::min(0, y0), r1 = std::max(font.LineHeight(), y0 + g.h);
    const int nx = c1 - c0, ny = r1 - r0;
    if (nx > kMaxCells || ny > kMaxCells)
        return;
    uint32_t rows[kMaxCells + 2] = {};  // bit c - c0 of rows[r - r0 + 1]; empty rows around
    for (int r = 0; r < g.h; ++r)
        for (int col = 0; col < g.w; ++col) {
            const int bit = r * g.w + col;
            if (g.bits[bit / 8] & (0x80 >> (bit % 8)))
                rows[y0 + r - r0 + 1] |= 1u << (g.x + col - c0);
        }
    // Ink pixels without ink on both sides along an axis: a stroke runs across it.
    int cost_x[kMaxCells] = {}, cost_y[kMaxCells] = {};
    for (int r = 1; r <= ny; ++r) {
        const uint32_t across = rows[r] & ~(rows[r] << 1) & ~(rows[r] >> 1);
        for (int c = 0; c < nx; ++c)
            cost_x[c] += (across >> c) & 1;
        cost_y[r - 1] = __builtin_popcount(rows[r] & ~rows[r - 1] & ~rows[r + 1]);
    }
    if (size >= kBold)
        for (const auto& axis : {std::make_pair(cost_x, nx), std::make_pair(cost_y, ny)}) {
            const int most = *std::max_element(axis.first, axis.first + axis.second);
            for (int i = 0; i < axis.second; ++i)
                axis.first[i] = most - axis.first[i];
        }
    int sx[kMaxCells + 1], sy[kMaxCells + 1];
    Stretch(cost_x, nx, Scale(c1, size) - Scale(c0, size), -1, sx);
    Stretch(cost_y, ny, Scale(r1, size) - Scale(r0, size), font.LineHeight() - font.BaseLine() - r0,
            sy);
    const int ox = x + Scale(c0, size), oy = top + Scale(r0, size);
    for (int r = 0; r < ny; ++r)
        for (int c = 0; c < nx; ++c)
            if ((rows[r + 1] >> c) & 1)
                canvas.Rect(ox + sx[c], oy + sy[r], sx[c + 1] - sx[c], sy[r + 1] - sy[r], color);
}

// The fitted title with its block's top at y.
inline void DrawLines(Canvas& canvas, const Font& font, const Fit& fit, int x, int y,
                      uint8_t color) {
    for (size_t i = 0; i < fit.lines.size(); ++i) {
        int cx = x;
        for (const uint32_t c : fit.lines[i]) {
            DrawGlyph(canvas, font, Lookup(font, c), cx, y + static_cast<int>(i) * Pitch(fit.size),
                      fit.size, color);
            cx += Advance(font, c, fit.size);
        }
    }
}
}  // namespace boilon::ui::title
