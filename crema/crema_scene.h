// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan
//
// A stack of places, and the difference between a scene and a game state.
//
// PoC 12 has four states in a `switch` and works perfectly well. They share
// every resource, none of them is ever entered or left, and nothing is loaded
// or freed when one becomes another — which is fine, because a shoot-'em-up is
// one place. The moment a game has two, the question stops being "what is
// happening" and becomes "what is alive", and that is what this module is: a
// state machine is bookkeeping, a scene is a *lifetime*.
//
// Three operations, and the difference between two of them is measured rather
// than stylistic. On real hardware, in PoC 13:
//
//     push a battle over a field    0.5-0.8 ms, no GPU drain
//     rebuild the field from scratch  10.8-12.5 ms, plus a 3.6 ms drain
//
// So `push` is not a convenience for keeping the previous picture — it is the
// difference between suspending a place and paying to build it again, and the
// bill is on the order of twenty times. A game that pops back to a field it
// never left is a game that spends nothing to get there.
//
// --- what a scene owns -------------------------------------------------------
//
// Its own memory, and that is the whole point. What it must NOT own is anything
// whose creation is slow: compiling two shaders through CafeGLSL is 51 ms on
// the console, which is three frames of a scene change nobody would forgive.
// Textures, meshes, shaders and sound banks belong to the application; uniform
// rings, entity pools and whatever the place is made of belong to the scene.
// The clock draws that line, not taste.
//
// --- why a scene has two drawing callbacks -----------------------------------
//
// Not a design choice: crema_frame hands out a uniform slot once at the top of
// the frame and then draws the contents TWICE, once to the television and once
// to the GamePad. Uniforms must be written before either draw and exactly once;
// the draw itself happens twice. So `build` fills this frame's uniform slice
// and `draw` issues the GX2 calls, and a single `render()` could not be both.
//
// It follows that a scene owns its own uniform rings rather than sharing the
// application's, and that is forced too: a see-through scene means two scenes
// filling uniforms in the same frame, and one ring between them would have the
// overlay overwrite the slice the scene underneath is still being drawn from.
// Same rule crema_hud already found — whoever knows how many lists a frame has
// owns the memory for them.
//
// --- what this module deliberately does not know -----------------------------
//
// How a transition looks. PoC 13 fades through black and PoC 12's transitions
// are instant, so a request is only *parked* here and applied when the caller
// says so — which for a fade is the frame the screen is black, and for anything
// else is immediately. There is no fade timer in here and no callback for one.

#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "crema_frame.h"
#include "crema_input.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CremaScene CremaScene;

struct CremaScene {
    const char *name;             // for the log line a switch prints

    // false = whatever is below is still drawn under this one. A pause overlay
    // and a status menu are see-through; a battle and a title screen are not.
    // One bit, and it is the entire difference between an overlay and a place.
    bool        opaque;

    // The screen this scene starts from, used when it is the deepest visible
    // one. A game whose scenes all look the same fills all of them identically
    // and never thinks about it again.
    float       clear[4];

    // Called once when this scene comes onto the stack and once when it leaves.
    // This is where memory is taken and handed back — and also where a scene
    // ducks the music or reads a save file, because those have lifetimes too.
    void (*enter) (CremaScene *self);
    void (*leave) (CremaScene *self);

    // Only the top of the stack is updated. What is underneath is suspended,
    // not running quietly: a battle must not take turns while the game-over
    // screen is up, and a field must not walk you into a second monster during
    // the fade to the first one.
    void (*update)(CremaScene *self, const CremaInput *in, float dt);

    // Every visible scene, deepest first. See the note above on why these are
    // two calls and not one.
    void (*build) (CremaScene *self, uint32_t slot);
    void (*draw)  (CremaScene *self);
};

typedef enum {
    CREMA_SCENE_NONE = 0,
    CREMA_SCENE_GOTO,    // leave every scene on the stack, then enter this one
    CREMA_SCENE_PUSH,    // keep everything alive, put this one on top
    CREMA_SCENE_POP,     // leave the top one, uncover what was under it
} CremaSceneOp;

typedef struct {
    CremaScene **items;        // caller's storage, as everywhere else in Crema
    uint32_t     capacity;
    uint32_t     depth;

    CremaSceneOp op;           // a request, parked until CremaSceneApply
    CremaScene  *target;

    // What the last switch cost, in microseconds. Kept because the whole
    // argument for push over goto is these three numbers.
    uint32_t     settleUs, leaveUs, enterUs;
    uint32_t     switches;
} CremaSceneStack;

void CremaSceneStackInit(CremaSceneStack *stack, CremaScene **storage,
                         uint32_t capacity);

// Park a transition. Called from inside a scene's own update, which is the one
// place it must not happen for real: the asking scene is about to be freed and
// the frame it is in the middle of has not been submitted yet.
//
// The first request of a frame wins. A scene that asks twice in one update —
// two monsters touched in the same step — gets the first answer, not the last.
void CremaSceneRequest(CremaSceneStack *stack, CremaSceneOp op,
                       CremaScene *target);

bool CremaScenePending(const CremaSceneStack *stack);

CremaScene *CremaSceneTop(const CremaSceneStack *stack);

// Update the top scene — unless a transition is parked, in which case nothing
// is updated at all and the caller is free to run a fade over the frozen
// picture.
void CremaSceneUpdate(CremaSceneStack *stack, const CremaInput *in, float dt);

// Fill this frame's uniforms for every visible scene, deepest first.
void CremaSceneBuild(CremaSceneStack *stack, uint32_t slot);

// Draw them, in the same order. This matches CremaRenderFn, so it can be handed
// straight to CremaFrameDrawBoth — or called from inside a caller's own render
// function when there is something to draw on top of every scene, which is what
// a fade is.
void CremaSceneDraw(void *stack);

// The clear colour of the deepest visible scene. Never NULL.
const float *CremaSceneClearColor(const CremaSceneStack *stack);

// Apply the parked transition, if there is one. Call it AFTER CremaFrameEnd and
// nowhere else — two things are true there and nowhere else in the frame: the
// frame has been submitted, so nothing will reference the outgoing scene's
// uniform slices again, and the GPU can be drained, which is what makes it safe
// to hand that memory back. A ring freed while frame N-1 still reads it is the
// exact bug the ring exists to prevent, coming in through the back door.
//
// The drain happens only for the operations that free something: a push costs
// nothing at all, which is why an overlay can open instantly and a scene change
// wants a fade to hide it behind. On real hardware the drain is 3.6-5.8 ms —
// ten times what Cemu reports for the same call — and it still cost no frames,
// because the pacing leaves the CPU idle and this runs after submission.
//
// A caller that wants the transition to wait (for a fade to reach black) simply
// does not call this yet. There is no timer in here.
void CremaSceneApply(CremaSceneStack *stack, CremaFrame *frame);

#ifdef __cplusplus
}
#endif
