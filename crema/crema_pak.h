// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan
//
// One archive, two reads, everything resident.
//
// This module exists because of a measurement, not a preference. On real
// hardware under Aroma, with the filesystem a round trip to IOSU:
//
//     fopen                       ~0.15 ms
//     the FIRST read on a stream  ~3-4 ms, whatever its size
//     every read after that       ~0.74 ms fixed
//     the bytes themselves        15-18 MB/s
//
// Sixty-four bytes cost 3.09 ms because that is the read that makes stdio set
// the stream up. So the cost of an asset is dominated by *touching its file at
// all*, and twenty assets in twenty files would burn ~80 ms before reading
// anything useful.
//
// A .cpak is therefore not a compression format and does no transformation: it
// is a directory followed by a concatenation. Opening one costs two reads no
// matter how many assets are inside — the header says how long the rest is, and
// the rest arrives in a single call.
//
// The trade it makes is honest and worth stating: the whole archive is resident
// in memory until you close it. That is the right shape for a level's worth of
// assets, which you load, hand to the GPU, and drop. It is the wrong shape for
// a hundred megabytes of streaming audio, and when that day comes this module
// grows a seek path rather than pretending it always had one.

#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char     name[32];
    uint32_t offset;      // from the start of the file
    uint32_t size;
} CremaPakEntry;

typedef struct {
    uint8_t             *blob;       // directory + every payload, one block
    size_t               blobSize;
    const CremaPakEntry *entries;    // points into blob; nothing is copied
    uint32_t             count;
} CremaPak;

// Open, read, close. The file handle does not outlive the call: there is
// nothing left to read from it.
bool CremaPakOpen(CremaPak *pak, const char *path);

// Returns a pointer into the archive's memory — no copy, no allocation. NULL
// if there is no such entry. `outSize` may be NULL.
const void *CremaPakFind(const CremaPak *pak, const char *name, size_t *outSize);

// Frees the block. Every pointer handed out by CremaPakFind dies with it, so
// close only once the assets have been handed to the GPU.
void CremaPakClose(CremaPak *pak);

#ifdef __cplusplus
}
#endif
