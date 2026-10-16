"""Exercise compact per-file bookmarks with real serialization and resume policy."""

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SOURCE = r'''
#include "boards/boilon-s3-gen2/m2_file_bookmarks.h"
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

namespace library = boilon::local_library;
using namespace media;
constexpr const char* kLibrary = "bookmark-test-card";

std::vector<std::string> erased;  // Which blobs were deleted, in order.

struct MemoryBlob : library::ResumeBlob {
    std::vector<uint8_t> bytes;
    bool write_error = false, erase_error = false;
    std::string name;
    mutable unsigned reads = 0;
    unsigned writes = 0;
    ResumeRead Read(std::vector<uint8_t>& out) const override {
        ++reads;
        if (bytes.empty()) return ResumeRead::kMissing;
        out = bytes;
        return ResumeRead::kFound;
    }
    bool Write(const std::vector<uint8_t>& in) override {
        ++writes;
        // Never a table without records: older releases read one as damaged.
        assert(in.size() >= 8 && (in[4] || in[5] || in[6] || in[7]));
        if (write_error) return false;
        bytes = in;
        return true;
    }
    bool Erase() override {
        erased.push_back(name);
        if (erase_error) return false;
        bytes.clear();
        return true;
    }
};

std::string Name(unsigned index, bool long_name = false) {
    std::string name = u8"故事";
    if (long_name)
        for (unsigned i = 0; i < 240; ++i) name += u8"中";
    return name + std::to_string(index) + ".mp3";
}

MediaCatalog Catalog(unsigned count = 3, bool long_name = false,
                     uint64_t size_change = 0, uint64_t time_change = 0) {
    MediaCatalogBuilder builder(kLibrary);
    for (unsigned i = 0; i < count; ++i)
        assert(builder.Add({u8"中文目录", Name(i, long_name),
                            1000 + i + size_change, 100 + i + time_change}) == AddResult::kAdded);
    auto built = builder.Finish();
    assert(built.Ok());
    return std::move(built.catalog);
}

ResolvedTrack SelectedTrack(const MediaCatalog& catalog, unsigned index, bool long_name = false) {
    for (const auto& collection : catalog.Collections())
        for (const auto& track : collection.tracks)
            if (track.file_name == Name(index, long_name)) return *catalog.Resolve(track.id);
    assert(false);
    return {};
}

ResumeRecord Record(const MediaCatalog& catalog, unsigned index, uint64_t position,
                    bool long_name = false) {
    return {1, kLibrary, SelectedTrack(catalog, index, long_name), position, 120000, 35};
}

// Select a specific file while keeping PlaybackResume's real validation policy.
struct TrackStore : ResumeStore {
    library::FileBookmarks& bookmarks;
    MediaId track;
    TrackStore(library::FileBookmarks& value, MediaId id) : bookmarks(value), track(id) {}
    ResumeRead Read(const std::string& key, std::optional<MediaId>,
                    ResumeRecord& out) const override {
        return bookmarks.ReadTrack(key, track, out);
    }
    ResumeWrite Write(const ResumeRecord& record) override { return bookmarks.Write(record); }
};

void Restart() {
    const auto catalog = Catalog();
    MemoryBlob compact, old;
    library::BlobResumeStore legacy(old);
    {
        library::FileBookmarks files(compact, legacy, catalog);
        PlaybackResume policy(catalog, kLibrary, files);
        assert(policy.Save(SelectedTrack(catalog, 0).track.id, 41000, 120000, 35, 1,
                           CheckpointReason::kTrackChange) == SaveStatus::kSaved);
        assert(policy.Save(SelectedTrack(catalog, 1).track.id, 62000, 120000, 40, 2,
                           CheckpointReason::kShutdown) == SaveStatus::kSaved);
    }
    assert(compact.bytes.size() == 132 && old.bytes.empty());
    // Fresh objects must recover both files from the serialized blob.
    library::BlobResumeStore restarted_legacy(old);
    library::FileBookmarks restarted(compact, restarted_legacy, catalog);
    for (unsigned index = 0; index < 2; ++index) {
        const auto id = SelectedTrack(catalog, index).track.id;
        TrackStore selected(restarted, id);
        PlaybackResume policy(catalog, kLibrary, selected);
        const auto exact = policy.Restore({}, ResumeMode::kSavedExact);
        assert(exact.status == ResumeStatus::kReady && exact.target->track.id == id);
        assert(exact.position_ms == (index == 0 ? 41000U : 62000U));
        const auto boot = policy.Restore({}, ResumeMode::kBoot);
        assert(boot.status == ResumeStatus::kReady && boot.auto_play);
        assert(boot.position_ms == (index == 0 ? 36000U : 57000U));
        assert(boot.volume_percent == (index == 0 ? 35 : 40));
    }
}

void CapacityAndLru() {
    const auto catalog = Catalog(35);
    MemoryBlob compact, old;
    library::BlobResumeStore legacy(old);
    library::FileBookmarks configured(compact, legacy, catalog);
    const unsigned capacity = BOILON_S3_GEN2_BOOKMARK_CAPACITY;
    for (unsigned i = 0; i <= capacity; ++i)
        assert(configured.Write(Record(catalog, i, 1000 + i)) == ResumeWrite::kOk);
    assert(compact.bytes.size() == 12 + capacity * 60);
    ResumeRecord out;
    assert(configured.ReadTrack(kLibrary, SelectedTrack(catalog, 0).track.id, out) == ResumeRead::kMissing);
    assert(configured.ReadTrack(kLibrary, SelectedTrack(catalog, capacity).track.id, out) == ResumeRead::kFound);
    assert(out.position_ms == 1000 + capacity);

    MemoryBlob two;
    library::FileBookmarks lru(two, legacy, catalog, 2);
    assert(lru.Write(Record(catalog, 0, 1000)) == ResumeWrite::kOk);
    assert(lru.Write(Record(catalog, 1, 2000)) == ResumeWrite::kOk);
    assert(lru.Write(Record(catalog, 0, 3000)) == ResumeWrite::kOk);
    assert(lru.Write(Record(catalog, 2, 4000)) == ResumeWrite::kOk);
    library::FileBookmarks fresh(two, legacy, catalog, 2);
    assert(fresh.ReadTrack(kLibrary, SelectedTrack(catalog, 0).track.id, out) == ResumeRead::kFound);
    assert(out.position_ms == 3000);
    assert(fresh.ReadTrack(kLibrary, SelectedTrack(catalog, 1).track.id, out) == ResumeRead::kMissing);
    assert(fresh.ReadTrack(kLibrary, SelectedTrack(catalog, 2).track.id, out) == ResumeRead::kFound);
    assert(two.bytes.size() == 132);
}

void CompactNames() {
    size_t sizes[2] = {};
    for (unsigned variant = 0; variant < 2; ++variant) {
        const bool long_name = variant != 0;
        const auto catalog = Catalog(32, long_name);
        MemoryBlob compact, old;
        library::BlobResumeStore legacy(old);
        {
            library::FileBookmarks files(compact, legacy, catalog, 32);
            for (unsigned i = 0; i < 32; ++i)
                assert(files.Write(Record(catalog, i, 1000 + i, long_name)) == ResumeWrite::kOk);
        }
        sizes[variant] = compact.bytes.size();
        library::FileBookmarks fresh(compact, legacy, catalog, 32);
        ResumeRecord out;
        for (unsigned i = 0; i < 32; ++i) {
            assert(fresh.ReadTrack(kLibrary, SelectedTrack(catalog, i, long_name).track.id, out)
                   == ResumeRead::kFound);
            assert(out.position_ms == 1000 + i && out.media.track.file_name == Name(i, long_name));
        }
    }
    assert(sizes[0] == 1932 && sizes[1] == sizes[0]);
}

void LegacyMigration() {
    const auto catalog = Catalog();
    MemoryBlob compact, old;
    library::BlobResumeStore legacy(old);
    assert(legacy.Write(Record(catalog, 0, 41000)) == ResumeWrite::kOk);
    const auto original = old.bytes;
    const auto original_writes = old.writes;
    library::FileBookmarks files(compact, legacy, catalog, 2);
    ResumeRecord out;
    assert(files.ReadTrack(kLibrary, SelectedTrack(catalog, 0).track.id, out) == ResumeRead::kFound);
    assert(out.position_ms == 41000 && compact.bytes.empty());
    assert(files.ReadTrack(kLibrary, SelectedTrack(catalog, 1).track.id, out) == ResumeRead::kMissing);

    // First successful write migrates the retained state_v1 record into BFR2.
    assert(files.Write(Record(catalog, 1, 62000)) == ResumeWrite::kOk);
    assert(compact.bytes.size() == 132 && compact.bytes[2] == 'R' && compact.bytes[3] == '2');
    assert(old.bytes == original && old.writes == original_writes);
    assert(files.ReadTrack(kLibrary, SelectedTrack(catalog, 0).track.id, out) == ResumeRead::kFound);
    assert(files.Write(Record(catalog, 2, 73000)) == ResumeWrite::kOk);
    library::BlobResumeStore restarted_legacy(old);
    library::FileBookmarks fresh(compact, restarted_legacy, catalog, 2);
    assert(fresh.ReadTrack(kLibrary, SelectedTrack(catalog, 0).track.id, out) == ResumeRead::kMissing);
    assert(fresh.ReadTrack(kLibrary, SelectedTrack(catalog, 1).track.id, out) == ResumeRead::kFound);
    assert(out.position_ms == 62000);
    assert(fresh.ReadTrack(kLibrary, SelectedTrack(catalog, 2).track.id, out) == ResumeRead::kFound);
    assert(old.bytes == original && old.writes == original_writes);
}

void IntegrityAndWriteFailure() {
    const auto catalog = Catalog();
    MemoryBlob compact, old;
    library::BlobResumeStore legacy(old);
    assert(legacy.Write(Record(catalog, 0, 90000)) == ResumeWrite::kOk);
    library::FileBookmarks files(compact, legacy, catalog, 2);
    assert(files.Write(Record(catalog, 1, 62000)) == ResumeWrite::kOk);
    const auto valid = compact.bytes;
    ResumeRecord out;
    compact.write_error = true;
    assert(files.Write(Record(catalog, 2, 73000)) == ResumeWrite::kError);
    assert(compact.bytes == valid);
    library::FileBookmarks retained(compact, legacy, catalog, 2);
    assert(retained.ReadTrack(kLibrary, SelectedTrack(catalog, 1).track.id, out) == ResumeRead::kFound);
    assert(out.position_ms == 62000);
    assert(retained.ReadTrack(kLibrary, SelectedTrack(catalog, 2).track.id, out) == ResumeRead::kMissing);
    compact.write_error = false;
    for (unsigned corruption = 0; corruption < 2; ++corruption) {
        compact.bytes = valid;
        if (corruption == 0) compact.bytes.back() ^= 1;
        else compact.bytes.pop_back();
        const auto damaged = compact.bytes;
        const auto writes = compact.writes;
        library::FileBookmarks fresh(compact, legacy, catalog, 2);
        assert(fresh.ReadTrack(kLibrary, SelectedTrack(catalog, 0).track.id, out) == ResumeRead::kError);
        assert(fresh.Write(Record(catalog, 2, 73000)) == ResumeWrite::kError);
        assert(compact.bytes == damaged && compact.writes == writes);
    }
    compact.bytes = valid;
    library::FileBookmarks fresh(compact, legacy, catalog, 2);
    assert(fresh.ReadTrack(kLibrary, SelectedTrack(catalog, 1).track.id, out) == ResumeRead::kFound);
    assert(out.position_ms == 62000);
}

void StaleMetadata() {
    const auto catalog = Catalog();
    MemoryBlob compact, old;
    library::BlobResumeStore legacy(old);
    library::FileBookmarks files(compact, legacy, catalog);
    assert(files.Write(Record(catalog, 0, 41000)) == ResumeWrite::kOk);
    const auto bytes = compact.bytes;
    for (unsigned changed = 0; changed < 2; ++changed) {
        const auto newer = Catalog(3, false, changed == 0 ? 1 : 0, changed == 1 ? 2 : 0);
        const auto id = SelectedTrack(newer, 0).track.id;
        assert(id == SelectedTrack(catalog, 0).track.id);
        library::FileBookmarks fresh(compact, legacy, newer);
        TrackStore selected(fresh, id);
        PlaybackResume policy(newer, kLibrary, selected);
        const auto restored = policy.Restore({}, ResumeMode::kSavedExact);
        assert(restored.status == ResumeStatus::kStaleMedia && !restored.target);
        assert(fresh.Positions(kLibrary).empty() && compact.bytes == bytes);
    }
}

void EndOfFile() {
    const auto catalog = Catalog();
    MemoryBlob compact, old;
    library::BlobResumeStore legacy(old);
    const auto id = SelectedTrack(catalog, 0).track.id;
    {
        library::FileBookmarks files(compact, legacy, catalog);
        PlaybackResume policy(catalog, kLibrary, files);
        assert(policy.Save(id, 120000, 120000, 35, 1, CheckpointReason::kTrackChange)
               == SaveStatus::kSaved);
    }
    library::FileBookmarks fresh(compact, legacy, catalog);
    TrackStore selected(fresh, id);
    PlaybackResume policy(catalog, kLibrary, selected);
    for (auto mode : {ResumeMode::kSavedExact, ResumeMode::kBoot}) {
        const auto plan = policy.Restore({}, mode);
        assert(plan.status == ResumeStatus::kReady && plan.position_ms == 0);
    }
}

// 2026-10-16: forgetting progress. 西游记 holds two files, 三国 one.
MediaCatalog TwoDirectories() {
    MediaCatalogBuilder builder(kLibrary);
    for (unsigned i = 0; i < 3; ++i)
        assert(builder.Add({i < 2 ? u8"西游记" : u8"三国", Name(i), 1000 + i, 100 + i}) ==
               AddResult::kAdded);
    auto built = builder.Finish();
    assert(built.Ok());
    return std::move(built.catalog);
}

MediaId Directory(const MediaCatalog& catalog, const std::string& name) {
    for (const auto& collection : catalog.Collections())
        if (collection.directory_name == name) return collection.id;
    assert(false);
    return {};
}

void Forget() {
    const auto catalog = TwoDirectories();
    const auto xiyou = Directory(catalog, u8"西游记"), sanguo = Directory(catalog, u8"三国");
    MemoryBlob compact, old;
    compact.name = "v2";
    old.name = "v1";
    library::BlobResumeStore legacy(old);
    library::FileBookmarks files(compact, legacy, catalog);
    assert(files.Write(Record(catalog, 0, 41000)) == ResumeWrite::kOk);
    assert(files.Write(Record(catalog, 1, 42000)) == ResumeWrite::kOk);
    assert(files.Write(Record(catalog, 2, 43000)) == ResumeWrite::kOk);
    auto other = Record(catalog, 0, 44000);  // 西游记 on another card.
    other.library_key = "other-card";
    assert(files.Write(other) == ResumeWrite::kOk);
    const auto writes = compact.writes;
    // One directory of this card: 三国 and the other card's 西游记 stay.
    assert(files.Forget(kLibrary, xiyou) == ResumeWrite::kOk);
    assert(compact.writes == writes + 1 && compact.bytes.size() == 12 + 2 * 60 && erased.empty());
    const auto positions = files.Positions(kLibrary);
    assert(positions.size() == 1 && positions[0].first == SelectedTrack(catalog, 2).track.id);
    library::BlobResumeStore restarted_legacy(old);
    library::FileBookmarks fresh(compact, restarted_legacy, catalog);
    ResumeRecord out;
    for (unsigned index : {0u, 1u})
        assert(fresh.ReadTrack(kLibrary, SelectedTrack(catalog, index).track.id, out) ==
               ResumeRead::kMissing);
    assert(fresh.ReadTrack(kLibrary, SelectedTrack(catalog, 2).track.id, out) ==
           ResumeRead::kFound && out.position_ms == 43000);
    assert(fresh.ReadTrack("other-card", SelectedTrack(catalog, 0).track.id, out) ==
           ResumeRead::kFound && out.position_ms == 44000);
    // Nothing of it left: nothing is written.
    assert(files.Forget(kLibrary, xiyou) == ResumeWrite::kOk && compact.writes == writes + 1);
    assert(files.Forget(kLibrary, sanguo) == ResumeWrite::kOk);
    assert(compact.writes == writes + 2 && compact.bytes.size() == 12 + 60 && erased.empty());
    assert(files.Positions(kLibrary).empty());
    // The last record: no empty table, both keys go, the old format first.
    // Otherwise this old 西游记 record would come back.
    assert(legacy.Write(Record(catalog, 1, 9000)) == ResumeWrite::kOk);
    assert(files.Forget("other-card", xiyou) == ResumeWrite::kOk);
    assert(compact.writes == writes + 2 && compact.bytes.empty() && old.bytes.empty());
    assert(erased == (std::vector<std::string>{"v1", "v2"}));
    library::FileBookmarks after(compact, legacy, catalog);
    assert(after.ReadTrack(kLibrary, SelectedTrack(catalog, 1).track.id, out) ==
           ResumeRead::kMissing);
    // A damaged table: one directory cannot be forgotten, everything can, and
    // saving works again afterwards.
    assert(files.Write(Record(catalog, 0, 41000)) == ResumeWrite::kOk);
    compact.bytes.back() ^= 1;
    const auto damaged = compact.bytes;
    erased.clear();
    assert(files.Forget(kLibrary, sanguo) == ResumeWrite::kError && compact.bytes == damaged);
    assert(erased.empty());
    assert(files.Forget(kLibrary, std::nullopt) == ResumeWrite::kOk && compact.bytes.empty());
    assert(erased == (std::vector<std::string>{"v1", "v2"}) && files.Positions(kLibrary).empty());
    assert(files.Write(Record(catalog, 2, 43000)) == ResumeWrite::kOk);
    // Failed deletes and writes say so and keep what was there; v2 never goes
    // while v1 stays.
    const auto kept = compact.bytes;
    old.erase_error = true;
    erased.clear();
    assert(files.Forget(kLibrary, std::nullopt) == ResumeWrite::kError && compact.bytes == kept);
    assert(erased == (std::vector<std::string>{"v1"}));
    old.erase_error = false;
    compact.erase_error = true;
    assert(files.Forget(kLibrary, sanguo) == ResumeWrite::kError && compact.bytes == kept);
    assert(files.Positions(kLibrary).size() == 1);
    compact.erase_error = false;
    assert(files.Write(Record(catalog, 0, 41000)) == ResumeWrite::kOk);
    const auto two = compact.bytes;
    compact.write_error = true;
    assert(files.Forget(kLibrary, sanguo) == ResumeWrite::kError && compact.bytes == two);
    assert(files.Positions(kLibrary).size() == 2);
    compact.write_error = false;
}

// Before the first table only the old format holds this card's records.
void ForgetBeforeFirstWrite() {
    const auto catalog = TwoDirectories();
    const auto xiyou = Directory(catalog, u8"西游记"), sanguo = Directory(catalog, u8"三国");
    MemoryBlob compact, old;
    library::BlobResumeStore legacy(old);
    assert(legacy.Write(Record(catalog, 0, 41000)) == ResumeWrite::kOk);
    assert(legacy.Write(Record(catalog, 2, 43000)) == ResumeWrite::kOk);
    const auto original = old.bytes;
    library::FileBookmarks files(compact, legacy, catalog);
    // The first table holds what is left; 三国 does not come back from v1.
    assert(files.Forget(kLibrary, sanguo) == ResumeWrite::kOk);
    assert(compact.bytes.size() == 12 + 60 && old.bytes == original && erased.empty());
    library::FileBookmarks fresh(compact, legacy, catalog);
    ResumeRecord out;
    assert(fresh.ReadTrack(kLibrary, SelectedTrack(catalog, 2).track.id, out) ==
           ResumeRead::kMissing);
    assert(fresh.ReadTrack(kLibrary, SelectedTrack(catalog, 0).track.id, out) ==
           ResumeRead::kFound && out.position_ms == 41000);
    // Nothing saved of it: nothing written; the last of it: v1 goes, no table.
    MemoryBlob none, only;
    library::BlobResumeStore only_legacy(only);
    assert(only_legacy.Write(Record(catalog, 0, 41000)) == ResumeWrite::kOk);
    library::FileBookmarks first(none, only_legacy, catalog);
    assert(first.Forget(kLibrary, sanguo) == ResumeWrite::kOk);
    assert(none.writes == 0 && !only.bytes.empty() && erased.empty());
    assert(first.Forget(kLibrary, xiyou) == ResumeWrite::kOk);
    assert(none.writes == 0 && none.bytes.empty() && only.bytes.empty() && erased.size() == 2);
    assert(first.ReadTrack(kLibrary, SelectedTrack(catalog, 0).track.id, out) ==
           ResumeRead::kMissing);
}

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string mode(argv[1]);
    if (mode == "restart") Restart();
    else if (mode == "capacity") CapacityAndLru();
    else if (mode == "compact") CompactNames();
    else if (mode == "migration") LegacyMigration();
    else if (mode == "integrity") IntegrityAndWriteFailure();
    else if (mode == "stale") StaleMetadata();
    else if (mode == "eof") EndOfFile();
    else if (mode == "forget") Forget();
    else if (mode == "forget_first") ForgetBeforeFirstWrite();
    else return 2;
    std::cout << mode << " PASS\n";
}
'''


class M2FileBookmarkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ["CXX"]) if "CXX" in os.environ else [shutil.which("c++")]
        if not compiler or not compiler[0]:
            raise AssertionError("existing native compiler required; no download")
        cls.temporary = tempfile.TemporaryDirectory(prefix="m2-file-bookmarks-")
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        source = directory / "bookmarks.cc"
        source.write_text(SOURCE)
        cls.executables = {}
        for capacity in (2, 32):
            executable = directory / f"test-{capacity}"
            command = compiler + [
                "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                f"-DBOILON_S3_GEN2_BOOKMARK_CAPACITY={capacity}", "-I", str(ROOT / "main"),
                str(source), str(ROOT / "main/media/media_catalog.cc"),
                str(ROOT / "main/media/playback_resume.cc"), "-o", str(executable),
            ]
            result = subprocess.run(command, capture_output=True, text=True, timeout=45,
                                    stdin=subprocess.DEVNULL, cwd=ROOT)
            if result.returncode:
                raise AssertionError(result.stdout + result.stderr)
            cls.executables[capacity] = executable

    def scenario(self, mode, capacity=32):
        result = subprocess.run([str(self.executables[capacity]), mode], capture_output=True,
                                text=True, timeout=10, stdin=subprocess.DEVNULL,
                                cwd=self.temporary.name)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(mode + " PASS", result.stdout)

    def test_real_serialization_restarts_and_restores_two_files(self):
        self.scenario("restart")

    def test_capacity_configuration_two_and_lru_update(self):
        self.scenario("capacity", 2)

    def test_capacity_configuration_thirty_two_and_lru_update(self):
        self.scenario("capacity", 32)

    def test_long_chinese_names_keep_32_records_at_1932_bytes(self):
        self.scenario("compact")

    def test_legacy_fallback_first_write_migration_and_no_resurrection(self):
        self.scenario("migration")

    def test_crc_truncation_and_write_failure_preserve_retained_bytes(self):
        self.scenario("integrity")

    def test_changed_size_or_mtime_is_rejected_by_real_resume_policy(self):
        self.scenario("stale")

    def test_eof_checkpoint_restores_at_start(self):
        self.scenario("eof")

    def test_forget_one_directory_the_last_record_or_all_never_an_empty_table(self):
        self.scenario("forget")

    def test_forget_before_the_first_table_keeps_old_records_from_coming_back(self):
        self.scenario("forget_first")


if __name__ == "__main__":
    unittest.main()
