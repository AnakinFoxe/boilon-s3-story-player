#pragma once

#include <cstdint>
#include <memory>
#include <string>

class AudioService;

// One bounded file/decode worker for MP3 and, by its .wav name, PCM WAV (F08);
// all speaker writes remain in AudioService.
// Call controls from one owner task. AudioService must outlive this object AND its worker.
// Start takes an already-resolved mounted-file path; it never mounts/writes media.
class LocalMp3Player {
public:
    enum class Result { kIdle, kRunning, kFinished, kStopped, kError };

    explicit LocalMp3Player(AudioService& audio);
    ~LocalMp3Player();  // Requests cancellation; does not block the caller.
    LocalMp3Player(const LocalMp3Player&) = delete;
    LocalMp3Player& operator=(const LocalMp3Player&) = delete;
    bool Start(const std::string& path);
    // Scan actual frame lengths, then decode/discard a bounded preroll.
    // Unrecognized layouts keep the linear stream-parser path. Never estimate
    // offsets from bitrate; cancellation stays cooperative on either path.
    // Frames seen earlier in the same file let the scan start close to the target.
    bool StartAt(const std::string& path, uint64_t position_ms);
    uint64_t PositionMs() const;  // Completed Codec output, not queued PCM.
    // MP3: 0 until EOF; fast seek counts prefix frame samples. WAV: from the header.
    uint64_t DurationMs() const;
    bool Pause(bool paused);
    void Stop();
    bool Running() const;
    Result LastResult() const;
    // After kError: the file could not be opened or read (the card), as
    // opposed to content the decoder or the WAV reader rejected.
    bool ReadFailed() const;

private:
    struct State;
    std::shared_ptr<State> state_;
    static void Run(const std::shared_ptr<State>& state);
};
