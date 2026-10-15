#include "media/media_catalog.h"

#include <algorithm>
#include <iostream>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

int failure_count = 0;

void Check(bool condition, const char* expression, const char* file, int line) {
    if (!condition) {
        std::cerr << file << ':' << line << ": check failed: " << expression << '\n';
        ++failure_count;
    }
}

#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __FILE__, __LINE__)

media::RawFile File(std::string collection, std::string name, uint64_t size = 100,
                    uint64_t modified_time = 1) {
    media::RawFile file;
    file.collection_name = std::move(collection);
    file.file_name = std::move(name);
    file.size_bytes = size;
    file.modified_time = modified_time;
    return file;
}

media::CatalogBuildResult Build(const std::vector<media::RawFile>& files,
                                std::string library_key = "card-a",
                                media::CatalogLimits limits = {}) {
    media::MediaCatalogBuilder builder(std::move(library_key), limits);
    for (const media::RawFile& file : files) {
        builder.Add(file);
    }
    return builder.Finish();
}

const media::Collection* FindCollection(const media::MediaCatalog& catalog,
                                        std::string_view directory_name) {
    for (const media::Collection& collection : catalog.Collections()) {
        if (collection.directory_name == directory_name) {
            return &collection;
        }
    }
    return nullptr;
}

const media::Track* FindTrack(const media::MediaCatalog& catalog, std::string_view directory_name,
                              std::string_view file_name) {
    const media::Collection* collection = FindCollection(catalog, directory_name);
    if (collection == nullptr) {
        return nullptr;
    }
    for (const media::Track& track : collection->tracks) {
        if (track.file_name == file_name) {
            return &track;
        }
    }
    return nullptr;
}

media::MediaId FindTrackId(const media::MediaCatalog& catalog, std::string_view directory_name,
                           std::string_view file_name) {
    const media::Track* track = FindTrack(catalog, directory_name, file_name);
    CHECK(track != nullptr);
    return track == nullptr ? media::MediaId{} : track->id;
}

std::vector<std::string> Sorted(std::vector<std::string> names) {
    std::sort(names.begin(), names.end(), media::NaturalNameLess);
    return names;
}

std::string Snapshot(const media::MediaCatalog& catalog) {
    std::ostringstream output;
    for (const media::Collection& collection : catalog.Collections()) {
        output << "C|" << collection.id.ToString() << '|' << collection.is_root << '|'
               << collection.directory_name << '\n';
        for (const media::Track& track : collection.tracks) {
            output << "T|" << track.id.ToString() << '|' << track.file_name << '|'
                   << track.size_bytes << '|' << track.modified_time << '\n';
        }
    }
    return output.str();
}

void TestUtf8Validation() {
    CHECK(media::IsValidUtf8(""));
    CHECK(media::IsValidUtf8("plain ASCII"));
    CHECK(media::IsValidUtf8(u8"第2集.mp3"));
    CHECK(media::IsValidUtf8(u8"Cafe\u0301.mp3"));
    CHECK(media::IsValidUtf8(u8"🙂.mp3"));

    CHECK(!media::IsValidUtf8(std::string("\xc0\xaf", 2)));          // Overlong slash.
    CHECK(!media::IsValidUtf8(std::string("\xe2\x82", 2)));          // Truncated sequence.
    CHECK(!media::IsValidUtf8(std::string("\xed\xa0\x80", 3)));      // Surrogate.
    CHECK(!media::IsValidUtf8(std::string("\xf4\x90\x80\x80", 4)));  // > U+10FFFF.
    CHECK(!media::IsValidUtf8(std::string("\x80", 1)));              // Stray continuation byte.
}

void TestFilenamePolicyAndFiltering() {
    CHECK(media::IsPlayableAudioName(u8"故事1.mp3"));
    CHECK(media::IsPlayableAudioName("story.MP3"));
    CHECK(media::IsPlayableAudioName("story.Mp3"));
    CHECK(media::IsPlayableAudioName("story.wav"));
    CHECK(media::IsPlayableAudioName("STORY.WaV"));
    CHECK(!media::IsPlayableAudioName(".hidden.mp3"));
    CHECK(!media::IsPlayableAudioName("._story.mp3"));
    CHECK(!media::IsPlayableAudioName("story.mp3.tmp"));
    CHECK(!media::IsPlayableAudioName("story.wave"));
    CHECK(!media::IsPlayableAudioName(".wav"));
    CHECK(!media::IsPlayableAudioName("story.flac"));
    CHECK(!media::IsPlayableAudioName("folder/story.mp3"));
    CHECK(!media::IsPlayableAudioName(std::string("bad-\xc0\xaf.mp3", 10)));

    media::MediaCatalogBuilder builder("card-a");
    CHECK(builder.Add(File("", u8"故事1.mp3")) == media::AddResult::kAdded);

    media::RawFile not_regular = File("", "directory.mp3");
    not_regular.is_regular = false;
    CHECK(builder.Add(not_regular) == media::AddResult::kSkippedNotRegular);
    CHECK(builder.Add(File("", "empty.mp3", 0)) == media::AddResult::kSkippedEmptyFile);
    CHECK(builder.Add(File("nested/dir", "story.mp3")) == media::AddResult::kSkippedInvalidName);
    CHECK(builder.Add(File("", std::string("\xc0\xaf.mp3", 6))) ==
          media::AddResult::kSkippedInvalidUtf8);

    media::RawFile hidden_attribute = File("", "hidden.mp3");
    hidden_attribute.is_hidden = true;
    CHECK(builder.Add(hidden_attribute) == media::AddResult::kSkippedHidden);
    CHECK(builder.Add(File("", ".hidden.mp3")) == media::AddResult::kSkippedHidden);
    CHECK(builder.Add(File("__MACOSX", "story.mp3")) == media::AddResult::kSkippedHidden);
    CHECK(builder.Add(File("", "cover.jpg")) == media::AddResult::kSkippedUnsupportedExtension);
    CHECK(builder.Add(File("Series", "STORY.MP3")) == media::AddResult::kAdded);

    media::CatalogBuildResult result = builder.Finish();
    CHECK(result.Ok());
    CHECK(result.stats.input_files == 10);
    CHECK(result.stats.added_tracks == 2);
    CHECK(result.stats.skipped_not_regular == 1);
    CHECK(result.stats.skipped_empty_files == 1);
    CHECK(result.stats.skipped_invalid_names == 1);
    CHECK(result.stats.skipped_invalid_utf8 == 1);
    CHECK(result.stats.skipped_hidden == 3);
    CHECK(result.stats.skipped_unsupported_extension == 1);

    media::MediaCatalogBuilder unsafe_builder("card-a");
    CHECK(unsafe_builder.Add(File("..", "story.mp3")) == media::AddResult::kSkippedInvalidName);
    CHECK(unsafe_builder.Add(File("", "folder\\story.mp3")) ==
          media::AddResult::kSkippedInvalidName);
    CHECK(unsafe_builder.Add(File("", std::string("bad\0.mp3", 8))) ==
          media::AddResult::kSkippedInvalidName);
    CHECK(unsafe_builder.Finish().Ok());
}

void TestNaturalOrdering() {
    CHECK(Sorted({u8"故事10.mp3", u8"故事2.mp3", u8"故事01.mp3", u8"故事1.mp3"}) ==
          std::vector<std::string>({u8"故事1.mp3", u8"故事01.mp3", u8"故事2.mp3", u8"故事10.mp3"}));
    CHECK(Sorted({u8"第10季第1集.mp3", u8"第2季第10集.mp3", u8"第2季第3集.mp3"}) ==
          std::vector<std::string>({u8"第2季第3集.mp3", u8"第2季第10集.mp3", u8"第10季第1集.mp3"}));
    CHECK(Sorted({"A10.mp3", "a2.mp3", "a02.mp3", "A2.mp3"}) ==
          std::vector<std::string>({"A2.mp3", "a2.mp3", "a02.mp3", "A10.mp3"}));

    const std::string hundred_nines(100, '9');
    CHECK(media::NaturalNameLess("item9.mp3", "item" + hundred_nines + ".mp3"));
    CHECK(media::NaturalNameLess("item" + std::string(99, '9') + ".mp3",
                                 "item" + hundred_nines + ".mp3"));

    const std::vector<std::string> corpus = {
        "A2.mp3",
        "a2.mp3",
        "a02.mp3",
        "a10.mp3",
        u8"故事1.mp3",
        u8"故事01.mp3",
        u8"故事2.mp3",
        u8"故事10.mp3",
        u8"Cafe\u0301.mp3",
        u8"Café.mp3",
        u8"🙂1.mp3",
        u8"🙂10.mp3",
        std::string("a1\xff", 3),
        std::string("a3\xfe", 3),
    };
    for (size_t lhs = 0; lhs < corpus.size(); ++lhs) {
        CHECK(!media::NaturalNameLess(corpus[lhs], corpus[lhs]));
        for (size_t rhs = 0; rhs < corpus.size(); ++rhs) {
            if (lhs == rhs) {
                continue;
            }
            CHECK(!(media::NaturalNameLess(corpus[lhs], corpus[rhs]) &&
                    media::NaturalNameLess(corpus[rhs], corpus[lhs])));
            for (size_t third = 0; third < corpus.size(); ++third) {
                if (media::NaturalNameLess(corpus[lhs], corpus[rhs]) &&
                    media::NaturalNameLess(corpus[rhs], corpus[third])) {
                    CHECK(media::NaturalNameLess(corpus[lhs], corpus[third]));
                }
            }
        }
    }
}

std::vector<media::RawFile> CatalogFixture() {
    return {
        File("01_西游记", u8"第10集.mp3", 110, 10),
        File("01_西游记", u8"第02集.MP3", 102, 2),
        File("01_西游记", u8"第2集.mp3", 100, 2),
        File("01_西游记", u8"第1集.mp3", 101, 1),
        File("01_西游记", "cover.jpg"),
        File("01_西游记/子目录", u8"第3集.mp3"),
        File("2_科学", "10.mp3", 210, 10),
        File("2_科学", "2.mp3", 202, 2),
        File("2_科学", "1.mp3", 201, 1),
        File("10_英语", "lesson1.mp3", 301, 1),
        File("empty", "notes.txt"),
        File(".hidden", "secret.mp3"),
        File("", u8"故事10.mp3", 410, 10),
        File("", u8"故事2.mp3", 402, 2),
    };
}

void TestCatalogModelAndDeterminism() {
    const std::vector<media::RawFile> fixture = CatalogFixture();
    media::CatalogBuildResult result = Build(fixture);
    CHECK(result.Ok());
    CHECK(result.catalog.Collections().size() == 4);

    const auto& collections = result.catalog.Collections();
    CHECK(collections[0].is_root);
    CHECK(collections[0].directory_name.empty());
    CHECK(collections[1].directory_name == "01_西游记");
    CHECK(collections[2].directory_name == "2_科学");
    CHECK(collections[3].directory_name == "10_英语");

    CHECK(collections[0].tracks.size() == 2);
    CHECK(collections[0].tracks[0].file_name == u8"故事2.mp3");
    CHECK(collections[0].tracks[1].file_name == u8"故事10.mp3");
    CHECK(collections[1].tracks.size() == 4);
    CHECK(collections[1].tracks[0].file_name == u8"第1集.mp3");
    CHECK(collections[1].tracks[1].file_name == u8"第2集.mp3");
    CHECK(collections[1].tracks[2].file_name == u8"第02集.MP3");
    CHECK(collections[1].tracks[3].file_name == u8"第10集.mp3");
    CHECK(FindCollection(result.catalog, "empty") == nullptr);
    CHECK(FindCollection(result.catalog, ".hidden") == nullptr);

    const media::Track* resolved_source = FindTrack(result.catalog, "01_西游记", u8"第2集.mp3");
    CHECK(resolved_source != nullptr);
    if (resolved_source != nullptr) {
        const std::optional<media::ResolvedTrack> resolved =
            result.catalog.Resolve(resolved_source->id);
        CHECK(resolved.has_value());
        if (resolved.has_value()) {
            CHECK(resolved->directory_name == "01_西游记");
            CHECK(resolved->track.file_name == u8"第2集.mp3");
        }
    }
    CHECK(!result.catalog.Resolve({}).has_value());

    const std::string expected_snapshot = Snapshot(result.catalog);
    for (uint32_t seed = 0; seed < 20; ++seed) {
        std::vector<media::RawFile> shuffled = fixture;
        std::mt19937 random(seed);
        std::shuffle(shuffled.begin(), shuffled.end(), random);
        media::CatalogBuildResult shuffled_result = Build(shuffled);
        CHECK(shuffled_result.Ok());
        CHECK(Snapshot(shuffled_result.catalog) == expected_snapshot);
    }
}

void TestStableIdsAndNormalizationContract() {
    media::CatalogBuildResult base =
        Build({File("01_西游记", u8"第2集.mp3"), File("", u8"故事2.mp3")});
    CHECK(base.Ok());
    const media::Collection* series = FindCollection(base.catalog, "01_西游记");
    const media::Collection* root = FindCollection(base.catalog, "");
    const media::Track* episode = FindTrack(base.catalog, "01_西游记", u8"第2集.mp3");
    const media::Track* root_story = FindTrack(base.catalog, "", u8"故事2.mp3");
    CHECK(series != nullptr);
    CHECK(root != nullptr);
    CHECK(episode != nullptr);
    CHECK(root_story != nullptr);
    if (series == nullptr || root == nullptr || episode == nullptr || root_story == nullptr) {
        return;
    }

    // Known-answer vectors lock the versioned byte serialization and hash.
    CHECK(root->id.ToString() == "m1-61e36d02c55c236f");
    CHECK(series->id.ToString() == "m1-2d66a435c6978633");
    CHECK(episode->id.ToString() == "m1-0aecf2dd2972f30a");
    CHECK(root_story->id.ToString() == "m1-a9299b3dbe1cf72c");
    CHECK(series->id != episode->id);  // Collection/track domain separation.

    media::CatalogBuildResult inserted =
        Build({File("01_西游记", u8"第1集.mp3"), File("01_西游记", u8"第2集.mp3"),
               File("", u8"故事1.mp3"), File("", u8"故事2.mp3")});
    CHECK(inserted.Ok());
    CHECK(FindTrackId(inserted.catalog, "01_西游记", u8"第2集.mp3") == episode->id);
    CHECK(FindTrackId(inserted.catalog, "", u8"故事2.mp3") == root_story->id);

    media::CatalogBuildResult renamed = Build({File("01_西游记", u8"第二集.mp3")});
    media::CatalogBuildResult moved = Build({File("02_西游记", u8"第2集.mp3")});
    media::CatalogBuildResult other_card = Build({File("01_西游记", u8"第2集.mp3")}, "card-b");
    media::CatalogBuildResult changed_metadata =
        Build({File("01_西游记", u8"第2集.mp3", 9999, 9999)});
    CHECK(renamed.Ok());
    CHECK(moved.Ok());
    CHECK(other_card.Ok());
    CHECK(changed_metadata.Ok());
    CHECK(FindTrackId(renamed.catalog, "01_西游记", u8"第二集.mp3") != episode->id);
    CHECK(FindTrackId(moved.catalog, "02_西游记", u8"第2集.mp3") != episode->id);
    CHECK(FindTrackId(other_card.catalog, "01_西游记", u8"第2集.mp3") != episode->id);
    CHECK(FindTrackId(changed_metadata.catalog, "01_西游记", u8"第2集.mp3") == episode->id);

    const std::string nfc = u8"Café.mp3";
    const std::string nfd = u8"Cafe\u0301.mp3";
    media::CatalogBuildResult unicode = Build({File("", nfc), File("", nfd)});
    CHECK(unicode.Ok());
    CHECK(unicode.catalog.Collections().size() == 1);
    CHECK(unicode.catalog.Collections()[0].tracks.size() == 2);
    CHECK(FindTrackId(unicode.catalog, "", nfc) != FindTrackId(unicode.catalog, "", nfd));
}

void TestDuplicatesAndLimits() {
    media::MediaCatalogBuilder duplicate_builder("card-a");
    CHECK(duplicate_builder.Add(File("Series", "1.mp3", 100, 1)) == media::AddResult::kAdded);
    CHECK(duplicate_builder.Add(File("Series", "1.mp3", 100, 1)) == media::AddResult::kDuplicate);
    media::CatalogBuildResult duplicate_result = duplicate_builder.Finish();
    CHECK(duplicate_result.Ok());
    CHECK(duplicate_result.stats.duplicate_files == 1);
    CHECK(duplicate_result.catalog.Collections()[0].tracks.size() == 1);
    CHECK(duplicate_builder.Add(File("Series", "2.mp3")) == media::AddResult::kBuilderFinished);
    CHECK(duplicate_builder.Finish().error == media::CatalogBuildError::kAlreadyFinished);

    media::MediaCatalogBuilder conflict_builder("card-a");
    CHECK(conflict_builder.Add(File("Series", "1.mp3", 100, 1)) == media::AddResult::kAdded);
    CHECK(conflict_builder.Add(File("Series", "1.mp3", 101, 1)) ==
          media::AddResult::kConflictingDuplicate);
    media::CatalogBuildResult conflict_result = conflict_builder.Finish();
    CHECK(conflict_result.error == media::CatalogBuildError::kConflictingDuplicate);
    CHECK(conflict_result.catalog.Collections().empty());

    media::MediaCatalogBuilder invalid_key("");
    CHECK(invalid_key.Add(File("", "1.mp3")) == media::AddResult::kBuilderFailed);
    CHECK(invalid_key.Finish().error == media::CatalogBuildError::kInvalidConfiguration);

    media::CatalogLimits collection_limit;
    collection_limit.max_collections = 1;
    media::MediaCatalogBuilder collection_builder("card-a", collection_limit);
    CHECK(collection_builder.Add(File("", "1.mp3")) == media::AddResult::kAdded);
    CHECK(collection_builder.Add(File("Series", "1.mp3")) == media::AddResult::kLimitExceeded);
    media::CatalogBuildResult collection_result = collection_builder.Finish();
    CHECK(collection_result.error == media::CatalogBuildError::kLimitExceeded);
    CHECK(collection_result.catalog.Collections().empty());

    media::CatalogLimits per_collection_limit;
    per_collection_limit.max_tracks_per_collection = 1;
    media::MediaCatalogBuilder per_collection_builder("card-a", per_collection_limit);
    CHECK(per_collection_builder.Add(File("Series", "1.mp3")) == media::AddResult::kAdded);
    CHECK(per_collection_builder.Add(File("Series", "2.mp3")) == media::AddResult::kLimitExceeded);
    CHECK(per_collection_builder.Finish().error == media::CatalogBuildError::kLimitExceeded);

    media::CatalogLimits total_track_limit;
    total_track_limit.max_tracks_total = 1;
    media::MediaCatalogBuilder total_track_builder("card-a", total_track_limit);
    CHECK(total_track_builder.Add(File("Series A", "1.mp3")) == media::AddResult::kAdded);
    CHECK(total_track_builder.Add(File("Series B", "1.mp3")) == media::AddResult::kLimitExceeded);
    CHECK(total_track_builder.Finish().error == media::CatalogBuildError::kLimitExceeded);

    media::CatalogLimits name_limit;
    name_limit.max_name_bytes_total = 5;
    name_limit.max_segment_bytes = 5;
    media::MediaCatalogBuilder name_builder("card-a", name_limit);
    CHECK(name_builder.Add(File("", "a.mp3")) == media::AddResult::kAdded);
    CHECK(name_builder.Add(File("", "b.mp3")) == media::AddResult::kLimitExceeded);
    CHECK(name_builder.Finish().error == media::CatalogBuildError::kLimitExceeded);

    media::CatalogLimits segment_limit;
    segment_limit.max_segment_bytes = 5;
    media::MediaCatalogBuilder segment_builder("card-a", segment_limit);
    CHECK(segment_builder.Add(File("", "long-name.mp3")) == media::AddResult::kSkippedInvalidName);
    CHECK(segment_builder.Finish().Ok());

    const std::string max_default_segment = std::string(1016, 'a') + ".mp3";
    const std::string oversized_default_segment = std::string(1017, 'a') + ".mp3";
    media::MediaCatalogBuilder default_segment_builder("card-a");
    CHECK(default_segment_builder.Add(File("", max_default_segment)) == media::AddResult::kAdded);
    CHECK(default_segment_builder.Add(File("", oversized_default_segment)) ==
          media::AddResult::kSkippedInvalidName);
    CHECK(default_segment_builder.Finish().Ok());
}

}  // namespace

int main() {
    TestUtf8Validation();
    TestFilenamePolicyAndFiltering();
    TestNaturalOrdering();
    TestCatalogModelAndDeterminism();
    TestStableIdsAndNormalizationContract();
    TestDuplicatesAndLimits();

    if (failure_count != 0) {
        std::cerr << failure_count << " media catalog test(s) failed\n";
        return 1;
    }
    std::cout << "media catalog host tests passed\n";
    return 0;
}
