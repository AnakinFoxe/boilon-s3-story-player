#pragma once
#include "m2_resume_store.h"
#include <nvs.h>

namespace boilon::local_library {
// Explicit opt-in only. No NVS initialization, auto-erase, format or migration.
// Do not construct/use this in the frozen diagnostic app or an audio task.
class NvsResumeBlob final : public ResumeBlob {
public:
    explicit NvsResumeBlob(const char* key = "state_v1") : key_(key) {}
    media::ResumeRead Read(std::vector<uint8_t>& bytes) const override {
        nvs_handle_t handle = 0;
        auto err = nvs_open("bl_resume", NVS_READONLY, &handle);
        if (err == ESP_ERR_NVS_NOT_FOUND) return media::ResumeRead::kMissing;
        if (err != ESP_OK) return media::ResumeRead::kError;
        size_t size = 0;
        err = nvs_get_blob(handle, key_, nullptr, &size);
        if (err == ESP_OK && size >= 12 && size <= BlobResumeStore::kMaxBytes) {
            bytes.resize(size);
            err = nvs_get_blob(handle, key_, bytes.data(), &size);
            if (size != bytes.size()) err = ESP_ERR_INVALID_SIZE;
        } else if (err == ESP_OK) err = ESP_ERR_INVALID_SIZE;
        nvs_close(handle);
        return err == ESP_OK ? media::ResumeRead::kFound : err == ESP_ERR_NVS_NOT_FOUND ?
            media::ResumeRead::kMissing : media::ResumeRead::kError;
    }
    bool Write(const std::vector<uint8_t>& bytes) override {
        if (bytes.size() < 12 || bytes.size() > BlobResumeStore::kMaxBytes) return false;
        nvs_handle_t handle = 0;
        if (nvs_open("bl_resume", NVS_READWRITE, &handle) != ESP_OK) return false;
        auto err = nvs_set_blob(handle, key_, bytes.data(), bytes.size());
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
        return err == ESP_OK;
    }

private:
    const char* key_;
};
}  // namespace boilon::local_library
