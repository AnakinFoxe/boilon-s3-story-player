#pragma once
#include <type_traits>
#include <utility>
#include "media/playback_resume.h"

namespace boilon::local_library {
// Nonblocking board-local boundary. Accepted means queued, not persisted.
class ResumePort {
public:
    virtual ~ResumePort() = default;
    virtual bool Restore(std::optional<media::MediaId>, media::ResumeMode) = 0;
    virtual std::optional<uint64_t> KnownPosition(media::MediaId) { return uint64_t{0}; }
    virtual bool TakeRestore(media::ResumePlan&) = 0;
    virtual bool Checkpoint(const media::ResolvedTrack&, uint64_t, uint64_t, int, uint64_t,
                            media::CheckpointReason) = 0;
    virtual uint64_t KnownDuration(media::MediaId) { return 0; }
};
template <typename P, typename = void>
struct HasResume : std::false_type {};
template <typename P>
struct HasResume<
    P, std::void_t<decltype(std::declval<P&>().StartAt(std::declval<std::string>(), uint64_t{})),
                   decltype(std::declval<P&>().PositionMs()),
                   decltype(std::declval<P&>().DurationMs())>> : std::true_type {};
template <typename P>
class ResumePlayer {
public:
    using Result = typename P::Result;
    explicit ResumePlayer(P& player) : player_(player) {}
    void Offset(uint64_t offset) { offset_ = offset; }
    uint64_t StartedOffset() const { return started_offset_; }
    bool Start(const std::string& path) {
        const auto offset = std::exchange(offset_, 0);
        started_offset_ = offset;
        if constexpr (HasResume<P>::value)
            return player_.StartAt(path, offset);
        else
            return offset == 0 && player_.Start(path);
    }
    bool Running() const { return player_.Running(); }
    Result LastResult() const { return player_.LastResult(); }
    bool Pause(bool paused) { return player_.Pause(paused); }
    void Stop() { player_.Stop(); }

private:
    P& player_;
    uint64_t offset_ = 0;
    uint64_t started_offset_ = 0;
};
}  // namespace boilon::local_library
