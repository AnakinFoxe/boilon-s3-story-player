#pragma once
#include <dirent.h>
#include <esp_app_desc.h>
#include <esp_app_format.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include "m6_release.h"
#include "m7_sd_update.h"

// M7: the ESP side of the SD-card update, AI variant only. Run lists the
// mounted card's root before the scan; every refusal is logged and the boot
// goes on with the running firmware. The card is only read.
namespace boilon::sd_update {
inline constexpr char kLogTag[] = "BoilonUpdate";
// Every AI image carries this tag whole; an update writes only an image that
// has it and its own release name, so a renamed offline or upstream image is
// never selected. Logged at boot, which keeps the bytes in the image.
__attribute__((used)) inline constexpr char kVariantTag[] = "boilon-variant:ai";

inline SlotState StateOf(const esp_partition_t* partition) {
    esp_ota_img_states_t state;
    if (partition == nullptr || esp_ota_get_state_partition(partition, &state) != ESP_OK)
        return SlotState::kUndefined;
    switch (state) {
        case ESP_OTA_IMG_NEW: return SlotState::kNew;
        case ESP_OTA_IMG_PENDING_VERIFY: return SlotState::kPendingVerify;
        case ESP_OTA_IMG_VALID: return SlotState::kValid;
        case ESP_OTA_IMG_INVALID: return SlotState::kInvalid;
        case ESP_OTA_IMG_ABORTED: return SlotState::kAborted;
        default: return SlotState::kUndefined;
    }
}
// After an SD update a rollback-capable bootloader shows here as PENDING_VERIFY.
inline void LogBoot() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    ESP_LOGI(kLogTag, "Running %s state=%s %s", running ? running->label : "?",
             Name(StateOf(running)), kVariantTag);
}
// The first boot after an SD update: a rollback-capable bootloader waits for the confirmation.
inline bool JustUpdated() {
    return StateOf(esp_ota_get_running_partition()) == SlotState::kPendingVerify;
}
// Once startup completes (a story plays or a no-story page is up).
inline void ConfirmIfPending() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (!ShouldConfirm(StateOf(running)))
        return;
    const esp_err_t result = esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(kLogTag, "Startup complete; confirmed %s: %s", running->label, esp_err_to_name(result));
}

struct Port {
    // The update page; percent < 0 says it failed. Also keeps the owner watchdog fed.
    std::function<void(const std::string& version, int percent)> show;
    std::function<int()> battery;  // percent, -1 unknown
};

// Writes a newer AI release from the card's root into the other slot, selects
// it and restarts. Returns when nothing was selected.
inline void Run(const char* root, const Port& port) {
    const auto running_version = FromReleaseName(release::kName);
    if (!running_version)
        return;
    std::vector<std::string> names;
    if (DIR* dir = opendir(root)) {
        while (const dirent* entry = readdir(dir))
            if (entry->d_type == DT_REG && FromFileName(entry->d_name))
                names.emplace_back(entry->d_name);
        closedir(dir);
    }
    const auto candidate = PickNewer(names, *running_version);
    if (!candidate) {
        if (!names.empty())
            ESP_LOGI(kLogTag, "%u release file(s) on the card, none newer than %s",
                     static_cast<unsigned>(names.size()), release::kName);
        return;
    }
    const std::string path = std::string(root) + "/" + candidate->name;
    const int file = open(path.c_str(), O_RDONLY);
    struct stat info = {};
    std::vector<uint8_t> buffer(4096);
    ssize_t count = file < 0 || fstat(file, &info) != 0 ? -1 : read(file, buffer.data(), buffer.size());
    constexpr size_t kDescOffset = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    esp_app_desc_t desc = {};
    if (count >= static_cast<ssize_t>(kDescOffset + sizeof(desc)))
        std::memcpy(&desc, buffer.data() + kDescOffset, sizeof(desc));
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
    if (desc.magic_word != ESP_APP_DESC_MAGIC_WORD || target == nullptr ||
        static_cast<size_t>(info.st_size) > target->size) {
        ESP_LOGW(kLogTag, "Not installing %s: unreadable, not an app or too large",
                 candidate->name.c_str());
        if (file >= 0)
            close(file);
        return;
    }
    Situation situation;
    situation.running = StateOf(running);
    situation.battery = port.battery();
    situation.target = StateOf(target);
    esp_app_desc_t current = {};
    situation.target_holds_this_image =
        esp_ota_get_partition_description(target, &current) == ESP_OK &&
        std::memcmp(current.app_elf_sha256, desc.app_elf_sha256, sizeof(desc.app_elf_sha256)) == 0;
    if (const auto skip = Decide(situation); skip != Skip::kNone) {
        ESP_LOGW(kLogTag, "Not installing %s: %s (battery=%d %s=%s)", candidate->name.c_str(),
                 Name(skip), situation.battery, target->label, Name(situation.target));
        close(file);
        return;
    }
    const size_t total = info.st_size;
    ESP_LOGI(kLogTag, "Installing %s (%u bytes) into %s over %s; running %s from %s",
             candidate->name.c_str(), static_cast<unsigned>(total), target->label,
             Name(situation.target), release::kName, running ? running->label : "?");
    const std::string version = "v" + Numbers(candidate->version);
    port.show(version, 0);
    Marker name(ReleaseName(candidate->version)), variant(kVariantTag);
    esp_ota_handle_t handle = 0;
    esp_err_t result = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    const bool begun = result == ESP_OK;
    size_t done = 0;
    int shown = 0;
    while (result == ESP_OK && count > 0) {
        name.Feed(buffer.data(), count);
        variant.Feed(buffer.data(), count);
        result = esp_ota_write(handle, buffer.data(), count);
        done += count;
        if (const int percent = Progress(done, total); percent >= shown + 5) {
            shown = percent;
            port.show(version, percent);
        }
        count = read(file, buffer.data(), buffer.size());
    }
    close(file);
    if (result == ESP_OK && (count < 0 || done != total))
        result = ESP_ERR_INVALID_SIZE;  // A read error or a file that changed size.
    if (result == ESP_OK && !(name.Found() && variant.Found()))
        result = ESP_ERR_NOT_FOUND;  // Not our AI release, whatever its name says.
    if (result != ESP_OK && begun)
        esp_ota_abort(handle);
    if (result == ESP_OK)
        result = esp_ota_end(handle);  // Validates the image; frees the handle.
    if (result == ESP_OK)
        result = esp_ota_set_boot_partition(target);
    if (result != ESP_OK) {
        ESP_LOGE(kLogTag, "Update to %s failed after %u bytes: %s (name=%d variant=%d)",
                 version.c_str(), static_cast<unsigned>(done), esp_err_to_name(result),
                 name.Found(), variant.Found());
        port.show(version, -1);
        return;
    }
    ESP_LOGI(kLogTag, "Installed %s into %s; restarting", version.c_str(), target->label);
    port.show(version, 100);
    esp_restart();
}
}  // namespace boilon::sd_update
