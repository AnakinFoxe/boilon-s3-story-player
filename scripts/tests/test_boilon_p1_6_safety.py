"""Run the actual OTA policy paths with host-only HTTP and flash fakes.

NVS: both Boilon variants leave upstream main.cc out and never erase NVS; the
build guard (test_boilon_gen2_build_guard) checks their entries.
"""

import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from scripts.tests.boilon_cpp_helpers import function


ROOT = Path(__file__).resolve().parents[2]


class BoilonP16SafetyTests(unittest.TestCase):
    def compile_and_run(self, source, enabled, *, cjson=False):
        compiler = shutil.which("c++")
        self.assertTrue(compiler, "A native C++ compiler is required")
        with tempfile.TemporaryDirectory(prefix="boilon-p1-6-safety-") as directory:
            executable = Path(directory) / "safety-test"
            command = [compiler, "-std=c++23", "-Wall", "-Wextra", "-Werror",
                       f"-DCONFIG_FIRMWARE_UPGRADE_ENABLED={int(enabled)}"]
            if cjson:
                candidates = [ROOT / "managed_components/espressif__cjson/cJSON",
                              ROOT.parent / "boilon-s3-story-player/managed_components/espressif__cjson/cJSON"]
                if os.environ.get("BOILON_TEST_CJSON_DIR"):
                    candidates.insert(0, Path(os.environ["BOILON_TEST_CJSON_DIR"]))
                library = next((path for path in candidates if (path / "cJSON.c").is_file()), None)
                self.assertIsNotNone(library, "Provide the existing cJSON source via BOILON_TEST_CJSON_DIR")
                obj = Path(directory) / "cJSON.o"
                result = subprocess.run([shutil.which("cc"), "-c", str(library / "cJSON.c"),
                                         "-o", str(obj)], capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                command += ["-I", str(ROOT / "main"), "-I", str(library), str(obj)]
            command += ["-x", "c++", "-", "-o", str(executable)]
            result = subprocess.run(command, input=source, capture_output=True, text=True,
                                    timeout=30, cwd=ROOT)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True,
                                    timeout=10, cwd=ROOT)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_actual_ota_response_upgrade_and_mark_valid_policies(self):
        ota = (ROOT / "main/ota.cc").read_text()
        header = (ROOT / "main/ota.h").read_text()
        declaration = header[header.index("class Ota {"):header.index("\n#endif")]
        actual = "\n".join(function(ota, signature) for signature in (
            "Ota::Ota()", "Ota::~Ota()", "std::string Ota::GetCheckVersionUrl()",
            "std::unique_ptr<Http> Ota::SetupHttp()", "NetworkResult<> Ota::CheckVersion()",
            "void Ota::MarkCurrentVersionValid()", "bool Ota::Upgrade(",
            "bool Ota::StartUpgrade(", "std::expected<std::vector<int>, std::string> Ota::ParseVersion(",
            "bool Ota::IsNewVersionAvailable(",
        ))
        source = r'''
#include <algorithm>
#include <cassert>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>
#include <sys/time.h>
#include "cJSON.h"
#include "cjson_utils.h"
using esp_err_t = int;
struct NetworkError {
    enum class Code { InvalidArgument, Protocol, Http };
    Code code;
    int status = 0;
    static NetworkError InvalidArgument() { return {Code::InvalidArgument}; }
    static NetworkError ProtocolError() { return {Code::Protocol}; }
    static NetworkError HttpFailed(int status) { return {Code::Http, status}; }
    std::string ToString() const { return "fake-network-error"; }
};
template <typename T = void> using NetworkResult = std::expected<T, NetworkError>;
enum { ESP_OK = 0, ESP_ERR_INVALID_ARG = 1, ESP_ERR_INVALID_RESPONSE = 2,
       ESP_ERR_OTA_VALIDATE_FAILED = 3, ESP_OTA_IMG_PENDING_VERIFY = 4,
       OTA_WITH_SEQUENTIAL_WRITES = 5, MALLOC_CAP_INTERNAL = 6 };
#define CONFIG_OTA_URL "https://fake.invalid/check"
template<class... T> void log_fake(T&&...) {}
#define ESP_LOGI(...) log_fake(__VA_ARGS__)
#define ESP_LOGW(...) log_fake(__VA_ARGS__)
#define ESP_LOGE(...) log_fake(__VA_ARGS__)
#define TAG "fake"
static int http_created, http_open, callbacks, mark_writes, partition_queries;
static std::vector<std::string> flash;
static std::string response;
static std::map<std::string, std::string> strings;
static std::map<std::string, int> numbers;
struct Settings {
    std::string ns;
    Settings(const char* name, bool): ns(name) {}
    std::string GetString(const char* key) { return strings[ns + "/" + key]; }
    int GetInt(const char* key) { return numbers[ns + "/" + key]; }
    void SetString(const char* key, const char* value) { strings[ns + "/" + key] = value; }
    void SetInt(const char* key, int value) { numbers[ns + "/" + key] = value; }
};
struct Http {
    int reads = 0;
    void SetHeader(const char*, const std::string&) {}
    void SetContent(std::string) {}
    NetworkResult<> Open(const std::string&, const std::string&) { ++http_open; return {}; }
    NetworkResult<int> GetStatusCode() { return 200; }
    std::string ReadAll() { return response; }
    size_t GetBodyLength() { return 256; }
    NetworkResult<int> Read(char* buffer, size_t size) {
        if (reads++) return 0;
        assert(size >= 256); memset(buffer, 0, 256); return 256;
    }
    void Close() {}
};
struct Network { std::unique_ptr<Http> CreateHttp(int) { ++http_created; return std::make_unique<Http>(); } };
struct Board {
    static Board& GetInstance() { static Board board; return board; }
    Network* GetNetwork() { static Network network; return &network; }
    std::string GetSystemInfoJson() { return "{}"; }
    std::string GetUuid() { return "fake-client"; }
};
namespace SystemInfo {
std::string GetUserAgent() { return "fake-agent"; }
std::string GetMacAddress() { return "fake-device"; }
}
namespace Lang { const char* CODE = "zh-CN"; }
struct esp_app_desc_t { char version[32]; };
const esp_app_desc_t* esp_app_get_description() { static esp_app_desc_t desc{"1.0.0"}; return &desc; }
struct esp_partition_t { const char* label; unsigned long address; };
static esp_partition_t partition{"ota_0", 0x20000};
using esp_ota_handle_t = int;
using esp_ota_img_states_t = int;
struct esp_image_header_t { char data[16]; };
struct esp_image_segment_header_t { char data[16]; };
const esp_partition_t* esp_ota_get_running_partition() { ++partition_queries; return &partition; }
const esp_partition_t* esp_ota_get_next_update_partition(const void*) { ++partition_queries; return &partition; }
int esp_ota_get_state_partition(const esp_partition_t*, int* state) { *state = ESP_OTA_IMG_PENDING_VERIFY; return 0; }
int esp_ota_mark_app_valid_cancel_rollback() { ++mark_writes; return 0; }
int esp_ota_begin(const esp_partition_t*, int, int* handle) { flash.push_back("begin"); *handle = 1; return 0; }
int esp_ota_abort(int) { flash.push_back("abort"); return 0; }
int esp_ota_write(int, const void*, size_t size) { assert(size == 256); flash.push_back("write"); return 0; }
int esp_ota_end(int) { flash.push_back("end"); return 0; }
int esp_ota_set_boot_partition(const esp_partition_t*) { flash.push_back("boot"); return 0; }
void* heap_caps_malloc(size_t size, int) { return malloc(size); }
void heap_caps_free(void* ptr) { free(ptr); }
long long esp_timer_get_time() { return 0; }
const char* esp_err_to_name(int) { return "fake"; }
int fake_settimeofday(const timeval*, const void*) { return 0; }
#define settimeofday fake_settimeofday
''' + declaration + actual + r'''
int main() {
    Ota ota;
    for (const char* version : {"2.0.0", "1.0.0", "0.1.0"}) {
        for (int force : {0, 1}) {
            response = std::string("{\"firmware\":{\"version\":\"") + version +
                "\",\"url\":\"https://fake.invalid/app.bin\",\"force\":" + std::to_string(force) +
                "},\"activation\":{\"code\":\"test-code\",\"challenge\":\"test-challenge\"},"
                "\"mqtt\":{\"endpoint\":\"fake-mqtt\",\"keepalive\":90},"
                "\"websocket\":{\"url\":\"wss://fake.invalid/\",\"version\":3}}";
            const int before = http_created;
            assert(ota.CheckVersion().has_value());
            assert(http_created == before + 1);
            assert(ota.HasActivationCode() && ota.HasActivationChallenge());
            assert(ota.HasMqttConfig() && ota.HasWebsocketConfig());
            assert(strings["mqtt/endpoint"] == "fake-mqtt" && numbers["mqtt/keepalive"] == 90);
            assert(strings["websocket/url"] == "wss://fake.invalid/" && numbers["websocket/version"] == 3);
#if CONFIG_FIRMWARE_UPGRADE_ENABLED
            assert(ota.HasNewVersion() == (force == 1 || std::string(version) == "2.0.0"));
            assert(ota.GetFirmwareVersion() == version);
            assert(ota.GetFirmwareUrl() == "https://fake.invalid/app.bin");
#else
            assert(!ota.HasNewVersion());
            assert(ota.GetFirmwareVersion().empty() && ota.GetFirmwareUrl().empty());
#endif
        }
    }
#if !CONFIG_FIRMWARE_UPGRADE_ENABLED
    for (const char* firmware : {
            "{\"version\":\"not-a-version\",\"url\":\"https://fake.invalid/app\",\"force\":1}",
            "{\"version\":\"999999999999999999999999999999\",\"url\":\"https://fake.invalid/app\",\"force\":1}",
            "{\"version\":3,\"url\":null,\"force\":1}", "{}", "null"}) {
        response = std::string("{\"firmware\":") + firmware + "}";
        assert(ota.CheckVersion().has_value() && !ota.HasNewVersion());
        assert(ota.GetFirmwareVersion().empty() && ota.GetFirmwareUrl().empty());
    }
    response = "not-json";
    const auto invalid = ota.CheckVersion();
    assert(!invalid && invalid.error().code == NetworkError::Code::Protocol);
    assert(!ota.HasNewVersion() && ota.GetFirmwareUrl().empty());
#endif
    assert(flash.empty() && mark_writes == 0);
    for (int start = 0; start < 2; ++start) {
        http_created = http_open = callbacks = partition_queries = 0;
        flash.clear();
        auto callback = [](int, size_t) { ++callbacks; };
        const bool result = start ? ota.StartUpgrade(callback) :
            Ota::Upgrade("https://fake.invalid/direct.bin", callback);
#if CONFIG_FIRMWARE_UPGRADE_ENABLED
        assert(result && http_created == 1 && http_open == 1 && callbacks > 0);
        assert((flash == std::vector<std::string>{"begin", "write", "end", "boot"}));
#else
        assert(!result && http_created == 0 && http_open == 0 && callbacks == 0);
        assert(flash.empty() && partition_queries == 0);
#endif
    }
    partition_queries = mark_writes = 0;
    ota.MarkCurrentVersionValid();
#if CONFIG_FIRMWARE_UPGRADE_ENABLED
    assert(partition_queries == 1 && mark_writes == 1);
    partition.label = "factory";
    ota.MarkCurrentVersionValid();
    assert(partition_queries == 2 && mark_writes == 1);
#else
    assert(partition_queries == 0 && mark_writes == 0);
#endif
}
'''
        for enabled in (False, True):
            with self.subTest(enabled=enabled):
                self.compile_and_run(source, enabled, cjson=True)

    def test_actual_identity_headers_and_activation_results(self):
        # M3 batch 2: the AI variant runs these unchanged Ota paths against the
        # official server. Fakes record each request; firmware offers are covered above.
        ota = (ROOT / "main/ota.cc").read_text()
        header = (ROOT / "main/ota.h").read_text()
        declaration = header[header.index("class Ota {"):header.index("\n#endif")]
        actual = "\n".join(function(ota, signature) for signature in (
            "Ota::Ota()", "Ota::~Ota()", "std::string Ota::GetCheckVersionUrl()",
            "std::unique_ptr<Http> Ota::SetupHttp()", "NetworkResult<> Ota::CheckVersion()",
            "std::string Ota::GetActivationPayload()", "esp_err_t Ota::Activate()",
            "std::expected<std::vector<int>, std::string> Ota::ParseVersion(", "bool Ota::IsNewVersionAvailable(",
        ))
        source = r'''
#include <algorithm>
#include <cassert>
#include <charconv>
#include <cstring>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>
#include <sys/time.h>
#include "cJSON.h"
using esp_err_t = int;
struct NetworkError {
    enum class Code { InvalidArgument, Protocol, Http };
    Code code;
    int status = 0;
    static NetworkError InvalidArgument() { return {Code::InvalidArgument}; }
    static NetworkError ProtocolError() { return {Code::Protocol}; }
    static NetworkError HttpFailed(int status) { return {Code::Http, status}; }
    std::string ToString() const { return "fake-network-error"; }
};
template <typename T = void> using NetworkResult = std::expected<T, NetworkError>;
enum { ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_TIMEOUT = 0x107 };
#define CONFIG_OTA_URL "https://official.invalid/xiaozhi/ota/"
template<class... T> void log_fake(T&&...) {}
#define ESP_LOGI(...) log_fake(__VA_ARGS__)
#define ESP_LOGW(...) log_fake(__VA_ARGS__)
#define ESP_LOGE(...) log_fake(__VA_ARGS__)
#define TAG "fake"
#if FAKE_SERIAL
#define ESP_EFUSE_BLOCK_USR_DATA 1
static const int ESP_EFUSE_USER_DATA = 0;
int esp_efuse_read_field_blob(int, void* data, size_t bits) {
    assert(bits == 32 * 8);
    memcpy(data, "SN-FAKE-0001", 13);
    return ESP_OK;
}
#endif
static std::map<std::string, std::string> strings, headers;
static std::map<std::string, int> numbers;
static std::vector<std::string> writes;
static std::vector<int> statuses;
static std::string response, method, url, content;
static int requests;
struct Settings {
    std::string ns;
    Settings(const char* name, bool) : ns(name) {}
    std::string GetString(const char* key) { return strings[ns + "/" + key]; }
    int GetInt(const char* key) { return numbers[ns + "/" + key]; }
    void SetString(const char* key, const char* value) {
        writes.push_back(ns + "/" + key);
        strings[ns + "/" + key] = value;
    }
    void SetInt(const char* key, int value) {
        writes.push_back(ns + "/" + key);
        numbers[ns + "/" + key] = value;
    }
};
struct Http {
    void SetHeader(const char* key, const std::string& value) { headers[key] = value; }
    void SetContent(std::string body) { content = std::move(body); }
    NetworkResult<> Open(const std::string& m, const std::string& u) {
        ++requests; method = m; url = u; return {};
    }
    NetworkResult<int> GetStatusCode() {
        assert(!statuses.empty());
        const int status = statuses.front();
        statuses.erase(statuses.begin());
        return status;
    }
    std::string ReadAll() { return response; }
    void Close() {}
};
struct Network {
    std::unique_ptr<Http> CreateHttp(int) { headers.clear(); return std::make_unique<Http>(); }
};
struct Board {
    static Board& GetInstance() { static Board board; return board; }
    Network* GetNetwork() { static Network network; return &network; }
    std::string GetSystemInfoJson() { return "{\"uuid\":\"persisted-uuid\"}"; }
    std::string GetUuid() { return "persisted-uuid"; }
};
namespace SystemInfo {
std::string GetUserAgent() { return "boilon-s3-gen2-ai/2.5.0"; }
std::string GetMacAddress() { return "aa:bb:cc:dd:ee:ff"; }
}
namespace Lang { const char* CODE = "zh-CN"; }
struct esp_app_desc_t { char version[32]; };
const esp_app_desc_t* esp_app_get_description() { static esp_app_desc_t desc{"2.5.0"}; return &desc; }
int fake_settimeofday(const timeval*, const void*) { return 0; }
#define settimeofday fake_settimeofday
''' + declaration + actual + r'''
int main() {
    Ota ota;
    response = "{\"activation\":{\"message\":\"xiaozhi.me 123456\",\"code\":\"123456\","
               "\"challenge\":\"challenge-1\",\"timeout_ms\":30000},"
               "\"mqtt\":{\"endpoint\":\"mqtt.invalid\",\"client_id\":\"cid\",\"keepalive\":240},"
               "\"websocket\":{\"url\":\"wss://official.invalid/\",\"token\":\"t\",\"version\":3},"
               "\"firmware\":{\"version\":\"9.9.9\",\"url\":\"https://x.invalid/app.bin\",\"force\":1}}";
    statuses = {200};
    assert(ota.CheckVersion().has_value());
    // One POST of the board's system info to the built-in official server.
    assert(method == "POST" && url == CONFIG_OTA_URL && content == "{\"uuid\":\"persisted-uuid\"}");
    assert(headers["Device-Id"] == "aa:bb:cc:dd:ee:ff" && headers["Client-Id"] == "persisted-uuid");
    assert(headers["User-Agent"] == "boilon-s3-gen2-ai/2.5.0" && headers["Accept-Language"] == "zh-CN");
#if FAKE_SERIAL
    assert(headers["Activation-Version"] == "2" && headers["Serial-Number"].rfind("SN-FAKE-0001", 0) == 0);
#else
    assert(headers["Activation-Version"] == "1" && !headers.count("Serial-Number"));
#endif
    assert(ota.HasActivationCode() && ota.GetActivationCode() == "123456");
    assert(ota.GetActivationMessage() == "xiaozhi.me 123456" && ota.HasActivationChallenge());
    assert(!ota.HasNewVersion() && ota.GetFirmwareUrl().empty());
    // Persistent writes stay in the two transport namespaces.
    assert(!writes.empty());
    for (const auto& key : writes)
        assert(key.rfind("mqtt/", 0) == 0 || key.rfind("websocket/", 0) == 0);
    assert(strings["mqtt/endpoint"] == "mqtt.invalid" && numbers["mqtt/keepalive"] == 240);
    assert(strings["websocket/url"] == "wss://official.invalid/" && numbers["websocket/version"] == 3);
    // Activation: 202 means keep waiting, 200 success, anything else failure.
    statuses = {202};
    assert(ota.Activate() == ESP_ERR_TIMEOUT);
    assert(method == "POST" && url == std::string(CONFIG_OTA_URL) + "activate");
    assert(headers["Client-Id"] == "persisted-uuid");
#if FAKE_SERIAL
    assert(content.find("\"serial_number\":\"SN-FAKE-0001\"") != std::string::npos);
    assert(content.find("\"challenge\":\"challenge-1\"") != std::string::npos);
#else
    assert(content == "{}");
#endif
    statuses = {200};
    assert(ota.Activate() == ESP_OK);
    statuses = {500};
    assert(ota.Activate() == ESP_FAIL);
    // Without a challenge there is no activation request at all.
    response = "{}";
    statuses = {200};
    assert(ota.CheckVersion().has_value() && !ota.HasActivationChallenge());
    const int before = requests;
    assert(ota.Activate() == ESP_FAIL && requests == before);
    // A saved wifi/ota_url silently redirects the identity requests; this is
    // why the AI board refuses to start Wi-Fi when one is present.
    strings["wifi/ota_url"] = "https://other.invalid/ota/";
    statuses = {200};
    assert(ota.CheckVersion().has_value() && url == "https://other.invalid/ota/");
    assert(statuses.empty());
}
'''
        for serial in (False, True):
            with self.subTest(serial=serial):
                self.compile_and_run(f"#define FAKE_SERIAL {int(serial)}\n" + source, False,
                                     cjson=True)

    def test_actual_hidden_kconfig_policy_overrides_saved_values(self):
        config = (ROOT / "main/Kconfig.projbuild").read_text()
        match = re.search(r"(?m)^config FIRMWARE_UPGRADE_ENABLED\n(?:(?:[ \t].*|)\n)*", config)
        self.assertIsNotNone(match)
        sdk_python = "idf-tools-v6.0.2/python_env/idf6.0_py3.12_env/bin/python"
        interpreters = [ROOT / ".tools" / sdk_python,
                        ROOT.parent / "boilon-s3-story-player/.tools" / sdk_python]
        if os.environ.get("BOILON_TEST_IDF_PYTHON"):
            interpreters.insert(0, Path(os.environ["BOILON_TEST_IDF_PYTHON"]))
        interpreter = next((path for path in interpreters if path.is_file()), None)
        self.assertIsNotNone(interpreter, "Provide the existing ESP-IDF Python via BOILON_TEST_IDF_PYTHON")
        with tempfile.TemporaryDirectory(prefix="boilon-p1-6-kconfig-") as directory:
            kconfig = Path(directory) / "Kconfig"
            kconfig.write_text('config BOARD_TYPE_BOILON_S3_GEN2\n    bool "Test board"\n\n' +
                               match.group())
            result = subprocess.run([str(interpreter), "-E", "-s", "-B", "-c", r'''
import pathlib
import sys
import kconfiglib
path = pathlib.Path(sys.argv[1])
saved = path.parent / "sdkconfig"
for board, expected in (("y", "n"), ("n", "y")):
    for requested in ("y", "n"):
        saved.write_text(f"CONFIG_BOARD_TYPE_BOILON_S3_GEN2={board}\n"
                         f"CONFIG_FIRMWARE_UPGRADE_ENABLED={requested}\n")
        config = kconfiglib.Kconfig(str(path), warn=False)
        config.load_config(str(saved))
        symbol = config.syms["FIRMWARE_UPGRADE_ENABLED"]
        assert all(node.prompt is None for node in symbol.nodes)
        assert symbol.str_value == expected, (board, requested, symbol.str_value)
''', str(kconfig)], capture_output=True, text=True, timeout=10, cwd=directory)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
