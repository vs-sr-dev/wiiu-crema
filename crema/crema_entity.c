// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan

#include "crema_entity.h"

#include <string.h>

void CremaEntityPoolInit(CremaEntityPool *pool, CremaEntity *storage,
                         uint32_t capacity)
{
    pool->items     = storage;
    pool->capacity  = capacity;
    pool->watermark = 0;
    memset(storage, 0, sizeof(CremaEntity) * capacity);
}

CremaEntity *CremaEntitySpawn(CremaEntityPool *pool, uint32_t kind)
{
    for (uint32_t i = 0; i < pool->capacity; i++) {
        CremaEntity *e = &pool->items[i];
        if (e->active)
            continue;
        memset(e, 0, sizeof(*e));
        e->active = true;
        e->kind   = kind;
        if (i + 1 > pool->watermark)
            pool->watermark = i + 1;
        return e;
    }
    return NULL;
}

void CremaEntityDespawn(CremaEntityPool *pool, CremaEntity *entity)
{
    (void)pool;
    if (entity)
        entity->active = false;
}

void CremaEntityClear(CremaEntityPool *pool)
{
    memset(pool->items, 0, sizeof(CremaEntity) * pool->capacity);
    pool->watermark = 0;
}

uint32_t CremaEntityActiveCount(const CremaEntityPool *pool)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < pool->watermark; i++)
        if (pool->items[i].active)
            n++;
    return n;
}
