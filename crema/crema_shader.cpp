// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan

#include "crema_shader.h"

#include <malloc.h>
#include <string.h>
#include <stdlib.h>

#include <gx2/mem.h>
#include <gx2/enum.h>
#include <gx2/utils.h>
#include <whb/log.h>

#include "CafeGLSLCompiler.h"

bool CremaShaderInitCompiler(void)
{
    if (!GLSL_Init()) {
        WHBLogPrintf("[shader] GLSL_Init failed - glslcompiler.rpl missing?"
                     " (Cemu: <data>/cafeLibs/, HW: sd:/wiiu/libs/)");
        return false;
    }
    WHBLogPrintf("[shader] CafeGLSL compiler loaded");
    return true;
}

void CremaShaderShutdownCompiler(void)
{
    GLSL_Shutdown();
}

// destSel mask per attribute format: missing components read as (0,0,0,1).
static uint32_t maskForFormat(GX2AttribFormat format)
{
    switch (format) {
    case GX2_ATTRIB_FORMAT_FLOAT_32:
        return GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_0, GX2_SQ_SEL_0, GX2_SQ_SEL_1);
    case GX2_ATTRIB_FORMAT_FLOAT_32_32:
        return GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_Y, GX2_SQ_SEL_0, GX2_SQ_SEL_1);
    case GX2_ATTRIB_FORMAT_FLOAT_32_32_32:
        return GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_Y, GX2_SQ_SEL_Z, GX2_SQ_SEL_1);
    case GX2_ATTRIB_FORMAT_UNORM_8_8_8_8:
    case GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32:
    default:
        return GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_Y, GX2_SQ_SEL_Z, GX2_SQ_SEL_W);
    }
}

CremaShader *CremaShaderCompile(const char *vsSource, const char *psSource,
                            const CremaAttrib *attribs, uint32_t attribCount)
{
    char infoLog[1024];

    GX2VertexShader *vs = GLSL_CompileVertexShader(vsSource, infoLog,
                                                   sizeof(infoLog),
                                                   GLSL_COMPILER_FLAG_NONE);
    if (!vs) {
        WHBLogPrintf("[shader] VS compile failed: %s", infoLog);
        return nullptr;
    }

    GX2PixelShader *ps = GLSL_CompilePixelShader(psSource, infoLog,
                                                 sizeof(infoLog),
                                                 GLSL_COMPILER_FLAG_NONE);
    if (!ps) {
        WHBLogPrintf("[shader] PS compile failed: %s", infoLog);
        GLSL_FreeVertexShader(vs);
        return nullptr;
    }

    CremaShader *shader = (CremaShader *)calloc(1, sizeof(CremaShader));
    shader->vs = vs;
    shader->ps = ps;

    GX2AttribStream streams[16];
    memset(streams, 0, sizeof(streams));
    for (uint32_t i = 0; i < attribCount && i < 16; i++) {
        streams[i].location   = attribs[i].location;
        streams[i].buffer     = attribs[i].buffer;
        streams[i].offset     = attribs[i].offset;
        streams[i].format     = attribs[i].format;
        streams[i].type       = GX2_ATTRIB_INDEX_PER_VERTEX;
        streams[i].aluDivisor = 0;
        streams[i].mask       = maskForFormat(attribs[i].format);
        streams[i].endianSwap = GX2_ENDIAN_SWAP_DEFAULT;
    }

    uint32_t fsSize = GX2CalcFetchShaderSizeEx(attribCount,
                                               GX2_FETCH_SHADER_TESSELLATION_NONE,
                                               GX2_TESSELLATION_MODE_DISCRETE);
    shader->fetchShaderProgram = memalign(GX2_SHADER_PROGRAM_ALIGNMENT, fsSize);
    GX2InitFetchShaderEx(&shader->fetchShader, (uint8_t *)shader->fetchShaderProgram,
                         attribCount, streams,
                         GX2_FETCH_SHADER_TESSELLATION_NONE,
                         GX2_TESSELLATION_MODE_DISCRETE);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, shader->fetchShaderProgram, fsSize);

    WHBLogPrintf("[shader] compiled ok (vs %u bytes, ps %u bytes, %u attribs)",
                 vs->size, ps->size, attribCount);
    return shader;
}

void CremaShaderFree(CremaShader *shader)
{
    if (!shader)
        return;
    if (shader->fetchShaderProgram)
        free(shader->fetchShaderProgram);
    if (shader->vs)
        GLSL_FreeVertexShader(shader->vs);
    if (shader->ps)
        GLSL_FreePixelShader(shader->ps);
    free(shader);
}

void CremaShaderBind(const CremaShader *shader)
{
    GX2SetFetchShader(&shader->fetchShader);
    GX2SetVertexShader(shader->vs);
    GX2SetPixelShader(shader->ps);
    // CafeGLSL emits uniform-block-mode shaders. Cemu renders even without
    // this, real hardware does not — keep it here so both behave the same.
    GX2SetShaderMode(shader->vs->mode);
}

// --- uniform blocks ----------------------------------------------------------

void *CremaUniformAlloc(size_t bytes)
{
    return memalign(GX2_UNIFORM_BLOCK_ALIGNMENT, bytes);
}

void CremaUniformFreeBlock(void *block)
{
    free(block);
}

int32_t CremaShaderVSBlockLocation(const CremaShader *shader, const char *name)
{
    const GX2VertexShader *vs = shader->vs;
    for (uint32_t i = 0; i < vs->uniformBlockCount; i++) {
        if (vs->uniformBlocks[i].name && !strcmp(vs->uniformBlocks[i].name, name))
            return (int32_t)vs->uniformBlocks[i].offset;
    }
    WHBLogPrintf("[shader] uniform block '%s' not found (%u blocks)",
                 name, vs->uniformBlockCount);
    return -1;
}

int32_t CremaShaderPSBlockLocation(const CremaShader *shader, const char *name)
{
    const GX2PixelShader *ps = shader->ps;
    for (uint32_t i = 0; i < ps->uniformBlockCount; i++) {
        if (ps->uniformBlocks[i].name && !strcmp(ps->uniformBlocks[i].name, name))
            return (int32_t)ps->uniformBlocks[i].offset;
    }
    return -1;
}

void CremaUniformStore(void *block, const void *src, size_t bytes)
{
    const uint32_t *in = (const uint32_t *)src;
    uint32_t *out = (uint32_t *)block;
    size_t words = bytes / 4;
    // GPU7 reads uniform buffers as little-endian 32-bit words; the PPC side
    // is big-endian, so swap each word (GX2SetVertexUniformBlock does not).
    for (size_t i = 0; i < words; i++)
        out[i] = __builtin_bswap32(in[i]);
    GX2Invalidate((GX2InvalidateMode)(GX2_INVALIDATE_MODE_CPU |
                                      GX2_INVALIDATE_MODE_UNIFORM_BLOCK),
                  block, bytes);
}
