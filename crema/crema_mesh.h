// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan
//
// Baked mesh loading (.cmesh, produced by tools/crema_bake.py).
//
// The whole point of baking offline is that this file does no parsing and no
// conversion: the vertex and index blobs are already interleaved in the layout
// the fetch shader wants, and already big-endian — which is the CPU's native
// order — so the bytes go from the file straight into GPU memory.
//
// The mesh also carries its own attribute layout, so the file describes the
// fetch shader instead of the program hard-coding it:
//
//     CremaMeshLoad(&ship, "/vol/content/ship.cmesh");
//     shader = CremaShaderCompile(vs, ps, ship.attribs, ship.attribCount);

#pragma once
#include <gx2r/buffer.h>
#include <stdbool.h>
#include <stdint.h>

#include "crema_shader.h"   // CremaAttrib

#ifdef __cplusplus
extern "C" {
#endif

#define CREMA_MESH_MAX_ATTRIBS 8

typedef struct {
    GX2RBuffer  vbo;
    GX2RBuffer  ibo;
    uint32_t    vertexCount;
    uint32_t    indexCount;
    uint32_t    stride;
    float       aabbMin[3];    // model-space bounds, for culling later
    float       aabbMax[3];
    CremaAttrib attribs[CREMA_MESH_MAX_ATTRIBS];
    uint32_t    attribCount;
} CremaMesh;

// Load a .cmesh. Paths inside a .wuhb live under /vol/content/.
bool CremaMeshLoad(CremaMesh *mesh, const char *path);

// Same, from bytes already in memory — what a .cpak hands you (crema_pak.h).
// `label` is only used for logging. Nothing is kept: the blob may be freed as
// soon as this returns.
bool CremaMeshLoadFromMemory(CremaMesh *mesh, const void *blob, size_t size,
                             const char *label);

// GX2RSetAttributeBuffer + GX2RDrawIndexed. instanceCount 1 = a single draw.
void CremaMeshDraw(const CremaMesh *mesh, uint32_t instanceCount);

void CremaMeshDestroy(CremaMesh *mesh);

#ifdef __cplusplus
}
#endif
