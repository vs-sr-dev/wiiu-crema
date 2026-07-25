// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan
//
// The waveforms poc11 plays, generated in plain C.
//
// They live here rather than inside Crema for the usual rule of this repo: the
// framework knows how to hand a buffer to the DSP, this example knows what a
// laser sounds like. When a second example wants the same oscillators, that is
// when they move.
//
// They live in a *header with no console dependency* for a different reason.
// Nothing below knows what a Wii U is: it is arithmetic that fills an int16
// buffer. So the same source that plays on the DSP compiles on a PC and writes
// a WAV — which is how these were checked (tools/render_sounds.c). An offline
// preview that re-implements the sound is a preview of a different sound, and
// on this project we have already learned once what happens when the thing you
// test and the thing you ship are two pieces of code.
//
// Everything is baked at 32 kHz, not the renderer's 48: half the memory, and it
// puts the resampler on the critical path from the first frame. If the pitch is
// wrong we hear it immediately instead of discovering it the day we need it.

#pragma once
#include <math.h>
#include <stdint.h>

#define SND_RATE      32000
#define SND_MAX_SAMPS SND_RATE     // one second: the longest of them fits

static int16_t clip16(float v)
{
    if (v >  1.0f) v =  1.0f;
    if (v < -1.0f) v = -1.0f;
    return (int16_t)(v * 32000.0f);
}

// The NES noise channel in three lines: xor two taps, shift the bit back in.
// It is not white noise, and that is exactly why chip explosions sound like
// chip explosions.
static uint16_t noiseReg = 0x7FFF;
static float noiseNext(void)
{
    uint16_t bit = (noiseReg ^ (noiseReg >> 1)) & 1u;
    noiseReg = (uint16_t)((noiseReg >> 1) | (bit << 14));
    return (noiseReg & 1u) ? 1.0f : -1.0f;
}

// A descending pulse: the oldest laser sound there is. The sweep is what sells
// it — a fixed-pitch beep reads as a menu, the same beep falling reads as a
// shot leaving the ship.
static uint32_t bakeLaser(int16_t *dst)
{
    uint32_t n = SND_RATE * 22 / 100;             // 0.22 s
    float phase = 0.0f;
    for (uint32_t i = 0; i < n; i++) {
        float t = (float)i / (float)SND_RATE;
        float u = (float)i / (float)n;
        float freq = 1500.0f * powf(0.18f, u);    // two and a half octaves down
        phase += freq / (float)SND_RATE;
        phase -= floorf(phase);
        float pulse = phase < 0.30f ? 1.0f : -1.0f;   // 30% duty: thin, bright
        dst[i] = clip16(pulse * expf(-7.0f * t) * 0.75f);
    }
    return n;
}

// An explosion is noise that gets darker and slower as it dies, over a thump
// that falls in pitch. The low-pass is one line and it is the whole difference
// between a bang and a hiss.
static uint32_t bakeBoom(int16_t *dst)
{
    uint32_t n = SND_RATE * 9 / 10;               // 0.9 s
    float lp = 0.0f, phase = 0.0f, sample = 0.0f;
    uint32_t hold = 0;
    for (uint32_t i = 0; i < n; i++) {
        float t = (float)i / (float)SND_RATE;
        float u = (float)i / (float)n;
        // holding each noise value for a few samples lowers its pitch: the
        // chip trick for turning a hiss into rubble
        if (hold == 0) {
            sample = noiseNext();
            hold = 2 + (uint32_t)(u * 8.0f);
        } else {
            hold--;
        }
        float cutoff = 0.55f * expf(-2.2f * t) + 0.03f;
        lp += (sample - lp) * cutoff;
        float thump = sinf(phase * 6.2831853f) * expf(-9.0f * t);
        phase += (70.0f * powf(0.45f, u)) / (float)SND_RATE;
        phase -= floorf(phase);
        // The attack of noise plus thump goes over full scale, and the renderer
        // counted it: 550 samples sitting flat on the ceiling. Saturate instead
        // of clamping — an explosion is *meant* to sound squashed, and a curve
        // does it without the fizz a flat top makes. Offline, so tanh is free.
        float mix = lp * expf(-3.2f * t) * 0.85f + thump * 0.55f;
        dst[i] = clip16(tanhf(mix) * 0.98f);
    }
    return n;
}

// The engine: 0.2 s holding exactly eleven cycles of 55 Hz, so every harmonic
// and the slow wobble are whole multiples of the loop. A loop that does not
// close on itself clicks once per period, forever — and at 5 Hz you hear it.
#define ENGINE_CYCLES 11
static uint32_t bakeEngine(int16_t *dst)
{
    uint32_t n = SND_RATE / 5;
    for (uint32_t i = 0; i < n; i++) {
        float ph = (float)i / (float)n;           // 0..1 across the whole loop
        float w = ph * (float)ENGINE_CYCLES * 6.2831853f;
        float v = sinf(w)        * 0.55f + sinf(w * 2.0f) * 0.28f
                + sinf(w * 3.0f) * 0.16f + sinf(w * 5.0f) * 0.09f;
        float wobble = 0.85f + 0.15f * sinf(ph * 6.2831853f);
        dst[i] = clip16(v * wobble * 0.6f);
    }
    return n;
}
