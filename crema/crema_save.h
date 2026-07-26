// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan
//
// Persistence — getting a few bytes out of the process and back in identical.
//
// This is one of the two things the README says nothing here knows how to do,
// and it turned out to be less about writing a file than about deciding *where*
// a homebrew title is allowed to put one. Three candidates, and only one of them
// survives contact with both the emulator and the console:
//
//   /vol/save via nn_save — the real Wii U answer, and wrong here twice over.
//     A .wuhb launched under Aroma does not have a save directory of its own:
//     /vol/save belongs to whichever title is hosting it, so a high score would
//     be written into somebody else's save data. And Cemu does not export
//     SAVEInitCommonSaveDir at all (nn_save.cpp registers SAVEInit and
//     SAVEInitSaveDir and stops), so the emulator and the hardware would
//     disagree about a call that cannot be tested in the place it fails.
//
//   /vol/content — read-only. It is inside the bundle.
//
//   /vol/external01 — the SD card, which is where the .wuhb already lives. The
//     file lands next to the thing that wrote it, a PC can read it, and both
//     targets reach it the same way. This is what this module does.
//
// The SD needs mounting on one of the two and not on the other, and the
// difference is worth writing down because it is invisible until it bites. Under
// Aroma the card is already mounted process-wide (AromaBaseModule refcounts it),
// so /vol/external01 simply resolves. Cemu only mounts it when somebody asks —
// mountSDCard() is reached from FSMount and FSBindMount and nowhere else — so on
// the emulator the first fopen fails until the mount happens. So this module
// looks before it mounts: if the path is already there it touches nothing, which
// is the polite thing to do on a console where another module owns the card.
//
// A save file is the one file in a game that outlives the code that wrote it, so
// it carries a magic, a version and a checksum. The version is the caller's, not
// this module's: a game that changes the shape of its save says so, and a file
// from the old shape reads as absent rather than as garbage that happens to fit.
//
// The write is a write-elsewhere-and-rename, because the interesting failure is
// not a corrupt file but a *half* one: the console losing power between the
// header and the payload would otherwise take the previous save with it. The
// rename is the only moment the real name changes meaning.

#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Mounts the SD if it needs mounting and makes
// /vol/external01/wiiu/apps/<appName>/ exist. `appName` is the folder the
// .wuhb is deployed into, so the save sits beside the game rather than in a
// place only this module knows about.
//
// Returns false if the directory could not be made, which is the only failure
// worth reporting up: a game that cannot save is still a game, and every call
// below then fails quietly on its own.
bool CremaSaveInit(const char *appName);

// Gives the card back if this module was the one that mounted it. Does nothing
// when it found it already mounted — the card is not ours to unmount.
void CremaSaveShutdown(void);

// The directory in use, for a log line. Never NULL.
const char *CremaSaveDir(void);

// `name` is a bare filename ("record.dat"), not a path.
//
// `version` is the caller's idea of what `data` looks like. Bump it when the
// struct changes and old files stop being read instead of being misread.
bool CremaSaveWrite(const char *name, uint32_t version,
                    const void *data, size_t size);

// Reads at most `size` bytes into `data` and returns how many arrived: 0 when
// the file is absent, truncated, of another version, or fails its checksum.
// Every one of those is the same thing to a caller — "there is nothing to load"
// — and a caller that treats them differently is a caller writing a repair tool.
size_t CremaSaveRead(const char *name, uint32_t version,
                     void *data, size_t size);

// For a "clear data" menu. True if the file is gone afterwards, including the
// case where it was never there.
bool CremaSaveErase(const char *name);

#ifdef __cplusplus
}
#endif
