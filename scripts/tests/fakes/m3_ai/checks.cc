// M3 AI variant: the full XiaoZhi AudioService (as Application initializes it)
// drives the board's local PCM path while Application stays in Starting.
int main() {
    fake::reset();
    {
        AudioService service;
        AudioCodec codec;  // 16 kHz mono in/out, like the Gen2 LocalCodec.
        service.Initialize(&codec);
        assert(fake::codec_starts == 1);
        assert(fake::opus_dec_opened == 1 && fake::opus_enc_opened == 1);
        assert(fake::resamplers_opened == 0 && fake::engines_constructed == 1);
        service.Start();
        assert(fake::tasks.size() == 3);
        assert(fake::tasks[0].name == "audio_input" && fake::tasks[1].name == "audio_output");
        assert(fake::tasks[2].name == "opus_codec");
        // LocalCodec::Write keeps a 640-byte stereo buffer on this task's stack:
        // the variant enables the audio processor to keep the 4096-byte stack.
        assert(fake::tasks[1].stack == 4096);
        // Without a network Application never enables wake word or voice
        // processing, so the microphone path stays idle.
        assert(fake::event_bits == 0 && fake::codec_reads == 0);

        const auto session = service.BeginLocalPlayback();
        assert(session != 0 && service.IsLocalPlaybackActive(session));
        // Protocol audio, notifications and prompt sounds cannot cut into the story.
        assert(!service.PushPacketToDecodeQueue(std::make_unique<AudioStreamPacket>()));
        service.PlaySound("synthetic OGG");
        assert(service.audio_decode_queue_.empty());

        std::vector<int16_t> first = {1, -2, 3};
        assert(service.SubmitLocalPcm(session, first) == LocalPcmResult::kAccepted &&
               first.empty());
        // A pause holds new PCM but keeps the session.
        assert(service.PauseLocalPlayback(session, true));
        std::vector<int16_t> held = {6};
        assert(service.SubmitLocalPcm(session, held) == LocalPcmResult::kWouldBlock);
        assert(held == std::vector<int16_t>({6}) && service.IsLocalPlaybackActive(session));
        assert(service.PauseLocalPlayback(session, false));
        std::vector<int16_t> second = {4, 5};
        assert(service.SubmitLocalPcm(session, second) == LocalPcmResult::kAccepted);

        codec.output = false;
        int writes = 0;
        codec.on_output = [&]() {
            if (++writes == 2)
                service.Stop();
        };
        fake::tasks[1].entry(fake::tasks[1].argument);
        assert(codec.output);  // The output task powered the speaker path itself.
        assert(codec.writes == std::vector<std::vector<int16_t>>({{1, -2, 3}, {4, 5}}));
        assert(service.LocalPlaybackSamples(session) == 5);
    }
    fake::reset();
    {
        AudioService service;
        AudioCodec codec;
        service.Initialize(&codec);
        service.Start();
        const auto session = service.BeginLocalPlayback();
        assert(session != 0);
        // A long pause lets the shared power timer turn the speaker path off
        // without ending the story session...
        codec.output = true;
        assert(service.PauseLocalPlayback(session, true));
        service.last_output_time_ = std::chrono::steady_clock::now() -
                                    std::chrono::milliseconds(AUDIO_POWER_TIMEOUT_MS + 1);
        service.CheckAndUpdateAudioPowerState();
        assert(!codec.output && service.IsLocalPlaybackActive(session));
        // ...while recent output keeps it on.
        codec.output = true;
        service.last_output_time_ = std::chrono::steady_clock::now();
        service.CheckAndUpdateAudioPowerState();
        assert(codec.output);
        // Ending the story returns the output to protocol audio for later batches.
        service.EndLocalPlayback(session);
        assert(!service.IsLocalPlaybackActive(session));
        std::vector<int16_t> stale = {1};
        assert(service.SubmitLocalPcm(session, stale) == LocalPcmResult::kStale);
        assert(service.PushPacketToDecodeQueue(std::make_unique<AudioStreamPacket>()));
        service.Stop();
    }
}
