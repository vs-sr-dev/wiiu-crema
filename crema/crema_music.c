// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan

#include "crema_music.h"

#include <coreinit/time.h>
#include <math.h>
#include <sndcore2/core.h>
#include <stdlib.h>
#include <string.h>
#include <whb/log.h>

#define CSONG_VERSION 1
#define CSONG_HEADER_SIZE 32
#define CSONG_NAME_SIZE 24

enum { EVENT_NOTE_OFF = 0, EVENT_NOTE_ON = 1 };

// Mirrors tools/gen_song.py.
typedef struct {
    char     magic[4];
    uint32_t version;
    uint32_t eventCount;
    uint32_t instrumentCount;
    uint32_t loopMs;
    uint32_t endMs;
    uint32_t channels;
    uint32_t reserved;
} SongHeader;
_Static_assert(sizeof(SongHeader) == CSONG_HEADER_SIZE, "csong header is 32 B");

typedef struct {
    uint32_t timeMs;
    uint8_t  channel;
    uint8_t  type;
    uint8_t  note;
    uint8_t  instrument;
    uint8_t  volume;
    // Cents off the note. It exists because a real chip cannot play in tune:
    // its pitch comes from an integer divider, so an E6 on a NES is 3.3 cents
    // flat and always the same 3.3 cents flat. Throwing that away and playing
    // the correct note is the one change that makes a chip tune stop sounding
    // like the machine it was written for.
    int8_t   detune;
    uint8_t  pad[2];
} SongEvent;
_Static_assert(sizeof(SongEvent) == 12, "csong event is 12 bytes");

// --- the shape of a note -----------------------------------------------------
//
// The sequencer already ticks 333 times a second and, until now, spent every
// one of those ticks doing nothing but starting and stopping notes. An envelope
// costs a handful of multiplies on a tick we are paying for anyway, and it is
// the difference between a melody and a tune: a note that fades has an end, and
// a note that ends leaves room for the next one.
//
// An instrument's milliseconds are converted into per-tick numbers once, at
// Start, because the tick rate cannot change while a song plays and expf on the
// audio thread is a cost with nothing to buy.

enum { ENV_OFF = 0, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };

typedef struct {
    float    attackStep;     // level added per tick; 0 means no attack at all
    float    decayCoef;      // one-pole toward sustain, per tick; 1 is instant
    float    sustain;        // 0..1 of the note's own volume
    float    releaseCoef;    // one-pole toward silence; 1 is a hard stop
    float    vibPhaseStep;   // radians per tick
    float    vibDepth;       // playback-ratio deviation, ± this much
    uint32_t vibDelayTicks;
} Shape;

// What every note was before this file learned about envelopes, and what the
// audition switch turns back on: full volume at the attack, full volume until
// the note-off, silence. Kept as a real Shape so there is one code path and the
// A/B comparison is honestly the same machine running different numbers.
static const Shape RECTANGLE = { 0.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0u };

typedef struct {
    const Shape *shape;
    uint8_t  stage;
    bool     ramping;        // a non-zero delta is sitting in the voice
    float    level;          // 0..1, the envelope itself
    float    peak;           // the note's volume, captured at note-on
    float    lastVolume;     // where we told the DSP the voice is now
    float    pitch;
    float    vibPhase;
    uint32_t ticks;          // since note-on, for the vibrato's delay
} Channel;

struct CremaMusicState {
    SongEvent *events;
    uint32_t   eventCount;
    const CremaInstrument *instruments[64];
    Shape      shapes[64];
    uint32_t   instrumentCount;
    uint32_t   loopMs, endMs, channels;

    CremaAudioVoice *voices[CREMA_MUSIC_MAX_CHANNELS];
    Channel    chan[CREMA_MUSIC_MAX_CHANNELS];
    float      volume;
    volatile bool playing;
    volatile bool shaping;

    // Time is accumulated in microseconds so the 3 ms frame does not have to
    // divide evenly into a millisecond — it does not, and rounding it every
    // tick would drift the whole song against itself.
    uint64_t   timeUs;
    uint32_t   frameUs;
    uint32_t   frameSamples;
    uint32_t   cursor;

    CremaMusicStats stats;
};

// AXRegisterAppFrameCallback takes a bare function pointer with no user data,
// so there is one song at a time. That is not a limitation worth engineering
// around: two songs playing at once is a bug in every game ever written.
static CremaMusic *s_active;

static void applyEvent(CremaMusic *m, const SongEvent *e)
{
    if (e->channel >= m->channels || e->channel >= CREMA_MUSIC_MAX_CHANNELS)
        return;
    CremaAudioVoice *voice = m->voices[e->channel];
    if (!voice)
        return;
    Channel *c = &m->chan[e->channel];

    if (e->type == EVENT_NOTE_OFF) {
        // Not a stop — the start of the release. A note that is cut off at the
        // note-off ends with a click and leaves a hole where its tail should
        // be; the envelope pass below is what actually silences the voice, in
        // this same tick if the instrument asked for no release at all.
        if (c->stage != ENV_OFF)
            c->stage = ENV_RELEASE;
        return;
    }
    if (e->instrument >= m->instrumentCount)
        return;
    const CremaInstrument *inst = m->instruments[e->instrument];
    if (!inst)
        return;

    const Shape *shape = m->shaping ? &m->shapes[e->instrument] : &RECTANGLE;
    // 64 is full in the file, the way every piano roll and every tracker before
    // it has counted volume.
    float vol = (float)e->volume / 64.0f * m->volume;
    float note = (float)e->note + (float)e->detune / 100.0f;

    c->shape    = shape;
    c->peak     = vol;
    c->pitch    = CremaInstrumentPitch(inst, note);
    c->ticks    = 0;
    c->vibPhase = 0.0f;
    c->ramping  = false;
    if (shape->attackStep <= 0.0f) {
        c->level = 1.0f;
        c->stage = ENV_DECAY;
    } else {
        // The level is deliberately left where it was: a new note on a channel
        // whose last one is still releasing starts from that level instead of
        // from zero, so the attack is a continuation and not a gap.
        c->stage = ENV_ATTACK;
    }
    c->lastVolume = c->level * c->peak;

    CremaAudioVoiceRetrigger(voice, &inst->sound, c->lastVolume, c->pitch);
    m->stats.notesOn++;
}

// One tick of every sounding note. Everything here is arithmetic on numbers
// prepared at Start, plus at most one voice write per channel — and a channel
// sitting on its sustain writes nothing at all, which is why holding a chord
// costs the same as holding silence.
static void advanceChannels(CremaMusic *m)
{
    for (uint32_t i = 0; i < m->channels && i < CREMA_MUSIC_MAX_CHANNELS; i++) {
        Channel *c = &m->chan[i];
        if (c->stage == ENV_OFF)
            continue;
        CremaAudioVoice *voice = m->voices[i];
        if (!voice)
            continue;
        const Shape *s = c->shape;

        switch (c->stage) {
        case ENV_ATTACK:
            c->level += s->attackStep;
            if (c->level >= 1.0f) {
                c->level = 1.0f;
                c->stage = ENV_DECAY;
            }
            break;
        case ENV_DECAY:
            if (s->decayCoef >= 1.0f || c->level <= s->sustain) {
                c->level = s->sustain;
            } else {
                c->level += (s->sustain - c->level) * s->decayCoef;
                // The one-pole only ever approaches its target, so it is told
                // when it has arrived; otherwise the channel would rewrite its
                // voice forever over changes nothing can hear.
                if (c->level - s->sustain < 0.002f)
                    c->level = s->sustain;
            }
            if (c->level <= s->sustain)
                c->stage = ENV_SUSTAIN;
            break;
        case ENV_SUSTAIN:
            break;
        case ENV_RELEASE:
            c->level = (s->releaseCoef >= 1.0f)
                     ? 0.0f : c->level * (1.0f - s->releaseCoef);
            break;
        }

        // Silence is an end, whichever stage reached it. A percussive
        // instrument — sustain zero — gets here while the key is still down,
        // and that is right: it has already said everything it had to say, and
        // holding the voice open would only leave the DSP looping nothing.
        if (c->level <= 0.0008f && c->stage != ENV_ATTACK) {
            CremaAudioVoiceSilence(voice);
            c->level = c->lastVolume = 0.0f;
            c->stage = ENV_OFF;
            c->ramping = false;
            continue;
        }

        float vol = c->level * c->peak;
        float pitch = c->pitch;
        bool vib = (s->vibDepth != 0.0f && c->ticks >= s->vibDelayTicks);
        if (vib) {
            c->vibPhase += s->vibPhaseStep;
            if (c->vibPhase > 6.2831853f)
                c->vibPhase -= 6.2831853f;
            // 1 + d*sin rather than 2^(cents/1200): at the depths a vibrato
            // uses the two differ by well under a cent, and one of them is a
            // multiply.
            pitch *= 1.0f + s->vibDepth * sinf(c->vibPhase);
        }

        // Written only when something moved — or when the voice still carries a
        // delta from the previous tick, which has to be cleared once or the DSP
        // would keep ramping on its own after we stopped asking.
        if (vol != c->lastVolume || c->ramping || vib) {
            CremaAudioVoiceRamp(voice, c->lastVolume, vol, m->frameSamples,
                                pitch);
            c->ramping = (vol != c->lastVolume);
            c->lastVolume = vol;
        }
        c->ticks++;
    }
}

// The seam of a loop, or a song being stopped: let every note fall rather than
// cutting it. A held note at the wrap has no note-off waiting for it any more.
static void releaseAll(CremaMusic *m)
{
    for (uint32_t i = 0; i < CREMA_MUSIC_MAX_CHANNELS; i++)
        if (m->chan[i].stage != ENV_OFF)
            m->chan[i].stage = ENV_RELEASE;
}

static void silenceAll(CremaMusic *m)
{
    for (uint32_t i = 0; i < CREMA_MUSIC_MAX_CHANNELS; i++) {
        if (m->voices[i])
            CremaAudioVoiceSilence(m->voices[i]);
        memset(&m->chan[i], 0, sizeof(m->chan[i]));
    }
}

// One audio frame. Everything here is arithmetic and voice writes; if it ever
// grows a malloc or a lock, the music will stutter and the cause will not be
// obvious, so the cost is measured on every tick instead of being assumed.
static void musicTick(void)
{
    CremaMusic *m = s_active;
    if (!m || !m->playing)
        return;
    uint64_t t0 = OSGetSystemTime();

    m->timeUs += m->frameUs;
    uint64_t nowMs = m->timeUs / 1000u;

    while (m->cursor < m->eventCount &&
           m->events[m->cursor].timeMs <= nowMs) {
        applyEvent(m, &m->events[m->cursor]);
        m->cursor++;
    }

    if (nowMs >= m->endMs) {
        releaseAll(m);
        m->timeUs = (uint64_t)m->loopMs * 1000u;
        m->cursor = 0;
        while (m->cursor < m->eventCount &&
               m->events[m->cursor].timeMs < m->loopMs)
            m->cursor++;
        m->stats.loops++;
    }

    // After the events, always: a note-on and the first step of its attack
    // belong to the same tick, and a note-off with no release has to reach the
    // voice before the frame it was written for is rendered.
    advanceChannels(m);

    uint32_t us = (uint32_t)OSTicksToMicroseconds(OSGetSystemTime() - t0);
    m->stats.lastUs = us;
    if (us > m->stats.maxUs)
        m->stats.maxUs = us;
    m->stats.ticks++;
}

bool CremaMusicLoadFromMemory(CremaMusic **out, const void *blob, size_t size,
                              const CremaBank *bank)
{
    *out = NULL;
    if (!blob || size < sizeof(SongHeader) || !bank) {
        WHBLogPrintf("[music] too small to be a .csong");
        return false;
    }

    const uint8_t *base = (const uint8_t *)blob;
    SongHeader h;
    memcpy(&h, base, sizeof(h));
    if (memcmp(h.magic, "CSNG", 4) != 0 || h.version != CSONG_VERSION) {
        WHBLogPrintf("[music] not a v%d .csong", CSONG_VERSION);
        return false;
    }
    size_t namesBytes = (size_t)h.instrumentCount * CSONG_NAME_SIZE;
    size_t eventBytes = (size_t)h.eventCount * sizeof(SongEvent);
    if (h.channels > CREMA_MUSIC_MAX_CHANNELS || h.instrumentCount > 64 ||
        sizeof(SongHeader) + namesBytes + eventBytes > size) {
        WHBLogPrintf("[music] %u channels / %u instruments / %u events does "
                     "not fit what we were handed",
                     h.channels, h.instrumentCount, h.eventCount);
        return false;
    }

    CremaMusic *m = (CremaMusic *)calloc(1, sizeof(CremaMusic));
    if (!m)
        return false;
    m->events = (SongEvent *)malloc(eventBytes);
    if (!m->events) {
        free(m);
        return false;
    }
    memcpy(m->events, base + sizeof(SongHeader) + namesBytes, eventBytes);
    m->eventCount = h.eventCount;
    m->loopMs     = h.loopMs;
    m->endMs      = h.endMs;
    m->channels   = h.channels;
    m->volume     = 1.0f;
    m->shaping    = true;
    m->instrumentCount = h.instrumentCount;

    // Names, not indices: a song and a bank are baked separately and this is
    // the only thing that keeps them honest about each other.
    uint32_t missing = 0;
    for (uint32_t i = 0; i < h.instrumentCount; i++) {
        char name[CSONG_NAME_SIZE + 1];
        memcpy(name, base + sizeof(SongHeader) + i * CSONG_NAME_SIZE,
               CSONG_NAME_SIZE);
        name[CSONG_NAME_SIZE] = '\0';
        m->instruments[i] = CremaBankFind(bank, name);
        if (!m->instruments[i])
            missing++;
    }

    WHBLogPrintf("[music] %u events, %u channels, %u instruments (%u missing), "
                 "%.1f s, loops at %.1f s", h.eventCount, h.channels,
                 h.instrumentCount, missing, h.endMs / 1000.0,
                 h.loopMs / 1000.0);
    *out = m;
    return true;
}

// A one-pole coefficient that reaches its target within `ms`. "Within" means
// three time constants, 95% of the way — which is what a person means when they
// say a note decays in 200 ms, and closer to how a string or a chip envelope
// behaves than a straight line down would be.
static float onePoleCoef(uint32_t ms, uint32_t frameUs)
{
    if (ms == 0)
        return 1.0f;                       // instant: no ramp at all
    float ticks = (float)ms * 1000.0f / (float)frameUs;
    if (ticks <= 1.0f)
        return 1.0f;                       // shorter than a tick is instant too
    return 1.0f - expf(-3.0f / ticks);
}

// Milliseconds and cents are what a person writes in a bank; steps per tick and
// playback ratios are what the tick can afford. This is where one becomes the
// other, once, on the game thread.
static void buildShapes(CremaMusic *m)
{
    float ticksPerMs = 1000.0f / (float)m->frameUs;

    for (uint32_t i = 0; i < m->instrumentCount; i++) {
        Shape *s = &m->shapes[i];
        *s = RECTANGLE;
        const CremaInstrument *inst = m->instruments[i];
        if (!inst)
            continue;
        const CremaEnvelope *e = &inst->env;

        // An instrument that says nothing about its shape keeps the one it
        // always had. Note that this is not the same as sustain zero, which is
        // a real answer — a drum — and reads identically in the file only when
        // every other field is zero too.
        if (e->attackMs || e->decayMs || e->sustain || e->releaseMs) {
            // An attack shorter than a tick still gets a whole tick of ramp
            // rather than being rounded away to nothing. Three milliseconds
            // instead of two is not a difference anyone can hear; a step from
            // silence to full volume in one sample is a click everyone can, and
            // the DSP walks that single frame at the sample rate anyway.
            float atkTicks = (float)e->attackMs * ticksPerMs;
            if (atkTicks < 1.0f)
                atkTicks = 1.0f;
            s->attackStep = e->attackMs ? 1.0f / atkTicks : 0.0f;
            s->sustain    = (float)e->sustain / 1000.0f;
            if (s->sustain > 1.0f)
                s->sustain = 1.0f;
            s->decayCoef   = onePoleCoef(e->decayMs, m->frameUs);
            s->releaseCoef = onePoleCoef(e->releaseMs, m->frameUs);
        }

        if (inst->vib.depthCents != 0 && inst->vib.rateMilliHz != 0) {
            // 0.00057762 is ln 2 / 1200: a cent, as a multiplier, to first
            // order. The depth is a deviation of the playback ratio, so it
            // rides on top of whatever the note's own detune already did.
            s->vibDepth = (float)inst->vib.depthCents * 0.00057762f;
            s->vibPhaseStep = 6.2831853f * (float)inst->vib.rateMilliHz *
                              (float)m->frameUs / 1.0e9f;
            s->vibDelayTicks = (uint32_t)((float)inst->vib.delayMs * ticksPerMs);
        }
    }
}

bool CremaMusicStart(CremaMusic *m)
{
    if (!m || m->playing)
        return false;

    // The one allocation, here, on the game thread, before anything ticks.
    for (uint32_t i = 0; i < m->channels; i++) {
        if (!m->voices[i])
            m->voices[i] = CremaAudioReserve();
        if (!m->voices[i]) {
            WHBLogPrintf("[music] no voice left for channel %u", i);
            return false;
        }
    }

    uint32_t samplesPerFrame = AXGetInputSamplesPerFrame();
    uint32_t rate = AXGetInputSamplesPerSec();
    if (samplesPerFrame == 0 || rate == 0) {
        WHBLogPrintf("[music] AX reports a frame of %u samples at %u Hz",
                     samplesPerFrame, rate);
        return false;
    }
    m->frameUs = (uint32_t)((uint64_t)samplesPerFrame * 1000000u / rate);
    m->frameSamples = samplesPerFrame;
    m->timeUs  = 0;
    m->cursor  = 0;
    memset(&m->stats, 0, sizeof(m->stats));
    memset(m->chan, 0, sizeof(m->chan));
    buildShapes(m);

    s_active  = m;
    m->playing = true;
    AXRegisterAppFrameCallback(musicTick);
    WHBLogPrintf("[music] playing: %u samples per audio frame = %u us per tick "
                 "(%.0f ticks/s)", samplesPerFrame, m->frameUs,
                 1000000.0 / m->frameUs);
    return true;
}

void CremaMusicStop(CremaMusic *m)
{
    if (!m || !m->playing)
        return;
    m->playing = false;                    // the callback returns immediately
    AXDeregisterAppFrameCallback(musicTick);
    s_active = NULL;
    silenceAll(m);
}

void CremaMusicSetVolume(CremaMusic *m, float volume)
{
    if (!m)
        return;
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    m->volume = volume;
}

void CremaMusicSetShaping(CremaMusic *m, bool on)
{
    if (!m)
        return;
    // Only new notes change hands: a note already sounding keeps the Shape it
    // was started with, because swapping an envelope out from under a note in
    // its decay is a click, and the switch exists to compare sounds rather than
    // to make one.
    m->shaping = on;
}

bool CremaMusicShaping(const CremaMusic *m)
{
    return m && m->shaping;
}

bool CremaMusicPlaying(const CremaMusic *m)
{
    return m && m->playing;
}

void CremaMusicGetStats(const CremaMusic *m, CremaMusicStats *out)
{
    if (!m || !out)
        return;
    *out = m->stats;
}

void CremaMusicClose(CremaMusic *m)
{
    if (!m)
        return;
    CremaMusicStop(m);
    for (uint32_t i = 0; i < CREMA_MUSIC_MAX_CHANNELS; i++)
        if (m->voices[i])
            CremaAudioRelease(m->voices[i]);
    free(m->events);
    free(m);
}
