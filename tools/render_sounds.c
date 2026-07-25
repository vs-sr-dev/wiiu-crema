// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan
//
// Renders poc11's sounds to WAV on the PC — from the example's own source, not
// from a copy of it. sounds.h contains no console header, so the compiler that
// builds the game and the one on your desk produce the same samples; anything
// you fix here is fixed in the game, because it is the same file.
//
//   cc -O2 -o render_sounds tools/render_sounds.c -lm && ./render_sounds out/
//
// It also prints what the ear cannot measure: peak level, and whether the
// engine loop really closes on itself. A loop that ends on a different value
// from the one it starts on clicks once per period — five times a second, in
// this case — and on the console you would hear it and blame the DSP.

#include "../examples/poc11-flight/sounds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put32(FILE *f, uint32_t v)
{
    fputc(v & 0xFF, f); fputc((v >> 8) & 0xFF, f);
    fputc((v >> 16) & 0xFF, f); fputc((v >> 24) & 0xFF, f);
}

static void put16(FILE *f, uint16_t v)
{
    fputc(v & 0xFF, f); fputc((v >> 8) & 0xFF, f);
}

static int writeWav(const char *path, const int16_t *pcm, uint32_t n, uint32_t rate)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return 0;
    uint32_t dataBytes = n * 2u;
    fwrite("RIFF", 1, 4, f);  put32(f, 36 + dataBytes);  fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);  put32(f, 16);
    put16(f, 1);              // PCM
    put16(f, 1);              // mono, like an AX voice
    put32(f, rate);
    put32(f, rate * 2);       // byte rate
    put16(f, 2);              // block align
    put16(f, 16);             // bits
    fwrite("data", 1, 4, f);  put32(f, dataBytes);
    for (uint32_t i = 0; i < n; i++)
        put16(f, (uint16_t)pcm[i]);
    fclose(f);
    return 1;
}

static void report(const char *name, const int16_t *pcm, uint32_t n)
{
    int32_t peak = 0;
    double sum = 0.0;
    uint32_t clipped = 0;
    for (uint32_t i = 0; i < n; i++) {
        int32_t a = pcm[i] < 0 ? -pcm[i] : pcm[i];
        if (a > peak) peak = a;
        if (a >= 32000) clipped++;
        sum += (double)pcm[i] * (double)pcm[i];
    }
    printf("  %-8s %6u samples  %.3f s  peak %5d (%.0f%%)  rms %5.0f  clipped %u\n",
           name, n, (double)n / SND_RATE, (int)peak, 100.0 * peak / 32767.0,
           n ? sqrt(sum / n) : 0.0, clipped);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    int16_t *buf = (int16_t *)malloc(SND_MAX_SAMPS * sizeof(int16_t));
    char path[512];
    uint32_t n;

    printf("poc11 sounds, %d Hz mono:\n", SND_RATE);

    n = bakeLaser(buf);
    report("laser", buf, n);
    snprintf(path, sizeof(path), "%s/laser.wav", dir);
    writeWav(path, buf, n, SND_RATE);

    n = bakeBoom(buf);
    report("boom", buf, n);
    snprintf(path, sizeof(path), "%s/boom.wav", dir);
    writeWav(path, buf, n, SND_RATE);

    n = bakeEngine(buf);
    report("engine", buf, n);
    snprintf(path, sizeof(path), "%s/engine.wav", dir);
    writeWav(path, buf, n, SND_RATE);

    // The seam: how big is the jump from the last sample back to the first,
    // measured against the biggest step the waveform takes anywhere inside the
    // loop. If the wrap is not the smallest step in the sound, it will tick.
    int32_t worst = 0;
    for (uint32_t i = 1; i < n; i++) {
        int32_t d = buf[i] - buf[i - 1];
        if (d < 0) d = -d;
        if (d > worst) worst = d;
    }
    int32_t seam = buf[0] - buf[n - 1];
    if (seam < 0) seam = -seam;
    printf("  engine loop seam: %d, biggest step inside the loop: %d -> %s\n",
           (int)seam, (int)worst, seam <= worst ? "closes clean" : "WILL CLICK");

    free(buf);
    printf("wrote laser.wav, boom.wav, engine.wav to %s\n", dir);
    return 0;
}
