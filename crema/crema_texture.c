// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan

#include "crema_texture.h"

#include <coreinit/time.h>
#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/state.h>
#include <gx2/surface.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <whb/log.h>

#define BYTES_PER_PIXEL 4
#define CREMA_MAX_MIP_LEVELS 14   // a full chain from 8192 down to 1

static uint32_t levelSize(uint32_t base, uint32_t level)
{
    uint32_t v = base >> level;
    return v ? v : 1;
}

uint32_t CremaTextureMipLevels(uint32_t width, uint32_t height)
{
    uint32_t levels = 1;
    while (width > 1 || height > 1) {
        width  = width  > 1 ? width  >> 1 : 1;
        height = height > 1 ? height >> 1 : 1;
        levels++;
    }
    return levels;
}

// `clear` zeroes the surface. Skip it when every byte is about to be
// overwritten — but never skip the GX2Invalidate that follows: the allocation
// may carry dirty cache lines from whatever owned that memory before, and
// those would flush over the GPU's work later. That is the PoC 7 bug, and it
// only ever shows up on hardware.
static bool createSurface(GX2Texture *tex, uint32_t width, uint32_t height,
                          uint32_t mipLevels, GX2TileMode tileMode, bool clear)
{
    memset(tex, 0, sizeof(*tex));
    tex->surface.width     = width;
    tex->surface.height    = height;
    tex->surface.depth     = 1;
    tex->surface.mipLevels = mipLevels;
    tex->surface.format    = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    tex->surface.aa        = GX2_AA_MODE1X;
    tex->surface.use       = GX2_SURFACE_USE_TEXTURE;
    tex->surface.dim       = GX2_SURFACE_DIM_TEXTURE_2D;
    tex->surface.tileMode  = tileMode;
    tex->viewNumMips   = mipLevels;
    tex->viewNumSlices = 1;
    tex->compMap       = 0x00010203;   // R,G,B,A straight through
    GX2CalcSurfaceSizeAndAlignment(&tex->surface);
    GX2InitTextureRegs(tex);

    tex->surface.image = memalign(tex->surface.alignment, tex->surface.imageSize);
    if (!tex->surface.image) {
        WHBLogPrintf("[texture] out of memory: %ux%u image (%u bytes)",
                     width, height, tex->surface.imageSize);
        return false;
    }
    if (clear)
        memset(tex->surface.image, 0, tex->surface.imageSize);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE,
                  tex->surface.image, tex->surface.imageSize);

    // Levels 1..n-1 live in a separate allocation, sized by the surface calc.
    if (tex->surface.mipmapSize > 0) {
        tex->surface.mipmaps = memalign(tex->surface.alignment,
                                        tex->surface.mipmapSize);
        if (!tex->surface.mipmaps) {
            WHBLogPrintf("[texture] out of memory: mip chain (%u bytes)",
                         tex->surface.mipmapSize);
            free(tex->surface.image);
            tex->surface.image = NULL;
            return false;
        }
        if (clear)
            memset(tex->surface.mipmaps, 0, tex->surface.mipmapSize);
        GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE,
                      tex->surface.mipmaps, tex->surface.mipmapSize);
    }
    return true;
}

bool CremaTextureCreate(GX2Texture *tex, uint32_t width, uint32_t height,
                        uint32_t mipLevels, GX2TileMode tileMode)
{
    return createSurface(tex, width, height, mipLevels, tileMode, true);
}

void CremaTextureDestroy(GX2Texture *tex)
{
    free(tex->surface.image);
    free(tex->surface.mipmaps);
    tex->surface.image   = NULL;
    tex->surface.mipmaps = NULL;
}

// Queue one level's copy without waiting for it. The staging surface must stay
// alive until the caller syncs, which is the whole point: a chain uploaded this
// way costs ONE GX2DrawDone instead of one per level. Measured on hardware, the
// per-level sync was most of the cost of loading a mipped texture.
static bool stageLevel(GX2Texture *tex, uint32_t level, const void *rgba8,
                       GX2Texture *staging)
{
    uint32_t w = levelSize(tex->surface.width, level);
    uint32_t h = levelSize(tex->surface.height, level);

    // Linear staging copy: the CPU can only write a row-major image, the GPU
    // does the swizzling for us in GX2CopySurface. No need to zero it — every
    // row is written below, and the pitch padding is never sampled.
    if (!createSurface(staging, w, h, 1, GX2_TILE_MODE_LINEAR_ALIGNED, false))
        return false;

    const uint8_t *src = (const uint8_t *)rgba8;
    uint8_t *dst = (uint8_t *)staging->surface.image;
    for (uint32_t y = 0; y < h; y++)
        memcpy(dst + (size_t)y * staging->surface.pitch * BYTES_PER_PIXEL,
               src + (size_t)y * w * BYTES_PER_PIXEL,
               (size_t)w * BYTES_PER_PIXEL);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE,
                  staging->surface.image, staging->surface.imageSize);

    GX2CopySurface(&staging->surface, 0, 0, &tex->surface, level, 0);
    return true;
}

// Let every queued copy land, then release the staging surfaces.
static void settleStaging(GX2Texture *staging, uint32_t count)
{
    GX2Flush();
    GX2DrawDone();
    for (uint32_t i = 0; i < count; i++)
        CremaTextureDestroy(&staging[i]);
}

bool CremaTextureUploadLevel(GX2Texture *tex, uint32_t level, const void *rgba8)
{
    GX2Texture staging;
    if (!stageLevel(tex, level, rgba8, &staging))
        return false;
    settleStaging(&staging, 1);
    return true;
}

// 2x2 box filter, tightly packed RGBA8. Taps are clamped so non-square and
// already-degenerate (1-pixel) dimensions keep working down to 1x1.
static void boxReduce(const uint8_t *src, uint32_t sw, uint32_t sh, uint8_t *dst)
{
    uint32_t dw = sw > 1 ? sw >> 1 : 1;
    uint32_t dh = sh > 1 ? sh >> 1 : 1;
    for (uint32_t y = 0; y < dh; y++) {
        uint32_t y0 = 2 * y, y1 = y0 + 1;
        if (y0 >= sh) y0 = sh - 1;
        if (y1 >= sh) y1 = sh - 1;
        for (uint32_t x = 0; x < dw; x++) {
            uint32_t x0 = 2 * x, x1 = x0 + 1;
            if (x0 >= sw) x0 = sw - 1;
            if (x1 >= sw) x1 = sw - 1;
            for (uint32_t c = 0; c < BYTES_PER_PIXEL; c++) {
                uint32_t sum =
                    src[(y0 * sw + x0) * BYTES_PER_PIXEL + c] +
                    src[(y0 * sw + x1) * BYTES_PER_PIXEL + c] +
                    src[(y1 * sw + x0) * BYTES_PER_PIXEL + c] +
                    src[(y1 * sw + x1) * BYTES_PER_PIXEL + c];
                dst[(y * dw + x) * BYTES_PER_PIXEL + c] = (uint8_t)(sum / 4);
            }
        }
    }
}

bool CremaTextureUploadWithMips(GX2Texture *tex, const void *rgba8)
{
    uint32_t levels = tex->surface.mipLevels;
    if (levels > CREMA_MAX_MIP_LEVELS)
        levels = CREMA_MAX_MIP_LEVELS;

    // Every level's staging surface stays alive until the single sync below.
    GX2Texture staging[CREMA_MAX_MIP_LEVELS];
    uint32_t staged = 0;

    bool ok = stageLevel(tex, 0, rgba8, &staging[staged]);
    if (ok)
        staged++;

    uint32_t sw = tex->surface.width;
    uint32_t sh = tex->surface.height;
    uint8_t *src = NULL;
    if (ok && levels > 1) {
        src = (uint8_t *)malloc((size_t)sw * sh * BYTES_PER_PIXEL);
        ok = src != NULL;
        if (ok)
            memcpy(src, rgba8, (size_t)sw * sh * BYTES_PER_PIXEL);
    }

    for (uint32_t l = 1; l < levels && ok; l++) {
        uint32_t dw = levelSize(tex->surface.width, l);
        uint32_t dh = levelSize(tex->surface.height, l);
        uint8_t *dst = (uint8_t *)malloc((size_t)dw * dh * BYTES_PER_PIXEL);
        if (!dst) {
            ok = false;
            break;
        }
        boxReduce(src, sw, sh, dst);
        ok = stageLevel(tex, l, dst, &staging[staged]);
        if (ok)
            staged++;
        free(src);
        src = dst;
        sw = dw;
        sh = dh;
    }
    free(src);

    settleStaging(staging, staged);
    if (ok)
        WHBLogPrintf("[texture] %ux%u uploaded, %u mip levels",
                     tex->surface.width, tex->surface.height, levels);
    return ok;
}

// --- baked textures ----------------------------------------------------------

// Mirrors tools/crema_bake.py. File and CPU are both big-endian: fread is enough.
typedef struct {
    char     magic[4];
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t mipLevels;
    uint32_t format;      // 1 = RGBA8
    uint32_t dataOffset;
    uint32_t reserved;
} TexHeader;
_Static_assert(sizeof(TexHeader) == 32, "ctex header must stay 32 bytes");

#define CTEX_VERSION 1
#define CTEX_FORMAT_RGBA8 1

bool CremaTextureLoad(GX2Texture *tex, const char *path)
{
    memset(tex, 0, sizeof(*tex));

    uint64_t openStart = OSGetSystemTime();
    FILE *fh = fopen(path, "rb");
    if (!fh) {
        WHBLogPrintf("[texture] cannot open %s", path);
        return false;
    }

    TexHeader h;
    bool ok = fread(&h, 1, sizeof(h), fh) == sizeof(h);
    uint64_t openTicks = OSGetSystemTime() - openStart;   // open + header read
    if (ok && (memcmp(h.magic, "CTEX", 4) != 0 || h.version != CTEX_VERSION ||
               h.format != CTEX_FORMAT_RGBA8)) {
        WHBLogPrintf("[texture] %s: not a v%d RGBA8 .ctex", path, CTEX_VERSION);
        ok = false;
    }
    // Every level is uploaded below, so there is nothing to gain from zeroing
    // half a megabyte first.
    uint64_t createStart = OSGetSystemTime();
    if (ok)
        ok = createSurface(tex, h.width, h.height, h.mipLevels,
                           GX2_TILE_MODE_DEFAULT, false);
    uint64_t createTicks = OSGetSystemTime() - createStart;
    if (ok && fseek(fh, (long)h.dataOffset, SEEK_SET) != 0)
        ok = false;

    uint32_t levels = h.mipLevels;
    if (levels > CREMA_MAX_MIP_LEVELS)
        levels = CREMA_MAX_MIP_LEVELS;

    // Levels are already box-filtered offline, so this is pure I/O + upload.
    // Timed apart, because "how long does an asset take to load" is a useless
    // number if you cannot tell reading from uploading.
    //
    // One read for the whole chain, not one per level: on hardware a file read
    // costs about 1.1 ms whatever its size, so nine reads spend most of their
    // time in fixed overhead. The tail of a mip chain is tiny — the last six
    // levels of a 256x256 texture are 1.4 KB together, and paying a millisecond
    // each for them is absurd.
    GX2Texture staging[CREMA_MAX_MIP_LEVELS];
    uint32_t staged = 0;
    uint64_t readTicks = 0, stageTicks = 0;
    size_t readBytes = 0;
    uint32_t readCalls = 0;
    for (uint32_t level = 0; level < levels; level++)
        readBytes += (size_t)levelSize(h.width, level) *
                     levelSize(h.height, level) * BYTES_PER_PIXEL;

    uint8_t *blob = ok ? (uint8_t *)malloc(readBytes) : NULL;
    if (ok && !blob) {
        // A full chain is ~4/3 of the top level; if that will not fit, fall
        // back to reading level by level and pay the per-call cost.
        WHBLogPrintf("[texture] %s: %u KB blob did not fit, reading per level",
                     path, (uint32_t)(readBytes / 1024));
    }

    if (ok && blob) {
        uint64_t t0 = OSGetSystemTime();
        ok = fread(blob, 1, readBytes, fh) == readBytes;
        readTicks = OSGetSystemTime() - t0;
        readCalls = 1;

        size_t offset = 0;
        for (uint32_t level = 0; level < levels && ok; level++) {
            uint64_t t1 = OSGetSystemTime();
            ok = stageLevel(tex, level, blob + offset, &staging[staged]);
            if (ok)
                staged++;
            stageTicks += OSGetSystemTime() - t1;
            offset += (size_t)levelSize(h.width, level) *
                      levelSize(h.height, level) * BYTES_PER_PIXEL;
        }
        free(blob);
    } else if (ok) {
        for (uint32_t level = 0; level < levels && ok; level++) {
            uint32_t w = levelSize(h.width, level);
            uint32_t d = levelSize(h.height, level);
            size_t bytes = (size_t)w * d * BYTES_PER_PIXEL;
            uint8_t *scratch = (uint8_t *)malloc(bytes);
            if (!scratch) {
                ok = false;
                break;
            }
            uint64_t t0 = OSGetSystemTime();
            ok = fread(scratch, 1, bytes, fh) == bytes;
            uint64_t t1 = OSGetSystemTime();
            readTicks += t1 - t0;
            readCalls++;
            if (ok) {
                ok = stageLevel(tex, level, scratch, &staging[staged]);
                if (ok)
                    staged++;
                stageTicks += OSGetSystemTime() - t1;
            }
            free(scratch);
        }
    }
    fclose(fh);

    uint64_t syncStart = OSGetSystemTime();
    settleStaging(staging, staged);
    uint64_t syncTicks = OSGetSystemTime() - syncStart;

    if (!ok) {
        WHBLogPrintf("[texture] %s: truncated or unreadable", path);
        CremaTextureDestroy(tex);
        return false;
    }

    double readMs  = (double)OSTicksToMicroseconds(readTicks) / 1000.0;
    double stageMs = (double)OSTicksToMicroseconds(stageTicks) / 1000.0;
    double syncMs  = (double)OSTicksToMicroseconds(syncTicks) / 1000.0;
    WHBLogPrintf("[texture] %s: %ux%u, %u levels, %u KB", path, h.width,
                 h.height, levels, (uint32_t)(readBytes / 1024));
    double openMs   = (double)OSTicksToMicroseconds(openTicks) / 1000.0;
    double createMs = (double)OSTicksToMicroseconds(createTicks) / 1000.0;
    WHBLogPrintf("[texture]   open+hdr %.2f ms | create %.2f ms | read %.2f ms in %u call(s) (%.1f MB/s)",
                 openMs, createMs, readMs, readCalls,
                 readMs > 0.0 ? (double)readBytes / 1000.0 / readMs : 0.0);
    WHBLogPrintf("[texture]   stage %.2f ms | sync %.2f ms | accounted %.2f ms",
                 stageMs, syncMs,
                 openMs + createMs + readMs + stageMs + syncMs);
    return true;
}

void CremaSamplerInitTrilinear(GX2Sampler *sampler, GX2TexClampMode clampMode)
{
    GX2InitSampler(sampler, clampMode, GX2_TEX_XY_FILTER_MODE_LINEAR);
    GX2InitSamplerZMFilter(sampler, GX2_TEX_Z_FILTER_MODE_NONE,
                           GX2_TEX_MIP_FILTER_MODE_LINEAR);
    GX2InitSamplerLOD(sampler, 0.0f, 13.0f, 0.0f);   // 13 == any chain up to 8192
}

void CremaSamplerInitBilinear(GX2Sampler *sampler, GX2TexClampMode clampMode)
{
    GX2InitSampler(sampler, clampMode, GX2_TEX_XY_FILTER_MODE_LINEAR);
    GX2InitSamplerZMFilter(sampler, GX2_TEX_Z_FILTER_MODE_NONE,
                           GX2_TEX_MIP_FILTER_MODE_NONE);
    GX2InitSamplerLOD(sampler, 0.0f, 0.0f, 0.0f);   // level 0 and nothing else
}

// The same .ctex, already in memory — which is what an archive hands you. All
// that is left is the part that was never I/O: build the surface, push each
// level through the staging copy, wait once for the GPU at the end.
bool CremaTextureLoadFromMemory(GX2Texture *tex, const void *blob, size_t size,
                                const char *label)
{
    memset(tex, 0, sizeof(*tex));
    if (!blob || size < sizeof(TexHeader)) {
        WHBLogPrintf("[texture] %s: too small to be a .ctex", label);
        return false;
    }

    const uint8_t *base = (const uint8_t *)blob;
    TexHeader h;
    memcpy(&h, base, sizeof(h));
    if (memcmp(h.magic, "CTEX", 4) != 0 || h.version != CTEX_VERSION ||
        h.format != CTEX_FORMAT_RGBA8) {
        WHBLogPrintf("[texture] %s: not a v%d RGBA8 .ctex", label, CTEX_VERSION);
        return false;
    }

    uint32_t levels = h.mipLevels;
    if (levels > CREMA_MAX_MIP_LEVELS)
        levels = CREMA_MAX_MIP_LEVELS;

    size_t needed = h.dataOffset;
    for (uint32_t level = 0; level < levels; level++)
        needed += (size_t)levelSize(h.width, level) *
                  levelSize(h.height, level) * BYTES_PER_PIXEL;
    if (needed > size) {
        WHBLogPrintf("[texture] %s: header wants %u B, the entry holds %u",
                     label, (uint32_t)needed, (uint32_t)size);
        return false;
    }

    uint64_t createStart = OSGetSystemTime();
    bool ok = createSurface(tex, h.width, h.height, levels,
                            GX2_TILE_MODE_DEFAULT, false);
    uint64_t createTicks = OSGetSystemTime() - createStart;

    GX2Texture staging[CREMA_MAX_MIP_LEVELS];
    uint32_t staged = 0;
    uint64_t stageStart = OSGetSystemTime();
    size_t offset = h.dataOffset;
    for (uint32_t level = 0; level < levels && ok; level++) {
        ok = stageLevel(tex, level, base + offset, &staging[staged]);
        if (ok)
            staged++;
        offset += (size_t)levelSize(h.width, level) *
                  levelSize(h.height, level) * BYTES_PER_PIXEL;
    }
    uint64_t stageTicks = OSGetSystemTime() - stageStart;

    uint64_t syncStart = OSGetSystemTime();
    settleStaging(staging, staged);
    uint64_t syncTicks = OSGetSystemTime() - syncStart;

    if (!ok) {
        WHBLogPrintf("[texture] %s: upload failed", label);
        CremaTextureDestroy(tex);
        return false;
    }

    WHBLogPrintf("[texture] %s: %ux%u, %u levels, %u KB (from memory)",
                 label, h.width, h.height, levels, (uint32_t)(size / 1024));
    WHBLogPrintf("[texture]   create %.2f ms | stage %.2f ms | sync %.2f ms",
                 (double)OSTicksToMicroseconds(createTicks) / 1000.0,
                 (double)OSTicksToMicroseconds(stageTicks) / 1000.0,
                 (double)OSTicksToMicroseconds(syncTicks) / 1000.0);
    return true;
}
