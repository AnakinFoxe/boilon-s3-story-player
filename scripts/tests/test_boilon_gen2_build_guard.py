import contextlib
import csv
import importlib.util
import io
import json
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("build", ROOT / "scripts/build.py")
build = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(build)


class BoilonGen2ConfigurationTests(unittest.TestCase):
    def test_library_gpio_polarity_matches_accepted_board(self):
        for entry in ("m3_ai_main.cc",):
            with self.subTest(entry=entry):
                source = (ROOT / "main/boards/boilon-s3-gen2" / entry).read_text(
                    encoding="utf-8"
                )
                pressed = source.split("uint8_t Pressed() {", 1)[1].split("\n}", 1)[0]
                self.assertIn("gpio_get_level(GPIO_NUM_3) ? G::kPower : 0", pressed)
                self.assertIn("gpio_get_level(GPIO_NUM_6) ? 0 : G::kUp", pressed)
                self.assertIn("gpio_get_level(GPIO_NUM_7) ? 0 : G::kDown", pressed)

    def test_variants_are_explicit_and_app_only(self):
        config_path = ROOT / "main/boards/boilon-s3-gen2/config.json"
        config = json.loads(config_path.read_text(encoding="utf-8"))

        self.assertEqual(config["type"], "boilon-s3-gen2")
        self.assertEqual(config["target"], "esp32s3")
        variants = {variant["name"]: variant for variant in config["builds"]}
        # The exploration-stage diagnostics were removed after dc0cc229, the
        # offline-only local-library variant after v1.4.0.
        self.assertEqual(set(variants), {"boilon-s3-gen2-ai"})

        common_options = {
            "CONFIG_FLASH_NONE_ASSETS=y",
            "CONFIG_ESP_SYSTEM_PANIC_PRINT_HALT=y",
            "CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y",
            "CONFIG_ESP_CONSOLE_SECONDARY_NONE=y",
            "CONFIG_COMPILER_CXX_EXCEPTIONS=n",
            "CONFIG_COMPILER_CXX_RTTI=n",
            "CONFIG_APP_REPRODUCIBLE_BUILD=y",
            'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions/boilon-s3-gen2.csv"',
        }
        for variant in variants.values():
            self.assertIs(variant["merge_bin"], False)
            self.assertEqual(variant["app_size_limit"], 0x3E0000)
            # Warn when an app nears the partition (3.5 MiB of 3,968 KiB); the old
            # 1 MiB diagnostic budget made every AI build warn.
            self.assertEqual(variant["app_size_warning"], 0x380000)
            self.assertTrue(common_options.issubset(set(variant["sdkconfig_append"])))

        ai_options = set(variants["boilon-s3-gen2-ai"]["sdkconfig_append"])
        self.assertIn("CONFIG_BOILON_S3_GEN2_AI=y", ai_options)
        self.assertTrue({
            "CONFIG_SD_ENABLE_SDIO_SUPPORT=n",
            "CONFIG_FATFS_LFN_HEAP=y",
            "CONFIG_FATFS_API_ENCODING_UTF_8=y",
            "CONFIG_CODEC_ES8311_SUPPORT=y",
            "CONFIG_CODEC_I2C_BACKWARD_COMPATIBLE=n",
            # No wake-word model yet; see M3_ARCHITECTURE.md.
            "CONFIG_WAKE_WORD_DISABLED=y",
            # Keeps the 4096-byte audio_output stack LocalCodec::Write needs
            # (test_m3_ai_host); upstream only creates the AFE engine at Idle.
            "CONFIG_USE_AUDIO_PROCESSOR=y",
        }.issubset(ai_options))
        for variant in variants.values():
            options = set(variant["sdkconfig_append"])
            self.assertEqual(
                {option for option in options if option.startswith("CONFIG_BOILON_S3_GEN2_")},
                {"CONFIG_BOILON_S3_GEN2_AI=y"})

        kconfig = (ROOT / "main/Kconfig.projbuild").read_text(encoding="utf-8")
        variant_choice = kconfig.split(
            "choice BOILON_S3_GEN2_DIAGNOSTIC_STAGE", 1
        )[1].split("endchoice", 1)[0]
        self.assertIn("depends on BOARD_TYPE_BOILON_S3_GEN2", variant_choice)
        self.assertIn("default BOILON_S3_GEN2_AI", variant_choice)
        self.assertEqual(variant_choice.count("config BOILON_S3_GEN2_"), 1)
        self.assertIn("config BOILON_S3_GEN2_AI", variant_choice)
        self.assertNotIn("LOCAL_LIBRARY", kconfig)

        discovered = {
            item["name"]: item
            for item in build._collect_variants(idf_version=(6, 0, 2))
            if item["board"] == "boilon-s3-gen2"
        }
        self.assertEqual(set(discovered), set(variants))
        for variant in discovered.values():
            self.assertIs(variant["merge_bin"], False)
            self.assertEqual(variant["artifact_path"], "build/xiaozhi.bin")

    def test_local_player_preserves_accepted_memory_policy(self):
        config = json.loads(
            (ROOT / "main/boards/boilon-s3-gen2/config.json").read_text(encoding="utf-8")
        )
        defaults = []
        for path in (ROOT / "sdkconfig.defaults", ROOT / f"sdkconfig.defaults.{config['target']}"):
            defaults.extend(path.read_text(encoding="utf-8").splitlines())
        selected = {"boilon-s3-gen2-ai"}
        variants = [variant for variant in config["builds"] if variant["name"] in selected]
        self.assertEqual({variant["name"] for variant in variants}, selected)
        for variant in variants:
            with self.subTest(variant=variant["name"]):
                effective = {}
                for line in defaults + variant["sdkconfig_append"]:
                    assignment = re.fullmatch(r"(CONFIG_[A-Z0-9_]+)=(.*)", line.strip())
                    disabled = re.fullmatch(r"# (CONFIG_[A-Z0-9_]+) is not set", line.strip())
                    if assignment:
                        effective[assignment[1]] = assignment[2]
                    elif disabled:
                        effective[disabled[1]] = "n"
                self.assertEqual(effective["CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL"], "2048")
                self.assertEqual(effective["CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL"], "98304")
                self.assertEqual(effective["CONFIG_LV_USE_CLIB_MALLOC"], "y")
                # The accepted baseline selected CLIB; its inactive choice member
                # need not be written in defaults. This checks declared policy,
                # not the configured heap or an actual LVGL allocation.
                self.assertEqual(effective.get("CONFIG_LV_USE_BUILTIN_MALLOC", "n"), "n")

    def test_partition_geometry_matches_factory_contract(self):
        partition_path = ROOT / "partitions/boilon-s3-gen2.csv"
        with partition_path.open(encoding="utf-8", newline="") as file:
            rows = [
                [field.strip() for field in row]
                for row in csv.reader(
                    line for line in file if not line.lstrip().startswith("#")
                )
                if row
            ]

        self.assertEqual(
            rows,
            [
                ["nvs", "data", "nvs", "0x9000", "0x8000", ""],
                ["otadata", "data", "ota", "0x11000", "0x2000", ""],
                ["phy_init", "data", "phy", "0x13000", "0x1000", ""],
                ["ota_0", "app", "ota_0", "0x20000", "0x3e0000", ""],
                ["ota_1", "app", "ota_1", "0x400000", "0x3e0000", ""],
                ["assets", "data", "spiffs", "0x800000", "0x800000", ""],
            ],
        )
        self.assertEqual(int(rows[-1][3], 16) + int(rows[-1][4], 16), 0x1000000)

    def test_retired_entries_stay_removed(self):
        cmake = (ROOT / "main/CMakeLists.txt").read_text(encoding="utf-8")
        board_dir = ROOT / "main/boards/boilon-s3-gen2"

        self.assertIn('set(BOARD_DIR "boilon-s3-gen2")', cmake)
        self.assertIn("BOILON S3 Gen2 requires CONFIG_BOILON_S3_GEN2_AI", cmake)
        # The exploration entries (removed after dc0cc229), and the offline-only
        # local-library entry with its board factory (retired after v1.4.0).
        for removed in ("diagnostic_main.cc", "p1_3_passive_main.cc", "p1_4_lcd_main.cc",
                        "p1_5_sd_main.cc", "p1_5_audio_main.cc", "p1_5_mic_main.cc",
                        "p1_6_network_main.cc", "m2_local_main.cc", "m2_library_main.cc",
                        "boilon_s3_gen2_board.cc"):
            self.assertFalse((board_dir / removed).exists(), removed)
            self.assertNotIn(removed, cmake)
        for symbol in ("DIAGNOSTIC_BUILD", "DIAGNOSTIC_ENTRY", "LOCAL_MP3_BUILD", "LOCAL_LIBRARY"):
            self.assertNotIn(symbol, cmake)
        factories = [path.name for path in sorted(board_dir.glob("*.cc"))
                     if "DECLARE_BOARD(" in path.read_text(encoding="utf-8")]
        self.assertEqual(factories, ["m3_ai_board.cc"])

    def test_ai_variant_runs_application_beside_offline_player(self):
        board_dir = ROOT / "main/boards/boilon-s3-gen2"
        cmake = (ROOT / "main/CMakeLists.txt").read_text(encoding="utf-8")
        board_source = (board_dir / "m3_ai_board.cc").read_text(encoding="utf-8")
        source = (board_dir / "m3_ai_main.cc").read_text(encoding="utf-8")

        self.assertIn("    if(CONFIG_BOILON_S3_GEN2_AI)\n", cmake)
        ai_sources = cmake.split("if(BOILON_S3_GEN2_AI_BUILD)", 1)[1].split("endif()", 1)[0]
        self.assertIn('list(FILTER SOURCES EXCLUDE REGEX "/boards/boilon-s3-gen2/")', ai_sources)
        self.assertIn('list(REMOVE_ITEM SOURCES "main.cc")', ai_sources)
        for entry in ('"boards/boilon-s3-gen2/m3_ai_board.cc"',
                      '"boards/boilon-s3-gen2/m3_ai_main.cc"', '"media/playback_resume.cc"'):
            self.assertIn(entry, ai_sources)
        self.assertNotIn("boilon_s3_gen2_board.cc", ai_sources)

        # Batch 2: Wi-Fi through WifiManager's public API, never WifiBoard's
        # automatic hotspot or Board()'s aborting identity write.
        self.assertTrue(board_source.startswith('#include "sdkconfig.h"\n#if CONFIG_BOILON_S3_GEN2_AI\n'))
        self.assertEqual(board_source.count("DECLARE_BOARD("), 1)
        self.assertIn("Board(TransientIdentityTag{})", board_source)
        for forbidden in ("public WifiBoard", "EnterWifiConfigMode", "StartWifiConfigMode",
                          "TryWifiConnect", "nvs_flash_init", "nvs_flash_erase", "esp_wifi_init",
                          "EraseKey", "EraseAll", "Settings(name_space, true)"):
            self.assertNotIn(forbidden, board_source)
        self.assertEqual(board_source.count("Settings("), 1)  # The read-only helper.
        self.assertIn("Settings(name_space, false)", board_source)
        self.assertEqual(board_source.count("nvs_set_str("), 1)
        self.assertIn('nvs_set_str(handle, "uuid"', board_source)
        self.assertIn('config.ssid_prefix = "Boilon";', board_source)
        self.assertIn("config.show_ota_config = false;", board_source)
        self.assertEqual(board_source.count("StartConfigAp()"), 1)
        open_hotspot = board_source.split("void OpenHotspot() override", 1)[1].split("\n    }", 1)[0]
        self.assertIn("StartConfigAp()", open_hotspot)
        self.assertEqual(board_source.count(".Initialize(config)"), 1)
        init_wifi = board_source.split("bool InitWifi() override", 1)[1].split("\n    }", 1)[0]
        self.assertIn(".Initialize(config)", init_wifi)
        start_network = board_source.split("void StartNetwork() override", 1)[1].split("\n    }", 1)[0]
        self.assertIn("->Start()", start_network)
        self.assertNotIn("Hotspot", start_network)
        policy = (board_dir / "m3_ai_network.h").read_text(encoding="utf-8")
        self.assertNotIn("#include <esp", policy)
        self.assertEqual(policy.count("port_.OpenHotspot()"), 1)
        self.assertEqual(policy.count("port_.InitWifi()"), 2)  # Start() and OpenHotspot().

        # The factory assets partition stays invisible to the AI variant.
        assets = (ROOT / "main/assets.cc").read_text(encoding="utf-8")
        self.assertIn('#ifndef PARTITION_LABEL\n#define PARTITION_LABEL "assets"\n#endif\n', assets)
        self.assertIn("ESP_PARTITION_SUBTYPE_ANY,\n                                                  PARTITION_LABEL)", assets)
        self.assertIn('set_source_files_properties("assets.cc" PROPERTIES\n'
                      '        COMPILE_DEFINITIONS "PARTITION_LABEL=\\"boilon_none\\"")', ai_sources)
        labels = [line.split(",", 1)[0].strip() for line in
                  (ROOT / "partitions/boilon-s3-gen2.csv").read_text(encoding="utf-8").splitlines()
                  if line.strip() and not line.startswith("#")]
        self.assertIn("assets", labels)
        self.assertNotIn("boilon_none", labels)

        self.assertTrue(source.startswith('#include "sdkconfig.h"\n#if CONFIG_BOILON_S3_GEN2_AI\n'))
        for forbidden in ("nvs_flash_erase(", "esp_wifi_init(", "esp_ota_set_boot_partition(",
                          "esp_ota_mark_app_valid_cancel_rollback(", "InitializePlaybackOnly(",
                          # The player never drives Application's state machine.
                          "SetDeviceState(", "Schedule(", "ToggleChatState(", "StartListening(",
                          "PlaySound(", "Alert(", "ResetProtocol(", "AbortSpeaking("):
            self.assertNotIn(forbidden, source)
        # Batch 3: only the session adapter talks to Application: it reads the
        # state, starts a conversation like a wake word and closes it.
        focus = source.split("class StoryFocus final", 1)[1].split("\n};", 1)[0]
        outside = source.replace(focus, "")
        for call in ("GetDeviceState()", "WakeWordInvoke(", "CloseConversation()"):
            self.assertEqual(focus.count(call), 1, call)
            self.assertNotIn(call, outside)
        self.assertEqual(outside.count("Application::GetInstance()"), 2)
        session = (board_dir / "m3_ai_session.h").read_text(encoding="utf-8")
        self.assertNotIn("#include <esp", session)
        self.assertNotIn("application.h", session)
        upstream = (ROOT / "main/application.cc").read_text(encoding="utf-8")
        close = upstream.split("void Application::CloseConversation() {", 1)[1].split("\n}\n", 1)[0]
        self.assertIn("Schedule(", close)  # On the main task, like ResetProtocol().
        self.assertIn("protocol_->CloseAudioChannel();", close)
        self.assertNotIn("protocol_.reset()", close)  # Later conversations still work.
        self.assertIn("Application::GetInstance().GetAudioService()", source)
        self.assertIn("Board::GetInstance().GetAudioCodec()", source)
        # Existing capture summaries match this tag.
        self.assertIn('constexpr char kTag[] = "BoilonLibrary";', source)

        self.assertIn("if (gesture == Action::kParentPageRequest)\n"
                      "                boilon::ai::RequestHotspot();", source)
        # Batch 4: no card, an empty card and card errors share the no-story
        # page, which keeps XiaoZhi and the hotspot gesture and powers off only
        # on a long top press. The page's rules live in a header without SDK
        # includes (test_m3_ai_device).
        no_story = source.split("[[noreturn]] void NoStory(", 1)[1].split("\n}\n", 1)[0]
        # A long top press, or (2026-10-15) 喵喵's power-off after her answer.
        self.assertEqual(no_story.count("PowerOff("), 2)
        self.assertIn('if (tick.gesture == Action::kShutdownRequest)\n            PowerOff("USER");',
                      no_story)
        self.assertIn('if (power_off && !page.Conversation().Active())\n            PowerOff("USER");',
                      no_story)
        self.assertIn("StoryFocus focus(nullptr, audio);", no_story)
        self.assertIn("boilon::ai::RequestHotspot();", no_story)
        self.assertIn("boilon::ai::PollNetwork(now)", no_story)
        # M5 batch 4: without a card there is no storage worker, so the page
        # samples the same ADC with the same estimate and shows/reports it.
        self.assertIn("const auto reading = Battery().Read();", no_story)
        self.assertIn("battery_level = battery.Update(reading);", no_story)
        self.assertIn("model.battery = battery_level;", no_story)
        self.assertIn("uint64_t sampled_ms = NowMs();", no_story)
        self.assertIn("ResumeWorker resume_worker(catalog, mounted_library, &Battery());", source)
        for page in ("[[noreturn]] void EmptyLibrary(", "[[noreturn]] void MediaError("):
            definition = source.split(page)[2].split("\n}\n", 1)[0]
            self.assertNotIn("PowerOff(", definition)
            self.assertIn("NoStory(", definition)
        self.assertNotIn("3000000", source)
        device = (board_dir / "m3_ai_device.h").read_text(encoding="utf-8")
        self.assertNotIn("#include <esp", device)
        self.assertNotIn("application.h", device)
        self.assertIn("return boilon::ai::DeviceStatusJson(Codec().OutputVolume(), "
                      "boilon::ai::BatteryLevel());", board_source)
        self.assertIn("runtime.AdoptVolume(volume, NowMs());", source)
        # M5 batch 3: AiReady() reads Wi-Fi state cached from events, never the
        # WifiManager lock; only the AI build links the ES7210 driver.
        ai_ready = board_source.split("bool AiReady() {", 1)[1].split("\n}\n", 1)[0]
        self.assertNotIn("IsConnected()", ai_ready)
        self.assertIn("wifi_connected", ai_ready)
        self.assertIn("wifi_connected = !disconnected;", board_source)
        # P5-1 batch 2: the display hands the conversation to the AI page. The
        # child's words and the answers reach the screen, never the log.
        display = board_source.split("class AiDisplay final", 1)[1].split("\n};", 1)[0]
        chat = display.split("void SetChatMessage(", 1)[1].split("\n    }\n", 1)[0]
        self.assertEqual(chat.count("ESP_LOG"), 1)
        self.assertIn('if (std::strcmp(role, "system") == 0)\n            ESP_LOGI(', chat)
        self.assertIn("CurrentChat().Heard(content);", chat)
        self.assertIn("CurrentChat().Said(content);", chat)
        self.assertNotIn("ESP_LOG", device)
        codec = (board_dir / "m2_local_codec.h").read_text(encoding="utf-8")
        self.assertEqual(codec.count("es7210_codec_new("), 1)
        guarded = codec.split("#if CONFIG_BOILON_S3_GEN2_AI\n", 1)[1].split("#else\n", 1)[0]
        self.assertIn("es7210_codec_cfg_t adc = {};", guarded)
        self.assertIn("mic_ = adc.ctrl_if ? es7210_codec_new(&adc) : nullptr;", guarded)
        self.assertIn("boilon::ai::PollNetwork(now)", source)
        app_main = source.index('extern "C" void app_main()')
        order = [source.index(marker, app_main) for marker in (
            "rtc_gpio_set_level(BOILON_S3_GEN2_POWER_ENABLE_GPIO, 1)",
            "ESP_LOG",
            "Startup stage: LCD_READY",
            "nvs_flash_init()",
            "boilon::ai::InspectStorage(nvs)",
            "Application::GetInstance()",
            "app.Initialize()",
            "xTaskCreatePinnedToCore",
            "app.Run()",
        )]
        self.assertEqual(order, sorted(order))


class AppOnlyPackagingTests(unittest.TestCase):
    def test_app_only_build_removes_stale_flash_shortcuts(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / "build/esp-idf/partition_table").mkdir(parents=True)
            stale_artifacts = (
                root / "build/merged-binary.bin",
                root / "build/merged-binary.hex",
                root / "build/download.config",
            )
            for stale in stale_artifacts:
                stale.write_bytes(b"unsafe")
            manifests = (
                root / "build/flash_args",
                root / "build/app-flash_args.in",
                root / "build/encrypted_app-flash_args",
                root / "build/esp-idf/partition_table/partition-table-flash_args.in",
            )
            for manifest in manifests:
                manifest.write_text("unsafe", encoding="utf-8")
            audit_files = (
                root / "build/xiaozhi.bin",
                root / "build/xiaozhi.elf",
                root / "build/partition-table.bin",
            )
            for audit_file in audit_files:
                audit_file.write_bytes(b"keep")

            with contextlib.chdir(root):
                build._prepare_artifact_output({"merge_bin": False})

            for stale in stale_artifacts:
                self.assertFalse(stale.exists())
            for manifest in manifests:
                self.assertFalse(manifest.exists())
            for audit_file in audit_files:
                self.assertTrue(audit_file.exists())

    def test_app_only_packaging_never_calls_merge_bin(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            with (
                contextlib.chdir(temp_dir),
                mock.patch.object(build, "merge_bin") as merge,
                contextlib.redirect_stdout(io.StringIO()),
            ):
                build._package_build_artifact(
                    {"merge_bin": False},
                    preview=False,
                )

        merge.assert_not_called()

    def test_app_only_guard_cleans_after_configure_or_build_failure(self):
        for failure_stage in ("configure", "build"):
            with self.subTest(failure_stage=failure_stage):
                with tempfile.TemporaryDirectory() as temp_dir:
                    root = Path(temp_dir)
                    (root / "build").mkdir()
                    with contextlib.chdir(root):
                        with self.assertRaisesRegex(RuntimeError, failure_stage):
                            with build._guard_app_only_outputs(False, "test"):
                                (root / "build/merged-binary.bin").write_bytes(b"unsafe")
                                (root / "build/flash_args").write_text(
                                    "unsafe",
                                    encoding="utf-8",
                                )
                                raise RuntimeError(failure_stage)

                    self.assertFalse((root / "build/merged-binary.bin").exists())
                    self.assertFalse((root / "build/flash_args").exists())

    def test_normal_packaging_keeps_manifests_and_calls_merge_bin(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / "build").mkdir()
            manifest = root / "build/flash_args"
            manifest.write_text("normal-board", encoding="utf-8")
            with (
                contextlib.chdir(root),
                mock.patch.object(build, "merge_bin") as merge,
            ):
                build._prepare_artifact_output({"merge_bin": True})
                build._package_build_artifact(
                    {"merge_bin": True},
                    preview=True,
                )

            self.assertTrue(manifest.exists())
            merge.assert_called_once_with(True)

    def test_app_only_packaging_rejects_regenerated_manifest(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / "build").mkdir()
            (root / "build/flash_project_args").write_text(
                "unsafe",
                encoding="utf-8",
            )
            with contextlib.chdir(root):
                with self.assertRaisesRegex(RuntimeError, "flash command manifests"):
                    build._package_build_artifact(
                        {"merge_bin": False},
                        preview=False,
                    )

    def test_app_size_limit_is_hard_failure(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / "build").mkdir()
            (root / "build/xiaozhi.bin").write_bytes(b"x" * 17)
            config = {
                "merge_bin": False,
                "app_size_limit": 16,
                "app_size_warning": 8,
            }

            with contextlib.chdir(root):
                with self.assertRaisesRegex(RuntimeError, "exceeding"):
                    build._validate_app_binary(config)

    def test_invalid_guard_fields_fail_closed(self):
        for config in (
            {"merge_bin": "false"},
            {"app_size_limit": True},
            {"app_size_limit": 8, "app_size_warning": 9},
        ):
            with self.subTest(config=config):
                with self.assertRaises(ValueError):
                    build._artifact_settings(config)


if __name__ == "__main__":
    unittest.main()
