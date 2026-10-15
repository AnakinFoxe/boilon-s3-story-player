#include "media/media_catalog.h"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace media {
namespace {

constexpr uint64_t kFnv1aOffsetBasis = UINT64_C(14695981039346656037);
constexpr uint64_t kFnv1aPrime = UINT64_C(1099511628211);
constexpr std::string_view kMediaIdDomain = "boilon.media-id";
constexpr size_t kNotFound = std::numeric_limits<size_t>::max();

enum class MediaIdKind : uint8_t {
    kCollection = 1,
    kTrack = 2,
};

class Fnv1a64 {
public:
    void AddByte(uint8_t byte) {
        value_ ^= byte;
        value_ *= kFnv1aPrime;
    }

    void AddUint64(uint64_t number) {
        // The serialization is explicitly little-endian so IDs do not depend
        // on the host or target architecture.
        for (size_t index = 0; index < sizeof(number); ++index) {
            AddByte(static_cast<uint8_t>(number & UINT64_C(0xff)));
            number >>= 8;
        }
    }

    void AddField(std::string_view field) {
        AddUint64(static_cast<uint64_t>(field.size()));
        for (char character : field) {
            AddByte(static_cast<uint8_t>(static_cast<unsigned char>(character)));
        }
    }

    uint64_t Value() const { return value_; }

private:
    uint64_t value_ = kFnv1aOffsetBasis;
};

bool IsContinuationByte(uint8_t byte) { return (byte & 0xc0U) == 0x80U; }

bool DecodeNextUtf8(std::string_view text, size_t& offset, uint32_t& code_point) {
    if (offset >= text.size()) {
        return false;
    }

    const auto byte0 = static_cast<uint8_t>(static_cast<unsigned char>(text[offset]));
    if (byte0 <= 0x7fU) {
        code_point = byte0;
        ++offset;
        return true;
    }

    if (byte0 >= 0xc2U && byte0 <= 0xdfU) {
        if (text.size() - offset < 2) {
            return false;
        }
        const auto byte1 = static_cast<uint8_t>(static_cast<unsigned char>(text[offset + 1]));
        if (!IsContinuationByte(byte1)) {
            return false;
        }
        code_point = (static_cast<uint32_t>(byte0 & 0x1fU) << 6) | (byte1 & 0x3fU);
        offset += 2;
        return true;
    }

    if (byte0 >= 0xe0U && byte0 <= 0xefU) {
        if (text.size() - offset < 3) {
            return false;
        }
        const auto byte1 = static_cast<uint8_t>(static_cast<unsigned char>(text[offset + 1]));
        const auto byte2 = static_cast<uint8_t>(static_cast<unsigned char>(text[offset + 2]));
        if (!IsContinuationByte(byte1) || !IsContinuationByte(byte2)) {
            return false;
        }
        if ((byte0 == 0xe0U && byte1 < 0xa0U) || (byte0 == 0xedU && byte1 > 0x9fU)) {
            return false;
        }
        code_point = (static_cast<uint32_t>(byte0 & 0x0fU) << 12) |
                     (static_cast<uint32_t>(byte1 & 0x3fU) << 6) | (byte2 & 0x3fU);
        offset += 3;
        return true;
    }

    if (byte0 >= 0xf0U && byte0 <= 0xf4U) {
        if (text.size() - offset < 4) {
            return false;
        }
        const auto byte1 = static_cast<uint8_t>(static_cast<unsigned char>(text[offset + 1]));
        const auto byte2 = static_cast<uint8_t>(static_cast<unsigned char>(text[offset + 2]));
        const auto byte3 = static_cast<uint8_t>(static_cast<unsigned char>(text[offset + 3]));
        if (!IsContinuationByte(byte1) || !IsContinuationByte(byte2) ||
            !IsContinuationByte(byte3)) {
            return false;
        }
        if ((byte0 == 0xf0U && byte1 < 0x90U) || (byte0 == 0xf4U && byte1 > 0x8fU)) {
            return false;
        }
        code_point = (static_cast<uint32_t>(byte0 & 0x07U) << 18) |
                     (static_cast<uint32_t>(byte1 & 0x3fU) << 12) |
                     (static_cast<uint32_t>(byte2 & 0x3fU) << 6) | (byte3 & 0x3fU);
        offset += 4;
        return true;
    }

    return false;
}

bool IsAsciiDigit(char character) { return character >= '0' && character <= '9'; }

uint32_t FoldAsciiCase(uint32_t code_point) {
    if (code_point >= 'A' && code_point <= 'Z') {
        return code_point + ('a' - 'A');
    }
    return code_point;
}

int CompareUnsignedBytes(std::string_view lhs, std::string_view rhs) {
    const size_t common_size = std::min(lhs.size(), rhs.size());
    for (size_t index = 0; index < common_size; ++index) {
        const auto lhs_byte = static_cast<uint8_t>(static_cast<unsigned char>(lhs[index]));
        const auto rhs_byte = static_cast<uint8_t>(static_cast<unsigned char>(rhs[index]));
        if (lhs_byte < rhs_byte) {
            return -1;
        }
        if (lhs_byte > rhs_byte) {
            return 1;
        }
    }
    if (lhs.size() < rhs.size()) {
        return -1;
    }
    if (lhs.size() > rhs.size()) {
        return 1;
    }
    return 0;
}

struct DigitRun {
    size_t end = 0;
    size_t significant_begin = 0;
    size_t significant_length = 0;
    size_t leading_zero_count = 0;
};

DigitRun ReadDigitRun(std::string_view text, size_t begin) {
    DigitRun run;
    run.end = begin;
    while (run.end < text.size() && IsAsciiDigit(text[run.end])) {
        ++run.end;
    }

    run.significant_begin = begin;
    while (run.significant_begin < run.end && text[run.significant_begin] == '0') {
        ++run.significant_begin;
    }
    run.significant_length = run.end - run.significant_begin;
    run.leading_zero_count = run.significant_begin - begin;
    return run;
}

int CompareDigitRuns(std::string_view lhs, const DigitRun& lhs_run, std::string_view rhs,
                     const DigitRun& rhs_run) {
    if (lhs_run.significant_length < rhs_run.significant_length) {
        return -1;
    }
    if (lhs_run.significant_length > rhs_run.significant_length) {
        return 1;
    }

    for (size_t index = 0; index < lhs_run.significant_length; ++index) {
        const char lhs_digit = lhs[lhs_run.significant_begin + index];
        const char rhs_digit = rhs[rhs_run.significant_begin + index];
        if (lhs_digit < rhs_digit) {
            return -1;
        }
        if (lhs_digit > rhs_digit) {
            return 1;
        }
    }

    if (lhs_run.leading_zero_count < rhs_run.leading_zero_count) {
        return -1;
    }
    if (lhs_run.leading_zero_count > rhs_run.leading_zero_count) {
        return 1;
    }
    return 0;
}

int CompareNaturalPrimary(std::string_view lhs, std::string_view rhs) {
    size_t lhs_offset = 0;
    size_t rhs_offset = 0;
    while (lhs_offset < lhs.size() && rhs_offset < rhs.size()) {
        if (IsAsciiDigit(lhs[lhs_offset]) && IsAsciiDigit(rhs[rhs_offset])) {
            const DigitRun lhs_run = ReadDigitRun(lhs, lhs_offset);
            const DigitRun rhs_run = ReadDigitRun(rhs, rhs_offset);
            const int result = CompareDigitRuns(lhs, lhs_run, rhs, rhs_run);
            if (result != 0) {
                return result;
            }
            lhs_offset = lhs_run.end;
            rhs_offset = rhs_run.end;
            continue;
        }

        uint32_t lhs_code_point = 0;
        uint32_t rhs_code_point = 0;
        if (!DecodeNextUtf8(lhs, lhs_offset, lhs_code_point) ||
            !DecodeNextUtf8(rhs, rhs_offset, rhs_code_point)) {
            return CompareUnsignedBytes(lhs, rhs);
        }
        lhs_code_point = FoldAsciiCase(lhs_code_point);
        rhs_code_point = FoldAsciiCase(rhs_code_point);
        if (lhs_code_point < rhs_code_point) {
            return -1;
        }
        if (lhs_code_point > rhs_code_point) {
            return 1;
        }
    }

    if (lhs_offset < lhs.size()) {
        return 1;
    }
    if (rhs_offset < rhs.size()) {
        return -1;
    }
    return 0;
}

bool AsciiEqualsIgnoreCase(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (size_t index = 0; index < lhs.size(); ++index) {
        const auto lhs_byte = static_cast<uint8_t>(static_cast<unsigned char>(lhs[index]));
        const auto rhs_byte = static_cast<uint8_t>(static_cast<unsigned char>(rhs[index]));
        if (FoldAsciiCase(lhs_byte) != FoldAsciiCase(rhs_byte)) {
            return false;
        }
    }
    return true;
}

bool IsStructurallySafeSegment(std::string_view segment, bool allow_empty, size_t max_bytes) {
    if (segment.empty()) {
        return allow_empty;
    }
    if (segment.size() > max_bytes || segment == "." || segment == "..") {
        return false;
    }
    for (char character : segment) {
        const auto byte = static_cast<uint8_t>(static_cast<unsigned char>(character));
        if (byte == 0 || byte == '/' || byte == '\\' || byte < 0x20U || byte == 0x7fU) {
            return false;
        }
    }
    return true;
}

bool IsIgnoredCollectionName(std::string_view name) {
    return AsciiEqualsIgnoreCase(name, "__MACOSX") ||
           AsciiEqualsIgnoreCase(name, "System Volume Information") ||
           AsciiEqualsIgnoreCase(name, "$RECYCLE.BIN") || AsciiEqualsIgnoreCase(name, "RECYCLED");
}

bool IsHiddenName(std::string_view name) { return !name.empty() && name.front() == '.'; }

// MP3, and WAV since F08; both extensions have four bytes.
bool HasAudioExtension(std::string_view name) {
    if (name.size() <= 4) {
        return false;
    }
    const auto extension = name.substr(name.size() - 4);
    return AsciiEqualsIgnoreCase(extension, ".mp3") || AsciiEqualsIgnoreCase(extension, ".wav");
}

MediaId MakeStableId(std::string_view library_key, MediaIdKind kind,
                     std::string_view directory_name, std::string_view file_name) {
    Fnv1a64 hash;
    hash.AddField(kMediaIdDomain);
    hash.AddByte(kMediaIdSchemeVersion);
    hash.AddByte(static_cast<uint8_t>(kind));
    hash.AddField(library_key);
    hash.AddField(directory_name);
    hash.AddField(file_name);
    return MediaId{kMediaIdSchemeVersion, hash.Value()};
}

}  // namespace

std::string MediaId::ToString() const {
    static constexpr std::array<char, 16> hex = {'0', '1', '2', '3', '4', '5', '6', '7',
                                                 '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    if (!IsValid()) {
        return {};
    }

    std::string result = "m" + std::to_string(static_cast<unsigned int>(version)) + "-";
    result.reserve(result.size() + 16);
    for (int shift = 60; shift >= 0; shift -= 4) {
        result.push_back(hex[static_cast<size_t>((value >> shift) & UINT64_C(0x0f))]);
    }
    return result;
}

bool operator==(const MediaId& lhs, const MediaId& rhs) {
    return lhs.version == rhs.version && lhs.value == rhs.value;
}

bool operator!=(const MediaId& lhs, const MediaId& rhs) { return !(lhs == rhs); }

MediaCatalog::MediaCatalog(std::vector<Collection>&& collections)
    : collections_(std::move(collections)) {}

std::optional<ResolvedTrack> MediaCatalog::Resolve(MediaId id) const {
    if (!id.IsValid()) {
        return std::nullopt;
    }
    for (const Collection& collection : collections_) {
        for (const Track& track : collection.tracks) {
            if (track.id == id) {
                return ResolvedTrack{collection.id, collection.is_root, collection.directory_name,
                                     track};
            }
        }
    }
    return std::nullopt;
}

MediaCatalogBuilder::MediaCatalogBuilder(std::string library_key, CatalogLimits limits)
    : library_key_(std::move(library_key)), limits_(limits) {
    if (library_key_.empty() || limits_.max_collections == 0 ||
        limits_.max_tracks_per_collection == 0 || limits_.max_tracks_total == 0 ||
        limits_.max_name_bytes_total == 0 || limits_.max_segment_bytes == 0) {
        Fail(CatalogBuildError::kInvalidConfiguration);
    }
}

AddResult MediaCatalogBuilder::Add(RawFile file) {
    if (finished_) {
        return AddResult::kBuilderFinished;
    }
    ++stats_.input_files;
    if (error_ != CatalogBuildError::kNone) {
        return AddResult::kBuilderFailed;
    }
    if (!file.is_regular) {
        ++stats_.skipped_not_regular;
        return AddResult::kSkippedNotRegular;
    }
    if (file.size_bytes == 0) {
        ++stats_.skipped_empty_files;
        return AddResult::kSkippedEmptyFile;
    }
    if (!IsStructurallySafeSegment(file.collection_name, true, limits_.max_segment_bytes) ||
        !IsStructurallySafeSegment(file.file_name, false, limits_.max_segment_bytes)) {
        ++stats_.skipped_invalid_names;
        return AddResult::kSkippedInvalidName;
    }
    if (!IsValidUtf8(file.collection_name) || !IsValidUtf8(file.file_name)) {
        ++stats_.skipped_invalid_utf8;
        return AddResult::kSkippedInvalidUtf8;
    }
    if (file.is_hidden || IsHiddenName(file.collection_name) || IsHiddenName(file.file_name) ||
        IsIgnoredCollectionName(file.collection_name)) {
        ++stats_.skipped_hidden;
        return AddResult::kSkippedHidden;
    }
    if (!HasAudioExtension(file.file_name)) {
        ++stats_.skipped_unsupported_extension;
        return AddResult::kSkippedUnsupportedExtension;
    }

    size_t collection_index = FindCollection(file.collection_name);
    const bool creates_collection = collection_index == kNotFound;
    if (!creates_collection) {
        const Collection& collection = collections_[collection_index];
        const auto duplicate =
            std::find_if(collection.tracks.begin(), collection.tracks.end(),
                         [&](const Track& track) { return track.file_name == file.file_name; });
        if (duplicate != collection.tracks.end()) {
            if (duplicate->size_bytes == file.size_bytes &&
                duplicate->modified_time == file.modified_time) {
                ++stats_.duplicate_files;
                return AddResult::kDuplicate;
            }
            Fail(CatalogBuildError::kConflictingDuplicate);
            return AddResult::kConflictingDuplicate;
        }
    }

    const size_t collection_track_count =
        creates_collection ? 0 : collections_[collection_index].tracks.size();
    const size_t collection_name_bytes = creates_collection ? file.collection_name.size() : 0;
    const bool name_size_overflow =
        file.file_name.size() > std::numeric_limits<size_t>::max() - collection_name_bytes;
    const size_t additional_name_bytes =
        name_size_overflow ? 0 : file.file_name.size() + collection_name_bytes;
    const bool name_budget_exceeded =
        name_size_overflow || total_name_bytes_ > limits_.max_name_bytes_total ||
        additional_name_bytes > limits_.max_name_bytes_total - total_name_bytes_;
    if ((creates_collection && collections_.size() >= limits_.max_collections) ||
        collection_track_count >= limits_.max_tracks_per_collection ||
        total_tracks_ >= limits_.max_tracks_total || name_budget_exceeded) {
        Fail(CatalogBuildError::kLimitExceeded);
        return AddResult::kLimitExceeded;
    }

    const MediaId collection_id = creates_collection ? MakeCollectionId(file.collection_name)
                                                     : collections_[collection_index].id;
    const MediaId track_id = MakeTrackId(file.collection_name, file.file_name);
    if ((creates_collection && used_id_values_.count(collection_id.value) != 0) ||
        used_id_values_.count(track_id.value) != 0 ||
        (creates_collection && collection_id.value == track_id.value)) {
        Fail(CatalogBuildError::kIdCollision);
        return AddResult::kIdCollision;
    }

    if (creates_collection) {
        collections_.push_back(Collection{
            collection_id, file.collection_name.empty(), std::move(file.collection_name), {}});
        used_id_values_.insert(collection_id.value);
        collection_index = collections_.size() - 1;
    }
    collections_[collection_index].tracks.push_back(
        Track{track_id, std::move(file.file_name), file.size_bytes, file.modified_time});
    used_id_values_.insert(track_id.value);
    ++total_tracks_;
    total_name_bytes_ += additional_name_bytes;
    ++stats_.added_tracks;
    return AddResult::kAdded;
}

CatalogBuildResult MediaCatalogBuilder::Finish() {
    CatalogBuildResult result;
    result.stats = stats_;
    if (finished_) {
        result.error = CatalogBuildError::kAlreadyFinished;
        return result;
    }
    finished_ = true;
    if (error_ != CatalogBuildError::kNone) {
        result.error = error_;
        return result;
    }

    for (Collection& collection : collections_) {
        std::sort(collection.tracks.begin(), collection.tracks.end(),
                  [](const Track& lhs, const Track& rhs) {
                      return NaturalNameLess(lhs.file_name, rhs.file_name);
                  });
    }
    std::sort(collections_.begin(), collections_.end(),
              [](const Collection& lhs, const Collection& rhs) {
                  if (lhs.is_root != rhs.is_root) {
                      return lhs.is_root;
                  }
                  return NaturalNameLess(lhs.directory_name, rhs.directory_name);
              });

    result.catalog = MediaCatalog(std::move(collections_));
    return result;
}

size_t MediaCatalogBuilder::FindCollection(std::string_view directory_name) const {
    for (size_t index = 0; index < collections_.size(); ++index) {
        if (collections_[index].directory_name == directory_name) {
            return index;
        }
    }
    return kNotFound;
}

MediaId MediaCatalogBuilder::MakeCollectionId(std::string_view directory_name) const {
    return MakeStableId(library_key_, MediaIdKind::kCollection, directory_name, {});
}

MediaId MediaCatalogBuilder::MakeTrackId(std::string_view directory_name,
                                         std::string_view file_name) const {
    return MakeStableId(library_key_, MediaIdKind::kTrack, directory_name, file_name);
}

void MediaCatalogBuilder::Fail(CatalogBuildError error) {
    if (error_ == CatalogBuildError::kNone) {
        error_ = error;
    }
}

bool IsValidUtf8(std::string_view text) {
    size_t offset = 0;
    while (offset < text.size()) {
        uint32_t code_point = 0;
        if (!DecodeNextUtf8(text, offset, code_point)) {
            return false;
        }
        if (code_point > 0x10ffffU || (code_point >= 0xd800U && code_point <= 0xdfffU)) {
            return false;
        }
    }
    return true;
}

bool IsPlayableAudioName(std::string_view file_name) {
    return IsStructurallySafeSegment(file_name, false, std::numeric_limits<size_t>::max()) &&
           IsValidUtf8(file_name) && !IsHiddenName(file_name) && HasAudioExtension(file_name);
}

bool NaturalNameLess(std::string_view lhs, std::string_view rhs) {
    const bool lhs_is_valid = IsValidUtf8(lhs);
    const bool rhs_is_valid = IsValidUtf8(rhs);
    if (lhs_is_valid != rhs_is_valid) {
        return lhs_is_valid;
    }
    if (!lhs_is_valid) {
        return CompareUnsignedBytes(lhs, rhs) < 0;
    }
    const int primary_result = CompareNaturalPrimary(lhs, rhs);
    if (primary_result != 0) {
        return primary_result < 0;
    }
    return CompareUnsignedBytes(lhs, rhs) < 0;
}

}  // namespace media
