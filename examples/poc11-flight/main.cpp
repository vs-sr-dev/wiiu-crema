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
#include "crema_matrix.h"
#include "crema_mesh.h"
#include "crema_shader.h"
#include "crema_texture.h"
#include "sounds.h"     // the waveforms: plain C, no console in them at all

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
} SceneView;

static void drawScene(void *user)
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

int main(int argc, char **argv)
{
    if (!CremaAppInit("poc11-flight"))
        return -1;
    // First thing after the app is up, before a single asset is read: this is
    // what takes the audio hardware away from the Wii U Menu's music.
    CremaAudioInit();
    if (!CremaShaderInitCompiler()) {
        CremaAppShutdown();
        return -1;
    }

    CremaMesh ship;
    GX2Texture hull;
    if (!CremaMeshLoad(&ship, "/vol/content/ship.cmesh") ||
        !CremaTextureLoad(&hull, "/vol/content/hull.ctex")) {
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
    if (!shShip || !shGround || !shEnemy || !shFx) {
        CremaShaderShutdownCompiler();
        CremaAppShutdown();
        return -1;
    }

    GX2RBuffer groundVbo, groundIbo, fxVbo, fxIbo;
    CremaBufferCreateVertex(&groundVbo, GROUND_STRIDE, 4, GROUND_VERTS);
    CremaBufferCreateIndexU16(&groundIbo, 6, GROUND_TRIS);
    CremaBufferCreateVertex(&fxVbo, FX_STRIDE, 4, FX_VERTS);
    CremaBufferCreateIndexU16(&fxIbo, 6, FX_TRIS);

    GX2Texture ground;
    CremaTextureCreate(&ground, GROUND_TEX, GROUND_TEX,
                       CremaTextureMipLevels(GROUND_TEX, GROUND_TEX),
                       GX2_TILE_MODE_DEFAULT);
    uint8_t *pixels = (uint8_t *)malloc(GROUND_TEX * GROUND_TEX * 4);
    fillGroundTexture(pixels);
    CremaTextureUploadWithMips(&ground, pixels);
    free(pixels);

    GX2Sampler sampler;
    CremaSamplerInitTrilinear(&sampler, GX2_TEX_CLAMP_MODE_WRAP);

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

    // --- the sounds, cooked once into DSP memory --------------------------
    // One scratch buffer, three sounds, ~85 KB of PCM. Baking them here costs a
    // few milliseconds at startup; the alternative is a synthesiser running
    // every frame for sounds that never change.
    CremaSound sndLaser, sndBoom, sndEngine;
    memset(&sndLaser, 0, sizeof(sndLaser));
    memset(&sndBoom,  0, sizeof(sndBoom));
    memset(&sndEngine, 0, sizeof(sndEngine));
    {
        int16_t *scratch = (int16_t *)malloc(SND_MAX_SAMPS * sizeof(int16_t));
        if (scratch) {
            uint32_t n;
            n = bakeLaser(scratch);
            CremaSoundCreate(&sndLaser, scratch, n, SND_RATE);
            n = bakeBoom(scratch);
            CremaSoundCreate(&sndBoom, scratch, n, SND_RATE);
            n = bakeEngine(scratch);
            CremaSoundCreateLooping(&sndEngine, scratch, n, SND_RATE, 0);
            free(scratch);
        }
    }
    // The engine is a voice we own: it never ends, so nothing can reclaim it.
    CremaAudioVoice *engineVoice = CremaAudioHold(&sndEngine, 0.30f, 1.0f);

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

    static const float SKY[4] = { 0.50f, 0.62f, 0.74f, 1.0f };

    CremaFrame frame;
    CremaFrameInit(&frame, CREMA_PACING_FENCED, 1);
    WHBLogPrintf("[flight] L-stick fly (forward dives), ZR/ZL throttle, A fire."
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

        // --- guns: a ray down the nose, nearest target wins ---
        // A is edge-triggered, which is the entire reason crema_input tracks
        // edges: with `held` this would fire sixty times a second.
        if (CremaInputPressed(&input, VPAD_BUTTON_A)) {
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
            // A shot you cannot see is a shot the player will not believe:
            // muzzle flash, a line of tracer beads, and a bloom where it lands
            // A little pitch scatter, so ten shots in a row are not one shot
            // played ten times — the cheapest anti-repetition trick there is.
            CremaAudioPlay(&sndLaser, 0.80f, 0.92f + nextRandom(&rng) * 0.16f);

            float reach = best ? bestDist : 320.0f;
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
                CremaAudioPlay(&sndBoom, loud, 0.90f + loud * 0.20f);

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
                spawnEnemy(&enemies, &rng, shipRadius, flight.pos);
                score++;
                WHBLogPrintf("[flight] hit at %.0f m — score %u", bestDist, score);
            }
        }

        // --- and flying into one counts too ---
        for (uint32_t i = 0; i < enemies.watermark; i++) {
            CremaEntity *e = &enemies.items[i];
            if (!e->active)
                continue;
            if (CremaSphereHitsSphere(flight.pos, shipRadius, e->pos, e->radius)) {
                // pitched down: a hit on you should sound heavier than a kill
                CremaAudioPlay(&sndBoom, 1.0f, 0.72f);
                CremaEffectSpawn(&fx, e->pos, 0.6f, 4.0f, 44.0f,
                                 1.0f, 0.42f, 0.12f, 1.0f);
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

        CremaFrameDrawBoth(SKY, drawScene, &view);
        CremaFrameEnd(&frame, &stats);

        if (stats.updated)
            WHBLogPrintf("[flight] %.1f fps | speed %.0f | alt %.0f | sync %.2f ms"
                         " | voices %u",
                         stats.fps, flight.speed, flight.pos.y, stats.drainMs,
                         (unsigned)CremaAudioVoicesInUse());
    }

    CremaFrameSettle(&frame);
    CremaAudioRelease(engineVoice);
    CremaSoundDestroy(&sndEngine);
    CremaSoundDestroy(&sndBoom);
    CremaSoundDestroy(&sndLaser);
    CremaAudioShutdown();
    CremaUniformRingDestroy(&fxRing);
    CremaUniformRingDestroy(&enemyRing);
    CremaUniformRingDestroy(&globals);
    CremaUniformFreeBlock(formation);
    CremaBufferDestroy(&fxIbo);
    CremaBufferDestroy(&fxVbo);
    CremaBufferDestroy(&groundIbo);
    CremaBufferDestroy(&groundVbo);
    CremaTextureDestroy(&ground);
    CremaTextureDestroy(&hull);
    CremaMeshDestroy(&ship);
    CremaShaderFree(shFx);
    CremaShaderFree(shEnemy);
    CremaShaderFree(shGround);
    CremaShaderFree(shShip);
    CremaShaderShutdownCompiler();
    CremaAppShutdown();
    return 0;
}
