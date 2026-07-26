// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan
//
// GX2 PoC 13 — a slice of a role-playing game, and the reason it exists is the
// one thing the README still says Crema does not know: what a scene is.
//
// PoC 12 has four game states in a `switch`, and that is not the same thing.
// Its states share every resource, none of them is ever entered or left, and
// nothing is loaded or freed when one becomes another — a state machine is
// bookkeeping, and switching scenes is a question about *lifetime*. So this is
// the first example in the project where two pieces of the game exist at once,
// own memory of their own, and hand the frame back and forth:
//
//     TITLE  --goto-->  FIELD  --push-->  BATTLE  --push-->  GAME OVER
//                         |                                     |
//                         +--push--> STATUS (see-through)        |
//                                                                v
//                                                            goto TITLE
//
// Two of those arrows are different in kind, and telling them apart is most of
// what this PoC found out:
//
//   *push* keeps what is underneath alive. Walking into a monster suspends the
//   field, and when the battle ends you are standing exactly where you were,
//   with the other monsters where they wandered to. Nothing was saved and
//   restored, because nothing was destroyed.
//
//   *goto* forgets everything. The stack is emptied one scene at a time, each
//   one handing its memory back, and the new scene starts from nothing.
//
// The machinery below is written HERE, by hand, and not in `crema/`. That is
// the project's rule and this is exactly the case it is for: one example is not
// evidence. The test comes after — whether PoC 12's four states can be
// rewritten onto this shape without losing anything. If they can, the shape was
// discovered twice and belongs in the framework; if they cannot, this file is
// where it should have stayed.
//
// What is deliberately NOT here: any art of its own. The hull, the font and the
// sound bank are PoC 10's and PoC 11's, exactly as PoC 12 reused them. The one
// texture this example generates it generates at runtime, because a field needs
// a ground and a ground is the heaviest thing a scene can own — which turned
// out to be the measurement worth taking.

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
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "crema_app.h"
#include "crema_audio.h"
#include "crema_bank.h"
#include "crema_blend.h"
#include "crema_buffer.h"
#include "crema_collide.h"
#include "crema_effect.h"
#include "crema_entity.h"
#include "crema_frame.h"
#include "crema_hud.h"
#include "crema_input.h"
#include "crema_matrix.h"
#include "crema_mesh.h"
#include "crema_music.h"
#include "crema_pak.h"
#include "crema_save.h"
#include "crema_scene.h"
#include "crema_shader.h"
#include "crema_texture.h"

#define PI 3.14159265f

// --- shared shape ------------------------------------------------------------

#define MAX_INSTANCES   32       // uInst[] below is twice this
#define MAX_EFFECTS     32

#define GLOBAL_UBO_DECL \
    "layout(binding = 0) uniform Global {\n" \
    "    mat4 uViewProj;\n"       \
    "    vec4 uLightDir;\n"       \
    "    vec4 uCamPos;\n"         \
    "    vec4 uFog;\n"            /* x = starts, y = ends */ \
    "    vec4 uFogColor;\n"       \
    "    vec4 uGroundOffset;\n"   \
    "};\n"

typedef struct {
    Mat4  viewProj;
    float lightDir[4];
    float camPos[4];
    float fog[4];
    float fogColor[4];
    float groundOffset[4];
} Global;

// Every character in this game — the hero, the monsters wandering the field,
// the monsters lined up in a battle — is the same hull drawn at a different
// size, tint and heading. A token is not a model; it is four numbers.
static const char *VS_TOKEN =
    "#version 450\n"
    "layout(location = 0) in vec3 aPosition;\n"
    "layout(location = 1) in vec3 aNormal;\n"
    "layout(location = 2) in vec2 aUV;\n"
    GLOBAL_UBO_DECL
    "layout(binding = 1) uniform Instances {\n"
    "    vec4 uInst[64];\n"   // { xyz = position, w = scale }, { rgb, yaw }
    "};\n"
    "layout(location = 0) out vec2 vUV;\n"
    "layout(location = 1) out vec3 vNormal;\n"
    "layout(location = 2) out vec3 vTint;\n"
    "layout(location = 3) out vec3 vWorld;\n"
    "void main()\n"
    "{\n"
    "    vec4 slot = uInst[gl_InstanceID * 2];\n"
    "    vec4 look = uInst[gl_InstanceID * 2 + 1];\n"
    // A walking character needs a real heading rather than PoC 12's sign trick:
    // a shoot-'em-up faces two ways, a field faces all of them.
    "    float cy = cos(look.w), sy = sin(look.w);\n"
    "    vec3 p = vec3(cy * aPosition.x + sy * aPosition.z, aPosition.y,\n"
    "                  -sy * aPosition.x + cy * aPosition.z);\n"
    "    vec3 n = vec3(cy * aNormal.x + sy * aNormal.z, aNormal.y,\n"
    "                  -sy * aNormal.x + cy * aNormal.z);\n"
    "    vec3 world = p * slot.w + slot.xyz;\n"
    "    gl_Position = uViewProj * vec4(world, 1.0);\n"
    "    vUV = aUV;\n"
    "    vNormal = n;\n"
    "    vTint = look.rgb;\n"
    "    vWorld = world;\n"
    "}\n";

static const char *PS_TOKEN =
    "#version 450\n"
    "layout(location = 0) in vec2 vUV;\n"
    "layout(location = 1) in vec3 vNormal;\n"
    "layout(location = 2) in vec3 vTint;\n"
    "layout(location = 3) in vec3 vWorld;\n"
    GLOBAL_UBO_DECL
    "layout(binding = 0) uniform sampler2D uHull;\n"
    "layout(location = 0) out vec4 oColor;\n"
    "void main()\n"
    "{\n"
    "    vec3 base = texture(uHull, vUV).rgb * vTint;\n"
    "    float diff = max(dot(normalize(vNormal), -uLightDir.xyz), 0.0);\n"
    "    vec3 lit = base * (0.40 + 0.70 * diff);\n"
    "    float fog = smoothstep(uFog.x, uFog.y, length(vWorld - uCamPos.xyz));\n"
    "    oColor = vec4(mix(lit, uFogColor.rgb, fog), 1.0);\n"
    "}\n";

// The ground, one quad that follows the hero snapped to a whole texture tile —
// PoC 11's trick, and the only reason a finite quad reads as a world you can
// keep walking across.
static const char *VS_GROUND =
    "#version 450\n"
    "layout(location = 0) in vec3 aPosition;\n"
    "layout(location = 1) in vec2 aUV;\n"
    GLOBAL_UBO_DECL
    "layout(location = 0) out vec2 vUV;\n"
    "layout(location = 1) out vec3 vWorld;\n"
    "void main()\n"
    "{\n"
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
    "    vec3 lit = base * (0.35 + 0.75 * diff);\n"
    "    float fog = smoothstep(uFog.x, uFog.y, length(vWorld - uCamPos.xyz));\n"
    "    oColor = vec4(mix(lit, uFogColor.rgb, fog), 1.0);\n"
    "}\n";

// --- the party ---------------------------------------------------------------
//
// The one piece of state that belongs to none of the scenes. A battle reads and
// writes it, the field carries it around, the title screen shows it and the
// save file is exactly its shape — so it lives with the application and is
// handed to whoever is on top. Getting this wrong is the classic scene-system
// mistake: put the hero's HP inside the field and a battle cannot touch it; put
// it inside the battle and it dies when the battle does.

#define SAVE_FILE    "quest.dat"
#define SAVE_VERSION 1

typedef struct {
    uint32_t level, exp, gold;
    int32_t  hp, hpMax, mp, mpMax;
    uint32_t battles, steps;
    float    x, z, yaw;      // where you were standing when you saved
} Party;

static void partyReset(Party *p)
{
    memset(p, 0, sizeof(*p));
    p->level = 1;
    p->hpMax = 28; p->hp = p->hpMax;
    p->mpMax = 10; p->mp = p->mpMax;
}

static uint32_t expForNextLevel(uint32_t level)
{
    return 30u + level * 26u;
}

// --- what the game adds to a scene -------------------------------------------
//
// CremaScene carries no pointer back to the application, on purpose and for the
// same reason CremaEntityPool carries no allocator: the caller already has a
// struct, and a typed pointer in it beats a void* in the framework. So every
// scene here begins with these two fields, which makes one cast enough for all
// of them.

typedef struct App App;

typedef struct {
    CremaScene  base;
    App        *app;
} SceneBase;

static inline App *sceneApp(CremaScene *self)
{
    return ((SceneBase *)self)->app;
}

// Every transition in this game goes through here, because a request and "does
// it fade" are one decision made in one place. The fade itself is none of
// crema_scene's business: it parks the request and this file decides when the
// screen is black enough to apply it.
static void questRequest(App *a, CremaSceneOp op, CremaScene *target, bool fade);

// --- everything that outlives a scene ----------------------------------------
//
// Shaders, meshes, textures, the sound bank. Not because sharing is tidy but
// because of a number: compiling the two shaders below through CafeGLSL is
// logged at startup, and it is not a cost anything is allowed to pay when you
// walk into a monster. The line between what a scene owns and what the
// application owns is drawn by the clock, and this is the side with the
// milliseconds on it.

struct App {
    CremaShader *shToken, *shGround;
    CremaMesh    ship;
    GX2Texture   hull, font;
    GX2Sampler   sampler, fontSampler;

    CremaEffectRenderer fxRenderer;
    CremaHudRenderer    hudRenderer;

    CremaBank    bank;
    CremaMusic  *music;
    const CremaInstrument *sfxBlip, *sfxHit, *sfxCast;

    // token shader reflection
    int32_t  tokGlobalVs, tokGlobalPs, tokInst;
    uint32_t tokUnit;
    // ground shader reflection
    int32_t  grGlobalVs, grGlobalPs;
    uint32_t grUnit;

    Party  party;
    bool   canSave;
    float  saveFlash;      // seconds left on the "SAVED" notice

    // The stack, its caller-owned storage, and the five places.
    CremaSceneStack stack;
    CremaScene     *stackStorage[4];
    CremaScene *title, *field, *status, *battle, *over;

    // The fade, which lives here and not in crema_scene. It is a full-screen
    // quad, so it is a HUD list like any other and needs a uniform slice of its
    // own — it is drawn on top of scenes that are filling their own lists in
    // the same frame.
    bool             fadeThis;     // the parked request wants to go through black
    float            black;        // 0 = clear, 1 = black
    CremaUniformRing blackRing;
    HudList          blackList;
    const void      *blackUbo;
    uint32_t         blackCount;
};

#define FADE_TIME 0.22f

static void questRequest(App *a, CremaSceneOp op, CremaScene *target, bool fade)
{
    if (CremaScenePending(&a->stack))
        return;
    CremaSceneRequest(&a->stack, op, target);
    a->fadeThis = fade;
}

// What the field hands the battle at an encounter. Not a global out of
// laziness: it is the shape of a real question this PoC could not answer yet —
// a push carries no argument, so the two scenes need somewhere to meet. The
// party is where everything permanent meets; this is the one piece of data that
// belongs to the transition itself rather than to either side of it, and it
// stays a file-scope variable until a second example says what the right place
// for it is.
static uint32_t gEncounterTier = 1;

static uint32_t rngState = 0x2468ACEu;

static float randUnit(void)
{
    rngState = rngState * 1664525u + 1013904223u;
    return (float)((rngState >> 8) & 0xFFFF) / 65535.0f;
}

static int randRange(int lo, int hi)     // inclusive
{
    return lo + (int)(randUnit() * (float)(hi - lo + 1)) % (hi - lo + 1);
}

// --- token packing -----------------------------------------------------------

typedef struct {
    float pos[3];
    float scale;
    float tint[3];
    float yaw;
} Token;

static uint32_t packTokens(const Token *tok, uint32_t count, float (*out)[4])
{
    if (count > MAX_INSTANCES)
        count = MAX_INSTANCES;
    for (uint32_t i = 0; i < count; i++) {
        out[i * 2][0] = tok[i].pos[0];
        out[i * 2][1] = tok[i].pos[1];
        out[i * 2][2] = tok[i].pos[2];
        out[i * 2][3] = tok[i].scale;
        out[i * 2 + 1][0] = tok[i].tint[0];
        out[i * 2 + 1][1] = tok[i].tint[1];
        out[i * 2 + 1][2] = tok[i].tint[2];
        out[i * 2 + 1][3] = tok[i].yaw;
    }
    return count;
}

// The uniform rings a scene that draws a world needs. Five of them, allocated
// on enter and handed back on leave — this is the memory the drain protects.
typedef struct {
    CremaUniformRing globals, instances, fxViews, effects, hud;
    const void *globalUbo, *instUbo, *fxViewUbo, *fxUbo, *hudUbo;
    uint32_t    instCount, fxCount, hudCount;
    size_t      instBytes;
} SceneGfx;

static bool sceneGfxCreate(SceneGfx *g)
{
    memset(g, 0, sizeof(*g));
    g->instBytes = sizeof(float) * 4 * 2 * MAX_INSTANCES;
    return CremaUniformRingCreate(&g->globals, sizeof(Global),
                                  CREMA_FRAMES_IN_FLIGHT)
        && CremaUniformRingCreate(&g->instances, g->instBytes,
                                  CREMA_FRAMES_IN_FLIGHT)
        && CremaUniformRingCreate(&g->fxViews, sizeof(CremaEffectView),
                                  CREMA_FRAMES_IN_FLIGHT)
        && CremaUniformRingCreate(&g->effects, CREMA_EFFECT_BLOCK_BYTES,
                                  CREMA_FRAMES_IN_FLIGHT)
        && CremaUniformRingCreate(&g->hud, CREMA_HUD_BLOCK_BYTES,
                                  CREMA_FRAMES_IN_FLIGHT);
}

static void sceneGfxDestroy(SceneGfx *g)
{
    CremaUniformRingDestroy(&g->globals);
    CremaUniformRingDestroy(&g->instances);
    CremaUniformRingDestroy(&g->fxViews);
    CremaUniformRingDestroy(&g->effects);
    CremaUniformRingDestroy(&g->hud);
    memset(g, 0, sizeof(*g));
}

// The camera basis a billboard needs, from a view matrix we already built. The
// effect renderer asks for three numbers and this is where they come from.
static void gfxStoreFrame(SceneGfx *g, const App *app, uint32_t slot,
                          const Global *blk, Mat4 view,
                          const Token *tok, uint32_t tokCount,
                          const CremaEffectPool *fx, float (*fxScratch)[4],
                          const HudList *hud)
{
    g->globalUbo = CremaUniformRingStore(&g->globals, slot, blk, sizeof(*blk));

    static float instScratch[MAX_INSTANCES * 2][4];
    memset(instScratch, 0, sizeof(instScratch));
    g->instCount = packTokens(tok, tokCount, instScratch);
    g->instUbo   = CremaUniformRingStore(&g->instances, slot, instScratch,
                                         g->instBytes);

    CremaEffectView fxView;
    fxView.viewProj = blk->viewProj;
    // The view matrix's rows are the camera's axes in world space, which is
    // cheaper and more honest than recomputing them from angles the camera may
    // not even have.
    fxView.camRight[0] = view.m[0][0]; fxView.camRight[1] = view.m[1][0];
    fxView.camRight[2] = view.m[2][0]; fxView.camRight[3] = 0.0f;
    fxView.camUp[0]    = view.m[0][1]; fxView.camUp[1]    = view.m[1][1];
    fxView.camUp[2]    = view.m[2][1]; fxView.camUp[3]    = 0.0f;
    g->fxViewUbo = CremaUniformRingStore(&g->fxViews, slot, &fxView,
                                         sizeof(fxView));

    g->fxCount = fx ? CremaEffectPack(fx, fxScratch, MAX_EFFECTS) : 0;
    g->fxUbo   = CremaUniformRingStore(&g->effects, slot, fxScratch,
                                       CREMA_EFFECT_BLOCK_BYTES);

    g->hudCount = hud->count;
    g->hudUbo   = CremaUniformRingStore(&g->hud, slot, hud->items,
                                        CREMA_HUD_BLOCK_BYTES);
    (void)app;
}

static void gfxDrawTokens(const App *app, const SceneGfx *g)
{
    if (g->instCount == 0)
        return;
    CremaDepthSet(true, true);
    CremaBlendSet(CREMA_BLEND_OPAQUE);
    GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, TRUE);
    CremaShaderBind(app->shToken);
    GX2SetVertexUniformBlock(app->tokGlobalVs, sizeof(Global), g->globalUbo);
    GX2SetPixelUniformBlock(app->tokGlobalPs, sizeof(Global), g->globalUbo);
    GX2SetVertexUniformBlock(app->tokInst, g->instBytes, g->instUbo);
    GX2SetPixelTexture(&app->hull, app->tokUnit);
    GX2SetPixelSampler(&app->sampler, app->tokUnit);
    CremaMeshDraw(&app->ship, g->instCount);
}

static void gfxDrawOverlays(const App *app, const SceneGfx *g)
{
    CremaEffectDraw(&app->fxRenderer, g->fxViewUbo, g->fxUbo, g->fxCount);
    if (g->hudCount > 0)
        CremaHudDraw(&app->hudRenderer, g->hudUbo, g->hudCount,
                     &app->font, &app->fontSampler);
}

// =============================================================================
//  TITLE
// =============================================================================
//
// The cheapest scene in the game and the proof of the cheap end of the scale:
// it owns one uniform ring for a HUD list and nothing else, so entering it
// costs microseconds. It is also where the save file is read, because reading
// it anywhere else would mean reading it in a place that already had a party.

typedef struct {
    SceneBase        b;
    CremaUniformRing hudRing;
    HudList          list;
    const void      *hudUbo;
    uint32_t         hudCount;
    float            t;
    bool             hasSave;
    Party            saved;
} TitleScene;

static void titleEnter(CremaScene *self)
{
    TitleScene *s = (TitleScene *)self;
    CremaUniformRingCreate(&s->hudRing, CREMA_HUD_BLOCK_BYTES,
                           CREMA_FRAMES_IN_FLIGHT);
    s->t = 0.0f;
    s->hasSave = false;
    if (sceneApp(self)->canSave) {
        uint64_t before = OSGetSystemTime();
        size_t got = CremaSaveRead(SAVE_FILE, SAVE_VERSION, &s->saved,
                                   sizeof(s->saved));
        uint32_t us = (uint32_t)OSTicksToMicroseconds(OSGetSystemTime() - before);
        s->hasSave = (got == sizeof(s->saved));
        WHBLogPrintf("[poc13] journal %s in %s (%u us)",
                     s->hasSave ? "found" : "absent", CremaSaveDir(), us);
    }
    CremaMusicSetVolume(sceneApp(self)->music, 0.55f);
}

static void titleLeave(CremaScene *self)
{
    CremaUniformRingDestroy(&((TitleScene *)self)->hudRing);
}

static void titleUpdate(CremaScene *self, const CremaInput *in, float dt)
{
    TitleScene *s = (TitleScene *)self;
    App *app = sceneApp(self);
    s->t += dt;

    if (CremaInputPressed(in, VPAD_BUTTON_A)) {
        partyReset(&app->party);
        questRequest(app, CREMA_SCENE_GOTO, app->field, true);
    } else if (s->hasSave && CremaInputPressed(in, VPAD_BUTTON_X)) {
        // The whole point of the save, and the reason the field reads its
        // starting position out of the party rather than deciding it: continue
        // puts you back where you stood, not at a spawn point.
        app->party = s->saved;
        questRequest(app, CREMA_SCENE_GOTO, app->field, true);
    }
}

static void titleBuild(CremaScene *self, uint32_t slot)
{
    TitleScene *s = (TitleScene *)self;
    HudList *h = &s->list;
    hudClear(h);
    hudText(h, 366.0f, 168.0f, 62.0f, "CREMA", 1.0f, 0.86f, 0.48f, 1.0f);
    hudText(h, 366.0f, 246.0f, 34.0f, "QUEST", 0.86f, 0.92f, 1.0f, 0.95f);
    hudRect(h, 366.0f, 300.0f, 548.0f, 2.0f, 0.5f, 0.6f, 0.8f, 0.7f);

    if (fmodf(s->t, 1.0f) < 0.62f)
        hudText(h, 400.0f, 392.0f, 26.0f, "A - NEW QUEST", 0.9f, 0.9f, 0.9f, 1.0f);
    if (s->hasSave) {
        hudText(h, 400.0f, 440.0f, 26.0f, "X - CONTINUE", 0.75f, 0.9f, 0.8f, 1.0f);
        // The numbers that prove a struct went out to the card and came back
        // with its parts in the right order.
        hudText(h, 400.0f, 512.0f, 18.0f, "LV", 0.5f, 0.6f, 0.75f, 0.9f);
        hudNumber(h, 440.0f, 512.0f, 18.0f, s->saved.level, 2,
                  0.8f, 0.88f, 1.0f, 0.9f);
        hudText(h, 500.0f, 512.0f, 18.0f, "GOLD", 0.5f, 0.6f, 0.75f, 0.9f);
        hudNumber(h, 570.0f, 512.0f, 18.0f, s->saved.gold, 5,
                  0.9f, 0.85f, 0.55f, 0.9f);
        hudText(h, 400.0f, 542.0f, 18.0f, "BATTLES", 0.5f, 0.6f, 0.75f, 0.9f);
        hudNumber(h, 512.0f, 542.0f, 18.0f, s->saved.battles, 4,
                  0.8f, 0.88f, 1.0f, 0.9f);
        hudText(h, 610.0f, 542.0f, 18.0f, "STEPS", 0.5f, 0.6f, 0.75f, 0.9f);
        hudNumber(h, 700.0f, 542.0f, 18.0f, s->saved.steps, 5,
                  0.8f, 0.88f, 1.0f, 0.9f);
    } else {
        hudText(h, 400.0f, 512.0f, 18.0f, "NO JOURNAL ON THE CARD",
                0.45f, 0.52f, 0.62f, 0.9f);
    }

    s->hudCount = h->count;
    s->hudUbo   = CremaUniformRingStore(&s->hudRing, slot, h->items,
                                        CREMA_HUD_BLOCK_BYTES);
}

static void titleDraw(CremaScene *self)
{
    TitleScene *s = (TitleScene *)self;
    if (s->hudCount > 0)
        CremaHudDraw(&sceneApp(self)->hudRenderer, s->hudUbo, s->hudCount,
                     &sceneApp(self)->font, &sceneApp(self)->fontSampler);
}

// =============================================================================
//  FIELD
// =============================================================================
//
// The expensive scene, and deliberately so: it owns a ground texture it builds
// itself, a mip chain uploaded through GX2CopySurface, a vertex buffer and five
// uniform rings. Entering it is the slowest thing this game ever does, which is
// exactly what makes the battle a *push* and not a *goto* — coming back to a
// place you never left costs nothing at all, and the alternative would be
// paying that bill after every fight.

#define GROUND_HALF   240.0f
#define GROUND_TILES  30.0f            // texture repeats across the quad
#define GROUND_TEX    256
#define GROUND_STEP   (GROUND_HALF * 2.0f / GROUND_TILES)   // one tile in world units

#define FIELD_MAX_MONSTERS 6
#define FIELD_ENTITIES     16
#define HERO_SPEED        26.0f
#define HERO_RADIUS        2.4f
#define MONSTER_RADIUS     2.6f

enum { KIND_HERO = 1, KIND_MONSTER };

static const float GROUND_VERTS[] = {
    -GROUND_HALF, 0.0f, -GROUND_HALF,  0.0f,         0.0f,
     GROUND_HALF, 0.0f, -GROUND_HALF,  GROUND_TILES, 0.0f,
     GROUND_HALF, 0.0f,  GROUND_HALF,  GROUND_TILES, GROUND_TILES,
    -GROUND_HALF, 0.0f,  GROUND_HALF,  0.0f,         GROUND_TILES,
};
static const uint16_t GROUND_TRIS[] = { 0, 2, 1, 0, 3, 2 };
#define GROUND_STRIDE (5 * sizeof(float))

static uint8_t groundPixels[GROUND_TEX * GROUND_TEX * 4];

// Grass with a track worn through it. The track is what gives the eye something
// to measure walking against — a uniform field reads as standing still.
//
// It gives it rather more than that, as it turned out. The band is at constant
// X, so the lines run north-south, and that decides whether the save hitch below
// is visible at all: walking north you move *along* them and the picture is
// invariant under its own translation, so a dropped frame has no edge to jump;
// walking east you move *across* them and the same 42 ms lurches. Identical in
// the frame log. Not identical to look at, and the log cannot tell you which.
static void fillGroundTexture(uint8_t *dst)
{
    for (int y = 0; y < GROUND_TEX; y++) {
        for (int x = 0; x < GROUND_TEX; x++) {
            int n = (x * 37 + y * 17) % 21;
            bool blade = (((x * 3 + y * 5) >> 2) & 7) == 0;
            bool track = (x > 116 && x < 140);
            uint8_t r, g, b;
            if (track) {
                r = (uint8_t)(92 + n); g = (uint8_t)(80 + n); b = (uint8_t)(58 + n);
            } else if (blade) {
                r = (uint8_t)(36 + n); g = (uint8_t)(84 + n); b = (uint8_t)(44 + n);
            } else {
                r = (uint8_t)(26 + n); g = (uint8_t)(64 + n); b = (uint8_t)(36 + n);
            }
            uint8_t *px = dst + ((size_t)y * GROUND_TEX + x) * 4;
            px[0] = r; px[1] = g; px[2] = b; px[3] = 255;
        }
    }
}

typedef struct {
    SceneBase        b;
    SceneGfx        gfx;

    // The heavy things. Created in enter, destroyed in leave, and the numbers
    // for both are in the log.
    GX2Texture      ground;
    GX2RBuffer      groundVbo, groundIbo;
    bool            heavyReady;

    CremaEntityPool pool;
    CremaEntity     storage[FIELD_ENTITIES];
    float           wanderYaw[FIELD_ENTITIES];
    float           wanderTimer[FIELD_ENTITIES];
    uint32_t        tier[FIELD_ENTITIES];

    CremaEffectPool fx;
    CremaEffect     fxStorage[MAX_EFFECTS];

    float           spawnTimer;
    float           stepAccum;      // whole steps, for the journal
    Vec3            heroPos;
    float           heroYaw;
    float           t;
} FieldScene;

static CremaEntity *fieldSpawnMonster(FieldScene *s)
{
    CremaEntity *e = CremaEntitySpawn(&s->pool, KIND_MONSTER);
    if (!e)
        return NULL;
    float ang = randUnit() * 2.0f * PI;
    float dist = 70.0f + randUnit() * 70.0f;
    e->pos.x = s->heroPos.x + cosf(ang) * dist;
    e->pos.y = 0.0f;
    e->pos.z = s->heroPos.z + sinf(ang) * dist;
    e->radius = MONSTER_RADIUS;
    e->yaw = e->pitch = e->roll = 0.0f;
    int i = (int)(e - s->pool.items);
    s->wanderYaw[i]   = randUnit() * 2.0f * PI;
    s->wanderTimer[i] = 1.0f + randUnit() * 2.0f;
    // What is out there grows with you. Without this the first monster you met
    // could be a party of three, and a slice that kills you before you have
    // walked back to the field is a slice that never shows the field being
    // walked back to.
    uint32_t maxTier = 1u + s->b.app->party.level / 3u;
    if (maxTier > 3u)
        maxTier = 3u;
    s->tier[i] = (uint32_t)randRange(1, (int)maxTier);
    return e;
}

static void fieldEnter(CremaScene *self)
{
    FieldScene *s = (FieldScene *)self;
    App *app = sceneApp(self);

    uint64_t t0 = OSGetSystemTime();
    sceneGfxCreate(&s->gfx);
    uint32_t ringUs = (uint32_t)OSTicksToMicroseconds(OSGetSystemTime() - t0);

    uint64_t t1 = OSGetSystemTime();
    CremaBufferCreateVertex(&s->groundVbo, GROUND_STRIDE, 4, GROUND_VERTS);
    CremaBufferCreateIndexU16(&s->groundIbo, 6, GROUND_TRIS);
    uint32_t vboUs = (uint32_t)OSTicksToMicroseconds(OSGetSystemTime() - t1);

    // The bill. A 256x256 RGBA surface plus its whole mip chain, each level
    // pushed through a linear staging surface and a GX2CopySurface that ends in
    // GX2DrawDone — nine synchronous round trips to the GPU. It is the reason
    // this scene is entered once per quest and suspended thereafter.
    uint64_t t2 = OSGetSystemTime();
    fillGroundTexture(groundPixels);
    uint32_t genUs = (uint32_t)OSTicksToMicroseconds(OSGetSystemTime() - t2);

    uint64_t t3 = OSGetSystemTime();
    CremaTextureCreate(&s->ground, GROUND_TEX, GROUND_TEX,
                       CremaTextureMipLevels(GROUND_TEX, GROUND_TEX),
                       GX2_TILE_MODE_DEFAULT);
    CremaTextureUploadWithMips(&s->ground, groundPixels);
    uint32_t texUs = (uint32_t)OSTicksToMicroseconds(OSGetSystemTime() - t3);
    s->heavyReady = true;

    CremaEntityPoolInit(&s->pool, s->storage, FIELD_ENTITIES);
    CremaEffectPoolInit(&s->fx, s->fxStorage, MAX_EFFECTS);

    s->heroPos.x = app->party.x;
    s->heroPos.y = 0.0f;
    s->heroPos.z = app->party.z;
    s->heroYaw   = app->party.yaw;
    s->stepAccum = 0.0f;
    s->spawnTimer = 0.8f;
    s->t = 0.0f;

    CremaEntity *hero = CremaEntitySpawn(&s->pool, KIND_HERO);
    if (hero) {
        hero->pos = s->heroPos;
        hero->radius = HERO_RADIUS;
    }
    for (int i = 0; i < 3; i++)
        fieldSpawnMonster(s);

    CremaMusicSetVolume(app->music, 0.75f);
    WHBLogPrintf("[poc13] field ready: rings %u us | buffers %u us | "
                 "texture gen %u us | upload %u us", ringUs, vboUs, genUs, texUs);
}

static void fieldLeave(CremaScene *self)
{
    FieldScene *s = (FieldScene *)self;
    // Every one of these is memory the GPU was reading two frames ago, which is
    // why sceneApply drained it before calling this.
    CremaTextureDestroy(&s->ground);
    CremaBufferDestroy(&s->groundVbo);
    CremaBufferDestroy(&s->groundIbo);
    sceneGfxDestroy(&s->gfx);
    s->heavyReady = false;
}

static void fieldUpdate(CremaScene *self, const CremaInput *in, float dt)
{
    FieldScene *s = (FieldScene *)self;
    App *app = sceneApp(self);
    s->t += dt;

    if (app->saveFlash > 0.0f)
        app->saveFlash -= dt;

    CremaEntity *hero = NULL;
    for (uint32_t i = 0; i < s->pool.watermark; i++)
        if (s->pool.items[i].active && s->pool.items[i].kind == KIND_HERO)
            hero = &s->pool.items[i];

    // --- walking ------------------------------------------------------------
    float mx = in->leftX, mz = -in->leftY;
    float mag = sqrtf(mx * mx + mz * mz);
    if (hero && mag > 0.05f) {
        float step = HERO_SPEED * dt;
        hero->pos.x += mx / mag * step * mag;
        hero->pos.z += mz / mag * step * mag;
        s->heroYaw = atan2f(-mx, -mz);
        s->stepAccum += step * mag * 0.25f;
        while (s->stepAccum >= 1.0f) {
            s->stepAccum -= 1.0f;
            app->party.steps++;
        }
    }
    if (hero)
        s->heroPos = hero->pos;
    s->heroPos.y = 0.0f;

    // --- the monsters -------------------------------------------------------
    for (uint32_t i = 0; i < s->pool.watermark; i++) {
        CremaEntity *e = &s->pool.items[i];
        if (!e->active || e->kind != KIND_MONSTER)
            continue;

        s->wanderTimer[i] -= dt;
        if (s->wanderTimer[i] <= 0.0f) {
            s->wanderYaw[i] = randUnit() * 2.0f * PI;
            s->wanderTimer[i] = 1.2f + randUnit() * 2.2f;
        }
        // Close enough and they come for you, which is what turns a wandering
        // token into an encounter you can see arriving.
        float dx = s->heroPos.x - e->pos.x, dz = s->heroPos.z - e->pos.z;
        float d = sqrtf(dx * dx + dz * dz);
        float speed = 9.0f;
        if (d < 52.0f && d > 0.01f) {
            e->pos.x += dx / d * speed * 1.25f * dt;
            e->pos.z += dz / d * speed * 1.25f * dt;
            e->yaw = atan2f(-dx / d, -dz / d);
        } else {
            e->pos.x += cosf(s->wanderYaw[i]) * speed * dt;
            e->pos.z += sinf(s->wanderYaw[i]) * speed * dt;
            e->yaw = atan2f(-cosf(s->wanderYaw[i]), -sinf(s->wanderYaw[i]));
        }
        // Too far behind to matter: recycle it somewhere ahead.
        if (d > 320.0f)
            CremaEntityDespawn(&s->pool, e);

        if (hero && CremaSphereHitsSphere(hero->pos, hero->radius,
                                          e->pos, e->radius)) {
            // The encounter. The monster is taken off the field NOW, before the
            // fade, so that coming back cannot walk you straight into the same
            // one again — and the puff of dust it leaves is a scene-owned
            // effect that will die with the field, not with the battle.
            CremaEffectSpawn(&s->fx, e->pos, 0.5f, 3.0f, 9.0f,
                             1.0f, 0.85f, 0.55f, 0.9f);
            app->party.battles++;
            // What it is you walked into. A tier of 1..3 becomes one to three
            // monsters on the other side of the push.
            rngState ^= (uint32_t)(s->t * 1000.0f) + s->tier[i] * 7919u;
            gEncounterTier = s->tier[i];
            CremaEntityDespawn(&s->pool, e);
            if (app->sfxHit)
                CremaAudioPlay(&app->sfxHit->sound, 0.8f, 1.35f);
            questRequest(app, CREMA_SCENE_PUSH, app->battle, true);
            break;
        }
    }

    s->spawnTimer -= dt;
    if (s->spawnTimer <= 0.0f) {
        uint32_t live = 0;
        for (uint32_t i = 0; i < s->pool.watermark; i++)
            if (s->pool.items[i].active && s->pool.items[i].kind == KIND_MONSTER)
                live++;
        if (live < FIELD_MAX_MONSTERS)
            fieldSpawnMonster(s);
        s->spawnTimer = 2.5f;
    }

    CremaEffectUpdate(&s->fx, dt);

    // --- the two things you can do that are not walking ---------------------
    if (CremaInputPressed(in, VPAD_BUTTON_PLUS))
        questRequest(app, CREMA_SCENE_PUSH, app->status, false);

    // The only thing in this game that costs a frame, and it is not the one
    // anything was designed around. Twenty-four bytes to the SD, measured on
    // real hardware: 42 ms typically, and 209 ms the first time it replaced a
    // file left by a previous run — a quarter of a second, twelve frames, and
    // visible. Cemu says 1.2 ms for the same call and is wrong by forty times.
    //
    // The shape explains most of it: a write is fopen, fwrite, fclose, remove
    // and rename, which is four filesystem round trips through FSA and not one,
    // and the flush at the end is a card being programmed. The 209 ms outlier
    // is very likely the card erasing a block it had already written, which is
    // the slowest thing an SD card does and happens once.
    //
    // So this is on a button, deliberately: PoC 12 saved at a game over, when
    // the picture had stopped anyway, and never found out. A game that saves
    // where the player is still walking either wears the hitch or wants another
    // thread — and knowing which is what this measurement bought.
    if (CremaInputPressed(in, VPAD_BUTTON_Y) && app->canSave) {
        app->party.x   = s->heroPos.x;
        app->party.z   = s->heroPos.z;
        app->party.yaw = s->heroYaw;
        uint64_t before = OSGetSystemTime();
        bool ok = CremaSaveWrite(SAVE_FILE, SAVE_VERSION, &app->party,
                                 sizeof(app->party));
        uint32_t us = (uint32_t)OSTicksToMicroseconds(OSGetSystemTime() - before);
        app->saveFlash = ok ? 1.6f : 0.0f;
        WHBLogPrintf("[poc13] journal %s at (%.1f, %.1f): lv %u, %u gold, "
                     "%u battles, %u steps, %u us", ok ? "written" : "FAILED",
                     app->party.x, app->party.z, app->party.level,
                     app->party.gold, app->party.battles, app->party.steps, us);
        if (ok && app->sfxBlip)
            CremaAudioPlay(&app->sfxBlip->sound, 0.5f, 1.6f);
    }
}

static void fieldCamera(const FieldScene *s, Mat4 *outView, Mat4 *outViewProj,
                        Vec3 *outCam)
{
    Vec3 eye = { s->heroPos.x, 34.0f, s->heroPos.z + 40.0f };
    Vec3 at  = { s->heroPos.x, 3.0f,  s->heroPos.z - 6.0f };
    Vec3 up  = { 0.0f, 1.0f, 0.0f };
    Mat4 proj = mat4_perspective(52.0f * PI / 180.0f, 16.0f / 9.0f, 0.6f, 520.0f);
    *outView = mat4_look_at(eye, at, up);
    *outViewProj = mat4_mul(proj, *outView);
    *outCam = eye;
}

static void fieldHud(FieldScene *s, HudList *h)
{
    const Party *p = &s->b.app->party;
    hudClear(h);
    hudRect(h, 24.0f, 22.0f, 320.0f, 92.0f, 0.02f, 0.05f, 0.10f, 0.55f);
    hudFrame(h, 24.0f, 22.0f, 320.0f, 92.0f, 2.0f, 0.45f, 0.60f, 0.80f, 0.8f);
    hudText(h, 40.0f, 34.0f, 20.0f, "LV", 0.55f, 0.68f, 0.85f, 0.95f);
    hudNumber(h, 82.0f, 34.0f, 20.0f, p->level, 2, 1.0f, 1.0f, 1.0f, 1.0f);
    hudText(h, 140.0f, 34.0f, 20.0f, "HP", 0.6f, 0.85f, 0.6f, 0.95f);
    hudNumber(h, 182.0f, 34.0f, 20.0f, (uint32_t)(p->hp < 0 ? 0 : p->hp), 3,
              1.0f, 1.0f, 1.0f, 1.0f);
    hudBar(h, 40.0f, 64.0f, 288.0f, 12.0f,
           p->hpMax > 0 ? (float)p->hp / (float)p->hpMax : 0.0f,
           0.42f, 0.85f, 0.45f);
    hudBar(h, 40.0f, 82.0f, 288.0f, 8.0f,
           p->mpMax > 0 ? (float)p->mp / (float)p->mpMax : 0.0f,
           0.45f, 0.60f, 0.95f);

    hudText(h, 24.0f, 660.0f, 18.0f, "PLUS STATUS", 0.7f, 0.78f, 0.9f, 0.85f);
    hudText(h, 24.0f, 686.0f, 18.0f, "Y  WRITE JOURNAL", 0.7f, 0.78f, 0.9f, 0.85f);

    if (s->b.app->saveFlash > 0.0f &&
        fmodf(s->b.app->saveFlash, 0.4f) < 0.26f)
        hudText(h, 540.0f, 620.0f, 28.0f, "JOURNAL SAVED",
                0.95f, 0.92f, 0.6f, 1.0f);

    // A compass rose of nearby monsters, which is a radar and is also the only
    // way to notice that they kept wandering while you were in a battle.
    for (uint32_t i = 0; i < s->pool.watermark; i++) {
        const CremaEntity *e = &s->pool.items[i];
        if (!e->active || e->kind != KIND_MONSTER)
            continue;
        float dx = (e->pos.x - s->heroPos.x) * 0.34f;
        float dz = (e->pos.z - s->heroPos.z) * 0.34f;
        if (dx < -52.0f || dx > 52.0f || dz < -52.0f || dz > 52.0f)
            continue;
        hudRect(h, 1180.0f + dx, 96.0f + dz, 6.0f, 6.0f,
                1.0f, 0.45f, 0.40f, 0.95f);
    }
    hudFrame(h, 1128.0f, 44.0f, 112.0f, 112.0f, 2.0f, 0.45f, 0.6f, 0.8f, 0.7f);
    hudRect(h, 1181.0f, 97.0f, 4.0f, 4.0f, 0.85f, 0.95f, 1.0f, 1.0f);
}

static float fieldFxScratch[MAX_EFFECTS * 2][4];

static void fieldBuild(CremaScene *self, uint32_t slot)
{
    FieldScene *s = (FieldScene *)self;
    Mat4 view, viewProj;
    Vec3 cam;
    fieldCamera(s, &view, &viewProj, &cam);

    Global blk;
    memset(&blk, 0, sizeof(blk));
    blk.viewProj = viewProj;
    Vec3 lightRaw = { -0.4f, -0.82f, -0.4f };
    Vec3 light = vec3_normalize(lightRaw);
    blk.lightDir[0] = light.x; blk.lightDir[1] = light.y; blk.lightDir[2] = light.z;
    blk.camPos[0] = cam.x; blk.camPos[1] = cam.y; blk.camPos[2] = cam.z;
    blk.fog[0] = 160.0f; blk.fog[1] = 420.0f;
    blk.fogColor[0] = 0.62f; blk.fogColor[1] = 0.74f; blk.fogColor[2] = 0.86f;
    // Snapped to a whole tile, so the ground moving with you is invisible.
    blk.groundOffset[0] = floorf(s->heroPos.x / GROUND_STEP) * GROUND_STEP;
    blk.groundOffset[2] = floorf(s->heroPos.z / GROUND_STEP) * GROUND_STEP;

    static Token tokens[MAX_INSTANCES];
    uint32_t n = 0;
    for (uint32_t i = 0; i < s->pool.watermark && n < MAX_INSTANCES; i++) {
        const CremaEntity *e = &s->pool.items[i];
        if (!e->active)
            continue;
        tokens[n].pos[0] = e->pos.x;
        tokens[n].pos[2] = e->pos.z;
        if (e->kind == KIND_HERO) {
            // a small bob, so standing still does not look like a paused game
            tokens[n].pos[1] = 2.2f + sinf(s->t * 3.4f) * 0.25f;
            tokens[n].scale = 1.9f;
            tokens[n].tint[0] = 0.78f; tokens[n].tint[1] = 0.92f;
            tokens[n].tint[2] = 1.0f;
            tokens[n].yaw = s->heroYaw;
        } else {
            tokens[n].pos[1] = 2.0f + sinf(s->t * 2.6f + (float)i) * 0.35f;
            tokens[n].scale = 1.5f + 0.28f * (float)s->tier[i];
            tokens[n].tint[0] = 1.0f;
            tokens[n].tint[1] = 0.36f + 0.12f * (float)s->tier[i];
            tokens[n].tint[2] = 0.32f;
            tokens[n].yaw = e->yaw;
        }
        n++;
    }

    static HudList hud;
    fieldHud(s, &hud);
    gfxStoreFrame(&s->gfx, sceneApp(self), slot, &blk, view, tokens, n,
                  &s->fx, fieldFxScratch, &hud);
}

static void fieldDraw(CremaScene *self)
{
    FieldScene *s = (FieldScene *)self;
    const App *app = sceneApp(self);
    if (!s->heavyReady)
        return;

    CremaDepthSet(true, true);
    CremaBlendSet(CREMA_BLEND_OPAQUE);
    GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, TRUE);
    CremaShaderBind(app->shGround);
    GX2SetVertexUniformBlock(app->grGlobalVs, sizeof(Global), s->gfx.globalUbo);
    GX2SetPixelUniformBlock(app->grGlobalPs, sizeof(Global), s->gfx.globalUbo);
    GX2SetPixelTexture(&s->ground, app->grUnit);
    GX2SetPixelSampler(&app->sampler, app->grUnit);
    GX2RSetAttributeBuffer(&s->groundVbo, 0, GROUND_STRIDE, 0);
    GX2RDrawIndexed(GX2_PRIMITIVE_MODE_TRIANGLES, &s->groundIbo,
                    GX2_INDEX_TYPE_U16, 6, 0, 0, 1);

    gfxDrawTokens(app, &s->gfx);
    gfxDrawOverlays(app, &s->gfx);
}

// =============================================================================
//  STATUS
// =============================================================================
//
// The see-through one, and the case that decided the interface. It is pushed
// over the field: the field is still drawn, is not updated, and nothing at all
// is freed — so no fade, no drain, and opening it is instant. If a scene stack
// only ever replaced one picture with another, this would be impossible and the
// `opaque` flag would not need to exist.

typedef struct {
    SceneBase        b;
    CremaUniformRing hudRing;
    HudList          list;
    const void      *hudUbo;
    uint32_t         hudCount;
    float            t;
} StatusScene;

static void statusEnter(CremaScene *self)
{
    StatusScene *s = (StatusScene *)self;
    CremaUniformRingCreate(&s->hudRing, CREMA_HUD_BLOCK_BYTES,
                           CREMA_FRAMES_IN_FLIGHT);
    s->t = 0.0f;
    CremaMusicSetVolume(sceneApp(self)->music, 0.35f);
    if (sceneApp(self)->sfxBlip)
        CremaAudioPlay(&sceneApp(self)->sfxBlip->sound, 0.4f, 1.7f);
}

static void statusLeave(CremaScene *self)
{
    StatusScene *s = (StatusScene *)self;
    CremaUniformRingDestroy(&s->hudRing);
    CremaMusicSetVolume(sceneApp(self)->music, 0.75f);
}

static void statusUpdate(CremaScene *self, const CremaInput *in, float dt)
{
    StatusScene *s = (StatusScene *)self;
    s->t += dt;
    if (CremaInputPressed(in, VPAD_BUTTON_PLUS | VPAD_BUTTON_B))
        questRequest(sceneApp(self), CREMA_SCENE_POP, NULL, false);
}

static void statusBuild(CremaScene *self, uint32_t slot)
{
    StatusScene *s = (StatusScene *)self;
    const Party *p = &sceneApp(self)->party;
    HudList *h = &s->list;
    hudClear(h);

    // Dimmed, not black: the point of a see-through scene is that you can see
    // the place you are standing in behind it.
    hudRect(h, 0.0f, 0.0f, HUD_VIRTUAL_W, HUD_VIRTUAL_H, 0.0f, 0.0f, 0.0f, 0.55f);
    hudRect(h, 300.0f, 130.0f, 680.0f, 440.0f, 0.05f, 0.08f, 0.14f, 0.92f);
    hudFrame(h, 300.0f, 130.0f, 680.0f, 440.0f, 3.0f, 0.55f, 0.68f, 0.88f, 1.0f);

    hudText(h, 336.0f, 158.0f, 32.0f, "JOURNAL", 1.0f, 0.88f, 0.55f, 1.0f);
    hudRect(h, 336.0f, 200.0f, 608.0f, 2.0f, 0.4f, 0.5f, 0.65f, 0.8f);

    float y = 230.0f;
    hudText(h, 336.0f, y, 24.0f, "LEVEL", 0.6f, 0.72f, 0.88f, 1.0f);
    hudNumber(h, 700.0f, y, 24.0f, p->level, 3, 1.0f, 1.0f, 1.0f, 1.0f);
    y += 40.0f;
    hudText(h, 336.0f, y, 24.0f, "HP", 0.6f, 0.85f, 0.6f, 1.0f);
    hudNumber(h, 700.0f, y, 24.0f, (uint32_t)(p->hp < 0 ? 0 : p->hp), 3,
              1.0f, 1.0f, 1.0f, 1.0f);
    hudText(h, 760.0f, y, 24.0f, "OF", 0.5f, 0.6f, 0.7f, 1.0f);
    hudNumber(h, 812.0f, y, 24.0f, (uint32_t)p->hpMax, 3, 0.8f, 0.9f, 0.8f, 1.0f);
    y += 40.0f;
    hudText(h, 336.0f, y, 24.0f, "MP", 0.6f, 0.7f, 0.95f, 1.0f);
    hudNumber(h, 700.0f, y, 24.0f, (uint32_t)(p->mp < 0 ? 0 : p->mp), 3,
              1.0f, 1.0f, 1.0f, 1.0f);
    hudText(h, 760.0f, y, 24.0f, "OF", 0.5f, 0.6f, 0.7f, 1.0f);
    hudNumber(h, 812.0f, y, 24.0f, (uint32_t)p->mpMax, 3, 0.8f, 0.85f, 1.0f, 1.0f);
    y += 40.0f;
    hudText(h, 336.0f, y, 24.0f, "EXP TO NEXT", 0.6f, 0.72f, 0.88f, 1.0f);
    uint32_t need = expForNextLevel(p->level);
    hudNumber(h, 700.0f, y, 24.0f, need > p->exp ? need - p->exp : 0, 4,
              1.0f, 1.0f, 1.0f, 1.0f);
    y += 40.0f;
    hudText(h, 336.0f, y, 24.0f, "GOLD", 0.9f, 0.85f, 0.55f, 1.0f);
    hudNumber(h, 700.0f, y, 24.0f, p->gold, 5, 1.0f, 0.95f, 0.7f, 1.0f);
    y += 40.0f;
    hudText(h, 336.0f, y, 24.0f, "BATTLES", 0.6f, 0.72f, 0.88f, 1.0f);
    hudNumber(h, 700.0f, y, 24.0f, p->battles, 4, 1.0f, 1.0f, 1.0f, 1.0f);
    y += 40.0f;
    hudText(h, 336.0f, y, 24.0f, "STEPS", 0.6f, 0.72f, 0.88f, 1.0f);
    hudNumber(h, 700.0f, y, 24.0f, p->steps, 5, 1.0f, 1.0f, 1.0f, 1.0f);

    if (fmodf(s->t, 1.0f) < 0.65f)
        hudText(h, 396.0f, 528.0f, 20.0f, "PLUS OR B TO CLOSE",
                0.85f, 0.9f, 0.95f, 1.0f);

    s->hudCount = h->count;
    s->hudUbo   = CremaUniformRingStore(&s->hudRing, slot, h->items,
                                        CREMA_HUD_BLOCK_BYTES);
}

static void statusDraw(CremaScene *self)
{
    StatusScene *s = (StatusScene *)self;
    if (s->hudCount > 0)
        CremaHudDraw(&sceneApp(self)->hudRenderer, s->hudUbo, s->hudCount,
                     &sceneApp(self)->font, &sceneApp(self)->fontSampler);
}

// =============================================================================
//  BATTLE
// =============================================================================
//
// Five uniform rings and an entity pool, no textures of its own — so entering
// it costs microseconds where the field cost milliseconds. That contrast is the
// finding, not the fight.
//
// It also has a state machine INSIDE it, which is the distinction PoC 12 could
// not have shown: a scene is not a state. The battle passes through intro,
// menu, the hero's blow, each monster's reply and an outcome, and none of those
// owns anything or can be entered from outside. States are bookkeeping within
// one lifetime; a scene *is* a lifetime.

#define BATTLE_FOES 3

enum {
    BS_INTRO, BS_MENU, BS_HERO_ACT, BS_FOE_ACT, BS_WIN, BS_FLED, BS_LOST,
};

enum { CMD_FIGHT = 0, CMD_MAGIC, CMD_HEAL, CMD_RUN, CMD_COUNT };

typedef struct {
    SceneBase        b;
    SceneGfx        gfx;

    CremaEntityPool pool;
    CremaEntity     storage[BATTLE_FOES + 1];
    int32_t         hp[BATTLE_FOES + 1];
    int32_t         hpMax[BATTLE_FOES + 1];
    int32_t         atk[BATTLE_FOES + 1];
    float           flash[BATTLE_FOES + 1];
    uint32_t        tier[BATTLE_FOES + 1];

    CremaEffectPool fx;
    CremaEffect     fxStorage[MAX_EFFECTS];

    int             state;
    float           timer;
    int             cursor;
    int             actor;         // which foe is replying
    char            message[44];
    uint32_t        rewardExp, rewardGold;
    bool            leveled;
    // "You cannot afford that" is not a move. The message still has to be read,
    // so it goes through the same timed state every action does, and this is
    // what tells that state to hand the menu back instead of letting the
    // monsters reply to a turn the player never got to take. A failed escape is
    // the opposite case and rightly costs the turn: it was an attempt.
    bool            denied;
    float           t;
    float           shake;         // the hero got hit: the camera says so
} BattleScene;

static void battleSay(BattleScene *s, const char *text)
{
    snprintf(s->message, sizeof(s->message), "%s", text);
}

static void battleSayNum(BattleScene *s, const char *a, int n, const char *b)
{
    snprintf(s->message, sizeof(s->message), "%s%d%s", a, n, b);
}

static CremaEntity *battleHero(BattleScene *s)
{
    for (uint32_t i = 0; i < s->pool.watermark; i++)
        if (s->pool.items[i].active && s->pool.items[i].kind == KIND_HERO)
            return &s->pool.items[i];
    return NULL;
}

static int battleLiveFoes(const BattleScene *s)
{
    int n = 0;
    for (uint32_t i = 0; i < s->pool.watermark; i++)
        if (s->pool.items[i].active && s->pool.items[i].kind == KIND_MONSTER)
            n++;
    return n;
}

static void battleEnter(CremaScene *self)
{
    BattleScene *s = (BattleScene *)self;
    App *app = sceneApp(self);

    uint64_t t0 = OSGetSystemTime();
    sceneGfxCreate(&s->gfx);
    CremaEntityPoolInit(&s->pool, s->storage, BATTLE_FOES + 1);
    CremaEffectPoolInit(&s->fx, s->fxStorage, MAX_EFFECTS);

    CremaEntity *hero = CremaEntitySpawn(&s->pool, KIND_HERO);
    if (hero) {
        Vec3 p = { 15.0f, 2.6f, 6.0f };
        hero->pos = p;
        hero->radius = 3.0f;
        hero->yaw = PI * 0.5f;              // facing -X, at the monsters
    }

    uint32_t tier = gEncounterTier < 1 ? 1 : (gEncounterTier > 3 ? 3 : gEncounterTier);
    int count = (int)tier;
    s->rewardExp = 0;
    s->rewardGold = 0;
    for (int i = 0; i < count; i++) {
        CremaEntity *e = CremaEntitySpawn(&s->pool, KIND_MONSTER);
        if (!e)
            break;
        Vec3 p = { -16.0f, 2.4f, -8.0f + 8.0f * (float)i };
        e->pos = p;
        e->radius = 3.0f;
        e->yaw = -PI * 0.5f;
        int slot = (int)(e - s->pool.items);
        s->hpMax[slot] = 8 + (int)tier * 4 + randRange(0, 3)
                       + (int)app->party.level;
        s->hp[slot]    = s->hpMax[slot];
        s->atk[slot]   = 2 + (int)tier + (int)app->party.level / 2;
        s->flash[slot] = 0.0f;
        s->tier[slot]  = tier;
        s->rewardExp  += 8u + tier * 6u;
        s->rewardGold += 4u + tier * 5u;
    }

    s->state  = BS_INTRO;
    s->timer  = 0.75f;
    s->cursor = CMD_FIGHT;
    s->actor  = 0;
    s->t      = 0.0f;
    s->shake  = 0.0f;
    s->leveled = false;
    battleSay(s, count > 1 ? "MONSTERS BLOCK THE PATH"
                           : "A MONSTER BLOCKS THE PATH");

    CremaMusicSetVolume(app->music, 1.0f);
    uint32_t us = (uint32_t)OSTicksToMicroseconds(OSGetSystemTime() - t0);
    WHBLogPrintf("[poc13] battle: %d foe(s), tier %u, %u us to set up",
                 count, tier, us);
}

static void battleLeave(CremaScene *self)
{
    BattleScene *s = (BattleScene *)self;
    sceneGfxDestroy(&s->gfx);
}

static void battleHit(BattleScene *s, int slot, int damage, bool magic)
{
    App *app = s->b.app;
    s->hp[slot] -= damage;
    s->flash[slot] = 0.28f;
    CremaEntity *e = &s->pool.items[slot];
    CremaEffectSpawn(&s->fx, e->pos, 0.34f, 2.2f, 7.0f,
                     magic ? 0.55f : 1.0f, magic ? 0.75f : 0.72f,
                     magic ? 1.0f : 0.3f, 1.0f);
    if (app->sfxHit)
        CremaAudioPlay(&app->sfxHit->sound, 0.7f, magic ? 1.4f : 1.05f);
    if (s->hp[slot] <= 0) {
        CremaEffectSpawn(&s->fx, e->pos, 0.6f, 3.5f, 12.0f,
                         1.0f, 0.6f, 0.25f, 1.0f);
        CremaEntityDespawn(&s->pool, e);
    }
}

static void battleFinishHeroTurn(BattleScene *s)
{
    if (s->denied) {
        s->denied = false;
        s->state = BS_MENU;
        battleSay(s, "");
        return;
    }
    if (battleLiveFoes(s) == 0) {
        App *app = s->b.app;
        app->party.exp  += s->rewardExp;
        app->party.gold += s->rewardGold;
        s->leveled = false;
        while (app->party.exp >= expForNextLevel(app->party.level)) {
            app->party.exp -= expForNextLevel(app->party.level);
            app->party.level++;
            app->party.hpMax += 7;
            app->party.mpMax += 3;
            app->party.hp = app->party.hpMax;
            app->party.mp = app->party.mpMax;
            s->leveled = true;
        }
        s->state = BS_WIN;
        s->timer = s->leveled ? 2.2f : 1.5f;
        battleSay(s, s->leveled ? "LEVEL UP" : "VICTORY");
        return;
    }
    s->state = BS_FOE_ACT;
    s->actor = 0;
    s->timer = 0.35f;
}

static void battleUpdate(CremaScene *self, const CremaInput *in, float dt)
{
    BattleScene *s = (BattleScene *)self;
    App *app = sceneApp(self);
    Party *p = &app->party;
    s->t += dt;
    s->timer -= dt;
    if (s->shake > 0.0f)
        s->shake -= dt;
    for (int i = 0; i <= BATTLE_FOES; i++)
        if (s->flash[i] > 0.0f)
            s->flash[i] -= dt;
    CremaEffectUpdate(&s->fx, dt);

    switch (s->state) {
    case BS_INTRO:
        if (s->timer <= 0.0f) {
            s->state = BS_MENU;
            battleSay(s, "");
        }
        break;

    case BS_MENU:
        if (CremaInputPressed(in, VPAD_BUTTON_UP)) {
            s->cursor = (s->cursor + CMD_COUNT - 1) % CMD_COUNT;
            if (app->sfxBlip)
                CremaAudioPlay(&app->sfxBlip->sound, 0.32f, 1.8f);
        }
        if (CremaInputPressed(in, VPAD_BUTTON_DOWN)) {
            s->cursor = (s->cursor + 1) % CMD_COUNT;
            if (app->sfxBlip)
                CremaAudioPlay(&app->sfxBlip->sound, 0.32f, 1.8f);
        }
        if (CremaInputPressed(in, VPAD_BUTTON_A)) {
            if (s->cursor == CMD_FIGHT) {
                // the front-most survivor takes it
                int target = -1;
                for (uint32_t i = 0; i < s->pool.watermark; i++)
                    if (s->pool.items[i].active &&
                        s->pool.items[i].kind == KIND_MONSTER) {
                        target = (int)i;
                        break;
                    }
                if (target >= 0) {
                    int dmg = 6 + (int)p->level * 3 + randRange(0, 3);
                    battleHit(s, target, dmg, false);
                    battleSayNum(s, "HIT FOR ", dmg, "");
                }
                s->state = BS_HERO_ACT;
                s->timer = 0.55f;
            } else if (s->cursor == CMD_HEAL) {
                if (p->mp < 4) {
                    battleSay(s, "NOT ENOUGH MP");
                    s->denied = true;
                    s->state = BS_HERO_ACT;
                    s->timer = 0.7f;
                    break;
                }
                p->mp -= 4;
                int amount = 12 + (int)p->level * 4;
                if (p->hp + amount > p->hpMax)
                    amount = p->hpMax - p->hp;
                p->hp += amount;
                CremaEntity *hero = battleHero(s);
                if (hero)
                    CremaEffectSpawn(&s->fx, hero->pos, 0.5f, 2.0f, 8.0f,
                                     0.55f, 1.0f, 0.65f, 1.0f);
                if (app->sfxCast)
                    CremaAudioPlay(&app->sfxCast->sound, 0.6f, 1.5f);
                battleSayNum(s, "MENDED ", amount, "");
                s->state = BS_HERO_ACT;
                s->timer = 0.7f;
            } else if (s->cursor == CMD_MAGIC) {
                if (p->mp < 3) {
                    battleSay(s, "NOT ENOUGH MP");
                    // and no turn is spent: a menu that punishes a misread is
                    // a menu nobody reads twice
                    s->denied = true;
                    s->state = BS_HERO_ACT;
                    s->timer = 0.7f;
                    break;
                }
                p->mp -= 3;
                if (app->sfxCast)
                    CremaAudioPlay(&app->sfxCast->sound, 0.75f, 0.7f);
                int dmg = 6 + (int)p->level * 3;
                for (uint32_t i = 0; i < s->pool.watermark; i++)
                    if (s->pool.items[i].active &&
                        s->pool.items[i].kind == KIND_MONSTER)
                        battleHit(s, (int)i, dmg, true);
                battleSayNum(s, "SPARK FOR ", dmg, " EACH");
                s->state = BS_HERO_ACT;
                s->timer = 0.7f;
            } else {
                if (randUnit() < 0.55f) {
                    battleSay(s, "GOT AWAY");
                    s->state = BS_FLED;
                    s->timer = 0.8f;
                } else {
                    battleSay(s, "COULD NOT ESCAPE");
                    s->state = BS_HERO_ACT;
                    s->timer = 0.7f;
                }
            }
        }
        break;

    case BS_HERO_ACT:
        if (s->timer <= 0.0f)
            battleFinishHeroTurn(s);
        break;

    case BS_FOE_ACT:
        if (s->timer > 0.0f)
            break;
        {
            // Walk the pool looking for the next live foe that has not acted.
            int slot = -1;
            for (uint32_t i = (uint32_t)s->actor; i < s->pool.watermark; i++)
                if (s->pool.items[i].active &&
                    s->pool.items[i].kind == KIND_MONSTER) {
                    slot = (int)i;
                    break;
                }
            if (slot < 0) {
                s->state = BS_MENU;
                battleSay(s, "");
                break;
            }
            s->actor = slot + 1;
            int dmg = s->atk[slot] + randRange(0, 3) - (int)p->level;
            if (dmg < 1)
                dmg = 1;
            p->hp -= dmg;
            s->shake = 0.22f;
            CremaEntity *hero = battleHero(s);
            if (hero)
                CremaEffectSpawn(&s->fx, hero->pos, 0.3f, 2.4f, 7.5f,
                                 1.0f, 0.35f, 0.3f, 1.0f);
            if (app->sfxHit)
                CremaAudioPlay(&app->sfxHit->sound, 0.85f, 0.72f);
            battleSayNum(s, "TOOK ", dmg, " DAMAGE");
            if (p->hp <= 0) {
                p->hp = 0;
                s->state = BS_LOST;
                s->timer = 1.1f;
                battleSay(s, "");
            } else {
                s->timer = 0.55f;
            }
        }
        break;

    case BS_WIN:
        if (s->timer <= 0.0f) {
            WHBLogPrintf("[poc13] victory: +%u exp, +%u gold -> lv %u, "
                         "%d/%d hp, %d/%d mp", s->rewardExp, s->rewardGold,
                         p->level, p->hp, p->hpMax, p->mp, p->mpMax);
            questRequest(app, CREMA_SCENE_POP, NULL, true);
        }
        break;

    case BS_FLED:
        if (s->timer <= 0.0f)
            questRequest(app, CREMA_SCENE_POP, NULL, true);
        break;

    case BS_LOST:
        if (s->timer <= 0.0f)
            questRequest(app, CREMA_SCENE_PUSH, app->over, false);
        break;
    }
}

static float battleFxScratch[MAX_EFFECTS * 2][4];

static void battleHud(BattleScene *s, HudList *h)
{
    const Party *p = &s->b.app->party;
    hudClear(h);

    // The hero's plate, bottom left.
    hudRect(h, 40.0f, 520.0f, 380.0f, 150.0f, 0.04f, 0.06f, 0.12f, 0.88f);
    hudFrame(h, 40.0f, 520.0f, 380.0f, 150.0f, 3.0f, 0.55f, 0.68f, 0.88f, 1.0f);
    hudText(h, 62.0f, 540.0f, 24.0f, "HERO", 1.0f, 0.9f, 0.6f, 1.0f);
    hudText(h, 240.0f, 544.0f, 18.0f, "LV", 0.6f, 0.72f, 0.88f, 1.0f);
    hudNumber(h, 282.0f, 544.0f, 18.0f, p->level, 2, 1.0f, 1.0f, 1.0f, 1.0f);
    hudText(h, 62.0f, 580.0f, 20.0f, "HP", 0.6f, 0.85f, 0.6f, 1.0f);
    hudNumber(h, 104.0f, 580.0f, 20.0f, (uint32_t)(p->hp < 0 ? 0 : p->hp), 3,
              1.0f, 1.0f, 1.0f, 1.0f);
    hudBar(h, 176.0f, 582.0f, 220.0f, 12.0f,
           p->hpMax > 0 ? (float)p->hp / (float)p->hpMax : 0.0f,
           0.42f, 0.85f, 0.45f);
    hudText(h, 62.0f, 616.0f, 20.0f, "MP", 0.6f, 0.7f, 0.95f, 1.0f);
    hudNumber(h, 104.0f, 616.0f, 20.0f, (uint32_t)(p->mp < 0 ? 0 : p->mp), 3,
              1.0f, 1.0f, 1.0f, 1.0f);
    hudBar(h, 176.0f, 618.0f, 220.0f, 12.0f,
           p->mpMax > 0 ? (float)p->mp / (float)p->mpMax : 0.0f,
           0.45f, 0.60f, 0.95f);

    // The monsters' HP, top right, one row each.
    float y = 44.0f;
    for (uint32_t i = 0; i < s->pool.watermark; i++) {
        const CremaEntity *e = &s->pool.items[i];
        if (!e->active || e->kind != KIND_MONSTER)
            continue;
        hudRect(h, 900.0f, y, 340.0f, 40.0f, 0.06f, 0.03f, 0.05f, 0.82f);
        hudFrame(h, 900.0f, y, 340.0f, 40.0f, 2.0f, 0.8f, 0.45f, 0.42f, 0.9f);
        hudText(h, 916.0f, y + 10.0f, 18.0f,
                s->tier[i] >= 3 ? "BRUTE" : (s->tier[i] == 2 ? "PROWLER"
                                                             : "CRAWLER"),
                1.0f, 0.75f, 0.7f, 1.0f);
        hudBar(h, 1064.0f, y + 12.0f, 160.0f, 14.0f,
               s->hpMax[i] > 0 ? (float)s->hp[i] / (float)s->hpMax[i] : 0.0f,
               0.9f, 0.42f, 0.38f);
        y += 50.0f;
    }

    // The command list, and the cursor is a rectangle because everything is.
    if (s->state == BS_MENU) {
        static const char *CMDS[CMD_COUNT] = { "FIGHT", "SPARK", "MEND", "RUN" };
        hudRect(h, 460.0f, 496.0f, 240.0f, 174.0f, 0.04f, 0.06f, 0.12f, 0.88f);
        hudFrame(h, 460.0f, 496.0f, 240.0f, 174.0f, 3.0f,
                 0.55f, 0.68f, 0.88f, 1.0f);
        for (int i = 0; i < CMD_COUNT; i++) {
            float ry = 514.0f + (float)i * 38.0f;
            bool on = (i == s->cursor);
            if (on)
                hudRect(h, 476.0f, ry - 5.0f, 208.0f, 34.0f,
                        0.20f, 0.34f, 0.52f, 0.9f);
            hudText(h, 506.0f, ry, 24.0f, CMDS[i],
                    on ? 1.0f : 0.7f, on ? 1.0f : 0.78f, on ? 1.0f : 0.88f, 1.0f);
        }
        if (fmodf(s->t, 0.8f) < 0.5f)
            hudText(h, 482.0f, 514.0f + (float)s->cursor * 38.0f, 24.0f, ">",
                    1.0f, 0.9f, 0.5f, 1.0f);
        hudText(h, 462.0f, 682.0f, 16.0f, "SPARK 3 MP   MEND 4 MP",
                0.6f, 0.68f, 0.8f, 0.85f);
    }

    if (s->message[0]) {
        hudRect(h, 300.0f, 440.0f, 680.0f, 56.0f, 0.02f, 0.04f, 0.08f, 0.85f);
        hudFrame(h, 300.0f, 440.0f, 680.0f, 56.0f, 2.0f,
                 0.5f, 0.6f, 0.8f, 0.9f);
        hudText(h, 324.0f, 456.0f, 26.0f, s->message, 1.0f, 1.0f, 1.0f, 1.0f);
    }

    if (s->state == BS_WIN) {
        hudText(h, 470.0f, 200.0f, 22.0f, "EXP", 0.7f, 0.85f, 1.0f, 1.0f);
        hudNumber(h, 540.0f, 200.0f, 22.0f, s->rewardExp, 4,
                  1.0f, 1.0f, 1.0f, 1.0f);
        hudText(h, 640.0f, 200.0f, 22.0f, "GOLD", 0.9f, 0.85f, 0.55f, 1.0f);
        hudNumber(h, 730.0f, 200.0f, 22.0f, s->rewardGold, 4,
                  1.0f, 0.95f, 0.7f, 1.0f);
    }
}

static void battleBuild(CremaScene *self, uint32_t slot)
{
    BattleScene *s = (BattleScene *)self;

    float jolt = s->shake > 0.0f ? sinf(s->t * 90.0f) * s->shake * 3.0f : 0.0f;
    Vec3 eye = { jolt, 13.0f, 38.0f };
    Vec3 at  = { 0.0f, 3.0f, -2.0f };
    Vec3 up  = { 0.0f, 1.0f, 0.0f };
    Mat4 proj = mat4_perspective(48.0f * PI / 180.0f, 16.0f / 9.0f, 0.6f, 240.0f);
    Mat4 view = mat4_look_at(eye, at, up);

    Global blk;
    memset(&blk, 0, sizeof(blk));
    blk.viewProj = mat4_mul(proj, view);
    Vec3 lightRaw = { -0.3f, -0.78f, -0.55f };
    Vec3 light = vec3_normalize(lightRaw);
    blk.lightDir[0] = light.x; blk.lightDir[1] = light.y; blk.lightDir[2] = light.z;
    blk.camPos[0] = eye.x; blk.camPos[1] = eye.y; blk.camPos[2] = eye.z;
    blk.fog[0] = 44.0f; blk.fog[1] = 130.0f;
    blk.fogColor[0] = 0.06f; blk.fogColor[1] = 0.05f; blk.fogColor[2] = 0.10f;

    static Token tokens[MAX_INSTANCES];
    uint32_t n = 0;
    for (uint32_t i = 0; i < s->pool.watermark && n < MAX_INSTANCES; i++) {
        const CremaEntity *e = &s->pool.items[i];
        if (!e->active)
            continue;
        bool hero = (e->kind == KIND_HERO);
        // A hit is a white flash on the token, which costs a lerp on a tint
        // that was being uploaded anyway.
        float f = s->flash[i] > 0.0f ? s->flash[i] / 0.28f : 0.0f;
        tokens[n].pos[0] = e->pos.x;
        tokens[n].pos[1] = e->pos.y + sinf(s->t * 2.4f + (float)i) * 0.30f;
        tokens[n].pos[2] = e->pos.z;
        tokens[n].scale = hero ? 2.6f : 2.0f + 0.35f * (float)s->tier[i];
        float br = hero ? 0.78f : 1.0f;
        float bg = hero ? 0.92f : 0.38f + 0.10f * (float)s->tier[i];
        float bb = hero ? 1.0f  : 0.34f;
        tokens[n].tint[0] = br + (1.0f - br) * f;
        tokens[n].tint[1] = bg + (1.0f - bg) * f;
        tokens[n].tint[2] = bb + (1.0f - bb) * f;
        tokens[n].yaw = e->yaw;
        n++;
    }

    static HudList hud;
    battleHud(s, &hud);
    gfxStoreFrame(&s->gfx, sceneApp(self), slot, &blk, view, tokens, n,
                  &s->fx, battleFxScratch, &hud);
}

static void battleDraw(CremaScene *self)
{
    BattleScene *s = (BattleScene *)self;
    gfxDrawTokens(sceneApp(self), &s->gfx);
    gfxDrawOverlays(sceneApp(self), &s->gfx);
}

// =============================================================================
//  GAME OVER
// =============================================================================
//
// Pushed over the battle rather than replacing it, so the tableau you lost in
// is still on the screen behind the words. It is see-through for the same
// reason the status menu is, and it is the scene that showed what this stack
// cannot yet do: the battle underneath is suspended, so the last explosion
// freezes in mid-air instead of finishing. Whether that is wrong is a question
// for the television, not for the source.

typedef struct {
    SceneBase        b;
    CremaUniformRing hudRing;
    HudList          list;
    const void      *hudUbo;
    uint32_t         hudCount;
    float            t;
} OverScene;

static void overEnter(CremaScene *self)
{
    OverScene *s = (OverScene *)self;
    CremaUniformRingCreate(&s->hudRing, CREMA_HUD_BLOCK_BYTES,
                           CREMA_FRAMES_IN_FLIGHT);
    s->t = 0.0f;
    CremaMusicSetVolume(sceneApp(self)->music, 0.30f);
    WHBLogPrintf("[poc13] defeated after %u battles and %u steps",
                 sceneApp(self)->party.battles, sceneApp(self)->party.steps);
}

static void overLeave(CremaScene *self)
{
    CremaUniformRingDestroy(&((OverScene *)self)->hudRing);
}

static void overUpdate(CremaScene *self, const CremaInput *in, float dt)
{
    OverScene *s = (OverScene *)self;
    s->t += dt;
    if (s->t > 1.0f && CremaInputPressed(in, VPAD_BUTTON_A))
        questRequest(sceneApp(self), CREMA_SCENE_GOTO, sceneApp(self)->title, true);
}

static void overBuild(CremaScene *self, uint32_t slot)
{
    OverScene *s = (OverScene *)self;
    const Party *p = &sceneApp(self)->party;
    HudList *h = &s->list;
    hudClear(h);

    float veil = s->t < 0.8f ? s->t / 0.8f * 0.72f : 0.72f;
    hudRect(h, 0.0f, 0.0f, HUD_VIRTUAL_W, HUD_VIRTUAL_H, 0.0f, 0.0f, 0.0f, veil);
    hudText(h, 424.0f, 250.0f, 52.0f, "YOU FELL", 1.0f, 0.42f, 0.36f, 1.0f);
    hudText(h, 424.0f, 340.0f, 22.0f, "LEVEL", 0.7f, 0.78f, 0.9f, 1.0f);
    hudNumber(h, 540.0f, 340.0f, 22.0f, p->level, 3, 1.0f, 1.0f, 1.0f, 1.0f);
    hudText(h, 424.0f, 376.0f, 22.0f, "BATTLES", 0.7f, 0.78f, 0.9f, 1.0f);
    hudNumber(h, 580.0f, 376.0f, 22.0f, p->battles, 4, 1.0f, 1.0f, 1.0f, 1.0f);
    hudText(h, 424.0f, 412.0f, 22.0f, "GOLD", 0.9f, 0.85f, 0.55f, 1.0f);
    hudNumber(h, 540.0f, 412.0f, 22.0f, p->gold, 5, 1.0f, 0.95f, 0.7f, 1.0f);
    hudText(h, 424.0f, 470.0f, 18.0f, "THE JOURNAL KEEPS WHAT YOU WROTE",
            0.6f, 0.68f, 0.8f, 0.9f);
    if (s->t > 1.0f && fmodf(s->t, 1.0f) < 0.62f)
        hudText(h, 486.0f, 528.0f, 24.0f, "A - TITLE", 0.9f, 0.9f, 0.9f, 1.0f);

    s->hudCount = h->count;
    s->hudUbo   = CremaUniformRingStore(&s->hudRing, slot, h->items,
                                        CREMA_HUD_BLOCK_BYTES);
}

static void overDraw(CremaScene *self)
{
    OverScene *s = (OverScene *)self;
    if (s->hudCount > 0)
        CremaHudDraw(&sceneApp(self)->hudRenderer, s->hudUbo, s->hudCount,
                     &sceneApp(self)->font, &sceneApp(self)->fontSampler);
}

// =============================================================================
//  wiring
// =============================================================================

static void sceneInit(SceneBase *sb, App *app, const char *name, bool opaque,
                      float r, float g, float b,
                      void (*enter)(CremaScene *),
                      void (*leave)(CremaScene *),
                      void (*update)(CremaScene *, const CremaInput *, float),
                      void (*build)(CremaScene *, uint32_t),
                      void (*draw)(CremaScene *))
{
    memset(&sb->base, 0, sizeof(sb->base));
    sb->app = app;
    sb->base.name   = name;
    sb->base.opaque = opaque;
    sb->base.clear[0] = r; sb->base.clear[1] = g;
    sb->base.clear[2] = b; sb->base.clear[3] = 1.0f;
    sb->base.enter  = enter;
    sb->base.leave  = leave;
    sb->base.update = update;
    sb->base.build  = build;
    sb->base.draw   = draw;
}

// What is drawn on top of every scene: the fade, and nothing else. This is the
// one thing crema_scene cannot do for a caller, because it has no idea a fade
// exists.
static void questDraw(void *user)
{
    App *a = (App *)user;
    CremaSceneDraw(&a->stack);
    if (a->blackCount > 0)
        CremaHudDraw(&a->hudRenderer, a->blackUbo, a->blackCount,
                     &a->font, &a->fontSampler);
}

static void questBuildFade(App *a, uint32_t slot)
{
    hudClear(&a->blackList);
    if (a->black > 0.0f)
        hudRect(&a->blackList, 0.0f, 0.0f, HUD_VIRTUAL_W, HUD_VIRTUAL_H,
                0.0f, 0.0f, 0.0f, a->black);
    a->blackCount = a->blackList.count;
    a->blackUbo = CremaUniformRingStore(&a->blackRing, slot, a->blackList.items,
                                        CREMA_HUD_BLOCK_BYTES);
}

static App        app;
static TitleScene titleScene;
static FieldScene fieldScene;
static StatusScene statusScene;
static BattleScene battleScene;
static OverScene   overScene;

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    if (!CremaAppInit("poc13-quest"))
        return -1;
    CremaAudioInit();
    memset(&app, 0, sizeof(app));
    app.canSave = CremaSaveInit("gx2poc");
    if (!CremaShaderInitCompiler()) {
        CremaAppShutdown();
        return -1;
    }

    // --- assets, once ------------------------------------------------------
    bool ok = false;
    CremaPak pak;
    if (CremaPakOpen(&pak, "/vol/content/assets.cpak")) {
        size_t meshBytes = 0, hullBytes = 0, fontBytes = 0;
        size_t bankBytes = 0, songBytes = 0;
        const void *meshBlob = CremaPakFind(&pak, "ship.cmesh",   &meshBytes);
        const void *hullBlob = CremaPakFind(&pak, "hull.ctex",    &hullBytes);
        const void *fontBlob = CremaPakFind(&pak, "font.ctex",    &fontBytes);
        const void *bankBlob = CremaPakFind(&pak, "audio.cbank",  &bankBytes);
        const void *songBlob = CremaPakFind(&pak, "theme.csong",  &songBytes);
        ok = meshBlob && hullBlob && fontBlob &&
             CremaMeshLoadFromMemory(&app.ship, meshBlob, meshBytes,
                                     "ship.cmesh") &&
             CremaTextureLoadFromMemory(&app.hull, hullBlob, hullBytes,
                                        "hull.ctex") &&
             CremaTextureLoadFromMemory(&app.font, fontBlob, fontBytes,
                                        "font.ctex");
        if (ok && bankBlob &&
            CremaBankLoadFromMemory(&app.bank, bankBlob, bankBytes) && songBlob)
            CremaMusicLoadFromMemory(&app.music, songBlob, songBytes, &app.bank);
        CremaPakClose(&pak);
    }
    if (!ok) {
        WHBLogPrintf("[poc13] asset load failed - is the content dir bundled?");
        CremaShaderShutdownCompiler();
        CremaAudioShutdown();
        CremaAppShutdown();
        return -1;
    }

    app.sfxBlip = CremaBankFind(&app.bank, "laser");
    app.sfxCast = CremaBankFind(&app.bank, "laser");
    app.sfxHit  = CremaBankFind(&app.bank, "boom");
    CremaAudioSetHeadroom(0.45f);

    // The number that draws the line between a scene's memory and the
    // application's. Whatever this prints, it is not a bill anything may pay
    // when the player walks into a monster.
    uint64_t shaderT0 = OSGetSystemTime();
    app.shToken  = CremaShaderCompile(VS_TOKEN, PS_TOKEN,
                                      app.ship.attribs, app.ship.attribCount);
    static const CremaAttrib GROUND_ATTRIBS[] = {
        { 0, 0, 0,                 GX2_ATTRIB_FORMAT_FLOAT_32_32_32 },
        { 1, 0, 3 * sizeof(float), GX2_ATTRIB_FORMAT_FLOAT_32_32 },
    };
    app.shGround = CremaShaderCompile(VS_GROUND, PS_GROUND, GROUND_ATTRIBS, 2);
    uint32_t shaderUs =
        (uint32_t)OSTicksToMicroseconds(OSGetSystemTime() - shaderT0);

    if (!app.shToken || !app.shGround ||
        !CremaEffectRendererCreate(&app.fxRenderer) ||
        !CremaHudRendererCreate(&app.hudRenderer)) {
        WHBLogPrintf("[poc13] shader setup failed");
        CremaShaderShutdownCompiler();
        CremaAudioShutdown();
        CremaAppShutdown();
        return -1;
    }
    WHBLogPrintf("[poc13] two shaders compiled in %u us - which is why a scene "
                 "does not own one", shaderUs);

    CremaSamplerInitTrilinear(&app.sampler, GX2_TEX_CLAMP_MODE_WRAP);
    CremaSamplerInitBilinear(&app.fontSampler, GX2_TEX_CLAMP_MODE_CLAMP);

    app.tokGlobalVs = CremaShaderVSBlockLocation(app.shToken, "Global");
    app.tokGlobalPs = CremaShaderPSBlockLocation(app.shToken, "Global");
    app.tokInst     = CremaShaderVSBlockLocation(app.shToken, "Instances");
    if (app.tokGlobalVs < 0) app.tokGlobalVs = 0;
    if (app.tokGlobalPs < 0) app.tokGlobalPs = 0;
    if (app.tokInst     < 0) app.tokInst     = 1;
    if (app.shToken->ps->samplerVarCount > 0)
        app.tokUnit = app.shToken->ps->samplerVars[0].location;

    app.grGlobalVs = CremaShaderVSBlockLocation(app.shGround, "Global");
    app.grGlobalPs = CremaShaderPSBlockLocation(app.shGround, "Global");
    if (app.grGlobalVs < 0) app.grGlobalVs = 0;
    if (app.grGlobalPs < 0) app.grGlobalPs = 0;
    if (app.shGround->ps->samplerVarCount > 0)
        app.grUnit = app.shGround->ps->samplerVars[0].location;

    // --- the scenes --------------------------------------------------------
    sceneInit(&titleScene.b,  &app, "title",  true,  0.04f, 0.05f, 0.10f,
              titleEnter,  titleLeave,  titleUpdate,  titleBuild,  titleDraw);
    sceneInit(&fieldScene.b,  &app, "field",  true,  0.62f, 0.74f, 0.86f,
              fieldEnter,  fieldLeave,  fieldUpdate,  fieldBuild,  fieldDraw);
    sceneInit(&statusScene.b, &app, "status", false, 0.0f, 0.0f, 0.0f,
              statusEnter, statusLeave, statusUpdate, statusBuild, statusDraw);
    sceneInit(&battleScene.b, &app, "battle", true,  0.06f, 0.05f, 0.10f,
              battleEnter, battleLeave, battleUpdate, battleBuild, battleDraw);
    sceneInit(&overScene.b,   &app, "over",   false, 0.0f, 0.0f, 0.0f,
              overEnter,   overLeave,   overUpdate,   overBuild,   overDraw);

    CremaSceneStackInit(&app.stack, app.stackStorage,
                        sizeof(app.stackStorage) / sizeof(app.stackStorage[0]));
    CremaUniformRingCreate(&app.blackRing, CREMA_HUD_BLOCK_BYTES,
                           CREMA_FRAMES_IN_FLIGHT);

    app.title  = &titleScene.b.base;
    app.field  = &fieldScene.b.base;
    app.status = &statusScene.b.base;
    app.battle = &battleScene.b.base;
    app.over   = &overScene.b.base;
    partyReset(&app.party);

    if (app.music)
        CremaMusicStart(app.music);

    CremaFrame frame;
    CremaFrameInit(&frame, CREMA_PACING_FENCED, 1);
    CremaFrameStats stats;
    CremaClock clock;
    CremaClockInit(&clock);
    CremaInput input;
    CremaInputInit(&input);

    // The first transition, taken through the same door as every other one.
    // No frame has been drawn yet, so there is nothing to drain — which is what
    // the NULL says.
    CremaSceneRequest(&app.stack, CREMA_SCENE_GOTO, app.title);
    CremaSceneApply(&app.stack, NULL);

    WHBLogPrintf("[poc13] L-stick walks, PLUS status, Y writes the journal. "
                 "A monster starts a battle.");

    while (CremaAppRunning()) {
        CremaClockTick(&clock);
        CremaInputPoll(&input);

        // The fade, which is this game's business and not the framework's.
        // While a request is parked the scenes are frozen anyway, so this is
        // the only thing moving on the screen.
        if (CremaScenePending(&app.stack) && app.fadeThis) {
            app.black += clock.dt / FADE_TIME;
            if (app.black > 1.0f) app.black = 1.0f;
        } else if (!CremaScenePending(&app.stack) && app.black > 0.0f) {
            app.black -= clock.dt / FADE_TIME;
            if (app.black < 0.0f) app.black = 0.0f;
        }

        CremaSceneUpdate(&app.stack, &input, clock.dt);
        CremaAudioUpdate();

        uint32_t slot = CremaFrameBegin(&frame);
        CremaSceneBuild(&app.stack, slot);
        questBuildFade(&app, slot);
        CremaFrameDrawBoth(CremaSceneClearColor(&app.stack), questDraw, &app);
        CremaFrameEnd(&frame, &stats);

        // The one place a scene may come or go — and the fade decides when.
        if (CremaScenePending(&app.stack) &&
            (!app.fadeThis || app.black >= 1.0f)) {
            CremaSceneApply(&app.stack, &frame);
            app.fadeThis = false;
        }

        if (stats.updated) {
            CremaScene *top = CremaSceneTop(&app.stack);
            WHBLogPrintf("[poc13] %.1f fps | sync %.2f ms | %s (depth %u) | "
                         "switches %u | lv %u hp %d | voices %u",
                         stats.fps, stats.drainMs,
                         top ? top->name : "(none)", app.stack.depth,
                         app.stack.switches, app.party.level, app.party.hp,
                         (unsigned)CremaAudioVoicesInUse());
        }
    }

    CremaFrameSettle(&frame);
    while (app.stack.depth > 0) {
        CremaScene *s = app.stack.items[--app.stack.depth];
        if (s->leave)
            s->leave(s);
    }
    CremaUniformRingDestroy(&app.blackRing);
    CremaMusicClose(app.music);
    CremaBankClose(&app.bank);
    CremaEffectRendererDestroy(&app.fxRenderer);
    CremaHudRendererDestroy(&app.hudRenderer);
    CremaMeshDestroy(&app.ship);
    CremaTextureDestroy(&app.hull);
    CremaTextureDestroy(&app.font);
    CremaShaderFree(app.shToken);
    CremaShaderFree(app.shGround);
    CremaShaderShutdownCompiler();
    CremaAudioShutdown();
    CremaSaveShutdown();
    CremaAppShutdown();
    return 0;
}
