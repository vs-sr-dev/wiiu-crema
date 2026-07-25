// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan

#include "crema_blend.h"

#include <gx2/registers.h>

void CremaBlendSet(CremaBlendMode mode)
{
    if (mode == CREMA_BLEND_OPAQUE) {
        // targetBlendEnable is a bitmask of render targets; 0 disables all
        GX2SetColorControl(GX2_LOGIC_OP_COPY, 0x00, FALSE, TRUE);
        return;
    }

    GX2SetColorControl(GX2_LOGIC_OP_COPY, 0x01, FALSE, TRUE);
    if (mode == CREMA_BLEND_ADDITIVE) {
        GX2SetBlendControl(GX2_RENDER_TARGET_0,
                           GX2_BLEND_MODE_SRC_ALPHA, GX2_BLEND_MODE_ONE,
                           GX2_BLEND_COMBINE_MODE_ADD,
                           TRUE,
                           GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_ONE,
                           GX2_BLEND_COMBINE_MODE_ADD);
    } else {
        GX2SetBlendControl(GX2_RENDER_TARGET_0,
                           GX2_BLEND_MODE_SRC_ALPHA,
                           GX2_BLEND_MODE_INV_SRC_ALPHA,
                           GX2_BLEND_COMBINE_MODE_ADD,
                           TRUE,
                           GX2_BLEND_MODE_ONE,
                           GX2_BLEND_MODE_INV_SRC_ALPHA,
                           GX2_BLEND_COMBINE_MODE_ADD);
    }
}

void CremaDepthSet(bool test, bool write)
{
    GX2SetDepthOnlyControl(test ? TRUE : FALSE, write ? TRUE : FALSE,
                           GX2_COMPARE_FUNC_LESS);
}
