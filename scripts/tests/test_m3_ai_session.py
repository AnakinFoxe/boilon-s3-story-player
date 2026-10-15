"""Run the M3 batch 3 session policy (m3_ai_session.h) unchanged against a fake port.

The board adapts this policy to Application, AudioService and the story
runtime; here a fake records every request, so the tests prove when the story
hands over and takes back the speaker, not that a real server answers.
"""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/boilon-s3-gen2"

SOURCE = r'''
#include <cassert>
#include <cstring>
#include "m3_ai_session.h"
using namespace boilon::ai;

struct FakePort : SessionPort {
    Chat app = Chat::kQuiet;
    bool park_ok = true, released = true, waiting = false, idle = true;
    int parks = 0, starts = 0, closes = 0, drops = 0, resumes = 0;
    Chat App() override { return app; }
    bool ParkStory(uint64_t) override {
        ++parks;
        return park_ok;
    }
    bool StoryReleased() override { return released; }
    bool StoryWaiting() override { return waiting; }
    bool ResumeStory(uint64_t) override {
        ++resumes;
        waiting = false;
        return true;
    }
    bool OutputIdle() override { return idle; }
    void StartChat() override { ++starts; }
    void CloseChat() override { ++closes; }
    void DropAiAudio() override { ++drops; }
};

// Double-click, talk, top key: the story comes back only after Application is
// idle again and the conversation's leftover audio is gone.
static void ButtonEndsConversation() {
    FakePort port;
    Session session(port);
    port.released = false;  // The story's decoder is still stopping.
    assert(session.Open(1000) && port.parks == 1 && session.Current() == Phase::kEntering);
    assert(!session.Open(1001) && port.parks == 1);  // One session at a time.
    assert(session.Poll(1010) == Event::kNone && port.starts == 0);
    assert(session.Shown() == Session::Showing::kConnecting && session.OwnsKeys());
    port.released = true;
    port.idle = false;  // Its last block is still playing: wait before the chat.
    assert(session.Poll(1020) == Event::kNone && port.starts == 0);
    port.idle = true;
    assert(session.Poll(1030) == Event::kChatRequested && port.starts == 1);
    port.app = Chat::kConnecting;
    assert(session.Poll(1100) == Event::kNone);
    port.app = Chat::kListening;
    assert(session.Poll(2000) == Event::kNone && session.Shown() == Session::Showing::kListening);
    port.app = Chat::kSpeaking;
    assert(session.Poll(3000) == Event::kNone && session.Shown() == Session::Showing::kSpeaking);
    session.Close(EndReason::kButton, 4000);
    assert(port.closes == 1 && session.Current() == Phase::kExiting);
    assert(session.Reason() == EndReason::kButton);
    assert(session.Shown() == Session::Showing::kReturning);
    session.Close(EndReason::kTool, 4001);  // Already ending: nothing more.
    assert(port.closes == 1 && session.Reason() == EndReason::kButton);
    assert(session.Poll(4010) == Event::kNone && port.drops == 0 && port.resumes == 0);
    port.app = Chat::kQuiet;
    port.idle = false;  // Undelivered conversation audio is dropped first.
    assert(session.Poll(4050) == Event::kNone && port.drops == 1 && port.resumes == 0);
    assert(session.Poll(4060) == Event::kNone && port.drops == 1);
    port.idle = true;
    assert(session.Poll(4070) == Event::kResumed && port.resumes == 1);
    assert(session.ResumeDelayMs() == 70 && !session.Active() && !session.OwnsKeys());
    assert(session.Poll(4080) == Event::kNone && port.resumes == 1 && port.closes == 1);
}

// The tool, the server and a channel that never opens all end the same way.
static void OtherEndings() {
    {
        FakePort port;
        Session session(port);
        assert(session.Open(0) && session.Poll(10) == Event::kChatRequested);
        port.app = Chat::kListening;
        session.Poll(20);
        session.Close(EndReason::kTool, 30);
        assert(port.closes == 1 && session.Reason() == EndReason::kTool);
        port.app = Chat::kQuiet;
        assert(session.Poll(40) == Event::kResumed && port.drops == 1 && port.resumes == 1);
    }
    {
        FakePort port;  // Goodbye, a network drop or an error: Application is idle again.
        Session session(port);
        assert(session.Open(0) && session.Poll(10) == Event::kChatRequested);
        port.app = Chat::kSpeaking;
        session.Poll(500);
        port.app = Chat::kQuiet;
        assert(session.Poll(600) == Event::kEnded && session.Reason() == EndReason::kServer);
        assert(port.closes == 0);  // Nothing to close.
        assert(session.Poll(610) == Event::kResumed && port.resumes == 1);
        assert(port.drops == 0);  // Nothing was left to play.
    }
    {
        FakePort port;  // The server's goodbye is still playing: let it finish.
        Session session(port);
        assert(session.Open(0) && session.Poll(10) == Event::kChatRequested);
        port.app = Chat::kSpeaking;
        session.Poll(500);
        port.app = Chat::kQuiet;
        port.idle = false;
        assert(session.Poll(600) == Event::kEnded && session.Reason() == EndReason::kServer);
        assert(session.Poll(610) == Event::kNone && port.drops == 0 && port.resumes == 0);
        assert(session.Poll(600 + kFarewellMs - 1) == Event::kNone && port.drops == 0);
        port.idle = true;
        assert(session.Poll(600 + kFarewellMs - 1) == Event::kResumed);
        assert(port.drops == 0 && port.resumes == 1);
    }
    {
        FakePort port;  // A goodbye that never ends is cut after kFarewellMs.
        Session session(port);
        assert(session.Open(0) && session.Poll(10) == Event::kChatRequested);
        port.app = Chat::kSpeaking;
        session.Poll(500);
        port.app = Chat::kQuiet;
        port.idle = false;
        assert(session.Poll(600) == Event::kEnded);
        assert(session.Poll(600 + kFarewellMs - 1) == Event::kNone && port.drops == 0);
        assert(session.Poll(600 + kFarewellMs) == Event::kNone && port.drops == 1);
        port.idle = true;
        assert(session.Poll(600 + kFarewellMs + 10) == Event::kResumed && port.drops == 1);
    }
    {
        FakePort port;  // Application never leaves idle (no protocol, refused).
        Session session(port);
        assert(session.Open(0) && session.Poll(10) == Event::kChatRequested);
        assert(session.Poll(10 + kStartMs - 1) == Event::kNone);
        assert(session.Poll(10 + kStartMs) == Event::kEnded);
        assert(session.Reason() == EndReason::kNotStarted && port.closes == 0);
        assert(session.Poll(10 + kStartMs + 1) == Event::kResumed);
    }
    {
        FakePort port;  // The story never lets go: no chat, the story continues.
        Session session(port);
        port.released = false;
        assert(session.Open(0));
        assert(session.Poll(kReleaseMs - 1) == Event::kNone);
        assert(session.Poll(kReleaseMs) == Event::kEnded);
        assert(session.Reason() == EndReason::kStoryBusy && port.starts == 0);
        assert(session.Poll(kReleaseMs + 1) == Event::kResumed && port.resumes == 1);
    }
    {
        FakePort port;  // The top key before the chat was requested.
        Session session(port);
        port.released = false;
        assert(session.Open(0));
        session.Close(EndReason::kButton, 5);
        port.released = true;
        assert(session.Poll(10) == Event::kResumed && port.starts == 0 && port.closes == 0);
    }
}

static void RefusedOpen() {
    FakePort port;
    Session session(port);
    port.park_ok = false;  // Starting, switching or locating.
    assert(!session.Open(0) && !session.Active() && port.parks == 1);
    port.park_ok = true;
    port.app = Chat::kBusy;  // A server push owns the speaker.
    assert(!session.Open(0) && port.parks == 1);
}

// Application keeps talking after the close request: ask again, and never
// give the story the speaker while the microphone may be open.
static void CloseRepeatsUntilIdle() {
    FakePort port;
    Session session(port);
    assert(session.Open(0) && session.Poll(10) == Event::kChatRequested);
    port.app = Chat::kListening;
    session.Close(EndReason::kButton, 100);
    assert(port.closes == 1);
    assert(session.Poll(100 + kRetryMs - 1) == Event::kNone && port.closes == 1);
    assert(session.Poll(100 + kRetryMs) == Event::kNone && port.closes == 2);
    assert(port.resumes == 0 && port.drops == 0);
    port.app = Chat::kQuiet;
    assert(session.Poll(100 + kRetryMs + 1) == Event::kResumed);
}

// After a session, a late reply (or anything the child did not ask for)
// makes Application talk again: close it; if it cut the story, resume.
static void UnsolicitedConversation() {
    FakePort port;
    Session session(port);
    port.app = Chat::kSpeaking;
    port.waiting = true;  // The reply's ResetDecoder ended the story's session.
    assert(session.OwnsKeys() && !session.Active());
    assert(session.Poll(0) == Event::kClosedUnsolicited && port.closes == 1);
    assert(session.Poll(kRetryMs - 1) == Event::kNone && port.closes == 1);
    assert(port.resumes == 0);  // Never while Application may have the microphone.
    assert(session.Poll(kRetryMs) == Event::kClosedUnsolicited && port.closes == 2);
    port.app = Chat::kQuiet;
    assert(session.Poll(kRetryMs + 10) == Event::kResumed && port.drops == 1);
    assert(port.resumes == 1 && !session.OwnsKeys());
    port.app = Chat::kListening;  // A second one is closed right away.
    assert(session.Poll(kRetryMs + 20) == Event::kClosedUnsolicited && port.closes == 3);
}

// A server push (not a conversation) is left alone until it ends or the
// child presses the top key; the story waits for it either way.
static void ServerPush() {
    FakePort port;
    Session session(port);
    port.app = Chat::kBusy;
    port.waiting = true;
    assert(session.Poll(0) == Event::kNone && port.closes == 0 && port.resumes == 0);
    assert(session.OwnsKeys());
    session.Close(EndReason::kButton, 10);
    assert(port.closes == 1 && !session.Active());
    port.app = Chat::kQuiet;
    assert(session.Poll(20) == Event::kResumed && port.resumes == 1);
    session.Close(EndReason::kButton, 30);  // Nothing owns the speaker: no request.
    assert(port.closes == 1);
}

// P5-2: after the play tool, 喵喵 finishes the answer before the conversation
// ends, and what was already sent plays out like a goodbye.
static void PlayEndsAfterReply() {
    assert(std::strcmp(kEndReasonNames[static_cast<int>(EndReason::kPlay)], "play") == 0);
    {
        FakePort port;
        Session session(port);
        session.EndAfterReply(0);  // No conversation: nothing to end.
        port.released = false;
        assert(session.Open(0) && session.Current() == Phase::kEntering);
        session.EndAfterReply(5);  // Not talking yet: ignored too.
        port.released = true;
        assert(session.Poll(10) == Event::kChatRequested);
        port.app = Chat::kListening;
        assert(session.Poll(10 + kReplyMaxMs) == Event::kNone && port.closes == 0);
    }
    {
        FakePort port;  // The usual order: think, answer, listen again.
        Session session(port);
        assert(session.Open(0) && session.Poll(10) == Event::kChatRequested);
        port.app = Chat::kListening;
        assert(session.Poll(20) == Event::kNone);
        session.EndAfterReply(100);
        assert(session.Poll(200) == Event::kNone && port.closes == 0);
        port.app = Chat::kSpeaking;
        assert(session.Poll(300) == Event::kNone);
        assert(session.Poll(100 + kReplyStartMs + 1) == Event::kNone);  // Speaking: no start limit.
        port.app = Chat::kListening;  // The answer is over.
        assert(session.Poll(6000) == Event::kEnded && session.Reason() == EndReason::kPlay);
        assert(port.closes == 1 && session.Current() == Phase::kExiting);
        port.app = Chat::kQuiet;
        port.idle = false;  // Its last words are still playing: keep them.
        assert(session.Poll(6010) == Event::kNone && port.drops == 0 && port.resumes == 0);
        port.idle = true;
        assert(session.Poll(6020) == Event::kResumed && port.drops == 0 && port.resumes == 1);
    }
    {
        FakePort port;  // 喵喵 never answers: end after kReplyStartMs.
        Session session(port);
        assert(session.Open(0) && session.Poll(10) == Event::kChatRequested);
        port.app = Chat::kListening;
        session.Poll(20);
        session.EndAfterReply(1000);
        session.EndAfterReply(3000);  // A second pick keeps the first deadline.
        assert(session.Poll(1000 + kReplyStartMs - 1) == Event::kNone);
        assert(session.Poll(1000 + kReplyStartMs) == Event::kEnded);
        assert(session.Reason() == EndReason::kPlay && port.closes == 1);
    }
    {
        FakePort port;  // A very long answer: end after kReplyMaxMs; the rest is cut.
        Session session(port);
        assert(session.Open(0) && session.Poll(10) == Event::kChatRequested);
        port.app = Chat::kListening;
        session.Poll(20);
        session.EndAfterReply(1000);
        port.app = Chat::kSpeaking;
        assert(session.Poll(1000 + kReplyMaxMs - 1) == Event::kNone);
        assert(session.Poll(1000 + kReplyMaxMs) == Event::kEnded && port.closes == 1);
        const uint64_t ended = 1000 + kReplyMaxMs;
        port.app = Chat::kQuiet;
        port.idle = false;
        assert(session.Poll(ended + 10) == Event::kNone && port.drops == 0);
        assert(session.Poll(ended + kFarewellMs) == Event::kNone && port.drops == 1);
        port.idle = true;
        assert(session.Poll(ended + kFarewellMs + 10) == Event::kResumed && port.resumes == 1);
    }
    {
        FakePort port;  // The server ends first: its own reason, the goodbye kept.
        Session session(port);
        assert(session.Open(0) && session.Poll(10) == Event::kChatRequested);
        port.app = Chat::kListening;
        session.Poll(20);
        session.EndAfterReply(1000);
        port.app = Chat::kSpeaking;
        session.Poll(1500);
        port.app = Chat::kQuiet;
        assert(session.Poll(1600) == Event::kEnded && session.Reason() == EndReason::kServer);
        assert(port.closes == 0 && session.Poll(1610) == Event::kResumed && port.drops == 0);
    }
    {
        FakePort port;  // The top key while waiting ends at once, as always.
        Session session(port);
        assert(session.Open(0) && session.Poll(10) == Event::kChatRequested);
        port.app = Chat::kSpeaking;
        session.Poll(20);
        session.EndAfterReply(1000);
        session.Close(EndReason::kButton, 1200);
        assert(session.Reason() == EndReason::kButton && port.closes == 1);
        port.app = Chat::kListening;
        assert(session.Poll(1300) == Event::kNone && session.Reason() == EndReason::kButton);
        port.app = Chat::kQuiet;
        assert(session.Poll(1400) == Event::kResumed && port.drops == 1);
    }
    {
        FakePort port;  // A new conversation never inherits an old wait.
        Session session(port);
        assert(session.Open(0) && session.Poll(10) == Event::kChatRequested);
        port.app = Chat::kListening;
        session.Poll(20);
        session.EndAfterReply(100);
        session.Close(EndReason::kButton, 200);
        port.app = Chat::kQuiet;
        assert(session.Poll(300) == Event::kResumed);
        assert(session.Open(1000) && session.Poll(1010) == Event::kChatRequested);
        port.app = Chat::kListening;
        assert(session.Poll(1010 + kReplyMaxMs) == Event::kNone && port.closes == 1);
    }
}

int main() {
    ButtonEndsConversation();
    OtherEndings();
    RefusedOpen();
    CloseRepeatsUntilIdle();
    UnsolicitedConversation();
    ServerPush();
    PlayEndsAfterReply();
}
'''


class M3AiSessionHostTests(unittest.TestCase):
    def test_session_policy_with_fake_port(self):
        compiler = shutil.which("c++")
        self.assertTrue(compiler, "Native C++ compiler required; do not silently skip")
        with tempfile.TemporaryDirectory(prefix="m3-ai-session-") as temporary:
            executable = Path(temporary) / "m3-ai-session-test"
            compiled = subprocess.run(
                [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                 "-I", str(BOARD), "-x", "c++", "-", "-o", str(executable)],
                input=SOURCE, text=True, capture_output=True, timeout=30, cwd=ROOT)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            tested = subprocess.run([str(executable)], text=True, capture_output=True,
                                    timeout=10, cwd=temporary)
            self.assertEqual(tested.returncode, 0, tested.stdout + tested.stderr)


if __name__ == "__main__":
    unittest.main()
