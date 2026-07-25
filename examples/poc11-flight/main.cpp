// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan
//
// GX2 PoC 11 — flight: the first thing in Crema that is a game rather than a
// demonstration. You fly the ship; the camera chases it; four wingmen hold
// formation on you.
//
//   - arcade flight model: the stick rolls and pitches, and a banked turn
//     yaws you — the way a flight game feels, not the way physics works
//   - the wingmen cost nothing: they are four extra instances of the same
//     draw, offset in the ship's own local space, so they hold formation for
//     free without a line of AI
//   - textured ground and fog exist to sell speed: without a reference
//     surface, flying reads as hovering
//   - fenced pacing, TV + GamePad, 60 fps with the CPU idle
//   - and now it makes a noise: the guns, the hits, and an engine whose note
//     rises with the throttle — one looping sample, resampled by the DSP

#include <gx2/draw.h>
#include <gx2/registers.h>
#include <gx2/sampler.h>
#include <gx2/shaders.h>
#include <gx2/state.h>
#include <gx2/texture.h>
#include <gx2r/draw.h>
#include <whb/gfx.h>
#include <whb/log.h>
#include <vpad/input.h>
#include <coreinit/time.h>
#include <malloc.h>
#include <math.h>
#include <string.h>

#include "crema_app.h"
#include "crema_audio.h"
#include "crema_blend.h"
#include "crema_buffer.h"
#include "crema_effect.h"
#include "crema_collide.h"
#include "crema_entity.h"
#include "crema_frame.h"
#include "crema_input.h"
#include "crema_bank.h"
#include "crema_matrix.h"
#include "crema_mesh.h"
#include "crema_music.h"
#include "crema_pak.h"
#include "crema_shader.h"
#include "crema_texture.h"
// auxbus.h and not aux.h: Windows still reserves AUX as a device name, so a
// file called that cannot be opened by anything using the plain Win32 API —
// git included, which notices long after a Docker-hosted compiler did not.
#include "auxbus.h"     // our own code inside AX's signal path
#include "echo.h"       // and the effect it runs, which knows no console at all
#include "hud.h"        // the readout: quads in screen space, no console either

#define NUM_WINGMEN 4
#define NUM_SHIPS   (1 + NUM_WINGMEN)
#define GROUND_HALF 1200.0f
#define GROUND_UV   240.0f
#define GROUND_TEX  256

// --- shaders -----------------------------------------------------------------

#define GLOBAL_UBO_DECL \
    "layout(binding = 0) uniform Global {\n" \
    "    mat4 uViewProj;\n" \
    "    mat4 uModel;\n"    /* the player's orientation: wingmen ride it too */ \
    "    vec4 uLightDir;\n" \
    "    vec4 uCamPos;\n"   \
    "    vec4 uFogParam;\n" \
    "    vec4 uFogColor;\n" \
    "    vec4 uTime;\n"     \
    "    vec4 uGroundOffset;\n" \
    "    vec4 uCamRight;\n" \
    "    vec4 uCamUp;\n"    \
    "};\n"

static const char *VS_SHIP =
    "#version 450\n"
    "layout(location = 0) in vec3 aPosition;\n"
    "layout(location = 1) in vec3 aNormal;\n"
    "layout(location = 2) in vec2 aUV;\n"
    GLOBAL_UBO_DECL
    "layout(binding = 1) uniform Formation {\n"
    "    vec4 uSlot[8];\n"   // xyz = offset in ship-local space, w = bob phase
    "};\n"
    "layout(location = 0) out vec2 vUV;\n"
    "layout(location = 1) out vec3 vNormal;\n"
    "layout(location = 2) out vec3 vWorld;\n"
    "void main()\n"
    "{\n"
    "    vec4 slot = uSlot[gl_InstanceID];\n"
    // wingmen drift gently so the formation does not look welded together
    "    float bob = slot.w == 0.0 ? 0.0 : sin(uTime.x * 1.1 + slot.w) * 0.5;\n"
    "    vec3 local = aPosition + slot.xyz + vec3(0.0, bob, 0.0);\n"
    "    vec4 world = uModel * vec4(local, 1.0);\n"
    "    gl_Position = uViewProj * world;\n"
    "    vUV = aUV;\n"
    "    vNormal = mat3(uModel) * aNormal;\n"
    "    vWorld = world.xyz;\n"
    "}\n";

static const char *PS_SHIP =
    "#version 450\n"
    "layout(location = 0) in vec2 vUV;\n"
    "layout(location = 1) in vec3 vNormal;\n"
    "layout(location = 2) in vec3 vWorld;\n"
    GLOBAL_UBO_DECL
    "layout(binding = 0) uniform sampler2D uHull;\n"
    "layout(location = 0) out vec4 oColor;\n"
    "void main()\n"
    "{\n"
    "    vec3 base = texture(uHull, vUV).rgb;\n"
    "    vec3 n = normalize(vNormal);\n"
    "    float diff = max(dot(n, -uLightDir.xyz), 0.0);\n"
    "    vec3 v = normalize(uCamPos.xyz - vWorld);\n"
    "    vec3 h = normalize(-uLightDir.xyz + v);\n"
    "    float spec = pow(max(dot(n, h), 0.0), 32.0);\n"
    "    vec3 lit = base * (0.28 + 0.72 * diff) + vec3(0.5) * spec;\n"
    "    float fog = smoothstep(uFogParam.x, uFogParam.y,\n"
    "                           length(vWorld - uCamPos.xyz));\n"
    "    oColor = vec4(mix(lit, uFogColor.rgb, fog), 1.0);\n"
    "}\n";

// The rival squadron: same mesh, but each one carries its own position and
// heading instead of riding the player's matrix.
static const char *VS_ENEMY =
    "#version 450\n"
    "layout(location = 0) in vec3 aPosition;\n"
    "layout(location = 1) in vec3 aNormal;\n"
    "layout(location = 2) in vec2 aUV;\n"
    GLOBAL_UBO_DECL
    "layout(binding = 1) uniform Enemies {\n"
    "    vec4 uEnemy[32];\n"   // xyz = position, w = heading
    "};\n"
    "layout(location = 0) out vec2 vUV;\n"
    "layout(location = 1) out vec3 vNormal;\n"
    "layout(location = 2) out vec3 vWorld;\n"
    "void main()\n"
    "{\n"
    "    vec4 e = uEnemy[gl_InstanceID];\n"
    "    float cy = cos(e.w), sy = sin(e.w);\n"
    "    vec3 p = vec3(cy * aPosition.x + sy * aPosition.z, aPosition.y,\n"
    "                  -sy * aPosition.x + cy * aPosition.z);\n"
    "    vec3 n = vec3(cy * aNormal.x + sy * aNormal.z, aNormal.y,\n"
    "                  -sy * aNormal.x + cy * aNormal.z);\n"
    "    vec3 world = p + e.xyz;\n"
    "    gl_Position = uViewProj * vec4(world, 1.0);\n"
    "    vUV = aUV;\n"
    "    vNormal = n;\n"
    "    vWorld = world;\n"
    "}\n";

// Billboards: a quad spun to face the camera using the camera's own right and
// up axes. One draw covers tracers, muzzle flashes and explosions — they only
// differ by colour, size and how long they live.
static const char *VS_FX =
    "#version 450\n"
    "layout(location = 0) in vec2 aCorner;\n"   // -1..1 square
    GLOBAL_UBO_DECL
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

// The HUD: one instanced quad per letter, bar and blip. It reads its position
// in a virtual 1280x720 space and maps that to clip space itself, so the same
// layout lands identically on the 720p TV and the 854x480 GamePad — the screens
// differ in resolution, not in where things are.
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

static const char *VS_GROUND =
    "#version 450\n"
    "layout(location = 0) in vec3 aPosition;\n"
    "layout(location = 1) in vec2 aUV;\n"
    GLOBAL_UBO_DECL
    "layout(location = 0) out vec2 vUV;\n"
    "layout(location = 1) out vec3 vWorld;\n"
    "void main()\n"
    "{\n"
    // The ground follows the player, snapped to a whole texture tile so the
    // move is invisible: a finite quad you can fly off, or an endless world —
    // this is the entire difference, and it costs one add.
    "    vec3 world = aPosition + uGroundOffset.xyz;\n"
    "    gl_Position = uViewProj * vec4(world, 1.0);\n"
    "    vUV = aUV;\n"
    "    vWorld = world;\n"
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
    "    vec3 lit = base * (0.3 + 0.7 * diff);\n"
    "    float fog = smoothstep(uFogParam.x, uFogParam.y,\n"
    "                           length(vWorld - uCamPos.xyz));\n"
    "    oColor = vec4(mix(lit, uFogColor.rgb, fog), 1.0);\n"
    "}\n";

typedef struct {
    Mat4  viewProj;
    Mat4  model;
    float lightDir[4];
    float camPos[4];
    float fogParam[4];
    float fogColor[4];
    float time[4];
    float groundOffset[4];
    float camRight[4];
    float camUp[4];
} GlobalBlock;

// --- billboard quad -----------------------------------------------------------

#define MAX_EFFECTS 64
static const float FX_VERTS[] = { -1.0f, -1.0f,  1.0f, -1.0f,
                                   1.0f,  1.0f, -1.0f,  1.0f };
static const uint16_t FX_TRIS[] = { 0, 1, 2, 0, 2, 3 };
#define FX_STRIDE (2 * sizeof(float))

// The HUD quad is the same square measured from its corner instead of its
// centre: 2D layout puts things at a top-left, not around a middle.
static const float HUD_VERTS[] = { 0.0f, 0.0f,  1.0f, 0.0f,
                                   1.0f, 1.0f,  0.0f, 1.0f };
#define HUD_STRIDE (2 * sizeof(float))

// --- ground ------------------------------------------------------------------

static const float GROUND_VERTS[] = {
    -GROUND_HALF, 0.0f, -GROUND_HALF,  0.0f,      0.0f,
     GROUND_HALF, 0.0f, -GROUND_HALF,  GROUND_UV, 0.0f,
     GROUND_HALF, 0.0f,  GROUND_HALF,  GROUND_UV, GROUND_UV,
    -GROUND_HALF, 0.0f,  GROUND_HALF,  0.0f,      GROUND_UV,
};
static const uint16_t GROUND_TRIS[] = { 0, 2, 1, 0, 3, 2 };
#define GROUND_STRIDE (5 * sizeof(float))

// A ground you can read speed against: strong grid lines, quiet fill.
static void fillGroundTexture(uint8_t *dst)
{
    for (int y = 0; y < GROUND_TEX; y++) {
        for (int x = 0; x < GROUND_TEX; x++) {
            int edge = (x < 3 || y < 3);
            int patch = ((x >> 6) ^ (y >> 6)) & 1;
            int n = ((x * 13 + y * 7) % 17);
            uint8_t r, g, b;
            if (edge) {
                r = 96; g = 116; b = 128;
            } else {
                r = (uint8_t)((patch ? 46 : 38) + n);
                g = (uint8_t)((patch ? 74 : 62) + n);
                b = (uint8_t)((patch ? 58 : 48) + n);
            }
            uint8_t *px = dst + ((size_t)y * GROUND_TEX + x) * 4;
            px[0] = r; px[1] = g; px[2] = b; px[3] = 255;
        }
    }
}

// --- scene draw ---------------------------------------------------------------

typedef struct {
    const CremaShader *shShip, *shGround, *shEnemy;
    const CremaMesh   *ship;
    GX2RBuffer *groundVbo, *groundIbo;
    const GX2Texture *hull, *ground;
    const GX2Sampler *sampler;
    uint32_t hullUnit, groundUnit, enemyUnit;
    int32_t  gVsShip, gPsShip, formationLoc;
    int32_t  gVsGround, gPsGround;
    int32_t  gVsEnemy, gPsEnemy, enemyLoc;
    const void *globalUbo;
    const void *formation;
    size_t   formationBytes;
    const void *enemyUbo;
    size_t   enemyBytes;
    uint32_t enemyCount;
    const CremaShader *shFx;
    GX2RBuffer *fxVbo, *fxIbo;
    int32_t  gVsFx, fxLoc;
    const void *fxUbo;
    size_t   fxBytes;
    uint32_t fxCount;
    // the HUD, which is the only thing that differs between the two screens
    const CremaShader *shHud;
    GX2RBuffer *hudVbo, *hudIbo;
    int32_t  hudLoc;
    uint32_t fontUnit;
    const GX2Texture *font;
    const GX2Sampler *fontSampler;
    const void *hudUbo;
    size_t   hudBytes;
    uint32_t hudCount;
} SceneView;

static void drawWorld(void *user)
{
    const SceneView *s = (const SceneView *)user;

    CremaDepthSet(true, true);
    CremaBlendSet(CREMA_BLEND_OPAQUE);
    GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, TRUE);

    CremaShaderBind(s->shGround);
    GX2SetVertexUniformBlock(s->gVsGround, sizeof(GlobalBlock), s->globalUbo);
    GX2SetPixelUniformBlock(s->gPsGround, sizeof(GlobalBlock), s->globalUbo);
    GX2SetPixelTexture(s->ground, s->groundUnit);
    GX2SetPixelSampler(s->sampler, s->groundUnit);
    GX2RSetAttributeBuffer(s->groundVbo, 0, GROUND_STRIDE, 0);
    GX2RDrawIndexed(GX2_PRIMITIVE_MODE_TRIANGLES, s->groundIbo,
                    GX2_INDEX_TYPE_U16, 6, 0, 0, 1);

    CremaShaderBind(s->shShip);
    GX2SetVertexUniformBlock(s->gVsShip, sizeof(GlobalBlock), s->globalUbo);
    GX2SetPixelUniformBlock(s->gPsShip, sizeof(GlobalBlock), s->globalUbo);
    GX2SetVertexUniformBlock(s->formationLoc, s->formationBytes, s->formation);
    GX2SetPixelTexture(s->hull, s->hullUnit);
    GX2SetPixelSampler(s->sampler, s->hullUnit);
    CremaMeshDraw(s->ship, NUM_SHIPS);

    if (s->enemyCount > 0) {
        CremaShaderBind(s->shEnemy);
        GX2SetVertexUniformBlock(s->gVsEnemy, sizeof(GlobalBlock), s->globalUbo);
        GX2SetPixelUniformBlock(s->gPsEnemy, sizeof(GlobalBlock), s->globalUbo);
        GX2SetVertexUniformBlock(s->enemyLoc, s->enemyBytes, s->enemyUbo);
        GX2SetPixelTexture(s->hull, s->enemyUnit);
        GX2SetPixelSampler(s->sampler, s->enemyUnit);
        CremaMeshDraw(s->ship, s->enemyCount);
    }

    // Transparent pass, last: additive, depth test on, depth writes OFF so
    // overlapping billboards do not carve holes in each other. Culling off
    // too — a camera-facing quad has no reliable winding.
    if (s->fxCount > 0) {
        CremaBlendSet(CREMA_BLEND_ADDITIVE);
        CremaDepthSet(true, false);
        GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, FALSE);

        CremaShaderBind(s->shFx);
        GX2SetVertexUniformBlock(s->gVsFx, sizeof(GlobalBlock), s->globalUbo);
        GX2SetVertexUniformBlock(s->fxLoc, s->fxBytes, s->fxUbo);
        GX2RSetAttributeBuffer(s->fxVbo, 0, FX_STRIDE, 0);
        GX2RDrawIndexed(GX2_PRIMITIVE_MODE_TRIANGLES, s->fxIbo,
                        GX2_INDEX_TYPE_U16, 6, 0, 0, s->fxCount);

        CremaBlendSet(CREMA_BLEND_OPAQUE);
        CremaDepthSet(true, true);
    }
}

// The HUD pass: alpha blended, no depth at all, culling off. It is the last
// thing drawn and it is drawn over whatever is there — on the TV that is the
// world, on the GamePad it is an empty tactical screen.
static void drawHud(void *user)
{
    const SceneView *s = (const SceneView *)user;
    if (s->hudCount == 0)
        return;

    CremaBlendSet(CREMA_BLEND_ALPHA);
    CremaDepthSet(false, false);
    GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, FALSE);

    CremaShaderBind(s->shHud);
    GX2SetVertexUniformBlock(s->hudLoc, s->hudBytes, s->hudUbo);
    GX2SetPixelTexture(s->font, s->fontUnit);
    GX2SetPixelSampler(s->fontSampler, s->fontUnit);
    GX2RSetAttributeBuffer(s->hudVbo, 0, HUD_STRIDE, 0);
    GX2RDrawIndexed(GX2_PRIMITIVE_MODE_TRIANGLES, s->hudIbo,
                    GX2_INDEX_TYPE_U16, 6, 0, 0, s->hudCount);

    CremaBlendSet(CREMA_BLEND_OPAQUE);
    CremaDepthSet(true, true);
}

// --- the rival squadron -------------------------------------------------------

#define ENEMY_KIND   1
#define ENEMY_COUNT  12
#define MAX_ENEMIES  32
#define ENEMY_SPEED  34.0f
#define WORLD_HALF   900.0f
#define GUN_RANGE    900.0f

// Deterministic, so a run is reproducible when something goes wrong.
static float nextRandom(uint32_t *state)
{
    *state = (*state * 1664525u) + 1013904223u;
    return (float)((*state >> 8) & 0xFFFF) / 65535.0f;
}

// Positions are relative to the player, because the world has no centre: the
// ground follows you, so "somewhere in the level" only means "somewhere near".
static void spawnEnemy(CremaEntityPool *pool, uint32_t *rng, float radius,
                       Vec3 around)
{
    CremaEntity *e = CremaEntitySpawn(pool, ENEMY_KIND);
    if (!e)
        return;   // pool full: the caller decides whether that matters
    e->pos.x  = around.x + (nextRandom(rng) * 2.0f - 1.0f) * WORLD_HALF;
    e->pos.z  = around.z + (nextRandom(rng) * 2.0f - 1.0f) * WORLD_HALF;
    e->pos.y  = 25.0f + nextRandom(rng) * 220.0f;
    e->yaw    = nextRandom(rng) * 6.2831853f;
    e->radius = radius;
}

// --- flight model -------------------------------------------------------------

typedef struct {
    Vec3  pos;
    float yaw, pitch, roll;
    float speed;
} Flight;

#define SPEED_MIN   28.0f
#define SPEED_CRUSE 55.0f
#define SPEED_MAX   120.0f

static void flightUpdate(Flight *f, const CremaInput *in, float dt)
{
    float stickX = in->leftX;
    float stickY = in->leftY;
    float throttle = 0.0f;
    if (CremaInputHeld(in, VPAD_BUTTON_ZR)) throttle += 1.0f;
    if (CremaInputHeld(in, VPAD_BUTTON_ZL)) throttle -= 1.0f;

    // Roll follows the stick and springs back to level when released — the
    // player commands an attitude, not a torque. That is what makes an arcade
    // flight model feel responsive instead of floaty.
    float rollTarget = -stickX * 1.05f;
    f->roll += (rollTarget - f->roll) * fminf(1.0f, 4.0f * dt);

    // Inverted, like a stick: push forward to dive, pull back to climb.
    f->pitch -= stickY * 1.15f * dt;
    if (f->pitch >  1.15f) f->pitch =  1.15f;
    if (f->pitch < -1.15f) f->pitch = -1.15f;
    if (stickY == 0.0f)                       // level out slowly, hands off
        f->pitch -= f->pitch * fminf(1.0f, 0.8f * dt);

    // A banked turn: roll is what steers, exactly as in a real aircraft and in
    // every arcade flight game since.
    f->yaw += sinf(f->roll) * 1.15f * dt;

    float target = SPEED_CRUSE + throttle * (throttle > 0.0f
                   ? (SPEED_MAX - SPEED_CRUSE) : (SPEED_CRUSE - SPEED_MIN));
    f->speed += (target - f->speed) * fminf(1.0f, 1.6f * dt);

    float cp = cosf(f->pitch), sp = sinf(f->pitch);
    float cy = cosf(f->yaw),   sy = sinf(f->yaw);
    Vec3 fwd = { -sy * cp, sp, -cy * cp };
    f->pos.x += fwd.x * f->speed * dt;
    f->pos.y += fwd.y * f->speed * dt;
    f->pos.z += fwd.z * f->speed * dt;

    if (f->pos.y < 6.0f) {          // no crashing yet: skim the deck
        f->pos.y = 6.0f;
        if (f->pitch < 0.0f)
            f->pitch *= 0.85f;
    }
    if (f->pos.y > 420.0f)
        f->pos.y = 420.0f;
}

// --- the readouts -------------------------------------------------------------

#define RADAR_RANGE 900.0f

// The TV gets what you need while looking through the windscreen, and nothing
// else: numbers pushed into the corners, a crosshair where the guns point, and
// a bracket on the thing you are about to hit. Anything more competes with the
// view, which is the one thing the main screen is for.
static void buildTvHud(HudList *hud, const Flight *f, uint32_t score,
                       uint32_t collisions, const CremaEntity *target,
                       float targetDist, Vec3 aimPoint, Mat4 viewProj)
{
    const float CYAN[3] = { 0.55f, 0.88f, 1.0f };
    const float AMBER[3] = { 1.0f, 0.74f, 0.24f };

    hudClear(hud);

    hudText(hud, 40.0f, 38.0f, 20.0f, "SCORE", CYAN[0], CYAN[1], CYAN[2], 0.9f);
    hudNumber(hud, 130.0f, 32.0f, 28.0f, score, 3, 1.0f, 1.0f, 1.0f, 1.0f);
    hudText(hud, 40.0f, 76.0f, 20.0f, "HITS", 1.0f, 0.45f, 0.35f, 0.9f);
    hudNumber(hud, 130.0f, 74.0f, 22.0f, collisions, 2, 1.0f, 0.6f, 0.5f, 1.0f);

    hudText(hud, 1040.0f, 38.0f, 20.0f, "SPD", CYAN[0], CYAN[1], CYAN[2], 0.9f);
    hudNumber(hud, 1105.0f, 32.0f, 28.0f, (uint32_t)f->speed, 3,
              1.0f, 1.0f, 1.0f, 1.0f);
    hudText(hud, 1040.0f, 76.0f, 20.0f, "ALT", CYAN[0], CYAN[1], CYAN[2], 0.9f);
    hudNumber(hud, 1105.0f, 74.0f, 22.0f, (uint32_t)f->pos.y, 3,
              1.0f, 1.0f, 1.0f, 1.0f);

    hudText(hud, 40.0f, 626.0f, 18.0f, "THR", CYAN[0], CYAN[1], CYAN[2], 0.85f);
    hudBar(hud, 40.0f, 654.0f, 220.0f, 16.0f,
           (f->speed - SPEED_MIN) / (SPEED_MAX - SPEED_MIN), 0.3f, 0.85f, 1.0f);

    // The crosshair goes where the shot lands, which is NOT the centre of the
    // screen: the camera sits behind and above the ship, so the ray out of the
    // nose projects lower than the middle. Aim at a point on the ray itself and
    // the reticle sits on the gun instead of on the lens — the same reason a
    // real gunsight is bore-sighted to a range rather than to the windscreen.
    float ax = 640.0f, ay = 360.0f;
    mat4_project(viewProj, aimPoint, HUD_VIRTUAL_W, HUD_VIRTUAL_H, &ax, &ay);
    // four ticks with a hole in the middle: a solid cross would cover the one
    // pixel you are actually aiming at
    hudRect(hud, ax - 24.0f, ay - 1.0f, 16.0f, 2.0f, 1, 1, 1, 0.85f);
    hudRect(hud, ax +  8.0f, ay - 1.0f, 16.0f, 2.0f, 1, 1, 1, 0.85f);
    hudRect(hud, ax -  1.0f, ay - 24.0f, 2.0f, 16.0f, 1, 1, 1, 0.85f);
    hudRect(hud, ax -  1.0f, ay +  8.0f, 2.0f, 16.0f, 1, 1, 1, 0.85f);

    // The lock: the same projection the vertex shader does, done once on the
    // CPU, so a 2D bracket lands exactly on a 3D ship.
    if (target) {
        float sx, sy;
        if (mat4_project(viewProj, target->pos, HUD_VIRTUAL_W, HUD_VIRTUAL_H,
                         &sx, &sy)) {
            // the bracket shrinks with distance, like the thing it is on
            float half = 46.0f / (1.0f + targetDist / 220.0f) + 12.0f;
            hudFrame(hud, sx - half, sy - half, half * 2.0f, half * 2.0f, 2.0f,
                     AMBER[0], AMBER[1], AMBER[2], 0.95f);
            hudNumber(hud, sx - 24.0f, sy + half + 6.0f, 18.0f,
                      (uint32_t)targetDist, 3, AMBER[0], AMBER[1], AMBER[2], 0.9f);
            hudText(hud, sx + 22.0f, sy + half + 6.0f, 18.0f, "M",
                    AMBER[0], AMBER[1], AMBER[2], 0.9f);
        }
    }
}

// The GamePad gets the job the TV cannot do: the picture from above. This is
// the whole Star Fox Zero lesson taken the other way round — the second screen
// is worth having when it shows something the first one physically cannot, and
// worthless when it shows the same thing again. Here it costs no 3D pass at
// all: the tactical screen is quads on a dark field.
static void buildDrcHud(HudList *hud, const Flight *f,
                        const CremaEntityPool *enemies, uint32_t score,
                        uint32_t collisions, const CremaEntity *target)
{
    const float CYAN[3] = { 0.45f, 0.85f, 1.0f };
    const float DIM[3]  = { 0.16f, 0.42f, 0.55f };

    hudClear(hud);
    hudText(hud, 40.0f, 30.0f, 26.0f, "TACTICAL", CYAN[0], CYAN[1], CYAN[2], 1.0f);

    const float cx = 640.0f, cy = 380.0f, half = 250.0f;
    hudFrame(hud, cx - half, cy - half, half * 2.0f, half * 2.0f, 2.0f,
             DIM[0], DIM[1], DIM[2], 0.9f);
    hudFrame(hud, cx - half * 0.5f, cy - half * 0.5f, half, half, 1.0f,
             DIM[0], DIM[1], DIM[2], 0.5f);
    hudRect(hud, cx - half, cy - 1.0f, half * 2.0f, 1.0f,
            DIM[0], DIM[1], DIM[2], 0.35f);
    hudRect(hud, cx - 1.0f, cy - half, 1.0f, half * 2.0f,
            DIM[0], DIM[1], DIM[2], 0.35f);

    uint32_t contacts = 0;
    for (uint32_t i = 0; i < enemies->watermark; i++) {
        const CremaEntity *e = &enemies->items[i];
        if (!e->active)
            continue;
        // into the ship's own frame: forward is up, which is the only way a
        // radar is readable while you are turning
        float rx = e->pos.x - f->pos.x;
        float rz = e->pos.z - f->pos.z;
        float cyaw = cosf(f->yaw), syaw = sinf(f->yaw);
        float u =  rx * cyaw - rz * syaw;
        float v = -rx * syaw - rz * cyaw;
        if (u < -RADAR_RANGE || u > RADAR_RANGE ||
            v < -RADAR_RANGE || v > RADAR_RANGE)
            continue;
        contacts++;
        float bx = cx + (u / RADAR_RANGE) * half;
        float by = cy - (v / RADAR_RANGE) * half;
        if (e == target) {
            hudFrame(hud, bx - 9.0f, by - 9.0f, 18.0f, 18.0f, 2.0f,
                     1.0f, 0.74f, 0.24f, 1.0f);
            hudRect(hud, bx - 3.0f, by - 3.0f, 6.0f, 6.0f, 1.0f, 0.74f, 0.24f, 1.0f);
        } else {
            hudRect(hud, bx - 4.0f, by - 4.0f, 8.0f, 8.0f, 1.0f, 0.35f, 0.3f, 0.95f);
        }
    }

    // you, always in the middle, always pointing up
    hudRect(hud, cx - 4.0f, cy - 4.0f, 8.0f, 8.0f, 1.0f, 1.0f, 1.0f, 1.0f);
    hudRect(hud, cx - 1.0f, cy - 22.0f, 2.0f, 18.0f, 1.0f, 1.0f, 1.0f, 0.8f);

    hudText(hud, 40.0f, 120.0f, 20.0f, "SCORE", CYAN[0], CYAN[1], CYAN[2], 0.9f);
    hudNumber(hud, 40.0f, 148.0f, 30.0f, score, 3, 1.0f, 1.0f, 1.0f, 1.0f);
    hudText(hud, 40.0f, 200.0f, 20.0f, "HITS", 1.0f, 0.45f, 0.35f, 0.9f);
    hudNumber(hud, 40.0f, 228.0f, 30.0f, collisions, 2, 1.0f, 0.6f, 0.5f, 1.0f);
    hudText(hud, 40.0f, 280.0f, 20.0f, "CONTACTS", CYAN[0], CYAN[1], CYAN[2], 0.9f);
    hudNumber(hud, 40.0f, 308.0f, 30.0f, contacts, 2, 1.0f, 1.0f, 1.0f, 1.0f);

    hudText(hud, 970.0f, 120.0f, 20.0f, "SPD", CYAN[0], CYAN[1], CYAN[2], 0.9f);
    hudNumber(hud, 970.0f, 148.0f, 30.0f, (uint32_t)f->speed, 3, 1, 1, 1, 1);
    hudText(hud, 970.0f, 200.0f, 20.0f, "ALT", CYAN[0], CYAN[1], CYAN[2], 0.9f);
    hudNumber(hud, 970.0f, 228.0f, 30.0f, (uint32_t)f->pos.y, 3, 1, 1, 1, 1);
    hudText(hud, 970.0f, 280.0f, 20.0f, "RANGE", CYAN[0], CYAN[1], CYAN[2], 0.9f);
    hudNumber(hud, 970.0f, 308.0f, 30.0f, (uint32_t)RADAR_RANGE, 3, 1, 1, 1, 1);

    hudText(hud, 40.0f, 668.0f, 18.0f, "L-STICK FLY   ZR/ZL THROTTLE   A FIRE",
            DIM[0] + 0.2f, DIM[1] + 0.2f, DIM[2] + 0.2f, 0.9f);
}

int main(int argc, char **argv)
{
    if (!CremaAppInit("poc11-flight"))
        return -1;
    // First thing after the app is up, before a single asset is read: this is
    // what takes the audio hardware away from the Wii U Menu's music.
    CremaAudioInit();
    // Measured, not chosen. With every voice mixed at unity the meter on aux
    // bus 1 read 187-192% of full scale just idling — four music channels, an
    // engine and the odd explosion, all perfectly reasonable on their own,
    // adding up to nearly twice what a sample can hold. That is the clipping
    // you hear when the lead comes in and you fire at the same moment.
    //
    // 0.40 brought the dry mix down to 40-66%, but the meter only sees the
    // voices: the echo's return is added afterwards, and at 0.40 its peak was
    // another 10738 on top of a dry 21561 — 98% of full scale if the two ever
    // line up. 0.35 keeps the sum under 90% with the effect running, which is
    // what a headroom is for.
    CremaAudioSetHeadroom(0.35f);
    if (auxInit())
        auxSetEnabled(true);    // on from the start; Y is the A/B against dry
    if (!CremaShaderInitCompiler()) {
        CremaAppShutdown();
        return -1;
    }

    // Three assets, one archive, two reads. PoC 10 measures why; this one just
    // takes the answer and gets on with being a game.
    CremaMesh ship;
    GX2Texture hull, font;
    CremaBank bank;
    memset(&bank, 0, sizeof(bank));
    CremaMusic *music = NULL, *chiproll = NULL;
    bool assetsOk = false;
    {
        CremaPak pak;
        if (CremaPakOpen(&pak, "/vol/content/assets.cpak")) {
            size_t meshBytes = 0, hullBytes = 0, fontBytes = 0, bankBytes = 0;
            const void *meshBlob = CremaPakFind(&pak, "ship.cmesh", &meshBytes);
            const void *hullBlob = CremaPakFind(&pak, "hull.ctex", &hullBytes);
            const void *fontBlob = CremaPakFind(&pak, "font.ctex", &fontBytes);
            const void *bankBlob = CremaPakFind(&pak, "audio.cbank", &bankBytes);
            size_t songBytes = 0;
            const void *songBlob = CremaPakFind(&pak, "theme.csong", &songBytes);
            assetsOk = meshBlob && hullBlob && fontBlob && bankBlob &&
                CremaMeshLoadFromMemory(&ship, meshBlob, meshBytes, "ship.cmesh") &&
                CremaTextureLoadFromMemory(&hull, hullBlob, hullBytes, "hull.ctex") &&
                CremaTextureLoadFromMemory(&font, fontBlob, fontBytes, "font.ctex") &&
                CremaBankLoadFromMemory(&bank, bankBlob, bankBytes);
            if (assetsOk && songBlob)
                CremaMusicLoadFromMemory(&music, songBlob, songBytes, &bank);
            // The second song is the same pipeline from the other end: written
            // in chiproll, exported, imported. It carries the NES's own
            // out-of-tuneness note by note, which is the whole reason to
            // import from a chip editor rather than retype the notes.
            size_t chipBytes = 0;
            const void *chipBlob = CremaPakFind(&pak, "chiproll.csong",
                                                &chipBytes);
            if (assetsOk && chipBlob)
                CremaMusicLoadFromMemory(&chiproll, chipBlob, chipBytes, &bank);
            // Everything has its own copy now — the GPU for the two textures
            // and the mesh, the bank for its samples, because the DSP will
            // still be reading those while the ship flies.
            CremaPakClose(&pak);
        }
    }
    const CremaInstrument *sndLaser  = CremaBankFind(&bank, "laser");
    const CremaInstrument *sndBoom   = CremaBankFind(&bank, "boom");
    const CremaInstrument *sndEngine = CremaBankFind(&bank, "engine");
    if (!sndLaser || !sndBoom || !sndEngine)
        assetsOk = false;
    if (!assetsOk) {
        WHBLogPrintf("[flight] asset load failed");
        CremaShaderShutdownCompiler();
        CremaAppShutdown();
        return -1;
    }

    CremaShader *shShip = CremaShaderCompile(VS_SHIP, PS_SHIP,
                                             ship.attribs, ship.attribCount);
    const CremaAttrib groundAttribs[] = {
        { 0, 0, 0,                 GX2_ATTRIB_FORMAT_FLOAT_32_32_32 },
        { 1, 0, 3 * sizeof(float), GX2_ATTRIB_FORMAT_FLOAT_32_32 },
    };
    CremaShader *shGround = CremaShaderCompile(VS_GROUND, PS_GROUND,
                                               groundAttribs, 2);
    // same pixel shader, different vertex path: the enemies are placed
    // individually instead of riding the player's transform
    CremaShader *shEnemy = CremaShaderCompile(VS_ENEMY, PS_SHIP,
                                              ship.attribs, ship.attribCount);
    const CremaAttrib fxAttribs[] = {
        { 0, 0, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32 },
    };
    CremaShader *shFx = CremaShaderCompile(VS_FX, PS_FX, fxAttribs, 1);
    CremaShader *shHud = CremaShaderCompile(VS_HUD, PS_HUD, fxAttribs, 1);
    if (!shShip || !shGround || !shEnemy || !shFx || !shHud) {
        CremaShaderShutdownCompiler();
        CremaAppShutdown();
        return -1;
    }

    GX2RBuffer groundVbo, groundIbo, fxVbo, fxIbo, hudVbo, hudIbo;
    CremaBufferCreateVertex(&groundVbo, GROUND_STRIDE, 4, GROUND_VERTS);
    CremaBufferCreateIndexU16(&groundIbo, 6, GROUND_TRIS);
    CremaBufferCreateVertex(&fxVbo, FX_STRIDE, 4, FX_VERTS);
    CremaBufferCreateIndexU16(&fxIbo, 6, FX_TRIS);
    CremaBufferCreateVertex(&hudVbo, HUD_STRIDE, 4, HUD_VERTS);
    CremaBufferCreateIndexU16(&hudIbo, 6, FX_TRIS);   // same two triangles

    GX2Texture ground;
    CremaTextureCreate(&ground, GROUND_TEX, GROUND_TEX,
                       CremaTextureMipLevels(GROUND_TEX, GROUND_TEX),
                       GX2_TILE_MODE_DEFAULT);
    uint8_t *pixels = (uint8_t *)malloc(GROUND_TEX * GROUND_TEX * 4);
    fillGroundTexture(pixels);
    CremaTextureUploadWithMips(&ground, pixels);
    free(pixels);

    GX2Sampler sampler, fontSampler;
    CremaSamplerInitTrilinear(&sampler, GX2_TEX_CLAMP_MODE_WRAP);
    // The font has one level and is never minified: ask for mips it does not
    // have and the sampler reads past the end of the chain.
    CremaSamplerInitBilinear(&fontSampler, GX2_TEX_CLAMP_MODE_CLAMP);

    int32_t gVsShip   = CremaShaderVSBlockLocation(shShip, "Global");
    int32_t gPsShip   = CremaShaderPSBlockLocation(shShip, "Global");
    int32_t formLoc   = CremaShaderVSBlockLocation(shShip, "Formation");
    int32_t gVsGround = CremaShaderVSBlockLocation(shGround, "Global");
    int32_t gPsGround = CremaShaderPSBlockLocation(shGround, "Global");
    if (gVsShip < 0)   gVsShip = 0;
    if (gPsShip < 0)   gPsShip = 0;
    if (formLoc < 0)   formLoc = 1;
    if (gVsGround < 0) gVsGround = 0;
    if (gPsGround < 0) gPsGround = 0;
    int32_t gVsEnemy = CremaShaderVSBlockLocation(shEnemy, "Global");
    int32_t gPsEnemy = CremaShaderPSBlockLocation(shEnemy, "Global");
    int32_t enemyLoc = CremaShaderVSBlockLocation(shEnemy, "Enemies");
    if (gVsEnemy < 0) gVsEnemy = 0;
    if (gPsEnemy < 0) gPsEnemy = 0;
    if (enemyLoc < 0) enemyLoc = 1;
    int32_t gVsFx = CremaShaderVSBlockLocation(shFx, "Global");
    int32_t fxLoc = CremaShaderVSBlockLocation(shFx, "Effects");
    if (gVsFx < 0) gVsFx = 0;
    if (fxLoc < 0) fxLoc = 1;
    int32_t hudLoc = CremaShaderVSBlockLocation(shHud, "Hud");
    if (hudLoc < 0) hudLoc = 0;
    uint32_t fontUnit = 0;
    if (shHud->ps->samplerVarCount > 0)
        fontUnit = shHud->ps->samplerVars[0].location;
    uint32_t hullUnit = 0, groundUnit = 0, enemyUnit = 0;
    if (shShip->ps->samplerVarCount > 0)
        hullUnit = shShip->ps->samplerVars[0].location;
    if (shGround->ps->samplerVarCount > 0)
        groundUnit = shGround->ps->samplerVars[0].location;
    if (shEnemy->ps->samplerVarCount > 0)
        enemyUnit = shEnemy->ps->samplerVars[0].location;

    // slot 0 is the player at the origin of its own model matrix; the wingmen
    // sit behind and beside, in ship-local space, so they bank with the leader
    float slots[8][4] = {
        {  0.0f,  0.0f,  0.0f, 0.0f },
        { -6.5f, -0.9f,  5.5f, 1.3f },
        {  6.5f, -0.9f,  5.5f, 2.6f },
        { -12.5f, -1.6f, 10.5f, 3.9f },
        {  12.5f, -1.6f, 10.5f, 5.2f },
        { 0 }, { 0 }, { 0 },
    };
    uint8_t *formation = (uint8_t *)CremaUniformAlloc(sizeof(slots));
    CremaUniformStore(formation, slots, sizeof(slots));

    CremaUniformRing globals;
    CremaUniformRingCreate(&globals, sizeof(GlobalBlock), CREMA_FRAMES_IN_FLIGHT);
    // the enemies move every frame, so their block needs a slice per frame in
    // flight exactly like the globals do
    CremaUniformRing enemyRing;
    CremaUniformRingCreate(&enemyRing, sizeof(float) * 4 * MAX_ENEMIES,
                           CREMA_FRAMES_IN_FLIGHT);

    // --- the rival squadron -----------------------------------------------
    CremaEntity enemyStorage[MAX_ENEMIES];
    CremaEntityPool enemies;
    CremaEntityPoolInit(&enemies, enemyStorage, MAX_ENEMIES);

    // The radius comes from the AABB the baker wrote into the .cmesh — the
    // first thing in Crema to actually use those bounds.
    float shipRadius = CremaBoundsRadius(ship.aabbMin, ship.aabbMax);
    WHBLogPrintf("[flight] ship bounding radius %.2f (from the baked AABB)",
                 shipRadius);

    uint32_t rng = 0x1234567u;
    Vec3 startPoint = { 0.0f, 0.0f, 0.0f };   // where the player begins
    for (int i = 0; i < ENEMY_COUNT; i++)
        spawnEnemy(&enemies, &rng, shipRadius, startPoint);

    CremaEffect fxStorage[MAX_EFFECTS];
    CremaEffectPool fx;
    CremaEffectPoolInit(&fx, fxStorage, MAX_EFFECTS);
    CremaUniformRing fxRing;
    CremaUniformRingCreate(&fxRing, sizeof(float) * 4 * 2 * MAX_EFFECTS,
                           CREMA_FRAMES_IN_FLIGHT);

    // Two HUD rings, because the two screens show different readouts and both
    // are in flight at once. Same rule as every other uniform here: one slice
    // per frame the GPU might still be reading.
    HudList tvHud, drcHud;
    CremaUniformRing hudRingTv, hudRingDrc;
    const size_t HUD_BYTES = sizeof(float) * 4 * 2 * HUD_MAX_ITEMS;
    CremaUniformRingCreate(&hudRingTv, HUD_BYTES, CREMA_FRAMES_IN_FLIGHT);
    CremaUniformRingCreate(&hudRingDrc, HUD_BYTES, CREMA_FRAMES_IN_FLIGHT);

    // The engine is a voice we own: it never ends, so nothing can reclaim it.
    CremaAudioVoice *engineVoice = sndEngine
        ? CremaAudioHold(&sndEngine->sound, 0.30f, 1.0f) : NULL;

    uint32_t score = 0, collisions = 0;

    Flight flight;
    memset(&flight, 0, sizeof(flight));
    flight.pos.y = 40.0f;
    flight.speed = SPEED_CRUSE;

    Vec3 camPos = { 0.0f, 45.0f, 30.0f };
    Mat4 proj = mat4_perspective(62.0f * 3.14159265f / 180.0f,
                                 16.0f / 9.0f, 0.5f, 2600.0f);
    Vec3 lightRaw = { -0.40f, -0.62f, -0.68f };
    Vec3 lightDir = vec3_normalize(lightRaw);

    SceneView view;
    view.shShip = shShip;         view.shGround = shGround;
    view.ship = &ship;
    view.groundVbo = &groundVbo;  view.groundIbo = &groundIbo;
    view.hull = &hull;            view.ground = &ground;
    view.sampler = &sampler;
    view.hullUnit = hullUnit;     view.groundUnit = groundUnit;
    view.gVsShip = gVsShip;       view.gPsShip = gPsShip;
    view.formationLoc = formLoc;
    view.gVsGround = gVsGround;   view.gPsGround = gPsGround;
    view.formation = formation;   view.formationBytes = sizeof(slots);
    view.shEnemy = shEnemy;
    view.gVsEnemy = gVsEnemy;     view.gPsEnemy = gPsEnemy;
    view.enemyLoc = enemyLoc;     view.enemyUnit = enemyUnit;
    view.enemyBytes = sizeof(float) * 4 * MAX_ENEMIES;
    view.enemyCount = 0;
    view.shFx = shFx;             view.fxVbo = &fxVbo;   view.fxIbo = &fxIbo;
    view.gVsFx = gVsFx;           view.fxLoc = fxLoc;
    view.fxBytes = sizeof(float) * 4 * 2 * MAX_EFFECTS;
    view.fxCount = 0;
    view.shHud = shHud;           view.hudVbo = &hudVbo; view.hudIbo = &hudIbo;
    view.hudLoc = hudLoc;         view.fontUnit = fontUnit;
    view.font = &font;            view.fontSampler = &fontSampler;
    view.hudBytes = HUD_BYTES;    view.hudCount = 0;     view.hudUbo = NULL;

    static const float SKY[4] = { 0.50f, 0.62f, 0.74f, 1.0f };
    static const float TACTICAL[4] = { 0.02f, 0.05f, 0.08f, 1.0f };

    // The song starts here, not at load: the callback ticks against the DSP
    // from this moment and nothing about the frame loop can hurry or delay it.
    CremaMusic *playing = chiproll ? chiproll : music;
    if (playing)
        CremaMusicStart(playing);

    CremaFrame frame;
    CremaFrameInit(&frame, CREMA_PACING_FENCED, 1);
    WHBLogPrintf("[flight] L-stick fly (forward dives), ZR/ZL throttle, A fire,"
                 " MINUS swaps song, PLUS toggles envelopes, Y toggles echo."
                 " %d wingmen, %d hostiles.", NUM_WINGMEN, ENEMY_COUNT);

    CremaFrameStats stats;
    CremaClock clock;
    CremaClockInit(&clock);
    CremaInput input;
    CremaInputInit(&input);

    while (CremaAppRunning()) {
        CremaClockTick(&clock);
        float dt = clock.dt;
        float t = clock.elapsed;

        CremaInputPoll(&input);
        flightUpdate(&flight, &input, dt);

        // MINUS swaps the two songs — the hand-written one and the one that
        // came out of chiproll. An audition mode is worth more than an editor
        // on a console: the only question you cannot answer on the PC is
        // whether it sounds right coming out of a television.
        if (CremaInputPressed(&input, VPAD_BUTTON_MINUS) && music && chiproll) {
            CremaMusicStop(playing);
            playing = (playing == chiproll) ? music : chiproll;
            CremaMusicStart(playing);
            WHBLogPrintf("[flight] now playing the %s song",
                         playing == chiproll ? "chiproll" : "hand-written");
        }

        // PLUS is the audition switch: the same song with the instruments'
        // envelopes and vibrato applied, and without. It is here rather than in
        // a tool because the one question a PC cannot answer is what a decay
        // sounds like through a television.
        if (CremaInputPressed(&input, VPAD_BUTTON_PLUS)) {
            bool on = !CremaMusicShaping(playing);
            CremaMusicSetShaping(music, on);
            CremaMusicSetShaping(chiproll, on);
            WHBLogPrintf("[flight] envelopes %s", on ? "on" : "off (rectangles)");
        }

        // Y opens the aux send into our own echo. The counter in the log is the
        // first thing to read even when nothing is audible: a callback that is
        // being called with the channel and sample counts we expected means the
        // registration and the ABI are right, and anything still wrong after
        // that is the effect or the routing rather than the plumbing.
        if (CremaInputPressed(&input, VPAD_BUTTON_Y)) {
            auxSetEnabled(!s_echoOn);
            WHBLogPrintf("[flight] echo %s", s_echoOn ? "on" : "off");
        }

        // The engine note rides the throttle: one 0.2 s loop, resampled. This
        // is the whole trick a sampler is capable of — the sound never changes,
        // only the rate it is read at, and that is a working synthesiser.
        CremaAudioUpdate();
        {
            float rpm = (flight.speed - SPEED_MIN) / (SPEED_MAX - SPEED_MIN);
            CremaAudioVoiceSet(engineVoice, 0.26f + rpm * 0.22f,
                               0.80f + rpm * 0.70f);
        }

        // model matrix: yaw, then pitch, then roll, at the ship's position
        Mat4 model = mat4_mul(mat4_translate(flight.pos.x, flight.pos.y, flight.pos.z),
                     mat4_mul(mat4_rotate_y(flight.yaw),
                     mat4_mul(mat4_rotate_x(flight.pitch),
                              mat4_rotate_z(flight.roll))));

        float cp = cosf(flight.pitch), sp = sinf(flight.pitch);
        float cy = cosf(flight.yaw),   sy = sinf(flight.yaw);
        Vec3 fwd = { -sy * cp, sp, -cy * cp };

        // Afterburner: one glow shed behind the ship every frame, brighter the
        // faster you go. Effects are cheap enough to spend on continuous ones.
        {
            Vec3 tail = { flight.pos.x - fwd.x * 3.6f,
                          flight.pos.y - fwd.y * 3.6f,
                          flight.pos.z - fwd.z * 3.6f };
            float heat = (flight.speed - SPEED_MIN) / (SPEED_MAX - SPEED_MIN);
            CremaEffectSpawn(&fx, tail, 0.22f, 1.5f + heat * 2.4f, 0.2f,
                             0.42f, 0.72f, 1.0f, 0.5f + heat * 0.45f);
        }

        // --- the squadron flies on, wrapping the world at its edges ---
        for (uint32_t i = 0; i < enemies.watermark; i++) {
            CremaEntity *e = &enemies.items[i];
            if (!e->active)
                continue;
            e->pos.x += -sinf(e->yaw) * ENEMY_SPEED * dt;
            e->pos.z += -cosf(e->yaw) * ENEMY_SPEED * dt;
            // wrap around the PLAYER, not the origin: in an endless world
            // there is no origin to wrap around
            float dx = e->pos.x - flight.pos.x;
            float dz = e->pos.z - flight.pos.z;
            if (dx >  WORLD_HALF) e->pos.x -= 2.0f * WORLD_HALF;
            if (dx < -WORLD_HALF) e->pos.x += 2.0f * WORLD_HALF;
            if (dz >  WORLD_HALF) e->pos.z -= 2.0f * WORLD_HALF;
            if (dz < -WORLD_HALF) e->pos.z += 2.0f * WORLD_HALF;
        }

        // --- the lock: a ray down the nose, nearest target wins ---
        // Worked out every frame, not only when you fire, because the HUD has
        // to show what the gun would hit. One answer, two consumers: the
        // bracket cannot disagree with the shot if they are the same variable.
        CremaEntity *best = NULL;
        float bestDist = 0.0f;
        for (uint32_t i = 0; i < enemies.watermark; i++) {
            CremaEntity *e = &enemies.items[i];
            if (!e->active)
                continue;
            float dist;
            if (CremaRayHitsSphere(flight.pos, fwd, GUN_RANGE,
                                   e->pos, e->radius, &dist) &&
                (best == NULL || dist < bestDist)) {
                best = e;
                bestDist = dist;
            }
        }

        // Where the shot actually ends up: the locked target if there is one,
        // otherwise a working range. The tracer, the crosshair and the shot all
        // read this one value, so the sight cannot disagree with the bullet.
        float reach = best ? bestDist : 320.0f;
        Vec3 aimPoint = { flight.pos.x + fwd.x * reach,
                          flight.pos.y + fwd.y * reach,
                          flight.pos.z + fwd.z * reach };

        // A is edge-triggered, which is the entire reason crema_input tracks
        // edges: with `held` this would fire sixty times a second.
        if (CremaInputPressed(&input, VPAD_BUTTON_A)) {
            // A shot you cannot see is a shot the player will not believe:
            // muzzle flash, a line of tracer beads, and a bloom where it lands
            // A little pitch scatter, so ten shots in a row are not one shot
            // played ten times — the cheapest anti-repetition trick there is.
            CremaAudioPlay(&sndLaser->sound, 0.80f, 0.92f + nextRandom(&rng) * 0.16f);

            Vec3 muzzle = { flight.pos.x + fwd.x * 5.0f,
                            flight.pos.y + fwd.y * 5.0f,
                            flight.pos.z + fwd.z * 5.0f };
            CremaEffectSpawn(&fx, muzzle, 0.09f, 4.0f, 0.6f,
                             1.0f, 0.86f, 0.55f, 1.0f);
            for (int k = 1; k <= 10; k++) {
                float d = reach * (float)k / 10.0f;
                Vec3 p = { flight.pos.x + fwd.x * d,
                           flight.pos.y + fwd.y * d,
                           flight.pos.z + fwd.z * d };
                CremaEffectSpawn(&fx, p, 0.13f, 1.7f, 0.25f,
                                 0.55f, 0.85f, 1.0f, 0.95f);
            }

            if (best) {
                // Distance is the cheapest mixing there is: the same explosion,
                // quieter and a shade lower when it goes off far away.
                float loud = 1.0f / (1.0f + bestDist / 240.0f);
                CremaAudioPlay(&sndBoom->sound, loud, 0.90f + loud * 0.20f);

                Vec3 at = best->pos;
                CremaEffectSpawn(&fx, at, 0.45f, 3.0f, 34.0f,
                                 1.0f, 0.5f, 0.14f, 1.0f);
                for (int k = 0; k < 8; k++) {
                    CremaEffect *sp = CremaEffectSpawn(&fx, at, 0.55f, 2.4f, 0.3f,
                                                       1.0f, 0.78f, 0.3f, 1.0f);
                    if (!sp)
                        break;
                    sp->velocity.x = (nextRandom(&rng) * 2.0f - 1.0f) * 48.0f;
                    sp->velocity.y = (nextRandom(&rng) * 2.0f - 1.0f) * 48.0f;
                    sp->velocity.z = (nextRandom(&rng) * 2.0f - 1.0f) * 48.0f;
                }
                CremaEntityDespawn(&enemies, best);
                best = NULL;      // the HUD still holds it: do not bracket a corpse
                spawnEnemy(&enemies, &rng, shipRadius, flight.pos);
                score++;
                WHBLogPrintf("[flight] hit at %.0f m - score %u", bestDist, score);
            }
        }

        // --- and flying into one counts too ---
        for (uint32_t i = 0; i < enemies.watermark; i++) {
            CremaEntity *e = &enemies.items[i];
            if (!e->active)
                continue;
            if (CremaSphereHitsSphere(flight.pos, shipRadius, e->pos, e->radius)) {
                // pitched down: a hit on you should sound heavier than a kill
                CremaAudioPlay(&sndBoom->sound, 1.0f, 0.72f);
                CremaEffectSpawn(&fx, e->pos, 0.6f, 4.0f, 44.0f,
                                 1.0f, 0.42f, 0.12f, 1.0f);
                if (e == best)
                    best = NULL;
                CremaEntityDespawn(&enemies, e);
                spawnEnemy(&enemies, &rng, shipRadius, flight.pos);
                collisions++;
                WHBLogPrintf("[flight] collision! (%u so far)", collisions);
            }
        }

        // Chase camera: lag behind the ideal spot instead of snapping to it.
        // The lag IS the feeling of speed — a rigid camera looks like the world
        // is moving and the ship is not.
        Vec3 want = { flight.pos.x - fwd.x * 19.0f,
                      flight.pos.y - fwd.y * 19.0f + 5.5f,
                      flight.pos.z - fwd.z * 19.0f };
        float follow = fminf(1.0f, 6.0f * dt);
        camPos.x += (want.x - camPos.x) * follow;
        camPos.y += (want.y - camPos.y) * follow;
        camPos.z += (want.z - camPos.z) * follow;
        if (camPos.y < 3.0f)
            camPos.y = 3.0f;

        Vec3 look = { flight.pos.x + fwd.x * 22.0f,
                      flight.pos.y + fwd.y * 22.0f,
                      flight.pos.z + fwd.z * 22.0f };
        Vec3 up = { 0.0f, 1.0f, 0.0f };

        CremaEffectUpdate(&fx, dt);

        // the billboards need the camera's own axes to face it
        Vec3 viewFwd   = vec3_normalize(vec3_sub(look, camPos));
        Vec3 camRight  = vec3_normalize(vec3_cross(viewFwd, up));
        Vec3 camUpAxis = vec3_cross(camRight, viewFwd);

        uint32_t slot = CremaFrameBegin(&frame);

        GlobalBlock blk;
        blk.viewProj = mat4_mul(proj, mat4_look_at(camPos, look, up));
        blk.model = model;
        blk.lightDir[0] = lightDir.x; blk.lightDir[1] = lightDir.y;
        blk.lightDir[2] = lightDir.z; blk.lightDir[3] = 0.0f;
        blk.camPos[0] = camPos.x; blk.camPos[1] = camPos.y;
        blk.camPos[2] = camPos.z; blk.camPos[3] = 0.0f;
        blk.fogParam[0] = 320.0f; blk.fogParam[1] = 1500.0f;
        blk.fogParam[2] = 0.0f;   blk.fogParam[3] = 0.0f;
        blk.fogColor[0] = SKY[0]; blk.fogColor[1] = SKY[1];
        blk.fogColor[2] = SKY[2]; blk.fogColor[3] = 1.0f;
        blk.time[0] = t; blk.time[1] = blk.time[2] = blk.time[3] = 0.0f;
        // snap to a whole texture tile, so the ground moving with you is
        // invisible: one add in the shader turns a finite quad into a world
        const float tile = (GROUND_HALF * 2.0f) / GROUND_UV;
        blk.groundOffset[0] = floorf(flight.pos.x / tile) * tile;
        blk.groundOffset[1] = 0.0f;
        blk.groundOffset[2] = floorf(flight.pos.z / tile) * tile;
        blk.groundOffset[3] = 0.0f;
        blk.camRight[0] = camRight.x; blk.camRight[1] = camRight.y;
        blk.camRight[2] = camRight.z; blk.camRight[3] = 0.0f;
        blk.camUp[0] = camUpAxis.x;   blk.camUp[1] = camUpAxis.y;
        blk.camUp[2] = camUpAxis.z;   blk.camUp[3] = 0.0f;
        view.globalUbo = CremaUniformRingStore(&globals, slot, &blk, sizeof(blk));

        // pack the live enemies into the instance array, contiguously: the
        // draw indexes it by gl_InstanceID, so holes would draw ghosts
        float enemyData[MAX_ENEMIES][4];
        uint32_t live = 0;
        for (uint32_t i = 0; i < enemies.watermark && live < MAX_ENEMIES; i++) {
            const CremaEntity *e = &enemies.items[i];
            if (!e->active)
                continue;
            enemyData[live][0] = e->pos.x;
            enemyData[live][1] = e->pos.y;
            enemyData[live][2] = e->pos.z;
            enemyData[live][3] = e->yaw;
            live++;
        }
        view.enemyUbo = CremaUniformRingStore(&enemyRing, slot, enemyData,
                                              sizeof(enemyData));
        view.enemyCount = live;

        float fxData[MAX_EFFECTS * 2][4];
        memset(fxData, 0, sizeof(fxData));
        uint32_t fxLive = CremaEffectPack(&fx, fxData, MAX_EFFECTS);
        view.fxUbo = CremaUniformRingStore(&fxRing, slot, fxData, sizeof(fxData));
        view.fxCount = fxLive;

        buildTvHud(&tvHud, &flight, score, collisions, best, bestDist,
                   aimPoint, blk.viewProj);
        buildDrcHud(&drcHud, &flight, &enemies, score, collisions, best);
        const void *tvHudUbo = CremaUniformRingStore(&hudRingTv, slot,
                                                     tvHud.items, HUD_BYTES);
        const void *drcHudUbo = CremaUniformRingStore(&hudRingDrc, slot,
                                                      drcHud.items, HUD_BYTES);

        // Not CremaFrameDrawBoth any more: the two screens no longer show the
        // same thing. The TV gets the world with a HUD over it; the GamePad
        // gets the tactical screen and no 3D pass at all — which is the point,
        // because a second screen that repeats the first is a second screen
        // you are paying for twice.
        WHBGfxBeginRenderTV();
        WHBGfxClearColor(SKY[0], SKY[1], SKY[2], SKY[3]);
        drawWorld(&view);
        view.hudUbo = tvHudUbo;
        view.hudCount = tvHud.count;
        drawHud(&view);
        WHBGfxFinishRenderTV();

        WHBGfxBeginRenderDRC();
        WHBGfxClearColor(TACTICAL[0], TACTICAL[1], TACTICAL[2], TACTICAL[3]);
        view.hudUbo = drcHudUbo;
        view.hudCount = drcHud.count;
        drawHud(&view);
        WHBGfxFinishRenderDRC();

        CremaFrameEnd(&frame, &stats);

        if (stats.updated) {
            CremaMusicStats ms;
            memset(&ms, 0, sizeof(ms));
            CremaMusicGetStats(playing, &ms);
            WHBLogPrintf("[flight] %.1f fps | speed %.0f | alt %.0f | sync %.2f ms"
                         " | voices %u | seq %u us last, %u us worst, %u ticks,"
                         " %u notes, %u loops | env %s",
                         stats.fps, flight.speed, flight.pos.y, stats.drainMs,
                         (unsigned)CremaAudioVoicesInUse(),
                         ms.lastUs, ms.maxUs, ms.ticks, ms.notesOn, ms.loops,
                         CremaMusicShaping(playing) ? "on" : "off");
            // What the aux callback saw. Channels and samples say the ABI was
            // read correctly; the peaks say what scale AX actually uses in an
            // aux buffer, which is the one number no header or emulator can be
            // trusted for.
            WHBLogPrintf("[aux] echo %s | %u calls | %u ch x %u samples | "
                         "peak in %d, out %d | send %.2f | %u us last, "
                         "%u us worst",
                         s_echoOn ? "on" : "off", s_echo.calls,
                         s_echo.lastChannels, s_echo.lastSamples,
                         (int)s_echo.peakIn, (int)s_echo.peakOut,
                         CremaAudioGetAuxSend(AUX_BUS),
                         s_auxUs, s_auxWorstUs);
            // The mix as the speakers get it, against the 32767 it has to fit
            // in. Over 100% is not a warning that it might clip: it is the
            // amount by which it already did.
            int32_t peak = meterRead();
            WHBLogPrintf("[mix] peak %d (%.0f%% of full scale), worst %d "
                         "(%.0f%%) | headroom %.2f",
                         (int)peak, peak * 100.0 / 32767.0,
                         (int)s_meterHigh, s_meterHigh * 100.0 / 32767.0,
                         CremaAudioGetHeadroom());
        }
    }

    CremaFrameSettle(&frame);
    auxShutdown();      // before the voices: nothing may still be sending
    CremaAudioRelease(engineVoice);
    CremaMusicClose(chiproll);
    CremaMusicClose(music);
    CremaBankClose(&bank);
    CremaAudioShutdown();
    CremaUniformRingDestroy(&hudRingDrc);
    CremaUniformRingDestroy(&hudRingTv);
    CremaUniformRingDestroy(&fxRing);
    CremaUniformRingDestroy(&enemyRing);
    CremaUniformRingDestroy(&globals);
    CremaUniformFreeBlock(formation);
    CremaBufferDestroy(&hudIbo);
    CremaBufferDestroy(&hudVbo);
    CremaBufferDestroy(&fxIbo);
    CremaBufferDestroy(&fxVbo);
    CremaBufferDestroy(&groundIbo);
    CremaBufferDestroy(&groundVbo);
    CremaTextureDestroy(&ground);
    CremaTextureDestroy(&font);
    CremaTextureDestroy(&hull);
    CremaMeshDestroy(&ship);
    CremaShaderFree(shHud);
    CremaShaderFree(shFx);
    CremaShaderFree(shEnemy);
    CremaShaderFree(shGround);
    CremaShaderFree(shShip);
    CremaShaderShutdownCompiler();
    CremaAppShutdown();
    return 0;
}
