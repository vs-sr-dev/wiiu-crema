// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan
//
// GX2 PoC 4 — rotating Gouraud-shaded torus. New pieces over PoC 3:
//   - procedural mesh (48x24 torus, smooth per-vertex normals)
//   - per-vertex diffuse + Blinn specular in the VS, colour interpolated
//     across triangles (classic Gouraud)
//   - GPU busy-time measurement via GX2SampleTopGPUCycle/BottomGPUCycle

#include <gx2/draw.h>
#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2/shaders.h>
#include <gx2r/buffer.h>
#include <gx2r/draw.h>
#include <whb/gfx.h>
#include <coreinit/time.h>
#include <malloc.h>
#include <string.h>
#include <math.h>

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
    vec4 uEyePos;     // world-space, w unused
};
layout(location = 0) out vec3 vColor;
void main()
{
    gl_Position = uMVP * vec4(aPosition, 1.0);
    vec3 wp = (uModel * vec4(aPosition, 1.0)).xyz;
    vec3 n  = normalize(mat3(uModel) * aNormal);
    vec3 l  = -uLightDir.xyz;
    float diff = max(dot(n, l), 0.0);
    vec3 v  = normalize(uEyePos.xyz - wp);
    vec3 h  = normalize(l + v);
    float spec = pow(max(dot(n, h), 0.0), 32.0) * (diff > 0.0 ? 1.0 : 0.0);
    vec3 base = vec3(0.20, 0.45, 0.95);
    vColor = base * (0.12 + 0.88 * diff) + vec3(0.6) * spec;
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

#define SEG_MAJOR 48
#define SEG_MINOR 24
#define TORUS_VERTS (SEG_MAJOR * SEG_MINOR)
#define TORUS_INDICES (SEG_MAJOR * SEG_MINOR * 6)
#define VERTEX_STRIDE (6 * sizeof(float))

// pos + normal, rings wrap via modulo indexing
static void buildTorus(float *verts, uint16_t *indices, float R, float r)
{
    for (int i = 0; i < SEG_MAJOR; i++) {
        float u = (float)i / SEG_MAJOR * 2.0f * (float)M_PI;
        float cu = cosf(u), su = sinf(u);
        for (int j = 0; j < SEG_MINOR; j++) {
            float v = (float)j / SEG_MINOR * 2.0f * (float)M_PI;
            float cv = cosf(v), sv = sinf(v);
            float *o = &verts[(i * SEG_MINOR + j) * 6];
            o[0] = (R + r * cv) * cu;
            o[1] = r * sv;
            o[2] = (R + r * cv) * su;
            o[3] = cv * cu;
            o[4] = sv;
            o[5] = cv * su;
        }
    }
    for (int i = 0; i < SEG_MAJOR; i++) {
        int i2 = (i + 1) % SEG_MAJOR;
        for (int j = 0; j < SEG_MINOR; j++) {
            int j2 = (j + 1) % SEG_MINOR;
            uint16_t a = (uint16_t)(i  * SEG_MINOR + j);
            uint16_t b = (uint16_t)(i2 * SEG_MINOR + j);
            uint16_t c = (uint16_t)(i2 * SEG_MINOR + j2);
            uint16_t d = (uint16_t)(i  * SEG_MINOR + j2);
            uint16_t *o = &indices[(i * SEG_MINOR + j) * 6];
            // a,c,b / a,d,c: CCW seen from outside (outward triangle normals)
            o[0] = a; o[1] = c; o[2] = b;
            o[3] = a; o[4] = d; o[5] = c;
        }
    }
}

typedef struct {
    Mat4  mvp;
    Mat4  model;
    float lightDir[4];
    float eyePos[4];
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
                    TORUS_INDICES, 0, 0, 1);
}

int main(int argc, char **argv)
{
    if (!CremaAppInit("poc4-gouraud"))
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

    float *verts = (float *)malloc(TORUS_VERTS * VERTEX_STRIDE);
    uint16_t *indices = (uint16_t *)malloc(TORUS_INDICES * sizeof(uint16_t));
    buildTorus(verts, indices, 1.2f, 0.5f);

    GX2RBuffer vbo;
    memset(&vbo, 0, sizeof(vbo));
    vbo.flags = (GX2RResourceFlags)(GX2R_RESOURCE_BIND_VERTEX_BUFFER |
                                    GX2R_RESOURCE_USAGE_CPU_WRITE |
                                    GX2R_RESOURCE_USAGE_GPU_READ);
    vbo.elemSize  = VERTEX_STRIDE;
    vbo.elemCount = TORUS_VERTS;
    GX2RCreateBuffer(&vbo);
    void *p = GX2RLockBufferEx(&vbo, (GX2RResourceFlags)0);
    memcpy(p, verts, TORUS_VERTS * VERTEX_STRIDE);
    GX2RUnlockBufferEx(&vbo, (GX2RResourceFlags)0);

    GX2RBuffer ibo;
    memset(&ibo, 0, sizeof(ibo));
    ibo.flags = (GX2RResourceFlags)(GX2R_RESOURCE_BIND_INDEX_BUFFER |
                                    GX2R_RESOURCE_USAGE_CPU_WRITE |
                                    GX2R_RESOURCE_USAGE_GPU_READ);
    ibo.elemSize  = sizeof(uint16_t);
    ibo.elemCount = TORUS_INDICES;
    GX2RCreateBuffer(&ibo);
    p = GX2RLockBufferEx(&ibo, (GX2RResourceFlags)0);
    memcpy(p, indices, TORUS_INDICES * sizeof(uint16_t));
    GX2RUnlockBufferEx(&ibo, (GX2RResourceFlags)0);

    free(verts);
    free(indices);

    TransformBlock *ubo = (TransformBlock *)CremaUniformAlloc(sizeof(TransformBlock));

    Mat4 proj = mat4_perspective(60.0f * 3.14159265f / 180.0f,
                                 16.0f / 9.0f, 0.1f, 100.0f);
    Vec3 eye = { 0.0f, 1.2f, 4.5f };
    Vec3 at  = { 0.0f, 0.0f, 0.0f };
    Vec3 up  = { 0.0f, 1.0f, 0.0f };
    Mat4 view = mat4_look_at(eye, at, up);
    Vec3 lightRaw = { -0.4f, -0.6f, -0.7f };
    Vec3 lightDir = vec3_normalize(lightRaw);

    uint64_t t0 = OSGetSystemTime();
    while (CremaAppRunning()) {
        float t = (float)((double)OSTicksToMilliseconds(OSGetSystemTime() - t0) / 1000.0);
        TransformBlock blk;
        blk.model = mat4_mul(mat4_rotate_y(t * 0.8f), mat4_rotate_x(t * 0.45f));
        blk.mvp   = mat4_mul(proj, mat4_mul(view, blk.model));
        blk.lightDir[0] = lightDir.x; blk.lightDir[1] = lightDir.y;
        blk.lightDir[2] = lightDir.z; blk.lightDir[3] = 0.0f;
        blk.eyePos[0] = eye.x; blk.eyePos[1] = eye.y;
        blk.eyePos[2] = eye.z; blk.eyePos[3] = 0.0f;
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
