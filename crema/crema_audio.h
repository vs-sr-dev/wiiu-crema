// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan
//
// Sound through AX — and the first thing to understand about the Wii U is that
// AX is not a file player: it is a hardware sampler. You hand it PCM sitting in
// memory, it plays it at a pitch you choose, loops it where you tell it to, and
// mixes it into the TV, the GamePad, or both. There is no decoder, no music
// format, no synthesiser. There is a bank of voices and a resampler.
//
// Which is why this module is so small. A sound is a buffer the DSP can read; a
// voice is that buffer playing at some rate and volume. Everything a game calls
// "audio" is built on top of those two things.
//
// Two ways to play, because ownership differs:
//
//   CremaAudioPlay   — fire and forget. The shot, the explosion. It ends, and
//                      CremaAudioUpdate gives the voice back. You keep nothing.
//   CremaAudioHold   — you own it. The engine hum, a music loop. It plays until
//                      you release it, and you can retune it every frame.
//
// The sample data must be flushed out of the CPU cache before the DSP reads it
// — the same law as lesson 3 for the GPU, a different piece of silicon reading
// memory the CPU still holds dirty. CremaSoundCreate does it for you.

#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// AXInit + voice pool. Safe to call when AX is already up.
//
// Call it early — before loading assets, not after. Until a title takes audio
// over, the Wii U Menu's music keeps playing underneath it (the system hands it
// to you as transition audio and waits for you to end it), so every second you
// spend loading before this call is a second of somebody else's soundtrack.
bool CremaAudioInit(void);
void CremaAudioShutdown(void);

// --- sounds ------------------------------------------------------------------

// 16-bit mono PCM in memory the DSP can read. Mono because an AX voice is mono:
// stereo is two voices, and a game with a listener would rather pan one.
typedef struct {
    int16_t *samples;     // DSP-visible, cache-flushed, owned by the sound
    uint32_t count;       // samples, not bytes
    uint32_t rate;        // the rate the data was written for
    uint32_t loopStart;   // sample to jump back to; ignored unless looping
    bool     looping;
} CremaSound;

// Copies `count` samples into a fresh DSP-visible buffer and flushes it.
bool CremaSoundCreate(CremaSound *snd, const int16_t *pcm, uint32_t count,
                      uint32_t rate);

// Same, but the voice wraps to `loopStart` instead of ending. The buffer must
// join back onto itself without a step, or you will hear the seam once per loop.
bool CremaSoundCreateLooping(CremaSound *snd, const int16_t *pcm, uint32_t count,
                             uint32_t rate, uint32_t loopStart);

void CremaSoundDestroy(CremaSound *snd);

// --- playing -----------------------------------------------------------------

// pitch is a ratio: 1.0 plays at the rate the sound was baked at, 2.0 an octave
// up. volume is 0..1. Returns false only when every voice is busy — which for a
// one-shot is not an error worth propagating, so most callers ignore it.
bool CremaAudioPlay(const CremaSound *snd, float volume, float pitch);

// A voice you own. NULL when the pool is full.
typedef struct CremaAudioVoice CremaAudioVoice;

CremaAudioVoice *CremaAudioHold(const CremaSound *snd, float volume, float pitch);
void CremaAudioVoiceSet(CremaAudioVoice *voice, float volume, float pitch);
void CremaAudioRelease(CremaAudioVoice *voice);

// Once per frame: hands back the voices whose one-shots have finished. Without
// it the pool fills up after a few dozen shots and the game goes quiet.
void CremaAudioUpdate(void);

uint32_t CremaAudioVoicesInUse(void);

#ifdef __cplusplus
}
#endif
