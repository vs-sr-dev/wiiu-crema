// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan

#include "crema_effect.h"

#include <gx2/registers.h>
#include <gx2r/draw.h>
#include <string.h>
#include <whb/log.h>

#include "crema_blend.h"
#include "crema_buffer.h"

void CremaEffectPoolInit(CremaEffectPool *pool, CremaEffect *storage,
                         uint32_t capacity)
{
    pool->items     = storage;
    pool->capacity  = capacity;
    pool->watermark = 0;
    memset(storage, 0, sizeof(CremaEffect) * capacity);
}

CremaEffect *CremaEffectSpawn(CremaEffectPool *pool, Vec3 pos, float life,
                              float sizeStart, float sizeEnd,
                              float r, float g, float b, float alpha)
{
    for (uint32_t i = 0; i < pool->capacity; i++) {
        CremaEffect *e = &pool->items[i];
        if (e->active)
            continue;
        memset(e, 0, sizeof(*e));
        e->pos        = pos;
        e->life       = life > 0.0f ? life : 0.001f;
        e->sizeStart  = sizeStart;
        e->sizeEnd    = sizeEnd;
        e->r = r; e->g = g; e->b = b;
        e->alphaStart = alpha;
        e->active     = true;
        if (i + 1 > pool->watermark)
            pool->watermark = i + 1;
        return e;
    }
    return NULL;
}

void CremaEffectUpdate(CremaEffectPool *pool, float dt)
{
    for (uint32_t i = 0; i < pool->watermark; i++) {
        CremaEffect *e = &pool->items[i];
        if (!e->active)
            continue;
        e->age += dt;
        if (e->age >= e->life) {
            e->active = false;
            continue;
        }
        e->pos.x += e->velocity.x * dt;
        e->pos.y += e->velocity.y * dt;
        e->pos.z += e->velocity.z * dt;
    }
}

uint32_t CremaEffectPack(const CremaEffectPool *pool, float (*out)[4],
                         uint32_t maxEffects)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < pool->watermark && n < maxEffects; i++) {
        const CremaEffect *e = &pool->items[i];
        if (!e->active)
            continue;
        float t = e->age / e->life;          // 0 at birth, 1 at death
        float size = e->sizeStart + (e->sizeEnd - e->sizeStart) * t;
        out[n * 2][0] = e->pos.x;
        out[n * 2][1] = e->pos.y;
        out[n * 2][2] = e->pos.z;
        out[n * 2][3] = size;
        out[n * 2 + 1][0] = e->r;
        out[n * 2 + 1][1] = e->g;
        out[n * 2 + 1][2] = e->b;
        out[n * 2 + 1][3] = e->alphaStart * (1.0f - t);   // fade out linearly
        n++;
    }
    return n;
}

// --- drawing them ------------------------------------------------------------

static const char *VS_FX =
    "#version 450\n"
    "layout(location = 0) in vec2 aCorner;\n"   // -1..1 square
    "layout(binding = 0) uniform View {\n"
    "    mat4 uViewProj;\n"
    "    vec4 uCamRight;\n"
    "    vec4 uCamUp;\n"
    "};\n"
    "layout(binding = 1) uniform Effects {\n"
    "    vec4 uFx[128];\n"   // 64 x { xyz = position, w = size }, { rgb, alpha }
    "};\n"
    "layout(location = 0) out vec2 vCorner;\n"
    "layout(location = 1) out vec4 vTint;\n"
    "void main()\n"
    "{\n"
    "    vec4 ps  = uFx[gl_InstanceID * 2];\n"
    "    vec4 col = uFx[gl_InstanceID * 2 + 1];\n"
    "    vec3 world = ps.xyz + uCamRight.xyz * (aCorner.x * ps.w)\n"
    "                        + uCamUp.xyz    * (aCorner.y * ps.w);\n"
    "    gl_Position = uViewProj * vec4(world, 1.0);\n"
    "    vCorner = aCorner;\n"
    "    vTint = col;\n"
    "}\n";

static const char *PS_FX =
    "#version 450\n"
    "layout(location = 0) in vec2 vCorner;\n"
    "layout(location = 1) in vec4 vTint;\n"
    "layout(location = 0) out vec4 oColor;\n"
    "void main()\n"
    "{\n"
    // a soft round blob computed from the corner coordinate: no sprite sheet,
    // no texture fetch, and it never shows its square edges
    "    float d = length(vCorner);\n"
    "    float falloff = 1.0 - smoothstep(0.15, 1.0, d);\n"
    "    oColor = vec4(vTint.rgb * falloff, vTint.a * falloff);\n"
    "}\n";

static const float FX_VERTS[] = { -1.0f, -1.0f,  1.0f, -1.0f,
                                   1.0f,  1.0f, -1.0f,  1.0f };
static const uint16_t FX_TRIS[] = { 0, 1, 2, 0, 2, 3 };
#define FX_STRIDE (2 * sizeof(float))

bool CremaEffectRendererCreate(CremaEffectRenderer *r)
{
    if (!r)
        return false;
    memset(r, 0, sizeof(*r));

    const CremaAttrib attrib[] = {
        { 0, 0, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32 },
    };
    r->shader = CremaShaderCompile(VS_FX, PS_FX, attrib, 1);
    if (!r->shader) {
        WHBLogPrintf("[effect] shader compile failed");
        return false;
    }
    if (!CremaBufferCreateVertex(&r->quad, FX_STRIDE, 4, FX_VERTS) ||
        !CremaBufferCreateIndexU16(&r->indices, 6, FX_TRIS)) {
        WHBLogPrintf("[effect] could not take the quad");
        CremaShaderFree(r->shader);
        r->shader = NULL;
        return false;
    }

    r->viewLoc = CremaShaderVSBlockLocation(r->shader, "View");
    r->fxLoc   = CremaShaderVSBlockLocation(r->shader, "Effects");
    if (r->viewLoc < 0) r->viewLoc = 0;
    if (r->fxLoc   < 0) r->fxLoc   = 1;
    return true;
}

void CremaEffectRendererDestroy(CremaEffectRenderer *r)
{
    if (!r || !r->shader)
        return;
    CremaBufferDestroy(&r->quad);
    CremaBufferDestroy(&r->indices);
    CremaShaderFree(r->shader);
    memset(r, 0, sizeof(*r));
}

void CremaEffectDraw(const CremaEffectRenderer *r, const void *viewUbo,
                     const void *fxUbo, uint32_t count)
{
    if (!r || !r->shader || !viewUbo || !fxUbo || count == 0)
        return;
    if (count > CREMA_EFFECT_MAX_DRAWN)
        count = CREMA_EFFECT_MAX_DRAWN;

    CremaBlendSet(CREMA_BLEND_ADDITIVE);
    CremaDepthSet(true, false);
    GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, FALSE);

    CremaShaderBind(r->shader);
    GX2SetVertexUniformBlock(r->viewLoc, sizeof(CremaEffectView), viewUbo);
    GX2SetVertexUniformBlock(r->fxLoc, CREMA_EFFECT_BLOCK_BYTES, fxUbo);
    GX2RSetAttributeBuffer((GX2RBuffer *)&r->quad, 0, FX_STRIDE, 0);
    GX2RDrawIndexed(GX2_PRIMITIVE_MODE_TRIANGLES, (GX2RBuffer *)&r->indices,
                    GX2_INDEX_TYPE_U16, 6, 0, 0, count);

    CremaBlendSet(CREMA_BLEND_OPAQUE);
    CremaDepthSet(true, true);
}
