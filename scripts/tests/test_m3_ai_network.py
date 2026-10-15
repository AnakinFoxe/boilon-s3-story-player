"""Run the M3 batch 2 network policy (m3_ai_network.h) unchanged against a fake port.

The board adapts this controller to WifiManager and Application; here a fake
records every Wi-Fi action, so the tests prove when the AI variant may touch
Wi-Fi and the hotspot, not that a real radio or server works.
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
#include <string>
#include "m3_ai_network.h"
using namespace boilon::ai;

struct FakePort : NetworkPort {
    size_t saved = 0;
    bool init_ok = true, can_configure = true, open = false, activating = false, ota_url = false;
    int inits = 0, stations = 0, opens = 0, closes = 0;
    size_t SavedWifi() override { return saved; }
    bool InitWifi() override { ++inits; return init_ok; }
    void StartStation() override { ++stations; }
    bool CanConfigure() override { return can_configure; }
    void OpenHotspot() override { ++opens; open = true; }
    void CloseHotspot() override { ++closes; }
    bool HotspotOpen() override { return open; }
    std::string HotspotName() override { return "Boilon-AB12"; }
    bool Activating() override { return activating; }
    bool OtaUrlSet() override { return ota_url; }
};

static void StartPolicy() {
    struct Case { bool storage, ota_url; size_t saved; NetworkStart start; };
    for (const auto& c : {Case{false, false, 1, NetworkStart::kStorageError},
                          Case{false, true, 0, NetworkStart::kStorageError},
                          Case{true, true, 1, NetworkStart::kServerOverride},
                          Case{true, false, 0, NetworkStart::kNoSavedWifi},
                          Case{true, false, 2, NetworkStart::kConnect}}) {
        FakePort port;
        port.saved = c.saved;
        NetworkController controller(port, c.storage, c.ota_url);
        assert(controller.Start() == c.start);
        const bool connect = c.start == NetworkStart::kConnect;
        // A failing NVS never reaches Wi-Fi init (which would erase it), and
        // no saved network means the radio stays off exactly as in batch 1.
        assert(port.inits == (connect ? 1 : 0) && port.stations == (connect ? 1 : 0));
        assert(port.opens == 0 && !controller.Poll(1000));
    }
    FakePort port;
    port.saved = 1;
    port.init_ok = false;  // Identity not saved or Wi-Fi init failed.
    NetworkController controller(port, true, false);
    assert(controller.Start() == NetworkStart::kConnect);
    assert(port.inits == 1 && port.stations == 0 && port.opens == 0);
}

// M5 batch 5: with less than a page of NVS room the board treats storage as
// failed, so the policy above keeps Wi-Fi and the hotspot off.
static void NvsRoom() {
    assert(!NvsRoomForNetwork(0) && !NvsRoomForNetwork(kMinNvsEntries - 1));
    assert(NvsRoomForNetwork(kMinNvsEntries) && NvsRoomForNetwork(730));
}

static void FailedConnectionNeverOpensHotspot() {
    FakePort port;
    port.saved = 1;
    NetworkController controller(port, true, false);
    controller.Start();
    for (uint64_t now = 0; now < 2 * kHotspotMs; now += 1000)
        assert(!controller.Poll(now));
    assert(port.opens == 0 && port.closes == 0);
}

static void RefusedHotspot() {
    struct Case { bool storage, ota_url, can_configure, init_ok; };
    for (const auto& c : {Case{false, false, true, true}, Case{true, true, true, true},
                          Case{true, false, false, true}, Case{true, false, true, false}}) {
        FakePort port;
        port.can_configure = c.can_configure;
        port.init_ok = c.init_ok;
        NetworkController controller(port, c.storage, c.ota_url);
        assert(!controller.OpenHotspot(10000) && port.opens == 0);
        // Wi-Fi is not even initialized for storage or server-override refusals.
        assert(port.inits == (c.storage && !c.ota_url && c.can_configure ? 1 : 0));
        auto notice = controller.Poll(10000 + kNoticeMs - 1);
        assert(notice && notice->status == "配网" && notice->title == "现在不能配网");
        assert(!controller.Poll(10000 + kNoticeMs));
    }
}

static void HotspotLifetime() {
    FakePort port;
    NetworkController controller(port, true, false);
    assert(controller.OpenHotspot(1000) && port.inits == 1 && port.opens == 1);
    auto notice = controller.Poll(2000);
    assert(notice && notice->status == "配网");
    assert(notice->title == "手机连接热点 Boilon-AB12，再打开 192.168.4.1");
    // Asking again while open only extends the deadline.
    assert(controller.OpenHotspot(5000) && port.opens == 1);
    assert(controller.Poll(5000 + kHotspotMs - 1) && port.closes == 0);
    assert(!controller.Poll(5000 + kHotspotMs) && port.closes == 1);
    controller.Poll(5000 + kHotspotMs + 10);  // Closing is asynchronous; ask only once.
    assert(port.closes == 1);
    // Closing restarts the station only when a network was saved.
    port.open = false;
    controller.HotspotClosed();
    assert(port.stations == 0);
    assert(controller.OpenHotspot(900000) && port.opens == 2);
    port.saved = 1;  // The parent saved the home network on the page.
    port.open = false;
    controller.HotspotClosed();  // Web exit before the deadline.
    assert(port.stations == 1);
    port.open = true;  // A stale deadline must not close a later hotspot.
    assert(controller.Poll(900000 + kHotspotMs) && port.closes == 1);
    // The page saved a server address (the hidden advanced field): the station
    // stays off this boot too, so the identity never goes to that address.
    port.open = false;
    port.ota_url = true;
    controller.HotspotClosed();
    assert(port.stations == 1);
    port.ota_url = false;  // Cleared on the page again.
    controller.HotspotClosed();
    assert(port.stations == 2);
}

static void ActivationNotice() {
    assert(ActivationCode("请登录到控制面板添加设备，输入验证码：123456") == "123456");
    assert(ActivationCode("12345 1234567 654321") == "654321");
    assert(ActivationCode("code 12345").empty() && ActivationCode("").empty());

    FakePort port;
    port.activating = true;
    NetworkController controller(port, true, false);
    controller.Activation("xiaozhi.me 验证码 246810");
    auto notice = controller.Poll(1000);
    assert(notice && notice->status == "246810");
    assert(notice->title == "请在 xiaozhi.me 添加设备，输入上面的激活码");
    controller.Activation("请到控制台激活");  // No code: show the server's words.
    notice = controller.Poll(1100);
    assert(notice && notice->status == "激活" && notice->title == "请到控制台激活");
    port.activating = false;  // Activation finished.
    notice = controller.Poll(2000);
    assert(notice && notice->status == "已激活" && notice->title.empty());
    assert(controller.Poll(2000 + kNoticeMs - 1) && !controller.Poll(2000 + kNoticeMs));

    // An error alert after the prompt (retries exhausted) is not success.
    port.activating = true;
    controller.Activation("验证码 135790");
    controller.Activation(nullptr);
    assert(controller.Poll(9000)->status == "135790");
    port.activating = false;
    assert(!controller.Poll(9100));
    // Errors without a prompt never invent an activation.
    controller.Activation(nullptr);
    assert(!controller.Poll(9200));
}

int main() {
    StartPolicy();
    NvsRoom();
    FailedConnectionNeverOpensHotspot();
    RefusedHotspot();
    HotspotLifetime();
    ActivationNotice();
}
'''


class M3AiNetworkHostTests(unittest.TestCase):
    def test_network_policy_with_fake_port(self):
        compiler = shutil.which("c++")
        self.assertTrue(compiler, "Native C++ compiler required; do not silently skip")
        with tempfile.TemporaryDirectory(prefix="m3-ai-network-") as temporary:
            executable = Path(temporary) / "m3-ai-network-test"
            compiled = subprocess.run(
                [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
                 "-I", str(BOARD), "-x", "c++", "-", "-o", str(executable)],
                input=SOURCE, text=True, capture_output=True, timeout=30, cwd=ROOT)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            tested = subprocess.run([str(executable)], text=True, capture_output=True,
                                    timeout=10, cwd=temporary)
            self.assertEqual(tested.returncode, 0, tested.stdout + tested.stderr)

    def test_board_keeps_the_network_off_when_nvs_is_nearly_full(self):
        board = (BOARD / "m3_ai_board.cc").read_text(encoding="utf-8")
        inspect = board.split("void InspectStorage(esp_err_t nvs_result) {", 1)[1]
        inspect = inspect.split("\n}\n", 1)[0]
        # app_main runs this before the board (and its network controller) exists.
        self.assertIn("if (!counted || !NvsRoomForNetwork(stats.available_entries)) {\n"
                      "        storage_ready = false;", inspect)
        self.assertLess(inspect.index("nvs_get_stats("), inspect.index("NvsRoomForNetwork("))


if __name__ == "__main__":
    unittest.main()
