// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan
//
// Putting our own code inside AX's signal path.
//
// This is the only programmable point in the Wii U's audio pipeline. wut
// exposes `sndcore2` and nothing else — there is no AXFX, no effect library, no
// reverb to switch on — so an effect is something you write and hand to
// `AXRegisterAuxCallback`, and AX calls it once per audio frame with three
// milliseconds of somebody's mix to do as you like with.
//
// What that call actually looks like is not written down. wut declares it:
//
//     typedef void (*AXAuxCallback)(void *, void *);
//
// and that is one argument short. AX passes three — the channel data, the user
// pointer we registered, and a structure saying how many channels and how many
// samples there are — which you cannot know from the header, and without the
// third one you cannot write the loop. The rest of the ABI is undocumented in
// the same way: the registration's two `unk` parameters, the layout of the
// buffer, the format of the samples. All of it below was read out of Cemu's
// implementation (`src/Cafe/OS/libs/snd_core/ax_aux.cpp` and `ax_mix.cpp`),
// which is where a working emulator had to know the answers:
//
//   - AXRegisterAuxCallback(device, deviceIndex, auxBus, callback, userData) —
//     so wut's `unk0` is the device index (0 for the TV) and `unk1` is which of
//     the three aux buses this callback owns.
//   - The samples are **planar int32**: an array of `numChannels` pointers,
//     each `numSamples` long. Not interleaved, not 16-bit, not float.
//   - Six channels for the TV, four for the GamePad, 144 samples at 48 kHz.
//   - It is processed **in place**. The buffer handed in is the buffer AX reads
//     back, so the effect's output goes where its input came from.
//   - `mix[channel].bus[1]` is the send to aux bus **0**: AX indexes its own
//     scratch as `(1 + auxBus)`, so bus 0 is the dry main mix and the three
//     sends sit above it. Off by one in the only place it could hurt.
//   - The callback processes the **previous** frame and its result is mixed
//     back after — one audio frame, 3 ms, of latency, which for a reverb is
//     nothing and for a comb filter would be everything.
//
// Two things are still unknown and are stated rather than assumed. **The
// numeric scale of the buffer**: Cemu stores it shifted right by eight with a
// comment saying it is not sure why ("probably because AUX mixing always goes
// through the DSP which uses 16-bit arithmetic"), so `Echo` measures its own
// peak and the log prints it. **Cache coherency**: if the DSP writes these
// buffers by DMA then the CPU's view of them needs invalidating first, and
// there is no cache maintenance here on purpose — Nintendo's own effect library
// ran in this callback, so AX presenting a coherent buffer is the likely
// answer, and the symptom if it is wrong is unmistakable (silence, or a burst
// of whatever was in that memory last). An emulator cannot answer that one:
// Cemu has no caches to be wrong about. Only the console can.
//
// The rule from the sequencer applies here twice over: **no allocation, no
// blocking.** The delay line is taken once, on the game thread.

#pragma once
#include <coreinit/time.h>
#include <malloc.h>
#include <sndcore2/core.h>
#include <sndcore2/device.h>
#include <stdbool.h>
#include <string.h>
#include <whb/log.h>

#include "crema_audio.h"
#include "echo.h"

#define AUX_BUS        0        // which of the three sends we own
// The TV device reports six channels and the log prints what it reported, but
// only two of them ever carry anything: crema_audio puts a mono voice on
// channels 0 and 1 and leaves the surround channels at zero. Processing all six
// would be three times the work to delay silence, so the effect is built for
// two and echoProcess leaves the rest untouched.
#define AUX_CHANNELS   2
#define AUX_LINE       9600     // 200 ms at 48 kHz, per channel

// A second aux bus, used as a meter rather than an effect.
//
// This is the trick that turns "how much headroom does the mix need" from a
// guess into a reading. An aux bus receives the same voices the main bus does,
// summed the same way, so with every send at 1.0 the number arriving here IS
// the number arriving at the speakers — before any clipping, because the aux
// path is a separate accumulator. The callback measures it and then writes
// silence over its own buffer, so a meter costs a peak comparison and returns
// nothing to the mix.
//
// Full scale is 32767: a peak above that is the amount by which the mix would
// have clipped, expressed as a number instead of as a crackle.
#define AUX_METER_BUS  1

// The signature AX really calls, as against the one wut declares.
typedef struct {
    uint32_t numChannels;
    uint32_t numSamples;
} AuxCallbackInfo;

typedef void (*AuxCallbackFn)(int32_t **data, void *user, AuxCallbackInfo *info);

static Echo     s_echo;
static int32_t *s_echoLine;
static bool     s_echoOn;
static uint32_t s_auxUs, s_auxWorstUs;

// On AX's thread, before the app frame callback of the same frame.
static void auxProcess(int32_t **data, void *user, AuxCallbackInfo *info)
{
    (void)user;
    if (!info || !data)
        return;
    if (!s_echoOn) {
        // Bypassed still means "called": the counter is how we know the
        // registration took, on a machine where the alternative explanation for
        // silence is that nothing was ever routed here.
        s_echo.calls++;
        s_echo.lastChannels = info->numChannels;
        s_echo.lastSamples  = info->numSamples;
        return;
    }

    // Measured for the same reason the sequencer's tick is: this is the audio
    // thread, the budget is 3000 µs, and an effect is the first thing in this
    // pipeline whose cost grows with how good you want it to sound.
    uint64_t t0 = OSGetSystemTime();
    echoProcess(&s_echo, data, info->numChannels, info->numSamples);
    s_auxUs = (uint32_t)OSTicksToMicroseconds(OSGetSystemTime() - t0);
    if (s_auxUs > s_auxWorstUs)
        s_auxWorstUs = s_auxUs;
}

static int32_t s_meterPeak;      // biggest |sample| since the last reading
static int32_t s_meterHigh;      // biggest ever, for the line that matters

static void meterProcess(int32_t **data, void *user, AuxCallbackInfo *info)
{
    (void)user;
    if (!info || !data)
        return;
    uint32_t n = info->numChannels < 2 ? info->numChannels : 2;
    for (uint32_t c = 0; c < n; c++) {
        int32_t *ch = data[c];
        for (uint32_t i = 0; i < info->numSamples; i++) {
            int32_t v = ch[i] < 0 ? -ch[i] : ch[i];
            if (v > s_meterPeak)
                s_meterPeak = v;
        }
    }
    if (s_meterPeak > s_meterHigh)
        s_meterHigh = s_meterPeak;
    // Silence on the way out, or the meter would be heard as a second copy of
    // the mix: the aux return adds this buffer back at unity.
    for (uint32_t c = 0; c < info->numChannels; c++)
        memset(data[c], 0, info->numSamples * sizeof(int32_t));
}

// Read and reset: a peak meter that never falls is a meter that tells you about
// something that happened once, minutes ago.
static int32_t meterRead(void)
{
    int32_t p = s_meterPeak;
    s_meterPeak = 0;
    return p;
}

static bool auxInit(void)
{
    s_echoLine = (int32_t *)memalign(64, (size_t)AUX_LINE * AUX_CHANNELS *
                                         sizeof(int32_t));
    if (!s_echoLine) {
        WHBLogPrintf("[aux] no memory for a %d ms delay line",
                     AUX_LINE * 1000 / 48000);
        return false;
    }
    // 170 ms and about a third back in: two or three repeats that fall inside
    // the gap between notes rather than smearing across it. The numbers are
    // restrained for a reason the meter made visible — an aux return adds to a
    // mix that was already over full scale, so an effect's settings are a claim
    // on the same headroom the music is using.
    echoInit(&s_echo, s_echoLine, AUX_LINE, AUX_CHANNELS,
             8160 /* 170 ms */, 0.35f, 0.45f);

    // wut's typedef is short an argument, so the function we register is not
    // the type wut wants. Going through a union rather than a cast keeps the
    // lie in one visible place instead of hiding it in a (void *).
    union {
        AuxCallbackFn  real;
        AXAuxCallback  wut;
    } cb;
    cb.real = auxProcess;

    AXResult r = AXRegisterAuxCallback(AX_DEVICE_TYPE_TV, 0, AUX_BUS,
                                       cb.wut, NULL);
    bool echoOk = (r == AX_RESULT_SUCCESS);
    if (!echoOk)
        WHBLogPrintf("[aux] AXRegisterAuxCallback returned %d", (int)r);
    cb.real = meterProcess;
    r = AXRegisterAuxCallback(AX_DEVICE_TYPE_TV, 0, AUX_METER_BUS, cb.wut, NULL);
    if (r != AX_RESULT_SUCCESS)
        WHBLogPrintf("[aux] the meter would not register: %d", (int)r);
    else
        CremaAudioSetAuxSend(AUX_METER_BUS, 1.0f);

    WHBLogPrintf("[aux] echo %s on TV aux bus %d: %d ms line, feedback %.2f, "
                 "wet %.2f; meter on bus %d",
                 echoOk ? "registered" : "NOT registered", AUX_BUS,
                 AUX_LINE * 1000 / 48000, s_echo.feedback, s_echo.wet,
                 AUX_METER_BUS);
    return echoOk;
}

// The send is what makes it audible; the flag is what makes it an effect rather
// than a detour. Both move together so that "off" is silent and not merely dry.
static void auxSetEnabled(bool on)
{
    s_echoOn = on;
    if (!on)
        echoClear(&s_echo);
    CremaAudioSetAuxSend(AUX_BUS, on ? 0.30f : 0.0f);
}

static void auxShutdown(void)
{
    CremaAudioSetAuxSend(AUX_BUS, 0.0f);
    CremaAudioSetAuxSend(AUX_METER_BUS, 0.0f);
    s_echoOn = false;
    AXRegisterAuxCallback(AX_DEVICE_TYPE_TV, 0, AUX_BUS, NULL, NULL);
    AXRegisterAuxCallback(AX_DEVICE_TYPE_TV, 0, AUX_METER_BUS, NULL, NULL);
    free(s_echoLine);
    s_echoLine = NULL;
}
