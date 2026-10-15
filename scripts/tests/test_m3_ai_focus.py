"""Run StoryFocus's state mapping and Application::CloseConversation() unchanged.

Both are extracted from the production sources (m3_ai_main.cc and the method
batch 3 added to application.cc) and compiled against a fake Application, then
driven through the real session policy. A fake protocol stands in for upstream's
OnAudioChannelClosed, which returns Application to idle; this proves the board's
close path and mapping, not a real server's behavior.
"""

import os
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/boilon-s3-gen2"


def block(source, signature):
    """One complete, unchanged production function body, starting at signature."""
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    # The extracted bodies contain no braces in strings or comments.
    for position in range(opening, len(source)):
        depth += (source[position] == "{") - (source[position] == "}")
        if depth == 0:
            return source[start:position + 1]
    raise AssertionError(f"Unclosed production function: {signature}")


FAKES = r'''
#include <cassert>
#include <functional>
#include <memory>
#include <vector>
#include "device_state.h"
#include "m3_ai_session.h"

// Upstream returns to idle when the audio channel closes (OnAudioChannelClosed).
struct FakeProtocol {
    std::function<void()> on_closed;
    int closes = 0;
    void CloseAudioChannel() {
        ++closes;
        on_closed();
    }
};

class Application {
public:
    static Application& GetInstance() {
        static Application app;
        return app;
    }
    DeviceState GetDeviceState() const { return state; }
    void SetDeviceState(DeviceState next) {
        state = next;
        ++sets;
    }
    void StopNotification() { ++stops; }
    void Schedule(std::function<void()> task) { scheduled.push_back(std::move(task)); }
    void CloseConversation();
    void RunMainTask() {
        auto tasks = std::move(scheduled);
        scheduled.clear();
        for (auto& task : tasks)
            task();
    }
    void Reset(DeviceState next) {
        state = next;
        sets = stops = 0;
        scheduled.clear();
        protocol_ = std::make_unique<FakeProtocol>();
        protocol_->on_closed = [this] { SetDeviceState(kDeviceStateIdle); };
    }
    DeviceState state = kDeviceStateIdle;
    int sets = 0, stops = 0;
    std::vector<std::function<void()>> scheduled;
    std::unique_ptr<FakeProtocol> protocol_;
};
'''

CHECKS = r'''
using boilon::ai::Chat;

// StoryFocus's other duties (the story, the speaker) are fakes here.
struct Focus : boilon::ai::SessionPort {
    int resumes = 0, drops = 0, starts = 0;
    Chat App() override { return FocusApp(); }
    bool ParkStory(uint64_t) override { return true; }
    bool StoryReleased() override { return true; }
    bool StoryWaiting() override { return false; }
    bool ResumeStory(uint64_t) override {
        ++resumes;
        return true;
    }
    bool OutputIdle() override { return true; }
    void StartChat() override { ++starts; }
    void CloseChat() override { Application::GetInstance().CloseConversation(); }
    void DropAiAudio() override { ++drops; }
};

static void Mapping() {
    struct Case {
        DeviceState state;
        Chat chat;
    };
    const Case cases[] = {
        {kDeviceStateUnknown, Chat::kQuiet},       {kDeviceStateStarting, Chat::kQuiet},
        {kDeviceStateWifiConfiguring, Chat::kQuiet}, {kDeviceStateIdle, Chat::kQuiet},
        {kDeviceStateConnecting, Chat::kConnecting}, {kDeviceStateListening, Chat::kListening},
        {kDeviceStateSpeaking, Chat::kSpeaking},   {kDeviceStateNotifying, Chat::kBusy},
        {kDeviceStateUpgrading, Chat::kBusy},      {kDeviceStateActivating, Chat::kQuiet},
        {kDeviceStateAudioTesting, Chat::kBusy},   {kDeviceStateFatalError, Chat::kQuiet},
    };
    static_assert(sizeof(cases) / sizeof(cases[0]) == kDeviceStateFatalError + 1);
    auto& app = Application::GetInstance();
    for (const auto& item : cases) {
        app.Reset(item.state);
        assert(FocusApp() == item.chat);
        assert(upgrading_ == (item.state == kDeviceStateUpgrading));  // M5 batch 5 guard.
    }
}

// The close request runs later on the main task and only ends a conversation.
static void CloseRequests() {
    auto& app = Application::GetInstance();
    for (auto state : {kDeviceStateListening, kDeviceStateSpeaking}) {
        app.Reset(state);
        app.CloseConversation();
        assert(app.state == state && app.protocol_->closes == 0);
        app.RunMainTask();
        assert(app.protocol_->closes == 1 && app.state == kDeviceStateIdle);
    }
    app.Reset(kDeviceStateConnecting);  // The scheduled open sees idle and stops.
    app.CloseConversation();
    app.RunMainTask();
    assert(app.state == kDeviceStateIdle && app.sets == 1 && app.protocol_->closes == 0);
    app.Reset(kDeviceStateNotifying);
    app.CloseConversation();
    app.RunMainTask();
    assert(app.stops == 1 && app.state == kDeviceStateNotifying && app.protocol_->closes == 0);
    for (auto state : {kDeviceStateIdle, kDeviceStateStarting, kDeviceStateWifiConfiguring,
                       kDeviceStateActivating, kDeviceStateUpgrading}) {
        app.Reset(state);
        app.CloseConversation();
        app.RunMainTask();
        assert(app.state == state && app.sets == 0 && app.stops == 0);
        assert(app.protocol_->closes == 0);
    }
}

// The top key during a conversation: the story comes back only after the close
// ran on the main task and Application is idle again.
static void SessionEndsThroughClose() {
    for (auto talking : {kDeviceStateListening, kDeviceStateSpeaking, kDeviceStateConnecting}) {
        auto& app = Application::GetInstance();
        app.Reset(kDeviceStateIdle);
        Focus focus;
        boilon::ai::Session session(focus);
        assert(session.Open(0) && session.Poll(10) == boilon::ai::Event::kChatRequested);
        app.state = talking;  // WakeWordInvoke would start it.
        assert(session.Poll(20) == boilon::ai::Event::kNone);
        session.Close(boilon::ai::EndReason::kButton, 30);
        assert(app.scheduled.size() == 1);
        assert(session.Poll(40) == boilon::ai::Event::kNone && focus.resumes == 0);
        app.RunMainTask();
        assert(app.state == kDeviceStateIdle);
        assert(session.Poll(50) == boilon::ai::Event::kResumed);
        assert(focus.resumes == 1 && focus.drops == 1 && !session.Active());
    }
}

int main() {
    Mapping();
    CloseRequests();
    SessionEndsThroughClose();
}
'''


class M3AiFocusHostTests(unittest.TestCase):
    def test_state_mapping_and_close_conversation_from_production_sources(self):
        compiler = shlex.split(os.environ["CXX"]) if "CXX" in os.environ else [shutil.which("c++")]
        self.assertTrue(compiler[0], "Native C++ compiler required; do not silently skip")
        application = (ROOT / "main/application.cc").read_text(encoding="utf-8")
        board = (BOARD / "m3_ai_main.cc").read_text(encoding="utf-8")
        close = block(application, "void Application::CloseConversation() {")
        focus = board.split("class StoryFocus final", 1)[1]
        mapping = block(focus, "boilon::ai::Chat App() override {")
        mapping = mapping.replace("boilon::ai::Chat App() override {",
                                  "boilon::ai::Chat FocusApp() {", 1)
        self.assertIn("Application::GetInstance().GetDeviceState()", mapping)
        self.assertIn("protocol_->CloseAudioChannel();", close)
        # App() also records the upgrade state in a StoryFocus member.
        source = (FAKES + "\n" + close + "\nstatic bool upgrading_ = false;\n" + mapping +
                  "\n" + CHECKS)
        with tempfile.TemporaryDirectory(prefix="m3-ai-focus-") as temporary:
            path = Path(temporary) / "focus_test.cc"
            path.write_text(source, encoding="utf-8")
            executable = Path(temporary) / "m3-ai-focus-test"
            compiled = subprocess.run(
                compiler + ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                            "-I", str(BOARD), "-I", str(ROOT / "main"), str(path),
                            "-o", str(executable)],
                text=True, capture_output=True, timeout=60, cwd=ROOT)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            tested = subprocess.run([str(executable)], text=True, capture_output=True,
                                    timeout=10, cwd=temporary)
            self.assertEqual(tested.returncode, 0, tested.stdout + tested.stderr)


if __name__ == "__main__":
    unittest.main()
