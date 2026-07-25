// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan

#include "crema_input.h"

#include <math.h>
#include <string.h>

#define CREMA_INPUT_DEFAULT_DEADZONE 0.12f

void CremaInputInit(CremaInput *in)
{
    memset(in, 0, sizeof(*in));
    in->deadzone = CREMA_INPUT_DEFAULT_DEADZONE;
}

// Rescale what is left of the range instead of simply zeroing the middle:
// clamping alone makes the stick jump straight from 0 to 0.12 the instant it
// leaves the dead zone, which feels like a notch. This starts from zero.
static float applyDeadzone(float value, float dz)
{
    float mag = fabsf(value);
    if (mag <= dz)
        return 0.0f;
    float scaled = (mag - dz) / (1.0f - dz);
    if (scaled > 1.0f)
        scaled = 1.0f;
    return value < 0.0f ? -scaled : scaled;
}

void CremaInputPoll(CremaInput *in)
{
    VPADStatus pad;
    VPADReadError err;
    memset(&pad, 0, sizeof(pad));
    VPADRead(VPAD_CHAN_0, &pad, 1, &err);

    in->prevHeld = in->held;
    if (err == VPAD_READ_SUCCESS) {
        in->connected = true;
        in->held   = pad.hold;
        in->leftX  = applyDeadzone(pad.leftStick.x,  in->deadzone);
        in->leftY  = applyDeadzone(pad.leftStick.y,  in->deadzone);
        in->rightX = applyDeadzone(pad.rightStick.x, in->deadzone);
        in->rightY = applyDeadzone(pad.rightStick.y, in->deadzone);
    } else {
        // Treat a silent pad as everything released, so a game that loses the
        // GamePad mid-frame stops flying rather than holding the last input.
        in->connected = false;
        in->held = 0;
        in->leftX = in->leftY = in->rightX = in->rightY = 0.0f;
    }

    in->pressed  = in->held & ~in->prevHeld;
    in->released = in->prevHeld & ~in->held;
}
