// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan

#include "crema_app.h"

#include <whb/proc.h>
#include <whb/gfx.h>
#include <whb/log.h>
#include <whb/log_cafe.h>
#include <whb/log_udp.h>
#include <coreinit/time.h>

static const char *sAppName = "gx2poc";

bool CremaAppInit(const char *name)
{
    sAppName = name;
    WHBProcInit();
    WHBLogCafeInit();   // OSReport -> shows up in Cemu's log.txt
    WHBLogUdpInit();    // UDP broadcast on port 4405 -> udplogserver on real HW
    WHBLogPrintf("[%s] starting", sAppName);
    if (!WHBGfxInit()) {
        WHBLogPrintf("[%s] WHBGfxInit failed", sAppName);
        return false;
    }
    return true;
}

void CremaAppShutdown(void)
{
    WHBLogPrintf("[%s] shutting down", sAppName);
    WHBGfxShutdown();
    WHBLogUdpDeinit();
    WHBLogCafeDeinit();
    WHBProcShutdown();
}

bool CremaAppRunning(void)
{
    return WHBProcIsRunning();
}

// --- frame stats -------------------------------------------------------------

static uint32_t sFrames;
static uint64_t sWindowStart;       // OSTime at start of current 1s window
static uint64_t sDrainTicksAccum;   // ticks spent blocked in WHBGfxFinishRender
static CremaFrameStats sLast;

void CremaFinishRenderAndMark(CremaFrameStats *outStats)
{
    uint64_t before = OSGetSystemTime();
    WHBGfxFinishRender();           // GX2SwapScanBuffers + GX2Flush + GX2DrawDone
    CremaFrameMarkManual(OSGetSystemTime() - before, outStats);
}

void CremaFrameMarkManual(uint64_t syncWaitTicks, CremaFrameStats *outStats)
{
    uint64_t now = OSGetSystemTime();
    sDrainTicksAccum += syncWaitTicks;

    if (sWindowStart == 0)
        sWindowStart = now;
    sFrames++;

    sLast.updated = false;
    uint64_t elapsed = now - sWindowStart;
    if (elapsed >= (uint64_t)OSSecondsToTicks(1)) {
        double sec = (double)OSTicksToMicroseconds(elapsed) / 1e6;
        sLast.frameCount = sFrames;
        sLast.fps        = sFrames / sec;
        sLast.frameMs    = 1000.0 * sec / sFrames;
        sLast.drainMs    = (double)OSTicksToMicroseconds(sDrainTicksAccum)
                           / 1000.0 / sFrames;
        sLast.updated = true;
        WHBLogPrintf("[%s] %.1f fps | frame %.2f ms | drain %.2f ms",
                     sAppName, sLast.fps, sLast.frameMs, sLast.drainMs);
        sFrames = 0;
        sDrainTicksAccum = 0;
        sWindowStart = now;
    }

    if (outStats)
        *outStats = sLast;
}
