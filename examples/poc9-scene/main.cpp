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
#include "crema_shader.h"
#include "crema_matrix.h"

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
#define GLOBAL_SLICE 256

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

static bool createTexture(GX2Texture *tex, GX2TileMode tileMode,
                          uint32_t size, uint32_t mipLevels)
{
    memset(tex, 0, sizeof(*tex));
    tex->surface.width     = size;
    tex->surface.height    = size;
    tex->surface.depth     = 1;
    tex->surface.mipLevels = mipLevels;
    tex->surface.format    = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    tex->surface.aa        = GX2_AA_MODE1X;
    tex->surface.use       = GX2_SURFACE_USE_TEXTURE;
    tex->surface.dim       = GX2_SURFACE_DIM_TEXTURE_2D;
    tex->surface.tileMode  = tileMode;
    tex->viewNumMips   = mipLevels;
    tex->viewNumSlices = 1;
    tex->compMap       = 0x00010203;
    GX2CalcSurfaceSizeAndAlignment(&tex->surface);
    GX2InitTextureRegs(tex);
    tex->surface.image = memalign(tex->surface.alignment, tex->surface.imageSize);
    if (!tex->surface.image)
        return false;
    memset(tex->surface.image, 0, tex->surface.imageSize);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE,
                  tex->surface.image, tex->surface.imageSize);
    if (tex->surface.mipmapSize > 0) {
        tex->surface.mipmaps = memalign(tex->surface.alignment,
                                        tex->surface.mipmapSize);
        if (!tex->surface.mipmaps)
            return false;
        memset(tex->surface.mipmaps, 0, tex->surface.mipmapSize);
        GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE,
                      tex->surface.mipmaps, tex->surface.mipmapSize);
    }
    return true;
}

// 2x2 box filter: src (size x size, tightly packed RGBA) -> dst (size/2)
static void mipReduce(const uint8_t *src, uint8_t *dst, uint32_t srcSize)
{
    uint32_t dstSize = srcSize / 2;
    for (uint32_t y = 0; y < dstSize; y++) {
        for (uint32_t x = 0; x < dstSize; x++) {
            for (int c = 0; c < 4; c++) {
                uint32_t sum =
                    src[((2*y)   * srcSize + 2*x)   * 4 + c] +
                    src[((2*y)   * srcSize + 2*x+1) * 4 + c] +
                    src[((2*y+1) * srcSize + 2*x)   * 4 + c] +
                    src[((2*y+1) * srcSize + 2*x+1) * 4 + c];
                dst[(y * dstSize + x) * 4 + c] = (uint8_t)(sum / 4);
            }
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

static GX2RBuffer makeBuffer(GX2RResourceFlags bind, uint32_t elemSize,
                             uint32_t elemCount, const void *data)
{
    GX2RBuffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.flags = (GX2RResourceFlags)(bind |
                                    GX2R_RESOURCE_USAGE_CPU_WRITE |
                                    GX2R_RESOURCE_USAGE_GPU_READ);
    buf.elemSize  = elemSize;
    buf.elemCount = elemCount;
    GX2RCreateBuffer(&buf);
    void *p = GX2RLockBufferEx(&buf, (GX2RResourceFlags)0);
    memcpy(p, data, (size_t)elemSize * elemCount);
    GX2RUnlockBufferEx(&buf, (GX2RResourceFlags)0);
    return buf;
}

static void drawScene(const CremaShader *shGround, const CremaShader *shObjects,
                      GX2RBuffer *groundVbo, GX2RBuffer *groundIbo,
                      GX2RBuffer *torusVbo, GX2RBuffer *torusIbo,
                      const GX2Texture *tex, const GX2Sampler *sampler,
                      uint32_t texUnit,
                      int32_t gLocG, int32_t gLocO, int32_t objLoc,
                      int32_t gPsLocG, int32_t gPsLocO,
                      const void *globalUbo, const void *objArray,
                      size_t objBytes)
{
    GX2SetDepthOnlyControl(TRUE, TRUE, GX2_COMPARE_FUNC_LESS);
    GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, TRUE);

    CremaShaderBind(shGround);
    GX2SetVertexUniformBlock(gLocG, sizeof(GlobalBlock), globalUbo);
    GX2SetPixelUniformBlock(gPsLocG, sizeof(GlobalBlock), globalUbo);
    GX2SetPixelTexture(tex, texUnit);
    GX2SetPixelSampler(sampler, texUnit);
    GX2RSetAttributeBuffer(groundVbo, 0, GROUND_STRIDE, 0);
    GX2RDrawIndexed(GX2_PRIMITIVE_MODE_TRIANGLES, groundIbo, GX2_INDEX_TYPE_U16,
                    6, 0, 0, 1);

    CremaShaderBind(shObjects);
    GX2SetVertexUniformBlock(gLocO, sizeof(GlobalBlock), globalUbo);
    GX2SetPixelUniformBlock(gPsLocO, sizeof(GlobalBlock), globalUbo);
    GX2SetVertexUniformBlock(objLoc, objBytes, objArray);
    GX2RSetAttributeBuffer(torusVbo, 0, TORUS_STRIDE, 0);
    GX2RDrawIndexed(GX2_PRIMITIVE_MODE_TRIANGLES, torusIbo, GX2_INDEX_TYPE_U16,
                    TORUS_INDICES, 0, 0, NUM_OBJECTS);
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
    GX2RBuffer torusVbo = makeBuffer(GX2R_RESOURCE_BIND_VERTEX_BUFFER,
                                     TORUS_STRIDE, TORUS_VERTS, tv);
    GX2RBuffer torusIbo = makeBuffer(GX2R_RESOURCE_BIND_INDEX_BUFFER,
                                     sizeof(uint16_t), TORUS_INDICES, ti);
    free(tv);
    free(ti);
    GX2RBuffer groundVbo = makeBuffer(GX2R_RESOURCE_BIND_VERTEX_BUFFER,
                                      GROUND_STRIDE, 4, GROUND_VERTS);
    GX2RBuffer groundIbo = makeBuffer(GX2R_RESOURCE_BIND_INDEX_BUFFER,
                                      sizeof(uint16_t), 6, GROUND_TRIS);

    // --- ground texture: full 9-level mip chain (256 -> 1), tiled ---
    // Without mips, looking down from altitude minifies the whole screen and
    // the per-pixel bilinear taps thrash the texture cache (measured: 60 -> 30
    // fps on real HW). Box-filter the chain on CPU, upload each level via a
    // linear staging texture + GX2CopySurface, sample trilinear.
    #define MIP_LEVELS 9
    GX2Texture texTiled;
    createTexture(&texTiled, GX2_TILE_MODE_DEFAULT, 256, MIP_LEVELS);

    uint8_t *mipData[MIP_LEVELS];
    mipData[0] = (uint8_t *)malloc(256 * 256 * 4);
    fillGroundTexture(mipData[0], 256);   // tightly packed at level 0
    for (int l = 1; l < MIP_LEVELS; l++) {
        uint32_t srcSize = 256u >> (l - 1);
        mipData[l] = (uint8_t *)malloc((srcSize / 2) * (srcSize / 2) * 4);
        mipReduce(mipData[l - 1], mipData[l], srcSize);
    }
    for (int l = 0; l < MIP_LEVELS; l++) {
        uint32_t size = 256u >> l;
        GX2Texture staging;
        createTexture(&staging, GX2_TILE_MODE_LINEAR_ALIGNED, size, 1);
        uint8_t *dst = (uint8_t *)staging.surface.image;
        for (uint32_t y = 0; y < size; y++)
            memcpy(dst + (size_t)y * staging.surface.pitch * 4,
                   mipData[l] + (size_t)y * size * 4, (size_t)size * 4);
        GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE,
                      staging.surface.image, staging.surface.imageSize);
        GX2CopySurface(&staging.surface, 0, 0, &texTiled.surface, l, 0);
        GX2Flush();
        GX2DrawDone();   // staging is freed right after: let the copy land
        free(staging.surface.image);
        free(mipData[l]);
    }
    WHBLogPrintf("[scene] ground texture ready: %d mip levels", MIP_LEVELS);

    GX2Sampler sampler;
    GX2InitSampler(&sampler, GX2_TEX_CLAMP_MODE_WRAP,
                   GX2_TEX_XY_FILTER_MODE_LINEAR);
    GX2InitSamplerZMFilter(&sampler, GX2_TEX_Z_FILTER_MODE_NONE,
                           GX2_TEX_MIP_FILTER_MODE_LINEAR);   // trilinear
    GX2InitSamplerLOD(&sampler, 0.0f, 13.0f, 0.0f);

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

    uint8_t *globalSlices = (uint8_t *)CremaUniformAlloc(2 * GLOBAL_SLICE);

    // --- camera state ---
    Vec3 camPos = { 0.0f, 6.0f, 60.0f };
    float yaw = 0.0f, pitch = -0.05f;
    Mat4 proj = mat4_perspective(60.0f * 3.14159265f / 180.0f,
                                 16.0f / 9.0f, 0.3f, 400.0f);
    Vec3 lightRaw = { -0.45f, -0.7f, -0.55f };
    Vec3 lightDir = vec3_normalize(lightRaw);

    GX2SetSwapInterval(1);   // this is a "game" now: rock-solid 59.94 Hz
    WHBLogPrintf("[scene] controls: L-stick move, R-stick look, ZR up, ZL down, B boost");

    uint32_t frameIdx = 0;
    OSTime fence[2] = { 0, 0 };
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

        // --- frame N-2 fence: its UBO slice is about to be reused ---
        uint32_t slot = frameIdx & 1;
        uint64_t syncWait = 0;
        if (fence[slot] != 0) {
            uint64_t w0 = OSGetSystemTime();
            GX2WaitTimeStamp(fence[slot]);
            syncWait += OSGetSystemTime() - w0;
        }

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
        uint8_t *globalUbo = globalSlices + slot * GLOBAL_SLICE;
        CremaUniformStore(globalUbo, &blk, sizeof(blk));

        WHBGfxBeginRender();

        WHBGfxBeginRenderTV();
        WHBGfxClearColor(0.55f, 0.70f, 0.85f, 1.0f);   // sky == fog colour
        drawScene(shGround, shObjects, &groundVbo, &groundIbo,
                  &torusVbo, &torusIbo, &texTiled, &sampler, texUnit,
                  gLocG, gLocO, objLoc, gPsLocG, gPsLocO,
                  globalUbo, objArray, sizeof(objData));
        WHBGfxFinishRenderTV();

        WHBGfxBeginRenderDRC();
        WHBGfxClearColor(0.55f, 0.70f, 0.85f, 1.0f);
        drawScene(shGround, shObjects, &groundVbo, &groundIbo,
                  &torusVbo, &torusIbo, &texTiled, &sampler, texUnit,
                  gLocG, gLocO, objLoc, gPsLocG, gPsLocO,
                  globalUbo, objArray, sizeof(objData));
        WHBGfxFinishRenderDRC();

        GX2SwapScanBuffers();
        GX2Flush();
        fence[slot] = GX2GetLastSubmittedTimeStamp();

        CremaFrameMarkManual(syncWait, &stats);
        frameIdx++;
    }

    GX2DrawDone();
    CremaUniformFreeBlock(globalSlices);
    CremaUniformFreeBlock(objArray);
    GX2RDestroyBufferEx(&groundIbo, (GX2RResourceFlags)0);
    GX2RDestroyBufferEx(&groundVbo, (GX2RResourceFlags)0);
    GX2RDestroyBufferEx(&torusIbo, (GX2RResourceFlags)0);
    GX2RDestroyBufferEx(&torusVbo, (GX2RResourceFlags)0);
    free(texTiled.surface.image);
    free(texTiled.surface.mipmaps);
    CremaShaderFree(shGround);
    CremaShaderFree(shObjects);
    CremaShaderShutdownCompiler();
    CremaAppShutdown();
    return 0;
}
