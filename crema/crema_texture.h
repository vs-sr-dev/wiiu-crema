// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan
//
// RGBA8 texture creation, mip-chain generation and upload.
//
// Two hardware lessons live in here, both invisible under Cemu:
//   - mipmaps are not optional. A minified no-mip ground texture thrashes the
//     texture cache badly enough to halve the frame rate (measured 60 -> 30 fps
//     just by looking down), so the default path builds the full chain.
//   - every CPU write to memory the GPU will read must be followed by
//     GX2Invalidate. Cache lines that flush *after* a GX2CopySurface overwrite
//     the GPU's output with stale bytes — random black blocks, hardware only.

#pragma once
#include <gx2/sampler.h>
#include <gx2/texture.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Number of levels in a full chain down to 1x1.
uint32_t CremaTextureMipLevels(uint32_t width, uint32_t height);

// Allocate an RGBA8 texture (image + mipmap memory, zeroed and invalidated).
// `tileMode` is normally GX2_TILE_MODE_DEFAULT — GPU-swizzled; use
// GX2_TILE_MODE_LINEAR_ALIGNED only for staging or CPU-visible surfaces.
bool CremaTextureCreate(GX2Texture *tex, uint32_t width, uint32_t height,
                        uint32_t mipLevels, GX2TileMode tileMode);

// Upload one mip level from tightly packed RGBA8 (width>>level x height>>level).
// Goes through a linear staging surface + GX2CopySurface, so it works whatever
// the destination tile mode is. Synchronous (GX2DrawDone): a load-time call.
bool CremaTextureUploadLevel(GX2Texture *tex, uint32_t level, const void *rgba8);

// Upload level 0 and box-filter the rest of the chain from it.
bool CremaTextureUploadWithMips(GX2Texture *tex, const void *rgba8);

// Load a baked .ctex (tools/crema_bake.py texture ...): creates the surface
// and uploads every mip level, all of them filtered offline. Paths inside a
// .wuhb live under /vol/content/.
bool CremaTextureLoad(GX2Texture *tex, const char *path);

void CremaTextureDestroy(GX2Texture *tex);

// Bilinear within a level + linear between levels — what a mip chain is for.
void CremaSamplerInitTrilinear(GX2Sampler *sampler, GX2TexClampMode clampMode);

// Bilinear, no mip filtering: for a texture with a single level, which is
// what a HUD font atlas is. Ask for mips it does not have and the sampler
// wanders off the end of the chain.
void CremaSamplerInitBilinear(GX2Sampler *sampler, GX2TexClampMode clampMode);

#ifdef __cplusplus
}
#endif
