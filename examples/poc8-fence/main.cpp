// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan
//
// GX2 PoC 8 — killing the per-frame GX2DrawDone. Same scene as PoC 6's
// winner (512 instanced Gouraud tori, ~1.18M tris, TV only, vsync off),
// two frame-pacing strategies rotating every 8 s:
//   A) drawdone-sync: WHBGfxFinishRender = full CPU/GPU sync every frame
//      (the pattern used by every PoC so far — the control group)
//   B) fenced-pipeline: GX2SwapScanBuffers + GX2Flush only; the CPU builds
//      frame N while the GPU draws N-1, throttled by GX2WaitTimeStamp on the
//      submit timestamp of frame N-2. Dynamic data (the global UBO) is
//      double-buffered so the CPU never rewrites memory the GPU still reads.

#include <gx2/draw.h>
#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2/shaders.h>
#include <gx2/state.h>
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

static const char *VS_SRC =
    "#version 450\n"
    "layout(location = 0) in vec3 aPosition;\n"
    "layout(location = 1) in vec3 aNormal;\n"
    "layout(binding = 0) uniform Global {\n"
    "    mat4 uViewProj;\n"
    "    vec4 uLightDir;\n"
    "    vec4 uTime;\n"
    "};\n"
    "layout(binding = 1) uniform Objects {\n"
    "    vec4 uData[1024];\n"
    "};\n"
    "layout(location = 0) out vec3 vColor;\n"
    "void main()\n"
    "{\n"
    "    vec4 pp  = uData[gl_InstanceID * 2];\n"
    "    vec4 col = uData[gl_InstanceID * 2 + 1];\n"
    "    float a = uTime.x * 0.8 + pp.w;\n"
    "    float b = uTime.x * 0.45 + pp.w * 0.5;\n"
    "    float ca = cos(a), sa = sin(a), cb = cos(b), sb = sin(b);\n"
    "    vec3 p1 = vec3(ca*aPosition.x + sa*aPosition.z, aPosition.y,\n"
    "                   -sa*aPosition.x + ca*aPosition.z);\n"
    "    vec3 p  = vec3(p1.x, cb*p1.y - sb*p1.z, sb*p1.y + cb*p1.z);\n"
    "    vec3 n1 = vec3(ca*aNormal.x + sa*aNormal.z, aNormal.y,\n"
    "                   -sa*aNormal.x + ca*aNormal.z);\n"
    "    vec3 n  = vec3(n1.x, cb*n1.y - sb*n1.z, sb*n1.y + cb*n1.z);\n"
    "    gl_Position = uViewProj * vec4(p + pp.xyz, 1.0);\n"
    "    float diff = max(dot(n, -uLightDir.xyz), 0.0);\n"
    "    vColor = col.rgb * (0.12 + 0.88 * diff);\n"
    "}\n";

static const char *PS_SRC =
    "#version 450\n"
    "layout(location = 0) in vec3 vColor;\n"
    "layout(location = 0) out vec4 oColor;\n"
    "void main() { oColor = vec4(vColor, 1.0); }\n";

#define SEG_MAJOR 48
#define SEG_MINOR 24
#define TORUS_VERTS (SEG_MAJOR * SEG_MINOR)
#define TORUS_INDICES (SEG_MAJOR * SEG_MINOR * 6)
#define TORUS_TRIS (TORUS_INDICES / 3)
#define VERTEX_STRIDE (6 * sizeof(float))

#define NUM_OBJECTS 512
#define MODE_SECONDS 8
#define GLOBAL_SLICE 256   // sizeof(GlobalBlock)=96, padded to UBO alignment

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
            o[0] = a; o[1] = c; o[2] = b;
            o[3] = a; o[4] = d; o[5] = c;
        }
    }
}

typedef struct {
    Mat4  viewProj;
    float lightDir[4];
    float time[4];
} GlobalBlock;

static const char *MODE_NAMES[2] = { "A:drawdone-sync", "B:fenced-pipeline" };

int main(int argc, char **argv)
{
    if (!CremaAppInit("poc8-fence"))
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
    int32_t globalLoc  = CremaShaderVSBlockLocation(shader, "Global");
    int32_t objectsLoc = CremaShaderVSBlockLocation(shader, "Objects");
    if (globalLoc < 0)  globalLoc = 0;
    if (objectsLoc < 0) objectsLoc = 1;

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

    // static per-object data, written once
    int cols = (int)ceilf(sqrtf((float)NUM_OBJECTS));
    float spacing = 4.0f;
    float half = (cols - 1) * spacing * 0.5f;
    float objData[NUM_OBJECTS][8];
    for (int i = 0; i < NUM_OBJECTS; i++) {
        int gx = i % cols, gy = i / cols;
        objData[i][0] = gx * spacing - half;
        objData[i][1] = half - gy * spacing;
        objData[i][2] = 0.0f;
        objData[i][3] = (float)i * 0.7f;
        objData[i][4] = 0.3f + 0.7f * ((i * 37) % 100) / 100.0f;
        objData[i][5] = 0.3f + 0.7f * ((i * 61) % 100) / 100.0f;
        objData[i][6] = 0.3f + 0.7f * ((i * 89) % 100) / 100.0f;
        objData[i][7] = 1.0f;
    }
    uint8_t *objArray = (uint8_t *)CremaUniformAlloc(sizeof(objData));
    CremaUniformStore(objArray, objData, sizeof(objData));

    // global UBO, DOUBLE buffered: frame N writes slice N&1, safe because the
    // fence guarantees frame N-2 (same slice) has fully retired.
    uint8_t *globalSlices = (uint8_t *)CremaUniformAlloc(2 * GLOBAL_SLICE);

    Mat4 proj = mat4_perspective(60.0f * 3.14159265f / 180.0f,
                                 16.0f / 9.0f, 0.1f, 300.0f);
    float camDist = 6.0f + (float)cols * spacing * 0.9f;
    Vec3 eye = { 0.0f, half * 0.6f, camDist };
    Vec3 at  = { 0.0f, 0.0f, 0.0f };
    Vec3 up  = { 0.0f, 1.0f, 0.0f };
    Mat4 viewProj = mat4_mul(proj, mat4_look_at(eye, at, up));
    Vec3 lightRaw = { -0.4f, -0.6f, -0.7f };
    Vec3 lightDir = vec3_normalize(lightRaw);

    GX2SetSwapInterval(0);

    int mode = 0;
    uint64_t modeStart = OSGetSystemTime();
    uint64_t t0 = modeStart;
    uint32_t frameIdx = 0;
    OSTime fence[2] = { 0, 0 };
    CremaFrameStats stats;

    while (CremaAppRunning()) {
        uint64_t nowTicks = OSGetSystemTime();
        if (OSTicksToSeconds(nowTicks - modeStart) >= MODE_SECONDS) {
            if (mode == 1) {
                // leaving the pipelined mode: settle the GPU once
                GX2DrawDone();
                fence[0] = fence[1] = 0;
            }
            mode = (mode + 1) % 2;
            modeStart = nowTicks;
            WHBLogPrintf("[fence] ---- switching to mode %s ----", MODE_NAMES[mode]);
        }

        uint32_t slot = frameIdx & 1;
        uint64_t syncWait = 0;

        if (mode == 1 && fence[slot] != 0) {
            // throttle: frame N-2 (same UBO slice) must be fully retired
            uint64_t w0 = OSGetSystemTime();
            GX2WaitTimeStamp(fence[slot]);
            syncWait += OSGetSystemTime() - w0;
        }

        float t = (float)((double)OSTicksToMilliseconds(nowTicks - t0) / 1000.0);
        GlobalBlock blk;
        blk.viewProj = viewProj;
        blk.lightDir[0] = lightDir.x; blk.lightDir[1] = lightDir.y;
        blk.lightDir[2] = lightDir.z; blk.lightDir[3] = 0.0f;
        blk.time[0] = t; blk.time[1] = blk.time[2] = blk.time[3] = 0.0f;
        uint8_t *globalUbo = globalSlices + slot * GLOBAL_SLICE;
        CremaUniformStore(globalUbo, &blk, sizeof(blk));

        if (mode == 0) {
            WHBGfxBeginRender();
        } else {
            // WHBGfxBeginRender's GX2WaitForFlip waits for EVERY flip: with two
            // frames in flight that pins the pipeline to the 59.94 Hz vblank.
            // Instead just keep at most 2 swaps outstanding.
            uint32_t swapCount, flipCount;
            OSTime lastFlip, lastVsync;
            uint64_t w0 = OSGetSystemTime();
            do {
                GX2GetSwapStatus(&swapCount, &flipCount, &lastFlip, &lastVsync);
            } while ((int32_t)(swapCount - flipCount) >= 2);
            syncWait += OSGetSystemTime() - w0;
        }

        WHBGfxBeginRenderTV();
        WHBGfxClearColor(0.02f, 0.02f, 0.08f, 1.0f);
        CremaShaderBind(shader);
        GX2SetDepthOnlyControl(TRUE, TRUE, GX2_COMPARE_FUNC_LESS);
        GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, TRUE);
        GX2SetVertexUniformBlock(globalLoc, sizeof(GlobalBlock), globalUbo);
        GX2SetVertexUniformBlock(objectsLoc, sizeof(objData), objArray);
        GX2RSetAttributeBuffer(&vbo, 0, VERTEX_STRIDE, 0);
        GX2RDrawIndexed(GX2_PRIMITIVE_MODE_TRIANGLES, &ibo, GX2_INDEX_TYPE_U16,
                        TORUS_INDICES, 0, 0, NUM_OBJECTS);
        WHBGfxFinishRenderTV();

        WHBGfxBeginRenderDRC();
        WHBGfxClearColor(0.02f, 0.02f, 0.08f, 1.0f);
        WHBGfxFinishRenderDRC();

        if (mode == 0) {
            uint64_t w0 = OSGetSystemTime();
            WHBGfxFinishRender();       // swap + flush + GX2DrawDone
            syncWait += OSGetSystemTime() - w0;
        } else {
            GX2SwapScanBuffers();
            GX2Flush();
            fence[slot] = GX2GetLastSubmittedTimeStamp();
        }

        CremaFrameMarkManual(syncWait, &stats);
        if (stats.updated) {
            double mtris = stats.fps * NUM_OBJECTS * TORUS_TRIS / 1e6;
            WHBLogPrintf("[fence] %s | %.1f fps | sync-wait %.2f ms | %.1f Mtris/s",
                         MODE_NAMES[mode], stats.fps, stats.drainMs, mtris);
        }
        frameIdx++;
    }

    GX2DrawDone();   // settle before tearing down buffers the GPU may read
    CremaUniformFreeBlock(globalSlices);
    CremaUniformFreeBlock(objArray);
    GX2RDestroyBufferEx(&ibo, (GX2RResourceFlags)0);
    GX2RDestroyBufferEx(&vbo, (GX2RResourceFlags)0);
    CremaShaderFree(shader);
    CremaShaderShutdownCompiler();
    CremaAppShutdown();
    return 0;
}
