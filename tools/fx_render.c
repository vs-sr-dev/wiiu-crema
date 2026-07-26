// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan
//
// The PC end of the one piece of audio code that is genuinely portable.
//
// `examples/poc11-flight/echo.h` includes no console header, and the comment at
// the top of it explains why: an effect is real DSP, it is judged by ear, and on
// the console a wrong number costs a build, a deploy and a reboot. So it was
// written to be auditionable on a PC. It just never had anything to audition it
// with. This is that thing.
//
// It is not a simulation of the Wii U's audio path, it is the same code in the
// same shape. Three details are copied rather than approximated, and they are
// the three that a plausible-looking test harness would get wrong:
//
//   1. **The buffer is planar int32, one pointer per channel.** Not interleaved,
//      not int16, not float. An off-by-one in a channel stride shows up here.
//   2. **144 samples at a time**, which is AX's audio frame. An effect that only
//      works on a whole file is an effect whose delay line wraps once instead of
//      three hundred times a second, and the wrap is where the bugs are.
//   3. **The signal chain around it.** The effect sits on an aux bus: AX sends
//      it `send * dry`, the effect returns something, and AX adds that to the
//      main mix which is *already carrying the dry*. So what a listener hears is
//
//          out = main*dry + echo(send*dry)
//
//      and since echoProcess passes its input through (`out = in + wet*tap`),
//      the dry arrives twice — once at `main`, once at `send`. That is not this
//      tool's invention, it is what the console does, and it is the reason
//      raising the send makes the mix louder before it makes it wetter. Worth
//      knowing before spending headroom on it. `--insert` models the other
//      placement instead (AXRegisterDeviceFinalMixCallback), where the effect
//      owns the whole mix and the dry passes through it exactly once.
//
// What is deliberately NOT reproduced: byte order. On the console the samples
// are big-endian because the CPU is; here they are little-endian because the CPU
// is. Both are native, echo.h does arithmetic and no byte fiddling, and a
// swapped version of this tool would be testing a bug the console cannot have.
//
// Build: tools/build_fx.sh  (host gcc, or the devkitPro container's if there is
// no local one — no new dependency either way).

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The effect itself, from where it lives. The path is awkward on purpose: this
// tool is a second *caller* of echo.h but not a second *example*, and in this
// project a file moves into crema/ when a second example needs it, not when a
// second file includes it. When that day comes the include gets shorter.
#include "../examples/poc11-flight/echo.h"

#define AX_FRAME_SAMPLES 144      // what AX hands an aux callback, at 48 kHz
#define AX_RATE          48000

// --- WAV, the sixteen-bit mono-or-stereo subset ------------------------------

typedef struct {
    int16_t *samples;      // interleaved, as a WAV keeps them
    uint32_t frames;
    uint32_t rate;
    uint32_t channels;
} Wave;

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t rd16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static bool waveLoad(Wave *w, const char *path)
{
    memset(w, 0, sizeof(*w));
    FILE *fh = fopen(path, "rb");
    if (!fh) {
        fprintf(stderr, "fx_render: cannot open %s\n", path);
        return false;
    }
    uint8_t riff[12];
    if (fread(riff, 1, 12, fh) != 12 || memcmp(riff, "RIFF", 4) != 0 ||
        memcmp(riff + 8, "WAVE", 4) != 0) {
        fprintf(stderr, "fx_render: %s is not a RIFF/WAVE file\n", path);
        fclose(fh);
        return false;
    }

    uint32_t channels = 0, rate = 0, bits = 0;
    // Chunk walk rather than "assume fmt then data": anything that has been
    // through an editor carries LIST or fact chunks in between.
    for (;;) {
        uint8_t hdr[8];
        if (fread(hdr, 1, 8, fh) != 8)
            break;
        uint32_t id0 = 0;
        memcpy(&id0, hdr, 4);
        uint32_t size = rd32(hdr + 4);

        if (memcmp(hdr, "fmt ", 4) == 0 && size >= 16) {
            uint8_t fmt[16];
            if (fread(fmt, 1, 16, fh) != 16)
                break;
            uint32_t tag = rd16(fmt);
            channels = rd16(fmt + 2);
            rate     = rd32(fmt + 4);
            bits     = rd16(fmt + 14);
            if (tag != 1 && tag != 0xFFFE) {
                fprintf(stderr, "fx_render: %s is not PCM (format tag %u)\n",
                        path, tag);
                fclose(fh);
                return false;
            }
            if (size > 16)
                fseek(fh, (long)(size - 16), SEEK_CUR);
        } else if (memcmp(hdr, "data", 4) == 0) {
            if (bits != 16 || channels < 1 || channels > 2) {
                fprintf(stderr, "fx_render: %s is %u-bit %u-channel; this tool "
                        "reads 16-bit mono or stereo\n", path, bits, channels);
                fclose(fh);
                return false;
            }
            uint32_t frames = size / (2 * channels);
            int16_t *pcm = (int16_t *)malloc((size_t)frames * channels * 2);
            if (!pcm || fread(pcm, 2 * channels, frames, fh) != frames) {
                fprintf(stderr, "fx_render: %s ends inside its data chunk\n",
                        path);
                free(pcm);
                fclose(fh);
                return false;
            }
            w->samples = pcm;
            w->frames = frames;
            w->rate = rate;
            w->channels = channels;
            fclose(fh);
            return true;
        } else {
            fseek(fh, (long)(size + (size & 1)), SEEK_CUR);
        }
        (void)id0;
    }
    fprintf(stderr, "fx_render: %s has no data chunk this tool can read\n", path);
    fclose(fh);
    return false;
}

static void wr32(FILE *fh, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16),
                     (uint8_t)(v >> 24) };
    fwrite(b, 1, 4, fh);
}

static void wr16(FILE *fh, uint32_t v)
{
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    fwrite(b, 1, 2, fh);
}

static bool waveSave(const char *path, const int16_t *interleaved,
                     uint32_t frames, uint32_t rate, uint32_t channels)
{
    FILE *fh = fopen(path, "wb");
    if (!fh) {
        fprintf(stderr, "fx_render: cannot write %s\n", path);
        return false;
    }
    uint32_t dataBytes = frames * channels * 2;
    fwrite("RIFF", 1, 4, fh);
    wr32(fh, 36 + dataBytes);
    fwrite("WAVEfmt ", 1, 8, fh);
    wr32(fh, 16);
    wr16(fh, 1);                        // PCM
    wr16(fh, channels);
    wr32(fh, rate);
    wr32(fh, rate * channels * 2);      // byte rate
    wr16(fh, channels * 2);             // block align
    wr16(fh, 16);
    fwrite("data", 1, 4, fh);
    wr32(fh, dataBytes);
    fwrite(interleaved, 2 * channels, frames, fh);
    bool ok = fclose(fh) == 0;
    return ok;
}

// A click train, for when there is no WAV to hand. It is also the fastest way to
// see whether a delay is doing what it was asked: the repeats are countable, the
// spacing is measurable with a ruler on a waveform, and a feedback that is
// secretly above one is obvious in one look instead of after ten seconds of
// listening to something get louder.
static void makeImpulses(Wave *w, uint32_t rate, float seconds, float everyMs)
{
    uint32_t frames = (uint32_t)(seconds * (float)rate);
    w->samples = (int16_t *)calloc(frames, sizeof(int16_t));
    w->frames = frames;
    w->rate = rate;
    w->channels = 1;
    uint32_t step = (uint32_t)(everyMs * (float)rate / 1000.0f);
    if (step == 0)
        step = 1;
    for (uint32_t i = 0; i < frames; i += step) {
        // Two samples, not one: a single sample is inaudible on most speakers
        // and invisible in most editors, and neither helps.
        w->samples[i] = 20000;
        if (i + 1 < frames)
            w->samples[i + 1] = -20000;
    }
}

// --- the run ------------------------------------------------------------------

static int32_t clampToInt16(int32_t v, uint32_t *clips)
{
    if (v > 32767) {
        (*clips)++;
        return 32767;
    }
    if (v < -32768) {
        (*clips)++;
        return -32768;
    }
    return v;
}

static void usage(void)
{
    printf(
"fx_render — run Crema's echo on a PC, the same code the DSP runs\n"
"\n"
"  fx_render [options] [input.wav]\n"
"\n"
"  --out FILE        where to write the result (default fx_out.wav)\n"
"  --impulse [MS]    no input file: a click every MS ms (default 500)\n"
"  --seconds S       length of the generated input (default 3)\n"
"  --rate HZ         rate of the generated input (default 48000)\n"
"\n"
"  --delay MS        how far back the tap is (default 170)\n"
"  --feedback F      how much of the tap goes back in (default 0.35)\n"
"  --wet W           how much of the tap comes out (default 0.45)\n"
"  --line MS         length of the delay line (default 200)\n"
"\n"
"  --send S          aux send level, 0..1 (default 0.30)\n"
"  --main M          dry level in the main mix (default 1.00)\n"
"  --insert          the effect owns the whole mix instead of an aux bus\n"
"  --block N         samples per call (default 144, which is AX's audio frame)\n");
}

int main(int argc, char **argv)
{
    const char *inPath = NULL;
    const char *outPath = "fx_out.wav";
    bool impulse = false, insert = false;
    float impulseMs = 500.0f, seconds = 3.0f;
    uint32_t genRate = AX_RATE, block = AX_FRAME_SAMPLES;
    float delayMs = 170.0f, feedback = 0.35f, wet = 0.45f, lineMs = 200.0f;
    float send = 0.30f, main_ = 1.0f;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        bool hasNext = (i + 1 < argc) && argv[i + 1][0] != '-';
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage();
            return 0;
        } else if (strcmp(a, "--impulse") == 0) {
            impulse = true;
            if (hasNext) impulseMs = (float)atof(argv[++i]);
        } else if (strcmp(a, "--insert") == 0) {
            insert = true;
        } else if (strcmp(a, "--out") == 0 && i + 1 < argc) {
            outPath = argv[++i];
        } else if (strcmp(a, "--seconds") == 0 && hasNext) {
            seconds = (float)atof(argv[++i]);
        } else if (strcmp(a, "--rate") == 0 && hasNext) {
            genRate = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(a, "--delay") == 0 && hasNext) {
            delayMs = (float)atof(argv[++i]);
        } else if (strcmp(a, "--feedback") == 0 && hasNext) {
            feedback = (float)atof(argv[++i]);
        } else if (strcmp(a, "--wet") == 0 && hasNext) {
            wet = (float)atof(argv[++i]);
        } else if (strcmp(a, "--line") == 0 && hasNext) {
            lineMs = (float)atof(argv[++i]);
        } else if (strcmp(a, "--send") == 0 && hasNext) {
            send = (float)atof(argv[++i]);
        } else if (strcmp(a, "--main") == 0 && hasNext) {
            main_ = (float)atof(argv[++i]);
        } else if (strcmp(a, "--block") == 0 && hasNext) {
            block = (uint32_t)atoi(argv[++i]);
        } else if (a[0] != '-') {
            inPath = a;
        } else {
            fprintf(stderr, "fx_render: unknown option %s\n", a);
            usage();
            return 2;
        }
    }

    Wave in;
    if (impulse || !inPath) {
        if (!inPath && !impulse)
            printf("fx_render: no input file, using a click train "
                   "(--help for options)\n");
        makeImpulses(&in, genRate, seconds, impulseMs);
    } else if (!waveLoad(&in, inPath)) {
        return 1;
    }
    if (in.frames == 0 || block == 0) {
        fprintf(stderr, "fx_render: nothing to process\n");
        return 1;
    }
    if (in.rate != AX_RATE)
        printf("note: this file is %u Hz, the console mixes at %u — the delay "
               "is computed in samples of *this* rate, so it lasts the same "
               "number of milliseconds but is not the same delay line\n",
               in.rate, AX_RATE);

    // Two channels, because that is what the effect is built for on the console:
    // crema_audio puts a mono voice on channels 0 and 1 and the surround
    // channels stay at zero, so delaying six would be delaying four silences.
    const uint32_t channels = 2;
    uint32_t lineLen = (uint32_t)(lineMs * (float)in.rate / 1000.0f);
    uint32_t delaySamples = (uint32_t)(delayMs * (float)in.rate / 1000.0f);
    if (lineLen < 2)
        lineLen = 2;

    int32_t *line = (int32_t *)calloc((size_t)lineLen * channels,
                                      sizeof(int32_t));
    int32_t *planar = (int32_t *)calloc((size_t)block * channels,
                                        sizeof(int32_t));
    int16_t *out = (int16_t *)calloc((size_t)in.frames * channels,
                                     sizeof(int16_t));
    if (!line || !planar || !out) {
        fprintf(stderr, "fx_render: out of memory\n");
        return 1;
    }

    Echo echo;
    // An aux effect returns the wet only; an insert has been handed the mix and
    // must give it back. The tool exists to let you hear the difference, so it
    // is the one thing it does not decide for you.
    echoInit(&echo, line, lineLen, channels, delaySamples, feedback, wet,
             !insert);

    // The two pointers AX would hand the callback.
    int32_t *data[ECHO_MAX_CHANNELS];
    for (uint32_t c = 0; c < channels; c++)
        data[c] = planar + (size_t)c * block;

    int32_t peakMix = 0;
    uint32_t clips = 0;

    for (uint32_t base = 0; base < in.frames; base += block) {
        uint32_t n = in.frames - base;
        if (n > block)
            n = block;

        // Deinterleave into what the effect expects, applying the send on the
        // way in. This is where a mono file becomes two channels, which is also
        // what a mono AX voice does.
        for (uint32_t i = 0; i < n; i++) {
            const int16_t *frame = in.samples +
                                   (size_t)(base + i) * in.channels;
            int32_t l = frame[0];
            int32_t r = in.channels > 1 ? frame[1] : l;
            float g = insert ? main_ : send;
            data[0][i] = (int32_t)((float)l * g);
            data[1][i] = (int32_t)((float)r * g);
        }
        for (uint32_t i = n; i < block; i++)
            data[0][i] = data[1][i] = 0;    // the tail of the last block

        echoProcess(&echo, data, channels, n);

        for (uint32_t i = 0; i < n; i++) {
            const int16_t *frame = in.samples +
                                   (size_t)(base + i) * in.channels;
            int32_t dryL = frame[0], dryR = in.channels > 1 ? frame[1] : dryL;
            int32_t l, r;
            if (insert) {
                // The effect had the mix; what comes out of it *is* the mix.
                l = data[0][i];
                r = data[1][i];
            } else {
                // An aux return is added to a main bus that already has the dry.
                l = (int32_t)((float)dryL * main_) + data[0][i];
                r = (int32_t)((float)dryR * main_) + data[1][i];
            }
            int32_t a = l < 0 ? -l : l, b = r < 0 ? -r : r;
            if (a > peakMix) peakMix = a;
            if (b > peakMix) peakMix = b;
            out[(size_t)(base + i) * 2]     = (int16_t)clampToInt16(l, &clips);
            out[(size_t)(base + i) * 2 + 1] = (int16_t)clampToInt16(r, &clips);
        }
    }

    if (!waveSave(outPath, out, in.frames, in.rate, channels))
        return 1;

    printf("fx_render: %s -> %s\n", impulse || !inPath ? "(clicks)" : inPath,
           outPath);
    printf("  %u frames at %u Hz, %u ch in, %u calls of %u samples\n",
           in.frames, in.rate, in.channels, echo.calls, block);
    printf("  echo: %.0f ms tap in a %.0f ms line, feedback %.2f, wet %.2f, "
           "%s\n", delayMs, lineMs, feedback, wet,
           insert ? "insert on the whole mix" : "on an aux bus");
    printf("  effect saw peak %d, returned peak %d\n",
           (int)echo.peakIn, (int)echo.peakOut);
    // The same question the console answers with a second aux bus used as a
    // meter, asked here where the answer costs nothing: full scale is 32767.
    printf("  mix peak %d (%.0f%% of full scale)%s\n", (int)peakMix,
           100.0 * peakMix / 32767.0,
           clips ? "" : " — nothing clipped");
    if (clips)
        printf("  %u samples clipped: turn --main or --send down\n", clips);

    free(line);
    free(planar);
    free(out);
    free(in.samples);
    return 0;
}
