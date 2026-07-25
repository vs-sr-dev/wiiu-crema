// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan

#include "crema_audio.h"

#include <coreinit/cache.h>
#include <coreinit/transition.h>
#include <sndcore2/core.h>
#include <sndcore2/device.h>
#include <sndcore2/voice.h>
#include <whb/log.h>

#include <malloc.h>
#include <math.h>
#include <string.h>

// Sixteen is not a hardware limit — AX has far more voices than this — it is
// how many a game like ours can have making noise at once before the mix turns
// to mud. Raise it when something actually runs out.
#define CREMA_AUDIO_VOICES 16

#define PRIORITY_ONESHOT 20
#define PRIORITY_HELD    25

// AX volumes are 1.15 fixed point: 0x8000 is unity gain, not maximum.
#define AX_UNITY_VOLUME 0x8000

struct CremaAudioVoice {
    AXVoice *volatile ax;   // NULL once AX has taken the voice back
    bool     inUse;
    bool     held;          // the caller owns it: never auto-reclaimed
    uint32_t rate;          // to recompute the SRC ratio when the pitch changes
};

static struct CremaAudioVoice s_voices[CREMA_AUDIO_VOICES];
static bool     s_ready;
static uint32_t s_mixRate = 48000;

// AX can take a voice away from us to give it to something more important. It
// tells us by calling this, on its own thread, *after* the voice is gone — so
// the one correct reaction is to forget the pointer and never free it.
static void voiceDropped(void *context, uint32_t reason, uint32_t unused)
{
    (void)reason;
    (void)unused;
    struct CremaAudioVoice *slot = (struct CremaAudioVoice *)context;
    if (slot)
        slot->ax = NULL;
}

bool CremaAudioInit(void)
{
    memset(s_voices, 0, sizeof(s_voices));

    // Why every PoC before this one ran with the Wii U Menu still humming
    // underneath it: when the system launches a title it does not stop its own
    // music, it hands it over as *transition audio* — a buffer the DSP keeps
    // playing by itself so the sound does not cut out while the game loads. The
    // title is expected to take audio over and end it. Ours never touched AX,
    // so nobody ever did, and the menu played forever.
    //
    // AXInit is that handover. We print the saved-audio flag on both sides of
    // it rather than assume: if the console still says the transition is live
    // afterwards, the log says so and we know where to look.
    int savedBefore = __OSGetSavedAudioFlags();

    if (!AXIsInit()) {
        AXInitParams params;
        memset(&params, 0, sizeof(params));
        params.renderer = AX_INIT_RENDERER_48KHZ;
        params.pipeline = AX_INIT_PIPELINE_SINGLE;
        AXInitWithParams(&params);
    }
    if (!AXIsInit()) {
        WHBLogPrintf("[audio] AXInit failed");
        return false;
    }
    WHBLogPrintf("[audio] saved audio flags: 0x%x before AXInit, 0x%x after",
                 savedBefore, __OSGetSavedAudioFlags());

    // Whatever the renderer runs at is the rate every pitch is measured
    // against: a sound baked at 32 kHz plays at ratio 0.667 here.
    s_mixRate = AXGetInputSamplesPerSec();
    if (s_mixRate == 0)
        s_mixRate = 48000;

    s_ready = true;
    WHBLogPrintf("[audio] AX up: %u Hz mix, %u voices available, %d in our pool",
                 s_mixRate, (unsigned)AXGetMaxVoices(), CREMA_AUDIO_VOICES);
    return true;
}

void CremaAudioShutdown(void)
{
    if (!s_ready)
        return;
    for (int i = 0; i < CREMA_AUDIO_VOICES; i++) {
        AXVoice *v = s_voices[i].ax;
        if (!s_voices[i].inUse || !v)
            continue;
        AXSetVoiceState(v, AX_VOICE_STATE_STOPPED);
        AXFreeVoice(v);
    }
    memset(s_voices, 0, sizeof(s_voices));
    AXQuit();
    s_ready = false;
}

// --- sounds ------------------------------------------------------------------

static bool soundCreate(CremaSound *snd, const int16_t *pcm, uint32_t count,
                        uint32_t rate, uint32_t loopStart, bool looping)
{
    if (!snd || !pcm || count == 0)
        return false;

    memset(snd, 0, sizeof(*snd));
    size_t bytes = (size_t)count * sizeof(int16_t);
    // 64-byte aligned so the flush below writes back our cache lines and
    // nothing else's — the buffer must not share a line with a live neighbour.
    snd->samples = (int16_t *)memalign(64, (bytes + 63u) & ~(size_t)63);
    if (!snd->samples)
        return false;

    memcpy(snd->samples, pcm, bytes);
    // The DSP reads memory directly. Everything we just wrote is still sitting
    // in the CPU's cache until this line, and a voice started before it would
    // play whatever was there before.
    DCFlushRange(snd->samples, bytes);

    snd->count     = count;
    snd->rate      = rate ? rate : s_mixRate;
    snd->loopStart = loopStart;
    snd->looping   = looping;
    return true;
}

bool CremaSoundCreate(CremaSound *snd, const int16_t *pcm, uint32_t count,
                      uint32_t rate)
{
    return soundCreate(snd, pcm, count, rate, 0, false);
}

bool CremaSoundCreateLooping(CremaSound *snd, const int16_t *pcm, uint32_t count,
                             uint32_t rate, uint32_t loopStart)
{
    if (loopStart >= count)
        loopStart = 0;
    return soundCreate(snd, pcm, count, rate, loopStart, true);
}

void CremaSoundDestroy(CremaSound *snd)
{
    if (!snd || !snd->samples)
        return;
    free(snd->samples);
    memset(snd, 0, sizeof(*snd));
}

// --- voices ------------------------------------------------------------------

static uint16_t volumeToFixed(float volume)
{
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    return (uint16_t)(volume * (float)AX_UNITY_VOLUME);
}

static float pitchToRatio(uint32_t rate, float pitch)
{
    float ratio = ((float)rate / (float)s_mixRate) * pitch;
    // The resampler will not follow you anywhere: a ratio of zero stops the
    // voice dead and a huge one races through the buffer in a single frame.
    // The floor is low on purpose — a 32-sample cycle asked for a bass A1 sits
    // at 0.055, so anything stricter would quietly transpose the bottom two
    // octaves of every instrument.
    if (ratio < 0.01f) ratio = 0.01f;
    if (ratio > 4.0f)  ratio = 4.0f;
    return ratio;
}

// Take a voice and give it everything that does not depend on which sound it
// will play: type, device mix, a silent envelope. It comes back stopped.
static struct CremaAudioVoice *acquireSlot(bool held)
{
    if (!s_ready)
        return NULL;

    struct CremaAudioVoice *slot = NULL;
    for (int i = 0; i < CREMA_AUDIO_VOICES; i++) {
        if (!s_voices[i].inUse) {
            slot = &s_voices[i];
            break;
        }
    }
    if (!slot)
        return NULL;

    AXVoice *v = AXAcquireVoiceEx(held ? PRIORITY_HELD : PRIORITY_ONESHOT,
                                  voiceDropped, slot);
    if (!v)
        return NULL;

    slot->ax    = v;
    slot->inUse = true;
    slot->held  = held;
    slot->rate  = 0;

    // Everything between Begin and End is one atomic change as far as the DSP
    // is concerned: without it a voice can start playing half-configured.
    AXVoiceBegin(v);

    AXSetVoiceType(v, 0);

    AXVoiceVeData ve;
    ve.volume = 0;
    ve.delta  = 0;
    AXSetVoiceVe(v, &ve);

    // The device mix is where a mono voice lands on a device's channels: bus 0
    // is the main mix, channels 0 and 1 are left and right. Both devices, so
    // the sound comes out of the TV and the GamePad alike.
    AXVoiceDeviceMixData mix[6];
    memset(mix, 0, sizeof(mix));
    mix[0].bus[0].volume = AX_UNITY_VOLUME;
    mix[1].bus[0].volume = AX_UNITY_VOLUME;
    AXSetVoiceDeviceMix(v, AX_DEVICE_TYPE_TV,  0, mix);
    AXSetVoiceDeviceMix(v, AX_DEVICE_TYPE_DRC, 0, mix);

    AXSetVoiceSrcType(v, AX_VOICE_SRC_TYPE_LINEAR);
    AXVoiceEnd(v);
    return slot;
}

// Aim a voice at a sound and start it from the top. Everything here is a write
// to voice state — no acquisition, no allocation — which is what makes it safe
// to call from the audio thread.
static void applySound(struct CremaAudioVoice *slot, const CremaSound *snd,
                       float volume, float pitch)
{
    AXVoice *v = slot ? slot->ax : NULL;
    if (!v || !snd || !snd->samples || snd->count == 0)
        return;
    slot->rate = snd->rate;

    AXVoiceBegin(v);

    AXVoiceVeData ve;
    ve.volume = volumeToFixed(volume);
    ve.delta  = 0;
    AXSetVoiceVe(v, &ve);

    // A recycled voice still holds the last one's resampler state: zero it, or
    // the first sample of a new sound is a click left over from the old one.
    AXVoiceSrc src;
    memset(&src, 0, sizeof(src));
    AXSetVoiceSrc(v, &src);
    AXSetVoiceSrcType(v, AX_VOICE_SRC_TYPE_LINEAR);
    AXSetVoiceSrcRatio(v, pitchToRatio(snd->rate, pitch));

    // Offsets are counted in samples for LPCM16, and endOffset is the last
    // sample, not the one past it.
    AXVoiceOffsets offs;
    memset(&offs, 0, sizeof(offs));
    offs.dataType       = AX_VOICE_FORMAT_LPCM16;
    offs.loopingEnabled = snd->looping ? AX_VOICE_LOOP_ENABLED
                                       : AX_VOICE_LOOP_DISABLED;
    offs.loopOffset     = snd->looping ? snd->loopStart : 0;
    offs.endOffset      = snd->count - 1;
    offs.currentOffset  = 0;
    offs.data           = snd->samples;
    AXSetVoiceOffsets(v, &offs);

    AXSetVoiceState(v, AX_VOICE_STATE_PLAYING);
    AXVoiceEnd(v);
}

bool CremaAudioPlay(const CremaSound *snd, float volume, float pitch)
{
    if (!snd || !snd->samples)
        return false;
    struct CremaAudioVoice *slot = acquireSlot(false);
    if (!slot)
        return false;
    applySound(slot, snd, volume, pitch);
    return true;
}

CremaAudioVoice *CremaAudioHold(const CremaSound *snd, float volume, float pitch)
{
    if (!snd || !snd->samples)
        return NULL;
    struct CremaAudioVoice *slot = acquireSlot(true);
    if (slot)
        applySound(slot, snd, volume, pitch);
    return slot;
}

CremaAudioVoice *CremaAudioReserve(void)
{
    return acquireSlot(true);
}

void CremaAudioVoiceRetrigger(CremaAudioVoice *voice, const CremaSound *snd,
                              float volume, float pitch)
{
    if (voice && voice->inUse)
        applySound(voice, snd, volume, pitch);
}

void CremaAudioVoiceSilence(CremaAudioVoice *voice)
{
    if (voice && voice->inUse && voice->ax)
        AXSetVoiceState(voice->ax, AX_VOICE_STATE_STOPPED);
}

void CremaAudioVoiceRamp(CremaAudioVoice *voice, float from, float to,
                         uint32_t samples, float pitch)
{
    if (!voice || !voice->inUse || samples == 0)
        return;
    AXVoice *v = voice->ax;
    if (!v)
        return;

    int32_t a = (int32_t)volumeToFixed(from);
    int32_t b = (int32_t)volumeToFixed(to);

    AXVoiceBegin(v);
    AXVoiceVeData ve;
    ve.volume = (uint16_t)a;
    // Truncating division leaves the ramp a hair short of its target, and that
    // is fine: the next frame writes the exact volume again, so the error never
    // accumulates. What must not happen is the opposite — a delta left behind
    // in a voice nobody writes to again keeps being applied every frame, which
    // is why the caller writes once more with from == to when the ramp ends.
    // (AXSetVoiceVeDelta sets that field alone; we always have both numbers.)
    ve.delta = (int16_t)((b - a) / (int32_t)samples);
    AXSetVoiceVe(v, &ve);
    AXSetVoiceSrcRatio(v, pitchToRatio(voice->rate, pitch));
    AXVoiceEnd(v);
}

void CremaAudioVoiceSet(CremaAudioVoice *voice, float volume, float pitch)
{
    if (!voice || !voice->inUse)
        return;
    AXVoice *v = voice->ax;
    if (!v)
        return;   // AX took it back while we were holding the handle

    AXVoiceBegin(v);
    AXVoiceVeData ve;
    ve.volume = volumeToFixed(volume);
    ve.delta  = 0;
    AXSetVoiceVe(v, &ve);
    AXSetVoiceSrcRatio(v, pitchToRatio(voice->rate, pitch));
    AXVoiceEnd(v);
}

void CremaAudioRelease(CremaAudioVoice *voice)
{
    if (!voice || !voice->inUse)
        return;
    AXVoice *v = voice->ax;
    if (v) {
        AXSetVoiceState(v, AX_VOICE_STATE_STOPPED);
        AXFreeVoice(v);
    }
    voice->ax    = NULL;
    voice->inUse = false;
    voice->held  = false;
}

void CremaAudioUpdate(void)
{
    if (!s_ready)
        return;
    for (int i = 0; i < CREMA_AUDIO_VOICES; i++) {
        struct CremaAudioVoice *slot = &s_voices[i];
        if (!slot->inUse || slot->held)
            continue;
        AXVoice *v = slot->ax;
        if (!v) {                 // AX reclaimed it for us
            slot->inUse = false;
            continue;
        }
        if (!AXIsVoiceRunning(v)) {
            AXFreeVoice(v);
            slot->ax    = NULL;
            slot->inUse = false;
        }
    }
}

uint32_t CremaAudioVoicesInUse(void)
{
    uint32_t n = 0;
    for (int i = 0; i < CREMA_AUDIO_VOICES; i++)
        if (s_voices[i].inUse)
            n++;
    return n;
}
