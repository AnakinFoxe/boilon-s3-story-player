#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <utility>
#include "p5_1_ui_canvas.h"

// P5-1: colors, icons and 喵喵, an original orange shorthair. The rows were
// printed by docs/boilon/p5_1_prototype/export_art.js from the prototype.
namespace boilon::ui {
// Pages draw with these indices; the palette decides the actual colors.
enum Color : uint8_t {
    kBg, kPanel, kLine, kShadow, kText, kSub, kFaint, kAmber, kGreen, kRed, kBlue,
    kAiBg, kAiPanel, kAiLine, kAiShadow, kAiText, kAiSub, kAiGlow, kAiAmber, kAiGreen, kAiRed,
    kAiFaint,
    kCatK, kCatW, kCatO, kCatM, kCatP, kCatC, kCatE, kCatH, kCatB,
    kColorCount
};
constexpr uint32_t kPaperRgb[kColorCount] = {
    0xefdcb9, 0xfcf4e1, 0x1f2244, 0x1f2244, 0x1f2244, 0x7c6a4f, 0xd9c39b, 0xd98c16,
    0x33925a, 0xd24332, 0x2f78c2, 0x4b4cb8, 0x3b3c9a, 0x1c1d5a, 0x1c1d5a, 0xfff0d4,
    0xd3d0ff, 0xc2beff, 0xffc04d, 0x8fe09d, 0xff806f, 0x6d6ed0, 0x4a2a1a, 0xf7a84e,
    0xd8702a, 0xffe9cb, 0xf49aa7, 0xff8f86, 0x3a2014, 0xffffff, 0x79c3f5,
};
constexpr uint32_t kNightRgb[kColorCount] = {
    0x171a38, 0x23285a, 0x0b0d22, 0x0b0d22, 0xf7e8c9, 0xa3a2cb, 0x363c74, 0xf4b23e,
    0x83d491, 0xf2705f, 0x74b8f2, 0x2c2e86, 0x212364, 0x11123c, 0x11123c, 0xfff0d4,
    0xbdb9f4, 0x9c97ff, 0xf4b23e, 0x83d491, 0xf2705f, 0x4a4db0, 0x4a2a1a, 0xf7a84e,
    0xd8702a, 0xffe9cb, 0xf49aa7, 0xff8f86, 0x3a2014, 0xffffff, 0x79c3f5,
};
// Same rounding as the prototype: each channel to the nearest 5/6/5-bit step.
constexpr uint16_t Rgb565(uint32_t rgb) {
    const uint32_t r = rgb >> 16 & 255, g = rgb >> 8 & 255, b = rgb & 255;
    return static_cast<uint16_t>((r * 62 + 255) / 510 << 11 | (g * 126 + 255) / 510 << 5 |
                                 (b * 62 + 255) / 510);
}

struct Icon {
    const char* const* rows;
    int count;
};
#define BOILON_UI_ICON(name, ...)                                   \
    constexpr const char* name##Rows[] = {__VA_ARGS__};             \
    constexpr Icon name = {name##Rows, static_cast<int>(sizeof(name##Rows) / sizeof(name##Rows[0]))};
BOILON_UI_ICON(kPlay, "#......", "###....", "#####..", "#######", "#####..", "###....", "#......")
BOILON_UI_ICON(kPause, "##..##", "##..##", "##..##", "##..##", "##..##", "##..##", "##..##")
BOILON_UI_ICON(kHourglass, "#######", "#.....#", ".#...#.", "..#.#..", ".#.#.#.", "#.###.#",
               "#######")
BOILON_UI_ICON(kDone, "......#", ".....##", "#...##.", "##.##..", ".###...", "..#....")
BOILON_UI_ICON(kWarn, "....#....", "...#.#...", "...#.#...", "..#.#.#..", "..#.#.#..", ".#.....#.",
               ".#..#..#.", "#########")
BOILON_UI_ICON(kFolder, "###......", "#########", "#.......#", "#.......#", "#.......#",
               "#########")
BOILON_UI_ICON(kSpeaker, "...#.....", "..##...#.", "####.#..#", "####.#..#", "####.#..#",
               "..##...#.", "...#.....")
BOILON_UI_ICON(kMute, "...#.....", "..##.....", "####.#.#.", "####..#..", "####.#.#.", "..##.....",
               "...#.....")
BOILON_UI_ICON(kWifi, ".#######.", "#.......#", "..#####..", ".#.....#.", "...###...", ".........",
               "....#....")
BOILON_UI_ICON(kSd, "...######", "..#.#.#.#", ".#..#.#.#", "#...#.#.#", "#.......#", "#.......#",
               "#.......#", "#.......#", "#.......#", "#########")
BOILON_UI_ICON(kBox, "#.......#", "##.....##", "#.#####.#", "#.......#", "#.......#", "#.......#",
               "#########")
BOILON_UI_ICON(kMoon, "...####..", ".###.....", ".##......", "##.......", "##.......", "##.......",
               ".##......", ".###.....", "...####..")
BOILON_UI_ICON(kForward, "#....#.....", "##...##....", "###..###...", "####.####..", "###..###...",
               "##...##....", "#....#.....")
BOILON_UI_ICON(kBack, ".....#....#", "....##...##", "...###..###", "..####.####", "...###..###",
               "....##...##", ".....#....#")
BOILON_UI_ICON(kNote, "..###", "..#.#", "..#..", "..#..", "###..", "###..")
BOILON_UI_ICON(kStar, ".#.", "###", ".#.")
// Play modes (2026-10-15): loop, single and shuffle in the prototype.
BOILON_UI_ICON(kRepeat, "......#..", "########.", "#.....#..", "#.......#", "..#.....#",
               ".########", "..#......")
BOILON_UI_ICON(kRepeatOne, "......#....", "########..#", "#.....#..##", "#.......#.#",
               "..#.....#.#", ".########.#", "..#........")
BOILON_UI_ICON(kShuffle, ".......#.", "##...####", "..#.#..#.", "...#.....", "..#.#..#.",
               "##...####", ".......#.")
#undef BOILON_UI_ICON

inline int IconWidth(const Icon& icon) { return static_cast<int>(std::strlen(icon.rows[0])); }
inline void DrawIcon(Canvas& canvas, const Icon& icon, int x, int y, int scale, uint8_t color) {
    canvas.Sprite(icon.rows, icon.count, [color](char) { return color; }, x, y, scale);
}

// 喵喵 without eyes (the eyes change with the state) and the 11x8 badge.
constexpr int kCatWidth = 41, kCatHeight = 46;
constexpr const char* kCatRows[] = {
    ".........................................",
    ".........................................",
    ".........................................",
    ".........................................",
    ".........kk...................kk.........",
    "........kwwk.................kwwk........",
    ".......kwwwwk...............kwwwwk.......",
    ".......kwwwwwkk...........kkwwwwwk.......",
    ".......kwwwwwwwkkkkkkkkkkkwwwwwwwk.......",
    "......kwwppwwwwwwwwwwwwwwwwwwwppwwk......",
    "......kwwpppwwwwwwwwowwwwwwwwpppwwk......",
    "......kwwpwwwwwwwowwowwowwwwwwwpwwk......",
    "......kwpwwwwwwwwowwowwowwwwwwwwpwk......",
    ".....kwwwwwwwwwwwwwwwwwwwwwwwwwwwwwk.....",
    ".....kwwwwwwwwwwwwwwwwwwwwwwwwwwwwwk.....",
    ".....kwwwwwwwwwwwwwwwwwwwwwwwwwwwwwk.....",
    "....kwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwk....",
    "....kwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwk....",
    "....kwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwk....",
    "...kwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwk...",
    "...kwooowwwwwwwwwwwwwwwwwwwwwwwwwooowk...",
    "...kwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwk...",
    "...kwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwk...",
    "kkkkwwwwwccwwwwwwwwwwwwwwwwwwwccwwwwwkkkk",
    "...kwwwwwwwwwwwwwwwpppwwwwwwwwwwwwwwwk...",
    "...kkwwwwwwwwwwwmmmmpmmmmwwwwwwwwwwwkk...",
    ".kk.kwwwwwwwwwwmmkmmkmmkmmwwwwwwwwwwk.kk.",
    "....kwwwwwwwwwwmmmkkmkkmmmwwwwwwwwwwk....",
    ".....kwwwwwwwwwmmmmmmmmmmmwwwwwwwwwk.....",
    "......kwwwwwwwwmmmmmmmmmmmwwwwwwwwk......",
    "......kwwwwwwwwwwmmmmmmmwwwwwwwwwwk......",
    ".......kkwwwwwwwwwwwwwwwwwwwwwwwkk.......",
    ".........kwwwwwwwwwwwwwwwwwwwwwk....kkk..",
    ".........kkkwwwwwwwwwwwwwwwwwkkk...kwwwk.",
    "........kwwwkkkwwwwwwwwwwwkkkwwok..kwwwk.",
    "........kowwwwwkkkkkkkkkkkwwwwwwk..kwwwwk",
    ".......kwwwwwwwmmmmmmmmmmmwwwwwwwk..kwwwk",
    ".......kowwwwwwwmmmmmmmmmwwwwwwowk..koowk",
    ".......kwowwwwwwwmmmmmmmwwwwwwowwk..kwwwk",
    ".......kwwwwwwwwwwmmmmmwwwwwwwwwwk..kwwwk",
    "........kwwwwwwwwwwwwwwwwwwwwwwwkkkkwwwwk",
    "........kwwwwwwwwwwwkwwwwwwwwwwwwwwwoowwk",
    ".........kwwwwwwwwwwkwwwwwwwwwwwwowwwwwk.",
    "..........kwwwkwkwwwkwwkwkwwwwwwwowwwkk..",
    "...........kkwwwwwwwwwwwwwwwkkwwwwwwk....",
    ".............kkkkkkkwkkkkkkk..kkkkkk.....",
};
constexpr const char* kBadgeRows[] = {
    ".k.......k.", "kwk.....kwk", "kwwkkkkkwwk", "kwwwwwwwwwk",
    "kwwewwwewwk", "kwwwwpwwwwk", ".kwwwwwwwk.", "..kkkkkkk..",
};
// The ears that move, as patches over kCatRows: '.' keeps the pixel and '_'
// shows the background. The right ear twitches at grid (26, 4); both droop at
// grid (2, 4).
constexpr const char* kTwitchRows[] = {
    "....__....", "...____...", "..___kk...", "___kk..wk.", "kkk....wk.",
    "....ww..wk", "...ww.p.wk", "......p.wk", "........wk",
};
constexpr const char* kDroopRows[] = {
    ".......__...................__.......", "......____.................____......",
    ".....______...............______.....", ".....________...........________.....",
    ".....________...........________.....", "....______kkk...........kkk______....",
    ".kkk.kkkkk.................kkkkk.kkk.", "kwwww..w.....................w..wwwwk",
    "kwwww.w.......................w.wwwwk", ".kwwp...........................pwwk.",
    ".kwwp...........................pwwk.", "..kw.............................wk..",
};
inline uint8_t CatColor(char key) {
    switch (key) {
        case 'k': return kCatK;
        case 'o': return kCatO;
        case 'm': return kCatM;
        case 'p': return kCatP;
        case 'c': return kCatC;
        case 'e': return kCatE;
        case 'h': return kCatH;
        case 'b': return kCatB;
        default: return kCatW;
    }
}

// The faces, as in the prototype's catGrid: eyes, mouths ('w' is the one in
// kCatRows) and ears.
enum class Eyes : uint8_t { kOpen, kBlink, kHappy, kSleep, kUp, kWide, kCross, kSad };
enum class Mouth : uint8_t { kW, kOpen, kRound, kFlat, kSad };
enum class Ears : uint8_t { kUp, kTwitch, kDroop };
inline void DrawCat(Canvas& canvas, int x, int y, int s, Eyes eyes, Mouth mouth = Mouth::kW,
                    Ears ears = Ears::kUp) {
    canvas.Sprite(kCatRows, kCatHeight, CatColor, x, y, s);
    // Only the AI page moves the ears, so they uncover its background.
    const auto ear = [](char key) { return key == '_' ? uint8_t{kAiBg} : CatColor(key); };
    if (ears == Ears::kTwitch)
        canvas.Sprite(kTwitchRows, 9, ear, x + 26 * s, y + 4 * s, s);
    if (ears == Ears::kDroop)
        canvas.Sprite(kDroopRows, 12, ear, x + 2 * s, y + 4 * s, s);
    auto dot = [&](int gx, int gy, uint8_t color) {
        canvas.Rect(x + gx * s, y + gy * s, s, s, color);
    };
    using Points = std::initializer_list<std::pair<int, int>>;
    auto dots = [&](Points points, uint8_t color) {
        for (const auto& p : points)
            dot(p.first, p.second, color);
    };
    // Left eye; the right one mirrors it around column 20.
    auto pair = [&](Points points, uint8_t color = kCatE) {
        for (const auto& p : points) {
            dot(p.first, p.second, color);
            dot(40 - p.first, p.second, color);
        }
    };
    switch (eyes) {
        case Eyes::kOpen:
        case Eyes::kWide:
            pair({{13, 17}, {14, 17}, {12, 18}, {13, 18}, {14, 18}, {15, 18}, {12, 19}, {13, 19},
                  {14, 19}, {15, 19}, {12, 20}, {13, 20}, {14, 20}, {15, 20}, {13, 21}, {14, 21}});
            for (const int gx : {13, 26, 14, 27})
                dot(gx, 18, kCatH);
            if (eyes == Eyes::kWide)
                dots({{15, 20}, {28, 20}}, kCatH);
            break;
        case Eyes::kBlink: pair({{12, 20}, {13, 20}, {14, 20}, {15, 20}}); break;
        case Eyes::kHappy: pair({{12, 20}, {13, 19}, {14, 19}, {15, 20}}); break;
        case Eyes::kSleep: pair({{12, 19}, {13, 20}, {14, 20}, {15, 19}}); break;
        case Eyes::kUp:
            for (const auto& p : std::initializer_list<std::pair<int, int>>{
                     {13, 16}, {14, 16}, {12, 17}, {13, 17}, {14, 17}, {15, 17}, {13, 18}, {14, 18}}) {
                dot(p.first + 1, p.second, kCatE);
                dot(p.first + 14, p.second, kCatE);
            }
            dot(14, 17, kCatH);
            dot(27, 17, kCatH);
            break;
        case Eyes::kCross:
            pair({{12, 18}, {15, 18}, {13, 19}, {14, 19}, {13, 20}, {14, 20}, {12, 21}, {15, 21}});
            break;
        case Eyes::kSad:
            pair({{13, 19}, {14, 19}, {12, 20}, {13, 20}, {14, 20}, {15, 20}, {13, 21}, {14, 21}});
            pair({{11, 17}, {12, 16}}, kCatO);  // brows
            dots({{13, 19}, {26, 19}}, kCatH);
            dots({{13, 23}, {13, 24}}, kCatB);  // a tear
            break;
    }
    if (mouth != Mouth::kW)  // back to the plain muzzle
        dots({{20, 26}, {17, 26}, {23, 26}, {18, 27}, {19, 27}, {21, 27}, {22, 27}}, kCatM);
    switch (mouth) {
        case Mouth::kW:
            break;
        case Mouth::kOpen:
            dots({{18, 26}, {19, 26}, {20, 26}, {21, 26}, {22, 26}, {18, 27}, {22, 27}}, kCatK);
            dots({{19, 28}, {20, 28}, {21, 28}}, kCatK);
            dots({{19, 27}, {20, 27}, {21, 27}}, kCatP);
            break;
        case Mouth::kRound:
            dots({{19, 26}, {20, 26}, {21, 26}, {19, 27}, {21, 27}}, kCatK);
            dots({{19, 28}, {20, 28}, {21, 28}}, kCatK);
            dots({{20, 27}}, kCatP);
            break;
        case Mouth::kFlat:
            dots({{18, 27}, {19, 27}, {20, 27}, {21, 27}, {22, 27}}, kCatK);
            break;
        case Mouth::kSad:
            dots({{18, 28}, {19, 27}, {20, 27}, {21, 27}, {22, 28}}, kCatK);
            break;
    }
}
// Drawn before the cat, which then covers parts of the band and the cups.
inline void DrawHeadphones(Canvas& canvas, int x, int y, int s, uint8_t color) {
    for (int gy = 0; gy < 22; ++gy)
        for (int gx = 0; gx < kCatWidth; ++gx) {
            const double dx = (gx - 20) / 18.2, dy = (gy - 22) / 18.6;
            const double d = dx * dx + dy * dy;
            if (d <= 1 && d >= 0.84)
                canvas.Rect(x + gx * s, y + gy * s, s, s, color);
        }
    for (const int gx : {0, 37}) {
        canvas.Rect(x + gx * s, y + 17 * s, 4 * s, 9 * s, kCatK);
        canvas.Rect(x + (gx + 1) * s, y + 18 * s, 2 * s, 7 * s, color);
    }
}
inline void DrawBadge(Canvas& canvas, int x, int y) {
    canvas.Sprite(kBadgeRows, 8, CatColor, x, y, 2);
}
}  // namespace boilon::ui
