// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan
//
// GX2 PoC 9 — a walkable scene: the engine pieces proven in PoC 1-8 finally
// assembled into something you can fly through.
//   - free camera on the GamePad: left stick moves, right stick looks,
//     ZR/ZL up/down, B = speed boost
//   - textured ground plane (tiled checkers) + 256 instanced Gouraud tori,
//     directional light, distance fog blending into the sky colour
//   - rendered on BOTH TV and GamePad, vsync ON, fenced pipeline (no
//     GX2DrawDone): the CPU never blocks on the GPU
//   - clean-room: every byte home-grown, no foreign engine code

#include <gx2/draw.h>
#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2/sampler.h>
#include <gx2/shaders.h>
#include <gx2/state.h>
#include <gx2/surface.h>
#include <gx2/swap.h>
#include <gx2/texture.h>
#include <gx2r/buffer.h>
#include <gx2r/draw.h>
#include <whb/gfx.h>
#include <whb/log.h>
#include <vpad/input.h>
#include <coreinit/time.h>
#include <malloc.h>
#include <string.h>
#include <math.h>

#include "crema_app.h"
#include "crema_buffer.h"
#include "crema_frame.h"
#include "crema_matrix.h"
#include "crema_shader.h"
#include "crema_texture.h"

// --- shaders -----------------------------------------------------------------

#define GLOBAL_UBO_DECL \
    "layout(binding = 0) uniform Global {\n" \
    "    mat4 uViewProj;\n" \
    "    vec4 uLightDir;\n"  \
    "    vec4 uCamPos;\n"    \
    "    vec4 uFogParam;\n"  /* x = start, y = end */ \
    "    vec4 uFogColor;\n"  \
    "    vec4 uTime;\n"      \
    "};\n"

static const char *VS_GROUND =
    "#version 450\n"
    "layout(location = 0) in vec3 aPosition;\n"
    "layout(location = 1) in vec2 aUV;\n"
    GLOBAL_UBO_DECL
    "layout(location = 0) out vec2 vUV;\n"
    "layout(location = 1) out vec3 vWorld;\n"
    "void main()\n"
    "{\n"
    "    gl_Position = uViewProj * vec4(aPosition, 1.0);\n"
    "    vUV = aUV;\n"
    "    vWorld = aPosition;\n"
    "}\n";

static const char *PS_GROUND =
    "#version 450\n"
    "layout(location = 0) in vec2 vUV;\n"
    "layout(location = 1) in vec3 vWorld;\n"
    GLOBAL_UBO_DECL
    "layout(binding = 0) uniform sampler2D uTex;\n"
    "layout(location = 0) out vec4 oColor;\n"
    "void main()\n"
    "{\n"
    "    vec3 base = texture(uTex, vUV).rgb;\n"
    "    float diff = max(dot(vec3(0.0, 1.0, 0.0), -uLightDir.xyz), 0.0);\n"
    "    vec3 lit = base * (0.25 + 0.75 * diff);\n"
    "    float dist = length(vWorld - uCamPos.xyz);\n"
    "    float fog = smoothstep(uFogParam.x, uFogParam.y, dist);\n"
    "    oColor = vec4(mix(lit, uFogColor.rgb, fog), 1.0);\n"
    "}\n";

static const char *VS_OBJECTS =
    "#version 450\n"
    "layout(location = 0) in vec3 aPosition;\n"
    "layout(location = 1) in vec3 aNormal;\n"
    GLOBAL_UBO_DECL
    "layout(binding = 1) uniform Objects {\n"
    "    vec4 uData[512];\n"   // 256 x { pos+phase, color }
    "};\n"
    "layout(location = 0) out vec3 vColor;\n"
    "layout(location = 1) out vec3 vWorld;\n"
    "void main()\n"
    "{\n"
    "    vec4 pp  = uData[gl_InstanceID * 2];\n"
    "    vec4 col = uData[gl_InstanceID * 2 + 1];\n"
    "    float a = uTime.x * 0.4 + pp.w;\n"
    "    float ca = cos(a), sa = sin(a);\n"
    "    vec3 p = vec3(ca*aPosition.x + sa*aPosition.z, aPosition.y,\n"
    "                  -sa*aPosition.x + ca*aPosition.z);\n"
    "    vec3 n = vec3(ca*aNormal.x + sa*aNormal.z, aNormal.y,\n"
    "                  -sa*aNormal.x + ca*aNormal.z);\n"
    "    vec3 world = p + pp.xyz;\n"
    "    gl_Position = uViewProj * vec4(world, 1.0);\n"
    "    float diff = max(dot(n, -uLightDir.xyz), 0.0);\n"
    "    vec3 v = normalize(uCamPos.xyz - world);\n"
    "    vec3 h = normalize(-uLightDir.xyz + v);\n"
    "    float spec = pow(max(dot(n, h), 0.0), 24.0);\n"
    "    vColor = col.rgb * (0.18 + 0.82 * diff) + vec3(0.4) * spec;\n"
    "    vWorld = world;\n"
    "}\n";

static const char *PS_OBJECTS =
    "#version 450\n"
    "layout(location = 0) in vec3 vColor;\n"
    "layout(location = 1) in vec3 vWorld;\n"
    GLOBAL_UBO_DECL
    "layout(location = 0) out vec4 oColor;\n"
    "void main()\n"
    "{\n"
    "    float dist = length(vWorld - uCamPos.xyz);\n"
    "    float fog = smoothstep(uFogParam.x, uFogParam.y, dist);\n"
    "    oColor = vec4(mix(vColor, uFogColor.rgb, fog), 1.0);\n"
    "}\n";

// --- geometry ------------------------------------------------------------------

#define SEG_MAJOR 48
#define SEG_MINOR 24
#define TORUS_VERTS (SEG_MAJOR * SEG_MINOR)
#define TORUS_INDICES (SEG_MAJOR * SEG_MINOR * 6)
#define TORUS_STRIDE (6 * sizeof(float))

#define NUM_OBJECTS 256
#define GROUND_HALF 200.0f
#define GROUND_UV   80.0f

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

// ground: x,z plane at y=0 — pos3 + uv2
static const float GROUND_VERTS[] = {
    -GROUND_HALF, 0.0f, -GROUND_HALF,  0.0f,      0.0f,
     GROUND_HALF, 0.0f, -GROUND_HALF,  GROUND_UV, 0.0f,
     GROUND_HALF, 0.0f,  GROUND_HALF,  GROUND_UV, GROUND_UV,
    -GROUND_HALF, 0.0f,  GROUND_HALF,  0.0f,      GROUND_UV,
};
static const uint16_t GROUND_TRIS[] = { 0, 2, 1, 0, 3, 2 };   // CCW from above
#define GROUND_STRIDE (5 * sizeof(float))

// two-green checkers, subtle variation
static void fillGroundTexture(uint8_t *dst, uint32_t pitchPixels)
{
    for (int y = 0; y < 256; y++) {
        uint8_t *row = dst + (size_t)y * pitchPixels * 4;
        for (int x = 0; x < 256; x++) {
            int check = ((x >> 5) ^ (y >> 5)) & 1;
            int n = ((x * 7 + y * 13) % 23);   // cheap deterministic dither
            row[x * 4 + 0] = (uint8_t)((check ? 60 : 44) + n);
            row[x * 4 + 1] = (uint8_t)((check ? 130 : 100) + n);
            row[x * 4 + 2] = (uint8_t)((check ? 55 : 40) + n);
            row[x * 4 + 3] = 255;
        }
    }
}

typedef struct {
    Mat4  viewProj;
    float lightDir[4];
    float camPos[4];
    float fogParam[4];
    float fogColor[4];
    float time[4];
} GlobalBlock;

// Everything the per-screen draw needs; CremaFrameDrawBoth replays it once for
// the TV and once for the GamePad.
typedef struct {
    const CremaShader *shGround;
    const CremaShader *shObjects;
    GX2RBuffer *groundVbo, *groundIbo;
    GX2RBuffer *torusVbo,  *torusIbo;
    const GX2Texture *tex;
    const GX2Sampler *sampler;
    uint32_t texUnit;
    int32_t  gLocG, gLocO, objLoc;      // vertex-shader block bindings
    int32_t  gPsLocG, gPsLocO;          // pixel-shader block bindings (fog)
    const void *globalUbo;
    const void *objArray;
    size_t   objBytes;
} SceneView;

static void drawScene(void *user)
{
    const SceneView *s = (const SceneView *)user;

    GX2SetDepthOnlyControl(TRUE, TRUE, GX2_COMPARE_FUNC_LESS);
    GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, TRUE);

    CremaShaderBind(s->shGround);
    GX2SetVertexUniformBlock(s->gLocG, sizeof(GlobalBlock), s->globalUbo);
    GX2SetPixelUniformBlock(s->gPsLocG, sizeof(GlobalBlock), s->globalUbo);
    GX2SetPixelTexture(s->tex, s->texUnit);
    GX2SetPixelSampler(s->sampler, s->texUnit);
    GX2RSetAttributeBuffer(s->groundVbo, 0, GROUND_STRIDE, 0);
    GX2RDrawIndexed(GX2_PRIMITIVE_MODE_TRIANGLES, s->groundIbo,
                    GX2_INDEX_TYPE_U16, 6, 0, 0, 1);

    CremaShaderBind(s->shObjects);
    GX2SetVertexUniformBlock(s->gLocO, sizeof(GlobalBlock), s->globalUbo);
    GX2SetPixelUniformBlock(s->gPsLocO, sizeof(GlobalBlock), s->globalUbo);
    GX2SetVertexUniformBlock(s->objLoc, s->objBytes, s->objArray);
    GX2RSetAttributeBuffer(s->torusVbo, 0, TORUS_STRIDE, 0);
    GX2RDrawIndexed(GX2_PRIMITIVE_MODE_TRIANGLES, s->torusIbo,
                    GX2_INDEX_TYPE_U16, TORUS_INDICES, 0, 0, NUM_OBJECTS);
}

int main(int argc, char **argv)
{
    if (!CremaAppInit("poc9-scene"))
        return -1;
    if (!CremaShaderInitCompiler()) {
        CremaAppShutdown();
        return -1;
    }

    const CremaAttrib groundAttribs[] = {
        { 0, 0, 0,                 GX2_ATTRIB_FORMAT_FLOAT_32_32_32 },
        { 1, 0, 3 * sizeof(float), GX2_ATTRIB_FORMAT_FLOAT_32_32 },
    };
    const CremaAttrib torusAttribs[] = {
        { 0, 0, 0,                 GX2_ATTRIB_FORMAT_FLOAT_32_32_32 },
        { 1, 0, 3 * sizeof(float), GX2_ATTRIB_FORMAT_FLOAT_32_32_32 },
    };
    CremaShader *shGround  = CremaShaderCompile(VS_GROUND, PS_GROUND, groundAttribs, 2);
    CremaShader *shObjects = CremaShaderCompile(VS_OBJECTS, PS_OBJECTS, torusAttribs, 2);
    if (!shGround || !shObjects) {
        CremaShaderShutdownCompiler();
        CremaAppShutdown();
        return -1;
    }
    int32_t gLocG   = CremaShaderVSBlockLocation(shGround, "Global");
    int32_t gLocO   = CremaShaderVSBlockLocation(shObjects, "Global");
    int32_t objLoc  = CremaShaderVSBlockLocation(shObjects, "Objects");
    // pixel shaders reference Global too (fog): separate PS binding namespace
    int32_t gPsLocG = CremaShaderPSBlockLocation(shGround, "Global");
    int32_t gPsLocO = CremaShaderPSBlockLocation(shObjects, "Global");
    if (gLocG < 0)   gLocG = 0;
    if (gLocO < 0)   gLocO = 0;
    if (objLoc < 0)  objLoc = 1;
    if (gPsLocG < 0) gPsLocG = 0;
    if (gPsLocO < 0) gPsLocO = 0;
    uint32_t texUnit = 0;
    if (shGround->ps->samplerVarCount > 0)
        texUnit = shGround->ps->samplerVars[0].location;

    // --- meshes ---
    float *tv = (float *)malloc(TORUS_VERTS * TORUS_STRIDE);
    uint16_t *ti = (uint16_t *)malloc(TORUS_INDICES * sizeof(uint16_t));
    buildTorus(tv, ti, 1.2f, 0.5f);
    GX2RBuffer torusVbo, torusIbo, groundVbo, groundIbo;
    CremaBufferCreateVertex(&torusVbo, TORUS_STRIDE, TORUS_VERTS, tv);
    CremaBufferCreateIndexU16(&torusIbo, TORUS_INDICES, ti);
    free(tv);
    free(ti);
    CremaBufferCreateVertex(&groundVbo, GROUND_STRIDE, 4, GROUND_VERTS);
    CremaBufferCreateIndexU16(&groundIbo, 6, GROUND_TRIS);

    // --- ground texture: tiled, full 9-level mip chain (256 -> 1) ---
    // Without mips, looking down from altitude minifies the whole screen and
    // the per-pixel bilinear taps thrash the texture cache (measured: 60 -> 30
    // fps on real HW). Crema box-filters the chain and uploads every level.
    #define GROUND_TEX_SIZE 256
    GX2Texture texTiled;
    CremaTextureCreate(&texTiled, GROUND_TEX_SIZE, GROUND_TEX_SIZE,
                       CremaTextureMipLevels(GROUND_TEX_SIZE, GROUND_TEX_SIZE),
                       GX2_TILE_MODE_DEFAULT);

    uint8_t *pixels = (uint8_t *)malloc(GROUND_TEX_SIZE * GROUND_TEX_SIZE * 4);
    fillGroundTexture(pixels, GROUND_TEX_SIZE);
    CremaTextureUploadWithMips(&texTiled, pixels);
    free(pixels);

    GX2Sampler sampler;
    CremaSamplerInitTrilinear(&sampler, GX2_TEX_CLAMP_MODE_WRAP);

    // --- static object field: ring-of-rings layout with jitter ---
    float objData[NUM_OBJECTS][8];
    for (int i = 0; i < NUM_OBJECTS; i++) {
        int gx = i % 16, gy = i / 16;
        float jx = (float)((i * 37) % 17) - 8.0f;
        float jz = (float)((i * 53) % 17) - 8.0f;
        objData[i][0] = (gx - 7.5f) * 22.0f + jx;
        objData[i][1] = 1.8f;
        objData[i][2] = (gy - 7.5f) * 22.0f + jz;
        objData[i][3] = (float)i * 0.7f;
        objData[i][4] = 0.35f + 0.65f * ((i * 37) % 100) / 100.0f;
        objData[i][5] = 0.35f + 0.65f * ((i * 61) % 100) / 100.0f;
        objData[i][6] = 0.35f + 0.65f * ((i * 89) % 100) / 100.0f;
        objData[i][7] = 1.0f;
    }
    uint8_t *objArray = (uint8_t *)CremaUniformAlloc(sizeof(objData));
    CremaUniformStore(objArray, objData, sizeof(objData));

    // per-frame globals: one byteswapped slice per frame in flight
    CremaUniformRing globals;
    CremaUniformRingCreate(&globals, sizeof(GlobalBlock), CREMA_FRAMES_IN_FLIGHT);

    // --- camera state ---
    Vec3 camPos = { 0.0f, 6.0f, 60.0f };
    float yaw = 0.0f, pitch = -0.05f;
    Mat4 proj = mat4_perspective(60.0f * 3.14159265f / 180.0f,
                                 16.0f / 9.0f, 0.3f, 400.0f);
    Vec3 lightRaw = { -0.45f, -0.7f, -0.55f };
    Vec3 lightDir = vec3_normalize(lightRaw);

    // this is a "game" now: fenced pipelining, presented at vsync — 59.94 Hz
    // with the CPU never blocking on the GPU.
    CremaFrame frame;
    CremaFrameInit(&frame, CREMA_PACING_FENCED, 1);
    WHBLogPrintf("[scene] controls: L-stick move, R-stick look, ZR up, ZL down, B boost");

    SceneView view3d;
    view3d.shGround  = shGround;
    view3d.shObjects = shObjects;
    view3d.groundVbo = &groundVbo;
    view3d.groundIbo = &groundIbo;
    view3d.torusVbo  = &torusVbo;
    view3d.torusIbo  = &torusIbo;
    view3d.tex       = &texTiled;
    view3d.sampler   = &sampler;
    view3d.texUnit   = texUnit;
    view3d.gLocG     = gLocG;
    view3d.gLocO     = gLocO;
    view3d.objLoc    = objLoc;
    view3d.gPsLocG   = gPsLocG;
    view3d.gPsLocO   = gPsLocO;
    view3d.objArray  = objArray;
    view3d.objBytes  = sizeof(objData);

    static const float SKY[4] = { 0.55f, 0.70f, 0.85f, 1.0f };   // == fog colour

    uint64_t prevTicks = OSGetSystemTime();
    uint64_t t0 = prevTicks;
    CremaFrameStats stats;

    while (CremaAppRunning()) {
        uint64_t nowTicks = OSGetSystemTime();
        float dt = (float)((double)OSTicksToMicroseconds(nowTicks - prevTicks) / 1e6);
        prevTicks = nowTicks;
        if (dt > 0.05f)
            dt = 0.05f;
        float t = (float)((double)OSTicksToMilliseconds(nowTicks - t0) / 1000.0);

        // --- input ---
        VPADStatus vpad;
        VPADReadError vpadErr;
        memset(&vpad, 0, sizeof(vpad));
        VPADRead(VPAD_CHAN_0, &vpad, 1, &vpadErr);
        if (vpadErr == VPAD_READ_SUCCESS) {
            float lx = vpad.leftStick.x,  ly = vpad.leftStick.y;
            float rx = vpad.rightStick.x, ry = vpad.rightStick.y;
            if (fabsf(lx) < 0.10f) lx = 0.0f;
            if (fabsf(ly) < 0.10f) ly = 0.0f;
            if (fabsf(rx) < 0.10f) rx = 0.0f;
            if (fabsf(ry) < 0.10f) ry = 0.0f;

            yaw   -= rx * 1.8f * dt;   // stick right = clockwise from above
            pitch += ry * 1.2f * dt;
            if (pitch >  1.45f) pitch =  1.45f;
            if (pitch < -1.45f) pitch = -1.45f;

            float speed = (vpad.hold & VPAD_BUTTON_B) ? 45.0f : 14.0f;
            float cp = cosf(pitch), sp = sinf(pitch);
            float cy = cosf(yaw),   sy = sinf(yaw);
            Vec3 fwd   = { -sy * cp, sp, -cy * cp };
            Vec3 right = {  cy, 0.0f, -sy };
            camPos.x += (fwd.x * ly + right.x * lx) * speed * dt;
            camPos.y += (fwd.y * ly + right.y * lx) * speed * dt;
            camPos.z += (fwd.z * ly + right.z * lx) * speed * dt;
            if (vpad.hold & VPAD_BUTTON_ZR) camPos.y += speed * dt;
            if (vpad.hold & VPAD_BUTTON_ZL) camPos.y -= speed * dt;
            if (camPos.y < 1.0f) camPos.y = 1.0f;
        }

        // waits for frame N-2 (whose uniform slice we are about to reuse)
        uint32_t slot = CremaFrameBegin(&frame);

        Mat4 view = mat4_mul(mat4_rotate_x(-pitch),
                    mat4_mul(mat4_rotate_y(-yaw),
                             mat4_translate(-camPos.x, -camPos.y, -camPos.z)));
        GlobalBlock blk;
        blk.viewProj = mat4_mul(proj, view);
        blk.lightDir[0] = lightDir.x; blk.lightDir[1] = lightDir.y;
        blk.lightDir[2] = lightDir.z; blk.lightDir[3] = 0.0f;
        blk.camPos[0] = camPos.x; blk.camPos[1] = camPos.y;
        blk.camPos[2] = camPos.z; blk.camPos[3] = 0.0f;
        blk.fogParam[0] = 80.0f; blk.fogParam[1] = 260.0f;
        blk.fogParam[2] = 0.0f;  blk.fogParam[3] = 0.0f;
        blk.fogColor[0] = 0.55f; blk.fogColor[1] = 0.70f;
        blk.fogColor[2] = 0.85f; blk.fogColor[3] = 1.0f;
        blk.time[0] = t; blk.time[1] = blk.time[2] = blk.time[3] = 0.0f;
        view3d.globalUbo = CremaUniformRingStore(&globals, slot, &blk, sizeof(blk));

        CremaFrameDrawBoth(SKY, drawScene, &view3d);
        CremaFrameEnd(&frame, &stats);
    }

    CremaFrameSettle(&frame);
    CremaUniformRingDestroy(&globals);
    CremaUniformFreeBlock(objArray);
    CremaBufferDestroy(&groundIbo);
    CremaBufferDestroy(&groundVbo);
    CremaBufferDestroy(&torusIbo);
    CremaBufferDestroy(&torusVbo);
    CremaTextureDestroy(&texTiled);
    CremaShaderFree(shGround);
    CremaShaderFree(shObjects);
    CremaShaderShutdownCompiler();
    CremaAppShutdown();
    return 0;
}
