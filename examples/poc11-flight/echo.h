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
// The signal is planar: one buffer per channel, `channels` of them, and the
// processing is IN PLACE because that is the contract the hardware imposes —
// AX hands the aux callback the same memory it will read back afterwards.
//
// The samples are int32 and their scale is not assumed anywhere: every path
// through here is linear, so a delay does the same thing to numbers in ±128 as
// to numbers in ±32767. That matters, because what scale AX actually uses in
// an aux buffer is exactly the thing we do not know yet — hence `peak`.

#pragma once
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
static inline void echoInit(Echo *e, int32_t *buffer, uint32_t length,
                            uint32_t channels, uint32_t delaySamples,
                            float feedback, float wet)
{
    memset(e, 0, sizeof(*e));
    e->buffer   = buffer;
    e->length   = length;
    e->channels = channels;
    if (delaySamples >= length)
        delaySamples = length - 1;
    e->delaySamples = delaySamples;
    e->feedback = feedback;
    e->wet      = wet;
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

    // The cursor advances once for the whole block and every channel walks it
    // in step: two channels running on different cursors is two rooms.
    for (uint32_t i = 0; i < samples; i++) {
        uint32_t write = (e->cursor + i) % e->length;
        uint32_t read  = (write + e->length - e->delaySamples) % e->length;

        for (uint32_t c = 0; c < n; c++) {
            int32_t *line = e->buffer + (size_t)c * e->length;
            int32_t in  = data[c][i];
            int32_t tap = line[read];

            int32_t out = in + (int32_t)((float)tap * e->wet);
            line[write] = in + (int32_t)((float)tap * e->feedback);
            data[c][i]  = out;

            int32_t a = in  < 0 ? -in  : in;
            int32_t b = out < 0 ? -out : out;
            if (a > e->peakIn)  e->peakIn  = a;
            if (b > e->peakOut) e->peakOut = b;
        }
    }
    e->cursor = (e->cursor + samples) % e->length;
}
