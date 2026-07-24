// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan
//
// GX2 PoC 3 — rotating flat-shaded cube. New pieces over PoC 2:
//   - filled triangles with depth test + back-face culling
//   - per-face normals (24-vertex cube) + directional Lambert light in the VS:
//     constant normal per face -> constant colour per face = flat shading
//   - multi-member uniform block (std140: mat4 + mat4 + vec4)

#include <gx2/draw.h>
#include <gx2/registers.h>
#include <gx2/shaders.h>
#include <gx2r/buffer.h>
#include <gx2r/draw.h>
#include <whb/gfx.h>
#include <coreinit/time.h>
#include <string.h>

#include "crema_app.h"
#include "crema_shader.h"
#include "crema_matrix.h"

static const char *VS_SRC = R"(
#version 450
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(binding = 0) uniform Transform {
    mat4 uMVP;
    mat4 uModel;
    vec4 uLightDir;   // world-space, normalized, w unused
};
layout(location = 0) out vec3 vColor;
void main()
{
    gl_Position = uMVP * vec4(aPosition, 1.0);
    vec3 n = normalize(mat3(uModel) * aNormal);
    float diff = max(dot(n, -uLightDir.xyz), 0.0);
    vColor = vec3(0.95, 0.55, 0.15) * (0.15 + 0.85 * diff);
}
)";

static const char *PS_SRC = R"(
#version 450
layout(location = 0) in vec3 vColor;
layout(location = 0) out vec4 oColor;
void main()
{
    oColor = vec4(vColor, 1.0);
}
)";

// 24 vertices (4 per face), CCW seen from outside: x,y,z, nx,ny,nz
static const float CUBE_VERTS[] = {
    // +Z front
    -1,-1, 1,  0,0,1,    1,-1, 1,  0,0,1,    1, 1, 1,  0,0,1,   -1, 1, 1,  0,0,1,
    // -Z back
     1,-1,-1,  0,0,-1,  -1,-1,-1,  0,0,-1,  -1, 1,-1,  0,0,-1,   1, 1,-1,  0,0,-1,
    // +X right
     1,-1, 1,  1,0,0,    1,-1,-1,  1,0,0,    1, 1,-1,  1,0,0,    1, 1, 1,  1,0,0,
    // -X left
    -1,-1,-1, -1,0,0,   -1,-1, 1, -1,0,0,   -1, 1, 1, -1,0,0,   -1, 1,-1, -1,0,0,
    // +Y top
    -1, 1, 1,  0,1,0,    1, 1, 1,  0,1,0,    1, 1,-1,  0,1,0,   -1, 1,-1,  0,1,0,
    // -Y bottom
    -1,-1,-1,  0,-1,0,   1,-1,-1,  0,-1,0,   1,-1, 1,  0,-1,0,  -1,-1, 1,  0,-1,0,
};
#define VERTEX_STRIDE (6 * sizeof(float))
#define VERTEX_COUNT  24

static uint16_t CUBE_TRIS[36];

static void buildIndices(void)
{
    for (int f = 0; f < 6; f++) {
        uint16_t b = (uint16_t)(f * 4);
        uint16_t *o = &CUBE_TRIS[f * 6];
        o[0] = b; o[1] = (uint16_t)(b + 1); o[2] = (uint16_t)(b + 2);
        o[3] = b; o[4] = (uint16_t)(b + 2); o[5] = (uint16_t)(b + 3);
    }
}

typedef struct {
    Mat4  mvp;
    Mat4  model;
    float lightDir[4];
} TransformBlock;

static void drawScene(const CremaShader *shader, GX2RBuffer *vbo, GX2RBuffer *ibo,
                      int32_t blockLoc, const void *ubo)
{
    CremaShaderBind(shader);
    GX2SetDepthOnlyControl(TRUE, TRUE, GX2_COMPARE_FUNC_LESS);
    GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, TRUE);
    GX2SetVertexUniformBlock(blockLoc, sizeof(TransformBlock), ubo);
    GX2RSetAttributeBuffer(vbo, 0, VERTEX_STRIDE, 0);
    GX2RDrawIndexed(GX2_PRIMITIVE_MODE_TRIANGLES, ibo, GX2_INDEX_TYPE_U16,
                    36, 0, 0, 1);
}

int main(int argc, char **argv)
{
    if (!CremaAppInit("poc3-flatcube"))
        return -1;
    if (!CremaShaderInitCompiler()) {
        CremaAppShutdown();
        return -1;
    }

    const CremaAttrib attribs[] = {
        { 0, 0, 0,                 GX2_ATTRIB_FORMAT_FLOAT_32_32_32 },
        { 1, 0, 3 * sizeof(float), GX2_ATTRIB_FORMAT_FLOAT_32_32_32 },
    };
    CremaShader *shader = CremaShaderCompile(VS_SRC, PS_SRC, attribs, 2);
    if (!shader) {
        CremaShaderShutdownCompiler();
        CremaAppShutdown();
        return -1;
    }
    int32_t blockLoc = CremaShaderVSBlockLocation(shader, "Transform");
    if (blockLoc < 0)
        blockLoc = 0;

    buildIndices();

    GX2RBuffer vbo;
    memset(&vbo, 0, sizeof(vbo));
    vbo.flags = (GX2RResourceFlags)(GX2R_RESOURCE_BIND_VERTEX_BUFFER |
                                    GX2R_RESOURCE_USAGE_CPU_WRITE |
                                    GX2R_RESOURCE_USAGE_GPU_READ);
    vbo.elemSize  = VERTEX_STRIDE;
    vbo.elemCount = VERTEX_COUNT;
    GX2RCreateBuffer(&vbo);
    void *p = GX2RLockBufferEx(&vbo, (GX2RResourceFlags)0);
    memcpy(p, CUBE_VERTS, sizeof(CUBE_VERTS));
    GX2RUnlockBufferEx(&vbo, (GX2RResourceFlags)0);

    GX2RBuffer ibo;
    memset(&ibo, 0, sizeof(ibo));
    ibo.flags = (GX2RResourceFlags)(GX2R_RESOURCE_BIND_INDEX_BUFFER |
                                    GX2R_RESOURCE_USAGE_CPU_WRITE |
                                    GX2R_RESOURCE_USAGE_GPU_READ);
    ibo.elemSize  = sizeof(uint16_t);
    ibo.elemCount = 36;
    GX2RCreateBuffer(&ibo);
    p = GX2RLockBufferEx(&ibo, (GX2RResourceFlags)0);
    memcpy(p, CUBE_TRIS, sizeof(CUBE_TRIS));
    GX2RUnlockBufferEx(&ibo, (GX2RResourceFlags)0);

    TransformBlock *ubo = (TransformBlock *)CremaUniformAlloc(sizeof(TransformBlock));

    Mat4 proj = mat4_perspective(60.0f * 3.14159265f / 180.0f,
                                 16.0f / 9.0f, 0.1f, 100.0f);
    Mat4 view = mat4_translate(0.0f, 0.0f, -5.0f);
    Vec3 lightRaw = { -0.4f, -0.6f, -0.7f };
    Vec3 lightDir = vec3_normalize(lightRaw);

    uint64_t t0 = OSGetSystemTime();
    while (CremaAppRunning()) {
        float t = (float)((double)OSTicksToMilliseconds(OSGetSystemTime() - t0) / 1000.0);
        TransformBlock blk;
        blk.model = mat4_mul(mat4_rotate_y(t * 0.9f), mat4_rotate_x(t * 0.5f));
        blk.mvp   = mat4_mul(proj, mat4_mul(view, blk.model));
        blk.lightDir[0] = lightDir.x;
        blk.lightDir[1] = lightDir.y;
        blk.lightDir[2] = lightDir.z;
        blk.lightDir[3] = 0.0f;
        CremaUniformStore(ubo, &blk, sizeof(blk));

        WHBGfxBeginRender();

        WHBGfxBeginRenderTV();
        WHBGfxClearColor(0.02f, 0.02f, 0.08f, 1.0f);
        drawScene(shader, &vbo, &ibo, blockLoc, ubo);
        WHBGfxFinishRenderTV();

        WHBGfxBeginRenderDRC();
        WHBGfxClearColor(0.02f, 0.02f, 0.08f, 1.0f);
        drawScene(shader, &vbo, &ibo, blockLoc, ubo);
        WHBGfxFinishRenderDRC();

        CremaFinishRenderAndMark(NULL);
    }

    CremaUniformFreeBlock(ubo);
    GX2RDestroyBufferEx(&ibo, (GX2RResourceFlags)0);
    GX2RDestroyBufferEx(&vbo, (GX2RResourceFlags)0);
    CremaShaderFree(shader);
    CremaShaderShutdownCompiler();
    CremaAppShutdown();
    return 0;
}
