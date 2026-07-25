// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan
//
// The HUD, built on the CPU as a list of quads and drawn in one instanced call.
//
// Everything on screen — a letter, a bar, a radar blip, the bracket around a
// locked target — is the same quad with different numbers, so the whole readout
// costs one draw. That is the same trick the billboards use, moved to 2D: no
// dynamic vertex buffer, no per-string draw call, nothing to synchronise.
//
// The packing, two vec4s per item, is what the vertex shader reads:
//
//   slot 0: xy = top-left corner in virtual pixels
//           z  = width
//           w >= 0 : glyph index (code - 32), the quad is z by z
//           w <  0 : no glyph — a solid rectangle whose height is -w
//   slot 1: rgba
//
// One negative number is the entire difference between text and geometry, and
// it is why the score, the throttle bar and the radar all arrive together.
//
// Positions are in a fixed 1280x720 space whatever the screen really is: the
// TV renders it 1:1 and the GamePad squeezes the same layout into 854x480, so
// the HUD is authored once and reads the same on both.
//
// Nothing here knows what a Wii U is.

#pragma once
#include <stdint.h>
#include <string.h>

#define HUD_VIRTUAL_W 1280.0f
#define HUD_VIRTUAL_H 720.0f
#define HUD_MAX_ITEMS 256

// Glyphs advance by 3/4 of their cell: the atlas leaves a column of air on
// either side of a 5-wide glyph inside an 8-wide cell, and this is what turns
// that padding into letter spacing instead of gaps.
#define HUD_ADVANCE 0.75f

typedef struct {
    float    items[HUD_MAX_ITEMS * 2][4];
    uint32_t count;
} HudList;

static void hudClear(HudList *hud)
{
    hud->count = 0;
}

static void hudRect(HudList *hud, float x, float y, float w, float h,
                    float r, float g, float b, float a)
{
    if (hud->count >= HUD_MAX_ITEMS)
        return;
    float *p = hud->items[hud->count * 2];
    float *c = hud->items[hud->count * 2 + 1];
    p[0] = x; p[1] = y; p[2] = w; p[3] = -h;   // negative height: solid quad
    c[0] = r; c[1] = g; c[2] = b; c[3] = a;
    hud->count++;
}

// A frame drawn as four rectangles, because a hollow quad is not a primitive.
static void hudFrame(HudList *hud, float x, float y, float w, float h,
                     float thick, float r, float g, float b, float a)
{
    hudRect(hud, x, y, w, thick, r, g, b, a);
    hudRect(hud, x, y + h - thick, w, thick, r, g, b, a);
    hudRect(hud, x, y, thick, h, r, g, b, a);
    hudRect(hud, x + w - thick, y, thick, h, r, g, b, a);
}

// Returns the x it stopped at, so callers can chain without counting letters.
static float hudText(HudList *hud, float x, float y, float size, const char *s,
                     float r, float g, float b, float a)
{
    for (; *s; s++) {
        int c = (unsigned char)*s;
        if (c >= 'a' && c <= 'z')       // the atlas is capitals only
            c -= 32;
        if (c < 32 || c > 95)
            c = '?';
        if (c != ' ') {                 // a space is advance, not a quad
            if (hud->count >= HUD_MAX_ITEMS)
                break;
            float *p = hud->items[hud->count * 2];
            float *col = hud->items[hud->count * 2 + 1];
            p[0] = x; p[1] = y; p[2] = size; p[3] = (float)(c - 32);
            col[0] = r; col[1] = g; col[2] = b; col[3] = a;
            hud->count++;
        }
        x += size * HUD_ADVANCE;
    }
    return x;
}

// Fixed-width unsigned, right-aligned in `digits` columns — a readout that
// changes width jitters, and a jittering number is unreadable at a glance.
static float hudNumber(HudList *hud, float x, float y, float size,
                       uint32_t value, int digits,
                       float r, float g, float b, float a)
{
    char buf[12];
    if (digits > 10) digits = 10;
    buf[digits] = '\0';
    for (int i = digits - 1; i >= 0; i--) {
        buf[i] = (char)('0' + (value % 10));
        value /= 10;
    }
    return hudText(hud, x, y, size, buf, r, g, b, a);
}

// A meter: the empty channel, then however much of it is full.
static void hudBar(HudList *hud, float x, float y, float w, float h,
                   float fill, float r, float g, float b)
{
    if (fill < 0.0f) fill = 0.0f;
    if (fill > 1.0f) fill = 1.0f;
    hudRect(hud, x, y, w, h, 0.10f, 0.14f, 0.18f, 0.55f);
    hudRect(hud, x, y, w * fill, h, r, g, b, 0.95f);
    hudFrame(hud, x, y, w, h, 1.0f, r * 0.8f, g * 0.8f, b * 0.8f, 0.8f);
}
