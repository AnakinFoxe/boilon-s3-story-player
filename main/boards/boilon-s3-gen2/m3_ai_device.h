#pragma once
#include <algorithm>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include "m3_ai_network.h"
#include "m3_ai_session.h"
#include "media/player_gestures.h"
#include "p5_1_ui_screens.h"

// M3 batch 4: what XiaoZhi reads about the device, and the page shown when
// there is no story. No ESP-IDF headers, so test_m3_ai_device runs this exact
// code against fakes.
namespace boilon::ai {
// self.get_device_status: the volume the speaker plays at and, when known,
// the estimated battery level. Nothing else (no network names).
inline std::string DeviceStatusJson(int volume, int battery_percent) {
    std::string json = R"({"audio_speaker":{"volume":)" + std::to_string(volume) + "}";
    if (battery_percent >= 0)
        json += R"(,"battery":{"level":)" + std::to_string(battery_percent) + "}";
    return json + "}";
}

// No card, an empty card or a card error: a double-click still talks to
// XiaoZhi, the middle and bottom keys set the volume, the volume-key hold
// opens the hotspot and a long top press powers off. Its session port has no
// story: parking and resuming succeed with nothing to do.
class NoStoryPage {
public:
    static constexpr int kStartVolume = 60;  // Medium; never saved.
    static constexpr int kMaxVolume = 100;
    struct Tick {
        media::GestureAction gesture = media::GestureAction::kNone;
        Event event = Event::kNone;
        bool volume_changed = false;  // By a key: the board sets the codec.
    };

    NoStoryPage(SessionPort& port, int volume) : session_(port), volume_(volume) {}

    // Every loop tick. codec_volume is what the speaker plays at now; a
    // different value means XiaoZhi set it, and the keys continue from it.
    Tick Poll(uint8_t pressed, uint64_t now_ms, bool ai_ready, int codec_volume,
              bool back_to_story) {
        using Action = media::GestureAction;
        volume_ = codec_volume;
        Tick tick;
        const auto mode =
            session_.OwnsKeys() ? media::GestureMode::kAi : media::GestureMode::kPlayer;
        tick.gesture = gestures_.Update(pressed, now_ms, {mode, ai_ready});
        if (tick.gesture == Action::kOpenAi && !session_.Open(now_ms))
            refused_ms_ = now_ms;
        if (tick.gesture == Action::kOfflineHint)
            refused_ms_ = now_ms;
        if (tick.gesture == Action::kCancelAi)
            session_.Close(EndReason::kButton, now_ms);
        if (tick.gesture == Action::kVolumeUp || tick.gesture == Action::kVolumeDown) {
            volume_ = std::clamp(volume_ + (tick.gesture == Action::kVolumeUp ? 5 : -5), 0,
                                 kMaxVolume);
            tick.volume_changed = true;
        }
        if (back_to_story)
            session_.Close(EndReason::kTool, now_ms);
        tick.event = session_.Poll(now_ms);
        return tick;
    }
    int Volume() const { return volume_; }
    const Session& Conversation() const { return session_; }
    // 2026-10-15: 喵喵 was asked to power off; the conversation ends after her answer.
    void EndAfterReply(uint64_t now_ms) { session_.EndAfterReply(now_ms); }
    // A double-click within the last 2 s found the AI not ready.
    bool Refused(uint64_t now_ms) const { return refused_ms_ && now_ms - *refused_ms_ < 2000; }

private:
    Session session_;
    media::PlayerGestures gestures_;
    int volume_;
    std::optional<uint64_t> refused_ms_;
};

// P5-1: a network notice takes over the page (hotspot, activation code) or
// shows as a short notice over it.
inline void ShowNotice(ui::Model& m, const std::optional<Notice>& notice) {
    if (!notice)
        return;
    switch (notice->kind) {
        case Notice::Kind::kRefused: m.toast = ui::Toast::kHotspotRefused; break;
        case Notice::Kind::kActivated: m.toast = ui::Toast::kActivated; break;
        case Notice::Kind::kHotspot:
            m.page = ui::Page::kHotspot;
            m.hotspot = notice->detail;
            m.hotspot_left_s = static_cast<uint32_t>((notice->remaining_ms + 999) / 1000);
            break;
        case Notice::Kind::kActivating:
            m.page = ui::Page::kActivation;
            m.code = notice->detail;
            m.prompt = notice->title;
            break;
    }
}
// P5-1 batch 2: what upstream told the display during this conversation.
struct ChatView {
    bool heard = false;        // a recognition result arrived this turn
    std::string child, reply;  // what the child said; the sentence being spoken
    ui::Face face = ui::Face::kDefault;
    bool failed = false;  // an error or a Wi-Fi drop since the conversation opened
};

// The server's emotion words as the faces in P5_1_PIXEL_UI.md.
inline ui::Face FaceFor(const std::string& emotion) {
    using F = ui::Face;
    static constexpr std::pair<const char*, F> kWords[] = {
        {"happy", F::kHappy},       {"laughing", F::kHappy},    {"funny", F::kHappy},
        {"loving", F::kHappy},      {"winking", F::kHappy},     {"relaxed", F::kHappy},
        {"delicious", F::kHappy},   {"kissy", F::kHappy},       {"confident", F::kHappy},
        {"cool", F::kHappy},        {"silly", F::kHappy},       {"surprised", F::kSurprised},
        {"shocked", F::kSurprised}, {"sad", F::kSad},           {"crying", F::kSad},
        {"sleepy", F::kSleepy},     {"thinking", F::kThinking}, {"confused", F::kThinking}};
    for (const auto& word : kWords)
        if (emotion == word.first)
            return word.second;
    return F::kDefault;
}

// Written by the board's display on upstream's main task (and a Wi-Fi drop on
// the event task), read by the player task once a frame.
class ChatScreen {
public:
    // More than the bubble's four lines hold, so a cut text still ends in "…".
    static constexpr size_t kMaxText = 240;
    void Begin() {  // A new conversation.
        std::lock_guard<std::mutex> lock(mutex_);
        view_ = ChatView();
    }
    void Listening() {  // A new turn.
        std::lock_guard<std::mutex> lock(mutex_);
        view_.heard = false;
        view_.child.clear();
        view_.reply.clear();
    }
    void Speaking() {
        std::lock_guard<std::mutex> lock(mutex_);
        view_.heard = false;
    }
    void Failed() {
        std::lock_guard<std::mutex> lock(mutex_);
        view_.failed = true;
    }
    void Heard(const char* text) {
        std::lock_guard<std::mutex> lock(mutex_);
        view_.heard = true;
        view_.child = Clip(text);
    }
    void Said(const char* text) {
        std::lock_guard<std::mutex> lock(mutex_);
        view_.reply = Clip(text);
    }
    void Emotion(const char* word) {
        std::lock_guard<std::mutex> lock(mutex_);
        view_.face = FaceFor(word);
    }
    ChatView View() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return view_;
    }

private:
    // At most kMaxText bytes, never cutting a UTF-8 sequence.
    static std::string Clip(const char* text) {
        size_t n = 0;
        while (n < kMaxText && text[n])
            ++n;
        while (n && (static_cast<unsigned char>(text[n]) & 0xc0) == 0x80)
            --n;
        return std::string(text, n);
    }
    mutable std::mutex mutex_;
    ChatView view_;
};
inline ChatScreen& CurrentChat() {
    static ChatScreen chat;
    return chat;
}

// A conversation replaces whatever page was showing. Thinking: upstream still
// listens but the child's words arrived. The error face also shows when the
// conversation never started.
inline void ShowConversation(ui::Model& m, const Session& session, bool interrupted, bool no_story,
                             const ChatView& chat, bool subtitles) {
    using Stage = ui::AiStage;
    static constexpr Stage kStages[] = {Stage::kConnecting, Stage::kListening, Stage::kSpeaking,
                                        Stage::kReturning};
    const auto shown = session.Shown();
    m.page = ui::Page::kAi;
    m.ai = shown == Session::Showing::kListening && chat.heard ? Stage::kThinking
                                                               : kStages[static_cast<int>(shown)];
    m.interrupted = interrupted;
    m.no_story = no_story;
    m.face = chat.face;
    m.failed = chat.failed || session.Reason() == EndReason::kNotStarted ||
               session.Reason() == EndReason::kStoryBusy;
    m.subtitles = subtitles;
    m.child = chat.child;
    m.reply = chat.reply;
}
}  // namespace boilon::ai
