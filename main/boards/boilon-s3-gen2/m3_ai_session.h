#pragma once
#include <cstdint>

// M3 batch 3: who owns the speaker, the story or a conversation. No ESP-IDF
// headers, so test_m3_ai_session runs this exact code against a fake port.
namespace boilon::ai {
// Application's state as far as the speaker is concerned. kBusy covers states
// that own the speaker without a conversation (a server push, an upgrade).
enum class Chat { kQuiet, kConnecting, kListening, kSpeaking, kBusy };
enum class Phase { kOff, kEntering, kActive, kExiting };
enum class EndReason { kNone, kButton, kTool, kServer, kNotStarted, kStoryBusy, kPlay };
constexpr const char* kEndReasonNames[] = {"none",        "button",     "tool", "server",
                                           "not_started", "story_busy", "play"};
// 2026-10-15: 喵喵 was asked to keep the story paused or to power off once the
// conversation ends (the pause and power_off tools).
enum class StoryEnd : uint8_t { kNone, kPause, kPowerOff };
// The story releases its decoder within kReleaseMs; Application leaves idle
// within kStartMs of the request; a close request is repeated every kRetryMs;
// a goodbye the server ended with may finish playing for kFarewellMs.
constexpr uint64_t kReleaseMs = 2000, kStartMs = 3000, kRetryMs = 3000, kFarewellMs = 3000;
// P5-2: after the play tool, 喵喵's answer may take kReplyStartMs to begin and
// kReplyMaxMs in all before the conversation ends anyway.
constexpr uint64_t kReplyStartMs = 5000, kReplyMaxMs = 20000;

// Board services, all called from the player task.
struct SessionPort {
    virtual ~SessionPort() = default;
    virtual Chat App() = 0;
    virtual bool ParkStory(uint64_t now_ms) = 0;  // False while the story is busy.
    virtual bool StoryReleased() = 0;             // No decoder holds the speaker.
    virtual bool StoryWaiting() = 0;              // Lost the speaker while playing.
    virtual bool ResumeStory(uint64_t now_ms) = 0;
    virtual bool OutputIdle() = 0;
    virtual void StartChat() = 0;    // Application opens a conversation.
    virtual void CloseChat() = 0;    // Application closes it and returns to idle.
    virtual void DropAiAudio() = 0;  // Discards conversation audio not yet played.
};

enum class Event { kNone, kChatRequested, kEnded, kResumed, kClosedUnsolicited };

class Session {
public:
    explicit Session(SessionPort& port) : port_(port) {}

    // A double-click while the AI is ready. False when the story cannot hand
    // over the speaker right now; nothing changes then.
    bool Open(uint64_t now_ms) {
        if (phase_ != Phase::kOff || port_.App() != Chat::kQuiet || !port_.ParkStory(now_ms))
            return false;
        phase_ = Phase::kEntering;
        since_ = now_ms;
        seen_chat_ = false;
        reason_ = EndReason::kNone;
        return true;
    }

    // The top key or the "back to story" tool. Outside a session the key
    // ends whatever else owns the speaker (a server push).
    void Close(EndReason reason, uint64_t now_ms) {
        if (phase_ == Phase::kOff) {
            if (port_.App() != Chat::kQuiet)
                port_.CloseChat();
        } else if (phase_ != Phase::kExiting) {
            End(reason, now_ms);
        }
    }

    // P5-2: the play tool picked something. 喵喵 finishes this answer, then the
    // conversation ends and what was already sent plays out, like a goodbye.
    void EndAfterReply(uint64_t now_ms) {
        if (phase_ != Phase::kActive || awaiting_reply_)
            return;
        awaiting_reply_ = true;
        replied_ = false;
        reply_since_ = now_ms;
    }

    // Every player-loop tick; returns what happened, for the log.
    Event Poll(uint64_t now_ms) {
        const Chat chat = port_.App();
        const bool talking =
            chat == Chat::kConnecting || chat == Chat::kListening || chat == Chat::kSpeaking;
        switch (phase_) {
            case Phase::kOff:
                // Nobody asked for this conversation (a late reply after the
                // child ended one): close it. Its channel is already closed,
                // so it brings no audio.
                if (!talking) {
                    guarding_ = false;
                } else if (!guarding_ || now_ms - closed_ms_ >= kRetryMs) {
                    port_.CloseChat();
                    guarding_ = true;
                    closed_ms_ = now_ms;
                    return Event::kClosedUnsolicited;
                }
                if (chat == Chat::kQuiet && port_.StoryWaiting())
                    return Resume(now_ms);
                return Event::kNone;
            case Phase::kEntering:
                if (port_.StoryReleased() && port_.OutputIdle()) {
                    port_.StartChat();
                    phase_ = Phase::kActive;
                    since_ = now_ms;
                    return Event::kChatRequested;
                }
                if (now_ms - since_ >= kReleaseMs)
                    return End(EndReason::kStoryBusy, now_ms);
                return Event::kNone;
            case Phase::kActive:
                seen_chat_ = seen_chat_ || talking;
                if (seen_chat_ && chat == Chat::kQuiet)
                    return End(EndReason::kServer, now_ms);
                if (awaiting_reply_) {
                    replied_ = replied_ || chat == Chat::kSpeaking;
                    const uint64_t waited = now_ms - reply_since_;
                    if ((replied_ && chat != Chat::kSpeaking) ||
                        (!replied_ && waited >= kReplyStartMs) || waited >= kReplyMaxMs)
                        return End(EndReason::kPlay, now_ms);
                }
                if (!seen_chat_ && now_ms - since_ >= kStartMs)
                    return End(EndReason::kNotStarted, now_ms);
                return Event::kNone;
            case Phase::kExiting:
                if (chat == Chat::kQuiet)
                    return Resume(now_ms);
                if (now_ms - closed_ms_ >= kRetryMs) {
                    port_.CloseChat();
                    closed_ms_ = now_ms;
                }
                return Event::kNone;
        }
        return Event::kNone;
    }

    Phase Current() const { return phase_; }
    bool Active() const { return phase_ != Phase::kOff; }
    EndReason Reason() const { return reason_; }
    // Milliseconds from the end request to the story's resume request.
    uint64_t ResumeDelayMs() const { return resume_delay_ms_; }
    // While anything other than the story owns the speaker, the keys follow
    // the AI rules: the top key ends it and track changes wait.
    bool OwnsKeys() const { return Active() || port_.App() != Chat::kQuiet; }
    // P5-1: the stage the AI page shows.
    enum class Showing { kConnecting, kListening, kSpeaking, kReturning };
    Showing Shown() const {
        const Chat chat = port_.App();
        return phase_ == Phase::kExiting  ? Showing::kReturning
               : chat == Chat::kListening ? Showing::kListening
               : chat == Chat::kSpeaking  ? Showing::kSpeaking
                                          : Showing::kConnecting;
    }

private:
    Event End(EndReason reason, uint64_t now_ms) {
        reason_ = reason;
        phase_ = Phase::kExiting;
        awaiting_reply_ = false;
        ended_ms_ = now_ms;
        dropped_ = false;
        if (port_.App() != Chat::kQuiet)
            port_.CloseChat();
        closed_ms_ = now_ms;
        return Event::kEnded;
    }
    // Late conversation audio never plays over the story. When the server ended
    // the conversation, or 喵喵 answered the play tool, what was already sent
    // (the goodbye) plays out first.
    Event Resume(uint64_t now_ms) {
        const bool farewell = phase_ == Phase::kExiting &&
                              (reason_ == EndReason::kServer || reason_ == EndReason::kPlay) &&
                              now_ms - ended_ms_ < kFarewellMs;
        if (!dropped_ && !farewell) {
            port_.DropAiAudio();
            dropped_ = true;
        }
        if (!port_.OutputIdle())
            return Event::kNone;
        resume_delay_ms_ = phase_ == Phase::kExiting ? now_ms - ended_ms_ : 0;
        phase_ = Phase::kOff;
        dropped_ = false;
        port_.ResumeStory(now_ms);
        return Event::kResumed;
    }

    SessionPort& port_;
    Phase phase_ = Phase::kOff;
    EndReason reason_ = EndReason::kNone;
    uint64_t since_ = 0, ended_ms_ = 0, closed_ms_ = 0, resume_delay_ms_ = 0, reply_since_ = 0;
    bool seen_chat_ = false, dropped_ = false, guarding_ = false;
    bool awaiting_reply_ = false, replied_ = false;
};
}  // namespace boilon::ai
