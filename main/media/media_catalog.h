#ifndef MEDIA_MEDIA_CATALOG_H_
#define MEDIA_MEDIA_CATALOG_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace media {

constexpr uint8_t kMediaIdSchemeVersion = 1;

struct MediaId {
    // v1 is an explicitly serialized, domain-separated FNV-1a 64-bit ID.
    // It is stable identity, not an authorization token.
    uint8_t version = 0;
    uint64_t value = 0;

    bool IsValid() const { return version != 0; }
    std::string ToString() const;
};

bool operator==(const MediaId& lhs, const MediaId& rhs);
bool operator!=(const MediaId& lhs, const MediaId& rhs);

struct RawFile {
    // Empty means the root collection. Both names are individual path segments,
    // never absolute or relative paths.
    std::string collection_name;
    std::string file_name;
    uint64_t size_bytes = 0;
    uint64_t modified_time = 0;
    bool is_regular = true;
    bool is_hidden = false;
};

struct CatalogLimits {
    size_t max_collections = 64;
    size_t max_tracks_per_collection = 512;
    size_t max_tracks_total = 2048;
    // Counts collection names once and every accepted file name. This is a
    // logical string-data budget, not an exact allocator-byte measurement.
    size_t max_name_bytes_total = 256 * 1024;
    size_t max_segment_bytes = 1020;
};

struct Track {
    MediaId id;
    std::string file_name;
    uint64_t size_bytes = 0;
    uint64_t modified_time = 0;
};

struct Collection {
    MediaId id;
    bool is_root = false;
    // Empty for the synthetic root collection. UI code owns its localized
    // display label, such as "Uncategorized".
    std::string directory_name;
    std::vector<Track> tracks;
};

struct ResolvedTrack {
    MediaId collection_id;
    bool collection_is_root = false;
    std::string directory_name;
    Track track;
};

class MediaCatalog {
public:
    MediaCatalog() = default;

    const std::vector<Collection>& Collections() const { return collections_; }
    // Returns an owning snapshot so rescans or catalog replacement cannot
    // leave asynchronous consumers with pointers into this catalog.
    std::optional<ResolvedTrack> Resolve(MediaId id) const;

private:
    friend class MediaCatalogBuilder;

    explicit MediaCatalog(std::vector<Collection>&& collections);

    std::vector<Collection> collections_;
};

enum class AddResult {
    kAdded,
    kDuplicate,
    kSkippedNotRegular,
    kSkippedEmptyFile,
    kSkippedInvalidName,
    kSkippedInvalidUtf8,
    kSkippedHidden,
    kSkippedUnsupportedExtension,
    kLimitExceeded,
    kIdCollision,
    kConflictingDuplicate,
    kBuilderFailed,
    kBuilderFinished,
};

enum class CatalogBuildError {
    kNone,
    kInvalidConfiguration,
    kLimitExceeded,
    kIdCollision,
    kConflictingDuplicate,
    kAlreadyFinished,
};

struct CatalogStats {
    size_t input_files = 0;
    size_t added_tracks = 0;
    size_t duplicate_files = 0;
    size_t skipped_not_regular = 0;
    size_t skipped_empty_files = 0;
    size_t skipped_invalid_names = 0;
    size_t skipped_invalid_utf8 = 0;
    size_t skipped_hidden = 0;
    size_t skipped_unsupported_extension = 0;
};

struct CatalogBuildResult {
    CatalogBuildError error = CatalogBuildError::kNone;
    MediaCatalog catalog;
    CatalogStats stats;

    bool Ok() const { return error == CatalogBuildError::kNone; }
};

class MediaCatalogBuilder {
public:
    // library_key must be a non-empty persistent card/library identity: equal
    // across reboot/remount of the same card and distinct for different cards.
    // Do not use a mount path, random boot value, or handle generation here.
    MediaCatalogBuilder(std::string library_key, CatalogLimits limits = {});

    AddResult Add(RawFile file);
    // Any fatal Add() error makes Finish() return an empty catalog. The first
    // fatal error follows discovery order; successful output is deterministic.
    CatalogBuildResult Finish();

private:
    size_t FindCollection(std::string_view directory_name) const;
    MediaId MakeCollectionId(std::string_view directory_name) const;
    MediaId MakeTrackId(std::string_view directory_name, std::string_view file_name) const;
    void Fail(CatalogBuildError error);

    std::string library_key_;
    CatalogLimits limits_;
    CatalogStats stats_;
    CatalogBuildError error_ = CatalogBuildError::kNone;
    bool finished_ = false;
    size_t total_tracks_ = 0;
    size_t total_name_bytes_ = 0;
    std::vector<Collection> collections_;
    std::unordered_set<uint64_t> used_id_values_;
};

// Validates Unicode scalar-value UTF-8, rejecting overlong forms, surrogates,
// truncation, and code points above U+10FFFF.
bool IsValidUtf8(std::string_view text);

// Applies the v1 filename policy only (.mp3, and .wav since F08). File
// attributes and byte size are handled by MediaCatalogBuilder::Add().
bool IsPlayableAudioName(std::string_view file_name);

// Deterministic, locale-independent ordering. ASCII digit runs are compared
// numerically without integer conversion. Non-digits use ASCII-only case
// folding followed by Unicode scalar order and a raw-byte tie-break. Valid
// UTF-8 sorts before invalid byte strings; invalid strings use raw-byte order.
bool NaturalNameLess(std::string_view lhs, std::string_view rhs);

}  // namespace media

#endif  // MEDIA_MEDIA_CATALOG_H_
