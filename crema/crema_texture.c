// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan

#include "crema_texture.h"

#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/state.h>
#include <gx2/surface.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <whb/log.h>

#define BYTES_PER_PIXEL 4

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

bool CremaTextureCreate(GX2Texture *tex, uint32_t width, uint32_t height,
                        uint32_t mipLevels, GX2TileMode tileMode)
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
        memset(tex->surface.mipmaps, 0, tex->surface.mipmapSize);
        GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE,
                      tex->surface.mipmaps, tex->surface.mipmapSize);
    }
    return true;
}

void CremaTextureDestroy(GX2Texture *tex)
{
    free(tex->surface.image);
    free(tex->surface.mipmaps);
    tex->surface.image   = NULL;
    tex->surface.mipmaps = NULL;
}

bool CremaTextureUploadLevel(GX2Texture *tex, uint32_t level, const void *rgba8)
{
    uint32_t w = levelSize(tex->surface.width, level);
    uint32_t h = levelSize(tex->surface.height, level);

    // Linear staging copy: the CPU can only write a row-major image, the GPU
    // does the swizzling for us in GX2CopySurface.
    GX2Texture staging;
    if (!CremaTextureCreate(&staging, w, h, 1, GX2_TILE_MODE_LINEAR_ALIGNED))
        return false;

    const uint8_t *src = (const uint8_t *)rgba8;
    uint8_t *dst = (uint8_t *)staging.surface.image;
    for (uint32_t y = 0; y < h; y++)
        memcpy(dst + (size_t)y * staging.surface.pitch * BYTES_PER_PIXEL,
               src + (size_t)y * w * BYTES_PER_PIXEL,
               (size_t)w * BYTES_PER_PIXEL);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE,
                  staging.surface.image, staging.surface.imageSize);

    GX2CopySurface(&staging.surface, 0, 0, &tex->surface, level, 0);
    GX2Flush();
    GX2DrawDone();   // staging dies at the end of this call: let the copy land
    CremaTextureDestroy(&staging);
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
    if (!CremaTextureUploadLevel(tex, 0, rgba8))
        return false;

    uint32_t levels = tex->surface.mipLevels;
    if (levels <= 1)
        return true;

    uint32_t sw = tex->surface.width;
    uint32_t sh = tex->surface.height;
    uint8_t *src = (uint8_t *)malloc((size_t)sw * sh * BYTES_PER_PIXEL);
    if (!src)
        return false;
    memcpy(src, rgba8, (size_t)sw * sh * BYTES_PER_PIXEL);

    bool ok = true;
    for (uint32_t l = 1; l < levels && ok; l++) {
        uint32_t dw = levelSize(tex->surface.width, l);
        uint32_t dh = levelSize(tex->surface.height, l);
        uint8_t *dst = (uint8_t *)malloc((size_t)dw * dh * BYTES_PER_PIXEL);
        if (!dst) {
            ok = false;
            break;
        }
        boxReduce(src, sw, sh, dst);
        ok = CremaTextureUploadLevel(tex, l, dst);
        free(src);
        src = dst;
        sw = dw;
        sh = dh;
    }
    free(src);
    if (ok)
        WHBLogPrintf("[texture] %ux%u uploaded, %u mip levels",
                     tex->surface.width, tex->surface.height, levels);
    return ok;
}

void CremaSamplerInitTrilinear(GX2Sampler *sampler, GX2TexClampMode clampMode)
{
    GX2InitSampler(sampler, clampMode, GX2_TEX_XY_FILTER_MODE_LINEAR);
    GX2InitSamplerZMFilter(sampler, GX2_TEX_Z_FILTER_MODE_NONE,
                           GX2_TEX_MIP_FILTER_MODE_LINEAR);
    GX2InitSamplerLOD(sampler, 0.0f, 13.0f, 0.0f);   // 13 == any chain up to 8192
}
