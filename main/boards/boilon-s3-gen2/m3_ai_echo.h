#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// P5-3: echo cancellation, so the child can interrupt 喵喵 while she speaks.
// The ES7210 slot that carries the playback reference is found by a silent
// self-check at boot, never assumed from another board (M1_AUDIO_CHANNEL_BASIS).
// Barge-in itself is off until the user asks for it, and the choice is saved.
// No ESP-IDF headers: host-tested by test_m3_ai_echo.
namespace boilon::ai::echo {
// The self-check plays and reads 10 ms blocks of 16 kHz frames, the codec's
// DMA block; each received frame holds the four TDM slots.
constexpr int kSlots = 4;
constexpr int kBlockFrames = 160;
constexpr int kProbeBlocks = 36;
// Stale receive buffers and the ADC's power-up transient come first.
constexpr int kSkipBlocks = 6;
// ES7210 TDM order is MIC1, MIC3, MIC2, MIC4; gain masks use the MIC number.
constexpr int kSlotInput[kSlots] = {0, 2, 1, 3};
constexpr uint16_t InputMask(int slot) { return 1u << kSlotInput[slot]; }

// A 1 kHz tone at -20 dBFS in two 60 ms bursts after 80 ms of silence. The
// amplifier is off, so the speaker stays silent and the microphones hear
// nothing; only an input wired back from the DAC follows the bursts.
constexpr float kToneDbfs = -20;
constexpr bool ToneOn(int block) {
    return (block >= 8 && block < 14) || (block >= 20 && block < 26);
}
// One period is 16 samples, so a block holds exactly ten.
inline int16_t ToneSample(int n) {
    static constexpr int16_t kPeriod[16] = {0, 1254,  2317,  3028,  3277,  3028,  2317,  1254,
                                            0, -1254, -2317, -3028, -3277, -3028, -2317, -1254};
    return kPeriod[n % 16];
}

// The 1 kHz amplitude in one slot of a block, in dB relative to a full-scale
// sine. Ten whole periods make this one exact DFT bin (Goertzel).
inline float ToneDb(const int16_t* tdm, int slot, int frames = kBlockFrames) {
    constexpr float kCoefficient = 2 * 0.92387953f;  // 2 cos(2 pi / 16)
    float s1 = 0, s2 = 0;
    for (int i = 0; i < frames; ++i) {
        const float s0 = tdm[i * kSlots + slot] + kCoefficient * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const float power = std::max(s1 * s1 + s2 * s2 - kCoefficient * s1 * s2, 0.0f);
    const float amplitude = 2 * std::sqrt(power) / frames;
    return 20 * std::log10(std::max(amplitude, 0.1f) / 32767);
}

// One slot's level during the bursts and during the silence. The receive side
// lags by up to eight blocks, so rank instead of aligning: the sixth loudest
// block lies inside a burst and the sixth quietest in silence.
struct SlotLevel {
    float on = -110, off = -110;
};
inline SlotLevel Levels(const float (&blocks)[kProbeBlocks]) {
    constexpr int kCount = kProbeBlocks - kSkipBlocks;
    float sorted[kCount];
    std::copy(blocks + kSkipBlocks, blocks + kProbeBlocks, sorted);
    std::sort(sorted, sorted + kCount);
    return {sorted[kCount - 6], sorted[5]};
}

// The reference must follow the bursts by 30 dB and not be buried in noise.
constexpr float kMinContrastDb = 30;
constexpr float kMinLevelDb = -60;
// Slots 1 and 3 (MIC3, MIC4) never answered a voice (P1-5); exactly one of
// them must follow the tone, or there is no reference. -1 when none.
inline int PickReference(const SlotLevel (&levels)[kSlots]) {
    int found = -1;
    for (int slot : {1, 3}) {
        if (levels[slot].on < kMinLevelDb || levels[slot].on - levels[slot].off < kMinContrastDb)
            continue;
        if (found >= 0)
            return -1;
        found = slot;
    }
    return found;
}

// The self-check played -20 dBFS at 0 dB gain. Choose the ES7210 gain (3 dB
// steps, at most 30 dB) that keeps full-scale playback at or below -6 dBFS.
inline int ReferenceGainDb(float tone_db) {
    const float wanted = -6 - (tone_db - kToneDbfs);
    return std::clamp(static_cast<int>(std::floor(wanted / 3)) * 3, 0, 30);
}

// Slot 0 (the voice) as mono, or followed by the reference slot as pairs.
inline void TakeChannels(const int16_t* tdm, int frames, int reference_slot, int16_t* out) {
    for (int i = 0; i < frames; ++i) {
        if (reference_slot < 0) {
            out[i] = tdm[i * kSlots];
        } else {
            out[2 * i] = tdm[i * kSlots];
            out[2 * i + 1] = tdm[i * kSlots + reference_slot];
        }
    }
}

// The switch: off unless the user asked; one byte in NVS bl_ai/barge_in.
constexpr bool kDefaultBargeIn = false;
constexpr char kNamespace[] = "bl_ai";
constexpr char kKey[] = "barge_in";
// 1 is on and 0 off; nothing saved, or any other byte, is the default.
constexpr bool Decode(bool stored, uint8_t value) {
    return stored && value <= 1 ? value == 1 : kDefaultBargeIn;
}
// Upstream's SetAecMode() closes an open voice channel, so switch only while
// idle, and only when the setting in effect differs.
constexpr bool ShouldSwitch(bool idle, bool reference, bool wanted, bool aec_on) {
    return idle && aec_on != (reference && wanted);
}
}  // namespace boilon::ai::echo
