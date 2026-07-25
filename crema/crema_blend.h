// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan
//
// Blending and depth state — everything Crema drew until now was opaque, and
// an explosion is not.
//
// The rule that goes with it, because GX2 will not enforce it: draw all the
// opaque geometry first, then the transparent passes with depth WRITES off
// and testing still on. Transparent surfaces need to be occluded by the world,
// but must not occlude each other — writing depth makes two overlapping
// billboards punch holes in one another depending on submission order.

#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CREMA_BLEND_OPAQUE = 0,   // no blending: the default for world geometry
    CREMA_BLEND_ALPHA,        // src.a over dst: decals, UI, smoke
    CREMA_BLEND_ADDITIVE,     // light added to what is there: fire, tracers
} CremaBlendMode;

void CremaBlendSet(CremaBlendMode mode);

// Depth testing and depth writing are separate switches on purpose: the
// transparent pass wants test on, write off.
void CremaDepthSet(bool test, bool write);

#ifdef __cplusplus
}
#endif
