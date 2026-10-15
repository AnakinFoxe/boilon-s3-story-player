// Appended after production declarations and selected AudioService methods.
#include <unistd.h>
void TestServiceOwnershipAndBounds() {
    AudioService service;
    AudioCodec codec;
    assert(service.BeginLocalPlayback() == 0);
    assert(service.LocalOutputSampleRate() == 0);
    service.codec_ = &codec;
    assert(service.BeginLocalPlayback() == 0);
    service.service_stopped_.store(false);
    assert(service.LocalOutputSampleRate() == 24000);
    assert(service.IsPlaybackIdle());
    auto packet = std::make_unique<AudioStreamPacket>();
    assert(service.PushPacketToDecodeQueue(std::move(packet), false));
    assert(!service.IsPlaybackIdle());
    assert(service.BeginLocalPlayback() == 0);
    service.ResetDecoder();
    assert(service.IsPlaybackIdle());
    service.decode_in_flight_ = true;
    assert(service.BeginLocalPlayback() == 0);
    service.decode_in_flight_ = false;
    service.output_in_flight_ = true;
    assert(service.BeginLocalPlayback() == 0);
    service.output_in_flight_ = false;

    const auto session = service.BeginLocalPlayback();
    assert(session != 0 && service.IsLocalPlaybackActive(session));
    assert(service.BeginLocalPlayback() == 0);
    for (bool wait : {false, true}) {
        assert(!service.PushPacketToDecodeQueue(std::make_unique<AudioStreamPacket>(), wait));
    }
    assert(service.audio_decode_queue_.empty());
    std::vector<int16_t> empty, oversized(961, 7), pcm(960, 42);
    assert(service.SubmitLocalPcm(session, empty) == LocalPcmResult::kInvalid);
    assert(service.SubmitLocalPcm(session, oversized) == LocalPcmResult::kInvalid);
    assert(oversized.size() == 961);
    assert(service.SubmitLocalPcm(0, pcm) == LocalPcmResult::kStale && pcm.size() == 960);
    assert(service.SubmitLocalPcm(session, pcm) == LocalPcmResult::kAccepted && pcm.empty());
    assert(service.audio_playback_queue_.front().local_source);
    assert(service.audio_playback_queue_.front().generation == session);
    pcm.assign(1, -17);
    assert(service.SubmitLocalPcm(session, pcm) == LocalPcmResult::kAccepted);
    pcm.assign(100, 9);
    for (int attempt = 0; attempt < 20; ++attempt) {
        assert(service.SubmitLocalPcm(session, pcm) == LocalPcmResult::kWouldBlock);
        assert(pcm.size() == 100 && service.audio_playback_queue_.size() == 2);
    }
    service.audio_playback_queue_.pop_front();
    assert(service.PauseLocalPlayback(session, true));
    assert(service.SubmitLocalPcm(session, pcm) == LocalPcmResult::kWouldBlock);
    assert(!service.PauseLocalPlayback(session + 1, false));
    assert(service.PauseLocalPlayback(session, false));
    assert(service.SubmitLocalPcm(session, pcm) == LocalPcmResult::kAccepted);
    service.EndLocalPlayback(session + 1);
    assert(service.IsLocalPlaybackActive(session) && service.audio_playback_queue_.size() == 2);
    service.EndLocalPlayback(session);
    assert(!service.IsLocalPlaybackActive(session) && service.IsPlaybackIdle());
    pcm.assign(1, 1);
    assert(service.SubmitLocalPcm(session, pcm) == LocalPcmResult::kStale);
    auto replacement = service.BeginLocalPlayback();
    assert(replacement != 0 && replacement != session);
    service.EndLocalPlayback(session);
    assert(service.IsLocalPlaybackActive(replacement));
    service.ResetDecoder();
    assert(!service.IsLocalPlaybackActive(replacement));
    assert(service.SubmitLocalPcm(replacement, pcm) == LocalPcmResult::kStale);
    assert(!service.PauseLocalPlayback(replacement, true));
    replacement = service.BeginLocalPlayback();
    assert(replacement != 0);
    service.Stop();
    assert(!service.IsLocalPlaybackActive(replacement));
    assert(service.SubmitLocalPcm(replacement, pcm) == LocalPcmResult::kStale);
    assert(!service.PushPacketToDecodeQueue(std::make_unique<AudioStreamPacket>(), false));
}

void TestServiceOutputAndPause() {
    event_bits = 0;
    AudioService service;
    AudioCodec codec;
    service.codec_ = &codec;
    service.service_stopped_.store(false);
    const auto session = service.BeginLocalPlayback();
    std::vector<int16_t> pcm{10, -20, 30};
    assert(service.SubmitLocalPcm(session, pcm) == LocalPcmResult::kAccepted);
    assert(service.PauseLocalPlayback(session, true));
    std::atomic<bool> output_seen{false};
    codec.on_output = [&] {
        output_seen.store(true);
        service.Stop();
    };
    std::thread output([&] { service.AudioOutputTask(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    assert(!output_seen.load());
    assert(service.PauseLocalPlayback(session, false));
    output.join();
    assert(output_seen.load());
    assert((codec.output == std::vector<std::vector<int16_t>>{{10, -20, 30}}));
    assert(!service.output_in_flight_ && service.IsPlaybackIdle());
}

void TestAudioTestingCannotInjectIntoLocalSession() {
    event_bits = 0;
    AudioService service;
    AudioCodec codec;
    service.codec_ = &codec;
    service.service_stopped_ = false;
    const auto session = service.BeginLocalPlayback();
    assert(session != 0);
    std::vector<int16_t> pcm{123};
    assert(service.SubmitLocalPcm(session, pcm) == LocalPcmResult::kAccepted);
    service.audio_testing_queue_.push_back(std::make_unique<AudioStreamPacket>());
    service.EnableAudioTesting(false);
    assert(service.audio_decode_queue_.empty());
    assert(service.audio_testing_queue_.empty());
    assert(service.IsLocalPlaybackActive(session));
    assert(service.audio_playback_queue_.size() == 1);
    assert(service.audio_playback_queue_.front().pcm == std::vector<int16_t>{123});
    service.EndLocalPlayback(session);

    service.EnableAudioTesting(true);
    assert(xEventGroupGetBits(nullptr) & AS_EVENT_AUDIO_TESTING_RUNNING);
    assert(service.BeginLocalPlayback() == 0);
    service.EnableAudioTesting(false);
    assert(service.audio_decode_queue_.empty());
    service.audio_testing_queue_.push_back(std::make_unique<AudioStreamPacket>());
    assert(service.BeginLocalPlayback() == 0);
    service.audio_testing_queue_.clear();
    service.EnableAudioTesting(true);
    service.audio_testing_queue_.push_back(std::make_unique<AudioStreamPacket>());
    service.EnableAudioTesting(false);
    assert(!(xEventGroupGetBits(nullptr) & AS_EVENT_AUDIO_TESTING_RUNNING));
    assert(service.audio_testing_queue_.empty() && service.audio_decode_queue_.size() == 1);
    assert(!service.IsPlaybackIdle());
    service.ResetDecoder();
}

void TestCompletedOutputPositionAndSessionIsolation() {
    event_bits = 0;
    AudioService service;
    AudioCodec codec;
    service.codec_ = &codec;
    service.service_stopped_ = false;
    const auto session = service.BeginLocalPlayback();
    std::vector<int16_t> first{1, 2, 3}, queued{4, 5};
    assert(service.SubmitLocalPcm(session, first) == LocalPcmResult::kAccepted);
    assert(service.SubmitLocalPcm(session, queued) == LocalPcmResult::kAccepted);
    assert(service.LocalPlaybackSamples(session) == 0);  // Both blocks only queued.
    codec.on_output = [&] {
        assert(service.LocalPlaybackSamples(session) == 0);  // Codec has not returned.
        assert(service.output_in_flight_);
        assert(service.audio_playback_queue_.size() == 1);
        service.EndLocalPlayback(session);  // Discard queued block, retain in-flight one.
        assert(service.LocalPlaybackSamples(session) == 0);
        service.Stop();
    };
    std::thread output([&] { service.AudioOutputTask(); });
    output.join();
    assert(service.LocalPlaybackSamples(session) == 3);
    assert(codec.output.size() == 1);
    assert(service.LocalPlaybackSamples(0) == 0);
    service.service_stopped_ = false;
    const auto replacement = service.BeginLocalPlayback();
    assert(replacement != 0 && replacement != session);
    assert(service.LocalPlaybackSamples(session) == 0);
    assert(service.LocalPlaybackSamples(replacement) == 0);
    AudioTask stale{};
    stale.local_source = true;
    stale.generation = session;
    stale.pcm = {99, 99, 99};
    service.audio_playback_queue_.push_back(std::move(stale));
    std::vector<int16_t> replacement_pcm{6, 7};
    assert(service.SubmitLocalPcm(replacement, replacement_pcm) == LocalPcmResult::kAccepted);
    codec.on_output = [&] {
        assert(service.LocalPlaybackSamples(replacement) == 0);
        service.Stop();
    };
    std::thread replaced_output([&] { service.AudioOutputTask(); });
    replaced_output.join();
    assert(service.LocalPlaybackSamples(replacement) == 2);
    assert(service.LocalPlaybackSamples(session) == 0);
    assert(codec.output.size() == 2 && codec.output.back() == std::vector<int16_t>({6, 7}));
}

void TestSessionGenerationBeyond32Bits() {
    event_bits = 0;
    AudioService service;
    AudioCodec codec;
    service.codec_ = &codec;
    service.service_stopped_ = false;
    service.playback_generation_ = (uint64_t{1} << 32) - 1;
    const auto session = service.BeginLocalPlayback();
    assert(session == (uint64_t{1} << 32));
    std::vector<int16_t> pcm{7, 8};
    assert(service.SubmitLocalPcm(static_cast<uint32_t>(session), pcm) == LocalPcmResult::kStale);
    assert(pcm == std::vector<int16_t>({7, 8}));
    assert(service.SubmitLocalPcm(session, pcm) == LocalPcmResult::kAccepted);
    codec.on_output = [&] { service.Stop(); };
    std::thread output([&] { service.AudioOutputTask(); });
    output.join();
    assert(service.LocalPlaybackSamples(session) == 2);
    assert(codec.output == std::vector<std::vector<int16_t>>({{7, 8}}));
    service.service_stopped_ = false;
    // Protocol producers share the same 64-bit generation after a local session.
    assert(service.PushPacketToDecodeQueue(std::make_unique<AudioStreamPacket>(), false));
    service.ResetDecoder();
}

void TestBlockedProtocolProducerInvalidatedByReset() {
    event_bits = 0;
    AudioService service;
    AudioCodec codec;
    service.codec_ = &codec;
    service.service_stopped_ = false;
    service.playback_generation_ = (uint64_t{1} << 32) + 7;
    for (size_t i = 0; i < MAX_DECODE_PACKETS_IN_QUEUE; ++i)
        assert(service.PushPacketToDecodeQueue(std::make_unique<AudioStreamPacket>(), false));
    std::atomic<bool> started{false}, finished{false};
    bool accepted = true;
    std::thread producer([&] {
        started = true;
        accepted = service.PushPacketToDecodeQueue(std::make_unique<AudioStreamPacket>(), true);
        finished = true;
    });
    while (!started.load())
        std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    assert(!finished.load());  // No output consumer can release the full queue.
    service.ResetDecoder();
    producer.join();
    assert(!accepted && service.audio_decode_queue_.empty());
    assert(service.playback_generation_ == (uint64_t{1} << 32) + 8);
}

void TestLocalDrainedCallbackTiming() {
    event_bits = 0;
    for (bool end_inside_codec : {false, true}) {
        AudioService service;
        AudioCodec codec;
        service.codec_ = &codec;
        service.service_stopped_ = false;
        std::atomic<unsigned> drained{0};
        AudioServiceCallbacks callbacks;
        callbacks.on_playback_drained = [&] {
            assert(!service.local_playback_active_ && !service.output_in_flight_);
            assert(service.IsPlaybackIdle());
            ++drained;
            service.Stop();
        };
        service.callbacks_ = callbacks;
        const auto session = service.BeginLocalPlayback();
        std::vector<int16_t> pcm{1, 2, 3};
        assert(service.SubmitLocalPcm(session, pcm) == LocalPcmResult::kAccepted);
        if (end_inside_codec) {
            std::vector<int16_t> queued{4, 5};
            assert(service.SubmitLocalPcm(session, queued) == LocalPcmResult::kAccepted);
            codec.on_output = [&] {
                assert(service.output_in_flight_);
                service.EndLocalPlayback(session);
                assert(drained.load() == 0);  // The codec has not returned yet.
            };
        }
        std::thread output([&] { service.AudioOutputTask(); });
        if (!end_inside_codec) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            while (service.LocalPlaybackSamples(session) == 0 &&
                   std::chrono::steady_clock::now() < deadline)
                std::this_thread::yield();
            assert(service.LocalPlaybackSamples(session) == 3);
            assert(drained.load() == 0 && service.IsLocalPlaybackActive(session));
            service.EndLocalPlayback(session);
        }
        output.join();
        assert(drained.load() == 1 && service.LocalPlaybackSamples(session) == 3);
        assert(codec.output == std::vector<std::vector<int16_t>>({{1, 2, 3}}));
    }
}

void WriteSyntheticFile(const std::string& path, size_t bytes) {
    FILE* file = std::fopen(path.c_str(), "wb");
    assert(file);
    const std::array<uint8_t, 512> contents{};
    while (bytes) {
        const size_t count = std::min(bytes, contents.size());
        assert(std::fwrite(contents.data(), 1, count, file) == count);
        bytes -= count;
    }
    assert(std::fclose(file) == 0);
}

struct WorkerFixture {
    AudioService service;
    AudioCodec codec;
    LocalMp3Player player{service};
    std::vector<int16_t> consumed;
    size_t largest_queue = 0;

    WorkerFixture() {
        fake::reset();
        service.codec_ = &codec;
        service.service_stopped_ = false;
        fake::on_delay = [&] { DrainOne(); };
    }

    // Deterministic scheduler boundary: consume one existing queue block. The
    // real AudioOutputTask is tested separately above, including its codec sink.
    void DrainOne() {
        std::lock_guard<std::mutex> lock(service.audio_queue_mutex_);
        largest_queue = std::max(largest_queue, service.audio_playback_queue_.size());
        assert(service.audio_playback_queue_.size() <= 2);
        if (service.audio_playback_queue_.empty()) return;
        auto& task = service.audio_playback_queue_.front();
        assert(task.local_source && task.generation == service.playback_generation_);
        assert(!task.pcm.empty() && task.pcm.size() <= 960);
        consumed.insert(consumed.end(), task.pcm.begin(), task.pcm.end());
        if (task.generation == service.local_position_session_)
            service.local_completed_samples_ += task.pcm.size();
        service.audio_playback_queue_.pop_front();
        service.audio_queue_cv_.notify_all();
    }

    void Run(const std::string& path, LocalMp3Player::Result expected) {
        assert(player.Start(path));
        assert(player.Running() && player.LastResult() == LocalMp3Player::Result::kRunning);
        assert(!player.Start(path));
        fake::run_task();
        assert(!player.Running() && player.LastResult() == expected);
        assert(fake::task_creates == 1 && fake::task_deletes == 1);
        assert(!service.local_playback_active_ && service.IsPlaybackIdle());
        assert(codec.output.empty());  // Worker never directly calls the codec.
    }
};

void TestWorkerEofDownmixAndBackpressure(const std::string& path) {
    for (size_t bytes : {512U, 513U, 1024U, 1300U}) {
        WriteSyntheticFile(path, bytes);
        WorkerFixture fixture;
        fixture.Run(path, LocalMp3Player::Result::kFinished);
        const auto chunks = (bytes + 511) / 512;
        assert(fake::decode_calls == static_cast<int>(chunks));
        assert(fake::reads.size() == chunks && fake::eos.size() == chunks);
        for (size_t i = 0; i < chunks; ++i) {
            assert(fake::reads[i] == std::min<size_t>(512, bytes - i * 512));
            assert(fake::eos[i] == (i + 1 == chunks));
        }
        assert(fixture.consumed.size() == chunks * 1152);
        assert(std::all_of(fixture.consumed.begin(), fixture.consumed.end(),
                           [](int16_t value) { return value == 500; }));
        assert(fixture.largest_queue == 2 && fake::open_calls == 1 && fake::close_calls == 1);
    }

    WriteSyntheticFile(path, 1536);
    WorkerFixture fixture;
    int held_decodes = 0;
    fake::on_delay = [&] {
        if (fake::delay_calls == 1) held_decodes = fake::decode_calls;
        if (fake::delay_calls <= 5) {
            assert(fake::decode_calls == held_decodes);
            assert(fixture.service.audio_playback_queue_.size() == 2);
        } else {
            fixture.DrainOne();
        }
    };
    fixture.Run(path, LocalMp3Player::Result::kFinished);
    assert(held_decodes == 2 && fixture.consumed.size() == 3 * 1152);
}

void TestWorkerPauseAndCancellation(const std::string& path) {
    WriteSyntheticFile(path, 1024);
    {
        WorkerFixture fixture;
        assert(fixture.player.Start(path) && fixture.player.Pause(true));
        fake::on_delay = [&] {
            if (fake::delay_calls <= 5) {
                assert(fake::decode_calls == 0 && fixture.consumed.empty());
                if (fake::delay_calls == 5) assert(fixture.player.Pause(false));
            } else {
                fixture.DrainOne();
            }
        };
        fake::run_task();
        assert(fixture.player.LastResult() == LocalMp3Player::Result::kFinished);
        assert(!fixture.player.Running() && !fixture.player.Pause(false));
        assert(fixture.consumed.size() == 2304);
    }
    for (bool reset : {false, true}) {
        WorkerFixture fixture;
        fake::on_delay = [&] {
            assert(fixture.service.audio_playback_queue_.size() == 2);
            if (reset) fixture.service.ResetDecoder();
            else fixture.player.Stop();
            assert(!fixture.player.Start(path));  // The old task must exit first.
        };
        fixture.Run(path, LocalMp3Player::Result::kStopped);
        assert(fake::delay_calls == 1 && fixture.consumed.empty());
        assert(fake::close_calls == 1);
    }
    {
        WorkerFixture fixture;
        uint64_t replacement = 0;
        fake::on_delay = [&] {
            fixture.service.ResetDecoder();
            replacement = fixture.service.BeginLocalPlayback();
            std::vector<int16_t> replacement_pcm{123};
            assert(fixture.service.SubmitLocalPcm(replacement, replacement_pcm) == LocalPcmResult::kAccepted);
        };
        assert(fixture.player.Start(path));
        fake::run_task();
        assert(fixture.player.LastResult() == LocalMp3Player::Result::kStopped);
        assert(fixture.service.IsLocalPlaybackActive(replacement));
        assert(fixture.service.audio_playback_queue_.size() == 1);
        assert(fixture.service.audio_playback_queue_.front().pcm == std::vector<int16_t>{123});
        fixture.service.EndLocalPlayback(replacement);
    }
    {
        WorkerFixture fixture;
        assert(fixture.player.Start(path));
        fixture.player.Stop();
        fake::run_task();
        assert(fake::open_calls == 0 && fake::decode_calls == 0);
        assert(fixture.player.LastResult() == LocalMp3Player::Result::kStopped);
    }
    {
        WorkerFixture fixture;
        auto player = std::make_unique<LocalMp3Player>(fixture.service);
        assert(player->Start(path));
        player.reset();
        fake::run_task();
        assert(fake::open_calls == 0 && fake::task_deletes == 1);
        assert(!fixture.service.local_playback_active_);
    }
}

void TestWorkerDecodeErrors(const std::string& path) {
    WriteSyntheticFile(path, 1024);
    for (auto error : {fake::Decode::error, fake::Decode::oversized, fake::Decode::no_progress,
                       fake::Decode::overconsumed, fake::Decode::output_only, fake::Decode::odd}) {
        WorkerFixture fixture;
        fake::decode = error;
        fixture.Run(path, LocalMp3Player::Result::kError);
        assert(fake::close_calls == 1 && fake::decode_calls <= 65);
        if (error == fake::Decode::output_only) assert(fake::decode_calls == 65);
        else assert(fake::decode_calls == 1 && fixture.consumed.empty());
        assert(!fixture.player.ReadFailed());  // The content, not the card.
    }
    for (int error = 0; error < 6; ++error) {
        WorkerFixture fixture;
        if (error == 0) fake::open_error = true;
        if (error == 1) fake::info_error = true;
        if (error == 2) fake::info.bits_per_sample = 24;
        if (error == 3) fake::info.channel = 3;
        if (error == 4) fake::info.sample_rate = 96000;
        if (error == 5) fake::format_change = true;
        fixture.Run(path, LocalMp3Player::Result::kError);
        assert(fake::close_calls == (error == 0 ? 0 : 1));
        assert(!fixture.player.ReadFailed());
    }
    {
        WorkerFixture fixture;
        fake::decode = fake::Decode::buffered;
        fixture.Run(path, LocalMp3Player::Result::kFinished);
        assert(fake::decode_calls == 2 && fixture.consumed.size() == 1152);
    }
    WriteSyntheticFile(path, 0);
    {
        WorkerFixture fixture;
        fixture.Run(path, LocalMp3Player::Result::kError);
        assert(fake::decode_calls == 0 && fake::close_calls == 1);
        assert(!fixture.player.ReadFailed());
    }
    {
        WorkerFixture fixture;  // The card cannot open it (pulled card).
        fixture.Run(path + ".missing", LocalMp3Player::Result::kError);
        assert(fake::open_calls == 0 && fake::decode_calls == 0);
        assert(fixture.player.ReadFailed());
    }
}

void TestWorkerRateConversion(const std::string& path) {
    WriteSyntheticFile(path, 512);
    for (uint32_t channels : {1U, 2U}) {
        WorkerFixture fixture;
        fixture.codec.rate = 48000;
        fake::info.sample_rate = 8000;
        fake::info.channel = channels;
        fixture.Run(path, LocalMp3Player::Result::kFinished);
        assert(fixture.consumed.size() == 1152 * 6 && fixture.largest_queue == 2);
        assert(fake::converter_open_calls == 1 && fake::converter_close_calls == 1);
    }
    for (auto error : {fake::Convert::open_error, fake::Convert::bound_error,
                       fake::Convert::oversized_bound, fake::Convert::process_error,
                       fake::Convert::oversized_output}) {
        WorkerFixture fixture;
        fake::info.sample_rate = 16000;
        fake::convert = error;
        fixture.Run(path, LocalMp3Player::Result::kError);
        assert(fake::converter_open_calls == 1);
        assert(fake::converter_close_calls == (error == fake::Convert::open_error ? 0 : 1));
        assert(fake::close_calls == 1 && fixture.consumed.empty());
    }
}

void TestWorkerMidstreamErrorsAndRestart(const std::string& path) {
    WriteSyntheticFile(path, 4 * 512);
    for (int fault = 0; fault < 3; ++fault) {
        WorkerFixture fixture;
        if (fault == 0)
            fake::file_fault = fake::FileFault::read;
        if (fault == 1)
            fake::file_fault = fake::FileFault::lookahead;
        if (fault < 2)
            fake::file_fault_call = 3;
        else
            fake::decode_error_call = 3;
        assert(fixture.player.Start(path));
        fake::run_task();
        assert(!fixture.player.Running());
        assert(fixture.player.LastResult() == LocalMp3Player::Result::kError);
        assert(fixture.player.ReadFailed() == (fault < 2));  // Read faults, not decoding.
        assert(fixture.player.DurationMs() == 0);  // Failure is never natural EOF.
        assert(fake::decode_calls == (fault == 2 ? 3 : 2));
        assert(fixture.consumed.size() == 1152);  // PCM was already produced and played.
        assert(fake::close_calls == 1 && fake::file_close_calls == 1);
        assert(fake::task_creates == 1 && fake::task_deletes == 1);
        assert(fake::file_error == nullptr && fake::pending_task == nullptr);
        assert(!fixture.service.local_playback_active_ && fixture.service.IsPlaybackIdle());
        assert(fixture.service.audio_playback_queue_.empty());

        // Clearing the injected fault is sufficient: the same player/service
        // can start again, without reset or a replacement worker owner.
        fake::file_fault = fake::FileFault::none;
        fake::decode_error_call = 0;
        assert(fixture.player.Start(path));
        fake::run_task();
        assert(!fixture.player.Running());
        assert(fixture.player.LastResult() == LocalMp3Player::Result::kFinished);
        assert(!fixture.player.ReadFailed());
        assert(fixture.player.DurationMs() == 192);
        assert(fixture.consumed.size() == 5 * 1152);
        assert(fake::close_calls == 2 && fake::file_close_calls == 2);
        assert(fake::task_creates == 2 && fake::task_deletes == 2);
        assert(!fixture.service.local_playback_active_ && fixture.service.IsPlaybackIdle());
    }
    {
        WorkerFixture fixture;
        assert(fixture.player.Start(path) && fixture.player.Pause(true));
        fake::on_delay = [&] {
            assert(fake::decode_calls == 0 && fixture.consumed.empty());
            fixture.player.Stop();
        };
        fake::run_task();
        assert(!fixture.player.Running());
        assert(fixture.player.LastResult() == LocalMp3Player::Result::kStopped);
        assert(!fixture.service.local_playback_active_ && fixture.service.IsPlaybackIdle());
        fake::on_delay = [&] { fixture.DrainOne(); };
        assert(fixture.player.Start(path));
        fake::run_task();
        assert(!fixture.player.Running());
        assert(fixture.player.LastResult() == LocalMp3Player::Result::kFinished);
        assert(fixture.consumed.size() == 4 * 1152);
        assert(fake::task_creates == 2 && fake::task_deletes == 2);
    }
}

void TestWorkerStartFailure(const std::string& path) {
    WriteSyntheticFile(path, 512);
    WorkerFixture fixture;
    for (const auto& invalid : {std::string{}, std::string{"relative.mp3"},
                               std::string(513, '/'), std::string("/a\0b", 4)}) {
        assert(!fixture.player.Start(invalid));
    }
    assert(fake::task_creates == 0 && !fixture.player.Running());
    fake::task_error = true;
    assert(!fixture.player.Start(path));
    assert(!fixture.player.Running() && fixture.player.LastResult() == LocalMp3Player::Result::kError);
    assert(!fixture.service.local_playback_active_ && fake::pending_task == nullptr);
    fake::task_error = false;
    assert(fixture.player.Start(path));
    fake::run_task();
    assert(fixture.player.LastResult() == LocalMp3Player::Result::kFinished);
}

// Synthetic decoder frames prove trim/count/control semantics, not MP3 bitstreams.
void TestWorkerStartAtAndPosition(const std::string& path) {
    WriteSyntheticFile(path, 512);
    for (uint64_t target : {0U, 25U, 48U, 49U}) {
        WorkerFixture fixture;
        fake::info.channel = 1;
        fake::sample_ramp = true;
        assert(fixture.player.StartAt(path, target));
        assert(fixture.player.PositionMs() == 0 && fixture.player.DurationMs() == 0);
        fake::on_delay = [&] {
            if (fixture.consumed.empty()) assert(fixture.player.PositionMs() == 0);
            fixture.DrainOne();
            if (!fixture.consumed.empty())
                assert(fixture.player.PositionMs() == target + fixture.consumed.size() * 1000 / 24000);
        };
        fake::run_task();
        assert(!fixture.player.Running());
        assert(fixture.player.LastResult() == LocalMp3Player::Result::kFinished);
        assert(fixture.player.DurationMs() == 48);
        const size_t skipped = std::min<uint64_t>(1152, target * 24);
        assert(fixture.consumed.size() == 1152 - skipped);
        for (size_t i = 0; i < fixture.consumed.size(); ++i)
            assert(fixture.consumed[i] == static_cast<int16_t>(skipped + i));
        assert(fixture.player.PositionMs() == 48);
        const auto replacement = fixture.service.BeginLocalPlayback();
        assert(replacement);
        std::vector<int16_t> replacement_pcm(960, 7);
        assert(fixture.service.SubmitLocalPcm(replacement, replacement_pcm) == LocalPcmResult::kAccepted);
        fixture.DrainOne();
        assert(fixture.service.LocalPlaybackSamples(replacement) == 960);
        assert(fixture.player.PositionMs() == 48);
        fixture.service.EndLocalPlayback(replacement);
    }
    {
        WorkerFixture fixture;
        fake::info.channel = 1;
        fake::info.sample_rate = 16000;
        fake::sample_ramp = true;
        assert(fixture.player.StartAt(path, 25));
        fake::run_task();
        assert(fixture.player.LastResult() == LocalMp3Player::Result::kFinished);
        assert(fixture.consumed.size() == 1728 - 600);
        for (size_t i = 0; i < fixture.consumed.size(); ++i)
            assert(fixture.consumed[i] == static_cast<int16_t>((i + 600) * 16000 / 24000));
        assert(fixture.player.DurationMs() == 72 && fixture.player.PositionMs() == 72);
        assert(fake::converter_open_calls == 1 && fake::converter_close_calls == 1);
    }
    WriteSyntheticFile(path, 3 * 512);
    for (bool cancel : {false, true}) {
        WorkerFixture fixture;
        assert(fixture.player.StartAt(path, 25));
        fake::on_delay = [&] {
            if (fake::delay_calls == 1) {
                assert(fake::decode_calls == 2);  // File has not reached EOF.
                assert(fixture.player.DurationMs() == 0);
                assert(fixture.player.PositionMs() == 0);  // Two queued blocks only.
            }
            fixture.DrainOne();
            assert(fixture.player.PositionMs() == 25 + fixture.consumed.size() * 1000 / 24000);
            if (cancel) fixture.player.Stop();
        };
        fake::run_task();
        assert(fixture.player.LastResult() == (cancel ? LocalMp3Player::Result::kStopped :
                                                       LocalMp3Player::Result::kFinished));
        assert(fixture.consumed.size() == (cancel ? 552U : 2856U));
        assert(fixture.player.PositionMs() == (cancel ? 48U : 144U));
        assert(fixture.player.DurationMs() == (cancel ? 0U : 144U));
    }
    WriteSyntheticFile(path, 300 * 512);
    {
        WorkerFixture fixture;
        assert(fixture.player.StartAt(path, 20000));
        fake::on_delay = [&] {
            assert(fake::decode_calls == 256);
            assert(fixture.service.audio_playback_queue_.empty());
            assert(fixture.player.PositionMs() == 0 && fixture.player.DurationMs() == 0);
            fixture.player.Stop();
        };
        fake::run_task();
        assert(fake::delay_calls == 1 && fake::decode_calls == 256);
        assert(fixture.player.LastResult() == LocalMp3Player::Result::kStopped);
        assert(fixture.consumed.empty());
        assert(fixture.player.PositionMs() == 0 && fixture.player.DurationMs() == 0);
        assert(fake::close_calls == 1 && fake::task_deletes == 1);
    }
}

void TestLongSeekSkipsPrefixConversion(const std::string& path) {
    constexpr size_t frames = 310, prefix = 300;
    constexpr uint64_t target = prefix * 72 + 25;  // 16 kHz, 1152 samples/frame.
    WriteSyntheticFile(path, frames * 512);
    WorkerFixture fixture;
    fake::info.channel = 1;
    fake::info.sample_rate = 16000;
    fake::sample_ramp = true;
    assert(fixture.player.StartAt(path, target));
    fake::on_delay = [&] {
        if (static_cast<size_t>(fake::decode_calls) <= prefix) {
            assert(fake::converter_process_calls == 0);
            assert(fixture.consumed.empty() && fixture.player.PositionMs() == 0);
        }
        fixture.DrainOne();
    };
    fake::run_task();
    assert(fixture.player.LastResult() == LocalMp3Player::Result::kFinished);
    assert(fake::decode_calls == frames);
    assert(fake::converter_process_calls == static_cast<int>(frames - prefix));
    assert(fixture.consumed.size() == (frames - prefix) * 1728 - 600);
    const size_t source_start = prefix * 1152 + 400;
    for (size_t i = 0; i < 1128; ++i)
        assert(fixture.consumed[i] == static_cast<int16_t>(source_start + i * 16000 / 24000));
    assert(fixture.player.DurationMs() == frames * 72);
    assert(fixture.player.PositionMs() == frames * 72);

    {
        WorkerFixture fractional;
        fractional.codec.rate = 16000;
        fake::info.sample_rate = 44100;
        fake::info.channel = 1;
        fake::sample_ramp = true;
        const uint64_t fractional_target = prefix * 1152 * 1000 / 44100 + 25;
        const size_t source_start = fractional_target * 44100 / 1000;
        const size_t skipped_frames = source_start / 1152;
        const size_t first_remaining = 1152 - source_start % 1152;
        assert(fractional.player.StartAt(path, fractional_target));
        fake::run_task();
        assert(fractional.player.LastResult() == LocalMp3Player::Result::kFinished);
        assert(fake::converter_process_calls == frames - skipped_frames);
        assert(fractional.consumed.size() ==
               first_remaining * 16000 / 44100 +
                   (frames - skipped_frames - 1) * (1152 * 16000 / 44100));
        assert(fractional.consumed.front() == static_cast<int16_t>(source_start));
        assert(fractional.player.PositionMs() ==
               fractional_target + fractional.consumed.size() * 1000 / 16000);
        assert(fractional.player.DurationMs() == frames * 1152 * 1000 / 44100);
    }

    for (bool cancel : {false, true}) {
        WorkerFixture controlled;
        fake::info.channel = 1;
        fake::info.sample_rate = 16000;
        assert(controlled.player.StartAt(path, target));
        fake::on_delay = [&] {
            if (fake::delay_calls == 1) {
                assert(fake::decode_calls == 256 && fake::converter_process_calls == 0);
                assert(controlled.player.PositionMs() == 0 && controlled.consumed.empty());
                if (cancel)
                    controlled.player.Stop();
                else
                    assert(controlled.player.Pause(true));
            } else if (fake::delay_calls == 2) {
                assert(fake::decode_calls == 256 && fake::converter_process_calls == 0);
                assert(controlled.player.Pause(false));
            } else {
                controlled.DrainOne();
            }
        };
        fake::run_task();
        assert(controlled.player.LastResult() ==
               (cancel ? LocalMp3Player::Result::kStopped : LocalMp3Player::Result::kFinished));
        assert(fake::converter_process_calls == (cancel ? 0 : static_cast<int>(frames - prefix)));
        assert(controlled.player.PositionMs() == (cancel ? 0 : frames * 72));
    }
}

void TestSerializedCheckpointRestartAndSeek(const std::string& path) {
    namespace library = boilon::local_library;
    struct FileBlob : library::ResumeBlob {
        std::string path;
        explicit FileBlob(std::string value) : path(std::move(value)) {}
        media::ResumeRead Read(std::vector<uint8_t>& bytes) const override {
            FILE* file = std::fopen(path.c_str(), "rb");
            if (!file) return media::ResumeRead::kMissing;
            assert(std::fseek(file, 0, SEEK_END) == 0);
            const auto size = std::ftell(file);
            assert(size >= 0 && size <= 8192 && std::fseek(file, 0, SEEK_SET) == 0);
            bytes.resize(size);
            assert(std::fread(bytes.data(), 1, bytes.size(), file) == bytes.size());
            assert(std::fclose(file) == 0);
            return media::ResumeRead::kFound;
        }
        bool Write(const std::vector<uint8_t>& bytes) override {
            const auto next = path + ".new";
            FILE* file = std::fopen(next.c_str(), "wb");
            assert(file && std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size());
            assert(std::fclose(file) == 0 && std::rename(next.c_str(), path.c_str()) == 0);
            return true;
        }
    };
    WriteSyntheticFile(path, 240 * 512);  // Fake decoder: 240 frames * 1152 samples at 24kHz.
    const auto root = path.substr(0, path.find_last_of('/'));
    const auto name = path.substr(path.find_last_of('/') + 1);
    media::MediaCatalogBuilder builder("synthetic-resume");
    assert(builder.Add({"", name, 240 * 512, 1}) == media::AddResult::kAdded);
    const auto catalog = builder.Finish().catalog;
    const auto track = *catalog.Resolve(catalog.Collections()[0].tracks[0].id);
    uint64_t saved = 0;
    {
        FileBlob disk(path + ".resume");
        library::BlobResumeStore store(disk);
        media::PlaybackResume resume(catalog, "synthetic-resume", store);
        WorkerFixture fixture;
        fake::on_delay = [&] {
            fixture.DrainOne();
            if (!saved && fixture.player.PositionMs() >= 6000) {
                saved = fixture.player.PositionMs();
                assert(library::SaveProgress(resume, track, fixture.player, 11520, 35, 10000,
                    media::CheckpointReason::kPause) == media::SaveStatus::kSaved);
            }
        };
        fixture.Run(path, LocalMp3Player::Result::kFinished);
        assert(saved >= 6000 && fixture.player.DurationMs() == 11520);
    }
    {
        FileBlob disk(path + ".resume");  // Fresh backend/store/policy/player objects.
        library::BlobResumeStore store(disk);
        media::PlaybackResume reboot(catalog, "synthetic-resume", store);
        const auto plan = reboot.Restore(std::nullopt, media::ResumeMode::kBoot);
        assert(plan.status == media::ResumeStatus::kReady && plan.position_ms == saved - 5000);
        WorkerFixture fixture;
        assert(library::StartResume(plan, root, fixture.player));
        fake::run_task();
        assert(fixture.player.LastResult() == LocalMp3Player::Result::kFinished);
        assert(fixture.consumed.size() == 240 * 1152 - plan.position_ms * 24);
        assert(fixture.player.PositionMs() == 11520 && fixture.player.DurationMs() == 11520);
        std::printf("resume integration: file checkpoint=%llu ms; fresh objects boot=%llu ms; decoded prefix skipped; final=11520 ms (decoder fake)\n",
            static_cast<unsigned long long>(saved), static_cast<unsigned long long>(plan.position_ms));
    }
    assert(std::remove((path + ".resume").c_str()) == 0);
}

// Real Layer III frame boundaries plus a fake PCM decoder: validates file seek,
// sample clock and work reduction, not the ESP decoder's audible warmup quality.
void WriteFramedMp3(const std::string& path, size_t frames, unsigned version = 3,
                    uint8_t tag_body = 10) {
    FILE* file = std::fopen(path.c_str(), "wb");
    assert(file);
    std::vector<uint8_t> tag(10 + tag_body, 0);
    const uint8_t header[10] = {'I', 'D', '3', 4, 0, 0, 0, 0, 0, tag_body};
    std::copy(header, header + 10, tag.begin());
    assert(std::fwrite(tag.data(), 1, tag.size(), file) == tag.size());
    const unsigned mpeg1[] = {112, 128, 160}, mpeg2[] = {64, 80, 96};
    const unsigned rate = version == 3 ? 44100 : version == 2 ? 22050 : 11025;
    for (size_t i = 0; i < frames; ++i) {
        const unsigned bitrate = 8 + i % 3, padding = i % 2;
        std::vector<uint8_t> bytes(
            (version == 3 ? 144000 * mpeg1[i % 3] : 72000 * mpeg2[i % 3]) / rate + padding, 0);
        bytes[0] = 0xff;
        bytes[1] = version == 3 ? 0xfb : version == 2 ? 0xf3 : 0xe3;
        bytes[2] = static_cast<uint8_t>((bitrate << 4) | (padding << 1));
        bytes[3] = 0xc0;  // MPEG1 LayerIII, 44.1kHz mono; variable lengths.
        for (int j = 0; j < 4; ++j)
            bytes[40 + j] = i >> (24 - 8 * j);
        assert(std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size());
    }
    assert(std::fclose(file) == 0);
}
void TestFastFramedResume(const std::string& path) {
    constexpr size_t frames = 34510, prefix = 34500;
    const uint64_t target = prefix * 1152 * 1000 / 44100 + 19;
    const uint64_t source_start = target * 44100 / 1000;
    WriteFramedMp3(path, frames);
    WorkerFixture fixture;
    fixture.codec.rate = fake::info.sample_rate = 44100;
    fake::info.channel = 1;
    fake::sample_ramp = true;
    assert(fixture.player.StartAt(path, target));
    fake::run_task();
    assert(fixture.player.LastResult() == LocalMp3Player::Result::kFinished);
    assert(fake::frame_mode && fake::decode_calls < 64);  // Not 34,500 prefix decodes.
    assert(fixture.consumed.size() == frames * 1152 - source_start);
    for (size_t i = 0; i < fixture.consumed.size(); ++i)
        assert(fixture.consumed[i] == static_cast<int16_t>(source_start + i));
    assert(fixture.player.DurationMs() == frames * 1152 * 1000 / 44100);
    assert(fixture.player.PositionMs() == target + fixture.consumed.size() * 1000 / 44100);
    // Size, the first block and the resume frame: the block refills follow in file
    // order without a seek each (F01; the old scan seeked 6,904 times here).
    assert(fake::file_seek_calls == 4);
    assert(fake::file_buffer_bytes == 8192);  // Multi-sector SD reads, not 128-byte ones.
    std::printf("fast resume: ~15min target; %d decoded frames; exact suffix PCM (decoder fake)\n",
                fake::decode_calls);
    {
        WorkerFixture buffered;
        buffered.codec.rate = fake::info.sample_rate = 44100;
        fake::info.channel = 1;
        fake::sample_ramp = true;
        fake::decode = fake::Decode::buffered;  // Initial preroll may buffer without PCM.
        assert(buffered.player.StartAt(path, target));
        fake::run_task();
        assert(buffered.player.LastResult() == LocalMp3Player::Result::kFinished);
        assert(buffered.consumed.size() == frames * 1152 - source_start);
        assert(buffered.consumed.front() == static_cast<int16_t>(source_start));
        assert(fake::decode_calls < 64);
    }
    {
        WorkerFixture missing_target;
        missing_target.codec.rate = fake::info.sample_rate = 44100;
        fake::info.channel = 1;
        fake::empty_output_call = 33;
        assert(missing_target.player.StartAt(path, target));
        fake::run_task();
        assert(missing_target.player.LastResult() == LocalMp3Player::Result::kError);
        assert(fake::decode_calls == 33 && missing_target.consumed.empty());
    }
    for (bool cancel : {false, true}) {
        WorkerFixture controlled;
        controlled.codec.rate = fake::info.sample_rate = 44100;
        fake::info.channel = 1;
        assert(controlled.player.StartAt(path, target));
        fake::on_delay = [&] {
            if (fake::delay_calls == 1) {
                assert(fake::decode_calls == 0 && controlled.consumed.empty());
                if (cancel)
                    controlled.player.Stop();
                else
                    assert(controlled.player.Pause(true));
            } else if (fake::delay_calls == 2) {
                assert(fake::decode_calls == 0 && controlled.consumed.empty());
                assert(controlled.player.Pause(false));
            } else
                controlled.DrainOne();
        };
        fake::run_task();
        assert(controlled.player.LastResult() ==
               (cancel ? LocalMp3Player::Result::kStopped : LocalMp3Player::Result::kFinished));
        assert(fake::decode_calls < 64);
        if (cancel)
            assert(fake::decode_calls == 0 && controlled.player.DurationMs() == 0);
    }
    {
        WorkerFixture failed;
        failed.codec.rate = fake::info.sample_rate = 44100;
        fake::info.channel = 1;
        fake::decode = fake::Decode::error;
        assert(failed.player.StartAt(path, target));
        fake::run_task();
        assert(failed.player.LastResult() == LocalMp3Player::Result::kError);
        assert(!failed.player.ReadFailed());
        assert(fake::decode_calls == 1 && failed.consumed.empty());
    }
    {
        WorkerFixture read_error;
        fake::file_fault = fake::FileFault::read;
        fake::file_fault_call = 10;
        assert(read_error.player.StartAt(path, target));
        fake::run_task();
        assert(read_error.player.LastResult() == LocalMp3Player::Result::kError);
        assert(read_error.player.ReadFailed());  // While locating the resume frame.
        assert(fake::decode_calls == 0 && fake::close_calls == 0 && fake::file_close_calls == 1);
    }
    {
        WorkerFixture midstream;
        midstream.codec.rate = fake::info.sample_rate = 44100;
        fake::info.channel = 1;
        fake::decode_error_call = 35;
        assert(midstream.player.StartAt(path, target));
        fake::run_task();
        assert(midstream.player.LastResult() == LocalMp3Player::Result::kError);
        assert(fake::decode_calls == 35 && fake::close_calls == 1);
    }
    {
        WorkerFixture truncated;
        truncated.codec.rate = fake::info.sample_rate = 44100;
        fake::info.channel = 1;
        FILE* file = std::fopen(path.c_str(), "r+b");
        assert(file && std::fseek(file, 0, SEEK_END) == 0);
        assert(ftruncate(fileno(file), std::ftell(file) - 1) == 0);
        assert(std::fclose(file) == 0);
        assert(truncated.player.StartAt(path, target));
        fake::run_task();
        assert(truncated.player.LastResult() == LocalMp3Player::Result::kError);
        assert(truncated.player.DurationMs() == 0 && fake::close_calls == 1);
    }
    WriteFramedMp3(path, frames);
    {
        // ID3v1 after the final complete frame is a valid natural EOF.
        FILE* file = std::fopen(path.c_str(), "ab");
        const uint8_t tag[128] = {'T', 'A', 'G'};
        assert(file && std::fwrite(tag, 1, sizeof(tag), file) == sizeof(tag));
        assert(std::fclose(file) == 0);
        WorkerFixture trailer;
        trailer.codec.rate = fake::info.sample_rate = 44100;
        fake::info.channel = 1;
        assert(trailer.player.StartAt(path, target));
        fake::run_task();
        assert(trailer.player.LastResult() == LocalMp3Player::Result::kFinished);
        assert(trailer.player.DurationMs() == frames * 1152 * 1000 / 44100);
        assert(fake::decode_calls < 64);
    }
    for (unsigned version : {2U, 0U}) {
        constexpr size_t low_frames = 2010, low_prefix = 2000;
        const unsigned rate = version == 2 ? 22050 : 11025;
        WriteFramedMp3(path, low_frames, version);
        WorkerFixture low;
        low.codec.rate = fake::info.sample_rate = rate;
        fake::info.channel = 1;
        fake::sample_ramp = true;
        const uint64_t low_target = low_prefix * 576 * 1000 / rate + 19;
        const uint64_t start_sample = low_target * rate / 1000;
        assert(low.player.StartAt(path, low_target));
        fake::run_task();
        assert(low.player.LastResult() == LocalMp3Player::Result::kFinished);
        assert(fake::frame_mode && fake::decode_calls < 64);
        assert(low.consumed.size() == low_frames * 576 - start_sample);
        assert(low.consumed.front() == static_cast<int16_t>(start_sample));
        assert(low.player.DurationMs() == low_frames * 576 * 1000 / rate);
    }
}

uint64_t ProfileValue(const std::string& line, const std::string& key) {
    const auto begin = line.find(key + "=");
    assert(begin != std::string::npos);
    return std::stoull(line.substr(begin + key.size() + 1));
}
void TestResumeScanProfile(const std::string& path) {
    constexpr size_t prefix = 300;
    WriteFramedMp3(path, prefix + 5);
    const uint64_t target = prefix * 1152 * 1000 / 44100 + 9;
    for (int fault = 0; fault < 6; ++fault) {
        WorkerFixture fixture;
        fixture.codec.rate = fake::info.sample_rate = 44100;
        fake::info.channel = 1;
        fake_timer::enabled = true;
        fake::io_time_us = {11, 23, 37, 53};
        fake::delay_time_us = 7;
        bool pending_pause = false;
        fake::on_read = [&] {
            if (fault == 0 && fake::file_read_calls == 0) {
                assert(fixture.player.Pause(true));
                pending_pause = true;
            }
        };
        fake::on_delay = [&] {
            if (pending_pause) {
                assert(fixture.player.Pause(false));
                pending_pause = false;
            } else if (fault == 4 && fake::decode_calls == 0) {
                fixture.player.Stop();
            } else
                fixture.DrainOne();
        };
        if (fault == 1) {
            fake::file_fault = fake::FileFault::read;
            fake::file_fault_call = 1;
        }
        if (fault == 2)
            fake::seek_error_call = 1;
        if (fault == 3)
            fake::file_open_error = true;
        assert(fixture.player.StartAt(path, fault == 5 ? 100 : target));
        fake::run_task();
        assert(fake::scan_logs.size() == 7);  // Exactly one fixed group, including early exits.
        const auto& summary = fake::scan_logs.front();
        uint64_t measured = 0;
        const char* parts[] = {"open", "seek", "tell", "read", "ready", "yield"};
        for (size_t i = 0; i < 6; ++i) {
            const auto& line = fake::scan_logs[i + 1];
            assert(line.find(std::string("part=") + parts[i]) != std::string::npos);
            const auto count = ProfileValue(line, "calls"), cost = ProfileValue(line, "total_us");
            measured += cost;
            if (i < 4) {
                assert(cost == count * fake::io_time_us[i]);
                assert(ProfileValue(line, "max_us") == (count ? fake::io_time_us[i] : 0));
            }
        }
        assert(ProfileValue(summary, "total_us") == measured);
        assert(ProfileValue(summary, "accounted_us") == measured &&
               ProfileValue(summary, "other_us") == 0);
        if (fault == 0) {
            assert(fixture.player.LastResult() == LocalMp3Player::Result::kFinished);
            assert(summary.find("outcome=frame_mode") != std::string::npos);
            assert(ProfileValue(summary, "frames") == prefix + 1 &&
                   ProfileValue(summary, "read_bytes") > 0);
            // Aligned 2 KB blocks in file order: every byte up to the target frame
            // is read once. Seeks: size (2), the first block, the resume frame.
            assert(ProfileValue(fake::scan_logs[2], "calls") == 4);
            assert(ProfileValue(summary, "read_bytes") % 2048 == 10);
            assert(ProfileValue(fake::scan_logs[5], "total_us") ==
                   7);  // Pause is only inside ready.
            assert(ProfileValue(fake::scan_logs[6], "calls") == 1 &&
                   ProfileValue(fake::scan_logs[6], "total_us") == 7);
        } else if (fault == 5) {
            assert(summary.find("outcome=fallback") != std::string::npos);
        } else {
            assert(summary.find(fault == 3 ? "outcome=open_error"
                                           : "outcome=scan_error_or_cancel") != std::string::npos);
            assert(fixture.consumed.empty() && fake::decode_calls == 0);
        }
    }
    WorkerFixture no_resume;
    assert(no_resume.player.Start(path));
    fake::run_task();
    assert(fake::scan_logs.empty());
    std::puts(
        "scan profile: measured injected I/O/pause/yield separately; all exits emit one group");
}

// Restarts an existing player at `target`, checks one second of exact PCM from
// the target sample (fake decoder ramp) and returns how many frames the scan read.
uint64_t RestartAt(WorkerFixture& fixture, const std::string& path, uint64_t target) {
    const size_t logs = fake::scan_logs.size();
    fixture.consumed.clear();
    fake::delay_calls = fake::decode_calls = 0;
    fake::on_delay = [&] {
        fixture.DrainOne();
        if (fixture.consumed.size() >= 44100)
            fixture.player.Stop();
    };
    assert(fixture.player.StartAt(path, target));
    fake::run_task();
    assert(!fixture.player.Running() && fixture.consumed.size() >= 44100);
    const uint64_t start = target * 44100 / 1000;
    for (size_t i = 0; i < 44100; ++i)
        assert(fixture.consumed[i] == static_cast<int16_t>(start + i));
    assert(fake::scan_logs.size() == logs + 7);
    return ProfileValue(fake::scan_logs[logs], "frames");
}

// Frames remembered while scanning or decoding let a later start in the same
// file (the story after the AI, or a seek) scan from a nearby frame.
void TestAnchoredRestart(const std::string& path) {
    constexpr size_t frames = 34600;
    const uint64_t deep = 34500ULL * 1152 * 1000 / 44100 + 19;  // About 15 minutes in.
    WriteFramedMp3(path, frames);
    WorkerFixture fixture;
    fixture.codec.rate = fake::info.sample_rate = 44100;
    fake::info.channel = 1;
    fake::sample_ramp = true;
    assert(RestartAt(fixture, path, deep) == 34501);  // Cold: from the file head.
    // Back five seconds (after the AI) and thirty seconds (a seek), then forward
    // again: each scan starts at most 64 frames before its target frame.
    for (uint64_t target : {deep - 5000, deep - 30000, deep - 2000}) {
        const auto scanned = RestartAt(fixture, path, target);
        assert(scanned >= 33 && scanned <= 64);
    }
    // Before the remembered window (about 3.5 minutes): the full scan, still exact.
    const uint64_t early = 1000ULL * 1152 * 1000 / 44100 + 7;
    assert(RestartAt(fixture, path, early) == 1001);
    assert(RestartAt(fixture, path, early + 1000) <= 64);

    // Stream playback from the file head leaves anchors too; another file resets them.
    const std::string other = path + ".other";
    WriteFramedMp3(other, 600);
    fixture.consumed.clear();
    fake::delay_calls = fake::decode_calls = 0;
    fake::on_delay = [&] { fixture.DrainOne(); };
    assert(fixture.player.Start(other));
    fake::run_task();
    assert(fixture.player.LastResult() == LocalMp3Player::Result::kFinished && !fake::frame_mode);
    const uint64_t late = 560ULL * 1152 * 1000 / 44100 + 3;
    assert(RestartAt(fixture, other, late) <= 64);
    assert(RestartAt(fixture, path, deep - 5000) == 34310);  // Back to the first file: cold.
    assert(std::remove(other.c_str()) == 0);

    // The same path with different content: remembered offsets no longer hold
    // frame headers, so the scan falls back to the head and stays exact.
    WriteFramedMp3(path, frames, 3, 30);
    assert(RestartAt(fixture, path, deep - 5000) == 34310);
    std::puts("anchored restart: -5 s/-30 s/+28 s scans <= 64 frames; stream anchors; "
              "file change falls back to the head");
}

// F08: a .wav name plays PCM straight from the file; the MP3 decoder stays closed.
struct WavSpec {
    uint16_t tag = 1, channels = 2, bits = 16;
    uint32_t rate = 24000;
    bool extensible = false, list_first = true, data_first = false;
    uint32_t claimed_extra = 0;  // The data chunk claims more than it holds.
};
void WriteWav(const std::string& path, const WavSpec& spec, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> bytes;
    auto put = [&](uint64_t value, int width) {
        for (int i = 0; i < width; ++i)
            bytes.push_back(static_cast<uint8_t>(value >> (8 * i)));
    };
    auto tag = [&](const char* id) { bytes.insert(bytes.end(), id, id + 4); };
    auto data_chunk = [&] {
        tag("data");
        put(data.size() + spec.claimed_extra, 4);
        bytes.insert(bytes.end(), data.begin(), data.end());
    };
    tag("RIFF");
    put(0, 4);
    tag("WAVE");
    if (spec.list_first) {  // An odd-sized chunk before fmt, with its pad byte.
        tag("LIST");
        put(3, 4);
        bytes.insert(bytes.end(), {'a', 'b', 'c', 0});
    }
    if (spec.data_first)
        data_chunk();
    const uint16_t block = spec.channels * spec.bits / 8;
    tag("fmt ");
    put(spec.extensible ? 40 : 16, 4);
    put(spec.extensible ? 0xfffe : spec.tag, 2);
    put(spec.channels, 2);
    put(spec.rate, 4);
    put(spec.rate * block, 4);
    put(block, 2);
    put(spec.bits, 2);
    if (spec.extensible) {
        put(22, 2);
        put(spec.bits, 2);
        put(3, 4);
        put(spec.tag, 4);
        for (uint8_t b : {0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71})
            bytes.push_back(b);
    }
    if (!spec.data_first)
        data_chunk();
    const uint32_t riff = static_cast<uint32_t>(bytes.size() - 8);
    for (int i = 0; i < 4; ++i)
        bytes[4 + i] = static_cast<uint8_t>(riff >> (8 * i));
    FILE* file = std::fopen(path.c_str(), "wb");
    assert(file && std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size());
    assert(std::fclose(file) == 0);
}
std::vector<uint8_t> Frames(size_t count, std::initializer_list<uint8_t> frame) {
    std::vector<uint8_t> data;
    for (size_t i = 0; i < count; ++i)
        data.insert(data.end(), frame.begin(), frame.end());
    return data;
}

void TestWavPlayback(const std::string& mp3_path) {
    const std::string path = mp3_path + ".wav";
    {
        // 16-bit stereo at the output rate: 3000 frames in steps of at most 1152.
        WriteWav(path, {}, Frames(3000, {0xe8, 0x03, 0xb8, 0x0b}));  // 1000 and 3000
        WorkerFixture fixture;
        fixture.Run(path, LocalMp3Player::Result::kFinished);
        assert(fake::open_calls == 0 && fake::decode_calls == 0 && fake::converter_open_calls == 0);
        assert(fixture.consumed.size() == 3000);
        assert(std::all_of(fixture.consumed.begin(), fixture.consumed.end(),
                           [](int16_t value) { return value == 2000; }));
        assert(fixture.player.DurationMs() == 125 && fixture.player.PositionMs() == 125);
    }
    // Every supported sample format, mono, plain and extensible.
    struct Case {
        WavSpec spec;
        std::initializer_list<uint8_t> frame;
        int16_t expected;
    };
    const Case cases[] = {
        {{1, 1, 8}, {0xc0}, 16384},
        {{1, 1, 24}, {0x00, 0x34, 0x12}, 0x1234},
        {{1, 1, 32}, {0x00, 0x00, 0x34, 0x12}, 0x1234},
        {{3, 1, 32}, {0x00, 0x00, 0x00, 0x3f}, 16383},   // 0.5f
        {{3, 1, 32}, {0x00, 0x00, 0xc0, 0x7f}, -32767},  // NaN reads as the floor
        {{1, 2, 16, 24000, true}, {0x10, 0x00, 0x30, 0x00}, 0x20},
        {{1, 2, 24, 24000, false, false}, {0, 0x00, 0x10, 0, 0x00, 0x30}, 0x2000},
    };
    for (const auto& item : cases) {
        WriteWav(path, item.spec, Frames(100, item.frame));
        WorkerFixture fixture;
        fixture.Run(path, LocalMp3Player::Result::kFinished);
        assert(fixture.consumed.size() == 100 && fixture.consumed.front() == item.expected);
    }
    {
        // A start position is a byte offset: no scan, no preroll.
        WriteWav(path, {}, Frames(3000, {0xe8, 0x03, 0xb8, 0x0b}));
        WorkerFixture fixture;
        assert(fixture.player.StartAt(path, 50));
        fake::run_task();
        assert(fixture.player.LastResult() == LocalMp3Player::Result::kFinished);
        assert(fixture.consumed.size() == 1800);
        assert(fixture.player.DurationMs() == 125 && fixture.player.PositionMs() == 125);
    }
    {
        // Sought to (or past) the end: a normal finish without sound.
        WorkerFixture fixture;
        assert(fixture.player.StartAt(path, 200));
        fake::run_task();
        assert(fixture.player.LastResult() == LocalMp3Player::Result::kFinished);
        assert(fixture.consumed.empty() && fixture.player.PositionMs() == 125);
    }
    {
        // Another rate goes through the converter, like an MP3 frame.
        WriteWav(path, {1, 2, 16, 8000}, Frames(800, {0xf4, 0x01, 0xf4, 0x01}));  // 500
        WorkerFixture fixture;
        fixture.codec.rate = 48000;
        fixture.Run(path, LocalMp3Player::Result::kFinished);
        assert(fake::converter_open_calls == 1 && fake::converter_close_calls == 1);
        assert(fixture.consumed.size() == 800 * 6 && fixture.player.DurationMs() == 100);
    }
    {
        // A cut-off file plays the whole frames it holds.
        WavSpec cut;
        cut.claimed_extra = 1000;
        WriteWav(path, cut, Frames(100, {0xe8, 0x03, 0xb8, 0x0b}));
        WorkerFixture fixture;
        fixture.Run(path, LocalMp3Player::Result::kFinished);
        assert(fixture.consumed.size() == 100);
    }
    // Not playable: an error, so the runtime skips the file.
    WavSpec unsupported[6];
    unsupported[0].tag = 0x11;  // IMA ADPCM
    unsupported[1].channels = 3;
    unsupported[2].rate = 96000;
    unsupported[3].bits = 12;
    unsupported[4].data_first = true;  // data before fmt
    unsupported[5].tag = 3;            // 16-bit float does not exist
    for (const auto& spec : unsupported) {
        WriteWav(path, spec, Frames(100, {1, 0, 1, 0, 1, 0}));
        WorkerFixture fixture;
        fixture.Run(path, LocalMp3Player::Result::kError);
        assert(fixture.consumed.empty() && fake::open_calls == 0);
        assert(!fixture.player.ReadFailed());
    }
    WriteSyntheticFile(path, 1024);  // Not RIFF/WAVE at all.
    {
        WorkerFixture fixture;
        fixture.Run(path, LocalMp3Player::Result::kError);
        assert(fixture.consumed.empty() && fake::open_calls == 0);
        assert(!fixture.player.ReadFailed());
    }
    WriteWav(path, {}, Frames(3000, {0xe8, 0x03, 0xb8, 0x0b}));
    {
        // A read error mid-file ends it with an error.
        WorkerFixture fixture;
        fake::file_fault = fake::FileFault::read;
        fake::file_fault_call = 7;  // Five header reads, then the second PCM step.
        fixture.Run(path, LocalMp3Player::Result::kError);
        assert(fake::file_read_calls == 7 && fixture.player.ReadFailed());
    }
    assert(std::remove(path.c_str()) == 0);
    std::puts("WAV: PCM from the header's offset, every sample format, seek, convert, errors");
}

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string path = argv[1];
    TestServiceOwnershipAndBounds();
    TestServiceOutputAndPause();
    TestCompletedOutputPositionAndSessionIsolation();
    TestAudioTestingCannotInjectIntoLocalSession();
    TestSessionGenerationBeyond32Bits();
    TestBlockedProtocolProducerInvalidatedByReset();
    TestLocalDrainedCallbackTiming();
    TestWorkerEofDownmixAndBackpressure(path);
    TestWorkerPauseAndCancellation(path);
    TestWorkerDecodeErrors(path);
    TestWorkerMidstreamErrorsAndRestart(path);
    TestWorkerRateConversion(path);
    TestWorkerStartFailure(path);
    TestWorkerStartAtAndPosition(path);
    TestLongSeekSkipsPrefixConversion(path);
    TestFastFramedResume(path);
    TestResumeScanProfile(path);
    TestAnchoredRestart(path);
    TestSerializedCheckpointRestartAndSeek(path);
    TestWavPlayback(path);
    assert(std::remove(path.c_str()) == 0);
    std::puts("production AudioService and MP3 control checks passed (SDK decoder is fake)");
}
