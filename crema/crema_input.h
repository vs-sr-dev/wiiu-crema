// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan
//
// GamePad input: one poll per frame producing a snapshot with dead-zoned
// sticks and edge-triggered buttons.
//
// The edges are the part worth having. `held` answers "is the trigger down",
// which is what you want for a throttle; firing a weapon needs "did it go down
// *this frame*", and every example that tried to do that with `hold` alone
// ended up firing sixty times a second.

#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <vpad/input.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float    leftX, leftY;      // -1..1, dead zone removed and rescaled
    float    rightX, rightY;
    uint32_t held;              // VPAD_BUTTON_* mask, down right now
    uint32_t pressed;           // went down between the last poll and this one
    uint32_t released;          // came up between the last poll and this one
    bool     connected;         // false when the pad did not answer this frame
    float    deadzone;          // default 0.12
    uint32_t prevHeld;          // internal
} CremaInput;

void CremaInputInit(CremaInput *in);
void CremaInputPoll(CremaInput *in);

static inline bool CremaInputHeld(const CremaInput *in, uint32_t buttons)
{
    return (in->held & buttons) != 0;
}

static inline bool CremaInputPressed(const CremaInput *in, uint32_t buttons)
{
    return (in->pressed & buttons) != 0;
}

static inline bool CremaInputReleased(const CremaInput *in, uint32_t buttons)
{
    return (in->released & buttons) != 0;
}

#ifdef __cplusplus
}
#endif
