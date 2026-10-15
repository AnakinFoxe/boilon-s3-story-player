"""Boilon volume scale, button range, saved-volume cap, adopted volume and volume pixels."""

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

SOURCE = r'''
#include "boards/boilon-s3-gen2/m2_local_policy.h"
#include "boards/boilon-s3-gen2/m2_library_runtime.h"
#include <cassert>
#include <climits>
#include <cmath>
#include <iostream>

namespace library = boilon::local_library;
namespace player = boilon::local_player;

void Gain() {
    // 100 plays PCM unscaled at the jiuchuan-s3 cap; 0 and below mute.
    assert(player::kMaxVolume == 100 && player::kDacGainDb == -6.5f);
    assert(player::kDacVolumeRegister == 0xB2);
    assert(player::kVolumeGains[0] == 0 && player::kVolumeGains[100] == 32768);
    // Each step of 5 is 2.5 dB, so 5 sits 47.5 dB below 100.
    for (int volume = 10; volume <= 100; volume += 5) {
        const double db = 20 * std::log10(double(player::kVolumeGains[volume]) /
                                          player::kVolumeGains[volume - 5]);
        assert(std::fabs(db - 2.5) < 0.1);
    }
    const double lowest = 20 * std::log10(player::kVolumeGains[5] / 32768.0);
    assert(std::fabs(lowest + 47.5) < 0.1);
    for (int volume = 1; volume <= 100; ++volume)
        assert(player::kVolumeGains[volume] > player::kVolumeGains[volume - 1]);
    for (int sample : {-32768, -32767, -1000, -1, 0, 1, 1000, 32767}) {
        assert(player::VolumeSample(int16_t(sample), 100) == sample);
        assert(player::VolumeSample(int16_t(sample), 101) == sample);
        assert(player::VolumeSample(int16_t(sample), 200) == sample);
        assert(player::VolumeSample(int16_t(sample), INT_MAX) == sample);
        assert(player::VolumeSample(int16_t(sample), 0) == 0);
        assert(player::VolumeSample(int16_t(sample), -1) == 0);
        for (int volume = 1; volume <= 100; ++volume)
            assert(player::VolumeSample(int16_t(sample), volume) ==
                   sample * player::kVolumeGains[volume] / 32768);
    }
}

struct Player {
    enum class Result { kIdle, kRunning, kFinished, kStopped, kError };
    bool running = false;
    Result result = Result::kIdle;
    bool Start(const std::string&) { running = true; result = Result::kRunning; return true; }
    bool Running() const { return running; }
    Result LastResult() const { return result; }
    bool ReadFailed() const { return false; }
    bool Pause(bool) { return true; }
    void Stop() { running = false; result = Result::kStopped; }
};
// Resume-capable player for the restore path.
struct ResumingPlayer : Player {
    bool StartAt(const std::string& path, uint64_t) { return Start(path); }
    uint64_t PositionMs() const { return 0; }
    uint64_t DurationMs() const { return 0; }
};
// Hands the runtime one prepared boot plan.
struct PlanPort : library::ResumePort {
    media::ResumePlan plan;
    bool pending = false;
    std::vector<int> saved;  // The volume of every checkpoint.
    bool Restore(std::optional<media::MediaId>, media::ResumeMode) override {
        pending = true;
        return true;
    }
    bool TakeRestore(media::ResumePlan& out) override {
        if (!pending)
            return false;
        pending = false;
        out = plan;
        return true;
    }
    bool Checkpoint(const media::ResolvedTrack&, uint64_t, uint64_t, int volume, uint64_t,
                    media::CheckpointReason) override {
        saved.push_back(volume);
        return true;
    }
};
media::MediaCatalog Catalog() {
    media::MediaCatalogBuilder builder("volume-fixture");
    assert(builder.Add({"", "story.mp3", 1000, 1}) == media::AddResult::kAdded);
    auto built = builder.Finish();
    assert(built.Ok());
    return std::move(built.catalog);
}
void Buttons() {
    Player player;
    library::Runtime<Player> runtime(Catalog(), player, [] { return true; });
    uint64_t now = 0;
    assert(runtime.Start(now));
    auto edge = [&](uint8_t mask) {
        assert(runtime.Tick(mask, ++now));
        now += 31;
        assert(runtime.Tick(mask, now));
    };
    auto tap = [&](uint8_t mask) {
        edge(mask);
        now += 70;
        assert(runtime.Tick(mask, now));
        edge(0);
    };
    edge(0);
    assert(runtime.View(now).volume == 10);
    for (int i = 1; i <= 25; ++i) {
        tap(media::PlayerGestures::kUp);
        assert(runtime.View(now).volume == std::min(100, 10 + i * 5));
    }
    for (int i = 1; i <= 25; ++i) {
        tap(media::PlayerGestures::kDown);
        assert(runtime.View(now).volume == std::max(0, 100 - i * 5));
    }
    assert(player.Running());
}
// Records saved before the rescale may hold up to 200; they restore at 100.
void Restore() {
    for (const int saved : {180, 100, 60, 0}) {
        ResumingPlayer resuming;
        PlanPort port;
        const auto catalog = Catalog();
        port.plan.status = media::ResumeStatus::kReady;
        port.plan.target = catalog.Resolve(catalog.Collections()[0].tracks[0].id);
        port.plan.volume_percent = saved;
        int applied = -1;
        library::Runtime<ResumingPlayer> runtime(
            Catalog(), resuming, [] { return true; }, {}, &port,
            [&applied](int volume) { applied = volume; });
        assert(runtime.Start(0));
        assert(runtime.Tick(0, 1));
        const int expected = std::min(saved, 100);
        assert(applied == expected);
        assert(runtime.View(1).volume == expected);
        assert(resuming.Running());
    }
}
// A volume XiaoZhi set (AI variant): shown like a key press, clamped to the
// scale, saved with the next checkpoint, and the keys continue from it.
void Adopt() {
    ResumingPlayer resuming;
    PlanPort port;
    const auto catalog = Catalog();
    port.plan.status = media::ResumeStatus::kReady;
    port.plan.target = catalog.Resolve(catalog.Collections()[0].tracks[0].id);
    port.plan.volume_percent = 60;
    library::Runtime<ResumingPlayer> runtime(Catalog(), resuming, [] { return true; }, {}, &port,
                                             [](int) {});
    uint64_t now = 0;
    assert(runtime.Start(now));
    assert(runtime.Tick(0, ++now) && runtime.Volume() == 60);
    runtime.AdoptVolume(35, now);
    assert(runtime.Volume() == 35 && runtime.View(now).volume == 35);
    assert(runtime.View(now).volume_visible && !runtime.View(now + 2000).volume_visible);
    auto edge = [&](uint8_t mask) {
        assert(runtime.Tick(mask, ++now));
        now += 31;
        assert(runtime.Tick(mask, now));
    };
    edge(0);
    edge(media::PlayerGestures::kUp);
    now += 70;
    assert(runtime.Tick(media::PlayerGestures::kUp, now));
    edge(0);
    assert(runtime.Volume() == 40);
    port.saved.clear();
    assert(runtime.Shutdown(now) && port.saved == std::vector<int>{40});
    runtime.AdoptVolume(150, now);
    assert(runtime.Volume() == 100);
    runtime.AdoptVolume(-5, now);
    assert(runtime.Volume() == 0);
    assert(resuming.Running());
}
int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string scenario = argv[1];
    if (scenario == "gain") Gain();
    else if (scenario == "buttons") Buttons();
    else if (scenario == "restore") Restore();
    else if (scenario == "adopt") Adopt();
    else return 2;
    std::cout << scenario << " PASS\n";
}
'''


class M2VolumeScaleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ["CXX"]) if "CXX" in os.environ else [shutil.which("c++")]
        if not compiler or not compiler[0]:
            raise AssertionError("existing native C++ compiler required; no download")
        cls.temporary = tempfile.TemporaryDirectory(prefix="m2-volume-scale-")
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        source = directory / "scale.cc"
        source.write_text(SOURCE, encoding="utf-8")
        cls.executable = directory / "scale-test"
        command = compiler + ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                              "-I", str(ROOT / "main"), str(source),
                              str(ROOT / "main/media/media_catalog.cc"),
                              str(ROOT / "main/media/player_controller.cc"),
                              str(ROOT / "main/media/player_gestures.cc"),
                              "-o", str(cls.executable)]
        result = subprocess.run(command, capture_output=True, text=True,
                                stdin=subprocess.DEVNULL, timeout=30, cwd=ROOT)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def scenario(self, name):
        result = subprocess.run([str(self.executable), name], capture_output=True, text=True,
                                stdin=subprocess.DEVNULL, timeout=10, cwd=ROOT)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(name + " PASS", result.stdout)

    def test_steps_are_2_5_db_and_100_plays_pcm_unscaled_at_the_dac_cap(self):
        self.scenario("gain")

    def test_actual_owner_steps_to_100_then_down_to_zero_without_wrap(self):
        self.scenario("buttons")

    def test_saved_volume_above_100_restores_at_100(self):
        self.scenario("restore")

    def test_adopted_volume_shows_saves_and_keys_continue_from_it(self):
        self.scenario("adopt")


if __name__ == "__main__":
    unittest.main()
