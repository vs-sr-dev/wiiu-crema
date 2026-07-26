// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan

#include "crema_bank.h"

#include <malloc.h>
#include <math.h>
#include <string.h>
#include <whb/log.h>

#define CBANK_VERSION 3
#define CBANK_HEADER_SIZE 32
#define CBANK_FLAG_LOOPING 1
#define CBANK_FORMAT_LPCM16 0
#define CBANK_FORMAT_ADPCM  1

// A frame of ADPCM holds fourteen samples in eight bytes. The bank stores a
// count of SAMPLES for both formats, so this is the only place the two differ.
#define CBANK_ADPCM_FRAME_SAMPLES 14
#define CBANK_ADPCM_FRAME_BYTES   8

// Mirrors tools/crema_bake.py. Big-endian file, big-endian CPU, and the samples
// themselves are big-endian too — which is the order AX wants, so the file's
// own bytes are handed to a voice with nothing done to them.
typedef struct {
    char     magic[4];
    uint32_t version;
    uint32_t count;
    uint32_t rate;
    uint32_t reserved[4];
} BankHeader;
_Static_assert(sizeof(BankHeader) == CBANK_HEADER_SIZE, "cbank header is 32 B");

typedef struct {
    char     name[24];
    uint32_t offset;
    uint32_t sampleCount;
    uint32_t loopStart;
    uint32_t cycleSamples;
    uint32_t flags;
    // v2: the shape of a note. Sixteen bytes that do more for how this sounds
    // than every sample in the file put together.
    uint16_t attackMs, decayMs, sustain, releaseMs;
    uint16_t vibDelayMs, vibRateMilliHz;
    int16_t  vibDepthCents;
    // v3: what v2 called `reserved`, and 0 still means the LPCM16 it meant then.
    uint16_t format;
    // v3: the decoder state, all zero unless format says ADPCM. Forty-four bytes
    // per instrument whether or not it uses them, which is the trade a flat
    // directory makes and is worth 44 bytes: the alternative is a payload with a
    // variable-length prefix, i.e. parsing, at load time, on the console.
    int16_t  coefficients[16];
    uint16_t predScale;
    int16_t  yn1, yn2;
    uint16_t loopPredScale;
    int16_t  loopYn1, loopYn2;
} BankEntry;
_Static_assert(sizeof(BankEntry) == 104, "cbank entry is 104 bytes");

bool CremaBankLoadFromMemory(CremaBank *bank, const void *blob, size_t size)
{
    memset(bank, 0, sizeof(*bank));
    if (!blob || size < sizeof(BankHeader)) {
        WHBLogPrintf("[bank] too small to be a .cbank");
        return false;
    }

    const uint8_t *base = (const uint8_t *)blob;
    BankHeader h;
    memcpy(&h, base, sizeof(h));
    if (memcmp(h.magic, "CBNK", 4) != 0 || h.version != CBANK_VERSION ||
        h.count == 0) {
        WHBLogPrintf("[bank] not a v%d .cbank", CBANK_VERSION);
        return false;
    }
    if (sizeof(BankHeader) + (size_t)h.count * sizeof(BankEntry) > size) {
        WHBLogPrintf("[bank] directory of %u runs past the blob", h.count);
        return false;
    }

    bank->items = (CremaInstrument *)calloc(h.count, sizeof(CremaInstrument));
    if (!bank->items)
        return false;
    bank->rate = h.rate;

    uint32_t loaded = 0;
    size_t totalSamples = 0, totalBytes = 0;
    uint32_t compressed = 0;
    for (uint32_t i = 0; i < h.count; i++) {
        BankEntry e;
        memcpy(&e, base + sizeof(BankHeader) + i * sizeof(BankEntry), sizeof(e));
        bool adpcm = e.format == CBANK_FORMAT_ADPCM;
        size_t bytes = adpcm
            ? ((size_t)e.sampleCount + CBANK_ADPCM_FRAME_SAMPLES - 1) /
              CBANK_ADPCM_FRAME_SAMPLES * CBANK_ADPCM_FRAME_BYTES
            : (size_t)e.sampleCount * sizeof(int16_t);
        if (e.offset < sizeof(BankHeader) ||
            (size_t)(e.offset) + bytes > size) {
            WHBLogPrintf("[bank] %s points outside the blob", e.name);
            break;
        }

        CremaInstrument *inst = &bank->items[loaded];
        memcpy(inst->name, e.name, sizeof(inst->name));
        inst->name[sizeof(inst->name) - 1] = '\0';
        inst->cycleSamples = e.cycleSamples;
        inst->env.attackMs   = e.attackMs;
        inst->env.decayMs    = e.decayMs;
        inst->env.sustain    = e.sustain;
        inst->env.releaseMs  = e.releaseMs;
        inst->vib.delayMs     = e.vibDelayMs;
        inst->vib.rateMilliHz = e.vibRateMilliHz;
        inst->vib.depthCents  = e.vibDepthCents;

        // Copied, not referenced: the DSP reads these samples while the voice
        // plays, long after whatever we were handed has been freed.
        const void *payload = base + e.offset;
        bool looping = (e.flags & CBANK_FLAG_LOOPING) != 0;
        bool ok;
        if (adpcm) {
            CremaAdpcm info;
            memcpy(info.coefficients, e.coefficients, sizeof(info.coefficients));
            info.predScale     = e.predScale;
            info.yn1           = e.yn1;
            info.yn2           = e.yn2;
            info.loopPredScale = e.loopPredScale;
            info.loopYn1       = e.loopYn1;
            info.loopYn2       = e.loopYn2;
            ok = CremaSoundCreateAdpcm(&inst->sound, payload, bytes,
                                       e.sampleCount, h.rate, e.loopStart,
                                       looping, &info);
            compressed++;
        } else if (looping) {
            ok = CremaSoundCreateLooping(&inst->sound, (const int16_t *)payload,
                                         e.sampleCount, h.rate, e.loopStart);
        } else {
            ok = CremaSoundCreate(&inst->sound, (const int16_t *)payload,
                                  e.sampleCount, h.rate);
        }
        if (!ok) {
            WHBLogPrintf("[bank] %s: out of memory", inst->name);
            break;
        }
        totalSamples += e.sampleCount;
        totalBytes += bytes;
        loaded++;
    }

    bank->count = loaded;
    if (loaded == 0) {
        CremaBankClose(bank);
        return false;
    }
    // Both numbers, because the interesting one is the difference: what the
    // instruments would have cost as PCM against what they actually occupy.
    WHBLogPrintf("[bank] %u instruments at %u Hz, %u KB resident "
                 "(%u KB as PCM16), %u compressed",
                 loaded, h.rate, (uint32_t)(totalBytes / 1024),
                 (uint32_t)(totalSamples * 2 / 1024), compressed);
    return true;
}

const CremaInstrument *CremaBankFind(const CremaBank *bank, const char *name)
{
    if (!bank || !name)
        return NULL;
    for (uint32_t i = 0; i < bank->count; i++)
        if (strncmp(bank->items[i].name, name, sizeof(bank->items[i].name)) == 0)
            return &bank->items[i];
    WHBLogPrintf("[bank] no instrument named %s", name);
    return NULL;
}

void CremaBankClose(CremaBank *bank)
{
    if (!bank || !bank->items)
        return;
    for (uint32_t i = 0; i < bank->count; i++)
        CremaSoundDestroy(&bank->items[i].sound);
    free(bank->items);
    memset(bank, 0, sizeof(*bank));
}

float CremaInstrumentPitch(const CremaInstrument *inst, float note)
{
    if (!inst)
        return 1.0f;
    if (inst->cycleSamples == 0 || inst->sound.rate == 0) {
        // Not pitched — a drum, a noise loop. There is no cycle to tune, so
        // the note transposes the sample itself, with 60 meaning "as recorded".
        // Every sampler ever built does this, and it is what lets a noise
        // channel have a low hit and a high one from the same four kilobytes.
        return powf(2.0f, (note - 60.0f) / 12.0f);
    }
    // 69 is A4. The twelfth root of two, and then the only step that matters:
    // the frequency a cycle of N samples has to be read at to sound at that
    // pitch is f * N, and everything else in this file is bookkeeping.
    float freq = 440.0f * powf(2.0f, (note - 69.0f) / 12.0f);
    return freq * (float)inst->cycleSamples / (float)inst->sound.rate;
}
