// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan

#include "crema_frame.h"
#include "crema_shader.h"

#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/state.h>
#include <gx2/swap.h>
#include <gx2/utils.h>
#include <string.h>
#include <whb/gfx.h>

void CremaFrameInit(CremaFrame *frame, CremaPacing pacing, uint32_t swapInterval)
{
    memset(frame, 0, sizeof(*frame));
    frame->pacing = pacing;
    GX2SetSwapInterval(swapInterval);
}

uint32_t CremaFrameBegin(CremaFrame *frame)
{
    frame->slot = frame->index % CREMA_FRAMES_IN_FLIGHT;
    frame->syncWaitTicks = 0;

    if (frame->pacing != CREMA_PACING_SYNC && frame->fence[frame->slot] != 0) {
        // Frame N-2 owns this uniform slice until its timestamp retires.
        uint64_t w0 = OSGetSystemTime();
        GX2WaitTimeStamp(frame->fence[frame->slot]);
        frame->syncWaitTicks += OSGetSystemTime() - w0;
    }

    if (frame->pacing == CREMA_PACING_FENCED_UNCAPPED) {
        // WHBGfxBeginRender's GX2WaitForFlip would cap us at the vblank; just
        // keep at most CREMA_FRAMES_IN_FLIGHT swaps outstanding instead.
        uint32_t swapCount, flipCount;
        OSTime lastFlip, lastVsync;
        uint64_t w0 = OSGetSystemTime();
        do {
            GX2GetSwapStatus(&swapCount, &flipCount, &lastFlip, &lastVsync);
        } while ((int32_t)(swapCount - flipCount) >= CREMA_FRAMES_IN_FLIGHT);
        frame->syncWaitTicks += OSGetSystemTime() - w0;
    } else {
        WHBGfxBeginRender();
    }
    return frame->slot;
}

void CremaFrameEnd(CremaFrame *frame, CremaFrameStats *stats)
{
    if (frame->pacing == CREMA_PACING_SYNC) {
        uint64_t w0 = OSGetSystemTime();
        WHBGfxFinishRender();     // swap + flush + GX2DrawDone
        frame->syncWaitTicks += OSGetSystemTime() - w0;
    } else {
        GX2SwapScanBuffers();
        GX2Flush();
        frame->fence[frame->slot] = GX2GetLastSubmittedTimeStamp();
    }
    CremaFrameMarkManual(frame->syncWaitTicks, stats);
    frame->index++;
}

void CremaFrameSettle(CremaFrame *frame)
{
    GX2DrawDone();
    memset(frame->fence, 0, sizeof(frame->fence));
}

void CremaFrameDrawBoth(const float clearColor[4], CremaRenderFn draw, void *user)
{
    WHBGfxBeginRenderTV();
    WHBGfxClearColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
    if (draw)
        draw(user);
    WHBGfxFinishRenderTV();

    WHBGfxBeginRenderDRC();
    WHBGfxClearColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
    if (draw)
        draw(user);
    WHBGfxFinishRenderDRC();
}

// --- uniform ring ------------------------------------------------------------

bool CremaUniformRingCreate(CremaUniformRing *ring, size_t bytesPerSlice,
                            uint32_t sliceCount)
{
    memset(ring, 0, sizeof(*ring));
    if (sliceCount == 0)
        return false;
    // Each slice must start at a uniform-block boundary to be bindable.
    size_t align = GX2_UNIFORM_BLOCK_ALIGNMENT;
    size_t padded = (bytesPerSlice + align - 1) & ~(align - 1);
    ring->base = (uint8_t *)CremaUniformAlloc(padded * sliceCount);
    if (!ring->base)
        return false;
    ring->sliceSize  = (uint32_t)padded;
    ring->sliceCount = sliceCount;
    return true;
}

void *CremaUniformRingSlice(const CremaUniformRing *ring, uint32_t slot)
{
    return ring->base + (size_t)(slot % ring->sliceCount) * ring->sliceSize;
}

void *CremaUniformRingStore(const CremaUniformRing *ring, uint32_t slot,
                            const void *src, size_t bytes)
{
    void *slice = CremaUniformRingSlice(ring, slot);
    CremaUniformStore(slice, src, bytes);
    return slice;
}

void CremaUniformRingDestroy(CremaUniformRing *ring)
{
    CremaUniformFreeBlock(ring->base);
    ring->base = NULL;
    ring->sliceCount = 0;
}
