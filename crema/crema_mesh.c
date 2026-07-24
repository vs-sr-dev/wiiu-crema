// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan

#include "crema_mesh.h"
#include "crema_buffer.h"

#include <coreinit/time.h>
#include <gx2/draw.h>
#include <gx2r/draw.h>
#include <stdio.h>
#include <string.h>
#include <whb/log.h>

// Mirrors the layout written by tools/crema_bake.py. The console is
// big-endian and so is the file, so a plain fread lands the fields correct.
typedef struct {
    char     magic[4];
    uint32_t version;
    uint32_t vertexCount;
    uint32_t indexCount;
    uint32_t stride;
    uint32_t attribCount;
    float    aabbMin[3];
    float    aabbMax[3];
    uint32_t vertexOffset;
    uint32_t indexOffset;
    uint32_t reserved[2];
} MeshHeader;
_Static_assert(sizeof(MeshHeader) == 64, "cmesh header must stay 64 bytes");

#define CMESH_VERSION 1

enum {
    CMESH_ATTRIB_FLOAT2 = 1,
    CMESH_ATTRIB_FLOAT3 = 2,
    CMESH_ATTRIB_FLOAT4 = 3,
    CMESH_ATTRIB_UNORM8x4 = 4,
};

static bool attribFormat(uint32_t type, GX2AttribFormat *out)
{
    switch (type) {
    case CMESH_ATTRIB_FLOAT2:   *out = GX2_ATTRIB_FORMAT_FLOAT_32_32; return true;
    case CMESH_ATTRIB_FLOAT3:   *out = GX2_ATTRIB_FORMAT_FLOAT_32_32_32; return true;
    case CMESH_ATTRIB_FLOAT4:   *out = GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32; return true;
    case CMESH_ATTRIB_UNORM8x4: *out = GX2_ATTRIB_FORMAT_UNORM_8_8_8_8; return true;
    default: return false;
    }
}

// Read straight into GPU-visible memory: no staging copy, no byteswap.
static bool readIntoBuffer(FILE *fh, GX2RBuffer *buf, uint32_t offset,
                           size_t bytes)
{
    if (fseek(fh, (long)offset, SEEK_SET) != 0)
        return false;
    void *dst = GX2RLockBufferEx(buf, (GX2RResourceFlags)0);
    if (!dst)
        return false;
    size_t got = fread(dst, 1, bytes, fh);
    GX2RUnlockBufferEx(buf, (GX2RResourceFlags)0);
    return got == bytes;
}

bool CremaMeshLoad(CremaMesh *mesh, const char *path)
{
    memset(mesh, 0, sizeof(*mesh));

    // Opening is timed separately from reading: on hardware a small asset is
    // dominated by the fixed cost of the open, not by the bytes.
    uint64_t openStart = OSGetSystemTime();
    FILE *fh = fopen(path, "rb");
    uint64_t openTicks = OSGetSystemTime() - openStart;
    if (!fh) {
        WHBLogPrintf("[mesh] cannot open %s", path);
        return false;
    }

    MeshHeader h;
    bool ok = fread(&h, 1, sizeof(h), fh) == sizeof(h);
    if (ok && memcmp(h.magic, "CMSH", 4) != 0) {
        WHBLogPrintf("[mesh] %s is not a .cmesh", path);
        ok = false;
    }
    if (ok && h.version != CMESH_VERSION) {
        WHBLogPrintf("[mesh] %s is version %u, expected %u",
                     path, h.version, CMESH_VERSION);
        ok = false;
    }
    if (ok && (h.attribCount == 0 || h.attribCount > CREMA_MESH_MAX_ATTRIBS)) {
        WHBLogPrintf("[mesh] %s has %u attributes", path, h.attribCount);
        ok = false;
    }
    if (!ok) {
        fclose(fh);
        return false;
    }

    for (uint32_t i = 0; i < h.attribCount && ok; i++) {
        uint32_t entry[3];
        if (fread(entry, 1, sizeof(entry), fh) != sizeof(entry)) {
            ok = false;
            break;
        }
        GX2AttribFormat format;
        if (!attribFormat(entry[2], &format)) {
            WHBLogPrintf("[mesh] %s: unknown attribute type %u", path, entry[2]);
            ok = false;
            break;
        }
        mesh->attribs[i].location = entry[0];
        mesh->attribs[i].buffer   = 0;
        mesh->attribs[i].offset   = entry[1];
        mesh->attribs[i].format   = format;
    }

    if (ok)
        ok = CremaBufferCreate(&mesh->vbo, GX2R_RESOURCE_BIND_VERTEX_BUFFER,
                               h.stride, h.vertexCount, NULL);
    if (ok)
        ok = CremaBufferCreate(&mesh->ibo, GX2R_RESOURCE_BIND_INDEX_BUFFER,
                               sizeof(uint16_t), h.indexCount, NULL);
    size_t blobBytes = (size_t)h.stride * h.vertexCount +
                       (size_t)sizeof(uint16_t) * h.indexCount;
    uint64_t readStart = OSGetSystemTime();
    if (ok)
        ok = readIntoBuffer(fh, &mesh->vbo, h.vertexOffset,
                            (size_t)h.stride * h.vertexCount);
    if (ok)
        ok = readIntoBuffer(fh, &mesh->ibo, h.indexOffset,
                            (size_t)sizeof(uint16_t) * h.indexCount);
    uint64_t readTicks = OSGetSystemTime() - readStart;
    fclose(fh);

    if (!ok) {
        WHBLogPrintf("[mesh] %s: truncated or unreadable", path);
        CremaMeshDestroy(mesh);
        return false;
    }

    mesh->vertexCount = h.vertexCount;
    mesh->indexCount  = h.indexCount;
    mesh->stride      = h.stride;
    mesh->attribCount = h.attribCount;
    memcpy(mesh->aabbMin, h.aabbMin, sizeof(mesh->aabbMin));
    memcpy(mesh->aabbMax, h.aabbMax, sizeof(mesh->aabbMax));

    double openMs = (double)OSTicksToMicroseconds(openTicks) / 1000.0;
    double readMs = (double)OSTicksToMicroseconds(readTicks) / 1000.0;
    WHBLogPrintf("[mesh] %s: %u verts, %u tris, stride %u, %u attribs",
                 path, h.vertexCount, h.indexCount / 3, h.stride, h.attribCount);
    WHBLogPrintf("[mesh]   open %.2f ms | read %u B in %.2f ms (%.1f MB/s)",
                 openMs, (uint32_t)blobBytes, readMs,
                 readMs > 0.0 ? (double)blobBytes / 1000.0 / readMs : 0.0);
    return true;
}

void CremaMeshDraw(const CremaMesh *mesh, uint32_t instanceCount)
{
    GX2RSetAttributeBuffer((GX2RBuffer *)&mesh->vbo, 0, mesh->stride, 0);
    GX2RDrawIndexed(GX2_PRIMITIVE_MODE_TRIANGLES, (GX2RBuffer *)&mesh->ibo,
                    GX2_INDEX_TYPE_U16, mesh->indexCount, 0, 0, instanceCount);
}

void CremaMeshDestroy(CremaMesh *mesh)
{
    if (mesh->vbo.buffer)
        CremaBufferDestroy(&mesh->vbo);
    if (mesh->ibo.buffer)
        CremaBufferDestroy(&mesh->ibo);
    memset(mesh, 0, sizeof(*mesh));
}
