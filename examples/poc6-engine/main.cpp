// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan
//
// GX2 PoC 6 — "engine-style" submission shootout. Same GPU workload as PoC 5
// level 512 (512 Gouraud tori, ~1.18M tris/frame, TV only, vsync off), but the
// animation lives in the vertex shader (global time + per-object phase), so
// all per-object data is STATIC. Three submission modes rotate every 8 s:
//   A) naive:        512 draw calls, one uniform-block bind each (PoC 5 style)
//   B) display list: the 512 draws recorded once, replayed with one call
//   C) instancing:   one draw with numInstances=512 + gl_InstanceID lookup
// Per frame the CPU only rewrites the 96-byte global block -> the fps deltas
// between modes measure pure submission overhead.

#include <gx2/draw.h>
#include <gx2/displaylist.h>
#include <gx2/mem.h>
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

// --- shaders -----------------------------------------------------------------
// Shared math: rotate around Y then X by time+phase, translate to grid slot,
// Lambert light — identical per-vertex cost in all three modes.

#define VS_BODY_MATH \
    "    float a = uTime.x * 0.8 + pp.w;\n" \
    "    float b = uTime.x * 0.45 + pp.w * 0.5;\n" \
    "    float ca = cos(a), sa = sin(a), cb = cos(b), sb = sin(b);\n" \
    "    vec3 p1 = vec3(ca*aPosition.x + sa*aPosition.z, aPosition.y,\n" \
    "                   -sa*aPosition.x + ca*aPosition.z);\n" \
    "    vec3 p  = vec3(p1.x, cb*p1.y - sb*p1.z, sb*p1.y + cb*p1.z);\n" \
    "    vec3 n1 = vec3(ca*aNormal.x + sa*aNormal.z, aNormal.y,\n" \
    "                   -sa*aNormal.x + ca*aNormal.z);\n" \
    "    vec3 n  = vec3(n1.x, cb*n1.y - sb*n1.z, sb*n1.y + cb*n1.z);\n" \
    "    gl_Position = uViewProj * vec4(p + pp.xyz, 1.0);\n" \
    "    float diff = max(dot(n, -uLightDir.xyz), 0.0);\n" \
    "    vColor = col.rgb * (0.12 + 0.88 * diff);\n"

static const char *VS_SINGLE =
    "#version 450\n"
    "layout(location = 0) in vec3 aPosition;\n"
    "layout(location = 1) in vec3 aNormal;\n"
    "layout(binding = 0) uniform Global {\n"
    "    mat4 uViewProj;\n"
    "    vec4 uLightDir;\n"
    "    vec4 uTime;\n"
    "};\n"
    "layout(binding = 1) uniform Object {\n"
    "    vec4 uPosPhase;\n"
    "    vec4 uColor;\n"
    "};\n"
    "layout(location = 0) out vec3 vColor;\n"
    "void main()\n"
    "{\n"
    "    vec4 pp  = uPosPhase;\n"
    "    vec4 col = uColor;\n"
    VS_BODY_MATH
    "}\n";

static const char *VS_INSTANCED =
    "#version 450\n"
    "layout(location = 0) in vec3 aPosition;\n"
    "layout(location = 1) in vec3 aNormal;\n"
    "layout(binding = 0) uniform Global {\n"
    "    mat4 uViewProj;\n"
    "    vec4 uLightDir;\n"
    "    vec4 uTime;\n"
    "};\n"
    "layout(binding = 1) uniform Objects {\n"
    "    vec4 uData[1024];\n"      // 512 x { posPhase, color }
    "};\n"
    "layout(location = 0) out vec3 vColor;\n"
    "void main()\n"
    "{\n"
    "    vec4 pp  = uData[gl_InstanceID * 2];\n"
    "    vec4 col = uData[gl_InstanceID * 2 + 1];\n"
    VS_BODY_MATH
    "}\n";

static const char *PS_SRC =
    "#version 450\n"
    "layout(location = 0) in vec3 vColor;\n"
    "layout(location = 0) out vec4 oColor;\n"
    "void main() { oColor = vec4(vColor, 1.0); }\n";

// --- mesh (same torus as PoC 4/5) ---------------------------------------------

#define SEG_MAJOR 48
#define SEG_MINOR 24
#define TORUS_VERTS (SEG_MAJOR * SEG_MINOR)
#define TORUS_INDICES (SEG_MAJOR * SEG_MINOR * 6)
#define TORUS_TRIS (TORUS_INDICES / 3)
#define VERTEX_STRIDE (6 * sizeof(float))

#define NUM_OBJECTS 512
#define UBO_SLICE 256
#define MODE_SECONDS 8

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

static const char *MODE_NAMES[3] = {
    "A:512-draws", "B:display-list", "C:instanced"
};

int main(int argc, char **argv)
{
    if (!CremaAppInit("poc6-engine"))
        return -1;
    if (!CremaShaderInitCompiler()) {
        CremaAppShutdown();
        return -1;
    }

    const CremaAttrib attribs[] = {
        { 0, 0, 0,                 GX2_ATTRIB_FORMAT_FLOAT_32_32_32 },
        { 1, 0, 3 * sizeof(float), GX2_ATTRIB_FORMAT_FLOAT_32_32_32 },
    };
    CremaShader *shSingle = CremaShaderCompile(VS_SINGLE, PS_SRC, attribs, 2);
    if (!shSingle) {
        CremaShaderShutdownCompiler();
        CremaAppShutdown();
        return -1;
    }
    // gl_InstanceID is the experimental bit: if CafeGLSL refuses it we still
    // run modes A/B and say so loudly in the log.
    CremaShader *shInst = CremaShaderCompile(VS_INSTANCED, PS_SRC, attribs, 2);
    if (!shInst)
        WHBLogPrintf("[engine] instanced shader failed to compile - mode C disabled");

    int32_t globalLocS = CremaShaderVSBlockLocation(shSingle, "Global");
    int32_t objectLoc  = CremaShaderVSBlockLocation(shSingle, "Object");
    int32_t globalLocI = shInst ? CremaShaderVSBlockLocation(shInst, "Global")  : -1;
    int32_t objectsLoc = shInst ? CremaShaderVSBlockLocation(shInst, "Objects") : -1;
    if (globalLocS < 0) globalLocS = 0;
    if (objectLoc  < 0) objectLoc  = 1;
    if (globalLocI < 0) globalLocI = 0;
    if (objectsLoc < 0) objectsLoc = 1;

    // --- geometry ---
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

    // --- static per-object data (written ONCE) ---
    int cols = (int)ceilf(sqrtf((float)NUM_OBJECTS));
    float spacing = 4.0f;
    float half = (cols - 1) * spacing * 0.5f;

    float objData[NUM_OBJECTS][8];   // posPhase + color, packed
    for (int i = 0; i < NUM_OBJECTS; i++) {
        int gx = i % cols, gy = i / cols;
        objData[i][0] = gx * spacing - half;
        objData[i][1] = half - gy * spacing;
        objData[i][2] = 0.0f;
        objData[i][3] = (float)i * 0.7f;             // phase
        objData[i][4] = 0.3f + 0.7f * ((i * 37) % 100) / 100.0f;
        objData[i][5] = 0.3f + 0.7f * ((i * 61) % 100) / 100.0f;
        objData[i][6] = 0.3f + 0.7f * ((i * 89) % 100) / 100.0f;
        objData[i][7] = 1.0f;
    }

    // modes A/B: one 256-byte slice per object
    uint8_t *objSlices = (uint8_t *)CremaUniformAlloc(NUM_OBJECTS * UBO_SLICE);
    for (int i = 0; i < NUM_OBJECTS; i++)
        CremaUniformStore(objSlices + i * UBO_SLICE, objData[i], 8 * sizeof(float));
    // mode C: one packed array block
    uint8_t *objArray = (uint8_t *)CremaUniformAlloc(NUM_OBJECTS * 8 * sizeof(float));
    CremaUniformStore(objArray, objData, sizeof(objData));

    GlobalBlock *globalUbo = (GlobalBlock *)CremaUniformAlloc(sizeof(GlobalBlock));

    // --- camera (fixed) ---
    Mat4 proj = mat4_perspective(60.0f * 3.14159265f / 180.0f,
                                 16.0f / 9.0f, 0.1f, 300.0f);
    float camDist = 6.0f + (float)cols * spacing * 0.9f;
    Vec3 eye = { 0.0f, half * 0.6f, camDist };
    Vec3 at  = { 0.0f, 0.0f, 0.0f };
    Vec3 up  = { 0.0f, 1.0f, 0.0f };
    Mat4 viewProj = mat4_mul(proj, mat4_look_at(eye, at, up));
    Vec3 lightRaw = { -0.4f, -0.6f, -0.7f };
    Vec3 lightDir = vec3_normalize(lightRaw);

    // --- record the display list for mode B (once) ---
    // Display-list-safe commands ONLY: uniform-block binds + raw indexed
    // draws. Anything that tracks CPU-side state (GX2SetShaderMode inside
    // CremaShaderBind, GX2R bookkeeping) hangs real hardware when replayed, so
    // shader/state/attribute binds happen outside, once per frame.
    uint32_t dlCapacity = 1024 * 1024;
    void *displayList = memalign(0x40, dlCapacity);   // GX2 display list alignment
    GX2BeginDisplayListEx(displayList, dlCapacity, TRUE);
    for (int i = 0; i < NUM_OBJECTS; i++) {
        GX2SetVertexUniformBlock(objectLoc, 8 * sizeof(float),
                                 objSlices + i * UBO_SLICE);
        GX2DrawIndexedEx(GX2_PRIMITIVE_MODE_TRIANGLES, TORUS_INDICES,
                         GX2_INDEX_TYPE_U16, ibo.buffer, 0, 1);
    }
    uint32_t dlSize = GX2EndDisplayList(displayList);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU, displayList, dlSize);
    WHBLogPrintf("[engine] display list recorded: %u bytes for %d draws",
                 dlSize, NUM_OBJECTS);

    GX2SetSwapInterval(0);

    int mode = 0;
    int numModes = shInst ? 3 : 2;
    bool modeBDisabled = false;
    int modeBStalls = 0;
    uint64_t modeStart = OSGetSystemTime();
    uint64_t t0 = modeStart;
    uint64_t submitTicks = 0;
    uint32_t submitFrames = 0;

    while (CremaAppRunning()) {
        uint64_t nowTicks = OSGetSystemTime();
        if (OSTicksToSeconds(nowTicks - modeStart) >= MODE_SECONDS) {
            do {
                mode = (mode + 1) % numModes;
            } while (mode == 1 && modeBDisabled);
            modeStart = nowTicks;
            WHBLogPrintf("[engine] ---- switching to mode %s ----", MODE_NAMES[mode]);
        }

        float t = (float)((double)OSTicksToMilliseconds(nowTicks - t0) / 1000.0);
        GlobalBlock blk;
        blk.viewProj = viewProj;
        blk.lightDir[0] = lightDir.x; blk.lightDir[1] = lightDir.y;
        blk.lightDir[2] = lightDir.z; blk.lightDir[3] = 0.0f;
        blk.time[0] = t; blk.time[1] = blk.time[2] = blk.time[3] = 0.0f;
        CremaUniformStore(globalUbo, &blk, sizeof(blk));

        WHBGfxBeginRender();

        uint64_t submitStart = OSGetSystemTime();
        WHBGfxBeginRenderTV();
        WHBGfxClearColor(0.02f, 0.02f, 0.08f, 1.0f);

        switch (mode) {
        case 0:   // A: naive 512 draws
            CremaShaderBind(shSingle);
            GX2SetDepthOnlyControl(TRUE, TRUE, GX2_COMPARE_FUNC_LESS);
            GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, TRUE);
            GX2SetVertexUniformBlock(globalLocS, sizeof(GlobalBlock), globalUbo);
            GX2RSetAttributeBuffer(&vbo, 0, VERTEX_STRIDE, 0);
            for (int i = 0; i < NUM_OBJECTS; i++) {
                GX2SetVertexUniformBlock(objectLoc, 8 * sizeof(float),
                                         objSlices + i * UBO_SLICE);
                GX2RDrawIndexed(GX2_PRIMITIVE_MODE_TRIANGLES, &ibo,
                                GX2_INDEX_TYPE_U16, TORUS_INDICES, 0, 0, 1);
            }
            break;
        case 1:   // B: per-frame state binds + one display-list call
            CremaShaderBind(shSingle);
            GX2SetDepthOnlyControl(TRUE, TRUE, GX2_COMPARE_FUNC_LESS);
            GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, TRUE);
            GX2SetVertexUniformBlock(globalLocS, sizeof(GlobalBlock), globalUbo);
            GX2RSetAttributeBuffer(&vbo, 0, VERTEX_STRIDE, 0);
            // Direct call injects the commands into the ring buffer instead
            // of an indirect-buffer call, which stalled the CP on real HW.
            GX2DirectCallDisplayList(displayList, dlSize);
            break;
        case 2:   // C: one instanced draw
            CremaShaderBind(shInst);
            GX2SetDepthOnlyControl(TRUE, TRUE, GX2_COMPARE_FUNC_LESS);
            GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, TRUE);
            GX2SetVertexUniformBlock(globalLocI, sizeof(GlobalBlock), globalUbo);
            GX2SetVertexUniformBlock(objectsLoc, NUM_OBJECTS * 8 * sizeof(float),
                                     objArray);
            GX2RSetAttributeBuffer(&vbo, 0, VERTEX_STRIDE, 0);
            GX2RDrawIndexed(GX2_PRIMITIVE_MODE_TRIANGLES, &ibo,
                            GX2_INDEX_TYPE_U16, TORUS_INDICES, 0, 0,
                            NUM_OBJECTS);
            break;
        }

        WHBGfxFinishRenderTV();

        WHBGfxBeginRenderDRC();
        WHBGfxClearColor(0.02f, 0.02f, 0.08f, 1.0f);
        WHBGfxFinishRenderDRC();
        submitTicks += OSGetSystemTime() - submitStart;
        submitFrames++;

        CremaFrameStats stats;
        CremaFinishRenderAndMark(&stats);
        if (stats.updated) {
            double submitMs = (double)OSTicksToMicroseconds(submitTicks)
                              / 1000.0 / submitFrames;
            double mtris = stats.fps * NUM_OBJECTS * TORUS_TRIS / 1e6;
            WHBLogPrintf("[engine] %s | %.1f fps | submit %.2f ms | %.1f Mtris/s",
                         MODE_NAMES[mode], stats.fps, submitMs, mtris);
            submitTicks = 0;
            submitFrames = 0;
            // Safety net: if the display-list path stalls the GPU on this
            // hardware, drop it from the rotation instead of wedging the app.
            if (mode == 1 && stats.drainMs > 100.0) {
                if (++modeBStalls >= 2) {
                    modeBDisabled = true;
                    WHBLogPrintf("[engine] mode B stalls the GPU here - disabled");
                }
            } else if (mode == 1) {
                modeBStalls = 0;
            }
        }
    }

    free(displayList);
    CremaUniformFreeBlock(globalUbo);
    CremaUniformFreeBlock(objSlices);
    CremaUniformFreeBlock(objArray);
    GX2RDestroyBufferEx(&ibo, (GX2RResourceFlags)0);
    GX2RDestroyBufferEx(&vbo, (GX2RResourceFlags)0);
    CremaShaderFree(shSingle);
    if (shInst)
        CremaShaderFree(shInst);
    CremaShaderShutdownCompiler();
    CremaAppShutdown();
    return 0;
}
