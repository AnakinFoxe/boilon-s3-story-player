#pragma once
#include <atomic>
#include <cstdint>
#include <string>

// P5-1: the screen choices built into the firmware. Change a default here and
// rebuild; the self.story.set_screen_style tool switches at run time until the
// next restart (nothing is saved).
namespace boilon::ui {
enum class Palette : uint8_t { kPaper, kNight };
enum class Layout : uint8_t { kCard, kCat };
constexpr Palette kDefaultPalette = Palette::kPaper;
constexpr Layout kDefaultLayout = Layout::kCard;
// The AI page's bubble with what the child said and the sentence being spoken.
constexpr bool kDefaultSubtitles = false;
// The AI's name in the xiaozhi.me console; the protocol does not send it.
constexpr char kAiName[] = "喵喵";

// Written by the MCP tool on the main task, read by the player task.
struct Style {
    std::atomic<Palette> palette{kDefaultPalette};
    std::atomic<Layout> layout{kDefaultLayout};
    std::atomic<bool> subtitles{kDefaultSubtitles};
};
inline Style& CurrentStyle() {
    static Style style;
    return style;
}

// "paper" or "night", "card" or "cat", "on" or "off"; empty keeps the current
// value. Any other value changes nothing and returns false.
inline bool ApplyStyle(Style& style, const std::string& palette, const std::string& layout,
                       const std::string& subtitles = {}) {
    Palette p = style.palette;
    Layout l = style.layout;
    bool s = style.subtitles;
    if (palette == "paper" || palette == "night")
        p = palette == "paper" ? Palette::kPaper : Palette::kNight;
    else if (!palette.empty())
        return false;
    if (layout == "card" || layout == "cat")
        l = layout == "card" ? Layout::kCard : Layout::kCat;
    else if (!layout.empty())
        return false;
    if (subtitles == "on" || subtitles == "off")
        s = subtitles == "on";
    else if (!subtitles.empty())
        return false;
    style.palette = p;
    style.layout = l;
    style.subtitles = s;
    return true;
}
inline std::string DescribeStyle(const Style& style) {
    return std::string(R"({"palette":")") + (style.palette == Palette::kPaper ? "paper" : "night") +
           R"(","layout":")" + (style.layout == Layout::kCard ? "card" : "cat") +
           R"(","subtitles":")" + (style.subtitles ? "on" : "off") + "\"}";
}
}  // namespace boilon::ui
