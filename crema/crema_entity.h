// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan
//
// A pool of things that exist in the world.
//
// Storage belongs to the caller — no allocation happens here. A game knows how
// many enemies it can ever have on screen; a framework does not, and pretending
// otherwise buys a heap allocator in the middle of a frame.
//
// Slots are reused, so a raw CremaEntity* is only valid until something is
// despawned. Hold the index if you need to remember an entity across frames.

#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "crema_matrix.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    Vec3     pos;
    float    yaw, pitch, roll;
    float    radius;       // bounding sphere in world units
    uint32_t kind;         // game-defined tag; the framework never reads it
    bool     active;
} CremaEntity;

typedef struct {
    CremaEntity *items;
    uint32_t     capacity;
    uint32_t     watermark;   // highest slot ever used: bounds the iteration
} CremaEntityPool;

void CremaEntityPoolInit(CremaEntityPool *pool, CremaEntity *storage,
                         uint32_t capacity);

// Returns NULL when the pool is full — a game that ignores this drops spawns
// silently, which is a bug that hides for weeks.
CremaEntity *CremaEntitySpawn(CremaEntityPool *pool, uint32_t kind);

void CremaEntityDespawn(CremaEntityPool *pool, CremaEntity *entity);
void CremaEntityClear(CremaEntityPool *pool);

uint32_t CremaEntityActiveCount(const CremaEntityPool *pool);

// Iterate every live entity:
//   for (uint32_t i = 0; i < pool.watermark; i++) {
//       CremaEntity *e = &pool.items[i];
//       if (!e->active) continue;
//       ...

#ifdef __cplusplus
}
#endif
