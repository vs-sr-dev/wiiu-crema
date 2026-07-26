// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan

#include "crema_hud.h"

#include <gx2/registers.h>
#include <gx2r/draw.h>
#include <whb/log.h>

#include "crema_blend.h"
#include "crema_buffer.h"

// The shader that turns the packing described in the header into pixels. Two
// examples wrote this character for character before it moved here.
static const char *VS_HUD =
    "#version 450\n"
    "layout(location = 0) in vec2 aCorner;\n"    // 0..1 square
    "layout(binding = 0) uniform Hud {\n"
    "    vec4 uItem[512];\n"   // 256 x { x, y, w, glyph-or-negative-height }, rgba
    "};\n"
    "layout(location = 0) out vec2 vUV;\n"
    "layout(location = 1) out vec4 vTint;\n"
    "layout(location = 2) out float vSolid;\n"
    "layout(location = 3) out vec4 vCell;\n"   // the cell's texel-centre bounds
    "void main()\n"
    "{\n"
    "    vec4 it  = uItem[gl_InstanceID * 2];\n"
    "    vec4 col = uItem[gl_InstanceID * 2 + 1];\n"
    // a negative fourth component means "not a glyph": a solid rectangle whose
    // height is the magnitude. One sign bit separates text from geometry.
    "    float solid = it.w < 0.0 ? 1.0 : 0.0;\n"
    "    vec2 size = solid > 0.5 ? vec2(it.z, -it.w) : vec2(it.z, it.z);\n"
    "    vec2 pos = it.xy + aCorner * size;\n"
    "    gl_Position = vec4(pos.x / 640.0 - 1.0,\n"     // 640 = 1280/2
    "                       1.0 - pos.y / 360.0, 0.0, 1.0);\n"
    // the atlas is 8x8 cells starting at ASCII 32, so the cell is two
    // divisions of the index and there is no lookup table anywhere
    "    float idx = max(it.w, 0.0);\n"
    "    vec2 cell = vec2(mod(idx, 8.0), floor(idx * 0.125));\n"
    // The coordinate runs edge to edge, so the glyph fills its quad; the
    // fragment shader then clamps it inside the cell's outermost texel
    // CENTRES. Both halves are needed and neither alone works: stop at the
    // edges and the filter drags in the first row of the glyph in the next
    // cell down (a phantom underscore under half the alphabet); shrink the
    // coordinate to the centres instead and the glyph's own first row lands
    // under-weighted, which reads as a top row shaved off.
    "    vUV = (cell + aCorner) * 0.125;\n"          // 8 cells across the atlas
    "    vCell = (vec4(cell, cell) * 16.0 + vec4(0.5, 0.5, 15.5, 15.5))\n"
    "            * (1.0 / 128.0);\n"                 // 16 texels per cell, 128 wide
    "    vTint = col;\n"
    "    vSolid = solid;\n"
    "}\n";

static const char *PS_HUD =
    "#version 450\n"
    "layout(location = 0) in vec2 vUV;\n"
    "layout(location = 1) in vec4 vTint;\n"
    "layout(location = 2) in float vSolid;\n"
    "layout(location = 3) in vec4 vCell;\n"
    "layout(binding = 0) uniform sampler2D uFont;\n"
    "layout(location = 0) out vec4 oColor;\n"
    "void main()\n"
    "{\n"
    // clamped to the cell's outermost texel centres: the filter can reach the
    // edge of this glyph and never past it
    "    vec2 uv = clamp(vUV, vCell.xy, vCell.zw);\n"
    "    float mask = vSolid > 0.5 ? 1.0 : texture(uFont, uv).a;\n"
    "    oColor = vec4(vTint.rgb, vTint.a * mask);\n"
    "}\n";

// The square measured from its corner, because 2D layout puts things at a
// top-left and not around a middle.
static const float HUD_VERTS[] = { 0.0f, 0.0f,  1.0f, 0.0f,
                                   1.0f, 1.0f,  0.0f, 1.0f };
static const uint16_t HUD_TRIS[] = { 0, 1, 2, 0, 2, 3 };
#define HUD_STRIDE (2 * sizeof(float))

bool CremaHudRendererCreate(CremaHudRenderer *r)
{
    if (!r)
        return false;
    memset(r, 0, sizeof(*r));

    const CremaAttrib attrib[] = {
        { 0, 0, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32 },
    };
    r->shader = CremaShaderCompile(VS_HUD, PS_HUD, attrib, 1);
    if (!r->shader) {
        WHBLogPrintf("[hud] shader compile failed");
        return false;
    }
    if (!CremaBufferCreateVertex(&r->quad, HUD_STRIDE, 4, HUD_VERTS) ||
        !CremaBufferCreateIndexU16(&r->indices, 6, HUD_TRIS)) {
        WHBLogPrintf("[hud] could not take the quad");
        CremaShaderFree(r->shader);
        r->shader = NULL;
        return false;
    }

    r->blockLoc = CremaShaderVSBlockLocation(r->shader, "Hud");
    if (r->blockLoc < 0)
        r->blockLoc = 0;
    r->texUnit = 0;
    if (r->shader->ps->samplerVarCount > 0)
        r->texUnit = r->shader->ps->samplerVars[0].location;
    return true;
}

void CremaHudRendererDestroy(CremaHudRenderer *r)
{
    if (!r || !r->shader)
        return;
    CremaBufferDestroy(&r->quad);
    CremaBufferDestroy(&r->indices);
    CremaShaderFree(r->shader);
    memset(r, 0, sizeof(*r));
}

void CremaHudDraw(const CremaHudRenderer *r, const void *itemsUbo,
                  uint32_t count, const GX2Texture *font,
                  const GX2Sampler *sampler)
{
    if (!r || !r->shader || !itemsUbo || count == 0 || !font || !sampler)
        return;
    if (count > HUD_MAX_ITEMS)
        count = HUD_MAX_ITEMS;

    CremaBlendSet(CREMA_BLEND_ALPHA);
    CremaDepthSet(false, false);
    GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, FALSE);

    CremaShaderBind(r->shader);
    GX2SetVertexUniformBlock(r->blockLoc, CREMA_HUD_BLOCK_BYTES, itemsUbo);
    GX2SetPixelTexture(font, r->texUnit);
    GX2SetPixelSampler(sampler, r->texUnit);
    GX2RSetAttributeBuffer((GX2RBuffer *)&r->quad, 0, HUD_STRIDE, 0);
    GX2RDrawIndexed(GX2_PRIMITIVE_MODE_TRIANGLES, (GX2RBuffer *)&r->indices,
                    GX2_INDEX_TYPE_U16, 6, 0, 0, count);

    CremaBlendSet(CREMA_BLEND_OPAQUE);
    CremaDepthSet(true, true);
}
