// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan
//
// Timed billboard effects: muzzle flashes, tracers, explosions.
//
// What separates an effect from an entity is that an effect knows it is going
// to die. It carries an age and a lifetime, interpolates size and fade between
// them, and removes itself — nothing in the game has to remember it exists.
//
// The pool packs into an instance array of two vec4s per effect, which is what
// a billboard vertex shader wants:
//     slot 0: xyz = world position, w = size
//     slot 1: rgb = colour,         w = alpha

#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "crema_matrix.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    Vec3  pos;
    Vec3  velocity;
    float age, life;
    float sizeStart, sizeEnd;
    float r, g, b;
    float alphaStart;
    bool  active;
} CremaEffect;

typedef struct {
    CremaEffect *items;
    uint32_t     capacity;
    uint32_t     watermark;
} CremaEffectPool;

void CremaEffectPoolInit(CremaEffectPool *pool, CremaEffect *storage,
                         uint32_t capacity);

// Returns NULL when full. Effects are the one thing where dropping a spawn is
// fine — nobody misses the sixty-fourth spark — so callers may ignore it.
CremaEffect *CremaEffectSpawn(CremaEffectPool *pool, Vec3 pos, float life,
                              float sizeStart, float sizeEnd,
                              float r, float g, float b, float alpha);

void CremaEffectUpdate(CremaEffectPool *pool, float dt);

// Fill `out` with 2 vec4s per live effect and return how many were written.
// Sizes and alpha are already interpolated for this frame.
uint32_t CremaEffectPack(const CremaEffectPool *pool, float (*out)[4],
                         uint32_t maxEffects);

// --- drawing them ------------------------------------------------------------
//
// A camera-facing quad per effect, additively blended, with the blob computed
// from the corner coordinate — no sprite sheet, no texture fetch, and it never
// shows its square edges. Two examples wrote this shader identically before it
// moved here.
//
// The renderer asks for exactly three numbers rather than reading an
// application's Global block, and that is the whole design of it. PoC 11's
// Global has ten fields and PoC 12's has five; a module that wanted "the Global
// block" would be a module that dictates the uniform layout of every game built
// on it. So it declares its own, the caller fills it, and the two never have to
// agree about anything else.

#include <gx2/sampler.h>
#include <gx2r/buffer.h>

#include "crema_shader.h"

// Effects drawn in one call. The pool may hold more; pack no more than this.
#define CREMA_EFFECT_MAX_DRAWN 64
#define CREMA_EFFECT_BLOCK_BYTES (sizeof(float) * 4 * 2 * CREMA_EFFECT_MAX_DRAWN)

typedef struct {
    Mat4  viewProj;
    float camRight[4];   // the camera's basis, so a flat quad can face it
    float camUp[4];
} CremaEffectView;

typedef struct {
    CremaShader *shader;
    GX2RBuffer   quad;        // the unit square measured from its centre
    GX2RBuffer   indices;
    int32_t      viewLoc, fxLoc;
} CremaEffectRenderer;

bool CremaEffectRendererCreate(CremaEffectRenderer *r);
void CremaEffectRendererDestroy(CremaEffectRenderer *r);

// Additive, depth tested but not depth written — overlapping billboards must
// not carve holes in each other — and culling off, because a camera-facing quad
// has no reliable winding. Both uniform blocks are the caller's storage.
void CremaEffectDraw(const CremaEffectRenderer *r, const void *viewUbo,
                     const void *fxUbo, uint32_t count);

#ifdef __cplusplus
}
#endif
