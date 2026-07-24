// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan
//
// Frame pacing, TV+GamePad presentation, and the double-buffered uniform ring
// that makes pipelining safe.
//
// The engine-grade path (CREMA_PACING_FENCED) never calls GX2DrawDone: it lets
// frame N submit while frame N-1 is still on the GPU, and only waits for the
// timestamp of frame N-2 — the frame whose uniform slice is about to be
// overwritten. Measured on real hardware that is worth +2.2% over a DrawDone
// per frame, and with vsync on it leaves the CPU 100% idle (0.00 ms of sync
// wait per frame) for game logic.
//
// Two hardware notes:
//   - WHBGfxBeginRender waits for EVERY flip. With two frames in flight that
//     pins you to 59.94 Hz even with a swap interval of 0 — which is exactly
//     what a game wants, and exactly what a benchmark does not. Uncapped
//     pipelining therefore skips it and throttles on GX2GetSwapStatus instead
//     (CREMA_PACING_FENCED_UNCAPPED).
//   - a uniform block written by the CPU while the GPU still reads it tears.
//     One slice per in-flight frame, guarded by the fence, is the fix.

#pragma once
#include <coreinit/time.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "crema_app.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CREMA_FRAMES_IN_FLIGHT 2

typedef enum {
    CREMA_PACING_SYNC = 0,        // GX2DrawDone every frame: simple, CPU blocks
    CREMA_PACING_FENCED,          // 2 frames in flight, presented at vsync
    CREMA_PACING_FENCED_UNCAPPED, // 2 frames in flight, no flip wait: benchmarks
} CremaPacing;

typedef struct {
    CremaPacing pacing;
    uint32_t    index;                            // frames since init
    uint32_t    slot;                             // index % CREMA_FRAMES_IN_FLIGHT
    OSTime      fence[CREMA_FRAMES_IN_FLIGHT];    // last submit per slot
    uint64_t    syncWaitTicks;                    // CPU ticks blocked this frame
} CremaFrame;

// swapInterval: 1 = present at vsync (a game), 0 = as fast as the GPU retires.
void CremaFrameInit(CremaFrame *frame, CremaPacing pacing, uint32_t swapInterval);

// Wait for the frame whose slot we are about to reuse, then open the frame.
// Returns the slot index — write this frame's uniforms into that slice.
uint32_t CremaFrameBegin(CremaFrame *frame);

// Present: swap + flush + record the fence (or GX2DrawDone in SYNC pacing),
// then close the one-second stats window. `stats` may be NULL.
void CremaFrameEnd(CremaFrame *frame, CremaFrameStats *stats);

// Drain the GPU and forget the fences — teardown, or before switching pacing.
void CremaFrameSettle(CremaFrame *frame);

// Render the same content to the TV and the GamePad, clearing both first.
// (For different content per screen, call the WHBGfx begin/finish pairs
// directly — this is just the common case.)
typedef void (*CremaRenderFn)(void *user);
void CremaFrameDrawBoth(const float clearColor[4], CremaRenderFn draw, void *user);

// --- uniform ring ------------------------------------------------------------
// One byteswapped uniform slice per in-flight frame, indexed by the frame slot.

typedef struct {
    uint8_t *base;
    uint32_t sliceSize;    // padded to the GX2 uniform-block alignment
    uint32_t sliceCount;
} CremaUniformRing;

bool  CremaUniformRingCreate(CremaUniformRing *ring, size_t bytesPerSlice,
                             uint32_t sliceCount);
void *CremaUniformRingSlice(const CremaUniformRing *ring, uint32_t slot);

// Byteswap + invalidate `bytes` into the slice; returns the slice pointer,
// ready to hand to GX2SetVertexUniformBlock / GX2SetPixelUniformBlock.
void *CremaUniformRingStore(const CremaUniformRing *ring, uint32_t slot,
                            const void *src, size_t bytes);
void  CremaUniformRingDestroy(CremaUniformRing *ring);

#ifdef __cplusplus
}
#endif
