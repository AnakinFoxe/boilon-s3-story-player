"""Run the M3 batch 4 device status and no-story page (m3_ai_device.h) unchanged.

The page drives the real session policy and key recognizer; a fake port
stands in for StoryFocus without a runtime (nothing to hand over or resume)
and records what Application would be asked to do. This proves the page's
rules, not that a real server answers or what the screen draws.
"""

import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/boilon-s3-gen2"

SOURCE = r'''
#include <cassert>
#include <string>
#include <utility>
#include <vector>
#include "m3_ai_device.h"
using namespace boilon::ai;
using Action = media::GestureAction;
using G = media::PlayerGestures;

// StoryFocus without a runtime: parking and resuming succeed with nothing to do.
struct FakePort : SessionPort {
    Chat app = Chat::kQuiet;
    bool idle = true;
    int starts = 0, closes = 0, drops = 0;
    Chat App() override { return app; }
    bool ParkStory(uint64_t) override { return true; }
    bool StoryReleased() override { return true; }
    bool StoryWaiting() override { return false; }
    bool ResumeStory(uint64_t) override { return true; }
    bool OutputIdle() override { return idle; }
    void StartChat() override { ++starts; }
    void CloseChat() override { ++closes; }
    void DropAiAudio() override { ++drops; }
};

// The board's loop: a 10 ms tick, and the codec follows the page's keys.
struct Board {
    FakePort port;
    NoStoryPage page{port, NoStoryPage::kStartVolume};
    uint64_t now = 1000;
    bool ready = true, tool = false;
    int codec = NoStoryPage::kStartVolume;
    std::vector<Action> gestures;
    std::vector<Event> events;
    void Step(uint8_t pressed) {
        now += 10;
        const auto tick = page.Poll(pressed, now, ready, codec, std::exchange(tool, false));
        if (tick.volume_changed)
            codec = page.Volume();
        if (tick.gesture != Action::kNone)
            gestures.push_back(tick.gesture);
        if (tick.event != Event::kNone)
            events.push_back(tick.event);
    }
    void Hold(uint8_t pressed, uint64_t ms) {
        for (uint64_t elapsed = 0; elapsed < ms; elapsed += 10)
            Step(pressed);
    }
    void Tap(uint8_t pressed) {
        Hold(pressed, 60);
        Hold(0, 60);
    }
    void DoubleClick() {
        Tap(G::kPower);
        Tap(G::kPower);
        Hold(0, 50);
    }
    bool Saw(Action action) const {
        for (auto seen : gestures)
            if (seen == action)
                return true;
        return false;
    }
    bool Saw(Event event) const {
        for (auto seen : events)
            if (seen == event)
                return true;
        return false;
    }
    Session::Showing Shown() const { return page.Conversation().Shown(); }
    bool Refused() const { return page.Refused(now); }
};

static void StatusJson() {
    assert(DeviceStatusJson(60, -1) == R"({"audio_speaker":{"volume":60}})");
    assert(DeviceStatusJson(100, 87) == R"({"audio_speaker":{"volume":100},"battery":{"level":87}})");
    assert(DeviceStatusJson(0, 0) == R"({"audio_speaker":{"volume":0},"battery":{"level":0}})");
}

// A double-click talks at once (no story to hand over); the top key ends it
// and the page comes back.
static void TalkWithoutStory() {
    Board board;
    board.Hold(0, 100);
    assert(!board.page.Conversation().Active() && !board.Refused());
    board.DoubleClick();
    assert(board.Saw(Action::kOpenAi) && board.Saw(Event::kChatRequested));
    assert(board.port.starts == 1 && board.page.Conversation().Active());
    assert(board.Shown() == Session::Showing::kConnecting);
    board.port.app = Chat::kListening;
    board.Hold(0, 50);
    assert(board.Shown() == Session::Showing::kListening);
    board.port.app = Chat::kSpeaking;
    board.Hold(0, 50);
    assert(board.Shown() == Session::Showing::kSpeaking);
    board.Tap(G::kPower);  // The keys follow the AI rules now.
    assert(board.Saw(Action::kCancelAi) && !board.Saw(Action::kTogglePlayback));
    assert(board.port.closes == 1 && board.page.Conversation().Reason() == EndReason::kButton);
    assert(board.Shown() == Session::Showing::kReturning);
    board.port.app = Chat::kQuiet;
    board.Hold(0, 50);
    assert(board.Saw(Event::kResumed) && board.port.drops == 1);
    assert(!board.page.Conversation().Active());
    assert(board.port.starts == 1 && board.port.closes == 1);
}

// Not ready (no Wi-Fi, not activated, microphone fault): only a notice.
static void NotReady() {
    Board board;
    board.ready = false;
    board.Hold(0, 100);
    board.DoubleClick();
    assert(board.Saw(Action::kOfflineHint) && !board.Saw(Action::kOpenAi));
    assert(board.port.starts == 0 && !board.page.Conversation().Active() && board.Refused());
    board.Hold(0, 2000);
    assert(!board.Refused());
}

// The "back to story" tool also ends it; a late reply or a conversation still
// open when the page appeared (a card error mid-chat) is closed unasked.
static void ToolAndLateReplies() {
    Board board;
    board.Hold(0, 100);
    board.DoubleClick();
    board.port.app = Chat::kListening;
    board.Hold(0, 50);
    board.tool = true;
    board.Step(0);
    assert(board.port.closes == 1 && board.page.Conversation().Reason() == EndReason::kTool);
    board.port.app = Chat::kQuiet;
    board.Hold(0, 50);
    assert(!board.page.Conversation().Active());
    board.port.app = Chat::kSpeaking;  // A late reply.
    board.Step(0);
    assert(board.Saw(Event::kClosedUnsolicited) && board.port.closes == 2);
    assert(!board.page.Conversation().Active());

    Board error;  // The page appears while a conversation is still open.
    error.port.app = Chat::kListening;
    error.Step(0);
    assert(error.Saw(Event::kClosedUnsolicited) && error.port.closes == 1);
    assert(error.port.starts == 0);
}

// Volume starts at 60 and is never saved; the keys move it in steps of 5
// within 0..100, and a value XiaoZhi set is where the keys continue.
static void Volume() {
    Board board;
    board.Hold(0, 100);
    assert(board.page.Volume() == 60);
    board.Tap(G::kUp);
    assert(board.page.Volume() == 65 && board.codec == 65);
    board.Tap(G::kDown);
    board.Tap(G::kDown);
    assert(board.page.Volume() == 55 && board.codec == 55);
    board.codec = 30;  // self.audio_speaker.set_volume
    board.Step(0);
    assert(board.page.Volume() == 30);
    board.Tap(G::kUp);
    assert(board.page.Volume() == 35 && board.codec == 35);
    board.codec = 100;
    board.Tap(G::kUp);
    assert(board.page.Volume() == 100 && board.codec == 100);
    board.codec = 0;
    board.Tap(G::kDown);
    assert(board.page.Volume() == 0 && board.codec == 0);
    board.DoubleClick();  // The keys still set the volume during a conversation.
    board.port.app = Chat::kListening;
    board.Hold(0, 50);
    board.Tap(G::kUp);
    assert(board.page.Volume() == 5 && board.page.Conversation().Active());
}

// Story keys do nothing; the board powers off and opens the hotspot.
static void OtherKeys() {
    Board board;
    board.Hold(0, 100);
    board.Tap(G::kPower);
    board.Hold(0, 500);
    assert(board.Saw(Action::kTogglePlayback) && board.port.starts == 0);
    board.Hold(G::kUp, 900);
    board.Hold(0, 100);
    assert(board.page.Volume() == 60 && board.codec == 60);  // Next track: nothing.
    board.Hold(G::kUp | G::kDown, 5100);
    board.Hold(0, 100);
    assert(board.Saw(Action::kParentPageRequest));
    board.Hold(G::kPower, 2100);
    assert(board.Saw(Action::kShutdownRequest));
    assert(board.port.starts == 0 && !board.page.Conversation().Active());
}

int main() {
    StatusJson();
    TalkWithoutStory();
    NotReady();
    ToolAndLateReplies();
    Volume();
    OtherKeys();
}
'''


class M3AiDeviceHostTests(unittest.TestCase):
    def test_status_and_no_story_page_with_fake_port(self):
        compiler = shlex.split(os.environ["CXX"]) if "CXX" in os.environ else [shutil.which("c++")]
        self.assertTrue(compiler[0], "Native C++ compiler required; do not silently skip")
        with tempfile.TemporaryDirectory(prefix="m3-ai-device-") as temporary:
            source = Path(temporary) / "device_test.cc"
            source.write_text(SOURCE, encoding="utf-8")
            executable = Path(temporary) / "m3-ai-device-test"
            compiled = subprocess.run(
                compiler + ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                 "-I", str(BOARD), "-I", str(ROOT / "main"), str(source),
                 str(ROOT / "main/media/player_gestures.cc"), "-o", str(executable)],
                text=True, capture_output=True, timeout=60, cwd=ROOT)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            tested = subprocess.run([str(executable)], text=True, capture_output=True,
                                    timeout=10, cwd=temporary)
            self.assertEqual(tested.returncode, 0, tested.stdout + tested.stderr)


if __name__ == "__main__":
    unittest.main()
