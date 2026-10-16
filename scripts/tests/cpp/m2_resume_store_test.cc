#include "boards/boilon-s3-gen2/m2_resume_store.h"
#include "boards/boilon-s3-gen2/m2_resume_nvs.h"
#include "boards/boilon-s3-gen2/m2_resume_session.h"
#include <cassert>
#include <cstring>
#include <iostream>

using namespace boilon::local_library;
using namespace media;

struct MemoryBlob : ResumeBlob {
    std::vector<uint8_t> bytes;
    bool read_error = false, write_error = false;
    int writes = 0;
    ResumeRead Read(std::vector<uint8_t>& out) const override {
        if (read_error) return ResumeRead::kError;
        if (bytes.empty()) return ResumeRead::kMissing;
        out = bytes; return ResumeRead::kFound;
    }
    bool Write(const std::vector<uint8_t>& in) override {
        ++writes;
        if (write_error) return false;
        bytes = in; return true;
    }
};

MediaCatalog Catalog() {
    MediaCatalogBuilder builder("card-A");
    assert(builder.Add({"", "root.mp3", 100, 1}) == AddResult::kAdded);
    assert(builder.Add({"故事", "第二集.mp3", 200, 2}) == AddResult::kAdded);
    auto built = builder.Finish(); assert(built.Ok()); return std::move(built.catalog);
}
ResumeRecord Record(size_t index = 0) {
    const auto catalog = Catalog();
    const auto& collection = catalog.Collections().at(index);
    return {1, "card-A", *catalog.Resolve(collection.tracks.front().id), 42000, 180000, 35};
}
void Rechecksum(std::vector<uint8_t>& bytes) {
    uint32_t crc = 0xffffffff;
    for (size_t i = 0; i < bytes.size() - 4; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320U : 0);
    }
    crc ^= 0xffffffff;
    for (int i = 0; i < 4; ++i) bytes[bytes.size() - 4 + i] = static_cast<uint8_t>(crc >> (8 * i));
}

void StoreCases() {
    MemoryBlob blob;
    BlobResumeStore store(blob);
    ResumeRecord out;
    assert(store.Read("card-A", {}, out) == ResumeRead::kMissing);
    auto first = Record(), second = Record(1);
    assert(store.Write(first) == ResumeWrite::kOk);
    assert(blob.bytes[0] == 'B' && blob.bytes[3] == '1');
    assert(blob.bytes[4] == 1 && blob.bytes[5] == 0 && blob.bytes[6] == 0 && blob.bytes[7] == 0);
    assert(store.Write(second) == ResumeWrite::kOk);
    auto other = first; other.library_key = "card-B"; other.position_ms = 12000;
    assert(store.Write(other) == ResumeWrite::kOk);
    BlobResumeStore reboot(blob);  // No typed records survive reconstruction.
    assert(reboot.Read("card-A", {}, out) == ResumeRead::kFound && out.media.track.id == second.media.track.id);
    assert(reboot.Read("card-A", first.media.collection_id, out) == ResumeRead::kFound && out.position_ms == 42000);
    assert(reboot.Read("card-B", {}, out) == ResumeRead::kFound && out.position_ms == 12000);
    assert(reboot.Read("missing", {}, out) == ResumeRead::kMissing);
    first.position_ms = 61000;
    assert(reboot.Write(first) == ResumeWrite::kOk);
    assert(reboot.Read("card-A", {}, out) == ResumeRead::kFound && out.position_ms == 61000);
    const auto good = blob.bytes;
    blob.write_error = true; first.position_ms = 62000;
    assert(reboot.Write(first) == ResumeWrite::kError && blob.bytes == good);
    blob.write_error = false;
    for (int mode = 0; mode < 5; ++mode) {
        blob.bytes = good;
        if (mode == 0) blob.bytes[20] ^= 1;  // CRC corruption.
        if (mode == 1) { blob.bytes[3] = '2'; Rechecksum(blob.bytes); }
        if (mode == 2) { blob.bytes[8] = 2; Rechecksum(blob.bytes); }  // Record version.
        if (mode == 3) { blob.bytes[4] = 65; Rechecksum(blob.bytes); }
        if (mode == 4) blob.bytes.resize(8193);
        const auto bad = blob.bytes; const int before = blob.writes;
        assert(reboot.Read("card-A", {}, out) == ResumeRead::kError);
        assert(reboot.Write(first) == ResumeWrite::kError);
        assert(blob.writes == before && blob.bytes == bad);
    }
    blob.bytes = good; blob.read_error = true;
    const int before = blob.writes;
    assert(reboot.Write(first) == ResumeWrite::kError && blob.writes == before);
    MemoryBlob limited; BlobResumeStore capacity(limited);
    auto short_record = Record();
    for (uint64_t i = 0; i < 64; ++i) {
        short_record.media.collection_id.value = i;
        assert(capacity.Write(short_record) == ResumeWrite::kOk);
    }
    short_record.media.collection_id.value = 64;
    const auto full = limited.bytes;
    assert(capacity.Write(short_record) == ResumeWrite::kFull && limited.bytes == full && limited.writes == 64);
    short_record.media.collection_id.value = 0;
    assert(capacity.Write(short_record) == ResumeWrite::kOk);  // Replacement at capacity.
    MemoryBlob byte_limited; BlobResumeStore byte_capacity(byte_limited);
    auto large = Record(1); large.media.directory_name = std::string(1000, 'd');
    large.media.track.file_name = std::string(996, 'f') + ".mp3";
    bool hit_limit = false;
    for (uint64_t i = 0; i < 10; ++i) {
        large.media.collection_id.value = i;
        const auto old = byte_limited.bytes; const int count = byte_limited.writes;
        const auto result = byte_capacity.Write(large);
        if (result == ResumeWrite::kFull) {
            assert(byte_limited.bytes == old && byte_limited.writes == count);
            hit_limit = true; break;
        }
        assert(result == ResumeWrite::kOk && byte_limited.bytes.size() <= 8192);
    }
    assert(hit_limit);
}

struct FakePlayer {
    bool busy = false, start_ok = true;
    int starts = 0;
    uint64_t position = 42000, requested = 0;
    std::string path;
    bool Running() const { return busy; }
    uint64_t PositionMs() const { return position; }
    bool StartAt(const std::string& p, uint64_t offset) {
        ++starts; path = p; requested = offset; return start_ok;
    }
};
void SessionCases() {
    MemoryBlob blob; BlobResumeStore store(blob); FakePlayer player;
    auto record = Record(1);
    PlaybackResume before(Catalog(), "card-A", store);
    assert(SaveProgress(before, record.media, player, 180000, 35, 100, CheckpointReason::kPause) == SaveStatus::kSaved);
    assert(before.LastSuccessfulSaveMs() == 100);
    blob.write_error = true; player.position = 50000;
    assert(SaveProgress(before, record.media, player, 180000, 40, 200, CheckpointReason::kPause) == SaveStatus::kStorageError);
    assert(before.LastSuccessfulSaveMs() == 100);
    blob.write_error = false;
    BlobResumeStore reboot_store(blob); PlaybackResume boot(Catalog(), "card-A", reboot_store);
    const auto plan = boot.Restore({}, ResumeMode::kBoot);
    assert(plan.status == ResumeStatus::kReady && plan.position_ms == 37000 && plan.volume_percent == 35);
    assert(StartResume(plan, "/sdcard", player));
    assert(player.starts == 1 && player.requested == 37000 && player.path == "/sdcard/故事/第二集.mp3");
    auto exact = boot.Restore({}, ResumeMode::kSavedExact);
    assert(exact.position_ms == 42000 && !exact.auto_play);
    for (int mode = 0; mode < 8; ++mode) {
        auto bad = plan; std::string root = "/sdcard"; player.busy = false;
        if (mode == 0) bad.status = ResumeStatus::kMissing;
        if (mode == 1) bad.target.reset();
        if (mode == 2) bad.auto_play = false;
        if (mode == 3) player.busy = true;
        if (mode == 4) root = "relative";
        if (mode == 5) bad.target->track.file_name = "../escape.mp3";
        if (mode == 6) bad.target->directory_name = "..";
        if (mode == 7) bad.target->track.file_name = std::string(513, 'f') + ".mp3";
        const int count = player.starts;
        assert(!StartResume(bad, root, player) && player.starts == count);
    }
    player.busy = false; player.start_ok = false;
    assert(!StartResume(plan, "/sdcard", player) && player.starts == 2);
    const int writes = blob.writes;
    assert(SaveProgress(before, record.media, player, 0, 35, 300, CheckpointReason::kPause) == SaveStatus::kInvalidInput);
    assert(blob.writes == writes);
}

static int nvs_mode = 0, opens = 0, get_calls = 0, sets = 0, commits = 0, closes = 0, erases = 0,
           opened_as = 0;
static std::vector<uint8_t> nvs_bytes(12, 7);
int nvs_open(const char* ns, int access, nvs_handle_t* handle) {
    assert(std::string(ns) == "bl_resume" && (access == NVS_READONLY || access == NVS_READWRITE));
    ++opens;
    *handle = 42;
    opened_as = access;
    return nvs_mode == 1 ? 99 : nvs_mode == 2 ? ESP_ERR_NVS_NOT_FOUND : ESP_OK;
}
int nvs_get_blob(nvs_handle_t handle, const char* key, void* data, size_t* size) {
    assert(handle == 42 && std::string(key) == "state_v1"); ++get_calls;
    if (nvs_mode == 3 || (nvs_mode == 4 && data)) return 99;
    if (nvs_mode == 5) return ESP_ERR_NVS_NOT_FOUND;
    if (!data) { *size = nvs_mode == 6 ? 8193 : nvs_bytes.size(); return ESP_OK; }
    assert(*size == nvs_bytes.size()); std::memcpy(data, nvs_bytes.data(), *size);
    if (nvs_mode == 7) --*size;
    return ESP_OK;
}
int nvs_set_blob(nvs_handle_t handle, const char* key, const void*, size_t size) {
    assert(handle == 42 && std::string(key) == "state_v1" && size == 12); ++sets;
    return nvs_mode == 8 ? 99 : ESP_OK;
}
int nvs_commit(nvs_handle_t handle) { assert(handle == 42); ++commits; return nvs_mode == 9 ? 99 : ESP_OK; }
int nvs_erase_key(nvs_handle_t handle, const char* key) {
    assert(handle == 42 && std::string(key) == "state_v1");
    ++erases;
    return nvs_mode == 10 ? ESP_ERR_NVS_NOT_FOUND : nvs_mode == 11 ? 99 : ESP_OK;
}
void nvs_close(nvs_handle_t handle) { assert(handle == 42); ++closes; }
void ResetNvs(int mode) {
    nvs_mode = mode;
    opens = get_calls = sets = commits = closes = erases = opened_as = 0;
}
void NvsCases() {
    NvsResumeBlob blob;
    for (int mode = 0; mode <= 7; ++mode) {
        ResetNvs(mode); std::vector<uint8_t> out;
        const auto result = blob.Read(out);
        assert(result == (mode == 0 ? ResumeRead::kFound : mode == 2 || mode == 5 ? ResumeRead::kMissing : ResumeRead::kError));
        assert(opens == 1 && sets == 0 && commits == 0 && erases == 0);
        assert(closes == (mode == 1 || mode == 2 ? 0 : 1));
        assert(get_calls == (mode == 1 || mode == 2 ? 0 : mode == 3 || mode == 5 || mode == 6 ? 1 : 2));
        if (mode == 0) assert(out == nvs_bytes);
    }
    for (int mode : {0, 1, 8, 9}) {
        ResetNvs(mode); const bool result = blob.Write(nvs_bytes);
        assert(result == (mode == 0) && opens == 1 && get_calls == 0);
        assert(sets == (mode == 1 ? 0 : 1) && erases == 0);
        assert(commits == (mode == 1 || mode == 8 ? 0 : 1));
        assert(closes == (mode == 1 ? 0 : 1));
    }
    // 2026-10-16: forgetting progress deletes only this key; a key that is not
    // there counts as deleted, any other failure is reported.
    for (int mode : {0, 1, 9, 10, 11}) {
        ResetNvs(mode);
        const bool result = blob.Erase();
        assert(result == (mode == 0 || mode == 10) && opens == 1 && opened_as == NVS_READWRITE);
        assert(get_calls == 0 && sets == 0 && erases == (mode == 1 ? 0 : 1));
        assert(commits == (mode == 1 || mode == 11 ? 0 : 1));
        assert(closes == (mode == 1 ? 0 : 1));
    }
    for (size_t size : {size_t{0}, size_t{11}, size_t{8193}}) {
        ResetNvs(0); assert(!blob.Write(std::vector<uint8_t>(size)));
        assert(opens == 0 && sets == 0 && commits == 0 && closes == 0);
    }
}
int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string mode = argv[1];
    if (mode == "store") StoreCases();
    else if (mode == "session") SessionCases();
    else if (mode == "nvs") NvsCases();
    else return 2;
    std::cout << mode << " PASS\n";
}
