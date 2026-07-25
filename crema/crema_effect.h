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

#ifdef __cplusplus
}
#endif
