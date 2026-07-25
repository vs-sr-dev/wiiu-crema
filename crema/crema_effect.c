// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan

#include "crema_effect.h"

#include <string.h>

void CremaEffectPoolInit(CremaEffectPool *pool, CremaEffect *storage,
                         uint32_t capacity)
{
    pool->items     = storage;
    pool->capacity  = capacity;
    pool->watermark = 0;
    memset(storage, 0, sizeof(CremaEffect) * capacity);
}

CremaEffect *CremaEffectSpawn(CremaEffectPool *pool, Vec3 pos, float life,
                              float sizeStart, float sizeEnd,
                              float r, float g, float b, float alpha)
{
    for (uint32_t i = 0; i < pool->capacity; i++) {
        CremaEffect *e = &pool->items[i];
        if (e->active)
            continue;
        memset(e, 0, sizeof(*e));
        e->pos        = pos;
        e->life       = life > 0.0f ? life : 0.001f;
        e->sizeStart  = sizeStart;
        e->sizeEnd    = sizeEnd;
        e->r = r; e->g = g; e->b = b;
        e->alphaStart = alpha;
        e->active     = true;
        if (i + 1 > pool->watermark)
            pool->watermark = i + 1;
        return e;
    }
    return NULL;
}

void CremaEffectUpdate(CremaEffectPool *pool, float dt)
{
    for (uint32_t i = 0; i < pool->watermark; i++) {
        CremaEffect *e = &pool->items[i];
        if (!e->active)
            continue;
        e->age += dt;
        if (e->age >= e->life) {
            e->active = false;
            continue;
        }
        e->pos.x += e->velocity.x * dt;
        e->pos.y += e->velocity.y * dt;
        e->pos.z += e->velocity.z * dt;
    }
}

uint32_t CremaEffectPack(const CremaEffectPool *pool, float (*out)[4],
                         uint32_t maxEffects)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < pool->watermark && n < maxEffects; i++) {
        const CremaEffect *e = &pool->items[i];
        if (!e->active)
            continue;
        float t = e->age / e->life;          // 0 at birth, 1 at death
        float size = e->sizeStart + (e->sizeEnd - e->sizeStart) * t;
        out[n * 2][0] = e->pos.x;
        out[n * 2][1] = e->pos.y;
        out[n * 2][2] = e->pos.z;
        out[n * 2][3] = size;
        out[n * 2 + 1][0] = e->r;
        out[n * 2 + 1][1] = e->g;
        out[n * 2 + 1][2] = e->b;
        out[n * 2 + 1][3] = e->alphaStart * (1.0f - t);   // fade out linearly
        n++;
    }
    return n;
}
