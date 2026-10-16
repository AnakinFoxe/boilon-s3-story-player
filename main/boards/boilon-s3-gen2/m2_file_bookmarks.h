#pragma once
#include "m2_resume_store.h"

#ifndef BOILON_S3_GEN2_BOOKMARK_CAPACITY
#define BOILON_S3_GEN2_BOOKMARK_CAPACITY 32
#endif
static_assert(BOILON_S3_GEN2_BOOKMARK_CAPACITY >= 1 && BOILON_S3_GEN2_BOOKMARK_CAPACITY <= 64);

namespace boilon::local_library {
// Compact, oldest-first records. Names are resolved from the mounted catalog;
// size/mtime stay in the record so replaced files fail the existing stale check.
class FileBookmarks final : public media::ResumeStore {
public:
    FileBookmarks(ResumeBlob& blob, BlobResumeStore& legacy, media::MediaCatalog catalog,
                  size_t capacity = BOILON_S3_GEN2_BOOKMARK_CAPACITY)
        : blob_(blob), legacy_(legacy), catalog_(std::move(catalog)), capacity_(capacity) {}
    bool WriteVolume(int volume) {
        write_volume_ = volume;
        return volume >= 0 && volume <= 200;
    }
    std::optional<int> LastReadVolume() const { return read_volume_; }
    std::vector<std::pair<media::MediaId, uint64_t>> Positions(const std::string& library) const {
        std::vector<std::pair<media::MediaId, uint64_t>> result;
        for (const auto& entry : snapshot_) {
            const auto track = catalog_.Resolve({media::kMediaIdSchemeVersion, entry.track});
            if (entry.library == Hash(library) && track && track->track.size_bytes == entry.size &&
                track->track.modified_time == entry.modified)
                result.emplace_back(track->track.id, entry.position);
        }
        return result;
    }
    media::ResumeRead Read(const std::string& library, std::optional<media::MediaId> collection,
                           media::ResumeRecord& out) const override {
        return Find(library, collection, {}, out);
    }
    media::ResumeRead ReadTrack(const std::string& library, media::MediaId track,
                                media::ResumeRecord& out) const {
        return Find(library, {}, track, out);
    }
    media::ResumeWrite Write(const media::ResumeRecord& record) override {
        if (capacity_ < 1 || capacity_ > 64 || record.library_key.empty() ||
            record.library_key.size() > 256 || record.version != 1 ||
            !catalog_.Resolve(record.media.track.id) || !record.duration_ms ||
            record.position_ms > record.duration_ms || record.volume_percent < 0 ||
            record.volume_percent > 200 ||
            (write_volume_ && (*write_volume_ < 0 || *write_volume_ > 200)))
            return media::ResumeWrite::kError;
        std::vector<Entry> entries;
        if (Current(record.library_key, entries) == media::ResumeRead::kError)
            return media::ResumeWrite::kError;
        const auto library = Hash(record.library_key);
        entries.erase(std::remove_if(entries.begin(), entries.end(),
                                     [&](const auto& item) {
                                         return item.library == library &&
                                                item.track == record.media.track.id.value;
                                     }),
                      entries.end());
        auto entry = From(record);
        if (write_volume_)
            entry.volume = *write_volume_;
        entries.push_back(entry);
        if (entries.size() > capacity_)
            entries.erase(entries.begin(), entries.end() - capacity_);
        return Store(entries);
    }
    // 2026-10-16: forgets this library's bookmarks in one collection, or with
    // none every bookmark on the device. Never writes an empty table: older
    // releases read one as damaged and then could not save again. With nothing
    // left both keys go, the old format first, or its records would come back.
    media::ResumeWrite Forget(const std::string& library,
                              std::optional<media::MediaId> collection) {
        if (collection) {
            std::vector<Entry> entries;
            if (Current(library, entries) == media::ResumeRead::kError)
                return media::ResumeWrite::kError;
            const auto count = entries.size();
            const auto key = Hash(library);
            entries.erase(std::remove_if(entries.begin(), entries.end(),
                                         [&](const auto& item) {
                                             return item.library == key &&
                                                    item.collection == collection->value;
                                         }),
                          entries.end());
            if (entries.size() == count)
                return media::ResumeWrite::kOk;  // Nothing of it was saved.
            if (!entries.empty())
                return Store(entries);
        }
        if (!legacy_.Erase() || !blob_.Erase())
            return media::ResumeWrite::kError;
        snapshot_.clear();
        return media::ResumeWrite::kOk;
    }

private:
    struct Entry {
        uint64_t library, track, collection, size, modified, position, duration;
        uint32_t volume;
    };
    // The table a change starts from: the saved one or, before its first
    // write, this library's records in the old format.
    media::ResumeRead Current(const std::string& library, std::vector<Entry>& entries) const {
        const auto status = Load(entries);
        if (status != media::ResumeRead::kMissing)
            return status;
        for (const auto& collection : catalog_.Collections()) {
            media::ResumeRecord old;
            const auto found = legacy_.Read(library, collection.id, old);
            if (found == media::ResumeRead::kError)
                return media::ResumeRead::kError;
            if (found == media::ResumeRead::kFound)
                entries.push_back(From(old));
        }
        return media::ResumeRead::kMissing;
    }
    media::ResumeWrite Store(const std::vector<Entry>& entries) {
        std::vector<uint8_t> bytes{'B', 'F', 'R', '2'};
        Put(bytes, entries.size(), 4);
        for (const auto& e : entries) {
            for (auto value :
                 {e.library, e.track, e.collection, e.size, e.modified, e.position, e.duration})
                Put(bytes, value, 8);
            Put(bytes, e.volume, 4);
        }
        Put(bytes, Crc(bytes.data(), bytes.size()), 4);
        if (!blob_.Write(bytes))
            return media::ResumeWrite::kError;
        snapshot_ = entries;
        return media::ResumeWrite::kOk;
    }
    static Entry From(const media::ResumeRecord& record) {
        return {Hash(record.library_key),
                record.media.track.id.value,
                record.media.collection_id.value,
                record.media.track.size_bytes,
                record.media.track.modified_time,
                record.position_ms == record.duration_ms ? 0 : record.position_ms,
                record.duration_ms,
                uint32_t(record.volume_percent)};
    }
    static uint64_t Hash(const std::string& value) {
        uint64_t hash = 14695981039346656037ULL;
        for (unsigned char byte : value)
            hash = (hash ^ byte) * 1099511628211ULL;
        return hash;
    }
    static void Put(std::vector<uint8_t>& bytes, uint64_t value, size_t width) {
        for (size_t i = 0; i < width; ++i)
            bytes.push_back(static_cast<uint8_t>(value >> (8 * i)));
    }
    static uint32_t Crc(const uint8_t* bytes, size_t size) {
        uint32_t crc = ~uint32_t{0};
        for (size_t i = 0; i < size; ++i) {
            crc ^= bytes[i];
            for (int bit = 0; bit < 8; ++bit)
                crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1)));
        }
        return ~crc;
    }
    media::ResumeRead Load(std::vector<Entry>& entries) const {
        std::vector<uint8_t> bytes;
        const auto status = blob_.Read(bytes);
        if (status != media::ResumeRead::kFound)
            return status;
        if (bytes.size() < 12 || bytes.size() > 12 + 64 * 60 ||
            !std::equal(bytes.begin(), bytes.begin() + 4, "BFR2"))
            return media::ResumeRead::kError;
        size_t offset = 4;
        auto get = [&](size_t width) {
            uint64_t value = 0;
            for (size_t i = 0; i < width; ++i)
                value |= uint64_t{bytes[offset++]} << (8 * i);
            return value;
        };
        const auto count = get(4);
        if (!count || count > 64 || bytes.size() != 12 + count * 60)
            return media::ResumeRead::kError;
        for (size_t i = 0; i < count; ++i) {
            Entry e{get(8), get(8), get(8), get(8), get(8), get(8), get(8), uint32_t(get(4))};
            if (!e.size || !e.duration || e.position > e.duration || e.volume > 200 ||
                std::any_of(entries.begin(), entries.end(), [&](const auto& old) {
                    return old.library == e.library && old.track == e.track;
                }))
                return media::ResumeRead::kError;
            entries.push_back(e);
        }
        if (get(4) != Crc(bytes.data(), bytes.size() - 4))
            return media::ResumeRead::kError;
        snapshot_ = entries;
        return media::ResumeRead::kFound;
    }
    media::ResumeRead Find(const std::string& library, std::optional<media::MediaId> collection,
                           std::optional<media::MediaId> track, media::ResumeRecord& out) const {
        read_volume_.reset();
        std::vector<Entry> entries;
        const auto loaded = Load(entries);
        if (loaded == media::ResumeRead::kError)
            return media::ResumeRead::kError;
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
            if (it->library != Hash(library) ||
                (collection && it->collection != collection->value) ||
                (track && it->track != track->value))
                continue;
            auto resolved = catalog_.Resolve({media::kMediaIdSchemeVersion, it->track});
            if (!resolved)
                return media::ResumeRead::kMissing;
            resolved->track.size_bytes = it->size;
            resolved->track.modified_time = it->modified;
            read_volume_ = int(it->volume);
            out = {
                1, library, *resolved, it->position, it->duration, std::min(int(it->volume), 100)};
            return media::ResumeRead::kFound;
        }
        if (loaded == media::ResumeRead::kFound)
            return media::ResumeRead::kMissing;
        if (track) {
            const auto resolved = catalog_.Resolve(*track);
            if (!resolved)
                return media::ResumeRead::kMissing;
            collection = resolved->collection_id;
        }
        const auto status = legacy_.Read(library, collection, out);
        if (status == media::ResumeRead::kFound && track && out.media.track.id != *track)
            return media::ResumeRead::kMissing;
        if (status == media::ResumeRead::kFound)
            read_volume_ = out.volume_percent;
        return status;
    }
    ResumeBlob& blob_;
    BlobResumeStore& legacy_;
    media::MediaCatalog catalog_;
    size_t capacity_;
    mutable std::vector<Entry> snapshot_;
    std::optional<int> write_volume_;
    mutable std::optional<int> read_volume_;
};
}  // namespace boilon::local_library
