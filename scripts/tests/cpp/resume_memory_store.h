#ifndef TESTS_RESUME_MEMORY_STORE_H_
#define TESTS_RESUME_MEMORY_STORE_H_

#include "media/playback_resume.h"

#include <algorithm>
#include <vector>

// Host fake shared by tests/demo, not a firmware storage implementation.
class MemoryResumeStore : public media::ResumeStore {
public:
    media::ResumeRead Read(const std::string& library, std::optional<media::MediaId> collection,
                           media::ResumeRecord& out) const override {
        if (read_error) {
            return media::ResumeRead::kError;
        }
        for (auto item = records.rbegin(); item != records.rend(); ++item) {
            if (item->library_key == library &&
                (!collection || item->media.collection_id == *collection)) {
                out = *item;
                return media::ResumeRead::kFound;
            }
        }
        return media::ResumeRead::kMissing;
    }

    media::ResumeWrite Write(const media::ResumeRecord& record) override {
        ++write_attempts;
        if (write_error) {
            return media::ResumeWrite::kError;
        }
        const auto found = std::find_if(records.begin(), records.end(), [&](const auto& item) {
            return item.library_key == record.library_key &&
                   item.media.collection_id == record.media.collection_id;
        });
        if (found == records.end() && records.size() >= 64) {
            return media::ResumeWrite::kFull;
        }
        if (found != records.end()) {
            records.erase(found);
        }
        records.push_back(record);
        return media::ResumeWrite::kOk;
    }

    std::vector<media::ResumeRecord> records;
    size_t write_attempts = 0;
    bool read_error = false;
    bool write_error = false;
};

#endif  // TESTS_RESUME_MEMORY_STORE_H_
