// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan
//
// GX2 PoC 7 — fill-rate / texture / pixel-ALU benchmark.
//   - one fullscreen triangle drawn OVERDRAW times per frame via instancing
//     (vsync off, depth test off so every layer shades). The render target is
//     whatever WHBGfx allocated from the console's display setting, asked for
//     at runtime rather than assumed — see the note by pixPerFrame.
//   - four pixel-shader modes rotate every 8 s:
//       A) flat colour            -> raw ROP fill rate (R7xx theoretical: 4.4 Gpix/s)
//       B) 1 bilinear tap, LINEAR -> cost of the "convenient" texture layout
//       C) 1 bilinear tap, TILED  -> quantifies the linear-vs-tiled penalty
//       D) TILED + animated per-pixel Blinn-Phong -> pixel ALU cost
//   - reports Gpix/s per mode

#include <gx2/draw.h>
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2/sampler.h>
#include <gx2/shaders.h>
#include <gx2/state.h>
#include <gx2/surface.h>
#include <gx2/swap.h>
#include <gx2/texture.h>
#include <gx2/event.h>
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

#define OVERDRAW 32
#define MODE_SECONDS 8

static const char *VS_SRC =
    "#version 450\n"
    "layout(location = 0) in vec2 aPosition;\n"
    "layout(location = 1) in vec2 aUV;\n"
    "layout(binding = 0) uniform Global { vec4 uTime; };\n"
    "layout(location = 0) out vec2 vUV;\n"
    "layout(location = 1) out float vTime;\n"
    "void main()\n"
    "{\n"
    "    gl_Position = vec4(aPosition, 0.0, 1.0);\n"
    "    vUV = aUV;\n"
    "    vTime = uTime.x;\n"
    "}\n";

static const char *PS_FLAT =
    "#version 450\n"
    "layout(location = 0) in vec2 vUV;\n"
    "layout(location = 1) in float vTime;\n"
    "layout(location = 0) out vec4 oColor;\n"
    "void main() { oColor = vec4(0.2, 0.3, 0.8, 1.0); }\n";

static const char *PS_TEX =
    "#version 450\n"
    "layout(location = 0) in vec2 vUV;\n"
    "layout(location = 1) in float vTime;\n"
    "layout(binding = 0) uniform sampler2D uTex;\n"
    "layout(location = 0) out vec4 oColor;\n"
    "void main() { oColor = texture(uTex, vUV); }\n";

static const char *PS_PHONG =
    "#version 450\n"
    "layout(location = 0) in vec2 vUV;\n"
    "layout(location = 1) in float vTime;\n"
    "layout(binding = 0) uniform sampler2D uTex;\n"
    "layout(location = 0) out vec4 oColor;\n"
    "void main()\n"
    "{\n"
    "    vec3 base = texture(uTex, vUV).rgb;\n"
    "    float t = vTime;\n"
    "    vec3 n = normalize(vec3(sin(vUV.x * 40.0 + t),\n"
    "                            cos(vUV.y * 40.0 + t), 1.5));\n"
    "    vec3 l = normalize(vec3(sin(t * 0.7), 0.6, cos(t * 0.7)));\n"
    "    float diff = max(dot(n, l), 0.0);\n"
    "    vec3 h = normalize(l + vec3(0.0, 0.0, 1.0));\n"
    "    float spec = pow(max(dot(n, h), 0.0), 32.0);\n"
    "    oColor = vec4(base * (0.15 + 0.85 * diff) + vec3(spec * 0.5), 1.0);\n"
    "}\n";

// fullscreen triangle: x, y, u, v
static const float FS_TRI[] = {
    -1.0f, -1.0f,  0.0f, 0.0f,
     3.0f, -1.0f,  2.0f, 0.0f,
    -1.0f,  3.0f,  0.0f, 2.0f,
};
#define VERTEX_STRIDE (4 * sizeof(float))

// 256x256 RGBA8 checkerboard with a gradient (bilinear clearly visible)
static void fillTexturePixels(uint8_t *dst, uint32_t pitchPixels)
{
    for (int y = 0; y < 256; y++) {
        uint8_t *row = dst + (size_t)y * pitchPixels * 4;
        for (int x = 0; x < 256; x++) {
            int check = ((x >> 4) ^ (y >> 4)) & 1;
            row[x * 4 + 0] = check ? (uint8_t)x : 40;
            row[x * 4 + 1] = check ? (uint8_t)y : 90;
            row[x * 4 + 2] = check ? 220 : (uint8_t)(255 - x);
            row[x * 4 + 3] = 255;
        }
    }
}

static bool createTexture(GX2Texture *tex, GX2TileMode tileMode)
{
    memset(tex, 0, sizeof(*tex));
    tex->surface.width     = 256;
    tex->surface.height    = 256;
    tex->surface.depth     = 1;
    tex->surface.mipLevels = 1;
    tex->surface.format    = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    tex->surface.aa        = GX2_AA_MODE1X;
    tex->surface.use       = GX2_SURFACE_USE_TEXTURE;
    tex->surface.dim       = GX2_SURFACE_DIM_TEXTURE_2D;
    tex->surface.tileMode  = tileMode;
    tex->viewNumMips   = 1;
    tex->viewNumSlices = 1;
    tex->compMap       = 0x00010203;   // RGBA straight through
    GX2CalcSurfaceSizeAndAlignment(&tex->surface);
    GX2InitTextureRegs(tex);
    tex->surface.image = memalign(tex->surface.alignment, tex->surface.imageSize);
    if (!tex->surface.image)
        return false;
    memset(tex->surface.image, 0, tex->surface.imageSize);
    // Flush the zeroed lines NOW: otherwise the CPU cache evicts them later,
    // clobbering whatever the GPU wrote in the meantime (black-pixel salt on
    // real HW after GX2CopySurface; Cemu doesn't emulate CPU caches).
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE,
                  tex->surface.image, tex->surface.imageSize);
    return true;
}

static const char *MODE_NAMES[4] = {
    "A:flat", "B:tex-linear", "C:tex-tiled", "D:tiled+phong"
};

int main(int argc, char **argv)
{
    if (!CremaAppInit("poc7-fillrate"))
        return -1;
    if (!CremaShaderInitCompiler()) {
        CremaAppShutdown();
        return -1;
    }

    const CremaAttrib attribs[] = {
        { 0, 0, 0,                 GX2_ATTRIB_FORMAT_FLOAT_32_32 },
        { 1, 0, 2 * sizeof(float), GX2_ATTRIB_FORMAT_FLOAT_32_32 },
    };
    CremaShader *shFlat  = CremaShaderCompile(VS_SRC, PS_FLAT,  attribs, 2);
    CremaShader *shTex   = CremaShaderCompile(VS_SRC, PS_TEX,   attribs, 2);
    CremaShader *shPhong = CremaShaderCompile(VS_SRC, PS_PHONG, attribs, 2);
    if (!shFlat || !shTex || !shPhong) {
        CremaShaderShutdownCompiler();
        CremaAppShutdown();
        return -1;
    }
    int32_t globalLoc = CremaShaderVSBlockLocation(shFlat, "Global");
    if (globalLoc < 0)
        globalLoc = 0;
    uint32_t texUnit = 0;
    if (shTex->ps->samplerVarCount > 0)
        texUnit = shTex->ps->samplerVars[0].location;

    // --- textures: linear (filled by CPU) + tiled (GPU-swizzled copy) ---
    GX2Texture texLinear, texTiled;
    if (!createTexture(&texLinear, GX2_TILE_MODE_LINEAR_ALIGNED) ||
        !createTexture(&texTiled, GX2_TILE_MODE_DEFAULT)) {
        WHBLogPrintf("[fill] texture alloc failed");
        CremaShaderShutdownCompiler();
        CremaAppShutdown();
        return -1;
    }
    fillTexturePixels((uint8_t *)texLinear.surface.image, texLinear.surface.pitch);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE,
                  texLinear.surface.image, texLinear.surface.imageSize);
    // one-shot GPU swizzle linear -> tiled, then wait for it
    GX2CopySurface(&texLinear.surface, 0, 0, &texTiled.surface, 0, 0);
    GX2Flush();
    GX2DrawDone();
    WHBLogPrintf("[fill] textures ready (linear pitch %u, tiled pitch %u)",
                 texLinear.surface.pitch, texTiled.surface.pitch);

    GX2Sampler sampler;
    GX2InitSampler(&sampler, GX2_TEX_CLAMP_MODE_WRAP,
                   GX2_TEX_XY_FILTER_MODE_LINEAR);

    // --- fullscreen triangle VBO ---
    GX2RBuffer vbo;
    memset(&vbo, 0, sizeof(vbo));
    vbo.flags = (GX2RResourceFlags)(GX2R_RESOURCE_BIND_VERTEX_BUFFER |
                                    GX2R_RESOURCE_USAGE_CPU_WRITE |
                                    GX2R_RESOURCE_USAGE_GPU_READ);
    vbo.elemSize  = VERTEX_STRIDE;
    vbo.elemCount = 3;
    GX2RCreateBuffer(&vbo);
    void *p = GX2RLockBufferEx(&vbo, (GX2RResourceFlags)0);
    memcpy(p, FS_TRI, sizeof(FS_TRI));
    GX2RUnlockBufferEx(&vbo, (GX2RResourceFlags)0);

    float *globalUbo = (float *)CremaUniformAlloc(4 * sizeof(float));

    GX2SetSwapInterval(0);

    // Asked, not assumed — and it was assumed from the day this was written.
    // The triangle is a
    // fullscreen one in clip space, so it covers whatever WHBGfx allocated,
    // and WHBGfx allocates from the console's own display setting: 1280x720
    // on a console set to 720p and 1920x1080 on one set to 1080p. Two
    // hard-coded numbers up here quietly divided every figure this benchmark
    // has ever published by 2.25.
    const GX2ColorBuffer *tvBuffer = WHBGfxGetTVColourBuffer();
    const double tvW = tvBuffer ? (double)tvBuffer->surface.width : 1280.0;
    const double tvH = tvBuffer ? (double)tvBuffer->surface.height : 720.0;
    // The two clears per frame (TV plus the GamePad's) are NOT counted, so
    // whatever comes out is if anything a floor rather than a ceiling.
    const double pixPerFrame = tvW * tvH * OVERDRAW;
    WHBLogPrintf("[fill] measuring into %.0fx%.0f at %dx overdraw = %.1f Mpix "
                 "per frame", tvW, tvH, OVERDRAW, pixPerFrame / 1e6);
    int mode = 0;
    uint64_t modeStart = OSGetSystemTime();
    uint64_t t0 = modeStart;

    while (CremaAppRunning()) {
        uint64_t nowTicks = OSGetSystemTime();
        if (OSTicksToSeconds(nowTicks - modeStart) >= MODE_SECONDS) {
            mode = (mode + 1) % 4;
            modeStart = nowTicks;
            WHBLogPrintf("[fill] ---- switching to mode %s ----", MODE_NAMES[mode]);
        }

        float tv[4] = {
            (float)((double)OSTicksToMilliseconds(nowTicks - t0) / 1000.0),
            0.0f, 0.0f, 0.0f
        };
        CremaUniformStore(globalUbo, tv, sizeof(tv));

        WHBGfxBeginRender();

        WHBGfxBeginRenderTV();
        WHBGfxClearColor(0.0f, 0.0f, 0.0f, 1.0f);

        const CremaShader *sh = (mode == 0) ? shFlat
                            : (mode == 3) ? shPhong : shTex;
        CremaShaderBind(sh);
        GX2SetDepthOnlyControl(FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS);
        GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, FALSE);
        GX2SetVertexUniformBlock(globalLoc, 4 * sizeof(float), globalUbo);
        if (mode >= 1) {
            const GX2Texture *tex = (mode == 1) ? &texLinear : &texTiled;
            GX2SetPixelTexture(tex, texUnit);
            GX2SetPixelSampler(&sampler, texUnit);
        }
        GX2RSetAttributeBuffer(&vbo, 0, VERTEX_STRIDE, 0);
        GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, 3, 0, OVERDRAW);

        WHBGfxFinishRenderTV();

        WHBGfxBeginRenderDRC();
        WHBGfxClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        WHBGfxFinishRenderDRC();

        CremaFrameStats stats;
        CremaFinishRenderAndMark(&stats);
        if (stats.updated) {
            double gpix = stats.fps * pixPerFrame / 1e9;
            WHBLogPrintf("[fill] %s | %dx overdraw | %.1f fps | %.2f Gpix/s",
                         MODE_NAMES[mode], OVERDRAW, stats.fps, gpix);
        }
    }

    CremaUniformFreeBlock(globalUbo);
    GX2RDestroyBufferEx(&vbo, (GX2RResourceFlags)0);
    free(texLinear.surface.image);
    free(texTiled.surface.image);
    CremaShaderFree(shFlat);
    CremaShaderFree(shTex);
    CremaShaderFree(shPhong);
    CremaShaderShutdownCompiler();
    CremaAppShutdown();
    return 0;
}
