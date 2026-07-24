// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan
//
// Runtime GLSL -> Latte shader pipeline via CafeGLSL (glslcompiler.rpl).
// Compiles a vertex+pixel shader pair and builds the matching fetch shader
// from an explicit attribute list (locations must match the GLSL
// layout(location=N) qualifiers — CafeGLSL requires explicit bindings).

#pragma once
#include <gx2/shaders.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    GX2VertexShader *vs;
    GX2PixelShader  *ps;
    GX2FetchShader   fetchShader;
    void            *fetchShaderProgram;
} CremaShader;

// Describes one vertex attribute stream for the fetch shader.
typedef struct {
    uint32_t        location;   // matches layout(location=N) in the VS
    uint32_t        buffer;     // attribute buffer slot
    uint32_t        offset;     // byte offset within the vertex
    GX2AttribFormat format;
} CremaAttrib;

// Load glslcompiler.rpl (Cemu: cafeLibs/, HW: sd:/wiiu/libs/). Must be called
// once before CremaShaderCompile.
bool CremaShaderInitCompiler(void);
void CremaShaderShutdownCompiler(void);

CremaShader *CremaShaderCompile(const char *vsSource, const char *psSource,
                            const CremaAttrib *attribs, uint32_t attribCount);
void CremaShaderFree(CremaShader *shader);

// GX2SetFetchShader + vertex/pixel shader + GX2SetShaderMode.
void CremaShaderBind(const CremaShader *shader);

// --- uniform blocks ---------------------------------------------------------
// CafeGLSL shaders run in GX2_SHADER_MODE_UNIFORM_BLOCK: uniform data lives in
// a buffer the GPU reads directly, as little-endian 32-bit words — the CPU
// (big-endian) must byteswap each word and flush the cache before drawing.

// Allocate a GX2_UNIFORM_BLOCK_ALIGNMENT-aligned block.
void *CremaUniformAlloc(size_t bytes);
void  CremaUniformFreeBlock(void *block);

// Byteswap `bytes` (multiple of 4) from src into block and GX2Invalidate it.
void  CremaUniformStore(void *block, const void *src, size_t bytes);

// Uniform-block binding location from vertex-shader reflection (-1 if not
// found; with explicit layout(binding=N) it should match N).
int32_t CremaShaderVSBlockLocation(const CremaShader *shader, const char *name);

// Same, from the pixel-shader reflection (VS and PS bindings are separate
// namespaces in GX2: a block read by both stages must be bound twice, via
// GX2SetVertexUniformBlock AND GX2SetPixelUniformBlock).
int32_t CremaShaderPSBlockLocation(const CremaShader *shader, const char *name);

#ifdef __cplusplus
}
#endif
