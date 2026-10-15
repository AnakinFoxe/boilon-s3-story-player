
static void no_capture_allocations() {
    assert(fake::opus_dec_opened == 0 && fake::opus_enc_opened == 0);
    assert(fake::resamplers_opened == 0 && fake::engines_constructed == 0);
    assert(fake::input_runs == 0 && fake::opus_runs == 0);
    assert(fake::codec_reads == 0);
}

int main() {
    fake::reset();
    {
        AudioService service;
        assert(!service.InitializePlaybackOnly(nullptr));
        assert(fake::codec_starts == 0);
    }
    fake::reset();
    for (int invalid = 0; invalid < 3; ++invalid) {
        AudioService service;
        AudioCodec codec;
        codec.rate = invalid == 0 ? 7999 : invalid == 1 ? 48001 : 16000;
        codec.channels = invalid == 2 ? 2 : 1;
        assert(!service.InitializePlaybackOnly(&codec));
        assert(fake::codec_starts == 0 && fake::timers_created == 0);
    }
    fake::reset();
    {
        AudioService service;
        AudioCodec codec;
        assert(service.InitializePlaybackOnly(&codec));
        assert(fake::codec_starts == 1 && fake::timers_created == 1);
        assert(service.BeginLocalPlayback() == 0);
        assert(!service.InitializePlaybackOnly(&codec));
        service.Initialize(&codec);  // No mode replacement after playback-only init.
        assert(fake::codec_starts == 1);
        no_capture_allocations();
        service.Start();
        assert(fake::tasks.size() == 1 && fake::tasks[0].name == "audio_output");
        service.Start();
        assert(fake::tasks.size() == 1);  // No duplicate output task.
        const auto timer_starts = fake::timers_started;
        service.PlaySound("synthetic unsupported OGG");
        assert(fake::timers_started == timer_starts && !codec.output);
        assert(fake::demuxers_constructed == 0);
        assert(!service.PushPacketToDecodeQueue(std::make_unique<AudioStreamPacket>()));
        service.EnableAudioTesting(true);
        service.EnableAudioTesting(false);
        assert(fake::event_bits == 0 && service.audio_decode_queue_.empty());
        std::vector<int16_t> microphone = {7};
        assert(!service.ReadAudioData(microphone, 16000, 160));
        assert(microphone == std::vector<int16_t>({7}));
        no_capture_allocations();
        auto session = service.BeginLocalPlayback();
        assert(session != 0);
        std::vector<int16_t> pcm = {1, -2, 3};
        assert(service.SubmitLocalPcm(session, pcm) == LocalPcmResult::kAccepted && pcm.empty());
        codec.on_output = [&]() { service.Stop(); };
        fake::tasks[0].entry(fake::tasks[0].argument);
        assert(codec.writes == std::vector<std::vector<int16_t>>({{1, -2, 3}}));
        assert(!service.IsLocalPlaybackActive(session));
        pcm = {4};
        assert(service.SubmitLocalPcm(session, pcm) == LocalPcmResult::kStale);
        assert(service.BeginLocalPlayback() == 0);
        service.Start();
        assert(fake::task_attempts == 1);  // Stop is terminal.
    }
    for (int failure = 0; failure < 4; ++failure) {
        fake::reset();
        fake::event_error = failure == 0;
        fake::timer_error = failure == 1;
        fake::timer_start_error = failure == 2;
        fake::task_error = failure == 3;
        AudioService service;
        AudioCodec codec;
        const bool initialized = service.InitializePlaybackOnly(&codec);
        assert(initialized == (failure >= 2));
        if (failure != 0) service.Start();
        assert(service.BeginLocalPlayback() == 0);
        assert(service.service_stopped_.load());
        assert(fake::tasks.empty());
        const auto attempts = fake::task_attempts;
        const auto starts = fake::timers_started;
        if (failure != 0) service.Start();
        assert(fake::task_attempts == attempts && fake::timers_started == starts);
        no_capture_allocations();
        service.Stop();
    }
    fake::reset();
    {
        AudioService service;
        AudioCodec codec;
        codec.input_rate = 24000;
        service.Initialize(&codec);
        assert(fake::codec_starts == 1);
        assert(fake::opus_dec_opened == 1 && fake::opus_enc_opened == 1);
        assert(fake::resamplers_opened == 1 && fake::engines_constructed == 1);
        assert(!service.InitializePlaybackOnly(&codec));
        service.Start();
        assert(fake::tasks.size() == 3);
        assert(fake::tasks[0].name == "audio_input");
        assert(fake::tasks[1].name == "audio_output");
        assert(fake::tasks[2].name == "opus_codec");
        assert(service.PushPacketToDecodeQueue(std::make_unique<AudioStreamPacket>()));
        std::vector<int16_t> microphone;
        assert(service.ReadAudioData(microphone, 24000, 160));
        assert(fake::codec_reads == 1 && microphone.size() == 160);
        service.EnableAudioTesting(true);
        assert(fake::event_bits & AS_EVENT_AUDIO_TESTING_RUNNING);
        service.EnableAudioTesting(false);
        service.Stop();
        service.Start();  // Existing ordinary-mode restart remains available.
        assert(fake::tasks.size() == 6 && fake::event_bits == 0);
        assert(fake::tasks[3].name == "audio_input" && fake::tasks[4].name == "audio_output");
        assert(fake::tasks[5].name == "opus_codec");
        service.Stop();
    }
    fake::reset();
    {
        AudioService service;
        AudioCodec codec;
        assert(service.InitializePlaybackOnly(&codec));
        service.Stop();
        service.Start();
        assert(fake::task_attempts == 0 && service.BeginLocalPlayback() == 0);
    }
}
