#pragma once
#include "media/playback_resume.h"
#include <algorithm>
#include <vector>

namespace boilon::local_library {
// One bounded blob, owned by the caller. Implementations must replace atomically
// on success; an error is not a promise of rollback. No implicit erase/retry.
class ResumeBlob {
public:
    virtual ~ResumeBlob() = default;
    virtual media::ResumeRead Read(std::vector<uint8_t>& bytes) const = 0;
    virtual bool Write(const std::vector<uint8_t>& bytes) = 0;
};

class BlobResumeStore final : public media::ResumeStore {
public:
    static constexpr size_t kMaxBytes = 8192, kMaxRecords = 64;
    explicit BlobResumeStore(ResumeBlob& blob) : blob_(blob) {}
    media::ResumeRead Read(const std::string& library, std::optional<media::MediaId> collection,
                          media::ResumeRecord& out) const override {
        std::vector<media::ResumeRecord> records;
        const auto read = Load(records);
        if (read != media::ResumeRead::kFound) return read;
        for (auto it = records.rbegin(); it != records.rend(); ++it)
            if (it->library_key == library && (!collection || it->media.collection_id == *collection)) {
                out = *it;
                return media::ResumeRead::kFound;
            }
        return media::ResumeRead::kMissing;
    }
    media::ResumeWrite Write(const media::ResumeRecord& record) override {
        if (!Valid(record)) return media::ResumeWrite::kError;
        std::vector<media::ResumeRecord> records;
        if (Load(records) == media::ResumeRead::kError) return media::ResumeWrite::kError;
        records.erase(std::remove_if(records.begin(), records.end(), [&](const auto& item) {
            return item.library_key == record.library_key && item.media.collection_id == record.media.collection_id;
        }), records.end());
        if (records.size() >= kMaxRecords) return media::ResumeWrite::kFull;
        records.push_back(record);
        std::vector<uint8_t> bytes{'B', 'L', 'R', '1'};
        Put(bytes, records.size(), 4);
        for (const auto& item : records) Encode(bytes, item);
        Put(bytes, Crc(bytes.data(), bytes.size()), 4);
        if (bytes.size() > kMaxBytes) return media::ResumeWrite::kFull;
        return blob_.Write(bytes) ? media::ResumeWrite::kOk : media::ResumeWrite::kError;
    }

private:
    static bool Valid(const media::ResumeRecord& r) {
        return r.version == 1 && !r.library_key.empty() && r.library_key.size() <= 256 &&
            r.library_key.find('\0') == std::string::npos &&
            r.media.collection_id.version == media::kMediaIdSchemeVersion &&
            r.media.track.id.version == media::kMediaIdSchemeVersion &&
            r.media.directory_name.size() <= 1020 && r.media.track.file_name.size() <= 1020 &&
            media::IsPlayableAudioName(r.media.track.file_name) && media::IsValidUtf8(r.media.directory_name) &&
            r.media.collection_is_root == r.media.directory_name.empty() && r.media.track.size_bytes > 0 &&
            r.duration_ms > 0 && r.position_ms <= r.duration_ms &&
            r.volume_percent >= 0 && r.volume_percent <= 100;
    }
    static uint32_t Crc(const uint8_t* bytes, size_t size) {
        uint32_t crc = ~uint32_t{0};
        for (size_t i = 0; i < size; ++i) {
            crc ^= bytes[i];
            for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1)));
        }
        return ~crc;
    }
    static void Put(std::vector<uint8_t>& b, uint64_t value, size_t width) {
        for (size_t i = 0; i < width; ++i) b.push_back(static_cast<uint8_t>(value >> (8 * i)));
    }
    static void Text(std::vector<uint8_t>& b, const std::string& text) {
        Put(b, text.size(), 4); b.insert(b.end(), text.begin(), text.end());
    }
    static void Encode(std::vector<uint8_t>& b, const media::ResumeRecord& r) {
        Put(b, r.version, 4); Text(b, r.library_key);
        Put(b, r.media.collection_id.version, 1); Put(b, r.media.collection_id.value, 8);
        Put(b, r.media.collection_is_root, 1); Text(b, r.media.directory_name);
        Put(b, r.media.track.id.version, 1); Put(b, r.media.track.id.value, 8);
        Text(b, r.media.track.file_name);
        for (auto value : {r.media.track.size_bytes, r.media.track.modified_time, r.position_ms, r.duration_ms}) Put(b, value, 8);
        Put(b, static_cast<unsigned>(r.volume_percent), 4);
    }
    struct Reader {
        const std::vector<uint8_t>& b;
        size_t pos = 4;
        bool ok = true;
        uint64_t Get(size_t width) {
            if (pos > b.size() - 4 || width > b.size() - 4 - pos) { ok = false; return 0; }
            uint64_t value = 0;
            for (size_t i = 0; i < width; ++i) value |= uint64_t{b[pos++]} << (8 * i);
            return value;
        }
        std::string Text(size_t limit) {
            const auto size = Get(4);
            if (!ok || size > limit || size > b.size() - 4 - pos) { ok = false; return {}; }
            std::string result(b.begin() + pos, b.begin() + pos + size); pos += size;
            return result;
        }
    };
    media::ResumeRead Load(std::vector<media::ResumeRecord>& records) const {
        std::vector<uint8_t> bytes;
        auto status = blob_.Read(bytes);
        if (status != media::ResumeRead::kFound) return status;
        if (bytes.size() < 12 || bytes.size() > kMaxBytes ||
            !std::equal(bytes.begin(), bytes.begin() + 4, "BLR1")) return media::ResumeRead::kError;
        uint32_t checksum = 0;
        for (size_t i = 0; i < 4; ++i) checksum |= uint32_t{bytes[bytes.size() - 4 + i]} << (8 * i);
        if (checksum != Crc(bytes.data(), bytes.size() - 4)) return media::ResumeRead::kError;
        Reader reader{bytes};
        const auto count = reader.Get(4);
        if (!count || count > kMaxRecords) return media::ResumeRead::kError;
        for (size_t i = 0; i < count; ++i) {
            media::ResumeRecord r;
            r.version = reader.Get(4); r.library_key = reader.Text(256);
            r.media.collection_id = {static_cast<uint8_t>(reader.Get(1)), reader.Get(8)};
            const auto root = reader.Get(1);
            r.media.collection_is_root = root == 1;
            r.media.directory_name = reader.Text(1020);
            r.media.track.id = {static_cast<uint8_t>(reader.Get(1)), reader.Get(8)};
            r.media.track.file_name = reader.Text(1020);
            r.media.track.size_bytes = reader.Get(8); r.media.track.modified_time = reader.Get(8);
            r.position_ms = reader.Get(8); r.duration_ms = reader.Get(8);
            const auto volume = reader.Get(4);
            r.volume_percent = volume <= 100 ? static_cast<int>(volume) : -1;
            if (!reader.ok || root > 1 || !Valid(r) || std::any_of(records.begin(), records.end(), [&](const auto& old) {
                return old.library_key == r.library_key && old.media.collection_id == r.media.collection_id;
            })) return media::ResumeRead::kError;
            records.push_back(std::move(r));
        }
        return reader.ok && reader.pos == bytes.size() - 4 ? media::ResumeRead::kFound : media::ResumeRead::kError;
    }
    ResumeBlob& blob_;
};
}  // namespace boilon::local_library
