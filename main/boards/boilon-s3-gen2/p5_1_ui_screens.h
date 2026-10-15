#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include "p5_1_ui_art.h"
#include "p5_1_ui_options.h"

// P5-1: every page, ported from docs/boilon/p5_1_prototype/proto.js with the
// same coordinates. The caller fills a Model; Draw paints it.
namespace boilon::ui {
enum class Page : uint8_t {
    kPlayer, kDirectory, kChecking, kNoCard, kEmpty, kCardError, kGoodbye, kHotspot,
    kActivation, kAi, kLowBattery, kUpdating, kUpdated
};
enum class Play : uint8_t { kPlaying, kPaused, kLocating, kSwitching, kFinished, kError };
enum class Toast : uint8_t { kNone, kAiOffline, kActivated, kHotspotRefused, kLowBattery };
// The selected directory's play mode (2026-10-15), as media::PlayMode.
enum class PlayMode : uint8_t { kOrder, kLoop, kSingle, kShuffle };
enum class AiStage : uint8_t { kConnecting, kListening, kThinking, kSpeaking, kReturning };
// The faces the server's emotion words map to, shown while speaking.
enum class Face : uint8_t { kDefault, kHappy, kSurprised, kSad, kSleepy, kThinking };

struct Model {
    Page page = Page::kPlayer;
    Play play = Play::kPlaying;
    std::string title, collection;
    size_t index = 0, count = 0;  // "2/24"; hidden while count is 0
    uint64_t position_ms = 0, duration_ms = 0;
    int volume = 0, battery = -1;
    bool ai_ready = false;  // the badge, and the hint on the no-story pages
    bool show_volume = false;
    int seek_seconds = 0;  // nonzero shows the ±30 s panel
    uint64_t seek_target_ms = 0;
    Toast toast = Toast::kNone;
    PlayMode mode = PlayMode::kOrder;  // an icon in the status bar unless in order
    std::vector<std::pair<std::string, size_t>> directories;  // name, files
    size_t highlighted = 0;
    uint32_t directory_left_ms = 0;
    std::string hotspot, code, prompt;
    uint32_t hotspot_left_s = 0;
    AiStage ai = AiStage::kConnecting;
    bool interrupted = false;  // the conversation paused a playing story
    bool no_story = false;     // no card: nothing to return to
    Face face = Face::kDefault;
    bool failed = false;       // the error face while returning
    bool subtitles = false;    // the bubble while thinking and speaking
    std::string child, reply;  // what the child said; the sentence being spoken
    int progress = 0;          // the SD update (M7): percent written; below 0 it failed
    std::string version;       // the firmware version, small on the checking page (M7)
};

struct Fonts {
    Font noto, pixel;
};

namespace detail {
inline std::string Clock(uint64_t seconds) {
    const unsigned s = static_cast<unsigned>(seconds % 60);
    return std::to_string(seconds / 60) + (s < 10 ? ":0" : ":") + std::to_string(s);
}
inline std::string Title(const std::string& name) {
    if (name.size() > 4) {
        std::string tail = name.substr(name.size() - 4);
        std::transform(tail.begin(), tail.end(), tail.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (tail == ".mp3" || tail == ".wav")
            return name.substr(0, name.size() - 4);
    }
    return name;
}
struct BatteryColors {
    uint8_t bg, text, sub, faint, green, red;
};

class Painter {
public:
    Painter(Canvas& canvas, const Fonts& fonts) : c_(canvas), f_(fonts) {}

    int Text(const std::string& s, int x, int y, uint8_t color, int max_width = 1000000000,
             int lines = 1, int pitch = 0, Align align = Align::kLeft, int scale = 1) {
        TextStyle style;
        style.font = &f_.noto;
        style.color = color;
        style.scale = scale;
        style.max_width = max_width;
        style.lines = lines;
        style.line_pitch = pitch;
        style.align = align;
        return c_.Text(s, x, y, style);
    }
    void Pixel(const std::string& s, int x, int y, int scale, uint8_t color,
               Align align = Align::kLeft) {
        TextStyle style;
        style.font = &f_.pixel;
        style.color = color;
        style.scale = scale;
        style.align = align;
        c_.Text(s, x, y, style);
    }
    int Width(const std::string& s, bool pixel = false, int scale = 1) {
        return Measure(Codepoints(s), pixel ? f_.pixel : f_.noto, scale);
    }
    void Panel(int x, int y, int w, int h) { c_.Panel(x, y, w, h, kPanel, kLine, kShadow); }

    // 2 px battery with four bars, red at 15% and below; returns the label's left edge.
    int Battery(const BatteryColors& k, int percent, int x, int y) {
        const bool low = percent >= 0 && percent <= 15;
        const uint8_t line = low ? k.red : k.text;
        c_.Round(x, y, 22, 12, line, 1);
        c_.Rect(x + 2, y + 2, 18, 8, k.bg);
        c_.Rect(x + 22, y + 3, 2, 6, line);
        const int bars = percent < 0 ? 0 : std::max(percent > 0 ? 1 : 0, (percent + 24) / 25);
        for (int i = 0; i < 4; ++i)
            c_.Rect(x + 3 + i * 4, y + 3, 3, 6, i < bars ? (low ? k.red : k.green) : k.faint);
        const std::string label = percent < 0 ? "--%" : std::to_string(percent) + "%";
        Text(label, x - 4, y - 3, k.sub, 1000000000, 1, 0, Align::kRight);
        return x - 4 - Width(label);
    }

    void TopBar(const Model& m) {
        static constexpr struct {
            const Icon* icon;
            const char* label;
            uint8_t color;
        } kStates[] = {{&kPlay, "正在播放", kGreen},     {&kPause, "已暂停", kAmber},
                       {&kHourglass, "正在定位", kBlue}, {&kHourglass, "正在切换", kBlue},
                       {&kDone, "已播完", kSub},         {&kWarn, "播放错误", kRed}};
        const auto& state = kStates[static_cast<int>(m.play)];
        DrawIcon(c_, *state.icon, 20 + (14 - IconWidth(*state.icon) * 2) / 2, 20, 2, state.color);
        Text(state.label, 42, 18, state.color, 110);
        // After the widest label, clear of the AI badge even at 100%.
        static constexpr const Icon* kModes[] = {nullptr, &kRepeat, &kRepeatOne, &kShuffle};
        if (const Icon* mode = kModes[static_cast<int>(m.mode)])
            DrawIcon(c_, *mode, 102, 20, 2, kSub);
        const int left = Battery({kBg, kText, kSub, kFaint, kGreen, kRed}, m.battery, 196, 21);
        if (m.ai_ready)
            DrawBadge(c_, left - 28, 18);
    }
    void Progress(const Model& m) {
        const double p = m.duration_ms ? static_cast<double>(m.position_ms) / m.duration_ms : 0;
        const int on = static_cast<int>(std::floor(p * 25 + 0.5));
        for (int i = 0; i < 25; ++i)
            c_.Rect(20 + i * 8, 216, 6, 10, i < on ? kAmber : kFaint);
    }
    void TimeRow(const Model& m) {
        const std::string elapsed = m.play == Play::kLocating ? "--:--" : Clock(m.position_ms / 1000);
        Pixel(elapsed, 20, 236, 2, kText);
        Text("/ " + (m.duration_ms ? Clock(m.duration_ms / 1000) : std::string("--:--")),
             24 + Width(elapsed, true, 2), 237, kSub);
        const std::string volume = std::to_string(m.volume);
        Pixel(volume, 220, 236, 2, kText, Align::kRight);
        DrawIcon(c_, m.volume ? kSpeaker : kMute, 220 - Width(volume, true, 2) - 22, 237, 2, kSub);
    }
    std::string Index(const Model& m) {
        return m.count ? std::to_string(m.index) + "/" + std::to_string(m.count) : std::string();
    }

    // A: the story card, title first.
    void Card(const Model& m) {
        c_.Clear(kBg);
        TopBar(m);
        const int cx = 16, cy = 44, cw = 206, ch = 156;
        Panel(cx, cy, cw, ch);
        DrawIcon(c_, kFolder, cx + 12, cy + 14, 2, kSub);
        const std::string index = Index(m);
        Text(index, cx + cw - 12, cy + 10, kSub, 1000000000, 1, 0, Align::kRight);
        Text(m.collection, cx + 36, cy + 10, kSub, cw - 60 - Width(index));
        c_.Dash(cx + 12, cy + 36, cw - 24, kFaint);
        const std::string title = Title(m.title);
        TextStyle style;
        style.font = &f_.noto;
        style.color = kText;
        style.scale = 2;
        style.max_width = cw - 24;
        style.lines = 9;
        const bool big = Wrap(title, style).size() <= 3;
        style.scale = big ? 2 : 1;
        style.lines = big ? 3 : 5;
        style.line_pitch = big ? 32 : 20;
        const int count = static_cast<int>(Wrap(title, style).size());
        const int block = (count - 1) * style.line_pitch + (big ? 30 : 16);
        c_.Text(title, cx + 12, cy + 40 + std::max(0, (ch - 48 - block) >> 1), style);
        Progress(m);
        TimeRow(m);
    }

    // C: 喵喵 listens along.
    void Cat(const Model& m, double t) {
        c_.Clear(kBg);
        TopBar(m);
        const bool playing = m.play == Play::kPlaying;
        const int x = 79, y = 40 + (playing && static_cast<long>(std::floor(t * 2)) % 2 ? 2 : 0);
        if (playing)
            DrawHeadphones(c_, x, y, 2, kBlue);
        const Eyes eyes = playing                        ? Eyes::kHappy
                          : m.play == Play::kFinished    ? Eyes::kSleep
                          : m.play == Play::kLocating    ? Eyes::kUp
                          : static_cast<long>(std::floor(t * 8)) % 24 == 0 ? Eyes::kBlink
                                                                         : Eyes::kOpen;
        DrawCat(c_, x, y, 2, eyes);
        if (playing) {
            const long f = static_cast<long>(std::floor(t * 3)) % 3;
            for (int i = 0; i < 2; ++i) {
                const int k = static_cast<int>((f + i * 2) % 3);
                DrawIcon(c_, kNote, 172 + i * 14, 92 - k * 10 - i * 6, 2, k == 2 ? kFaint : kSub);
            }
        }
        if (m.play == Play::kPaused)
            DrawIcon(c_, kPause, 172, 60, 2, kAmber);
        if (m.play == Play::kFinished) {
            Pixel("z", 168, 64, 2, kSub);
            Pixel("Z", 182, 50, 2, kSub);
        }
        if (m.play == Play::kLocating)
            DrawIcon(c_, kHourglass, 172, 60, 2, kBlue);
        DrawIcon(c_, kFolder, 20, 144, 2, kSub);
        const std::string index = Index(m);
        Text(index, 220, 140, kSub, 1000000000, 1, 0, Align::kRight);
        Text(m.collection, 44, 140, kSub, 170 - Width(index));
        Text(Title(m.title), 20, 166, kText, 200, 2, 20);
        Progress(m);
        TimeRow(m);
    }

    void Volume(int volume) {
        Panel(30, 92, 180, 88);
        DrawIcon(c_, volume ? kSpeaker : kMute, 46, 108, 3, kText);
        Pixel(std::to_string(volume), 194, 106, 3, kText, Align::kRight);
        for (int i = 0; i < 20; ++i)
            c_.Rect(50 + i * 7, 152, 5, 12, i * 5 < volume ? kAmber : kFaint);
    }
    void Seek(int seconds, uint64_t target_ms) {
        Panel(30, 92, 180, 88);
        DrawIcon(c_, seconds < 0 ? kBack : kForward, 46, 110, 2, kText);
        Text(std::string(seconds < 0 ? "后退 " : "前进 ") + std::to_string(std::abs(seconds)) + " 秒",
             76, 104, kText);
        Pixel(Clock(target_ms / 1000), 120, 140, 2, kAmber, Align::kCenter);
    }
    void ToastPanel(const Icon* icon, uint8_t icon_color, const std::string& line1,
                    const std::string& line2 = {}) {
        Panel(18, 168, 204, line2.empty() ? 46 : 64);
        if (icon)
            DrawIcon(c_, *icon, 32, 184, 2, icon_color);
        Text(line1, 62, line2.empty() ? 181 : 178, kText, 150);
        if (!line2.empty())
            Text(line2, 62, 200, kSub, 150);
    }
    void ShowToast(Toast toast) {
        if (toast == Toast::kAiOffline) {
            ToastPanel(nullptr, 0, std::string(kAiName) + "暂时不在线");
            DrawBadge(c_, 28, 183);
            Pixel("z", 50, 175, 1, kSub);
        } else if (toast == Toast::kActivated) {
            ToastPanel(&kDone, kGreen, "已激活", "双击顶部键就能聊天");
        } else if (toast == Toast::kHotspotRefused) {
            ToastPanel(&kWifi, kSub, "现在不能配网");
        } else if (toast == Toast::kLowBattery) {  // P5-4: once, at 10%.
            ToastPanel(&kWarn, kRed, "电量低，请充电", "没电前会自动保存");
        }
    }

    void Directories(const Model& m) {
        c_.Clear(kBg);
        DrawIcon(c_, kFolder, 20, 22, 2, kAmber);
        Text("选择目录", 46, 18, kText);
        const int total = static_cast<int>(m.directories.size());
        const int selected = static_cast<int>(m.highlighted);
        Text(std::to_string(selected + 1) + "/" + std::to_string(total), 220, 18, kSub, 1000000000,
             1, 0, Align::kRight);
        const int top = 46, row_height = 36;
        const int first = std::max(0, std::min(selected - 2, total - 5));
        Panel(16, top, 206, row_height * 5 + 8);
        for (int i = 0; i < 5 && first + i < total; ++i) {
            const auto& entry = m.directories[first + i];
            const int y = top + 4 + i * row_height;
            const bool chosen = first + i == selected;
            if (chosen) {
                c_.Rect(20, y, 198, row_height, kAmber);
                DrawIcon(c_, kPlay, 26, y + 11, 2, kLine);
            } else if (i) {
                c_.Dash(28, y, 180, kFaint);
            }
            Text(entry.first, 46, y + 8, chosen ? kLine : kText, 128);
            Text(std::to_string(entry.second) + "个", 210, y + 8, chosen ? kLine : kSub, 1000000000,
                 1, 0, Align::kRight);
        }
        Text("中键/底键 选择　顶部键 确认", 120, 246, kSub, 1000000000, 1, 0, Align::kCenter);
        c_.Rect(20, 270, static_cast<int>((200ULL * m.directory_left_ms + 2500) / 5000), 2, kFaint);
    }

    void Center(const Icon& icon, uint8_t color, const char* title,
                std::initializer_list<const char*> lines) {
        c_.Clear(kBg);
        const int w = IconWidth(icon) * 4, h = icon.count * 4;
        DrawIcon(c_, icon, (kWidth - w) >> 1, 70 - (h >> 1) + 10, 4, color);
        Text(title, 120, 118, kText, 1000000000, 1, 0, Align::kCenter, 2);
        int y = 160;
        for (const char* line : lines) {
            Text(line, 120, y, kSub, 200, 1, 0, Align::kCenter);
            y += 20;
        }
    }
    void AiHint(int y) {
        Panel(18, y, 204, 40);
        DrawBadge(c_, 28, y + 11);
        Text(std::string("双击顶部键 和") + kAiName + "聊天", 56, y + 11, kText);
    }
    void Hotspot(const Model& m) {
        c_.Clear(kBg);
        DrawIcon(c_, kWifi, 20, 22, 2, kBlue);
        Text("配网", 46, 18, kText);
        Text("剩 " + Clock(m.hotspot_left_s), 220, 18, kSub, 1000000000, 1, 0, Align::kRight);
        Panel(16, 48, 206, 196);
        Text("① 手机连接 Wi-Fi", 30, 60, kSub);
        Pixel(m.hotspot, 30, 86, 2, kText);
        c_.Dash(30, 116, 178, kFaint);
        Text("② 浏览器打开", 30, 128, kSub);
        Pixel("192.168.4.1", 30, 154, 2, kText);
        c_.Dash(30, 184, 178, kFaint);
        Text("③ 选家里的 Wi-Fi，填密码", 30, 196, kSub, 184);
        Text("10 分钟后自动关闭", 120, 254, kSub, 1000000000, 1, 0, Align::kCenter);
    }
    void Activation(const Model& m) {
        c_.Clear(kBg);
        DrawBadge(c_, 18, 18);
        Text(std::string("激活") + kAiName, 46, 18, kText);
        Panel(16, 52, 206, 96);
        if (m.code.size() == 6) {
            Text("激活码", 120, 62, kSub, 1000000000, 1, 0, Align::kCenter);
            Pixel(m.code.substr(0, 3) + " " + m.code.substr(3), 120, 94, 3, kText, Align::kCenter);
        } else {
            Text(m.prompt, 30, 62, kText, 180, 4);
        }
        Text("1. 打开 xiaozhi.me 添加设备", 22, 172, kText);
        Text("2. 输入上面的 6 位数字", 22, 196, kText);
        Text("激活后故事照常播放", 120, 246, kSub, 1000000000, 1, 0, Align::kCenter);
    }

    // t moves the animations; only the default round eyes blink.
    void Ai(const Model& m, double t) {
        static constexpr const char* kLabels[] = {"连接中", "我在听", "想一想", "我在说",
                                                  "回到故事"};
        static constexpr struct {
            Eyes eyes;
            Mouth mouth;
            Ears ears;
        } kFaces[] = {
            {Eyes::kOpen, Mouth::kW, Ears::kUp},     {Eyes::kHappy, Mouth::kW, Ears::kUp},
            {Eyes::kWide, Mouth::kRound, Ears::kUp}, {Eyes::kSad, Mouth::kSad, Ears::kDroop},
            {Eyes::kSleep, Mouth::kW, Ears::kUp},    {Eyes::kUp, Mouth::kFlat, Ears::kUp}};
        const long f8 = static_cast<long>(std::floor(t * 8));
        const long f3 = static_cast<long>(std::floor(t * 3));
        const bool open = static_cast<long>(std::floor(t * 5)) % 2 == 1;
        const char* label = kLabels[static_cast<int>(m.ai)];
        const bool returning = m.ai == AiStage::kReturning;
        const bool bubble =
            m.subtitles && (m.ai == AiStage::kThinking || m.ai == AiStage::kSpeaking);
        Eyes eyes = f8 % 26 == 0 ? Eyes::kBlink : Eyes::kOpen;
        Mouth mouth = Mouth::kW;
        Ears ears = Ears::kUp;
        if (m.ai == AiStage::kListening && f8 % 16 < 2)
            ears = Ears::kTwitch;
        if (m.ai == AiStage::kThinking) {
            eyes = Eyes::kUp;
            mouth = Mouth::kFlat;
        }
        if (m.ai == AiStage::kSpeaking) {
            const auto& face = kFaces[static_cast<int>(m.face)];
            eyes = face.eyes == Eyes::kOpen ? eyes : face.eyes;
            mouth = open ? Mouth::kOpen : face.mouth;
            ears = face.ears;
        }
        if (returning) {
            eyes = m.failed ? Eyes::kCross : Eyes::kHappy;
            mouth = m.failed ? Mouth::kFlat : Mouth::kW;
        }
        c_.Clear(kAiBg);
        DrawBadge(c_, 18, 18);
        Text(bubble ? std::string(label) : std::string("和") + kAiName + "聊天", 46, 18,
             bubble ? kAiText : kAiSub);
        Battery({kAiBg, kAiText, kAiSub, kAiFaint, kAiGreen, kAiRed}, m.battery, 196, 21);
        const int s = bubble ? 2 : 3, cw = kCatWidth * s, x = (kWidth - cw) >> 1, y = 40;
        DrawCat(c_, x, y, s, eyes, mouth, ears);
        if (m.ai == AiStage::kListening) {
            Arcs(x + 2 * s, y + 17 * s, f3 % 4, -1, s);
            Arcs(x + cw - 2 * s, y + 17 * s, f3 % 4, 1, s);
        }
        if (m.ai == AiStage::kThinking) {
            // Three thought bubbles rising from the right ear, filled one by one.
            static constexpr int kSpots[2][3][3] = {{{156, 66, 4}, {165, 56, 6}, {177, 44, 8}},
                                                    {{170, 78, 6}, {182, 64, 8}, {198, 48, 10}}};
            for (int i = 0; i < 3; ++i) {
                const int* b = kSpots[s == 3][i];
                c_.Round(b[0], b[1], b[2], b[2], i < f3 % 4 ? kAiText : kAiFaint,
                         b[2] >= 8 ? 2 : 1);
            }
        }
        if (m.ai == AiStage::kSpeaking && open)
            for (const auto& line : {std::array<int, 3>{1, -4, 3}, {2, 0, 4}, {1, 4, 3}})
                c_.Rect(x + cw + line[0] * s, y + 26 * s + line[1] * s, line[2] * s, s, kAiGlow);
        if (m.ai == AiStage::kConnecting) {
            // The Wi-Fi icon lights up from its bottom row.
            const int first = (3 - static_cast<int>(f3 % 4)) * 2;
            const auto light = [](char) { return uint8_t{kAiText}; };
            c_.Sprite(kWifi.rows + first, kWifi.count - first, light, x + cw - 2 * s,
                      y - 2 + first * s, s);
        }
        if (bubble) {
            // Thinking shows the child's words; speaking the sentence being
            // spoken, or the child's words until the first sentence arrives.
            c_.Panel(18, 140, 204, 100, kAiPanel, kAiLine, kAiShadow);
            for (int i = 0; i < 4; ++i)
                c_.Rect(112 + i * 2, 138 - i * 2, 16 - i * 4, 2, kAiLine);
            const bool reply = m.ai == AiStage::kSpeaking && !m.reply.empty();
            std::string words = reply ? m.reply : std::string();
            if (!reply && !m.child.empty())
                words = "你说：" + m.child;
            Text(words, 30, 150, reply ? kAiText : kAiSub, 180, 4, 20);
        } else {
            Text(m.no_story && returning ? "再见" : label, 120, y + kCatHeight * s + 2, kAiText,
                 1000000000, 1, 0, Align::kCenter, 2);
        }
        if (m.no_story) {
            Text(returning ? "对话已结束" : "按顶部键结束对话", 20, 252, returning ? kAiText : kAiSub);
        } else if (returning) {
            DrawIcon(c_, kPlay, 20, 254, 2, kAiGreen);
            Text("正在回到故事…", 42, 252, kAiText);
        } else if (m.interrupted) {
            DrawIcon(c_, kPause, 21, 254, 2, kAiAmber);
            Text("故事已暂停", 42, 252, kAiAmber);
            Text("顶部键 继续听", 220, 252, kAiSub, 1000000000, 1, 0, Align::kRight);
        } else {
            DrawIcon(c_, kPlay, 20, 254, 2, kAiSub);
            Text("按顶部键回到故事", 42, 252, kAiSub);
        }
    }
    // n of the three sound waves beside an ear, in the cat's own pixel scale.
    void Arcs(int cx, int cy, long n, int side, int s) {
        static constexpr int kWaves[3][11] = {
            {1, 0, 0, 1}, {2, 1, 0, 0, 0, 1, 2}, {3, 2, 1, 1, 0, 0, 0, 1, 1, 2, 3}};
        for (int i = 0; i < n; ++i) {
            const int rows = 4 + 3 * i + (i == 2);
            for (int r = 0; r < rows; ++r) {
                const int gx = side < 0 ? -(i * 3 + 2) + kWaves[i][r] : i * 3 + 2 - kWaves[i][r];
                c_.Rect(cx + gx * s - (side < 0 ? s : 0), cy + (r - rows / 2) * s, s, s, kAiGlow);
            }
        }
    }

private:
    Canvas& c_;
    const Fonts& f_;
};
}  // namespace detail

// t is in seconds and moves the animations (the C layout and the AI page).
inline void Draw(Canvas& canvas, const Fonts& fonts, const Model& m, Layout layout, double t) {
    detail::Painter p(canvas, fonts);
    const detail::BatteryColors battery = {kBg, kText, kSub, kFaint, kGreen, kRed};
    switch (m.page) {
        case Page::kPlayer:
            if (layout == Layout::kCat)
                p.Cat(m, t);
            else
                p.Card(m);
            if (m.show_volume)
                p.Volume(m.volume);
            else if (m.seek_seconds)
                p.Seek(m.seek_seconds, m.seek_target_ms);
            break;
        case Page::kDirectory: p.Directories(m); return;
        case Page::kChecking:
            p.Center(kSd, kBlue, "正在检查", {"SD 卡内容…"});
            p.Text(m.version, 120, 260, kSub, 1000000000, 1, 0, Align::kCenter);
            return;
        case Page::kNoCard:
            p.Center(kSd, kSub, "没有 SD 卡", {"请插入或检查 SD 卡"});
            p.Battery(battery, m.battery, 196, 21);
            if (m.ai_ready)
                p.AiHint(206);
            p.Text("长按顶部键关机", 120, 260, kSub, 1000000000, 1, 0, Align::kCenter);
            break;
        case Page::kEmpty:
            p.Center(kBox, kSub, "没有故事", {"卡里没有能播的 MP3", "放在根目录或一级目录"});
            p.Battery(battery, m.battery, 196, 21);
            if (m.ai_ready)
                p.AiHint(214);
            break;
        case Page::kCardError:
            p.Center(kWarn, kRed, "读卡出错", {"请检查 SD 卡内容"});
            p.Battery(battery, m.battery, 196, 21);
            if (m.ai_ready)
                p.AiHint(214);
            break;
        case Page::kGoodbye:
            p.Center(kMoon, kAmber, "晚安", {"正在保存进度…"});
            for (const auto& star : {std::pair<int, int>{150, 46}, {164, 84}, {82, 60}})
                DrawIcon(canvas, kStar, star.first, star.second, 2, kAmber);
            return;
        case Page::kLowBattery:  // P5-4: saving, then off.
            p.Center(kWarn, kRed, "电量低", {"请充电", "正在保存进度…"});
            return;
        case Page::kUpdating:  // M7: a newer version from the SD card, then a restart.
            if (m.progress < 0)
                p.Center(kWarn, kRed, "升级没有成功", {"仍用原来的版本"});
            else
                p.Center(kSd, kBlue, "正在升级",
                         {(m.title + "  " + std::to_string(m.progress) + "%").c_str(), "请不要关机"});
            return;
        case Page::kUpdated:  // M7: the first boot after an SD update, about 3 s.
            p.Center(kDone, kGreen, "升级完成", {("现在是 " + m.title).c_str()});
            return;
        case Page::kHotspot: p.Hotspot(m); return;
        case Page::kActivation: p.Activation(m); return;
        case Page::kAi: p.Ai(m, t); return;
    }
    p.ShowToast(m.toast);
}
}  // namespace boilon::ui
