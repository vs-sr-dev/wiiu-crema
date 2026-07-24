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
#include "crema_buffer.h"
#include "crema_frame.h"
#include "crema_matrix.h"
#include "crema_mesh.h"
#include "crema_shader.h"
#include "crema_texture.h"

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
} GlobalBlock;

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
    const CremaShader *shShip, *shGround;
    const CremaMesh   *ship;
    GX2RBuffer *groundVbo, *groundIbo;
    const GX2Texture *hull, *ground;
    const GX2Sampler *sampler;
    uint32_t hullUnit, groundUnit;
    int32_t  gVsShip, gPsShip, formationLoc;
    int32_t  gVsGround, gPsGround;
    const void *globalUbo;
    const void *formation;
    size_t   formationBytes;
} SceneView;

static void drawScene(void *user)
{
    const SceneView *s = (const SceneView *)user;

    GX2SetDepthOnlyControl(TRUE, TRUE, GX2_COMPARE_FUNC_LESS);
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

static void flightUpdate(Flight *f, const VPADStatus *pad, bool haveInput, float dt)
{
    float stickX = 0.0f, stickY = 0.0f;
    float throttle = 0.0f;
    if (haveInput) {
        stickX = pad->leftStick.x;
        stickY = pad->leftStick.y;
        if (fabsf(stickX) < 0.12f) stickX = 0.0f;
        if (fabsf(stickY) < 0.12f) stickY = 0.0f;
        if (pad->hold & VPAD_BUTTON_ZR) throttle += 1.0f;
        if (pad->hold & VPAD_BUTTON_ZL) throttle -= 1.0f;
    }

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
    f->yaw -= sinf(f->roll) * 1.15f * dt;

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
    if (!shShip || !shGround) {
        CremaShaderShutdownCompiler();
        CremaAppShutdown();
        return -1;
    }

    GX2RBuffer groundVbo, groundIbo;
    CremaBufferCreateVertex(&groundVbo, GROUND_STRIDE, 4, GROUND_VERTS);
    CremaBufferCreateIndexU16(&groundIbo, 6, GROUND_TRIS);

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
    uint32_t hullUnit = 0, groundUnit = 0;
    if (shShip->ps->samplerVarCount > 0)
        hullUnit = shShip->ps->samplerVars[0].location;
    if (shGround->ps->samplerVarCount > 0)
        groundUnit = shGround->ps->samplerVars[0].location;

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

    static const float SKY[4] = { 0.50f, 0.62f, 0.74f, 1.0f };

    CremaFrame frame;
    CremaFrameInit(&frame, CREMA_PACING_FENCED, 1);
    WHBLogPrintf("[flight] L-stick fly, ZR throttle up, ZL down. %d ships.",
                 NUM_SHIPS);

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

        VPADStatus vpad;
        VPADReadError vpadErr;
        memset(&vpad, 0, sizeof(vpad));
        VPADRead(VPAD_CHAN_0, &vpad, 1, &vpadErr);
        flightUpdate(&flight, &vpad, vpadErr == VPAD_READ_SUCCESS, dt);

        // model matrix: yaw, then pitch, then roll, at the ship's position
        Mat4 model = mat4_mul(mat4_translate(flight.pos.x, flight.pos.y, flight.pos.z),
                     mat4_mul(mat4_rotate_y(flight.yaw),
                     mat4_mul(mat4_rotate_x(flight.pitch),
                              mat4_rotate_z(flight.roll))));

        float cp = cosf(flight.pitch), sp = sinf(flight.pitch);
        float cy = cosf(flight.yaw),   sy = sinf(flight.yaw);
        Vec3 fwd = { -sy * cp, sp, -cy * cp };

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
        view.globalUbo = CremaUniformRingStore(&globals, slot, &blk, sizeof(blk));

        CremaFrameDrawBoth(SKY, drawScene, &view);
        CremaFrameEnd(&frame, &stats);

        if (stats.updated)
            WHBLogPrintf("[flight] %.1f fps | speed %.0f | alt %.0f | sync %.2f ms",
                         stats.fps, flight.speed, flight.pos.y, stats.drainMs);
    }

    CremaFrameSettle(&frame);
    CremaUniformRingDestroy(&globals);
    CremaUniformFreeBlock(formation);
    CremaBufferDestroy(&groundIbo);
    CremaBufferDestroy(&groundVbo);
    CremaTextureDestroy(&ground);
    CremaTextureDestroy(&hull);
    CremaMeshDestroy(&ship);
    CremaShaderFree(shGround);
    CremaShaderFree(shShip);
    CremaShaderShutdownCompiler();
    CremaAppShutdown();
    return 0;
}
