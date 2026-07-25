// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan
//
// Shared app scaffolding for the GX2 PoCs: process lifecycle (ProcUI via
// WHBProc), logging (OSReport -> Cemu log.txt / UDP for real HW), and a
// frame-rate meter that reports both wall-clock FPS and GPU busy time.

#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Init ProcUI + logging + WHBGfx. Returns false on failure.
bool CremaAppInit(const char *name);

// Shutdown in reverse order.
void CremaAppShutdown(void);

// One iteration of the ProcUI loop; false when the app must exit (HOME menu).
bool CremaAppRunning(void);

// --- clock -------------------------------------------------------------------

typedef struct {
    uint64_t startTicks;
    uint64_t prevTicks;
    float    dt;         // seconds since the previous tick, clamped
    float    elapsed;    // seconds since CremaClockInit
} CremaClock;

void CremaClockInit(CremaClock *clock);

// Advance the clock. dt is clamped to 50 ms: coming back from the HOME menu
// hands you a delta of several seconds, and an unclamped one teleports
// everything that integrates against it through the floor.
void CremaClockTick(CremaClock *clock);

// --- frame stats -----------------------------------------------------------

typedef struct {
    uint32_t frameCount;      // frames since last report
    double   fps;             // wall-clock frames per second (last window)
    double   frameMs;         // total wall time per frame, ms
    double   drainMs;         // time blocked in WHBGfxFinishRender (GX2DrawDone),
                              // ms — approximates GPU drain when CPU is idle
    bool     updated;         // true on the frame a new 1s window was closed
} CremaFrameStats;

// Call as the LAST thing in the frame instead of WHBGfxFinishRender: it times
// the GX2SwapScanBuffers+GX2DrawDone inside and logs a stats line once/second.
void CremaFinishRenderAndMark(CremaFrameStats *outStats);

// For frames that end WITHOUT WHBGfxFinishRender (custom swap/fence path):
// same per-second accounting, with the caller-measured sync wait ticks
// reported in the drainMs slot.
void CremaFrameMarkManual(uint64_t syncWaitTicks, CremaFrameStats *outStats);

#ifdef __cplusplus
}
#endif
