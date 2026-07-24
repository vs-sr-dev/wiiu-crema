// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan
//
// GX2 PoC 5 — geometry throughput stress test.
//   - vsync OFF (GX2SetSwapInterval(0)): wall-clock fps == GPU throughput
//   - N Gouraud tori (6912 tris each), N doubling 1 -> 128 every 4 seconds
//   - one draw call + one 256-byte uniform-block slice per object (the
//     realistic engine pattern), stress scene on TV only, DRC just cleared
//   - reports Mtris/s per workload level

#include <gx2/draw.h>
#include <gx2/registers.h>
#include <gx2/shaders.h>
#include <gx2/swap.h>
#include <gx2r/buffer.h>
#include <gx2r/draw.h>
#include <whb/gfx.h>
#include <whb/log.h>
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
    vec4 uLightDir;
    vec4 uBaseColor;
};
layout(location = 0) out vec3 vColor;
void main()
{
    gl_Position = uMVP * vec4(aPosition, 1.0);
    vec3 n = normalize(mat3(uModel) * aNormal);
    float diff = max(dot(n, -uLightDir.xyz), 0.0);
    vColor = uBaseColor.rgb * (0.12 + 0.88 * diff);
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
#define TORUS_TRIS (TORUS_INDICES / 3)
#define VERTEX_STRIDE (6 * sizeof(float))

#define MAX_OBJECTS 512
#define UBO_SLICE   256   // >= sizeof(TransformBlock), keeps GX2 alignment
#define LEVEL_SECONDS 4

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
    float baseColor[4];
} TransformBlock;

int main(int argc, char **argv)
{
    if (!CremaAppInit("poc5-stress"))
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

    uint8_t *uboPool = (uint8_t *)CremaUniformAlloc(MAX_OBJECTS * UBO_SLICE);

    Mat4 proj = mat4_perspective(60.0f * 3.14159265f / 180.0f,
                                 16.0f / 9.0f, 0.1f, 200.0f);
    Vec3 lightRaw = { -0.4f, -0.6f, -0.7f };
    Vec3 lightDir = vec3_normalize(lightRaw);

    // vsync off: measure raw throughput, not the 60 Hz cap
    GX2SetSwapInterval(0);

    int level = 1;
    uint64_t levelStart = OSGetSystemTime();
    uint64_t t0 = levelStart;

    while (CremaAppRunning()) {
        uint64_t nowTicks = OSGetSystemTime();
        float t = (float)((double)OSTicksToMilliseconds(nowTicks - t0) / 1000.0);

        if (OSTicksToSeconds(nowTicks - levelStart) >= LEVEL_SECONDS) {
            if (level < MAX_OBJECTS) {
                level *= 2;
                WHBLogPrintf("[stress] level up -> %d tori (%d tris/frame)",
                             level, level * TORUS_TRIS);
            }
            levelStart = nowTicks;
        }

        // layout: square grid, camera pulled back to keep it in frame
        int cols = (int)ceilf(sqrtf((float)level));
        float spacing = 4.0f;
        float half = (cols - 1) * spacing * 0.5f;
        float camDist = 6.0f + (float)cols * spacing * 0.9f;
        Vec3 eye = { 0.0f, half * 0.6f, camDist };
        Vec3 at  = { 0.0f, 0.0f, 0.0f };
        Vec3 up  = { 0.0f, 1.0f, 0.0f };
        Mat4 view = mat4_look_at(eye, at, up);

        for (int i = 0; i < level; i++) {
            int gx = i % cols, gy = i / cols;
            float phase = (float)i * 0.7f;
            TransformBlock blk;
            Mat4 rot = mat4_mul(mat4_rotate_y(t * 0.8f + phase),
                                mat4_rotate_x(t * 0.45f + phase * 0.5f));
            Mat4 pos = mat4_translate(gx * spacing - half,
                                      half - gy * spacing, 0.0f);
            blk.model = mat4_mul(pos, rot);
            blk.mvp   = mat4_mul(proj, mat4_mul(view, blk.model));
            blk.lightDir[0] = lightDir.x; blk.lightDir[1] = lightDir.y;
            blk.lightDir[2] = lightDir.z; blk.lightDir[3] = 0.0f;
            blk.baseColor[0] = 0.3f + 0.7f * ((i * 37) % 100) / 100.0f;
            blk.baseColor[1] = 0.3f + 0.7f * ((i * 61) % 100) / 100.0f;
            blk.baseColor[2] = 0.3f + 0.7f * ((i * 89) % 100) / 100.0f;
            blk.baseColor[3] = 1.0f;
            CremaUniformStore(uboPool + i * UBO_SLICE, &blk, sizeof(blk));
        }

        WHBGfxBeginRender();

        WHBGfxBeginRenderTV();
        WHBGfxClearColor(0.02f, 0.02f, 0.08f, 1.0f);
        CremaShaderBind(shader);
        GX2SetDepthOnlyControl(TRUE, TRUE, GX2_COMPARE_FUNC_LESS);
        GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, TRUE);
        GX2RSetAttributeBuffer(&vbo, 0, VERTEX_STRIDE, 0);
        for (int i = 0; i < level; i++) {
            GX2SetVertexUniformBlock(blockLoc, sizeof(TransformBlock),
                                     uboPool + i * UBO_SLICE);
            GX2RDrawIndexed(GX2_PRIMITIVE_MODE_TRIANGLES, &ibo,
                            GX2_INDEX_TYPE_U16, TORUS_INDICES, 0, 0, 1);
        }
        WHBGfxFinishRenderTV();

        WHBGfxBeginRenderDRC();
        WHBGfxClearColor(0.02f, 0.02f, 0.08f, 1.0f);
        WHBGfxFinishRenderDRC();

        CremaFrameStats stats;
        CremaFinishRenderAndMark(&stats);
        if (stats.updated) {
            double mtris = stats.fps * level * TORUS_TRIS / 1e6;
            WHBLogPrintf("[stress] %d tori | %d tris/frame | %.1f fps | %.1f Mtris/s",
                         level, level * TORUS_TRIS, stats.fps, mtris);
        }
    }

    CremaUniformFreeBlock(uboPool);
    GX2RDestroyBufferEx(&ibo, (GX2RResourceFlags)0);
    GX2RDestroyBufferEx(&vbo, (GX2RResourceFlags)0);
    CremaShaderFree(shader);
    CremaShaderShutdownCompiler();
    CremaAppShutdown();
    return 0;
}
