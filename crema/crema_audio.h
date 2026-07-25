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

// A held voice with nothing in it yet — for a caller that will aim it later.
//
// This exists for the sequencer, and the reason is the oldest rule in real-time
// audio: **never allocate on the audio thread.** A music channel reserves its
// voice once, on the game thread, and from then on a note-on is not an
// acquisition but a re-aiming — new samples, new pitch, play from the top.
// Nothing is taken or given back while the song runs, so nothing on the audio
// side ever has to agree with the game side about who owns what.
CremaAudioVoice *CremaAudioReserve(void);

// Point a voice at a sound and (re)start it from the beginning.
void CremaAudioVoiceRetrigger(CremaAudioVoice *voice, const CremaSound *snd,
                              float volume, float pitch);

// Note off: stop it, but keep the voice — the channel still owns it.
void CremaAudioVoiceSilence(CremaAudioVoice *voice);

// Volume and pitch for the next `samples` of this voice, as a ramp the DSP
// walks by itself.
//
// This exists because a sequencer thinks 333 times a second and the ear hears
// 48000. Writing a volume once per audio frame and leaving it there turns every
// fade into a staircase of 3 ms steps, and a staircase in a volume is a buzz.
// An AX voice carries a volume *and* a per-sample delta, so the hardware
// interpolates between the two numbers you hand it: the sequencer says "from
// here to there, over this frame", and the envelope comes out smooth at the
// sample rate for the price of one extra field.
//
// Safe from the audio thread — it is four writes to voice state and no more.
void CremaAudioVoiceRamp(CremaAudioVoice *voice, float from, float to,
                         uint32_t samples, float pitch);

// --- aux sends ---------------------------------------------------------------

// AX gives every voice three sends besides the main mix. What is on the other
// end of one is whatever you register with AXRegisterAuxCallback: your own code,
// in the signal path, three milliseconds of it at a time. That is the only
// programmable point in this pipeline — there is no effect library in wut, so
// an echo or a reverb is something you write, not something you switch on.
//
// The send is global rather than per voice, because that is what it models. An
// aux bus with a room on it is a room, and a room does not reverberate one
// sound and not another. 0 is dry.
#define CREMA_AUDIO_AUX_BUSES 3

void  CremaAudioSetAuxSend(uint32_t bus, float level);
float CremaAudioGetAuxSend(uint32_t bus);

// --- headroom ----------------------------------------------------------------

// How loud the whole mix is allowed to be, 0..1, applied to every voice's main
// bus and to its sends alike.
//
// It exists because nothing else in this pipeline keeps count. Each voice is
// mixed at the volume it was given and the buses simply add, so four music
// channels, an engine and a laser arriving in the same millisecond are a sum of
// six perfectly reasonable numbers — and the sum clips, which is heard exactly
// when the music gets busy and the player starts shooting, i.e. at the worst
// possible moment. Headroom is the answer to "how many loud things at once",
// and applying it uniformly means turning it down costs volume, never balance.
//
// What it should be set to is not a matter of taste but of measurement: send
// every voice to an aux bus at 1.0 and the effect on that bus sees precisely
// the sum the main bus sees.
void  CremaAudioSetHeadroom(float headroom);
float CremaAudioGetHeadroom(void);

// Once per frame: hands back the voices whose one-shots have finished. Without
// it the pool fills up after a few dozen shots and the game goes quiet.
void CremaAudioUpdate(void);

uint32_t CremaAudioVoicesInUse(void);

#ifdef __cplusplus
}
#endif
