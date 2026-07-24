// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan
//
// GX2R buffer helpers. Every example needs the same create / lock / copy /
// unlock dance for vertex and index data; GX2R owns the allocation and does
// the cache maintenance around the lock, which is what keeps the GPU from
// reading stale cache lines on real hardware.

#pragma once
#include <gx2r/buffer.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Create a CPU-written / GPU-read buffer. `bind` is one of the
// GX2R_RESOURCE_BIND_* flags; `initialData` may be NULL to leave it empty.
bool CremaBufferCreate(GX2RBuffer *buf, GX2RResourceFlags bind,
                       uint32_t elemSize, uint32_t elemCount,
                       const void *initialData);

// The two common cases.
bool CremaBufferCreateVertex(GX2RBuffer *buf, uint32_t stride,
                             uint32_t vertexCount, const void *data);
bool CremaBufferCreateIndexU16(GX2RBuffer *buf, uint32_t indexCount,
                               const void *data);

// Rewrite buffer contents (the lock/unlock pair does the invalidate). Only
// safe while the GPU is not reading the buffer — for per-frame data prefer a
// double-buffered CremaUniformRing (crema_frame.h).
void CremaBufferUpdate(GX2RBuffer *buf, const void *data, size_t bytes);

void CremaBufferDestroy(GX2RBuffer *buf);

#ifdef __cplusplus
}
#endif
