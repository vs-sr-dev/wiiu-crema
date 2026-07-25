// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan
//
// A bank of instruments — the other half of the idea in crema_audio.h.
//
// AX is a sampler, and a sampler with pitch and loop control is a sound chip:
// a pulse wave is one cycle in a loop with the duty already baked in, a NES
// triangle is its sixteen steps written out exactly, noise is an LFSR sequence
// generated once. All of that happens offline (tools/gen_waves.py), because a
// waveform the console will never vary at runtime is work done every boot for
// nothing. What arrives here is PCM and two numbers per instrument.
//
// The second of those numbers is what makes it an instrument rather than a
// sound: `cycleSamples`. A voice playing a cycle of N samples at rate R sounds
// at R/N Hz, so a note is a playback rate and nothing else —
//
//     ratio = frequency * cycleSamples / rate
//
// which is CremaInstrumentPitch below, and is the entire theory of playing
// music on this hardware. Zero means the instrument is not pitched (a drum, an
// engine loop): drive its rate yourself.
//
// Ownership matters here in a way it does not for a mesh. A texture can be
// uploaded and the source bytes dropped; an instrument's samples are read by
// the DSP *while it plays*, so the bank keeps its own cache-flushed copy and
// must outlive every voice that touches it.

#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "crema_audio.h"

#ifdef __cplusplus
extern "C" {
#endif

// What a note does over its own lifetime — and this, not reverb, is what
// separates an instrument from a beep. Without it every note is a rectangle:
// full volume the instant it starts, full volume until it stops, silence. No
// real sound has ever done that, which is why a tune made of rectangles sounds
// like a test tone playing a melody.
//
// Times are milliseconds, sustain is a fraction in thousandths of the note's
// own volume. All zeros mean the rectangle, deliberately: an instrument that
// says nothing about its shape gets the behaviour it had before this existed.
typedef struct {
    uint16_t attackMs;    // silence to full
    uint16_t decayMs;     // full down to the sustain level
    uint16_t sustain;     // 1000 = hold at full, 0 = a percussive hit
    uint16_t releaseMs;   // note-off to silence — the part that stops the click
} CremaEnvelope;

// Vibrato, which is the other half of the same idea applied to pitch. It waits
// before it starts because that is what a player does: you decide to hold a
// note first, and only then start moving it. Applied on top of the note's own
// detune, so a chip's out-of-tune-ness survives underneath it.
typedef struct {
    uint16_t delayMs;
    uint16_t rateMilliHz; // 5500 = 5.5 Hz
    int16_t  depthCents;  // peak deviation; 0 disables the whole thing
} CremaVibrato;

typedef struct {
    char       name[24];
    CremaSound sound;          // owns DSP-visible, cache-flushed PCM
    uint32_t   cycleSamples;   // 0 = not pitched
    CremaEnvelope env;
    CremaVibrato  vib;
} CremaInstrument;

typedef struct {
    CremaInstrument *items;
    uint32_t         count;
    uint32_t         rate;
} CremaBank;

// From bytes already in memory — a .cbank entry inside a .cpak, normally. The
// blob may be freed as soon as this returns: every instrument has been copied
// into memory of its own.
bool CremaBankLoadFromMemory(CremaBank *bank, const void *blob, size_t size);

const CremaInstrument *CremaBankFind(const CremaBank *bank, const char *name);

// Frees every instrument's samples. Nothing may be playing from this bank.
void CremaBankClose(CremaBank *bank);

// The pitch to hand CremaAudioPlay / CremaAudioHold for a given note, where 69
// is A4 = 440 Hz (the MIDI numbering, because everything else already uses it).
// Fractional notes are allowed and are how you bend, slide and detune.
float CremaInstrumentPitch(const CremaInstrument *inst, float note);

#ifdef __cplusplus
}
#endif
