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
//   - the HUD list builder and the shader that draws it        [extracted]
//   - the billboard shader that draws a CremaEffect            [extracted]
//   - a per-entity velocity, which CremaEntity does not carry  [still here]
//
// --- and then it was the witness, not the writer -----------------------------
//
// This file's four game states used to be a `switch` on an `int`. On 2026-07-26
// they became four CremaScenes, and the rewrite is the whole reason `crema_scene`
// is allowed to exist: PoC 13 discovered that shape while writing a role-playing
// game, and a shape one example likes is not a shape, it is a preference. What
// makes this file the test is that its four states were written months before
// there was any such thing to fit them to.
//
// Three things were learned by fitting them, and all three are visible here:
//
//   1. `enter` and `leave` are not only about memory. The pause ducks the music
//      on the way in and lifts it on the way out, which used to be two lines
//      sitting at opposite ends of a `switch` with nothing saying they were a
//      pair. Now they cannot drift apart.
//
//   2. The readout and the announcement are different lists. The score, the
//      record and the lives belong to the round; "PAUSED" and "GAME OVER"
//      belong to the thing on top of it. They used to be one `buildHud` with a
//      `switch` at the bottom, and splitting them was not tidying — an overlay
//      owns its own uniform slice because both lists are in flight at once.
//
//   3. What keeps moving under an overlay is the game's call, not the
//      framework's. A suspended scene is suspended completely, so the pause
//      freezes the explosions for free — and the game-over screen, which wants
//      them to finish burning, ticks them itself from on top. That is one line
//      in `overUpdate`, and it is *better* than the `if (state != PAUSED)` it
//      replaced, which had to name the state it was not.
//
// What did NOT have to change is the part worth reporting: `crema_scene.h` was
// not edited to make this file fit.

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
// The one part of this game that is supposed to outlive the process, and — now
// that a round is a scene with a lifetime — also the one part that outlives the
// round. It belongs to the application, beside the mesh and the sound bank,
// which is exactly the line the scene rewrite drew: the score is the round's,
// the record is the game's.
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

// --- what the game adds to a scene -------------------------------------------

typedef struct Shmup Shmup;

typedef struct {
    CremaScene  base;
    Shmup      *app;
} SceneBase;

static inline Shmup *sceneApp(CremaScene *self)
{
    return ((SceneBase *)self)->app;
}

// --- everything that outlives a round ----------------------------------------

struct Shmup {
    CremaShader *shShip;
    CremaMesh    ship;
    GX2Texture   hull, font;
    GX2Sampler   sampler;

    CremaEffectRenderer fxRenderer;
    CremaHudRenderer    hudRenderer;

    CremaBank    bank;
    CremaMusic  *music;
    const CremaInstrument *laser, *boom;

    int32_t  gVs, gPs, instLoc;
    uint32_t hullUnit;

    // The camera never moves. A shoot-'em-up is a 3D scene photographed from
    // one place forever, and every frame it does not recompute is a frame that
    // cannot be wrong.
    Mat4  viewProj;
    float camPitch;
    Vec3  lightDir;

    Record record;
    bool   canSave;

    CremaSceneStack stack;
    CremaScene     *stackStorage[3];
    CremaScene *title, *play, *pause, *over;
};

static uint32_t rngState = 0x1234567u;

static float randUnit(void)
{
    rngState = rngState * 1664525u + 1013904223u;
    return (float)((rngState >> 8) & 0xFFFF) / 65535.0f;
}

// =============================================================================
//  PLAY — the round, and the only scene here that owns anything
// =============================================================================

typedef struct {
    SceneBase       b;

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

    uint32_t score;
    int      lives;
    float    invuln;         // after a death: visible, blinking, unhittable
    float    fireCooldown;
    float    waveTimer;
    uint32_t wave;
    float    t;

    // The uniform rings, which used to be created once in main and are now the
    // round's own — because the pause and the game-over screen fill their own
    // lists in the same frame this one is being drawn from.
    CremaUniformRing globals, instances, fxViews, effects, hudRing;
    const void *globalUbo, *instUbo, *fxViewUbo, *fxUbo, *hudUbo;
    size_t      instBytes;
    uint32_t    instCount, fxCount, hudCount;
} PlayScene;

static int slotOf(const PlayScene *g, const CremaEntity *e)
{
    return (int)(e - g->pool.items);
}

static CremaEntity *spawn(PlayScene *g, uint32_t kind, Vec3 pos, Vec3 vel,
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

static void burst(PlayScene *g, Vec3 at, float size, float r, float gr, float b)
{
    CremaEffectSpawn(&g->fx, at, 0.45f, size, size * 3.0f, r, gr, b, 1.0f);
    for (int i = 0; i < 4; i++) {
        Vec3 p = { at.x + (randUnit() - 0.5f) * size * 2.0f, at.y,
                   at.z + (randUnit() - 0.5f) * size * 2.0f };
        CremaEffectSpawn(&g->fx, p, 0.30f + randUnit() * 0.3f,
                         size * 0.6f, size * 1.8f, r, gr * 0.7f, b * 0.5f, 0.9f);
    }
}

// Written once per game over, and timed because a file on the SD card is the
// slowest thing this program does on purpose. PoC 13 later found out how slow:
// 42 ms on real hardware and 209 ms once, against the 1.1 ms Cemu reports. This
// game gets away with it only because it saves at a game over, when the picture
// has already stopped — see the note in the README.
static void persistRecord(Shmup *app)
{
    uint64_t before = OSGetSystemTime();
    bool ok = CremaSaveWrite(RECORD_FILE, RECORD_VERSION, &app->record,
                             sizeof(app->record));
    uint32_t us = (uint32_t)OSTicksToMicroseconds(OSGetSystemTime() - before);
    WHBLogPrintf("[poc12] record %s: best %u, %u games, %u us",
                 ok ? "saved" : "NOT saved", app->record.best,
                 app->record.games, us);
}

static CremaEntity *findPlayer(PlayScene *g)
{
    for (uint32_t i = 0; i < g->pool.watermark; i++)
        if (g->pool.items[i].active && g->pool.items[i].kind == KIND_PLAYER)
            return &g->pool.items[i];
    return NULL;
}

// A wave is a row of ships entering together with a shared sway. Difficulty is
// one number growing: they come sooner, faster, and shoot more often.
static uint32_t countKind(const PlayScene *g, uint32_t kind)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < g->pool.watermark; i++)
        if (g->pool.items[i].active && g->pool.items[i].kind == kind)
            n++;
    return n;
}

static void launchWave(PlayScene *g)
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

// What used to be `resetRound`, and it did not have to be rewritten — it had
// always been an `enter`, it just had nowhere to be called from that said so.
static void playEnter(CremaScene *self)
{
    PlayScene *g = (PlayScene *)self;

    CremaUniformRingCreate(&g->globals,   sizeof(GlobalBlock),
                           CREMA_FRAMES_IN_FLIGHT);
    g->instBytes = sizeof(float) * 4 * 2 * MAX_INSTANCES;
    CremaUniformRingCreate(&g->instances, g->instBytes, CREMA_FRAMES_IN_FLIGHT);
    CremaUniformRingCreate(&g->fxViews,   sizeof(CremaEffectView),
                           CREMA_FRAMES_IN_FLIGHT);
    CremaUniformRingCreate(&g->effects,   CREMA_EFFECT_BLOCK_BYTES,
                           CREMA_FRAMES_IN_FLIGHT);
    CremaUniformRingCreate(&g->hudRing,   CREMA_HUD_BLOCK_BYTES,
                           CREMA_FRAMES_IN_FLIGHT);

    CremaEntityPoolInit(&g->pool, g->storage, MAX_ENTITIES);
    CremaEffectPoolInit(&g->fx, g->fxStorage, MAX_EFFECTS);
    memset(g->vel, 0, sizeof(g->vel));
    memset(g->ttl, 0, sizeof(g->ttl));
    g->score = 0;
    g->lives = START_LIVES;
    g->invuln = INVULN_TIME;
    g->fireCooldown = 0.0f;
    g->waveTimer = 0.6f;
    g->wave = 0;
    g->t = 0.0f;

    Vec3 start = { 0.0f, 0.0f, FIELD_NEAR - 4.0f };
    Vec3 still = { 0.0f, 0.0f, 0.0f };
    spawn(g, KIND_PLAYER, start, still, PLAYER_RADIUS, 0.0f);

    CremaMusicSetVolume(sceneApp(self)->music, 1.0f);
}

static void playLeave(CremaScene *self)
{
    PlayScene *g = (PlayScene *)self;
    CremaUniformRingDestroy(&g->globals);
    CremaUniformRingDestroy(&g->instances);
    CremaUniformRingDestroy(&g->fxViews);
    CremaUniformRingDestroy(&g->effects);
    CremaUniformRingDestroy(&g->hudRing);

    // Emptied, and not because anything reads it afterwards: the once-a-second
    // log line does, and on the title screen it was still reporting the entities
    // and the score of the round that had just ended. A leftover in a struct is
    // harmless; a leftover in a log is a lie, and this project has found more
    // than one real bug by believing that line.
    CremaEntityClear(&g->pool);
    g->score = 0;
    g->lives = 0;
    g->instCount = g->fxCount = g->hudCount = 0;
}

static void playUpdate(CremaScene *self, const CremaInput *in, float dt)
{
    PlayScene *g = (PlayScene *)self;
    Shmup *app = sceneApp(self);
    g->t += dt;

    if (CremaInputPressed(in, VPAD_BUTTON_PLUS)) {
        CremaSceneRequest(&app->stack, CREMA_SCENE_PUSH, app->pause);
        return;
    }

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
                if (app->laser)
                    CremaAudioPlay(&app->laser->sound, 0.55f,
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
            e->pos.x += sinf(g->t * 1.7f + (float)i) * 9.0f * dt;
            e->roll   = sinf(g->t * 1.7f + (float)i) * 0.4f;
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
            if (app->boom)
                CremaAudioPlay(&app->boom->sound, 0.75f,
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
            if (app->boom)
                CremaAudioPlay(&app->boom->sound, 1.0f, 0.72f);
            CremaEntityDespawn(&g->pool, threat);
            g->lives--;
            if (g->lives <= 0) {
                CremaEntityDespawn(&g->pool, player);
                if (g->score > app->record.best)
                    app->record.best = g->score;
                app->record.games++;
                // Here and nowhere else. A game that saved every time the score
                // changed would write to the card a hundred times a minute for
                // one number that only matters when the round is over.
                persistRecord(app);
                // A push and not a goto: the round stays alive underneath, so
                // the wreck you died in is what you read your score over — and
                // nothing had to be copied out of it first.
                CremaSceneRequest(&app->stack, CREMA_SCENE_PUSH, app->over);
            } else {
                g->invuln = INVULN_TIME;
                player->pos.x = 0.0f;
                player->pos.z = FIELD_NEAR - 4.0f;
            }
            break;
        }
    }

    CremaEffectUpdate(&g->fx, dt);

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

static uint32_t packInstances(const PlayScene *g, float (*out)[4], uint32_t max)
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

// The readout, and nothing else. What used to follow it — a `switch` painting
// "PAUSED" or "GAME OVER" — now belongs to whichever scene is on top, which is
// the same split the uniform rings had to make.
static void playHud(const PlayScene *g, HudList *h)
{
    const Shmup *app = g->b.app;
    hudClear(h);
    hudText(h, 40.0f, 34.0f, 22.0f, "SCORE", 0.62f, 0.78f, 0.95f, 0.9f);
    hudNumber(h, 140.0f, 34.0f, 22.0f, g->score, 6, 1.0f, 1.0f, 1.0f, 1.0f);

    hudText(h, 40.0f, 66.0f, 16.0f, "BEST", 0.45f, 0.55f, 0.70f, 0.8f);
    hudNumber(h, 112.0f, 66.0f, 16.0f, app->record.best, 6,
              0.65f, 0.75f, 0.9f, 0.8f);

    for (int i = 0; i < g->lives; i++)
        hudRect(h, 1180.0f - (float)i * 26.0f, 38.0f, 16.0f, 16.0f,
                0.75f, 0.90f, 1.0f, 0.95f);
}

static void playBuild(CremaScene *self, uint32_t slot)
{
    PlayScene *g = (PlayScene *)self;
    Shmup *app = sceneApp(self);

    GlobalBlock blk;
    blk.viewProj = app->viewProj;
    blk.lightDir[0] = app->lightDir.x; blk.lightDir[1] = app->lightDir.y;
    blk.lightDir[2] = app->lightDir.z; blk.lightDir[3] = 0.0f;
    blk.time[0] = g->t;
    blk.time[1] = blk.time[2] = blk.time[3] = 0.0f;
    g->globalUbo = CremaUniformRingStore(&g->globals, slot, &blk, sizeof(blk));

    // The billboards ask for three numbers of their own rather than reading
    // this game's Global block, which is what lets the same renderer serve
    // a shooter whose camera never moves and a flight demo whose camera
    // never stops. The camera here being fixed, its basis is a constant.
    CremaEffectView fxView;
    fxView.viewProj = app->viewProj;
    fxView.camRight[0] = 1.0f; fxView.camRight[1] = 0.0f;
    fxView.camRight[2] = 0.0f; fxView.camRight[3] = 0.0f;
    fxView.camUp[0] = 0.0f;    fxView.camUp[1] = cosf(app->camPitch);
    fxView.camUp[2] = -sinf(app->camPitch); fxView.camUp[3] = 0.0f;
    g->fxViewUbo = CremaUniformRingStore(&g->fxViews, slot, &fxView,
                                         sizeof(fxView));

    static float instData[MAX_INSTANCES * 2][4];
    static float fxData[MAX_EFFECTS * 2][4];
    static HudList hud;

    g->instCount = packInstances(g, instData, MAX_INSTANCES);
    g->instUbo   = CremaUniformRingStore(&g->instances, slot, instData,
                                         g->instBytes);
    g->fxCount   = CremaEffectPack(&g->fx, fxData, MAX_EFFECTS);
    g->fxUbo     = CremaUniformRingStore(&g->effects, slot, fxData,
                                         CREMA_EFFECT_BLOCK_BYTES);

    playHud(g, &hud);
    g->hudCount = hud.count;
    g->hudUbo   = CremaUniformRingStore(&g->hudRing, slot, hud.items,
                                        CREMA_HUD_BLOCK_BYTES);
}

static void playDraw(CremaScene *self)
{
    PlayScene *g = (PlayScene *)self;
    const Shmup *app = sceneApp(self);

    if (g->instCount > 0) {
        CremaDepthSet(true, true);
        GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, TRUE);
        CremaShaderBind(app->shShip);
        GX2SetVertexUniformBlock(app->gVs, sizeof(GlobalBlock), g->globalUbo);
        GX2SetPixelUniformBlock(app->gPs, sizeof(GlobalBlock), g->globalUbo);
        GX2SetVertexUniformBlock(app->instLoc, g->instBytes, g->instUbo);
        GX2SetPixelTexture(&app->hull, app->hullUnit);
        GX2SetPixelSampler(&app->sampler, app->hullUnit);
        CremaMeshDraw(&app->ship, g->instCount);
    }

    CremaEffectDraw(&app->fxRenderer, g->fxViewUbo, g->fxUbo, g->fxCount);
    if (g->hudCount > 0)
        CremaHudDraw(&app->hudRenderer, g->hudUbo, g->hudCount,
                     &app->font, &app->sampler);
}

// =============================================================================
//  The three scenes that are only words
// =============================================================================
//
// A title screen, a pause and a game over, and between them they own one
// uniform ring each and nothing else. Entering one is microseconds — which is
// the measurement that says an overlay may be pushed freely and a place may
// not.

typedef struct {
    SceneBase        b;
    CremaUniformRing hudRing;
    HudList          list;
    const void      *hudUbo;
    uint32_t         hudCount;
    float            t;
    PlayScene       *round;      // only the game-over screen uses it
} WordsScene;

static void wordsEnter(CremaScene *self)
{
    WordsScene *s = (WordsScene *)self;
    CremaUniformRingCreate(&s->hudRing, CREMA_HUD_BLOCK_BYTES,
                           CREMA_FRAMES_IN_FLIGHT);
    s->t = 0.0f;
}

static void wordsLeave(CremaScene *self)
{
    CremaUniformRingDestroy(&((WordsScene *)self)->hudRing);
}

static void wordsStore(WordsScene *s, uint32_t slot)
{
    s->hudCount = s->list.count;
    s->hudUbo   = CremaUniformRingStore(&s->hudRing, slot, s->list.items,
                                        CREMA_HUD_BLOCK_BYTES);
}

static void wordsDraw(CremaScene *self)
{
    WordsScene *s = (WordsScene *)self;
    const Shmup *app = sceneApp(self);
    if (s->hudCount > 0)
        CremaHudDraw(&app->hudRenderer, s->hudUbo, s->hudCount,
                     &app->font, &app->sampler);
}

// --- title -------------------------------------------------------------------

static void titleEnter(CremaScene *self)
{
    wordsEnter(self);
    CremaMusicSetVolume(sceneApp(self)->music, 0.7f);
}

static void titleUpdate(CremaScene *self, const CremaInput *in, float dt)
{
    WordsScene *s = (WordsScene *)self;
    s->t += dt;
    if (CremaInputPressed(in, VPAD_BUTTON_A))
        CremaSceneRequest(&sceneApp(self)->stack, CREMA_SCENE_GOTO,
                          sceneApp(self)->play);
}

static void titleBuild(CremaScene *self, uint32_t slot)
{
    WordsScene *s = (WordsScene *)self;
    const Shmup *app = sceneApp(self);
    HudList *h = &s->list;
    hudClear(h);
    hudText(h, 430.0f, 250.0f, 54.0f, "CREMA", 1.0f, 0.85f, 0.45f, 1.0f);
    hudText(h, 430.0f, 312.0f, 30.0f, "SQUADRON", 0.85f, 0.92f, 1.0f, 0.95f);
    // blinking, because a static prompt reads as a label and a blinking one
    // reads as an invitation
    if (fmodf(s->t, 1.0f) < 0.6f)
        hudText(h, 470.0f, 430.0f, 24.0f, "PRESS A", 0.9f, 0.9f, 0.9f, 1.0f);
    hudText(h, 470.0f, 490.0f, 18.0f, "BEST", 0.5f, 0.6f, 0.75f, 0.85f);
    hudNumber(h, 550.0f, 490.0f, 18.0f, app->record.best, 6,
              0.7f, 0.8f, 0.95f, 0.85f);
    // The proof that the save worked, and the reason it is on the title
    // screen: a number that is not zero on a fresh boot came from a file.
    hudText(h, 500.0f, 520.0f, 18.0f, "GAMES", 0.5f, 0.6f, 0.75f, 0.85f);
    hudNumber(h, 580.0f, 520.0f, 18.0f, app->record.games, 4,
              0.7f, 0.8f, 0.95f, 0.85f);
    wordsStore(s, slot);
}

// --- pause -------------------------------------------------------------------
//
// The two lines that used to be at opposite ends of a `switch`, now a pair.

static void pauseEnter(CremaScene *self)
{
    wordsEnter(self);
    CremaMusicSetVolume(sceneApp(self)->music, 0.35f);
}

static void pauseLeave(CremaScene *self)
{
    wordsLeave(self);
    CremaMusicSetVolume(sceneApp(self)->music, 1.0f);
}

static void pauseUpdate(CremaScene *self, const CremaInput *in, float dt)
{
    WordsScene *s = (WordsScene *)self;
    s->t += dt;
    // Nothing else happens here, and that is the feature: the round underneath
    // is suspended, so the explosions hold still without anybody saying so.
    if (CremaInputPressed(in, VPAD_BUTTON_PLUS))
        CremaSceneRequest(&sceneApp(self)->stack, CREMA_SCENE_POP, NULL);
}

static void pauseBuild(CremaScene *self, uint32_t slot)
{
    WordsScene *s = (WordsScene *)self;
    HudList *h = &s->list;
    hudClear(h);
    hudRect(h, 0.0f, 300.0f, 1280.0f, 120.0f, 0.0f, 0.0f, 0.0f, 0.55f);
    hudText(h, 530.0f, 330.0f, 40.0f, "PAUSED", 1.0f, 1.0f, 1.0f, 1.0f);
    wordsStore(s, slot);
}

// --- game over ---------------------------------------------------------------

static void overUpdate(CremaScene *self, const CremaInput *in, float dt)
{
    WordsScene *s = (WordsScene *)self;
    s->t += dt;

    // The one place this rewrite needed a decision rather than a move. A
    // suspended scene is suspended completely, so the last explosion would
    // freeze in the air — and this screen is the one place that wants it to
    // finish. So the overlay ticks it, which is a line the game is entitled to
    // write because the game owns both scenes. It replaces
    // `if (state != STATE_PAUSED) CremaEffectUpdate(...)`, which had to name a
    // state it was not in order to say the same thing.
    if (s->round)
        CremaEffectUpdate(&s->round->fx, dt);

    // A second before the prompt appears. Nothing else moves, so the last
    // explosion finishes burning over a scene frozen where it went wrong —
    // which is the right thing to be looking at while you read your score.
    if (s->t > 1.0f && CremaInputPressed(in, VPAD_BUTTON_A))
        CremaSceneRequest(&sceneApp(self)->stack, CREMA_SCENE_GOTO,
                          sceneApp(self)->title);
}

static void overBuild(CremaScene *self, uint32_t slot)
{
    WordsScene *s = (WordsScene *)self;
    HudList *h = &s->list;
    hudClear(h);
    hudRect(h, 0.0f, 260.0f, 1280.0f, 210.0f, 0.0f, 0.0f, 0.0f, 0.6f);
    hudText(h, 470.0f, 290.0f, 44.0f, "GAME OVER", 1.0f, 0.45f, 0.38f, 1.0f);
    hudText(h, 480.0f, 356.0f, 24.0f, "SCORE", 0.8f, 0.8f, 0.9f, 1.0f);
    hudNumber(h, 610.0f, 356.0f, 24.0f, s->round ? s->round->score : 0, 6,
              1.0f, 1.0f, 1.0f, 1.0f);
    if (s->t > 1.0f && fmodf(s->t, 1.0f) < 0.6f)
        hudText(h, 455.0f, 412.0f, 22.0f, "PRESS A", 0.9f, 0.9f, 0.9f, 1.0f);
    wordsStore(s, slot);
}

// =============================================================================
//  wiring
// =============================================================================

static void sceneInit(SceneBase *sb, Shmup *app, const char *name, bool opaque,
                      const float clear[4],
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
    memcpy(sb->base.clear, clear, sizeof(sb->base.clear));
    sb->base.enter  = enter;
    sb->base.leave  = leave;
    sb->base.update = update;
    sb->base.build  = build;
    sb->base.draw   = draw;
}

static Shmup      app;
static PlayScene  playScene;
static WordsScene titleScene, pauseScene, overScene;

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    if (!CremaAppInit("poc12-shmup"))
        return -1;
    CremaAudioInit();                  // before the assets: it ends the menu music
    memset(&app, 0, sizeof(app));
    // Not fatal. A game that cannot find a card is a game with no record, and
    // that is exactly how this PoC behaved before crema_save existed.
    app.canSave = CremaSaveInit("gx2poc");
    if (!CremaShaderInitCompiler()) {
        CremaAppShutdown();
        return -1;
    }

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
             CremaMeshLoadFromMemory(&app.ship, meshBlob, meshBytes, "ship.cmesh") &&
             CremaTextureLoadFromMemory(&app.hull, hullBlob, hullBytes, "hull.ctex") &&
             CremaTextureLoadFromMemory(&app.font, fontBlob, fontBytes, "font.ctex");
        // sound is not worth failing over: a silent game is still a game
        if (ok && bankBlob &&
            CremaBankLoadFromMemory(&app.bank, bankBlob, bankBytes) && songBlob)
            CremaMusicLoadFromMemory(&app.music, songBlob, songBytes, &app.bank);
        CremaPakClose(&pak);
    }
    if (!ok) {
        WHBLogPrintf("[poc12] asset load failed - is the content dir bundled?");
        CremaShaderShutdownCompiler();
        CremaAudioShutdown();
        CremaAppShutdown();
        return -1;
    }

    app.laser = CremaBankFind(&app.bank, "laser");
    app.boom  = CremaBankFind(&app.bank, "boom");

    // The mix has no idea how loud it is unless somebody decides: PoC 11 found
    // the same mix clipping at nearly twice full scale with everything at
    // unity, and this one has more shots on screen, not fewer.
    CremaAudioSetHeadroom(0.40f);

    // The only shader this game had to write. The other two came with the
    // framework, which is what an earlier half hour of work bought.
    app.shShip = CremaShaderCompile(VS_SHIP, PS_SHIP,
                                    app.ship.attribs, app.ship.attribCount);
    if (!app.shShip || !CremaEffectRendererCreate(&app.fxRenderer) ||
        !CremaHudRendererCreate(&app.hudRenderer)) {
        WHBLogPrintf("[poc12] shader compile failed");
        CremaShaderShutdownCompiler();
        CremaAudioShutdown();
        CremaAppShutdown();
        return -1;
    }

    CremaSamplerInitTrilinear(&app.sampler, GX2_TEX_CLAMP_MODE_WRAP);

    app.gVs     = CremaShaderVSBlockLocation(app.shShip, "Global");
    app.gPs     = CremaShaderPSBlockLocation(app.shShip, "Global");
    app.instLoc = CremaShaderVSBlockLocation(app.shShip, "Instances");
    if (app.gVs     < 0) app.gVs     = 0;
    if (app.gPs     < 0) app.gPs     = 0;
    if (app.instLoc < 0) app.instLoc = 1;
    if (app.shShip->ps->samplerVarCount > 0)
        app.hullUnit = app.shShip->ps->samplerVars[0].location;

    Vec3 camPos = { 0.0f, 42.0f, 34.0f };
    app.camPitch = -0.82f;
    Mat4 proj = mat4_perspective(58.0f * 3.14159265f / 180.0f,
                                 16.0f / 9.0f, 0.5f, 260.0f);
    Mat4 viewMat = mat4_mul(mat4_rotate_x(-app.camPitch),
                            mat4_translate(-camPos.x, -camPos.y, -camPos.z));
    app.viewProj = mat4_mul(proj, viewMat);
    Vec3 lightRaw = { -0.35f, -0.80f, -0.48f };
    app.lightDir = vec3_normalize(lightRaw);

    if (app.canSave) {
        Record rec;
        uint64_t before = OSGetSystemTime();
        size_t got = CremaSaveRead(RECORD_FILE, RECORD_VERSION,
                                   &rec, sizeof(rec));
        uint32_t us = (uint32_t)OSTicksToMicroseconds(OSGetSystemTime() - before);
        if (got == sizeof(rec)) {
            app.record = rec;
            WHBLogPrintf("[poc12] record loaded from %s: best %u after %u "
                         "games, %u us", CremaSaveDir(), rec.best, rec.games, us);
        } else {
            WHBLogPrintf("[poc12] no record yet in %s (%u us)",
                         CremaSaveDir(), us);
        }
    }

    static const float SPACE[4] = { 0.03f, 0.05f, 0.09f, 1.0f };

    sceneInit(&titleScene.b, &app, "title", true, SPACE,
              titleEnter, wordsLeave, titleUpdate, titleBuild, wordsDraw);
    sceneInit(&playScene.b,  &app, "play",  true, SPACE,
              playEnter, playLeave, playUpdate, playBuild, playDraw);
    sceneInit(&pauseScene.b, &app, "pause", false, SPACE,
              pauseEnter, pauseLeave, pauseUpdate, pauseBuild, wordsDraw);
    sceneInit(&overScene.b,  &app, "over",  false, SPACE,
              wordsEnter, wordsLeave, overUpdate, overBuild, wordsDraw);
    // The game-over screen reads the round's score from underneath. It did not
    // need anything from crema_scene to do it: the stack guarantees the round is
    // still alive, and this file already had a typed pointer to it.
    overScene.round = &playScene;

    CremaSceneStackInit(&app.stack, app.stackStorage,
                        sizeof(app.stackStorage) / sizeof(app.stackStorage[0]));
    app.title = &titleScene.b.base;
    app.play  = &playScene.b.base;
    app.pause = &pauseScene.b.base;
    app.over  = &overScene.b.base;

    if (app.music)
        CremaMusicStart(app.music);

    CremaFrame frame;
    CremaFrameInit(&frame, CREMA_PACING_FENCED, 1);
    WHBLogPrintf("[poc12] L-stick moves, A fires, PLUS pauses. %d entities, "
                 "%d effects.", MAX_ENTITIES, MAX_EFFECTS);

    CremaFrameStats stats;
    CremaClock clock;
    CremaClockInit(&clock);
    CremaInput input;
    CremaInputInit(&input);

    CremaSceneRequest(&app.stack, CREMA_SCENE_GOTO, app.title);
    CremaSceneApply(&app.stack, NULL);   // nothing drawn yet: nothing to drain

    while (CremaAppRunning()) {
        CremaClockTick(&clock);
        CremaInputPoll(&input);

        CremaSceneUpdate(&app.stack, &input, clock.dt);
        CremaAudioUpdate();

        uint32_t slot = CremaFrameBegin(&frame);
        CremaSceneBuild(&app.stack, slot);
        CremaFrameDrawBoth(CremaSceneClearColor(&app.stack), CremaSceneDraw,
                           &app.stack);
        CremaFrameEnd(&frame, &stats);

        // This game's transitions are instant, so there is no reason to wait.
        CremaSceneApply(&app.stack, &frame);

        if (stats.updated) {
            CremaScene *top = CremaSceneTop(&app.stack);
            WHBLogPrintf("[poc12] %.1f fps | sync %.2f ms | %s (depth %u) | "
                         "entities %u | fx %u | score %u | voices %u",
                         stats.fps, stats.drainMs,
                         top ? top->name : "(none)", app.stack.depth,
                         CremaEntityActiveCount(&playScene.pool),
                         playScene.fxCount, playScene.score,
                         (unsigned)CremaAudioVoicesInUse());
        }
    }

    CremaFrameSettle(&frame);
    while (app.stack.depth > 0) {
        CremaScene *s = app.stack.items[--app.stack.depth];
        if (s->leave)
            s->leave(s);
    }
    CremaMusicClose(app.music);
    CremaBankClose(&app.bank);
    CremaEffectRendererDestroy(&app.fxRenderer);
    CremaHudRendererDestroy(&app.hudRenderer);
    CremaMeshDestroy(&app.ship);
    CremaTextureDestroy(&app.hull);
    CremaTextureDestroy(&app.font);
    CremaShaderFree(app.shShip);
    CremaShaderShutdownCompiler();
    CremaAudioShutdown();
    CremaSaveShutdown();
    CremaAppShutdown();
    return 0;
}
