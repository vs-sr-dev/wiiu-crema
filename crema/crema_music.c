// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan

#include "crema_music.h"

#include <coreinit/time.h>
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

struct CremaMusicState {
    SongEvent *events;
    uint32_t   eventCount;
    const CremaInstrument *instruments[64];
    uint32_t   instrumentCount;
    uint32_t   loopMs, endMs, channels;

    CremaAudioVoice *voices[CREMA_MUSIC_MAX_CHANNELS];
    float      volume;
    volatile bool playing;

    // Time is accumulated in microseconds so the 3 ms frame does not have to
    // divide evenly into a millisecond — it does not, and rounding it every
    // tick would drift the whole song against itself.
    uint64_t   timeUs;
    uint32_t   frameUs;
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

    if (e->type == EVENT_NOTE_OFF) {
        CremaAudioVoiceSilence(voice);
        return;
    }
    if (e->instrument >= m->instrumentCount)
        return;
    const CremaInstrument *inst = m->instruments[e->instrument];
    if (!inst)
        return;

    // 64 is full in the file, the way every piano roll and every tracker before
    // it has counted volume.
    float vol = (float)e->volume / 64.0f * m->volume;
    float note = (float)e->note + (float)e->detune / 100.0f;
    CremaAudioVoiceRetrigger(voice, &inst->sound, vol,
                             CremaInstrumentPitch(inst, note));
    m->stats.notesOn++;
}

static void silenceAll(CremaMusic *m)
{
    for (uint32_t i = 0; i < CREMA_MUSIC_MAX_CHANNELS; i++)
        if (m->voices[i])
            CremaAudioVoiceSilence(m->voices[i]);
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
        // Every voice is stopped across the seam: a note still held when the
        // song wraps has no note-off waiting for it any more, and would sit
        // there for the rest of the game.
        silenceAll(m);
        m->timeUs = (uint64_t)m->loopMs * 1000u;
        m->cursor = 0;
        while (m->cursor < m->eventCount &&
               m->events[m->cursor].timeMs < m->loopMs)
            m->cursor++;
        m->stats.loops++;
    }

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
    m->timeUs  = 0;
    m->cursor  = 0;
    memset(&m->stats, 0, sizeof(m->stats));

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
