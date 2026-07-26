// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan
//
// GX2 PoC 12 — a small shoot-'em-up, and the first thing in this project that
// is a *game* rather than a demonstration: it has a title screen, a pause, a
// score, three lives and an ending.
//
// It was written from an empty file on purpose. Everything already extracted
// into `crema/` is reused without a second thought — the mesh, the texture, the
// audio, the pools, the frame pacing — but nothing was copied out of PoC 11,
// because a copy proves nothing. The rule this project runs on is that a module
// earns its place in `crema/` when a *second* example needs it, and a second
// example that was cloned from the first is not a witness. So the interesting
// output of this PoC is not the game. It is the list of things I had to reach
// back into PoC 11 for, each of which is now a candidate for extraction:
//
//   - the HUD list builder and the shader that draws it
//   - the billboard shader that draws a CremaEffect
//   - a per-entity velocity, which CremaEntity does not carry
//
// The game itself is deliberately small and unpolished. Its job is to make the
// scaffolding every game needs — states, pause, restart, a score that survives
// a death — exist somewhere other than in a plan.

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
#include "crema_input.h"
#include "crema_matrix.h"
#include "crema_mesh.h"
#include "crema_music.h"
#include "crema_pak.h"
#include "crema_save.h"
#include "crema_shader.h"
#include "crema_texture.h"

// The first extraction this PoC caused. It lived in PoC 11 until a second
// example wanted it unchanged, which is the whole test — and it passed without
// a line being edited, because it never knew what a Wii U was to begin with.
#include "crema_hud.h"

// --- the playfield -----------------------------------------------------------
//
// A vertical shooter on a horizontal plane: the camera looks down and slightly
// forward, the player sits near the bottom and everything hostile comes from
// -Z. Nothing here moves in Y, which is what makes a 3D engine draw a 2D game.

#define FIELD_X       35.0f     // half-width the player may reach
#define FIELD_NEAR    16.0f     // player's limit toward the camera
#define FIELD_FAR    -34.0f     // where enemies enter
#define DESPAWN_Z     28.0f     // past the player: gone

#define MAX_ENTITIES  192
#define MAX_EFFECTS    64
#define MAX_INSTANCES 192

enum {
    KIND_PLAYER = 1,
    KIND_ENEMY,
    KIND_SHOT,          // the player's
    KIND_BOMB,          // theirs
};

#define PLAYER_SPEED    34.0f
#define PLAYER_RADIUS    1.6f
#define ENEMY_RADIUS     1.8f
#define SHOT_RADIUS      0.7f
#define SHOT_SPEED      70.0f
#define BOMB_SPEED      26.0f
#define FIRE_INTERVAL    0.14f
#define INVULN_TIME      1.6f
#define START_LIVES      3

// A ceiling on how many of them may exist at once, and it is not a difficulty
// setting — it is what keeps the pool from filling.
//
// Waves arrive on a timer that shortens as the game goes on, and enemies take
// about five seconds to cross the field, so without a cap the two rates settle
// wherever they happen to meet: the log showed the entity count climbing 5, 14,
// 24, 36, 52 and never coming back down. Nothing looks wrong on screen until
// the pool runs out — and then `spawn` returns NULL for whoever asks next,
// which sooner or later is the player's own gun. A game that quietly stops
// firing is a worse bug than a game that is too hard, and the same number fixes
// both: difficulty now grows in speed and rate of fire rather than by piling up
// ships nobody can shoot down fast enough.
#define MAX_LIVE_ENEMIES 22

// --- shaders -----------------------------------------------------------------

#define GLOBAL_UBO_DECL \
    "layout(binding = 0) uniform Global {\n" \
    "    mat4 uViewProj;\n" \
    "    vec4 uLightDir;\n"  \
    "    vec4 uTime;\n"      \
    "};\n"

// One instanced draw for every ship on screen — player, enemies, and the shots,
// which are the same hull scaled down until it reads as a bolt. A shoot-'em-up
// is one mesh and a lot of numbers.
static const char *VS_SHIP =
    "#version 450\n"
    "layout(location = 0) in vec3 aPosition;\n"
    "layout(location = 1) in vec3 aNormal;\n"
    "layout(location = 2) in vec2 aUV;\n"
    GLOBAL_UBO_DECL
    "layout(binding = 1) uniform Instances {\n"
    "    vec4 uInst[384];\n"   // 192 x { xyz = position, w = ±scale }, { rgb, roll }
    "};\n"
    "layout(location = 0) out vec2 vUV;\n"
    "layout(location = 1) out vec3 vNormal;\n"
    "layout(location = 2) out vec3 vTint;\n"
    "void main()\n"
    "{\n"
    "    vec4 slot = uInst[gl_InstanceID * 2];\n"
    "    vec4 look = uInst[gl_InstanceID * 2 + 1];\n"
    // Nothing in this game faces anywhere except up or down the screen, so a
    // whole float for a yaw would be a whole float spent on two values. The
    // sign of the scale carries it instead — negative means turned around —
    // which leaves the fourth component free for the roll, and a ship that
    // leans into its movement is most of what makes it feel flown.
    "    float scale = abs(slot.w);\n"
    "    float face = slot.w < 0.0 ? -1.0 : 1.0;\n"
    "    float cr = cos(look.w), sr = sin(look.w);\n"
    "    vec3 p = vec3(cr * aPosition.x - sr * aPosition.y,\n"
    "                  sr * aPosition.x + cr * aPosition.y, aPosition.z);\n"
    "    vec3 n = vec3(cr * aNormal.x - sr * aNormal.y,\n"
    "                  sr * aNormal.x + cr * aNormal.y, aNormal.z);\n"
    "    p = vec3(p.x * face, p.y, p.z * face);\n"
    "    n = vec3(n.x * face, n.y, n.z * face);\n"
    "    gl_Position = uViewProj * vec4(p * scale + slot.xyz, 1.0);\n"
    "    vUV = aUV;\n"
    "    vNormal = n;\n"
    "    vTint = look.rgb;\n"
    "}\n";

static const char *PS_SHIP =
    "#version 450\n"
    "layout(location = 0) in vec2 vUV;\n"
    "layout(location = 1) in vec3 vNormal;\n"
    "layout(location = 2) in vec3 vTint;\n"
    GLOBAL_UBO_DECL
    "layout(binding = 0) uniform sampler2D uHull;\n"
    "layout(location = 0) out vec4 oColor;\n"
    "void main()\n"
    "{\n"
    "    vec3 base = texture(uHull, vUV).rgb * vTint;\n"
    "    float diff = max(dot(normalize(vNormal), -uLightDir.xyz), 0.0);\n"
    "    oColor = vec4(base * (0.35 + 0.75 * diff), 1.0);\n"
    "}\n";

typedef struct {
    Mat4  viewProj;
    float lightDir[4];
    float time[4];
} GlobalBlock;

// --- the record --------------------------------------------------------------
//
// The one part of this game that is supposed to outlive the process. It was in
// RAM until now, which meant the "BEST" line on the HUD was a lie the moment you
// pressed HOME — it said "best" and meant "best since you turned it on".
//
// Two fields rather than one on purpose. A high score alone would be indistinct
// from a single word written to a file, and the thing worth proving is that a
// *struct* goes out and comes back with its parts still in the right order; a
// play counter is also the honest way to see, from the title screen, that the
// last session really happened.
#define RECORD_FILE    "record.dat"
#define RECORD_VERSION 1

typedef struct {
    uint32_t best;
    uint32_t games;
} Record;

// --- the game ----------------------------------------------------------------

enum { STATE_TITLE, STATE_PLAY, STATE_PAUSED, STATE_OVER };

typedef struct {
    CremaEntityPool pool;
    CremaEntity     storage[MAX_ENTITIES];

    // CremaEntity carries a position and an orientation but no velocity, which
    // is right — a framework that decided how things move would have decided
    // what kind of game this is. So the motion lives here, in a parallel array
    // indexed by pool slot. Worth writing down: this is the second example to
    // need exactly this, and the first thing on the extraction list.
    Vec3            vel[MAX_ENTITIES];
    float           ttl[MAX_ENTITIES];   // shots die of old age, not collision

    CremaEffectPool fx;
    CremaEffect     fxStorage[MAX_EFFECTS];

    int      state;
    uint32_t score, best, games;
    int      lives;
    float    invuln;         // after a death: visible, blinking, unhittable
    float    fireCooldown;
    float    waveTimer;
    uint32_t wave;
    float    stateTime;
} Game;

static uint32_t rngState = 0x1234567u;

static float randUnit(void)
{
    rngState = rngState * 1664525u + 1013904223u;
    return (float)((rngState >> 8) & 0xFFFF) / 65535.0f;
}

static int slotOf(const Game *g, const CremaEntity *e)
{
    return (int)(e - g->pool.items);
}

static CremaEntity *spawn(Game *g, uint32_t kind, Vec3 pos, Vec3 vel,
                          float radius, float ttl)
{
    CremaEntity *e = CremaEntitySpawn(&g->pool, kind);
    if (!e)
        return NULL;
    e->pos    = pos;
    e->radius = radius;
    // Slots are recycled, so anything the previous occupant left behind is
    // inherited. A bullet born in a dead enemy's slot would fly out banked.
    e->yaw = e->pitch = e->roll = 0.0f;
    int i = slotOf(g, e);
    g->vel[i] = vel;
    g->ttl[i] = ttl;
    return e;
}

static void burst(Game *g, Vec3 at, float size, float r, float gr, float b)
{
    CremaEffectSpawn(&g->fx, at, 0.45f, size, size * 3.0f, r, gr, b, 1.0f);
    for (int i = 0; i < 4; i++) {
        Vec3 p = { at.x + (randUnit() - 0.5f) * size * 2.0f, at.y,
                   at.z + (randUnit() - 0.5f) * size * 2.0f };
        CremaEffectSpawn(&g->fx, p, 0.30f + randUnit() * 0.3f,
                         size * 0.6f, size * 1.8f, r, gr * 0.7f, b * 0.5f, 0.9f);
    }
}

static void resetRound(Game *g)
{
    CremaEntityClear(&g->pool);
    memset(g->vel, 0, sizeof(g->vel));
    memset(g->ttl, 0, sizeof(g->ttl));
    g->score = 0;
    g->lives = START_LIVES;
    g->invuln = INVULN_TIME;
    g->fireCooldown = 0.0f;
    g->waveTimer = 0.6f;
    g->wave = 0;

    Vec3 start = { 0.0f, 0.0f, FIELD_NEAR - 4.0f };
    Vec3 still = { 0.0f, 0.0f, 0.0f };
    spawn(g, KIND_PLAYER, start, still, PLAYER_RADIUS, 0.0f);
}

// Written once per game over, and timed because a file on the SD card is the
// slowest thing this program does on purpose and it happens while a frame is in
// flight. If it costs milliseconds it belongs on another thread; if it costs
// microseconds the simplest possible code is also the right one, and the only
// way to know which is to print the number.
static void persistRecord(Game *g)
{
    Record rec;
    rec.best  = g->best;
    rec.games = g->games;

    uint64_t before = OSGetSystemTime();
    bool ok = CremaSaveWrite(RECORD_FILE, RECORD_VERSION, &rec, sizeof(rec));
    uint32_t us = (uint32_t)OSTicksToMicroseconds(OSGetSystemTime() - before);
    WHBLogPrintf("[poc12] record %s: best %u, %u games, %u us",
                 ok ? "saved" : "NOT saved", rec.best, rec.games, us);
}

static CremaEntity *findPlayer(Game *g)
{
    for (uint32_t i = 0; i < g->pool.watermark; i++)
        if (g->pool.items[i].active && g->pool.items[i].kind == KIND_PLAYER)
            return &g->pool.items[i];
    return NULL;
}

// A wave is a row of ships entering together with a shared sway. Difficulty is
// one number growing: they come sooner, faster, and shoot more often.
static uint32_t countKind(const Game *g, uint32_t kind)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < g->pool.watermark; i++)
        if (g->pool.items[i].active && g->pool.items[i].kind == kind)
            n++;
    return n;
}

static void launchWave(Game *g)
{
    g->wave++;
    float hard = 1.0f + (float)g->wave * 0.06f;
    if (hard > 2.4f) hard = 2.4f;

    int count = 3 + (int)(g->wave % 4);
    float spread = FIELD_X * 1.5f / (float)(count + 1);
    float baseX = -spread * (float)(count - 1) * 0.5f + (randUnit() - 0.5f) * 8.0f;

    for (int i = 0; i < count; i++) {
        Vec3 p = { baseX + spread * (float)i, 0.0f, FIELD_FAR - randUnit() * 6.0f };
        Vec3 v = { 0.0f, 0.0f, (7.0f + randUnit() * 4.0f) * hard };
        spawn(g, KIND_ENEMY, p, v, ENEMY_RADIUS, 0.0f);
    }
    g->waveTimer = (1.9f - (float)g->wave * 0.02f) / hard;
    if (g->waveTimer < 0.5f) g->waveTimer = 0.5f;
}

typedef struct {
    const CremaInstrument *laser, *boom;
} Sfx;

static void updatePlay(Game *g, const CremaInput *in, const Sfx *sfx, float dt)
{
    CremaEntity *player = findPlayer(g);

    if (g->invuln > 0.0f)
        g->invuln -= dt;

    // --- the player -----------------------------------------------------
    if (player) {
        player->pos.x += in->leftX * PLAYER_SPEED * dt;
        player->pos.z -= in->leftY * PLAYER_SPEED * dt;
        if (player->pos.x >  FIELD_X) player->pos.x =  FIELD_X;
        if (player->pos.x < -FIELD_X) player->pos.x = -FIELD_X;
        if (player->pos.z >  FIELD_NEAR) player->pos.z =  FIELD_NEAR;
        if (player->pos.z <  0.0f)       player->pos.z =  0.0f;
        // the hull leans into the movement, which is the whole animation budget
        player->roll = -in->leftX * 0.5f;

        g->fireCooldown -= dt;
        if (CremaInputHeld(in, VPAD_BUTTON_A) && g->fireCooldown <= 0.0f) {
            Vec3 muzzle = { player->pos.x, 0.0f, player->pos.z - 2.0f };
            Vec3 v = { 0.0f, 0.0f, -SHOT_SPEED };
            if (spawn(g, KIND_SHOT, muzzle, v, SHOT_RADIUS, 1.4f)) {
                g->fireCooldown = FIRE_INTERVAL;
                if (sfx->laser)
                    CremaAudioPlay(&sfx->laser->sound, 0.55f,
                                   1.05f + randUnit() * 0.10f);
            }
        }
    }

    // --- everything that moves ------------------------------------------
    for (uint32_t i = 0; i < g->pool.watermark; i++) {
        CremaEntity *e = &g->pool.items[i];
        if (!e->active || e->kind == KIND_PLAYER)
            continue;

        e->pos.x += g->vel[i].x * dt;
        e->pos.z += g->vel[i].z * dt;

        if (e->kind == KIND_ENEMY) {
            e->pos.x += sinf(g->stateTime * 1.7f + (float)i) * 9.0f * dt;
            e->roll   = sinf(g->stateTime * 1.7f + (float)i) * 0.4f;
            // shooting is a die roll per second, not a timer per enemy: one
            // number, and it scales with the wave without any bookkeeping
            if (player && randUnit() < 0.35f * dt * (1.0f + g->wave * 0.05f)) {
                Vec3 p = { e->pos.x, 0.0f, e->pos.z + 2.0f };
                Vec3 v = { 0.0f, 0.0f, BOMB_SPEED };
                spawn(g, KIND_BOMB, p, v, SHOT_RADIUS, 4.0f);
            }
        }

        if (g->ttl[i] > 0.0f) {
            g->ttl[i] -= dt;
            if (g->ttl[i] <= 0.0f) {
                CremaEntityDespawn(&g->pool, e);
                continue;
            }
        }
        if (e->pos.z > DESPAWN_Z || e->pos.z < FIELD_FAR - 20.0f)
            CremaEntityDespawn(&g->pool, e);
    }

    // --- who hit whom ---------------------------------------------------
    for (uint32_t a = 0; a < g->pool.watermark; a++) {
        CremaEntity *shot = &g->pool.items[a];
        if (!shot->active || shot->kind != KIND_SHOT)
            continue;
        for (uint32_t b = 0; b < g->pool.watermark; b++) {
            CremaEntity *foe = &g->pool.items[b];
            if (!foe->active || foe->kind != KIND_ENEMY)
                continue;
            if (!CremaSphereHitsSphere(shot->pos, shot->radius,
                                       foe->pos, foe->radius))
                continue;
            burst(g, foe->pos, 2.4f, 1.0f, 0.62f, 0.20f);
            if (sfx->boom)
                CremaAudioPlay(&sfx->boom->sound, 0.75f,
                               1.15f + randUnit() * 0.2f);
            CremaEntityDespawn(&g->pool, foe);
            CremaEntityDespawn(&g->pool, shot);
            g->score += 100;
            break;
        }
    }

    if (player && g->invuln <= 0.0f) {
        for (uint32_t b = 0; b < g->pool.watermark; b++) {
            CremaEntity *threat = &g->pool.items[b];
            if (!threat->active)
                continue;
            if (threat->kind != KIND_ENEMY && threat->kind != KIND_BOMB)
                continue;
            if (!CremaSphereHitsSphere(player->pos, player->radius,
                                       threat->pos, threat->radius))
                continue;

            burst(g, player->pos, 3.4f, 0.55f, 0.75f, 1.0f);
            if (sfx->boom)
                CremaAudioPlay(&sfx->boom->sound, 1.0f, 0.72f);
            CremaEntityDespawn(&g->pool, threat);
            g->lives--;
            if (g->lives <= 0) {
                CremaEntityDespawn(&g->pool, player);
                if (g->score > g->best)
                    g->best = g->score;
                g->games++;
                // Here and nowhere else. A game that saved every time the score
                // changed would write to the card a hundred times a minute for
                // one number that only matters when the round is over.
                persistRecord(g);
                g->state = STATE_OVER;
                g->stateTime = 0.0f;
            } else {
                g->invuln = INVULN_TIME;
                player->pos.x = 0.0f;
                player->pos.z = FIELD_NEAR - 4.0f;
            }
            break;
        }
    }

    g->waveTimer -= dt;
    if (g->waveTimer <= 0.0f) {
        if (countKind(g, KIND_ENEMY) < MAX_LIVE_ENEMIES) {
            launchWave(g);
        } else {
            // The field is full: wait a beat and ask again, rather than skip
            // the wave. The player is falling behind, and the game noticing
            // that is the difference between pressure and a pile-up.
            g->waveTimer = 0.35f;
        }
    }
}

// --- rendering ---------------------------------------------------------------

typedef struct {
    const CremaShader *shShip;
    const CremaMesh   *mesh;
    const GX2Texture  *hull, *font;
    const GX2Sampler  *sampler;
    uint32_t hullUnit;
    int32_t  gVs, gPs, instLoc;
    const void *globalUbo, *instUbo, *fxViewUbo, *fxUbo, *hudUbo;
    size_t   instBytes;
    uint32_t instCount, fxCount, hudCount;

    // The two renderers that used to be spelled out here. What is left in this
    // file is the one shader this game actually invented.
    const CremaEffectRenderer *fxRenderer;
    const CremaHudRenderer    *hudRenderer;
} View;

static void drawWorld(void *user)
{
    const View *v = (const View *)user;

    if (v->instCount > 0) {
        CremaDepthSet(true, true);
        GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, TRUE);
        CremaShaderBind(v->shShip);
        GX2SetVertexUniformBlock(v->gVs, sizeof(GlobalBlock), v->globalUbo);
        GX2SetPixelUniformBlock(v->gPs, sizeof(GlobalBlock), v->globalUbo);
        GX2SetVertexUniformBlock(v->instLoc, v->instBytes, v->instUbo);
        GX2SetPixelTexture(v->hull, v->hullUnit);
        GX2SetPixelSampler(v->sampler, v->hullUnit);
        CremaMeshDraw(v->mesh, v->instCount);
    }

    CremaEffectDraw(v->fxRenderer, v->fxViewUbo, v->fxUbo, v->fxCount);
    CremaHudDraw(v->hudRenderer, v->hudUbo, v->hudCount, v->font, v->sampler);
}

static uint32_t packInstances(const Game *g, float (*out)[4], uint32_t max)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < g->pool.watermark && n < max; i++) {
        const CremaEntity *e = &g->pool.items[i];
        if (!e->active)
            continue;

        // scale carries the facing in its sign: negative is turned around
        float scale = 1.0f, r = 1.0f, gr = 1.0f, b = 1.0f;
        switch (e->kind) {
        case KIND_PLAYER:
            // blinking while invulnerable: the one piece of feedback a player
            // needs after dying is whether they can die again yet
            if (g->invuln > 0.0f && fmodf(g->invuln, 0.20f) < 0.10f)
                continue;
            scale = 1.0f; r = 0.75f; gr = 0.90f; b = 1.0f;
            break;
        case KIND_ENEMY:
            scale = -0.95f; r = 1.0f; gr = 0.45f; b = 0.38f;
            break;
        case KIND_SHOT:
            scale = 0.35f; r = 0.60f; gr = 1.0f; b = 0.85f;
            break;
        case KIND_BOMB:
            scale = -0.30f; r = 1.0f; gr = 0.85f; b = 0.35f;
            break;
        default:
            continue;
        }

        out[n * 2][0] = e->pos.x;
        out[n * 2][1] = e->pos.y;
        out[n * 2][2] = e->pos.z;
        out[n * 2][3] = scale;
        out[n * 2 + 1][0] = r;
        out[n * 2 + 1][1] = gr;
        out[n * 2 + 1][2] = b;
        out[n * 2 + 1][3] = e->roll;
        n++;
    }
    return n;
}

static void buildHud(const Game *g, HudList *hud)
{
    hudClear(hud);
    hudText(hud, 40.0f, 34.0f, 22.0f, "SCORE", 0.62f, 0.78f, 0.95f, 0.9f);
    hudNumber(hud, 140.0f, 34.0f, 22.0f, g->score, 6, 1.0f, 1.0f, 1.0f, 1.0f);

    hudText(hud, 40.0f, 66.0f, 16.0f, "BEST", 0.45f, 0.55f, 0.70f, 0.8f);
    hudNumber(hud, 112.0f, 66.0f, 16.0f, g->best, 6, 0.65f, 0.75f, 0.9f, 0.8f);

    for (int i = 0; i < g->lives; i++)
        hudRect(hud, 1180.0f - (float)i * 26.0f, 38.0f, 16.0f, 16.0f,
                0.75f, 0.90f, 1.0f, 0.95f);

    switch (g->state) {
    case STATE_TITLE:
        hudText(hud, 430.0f, 250.0f, 54.0f, "CREMA", 1.0f, 0.85f, 0.45f, 1.0f);
        hudText(hud, 430.0f, 312.0f, 30.0f, "SQUADRON", 0.85f, 0.92f, 1.0f, 0.95f);
        // blinking, because a static prompt reads as a label and a blinking one
        // reads as an invitation
        if (fmodf(g->stateTime, 1.0f) < 0.6f)
            hudText(hud, 470.0f, 430.0f, 24.0f, "PRESS A", 0.9f, 0.9f, 0.9f, 1.0f);
        // The proof that the save worked, and the reason it is on the title
        // screen: a number that is not zero on a fresh boot came from a file.
        hudText(hud, 500.0f, 520.0f, 18.0f, "GAMES", 0.5f, 0.6f, 0.75f, 0.85f);
        hudNumber(hud, 580.0f, 520.0f, 18.0f, g->games, 4,
                  0.7f, 0.8f, 0.95f, 0.85f);
        break;
    case STATE_PAUSED:
        hudRect(hud, 0.0f, 300.0f, 1280.0f, 120.0f, 0.0f, 0.0f, 0.0f, 0.55f);
        hudText(hud, 530.0f, 330.0f, 40.0f, "PAUSED", 1.0f, 1.0f, 1.0f, 1.0f);
        break;
    case STATE_OVER:
        hudRect(hud, 0.0f, 260.0f, 1280.0f, 210.0f, 0.0f, 0.0f, 0.0f, 0.6f);
        hudText(hud, 470.0f, 290.0f, 44.0f, "GAME OVER", 1.0f, 0.45f, 0.38f, 1.0f);
        hudText(hud, 480.0f, 356.0f, 24.0f, "SCORE", 0.8f, 0.8f, 0.9f, 1.0f);
        hudNumber(hud, 610.0f, 356.0f, 24.0f, g->score, 6, 1.0f, 1.0f, 1.0f, 1.0f);
        if (g->stateTime > 1.0f && fmodf(g->stateTime, 1.0f) < 0.6f)
            hudText(hud, 455.0f, 412.0f, 22.0f, "PRESS A", 0.9f, 0.9f, 0.9f, 1.0f);
        break;
    default:
        break;
    }
}

// --- main --------------------------------------------------------------------

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    if (!CremaAppInit("poc12-shmup"))
        return -1;
    CremaAudioInit();                  // before the assets: it ends the menu music
    // Not fatal. A game that cannot find a card is a game with no record, and
    // that is exactly how this PoC behaved yesterday.
    bool canSave = CremaSaveInit("gx2poc");
    if (!CremaShaderInitCompiler()) {
        CremaAppShutdown();
        return -1;
    }

    CremaMesh   ship;
    GX2Texture  hull, font;
    CremaBank   bank;
    CremaMusic *music = NULL;
    memset(&bank, 0, sizeof(bank));
    bool ok = false;

    CremaPak pak;
    if (CremaPakOpen(&pak, "/vol/content/assets.cpak")) {
        size_t meshBytes = 0, hullBytes = 0, fontBytes = 0;
        size_t bankBytes = 0, songBytes = 0;
        const void *meshBlob = CremaPakFind(&pak, "ship.cmesh", &meshBytes);
        const void *hullBlob = CremaPakFind(&pak, "hull.ctex",  &hullBytes);
        const void *fontBlob = CremaPakFind(&pak, "font.ctex",  &fontBytes);
        const void *bankBlob = CremaPakFind(&pak, "audio.cbank", &bankBytes);
        const void *songBlob = CremaPakFind(&pak, "theme.csong", &songBytes);
        ok = meshBlob && hullBlob && fontBlob &&
             CremaMeshLoadFromMemory(&ship, meshBlob, meshBytes, "ship.cmesh") &&
             CremaTextureLoadFromMemory(&hull, hullBlob, hullBytes, "hull.ctex") &&
             CremaTextureLoadFromMemory(&font, fontBlob, fontBytes, "font.ctex");
        // sound is not worth failing over: a silent game is still a game
        if (ok && bankBlob && CremaBankLoadFromMemory(&bank, bankBlob, bankBytes)
            && songBlob)
            CremaMusicLoadFromMemory(&music, songBlob, songBytes, &bank);
        CremaPakClose(&pak);
    }
    if (!ok) {
        WHBLogPrintf("[poc12] asset load failed - is the content dir bundled?");
        CremaShaderShutdownCompiler();
        CremaAudioShutdown();
        CremaAppShutdown();
        return -1;
    }

    Sfx sfx;
    sfx.laser = CremaBankFind(&bank, "laser");
    sfx.boom  = CremaBankFind(&bank, "boom");

    // The mix has no idea how loud it is unless somebody decides: PoC 11 found
    // the same mix clipping at nearly twice full scale with everything at
    // unity, and this one has more shots on screen, not fewer.
    CremaAudioSetHeadroom(0.40f);

    // The only shader this game had to write. The other two came with the
    // framework, which is what the previous half hour of work bought.
    CremaShader *shShip = CremaShaderCompile(VS_SHIP, PS_SHIP,
                                             ship.attribs, ship.attribCount);
    CremaEffectRenderer fxRenderer;
    CremaHudRenderer    hudRenderer;
    if (!shShip || !CremaEffectRendererCreate(&fxRenderer) ||
        !CremaHudRendererCreate(&hudRenderer)) {
        WHBLogPrintf("[poc12] shader compile failed");
        CremaShaderShutdownCompiler();
        CremaAudioShutdown();
        CremaAppShutdown();
        return -1;
    }

    GX2Sampler sampler;
    CremaSamplerInitTrilinear(&sampler, GX2_TEX_CLAMP_MODE_WRAP);

    View view;
    memset(&view, 0, sizeof(view));
    view.shShip = shShip;
    view.mesh = &ship;      view.hull = &hull;  view.font = &font;
    view.sampler = &sampler;
    view.fxRenderer  = &fxRenderer;
    view.hudRenderer = &hudRenderer;
    view.gVs     = CremaShaderVSBlockLocation(shShip, "Global");
    view.gPs     = CremaShaderPSBlockLocation(shShip, "Global");
    view.instLoc = CremaShaderVSBlockLocation(shShip, "Instances");
    if (view.gVs < 0)     view.gVs = 0;
    if (view.gPs < 0)     view.gPs = 0;
    if (view.instLoc < 0) view.instLoc = 1;
    if (shShip->ps->samplerVarCount > 0)
        view.hullUnit = shShip->ps->samplerVars[0].location;

    const size_t INST_BYTES = sizeof(float) * 4 * 2 * MAX_INSTANCES;
    CremaUniformRing globals, instances, fxViews, effects, hudRing;
    CremaUniformRingCreate(&globals,   sizeof(GlobalBlock), CREMA_FRAMES_IN_FLIGHT);
    CremaUniformRingCreate(&instances, INST_BYTES, CREMA_FRAMES_IN_FLIGHT);
    CremaUniformRingCreate(&fxViews,   sizeof(CremaEffectView),
                           CREMA_FRAMES_IN_FLIGHT);
    CremaUniformRingCreate(&effects,   CREMA_EFFECT_BLOCK_BYTES,
                           CREMA_FRAMES_IN_FLIGHT);
    CremaUniformRingCreate(&hudRing,   CREMA_HUD_BLOCK_BYTES,
                           CREMA_FRAMES_IN_FLIGHT);
    view.instBytes = INST_BYTES;

    static Game game;
    memset(&game, 0, sizeof(game));
    CremaEntityPoolInit(&game.pool, game.storage, MAX_ENTITIES);
    CremaEffectPoolInit(&game.fx, game.fxStorage, MAX_EFFECTS);
    game.state = STATE_TITLE;

    if (canSave) {
        Record rec;
        uint64_t before = OSGetSystemTime();
        size_t got = CremaSaveRead(RECORD_FILE, RECORD_VERSION,
                                   &rec, sizeof(rec));
        uint32_t us = (uint32_t)OSTicksToMicroseconds(OSGetSystemTime() - before);
        if (got == sizeof(rec)) {
            game.best  = rec.best;
            game.games = rec.games;
            WHBLogPrintf("[poc12] record loaded from %s: best %u after %u "
                         "games, %u us", CremaSaveDir(), rec.best, rec.games, us);
        } else {
            WHBLogPrintf("[poc12] no record yet in %s (%u us)",
                         CremaSaveDir(), us);
        }
    }

    // The camera never moves. A shoot-'em-up is a 3D scene photographed from
    // one place forever, and every frame it does not recompute is a frame that
    // cannot be wrong.
    Vec3 camPos = { 0.0f, 42.0f, 34.0f };
    float camPitch = -0.82f;
    Mat4 proj = mat4_perspective(58.0f * 3.14159265f / 180.0f,
                                 16.0f / 9.0f, 0.5f, 260.0f);
    Mat4 viewMat = mat4_mul(mat4_rotate_x(-camPitch),
                            mat4_translate(-camPos.x, -camPos.y, -camPos.z));
    Mat4 viewProj = mat4_mul(proj, viewMat);
    Vec3 lightRaw = { -0.35f, -0.80f, -0.48f };
    Vec3 lightDir = vec3_normalize(lightRaw);

    static float instData[MAX_INSTANCES * 2][4];
    static float fxData[MAX_EFFECTS * 2][4];
    static HudList hud;

    if (music)
        CremaMusicStart(music);

    CremaFrame frame;
    CremaFrameInit(&frame, CREMA_PACING_FENCED, 1);
    WHBLogPrintf("[poc12] L-stick moves, A fires, PLUS pauses. %d entities, "
                 "%d effects.", MAX_ENTITIES, MAX_EFFECTS);

    CremaFrameStats stats;
    CremaClock clock;
    CremaClockInit(&clock);
    CremaInput input;
    CremaInputInit(&input);

    static const float SPACE[4] = { 0.03f, 0.05f, 0.09f, 1.0f };

    while (CremaAppRunning()) {
        CremaClockTick(&clock);
        float dt = clock.dt;
        CremaInputPoll(&input);
        game.stateTime += dt;

        // The state machine, which is the point of this PoC as much as the
        // shooting is. Four states and the transitions between them are what
        // separates a demo that runs from a game that can be finished, lost and
        // started again — and none of it existed anywhere in Crema before now.
        switch (game.state) {
        case STATE_TITLE:
            if (CremaInputPressed(&input, VPAD_BUTTON_A)) {
                resetRound(&game);
                game.state = STATE_PLAY;
                game.stateTime = 0.0f;
            }
            break;
        case STATE_PLAY:
            if (CremaInputPressed(&input, VPAD_BUTTON_PLUS)) {
                game.state = STATE_PAUSED;
                game.stateTime = 0.0f;
                CremaMusicSetVolume(music, 0.35f);
            } else {
                updatePlay(&game, &input, &sfx, dt);
            }
            break;
        case STATE_PAUSED:
            if (CremaInputPressed(&input, VPAD_BUTTON_PLUS)) {
                game.state = STATE_PLAY;
                game.stateTime = 0.0f;
                CremaMusicSetVolume(music, 1.0f);
            }
            break;
        case STATE_OVER:
            // A second before the prompt appears. Nothing updates in this
            // state except the effects, so the last explosion finishes burning
            // over a scene frozen where it went wrong — which is the right
            // thing to be looking at while you read your score.
            if (game.stateTime > 1.0f &&
                CremaInputPressed(&input, VPAD_BUTTON_A)) {
                CremaEntityClear(&game.pool);
                game.state = STATE_TITLE;
                game.stateTime = 0.0f;
            }
            break;
        }

        if (game.state != STATE_PAUSED)
            CremaEffectUpdate(&game.fx, dt);

        CremaAudioUpdate();

        uint32_t slot = CremaFrameBegin(&frame);

        GlobalBlock blk;
        blk.viewProj = viewProj;
        blk.lightDir[0] = lightDir.x; blk.lightDir[1] = lightDir.y;
        blk.lightDir[2] = lightDir.z; blk.lightDir[3] = 0.0f;
        blk.time[0] = clock.elapsed;
        blk.time[1] = blk.time[2] = blk.time[3] = 0.0f;
        view.globalUbo = CremaUniformRingStore(&globals, slot, &blk, sizeof(blk));

        // The billboards ask for three numbers of their own rather than reading
        // this game's Global block, which is what lets the same renderer serve
        // a shooter whose camera never moves and a flight demo whose camera
        // never stops. The camera here being fixed, its basis is a constant.
        CremaEffectView fxView;
        fxView.viewProj = viewProj;
        fxView.camRight[0] = 1.0f; fxView.camRight[1] = 0.0f;
        fxView.camRight[2] = 0.0f; fxView.camRight[3] = 0.0f;
        fxView.camUp[0] = 0.0f;    fxView.camUp[1] = cosf(camPitch);
        fxView.camUp[2] = -sinf(camPitch); fxView.camUp[3] = 0.0f;
        view.fxViewUbo = CremaUniformRingStore(&fxViews, slot, &fxView,
                                               sizeof(fxView));

        view.instCount = packInstances(&game, instData, MAX_INSTANCES);
        view.instUbo   = CremaUniformRingStore(&instances, slot, instData,
                                               INST_BYTES);
        view.fxCount   = CremaEffectPack(&game.fx, fxData, MAX_EFFECTS);
        view.fxUbo     = CremaUniformRingStore(&effects, slot, fxData,
                                               CREMA_EFFECT_BLOCK_BYTES);

        buildHud(&game, &hud);
        view.hudCount = hud.count;
        view.hudUbo   = CremaUniformRingStore(&hudRing, slot, hud.items,
                                              CREMA_HUD_BLOCK_BYTES);

        CremaFrameDrawBoth(SPACE, drawWorld, &view);
        CremaFrameEnd(&frame, &stats);

        if (stats.updated)
            WHBLogPrintf("[poc12] %.1f fps | sync %.2f ms | state %d | "
                         "entities %u | fx %u | score %u | voices %u",
                         stats.fps, stats.drainMs, game.state,
                         CremaEntityActiveCount(&game.pool), view.fxCount,
                         game.score, (unsigned)CremaAudioVoicesInUse());
    }

    CremaFrameSettle(&frame);
    CremaMusicClose(music);
    CremaBankClose(&bank);
    CremaUniformRingDestroy(&globals);
    CremaUniformRingDestroy(&instances);
    CremaUniformRingDestroy(&fxViews);
    CremaUniformRingDestroy(&effects);
    CremaUniformRingDestroy(&hudRing);
    CremaEffectRendererDestroy(&fxRenderer);
    CremaHudRendererDestroy(&hudRenderer);
    CremaMeshDestroy(&ship);
    CremaTextureDestroy(&hull);
    CremaTextureDestroy(&font);
    CremaShaderFree(shShip);
    CremaShaderShutdownCompiler();
    CremaAudioShutdown();
    CremaSaveShutdown();
    CremaAppShutdown();
    return 0;
}
