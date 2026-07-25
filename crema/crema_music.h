// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan
//
// The sequencer: a song played from AX's own audio-frame callback.
//
// `AXRegisterAppFrameCallback` hands you a tick once per audio frame — 3 ms,
// about 333 times a second — on AX's thread, in time with the DSP rather than
// with the picture. That is the difference between music and music that
// stutters: a game loop can only place a note to the nearest 16.7 ms, and at
// 132 BPM a sixteenth note is 113 ms, so every note would land up to a seventh
// of its own length late, differently each time. The audio frame does not care
// what the renderer is doing.
//
// What that costs is a rule, not a bargain: **the callback never allocates and
// never blocks.** Each channel reserves its voice once, on the game thread,
// before the song starts. From then on a note-on is a re-aiming of a voice the
// channel already owns (crema_audio's Retrigger), and a note-off is a stop. No
// acquisition, no free, no lock — so the audio thread and the game thread never
// have to agree about who owns which voice, and there is nothing to race over.
//
// A song is a flat list of events sorted in time, which is a piano roll and not
// a pattern grid: this note starts on this channel at this millisecond, this
// note stops. Baked by tools/gen_song.py.

#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "crema_bank.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CREMA_MUSIC_MAX_CHANNELS 8

typedef struct {
    uint32_t ticks;        // audio frames since the song started
    uint32_t lastUs;       // how long the last tick took, microseconds
    uint32_t maxUs;        // the worst one so far
    uint32_t notesOn;      // note-ons issued, for a sanity check on the log
    uint32_t loops;
} CremaMusicStats;

typedef struct CremaMusicState CremaMusic;

// Parse a .csong and resolve its instrument names against the bank. The blob
// may be freed afterwards — the events are copied — but the bank must outlive
// the song, since its samples are what the DSP will be reading.
bool CremaMusicLoadFromMemory(CremaMusic **music, const void *blob, size_t size,
                              const CremaBank *bank);

// Reserves one voice per channel and starts the audio-frame callback. Call it
// from the game thread: this is the only moment anything is acquired.
bool CremaMusicStart(CremaMusic *music);

// Stops the callback and silences every channel; the voices stay reserved, so
// Start can be called again without allocating.
void CremaMusicStop(CremaMusic *music);

// 0..1, applied on top of each note's own volume. Takes effect on the next
// note — the point is to duck the music under an explosion, not to fade it.
void CremaMusicSetVolume(CremaMusic *music, float volume);

bool CremaMusicPlaying(const CremaMusic *music);
void CremaMusicGetStats(const CremaMusic *music, CremaMusicStats *out);

// Stops, hands the voices back, frees the song.
void CremaMusicClose(CremaMusic *music);

#ifdef __cplusplus
}
#endif
