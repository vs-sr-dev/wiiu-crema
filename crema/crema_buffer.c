// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan

#include "crema_buffer.h"

#include <string.h>
#include <whb/log.h>

bool CremaBufferCreate(GX2RBuffer *buf, GX2RResourceFlags bind,
                       uint32_t elemSize, uint32_t elemCount,
                       const void *initialData)
{
    memset(buf, 0, sizeof(*buf));
    buf->flags = (GX2RResourceFlags)(bind |
                                     GX2R_RESOURCE_USAGE_CPU_WRITE |
                                     GX2R_RESOURCE_USAGE_GPU_READ);
    buf->elemSize  = elemSize;
    buf->elemCount = elemCount;
    if (!GX2RCreateBuffer(buf)) {
        WHBLogPrintf("[buffer] GX2RCreateBuffer failed (%u x %u bytes)",
                     elemCount, elemSize);
        return false;
    }
    if (initialData)
        CremaBufferUpdate(buf, initialData, (size_t)elemSize * elemCount);
    return true;
}

bool CremaBufferCreateVertex(GX2RBuffer *buf, uint32_t stride,
                             uint32_t vertexCount, const void *data)
{
    return CremaBufferCreate(buf, GX2R_RESOURCE_BIND_VERTEX_BUFFER,
                             stride, vertexCount, data);
}

bool CremaBufferCreateIndexU16(GX2RBuffer *buf, uint32_t indexCount,
                               const void *data)
{
    return CremaBufferCreate(buf, GX2R_RESOURCE_BIND_INDEX_BUFFER,
                             sizeof(uint16_t), indexCount, data);
}

void CremaBufferUpdate(GX2RBuffer *buf, const void *data, size_t bytes)
{
    void *dst = GX2RLockBufferEx(buf, (GX2RResourceFlags)0);
    if (!dst)
        return;
    size_t capacity = (size_t)buf->elemSize * buf->elemCount;
    memcpy(dst, data, bytes < capacity ? bytes : capacity);
    GX2RUnlockBufferEx(buf, (GX2RResourceFlags)0);
}

void CremaBufferDestroy(GX2RBuffer *buf)
{
    GX2RDestroyBufferEx(buf, (GX2RResourceFlags)0);
}
