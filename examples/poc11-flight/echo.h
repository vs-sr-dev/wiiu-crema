// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan
//
// A delay line with feedback — an echo — in plain C and nothing else.
//
// This file includes no console header on purpose, and that purpose is the
// whole reason the effect lives in a file of its own. Everything else in this
// project that was "portable" turned out not to be worth it: a waveform the
// console never varies is better generated offline in Python, and the one
// attempt at sharing generator code with the PC became ballast. An effect is
// the opposite case. It is real DSP, it is fiddly, it is judged by ear, and the
// ear is attached to a person sitting in front of a television — so the same
// arithmetic has to be auditionable on a PC where a wrong number is a rerun,
// and then run unchanged in the audio callback where a wrong number is a
// reboot.
//
// That PC end is `tools/fx_render.c`, and it earned itself on the first run: see
// `dryMask` for what it found, which had been costing this effect three quarters
// of its output and the mix a quarter of its headroom since the day it was
// written. Neither the console nor the emulator had said a word about it.
//
// The signal is planar: one buffer per channel, `channels` of them, and the
// processing is IN PLACE because that is the contract the hardware imposes —
// AX hands the aux callback the same memory it will read back afterwards.
//
// The samples are int32 and their scale is not assumed anywhere: every path
// through here is linear, so a delay does the same thing to numbers in ±128 as
// to numbers in ±32767. That matters, because what scale AX uses in an aux
// buffer is undocumented — `peak` is how it was found (32767, measured on the
// console) and how it stays honest if it is ever different somewhere else.
//
// The arithmetic is fixed point rather than floating, and that is a measurement
// too, not a preference. See `feedbackQ`.

#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define ECHO_MAX_CHANNELS 6

typedef struct {
    int32_t *buffer;        // channels * length, planar; caller owns it
    uint32_t length;        // samples per channel in the delay line
    uint32_t channels;
    uint32_t cursor;        // where the next sample is written

    uint32_t delaySamples;  // how far back the tap is
    float    feedback;      // how much of the tap goes back in: < 1 or it grows
    float    wet;           // how much of the tap comes out

    // Whether the output carries the input, and it is not a taste setting: it
    // is a statement about where in the mixer this effect is standing, and
    // getting it wrong costs headroom for nothing.
    //
    // On an **aux bus** the mixer sends the effect `send * dry` and adds what
    // comes back to a main bus that is *already carrying the dry at full level*.
    // An effect that passes its input through therefore puts a second copy of
    // the dry into the mix at the send level — 30% louder for no more echo. The
    // PC tool made this a table instead of a suspicion: with a 20000 click,
    // send 0.30 and wet 0.45, the aux placement peaked at 26000 with a first
    // repeat of 2700, while the same numbers as an insert peaked at 20000 with a
    // repeat of 9000. Same effect, same settings, and the aux path was spending
    // its whole send budget on volume.
    //
    // On an **insert** (a device final-mix callback) the effect has been given
    // the mix itself, so dropping the dry would drop everything.
    //
    // Stored as a mask rather than a flag because this is read once per sample
    // in a loop that costs 12.5 µs of a 3000 µs frame: `in & -1` is `in`,
    // `in & 0` is silence, and an AND is a cycle where a multiply would be five.
    int32_t  dryMask;

    // The same two gains in 12-bit fixed point, which is what the loop actually
    // uses. Not premature: this CPU has no instruction that turns an integer
    // into a float, so a compiler writes one as a store and a load through
    // memory, and the same again coming back. Three of those per sample was
    // most of the cost of the entire effect — measured, 18 µs a call against
    // the 3000 µs frame, before this. A gain in Q12 is a multiply and a shift.
    //
    // Q12 and not Q16 so the product stays inside 32 bits without help: a tap
    // would have to reach 524288 to overflow, and these run in the thousands.
    int32_t  feedbackQ, wetQ;

    // What the effect has seen. The peak is not a diagnostic afterthought: the
    // numeric range of an AX aux buffer is undocumented, Cemu's mixer stores it
    // shifted right by eight with a comment saying it is not sure why, and the
    // only way to settle it is to look at the real thing on the real machine.
    uint32_t calls;
    uint32_t lastChannels;
    uint32_t lastSamples;
    int32_t  peakIn;
    int32_t  peakOut;
} Echo;

// `buffer` must hold channels * length int32s and outlive the effect.
//
// `wetOnly` has no sensible default, which is why it is a parameter and not a
// setter with one: see `dryMask`. True on an aux send, false on an insert.
static inline void echoInit(Echo *e, int32_t *buffer, uint32_t length,
                            uint32_t channels, uint32_t delaySamples,
                            float feedback, float wet, bool wetOnly)
{
    memset(e, 0, sizeof(*e));
    e->dryMask  = wetOnly ? 0 : -1;
    e->buffer   = buffer;
    e->length   = length;
    e->channels = channels;
    if (delaySamples >= length)
        delaySamples = length - 1;
    e->delaySamples = delaySamples;
    e->feedback  = feedback;
    e->wet       = wet;
    e->feedbackQ = (int32_t)(feedback * 4096.0f);
    e->wetQ      = (int32_t)(wet * 4096.0f);
    memset(buffer, 0, (size_t)length * channels * sizeof(int32_t));
}

// Silence the tail without disturbing the settings — for a bypass that must not
// spit out three seconds of stale room when it is switched back on.
static inline void echoClear(Echo *e)
{
    if (e->buffer)
        memset(e->buffer, 0,
               (size_t)e->length * e->channels * sizeof(int32_t));
}

// One block, in place. `data[c]` is `samples` long; there are `channels` of
// them, and `channels` may be fewer or more than the effect was built for —
// the caller does not get to choose how many channels a device has, so the
// extra ones are left alone rather than trusted.
static inline void echoProcess(Echo *e, int32_t *const *data,
                               uint32_t channels, uint32_t samples)
{
    e->calls++;
    e->lastChannels = channels;
    e->lastSamples  = samples;

    uint32_t n = channels < e->channels ? channels : e->channels;
    if (!e->buffer || n == 0 || samples == 0)
        return;

    uint32_t start = e->cursor;
    uint32_t startRead = start >= e->delaySamples
                       ? start - e->delaySamples
                       : start + e->length - e->delaySamples;
    uint32_t write = start;

    // Channel outside, samples inside — the other way round is the obvious way
    // to write it and the wrong one on this machine. Walking one channel's
    // delay line end to end reads memory in a straight line; alternating
    // between two lines 38 KB apart on every sample asks the cache to hold two
    // moving windows instead of one. Every channel starts from the same cursor,
    // so they still describe one room and not two.
    for (uint32_t c = 0; c < n; c++) {
        int32_t *line = e->buffer + (size_t)c * e->length;
        int32_t *ch   = data[c];
        uint32_t r = startRead, w = start;

        for (uint32_t i = 0; i < samples; i++) {
            int32_t in  = ch[i];
            int32_t tap = line[r];

            int32_t out = (in & e->dryMask) + ((tap * e->wetQ) >> 12);
            line[w] = in + ((tap * e->feedbackQ) >> 12);
            ch[i]   = out;

            int32_t a = in  < 0 ? -in  : in;
            int32_t b = out < 0 ? -out : out;
            if (a > e->peakIn)  e->peakIn  = a;
            if (b > e->peakOut) e->peakOut = b;

            // Not `(i + 1) % length`. A modulo is an integer division, and on
            // this CPU `divwu` is about twenty cycles and does not pipeline —
            // two of them per sample is more arithmetic than the entire effect.
            // An index that only ever advances by one wraps at most once, and a
            // comparison says so in a cycle.
            if (++r >= e->length) r = 0;
            if (++w >= e->length) w = 0;
        }
        write = w;
    }
    e->cursor = write;
}
