// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan

#include "crema_pak.h"

#include <coreinit/time.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <whb/log.h>

#define CPAK_VERSION 1
#define CPAK_HEADER_SIZE 32

// Mirrors tools/crema_bake.py. Big-endian file, big-endian CPU: a plain fread
// lands the fields already correct, which is the whole point of baking.
typedef struct {
    char     magic[4];
    uint32_t version;
    uint32_t entryCount;
    uint32_t totalSize;
    uint32_t reserved[4];
} PakHeader;
_Static_assert(sizeof(PakHeader) == CPAK_HEADER_SIZE, "cpak header is 32 bytes");
_Static_assert(sizeof(CremaPakEntry) == 40, "cpak entry is 40 bytes");

bool CremaPakOpen(CremaPak *pak, const char *path)
{
    memset(pak, 0, sizeof(*pak));

    uint64_t t0 = OSGetSystemTime();
    FILE *fh = fopen(path, "rb");
    if (!fh) {
        WHBLogPrintf("[pak] cannot open %s", path);
        return false;
    }
    uint64_t t1 = OSGetSystemTime();

    // Read one: the header. This is the expensive one — it is the read that
    // makes stdio set the stream up, and it costs the same 3-4 ms whether you
    // ask for 32 bytes or a megabyte. Which is exactly why the next one is
    // allowed to be enormous.
    PakHeader h;
    bool ok = fread(&h, 1, sizeof(h), fh) == sizeof(h);
    uint64_t t2 = OSGetSystemTime();

    if (ok && (memcmp(h.magic, "CPAK", 4) != 0 || h.version != CPAK_VERSION)) {
        WHBLogPrintf("[pak] %s: not a v%d .cpak", path, CPAK_VERSION);
        ok = false;
    }
    if (ok && (h.totalSize <= CPAK_HEADER_SIZE || h.entryCount == 0)) {
        WHBLogPrintf("[pak] %s: empty or malformed", path);
        ok = false;
    }

    size_t rest = ok ? h.totalSize - CPAK_HEADER_SIZE : 0;
    uint8_t *blob = ok ? (uint8_t *)memalign(64, rest) : NULL;
    if (ok && !blob) {
        WHBLogPrintf("[pak] %s: %u KB would not fit", path,
                     (uint32_t)(rest / 1024));
        ok = false;
    }

    // Read two: the directory and every payload, in one call.
    uint64_t t3 = OSGetSystemTime();
    if (ok)
        ok = fread(blob, 1, rest, fh) == rest;
    uint64_t t4 = OSGetSystemTime();
    fclose(fh);

    if (!ok) {
        free(blob);
        return false;
    }

    pak->blob     = blob;
    pak->blobSize = rest;
    pak->entries  = (const CremaPakEntry *)blob;
    pak->count    = h.entryCount;

    // A truncated or lying directory would hand out pointers past the end of
    // the block, so check it once here rather than trusting it at every Find.
    size_t dirBytes = (size_t)h.entryCount * sizeof(CremaPakEntry);
    ok = dirBytes <= rest;
    for (uint32_t i = 0; ok && i < h.entryCount; i++) {
        const CremaPakEntry *e = &pak->entries[i];
        if (e->offset < CPAK_HEADER_SIZE ||
            (size_t)(e->offset - CPAK_HEADER_SIZE) + e->size > rest)
            ok = false;
    }
    if (!ok) {
        WHBLogPrintf("[pak] %s: directory points outside the file", path);
        CremaPakClose(pak);
        return false;
    }

    double openMs = (double)OSTicksToMicroseconds(t1 - t0) / 1000.0;
    double hdrMs  = (double)OSTicksToMicroseconds(t2 - t1) / 1000.0;
    double readMs = (double)OSTicksToMicroseconds(t4 - t3) / 1000.0;
    WHBLogPrintf("[pak] %s: %u entries, %u KB", path, h.entryCount,
                 (uint32_t)(h.totalSize / 1024));
    WHBLogPrintf("[pak]   open %.2f ms | header(1st read) %.2f ms | "
                 "rest %.2f ms in 1 call (%.1f MB/s) | total %.2f ms",
                 openMs, hdrMs, readMs,
                 readMs > 0.0 ? (double)rest / 1000.0 / readMs : 0.0,
                 openMs + hdrMs + readMs);
    return true;
}

const void *CremaPakFind(const CremaPak *pak, const char *name, size_t *outSize)
{
    if (!pak || !pak->blob || !name)
        return NULL;
    for (uint32_t i = 0; i < pak->count; i++) {
        const CremaPakEntry *e = &pak->entries[i];
        // The names are fixed-width and zero-padded in the file, so a bounded
        // compare is both the correct one and the one that cannot run off.
        if (strncmp(e->name, name, sizeof(e->name)) != 0)
            continue;
        if (outSize)
            *outSize = e->size;
        return pak->blob + (e->offset - CPAK_HEADER_SIZE);
    }
    WHBLogPrintf("[pak] no entry named %s", name);
    return NULL;
}

void CremaPakClose(CremaPak *pak)
{
    if (!pak)
        return;
    free(pak->blob);
    memset(pak, 0, sizeof(*pak));
}
