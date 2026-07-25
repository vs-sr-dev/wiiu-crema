#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Samuele Voltan
"""Draw Crema's HUD font atlas.

The font is drawn here as strokes — polylines on a unit box — and rasterised
four times too large before being scaled down, which is where the smooth edges
come from. A hand-placed bitmap was the first version of this file and it was
honest but it looked it: a 5x7 grid magnified three times on a 720p screen is
visibly a 5x7 grid. Strokes cost the same to author and survive any size.

No typeface is imported and none is needed: every glyph below is a list of
points. That keeps the repo's rule intact (every byte home-grown) and it
happens to be the right look — a cockpit readout is drawn, not typeset.

The atlas is 128x128: an 8x8 grid of 16x16 cells holding ASCII 32..95, so
digits, capitals and the punctuation a cockpit needs. Because the set starts
at space and is contiguous, the shader finds a glyph with two divisions of
`code - 32` and no lookup table. Each glyph is drawn inside a 10x11 box with
at least two texels of margin all round — margin the sampler needs, or the
filter drags the neighbouring cell in at the edges.

White pixels with the shape in the alpha channel: the colour comes from the
instance data, so one atlas serves every readout on the screen.

    python tools/gen_font.py examples/poc11-flight/assets/font.png
"""

import math
import os
import sys

CELL = 16           # texels per cell in the final atlas
COLS = 8
FIRST = 32          # space
COUNT = 64          # through '_'
SS = 4              # supersampling: draw at 4x, scale down, get the edges free

GLYPH_X = 2.5       # the drawing box inside a cell, in texels
GLYPH_Y = 2.5
GLYPH_W = 10.0
GLYPH_H = 11.0
STROKE = 1.5        # texels


def arc(cx, cy, rx, ry, a0, a1, n=16):
    """Points along an ellipse. Angles in degrees, 0 = right, 90 = DOWN."""
    return [(cx + rx * math.cos(math.radians(a0 + (a1 - a0) * i / n)),
             cy + ry * math.sin(math.radians(a0 + (a1 - a0) * i / n)))
            for i in range(n + 1)]


def circle(cx, cy, rx, ry):
    return arc(cx, cy, rx, ry, 0, 360, 24)


def dot(x, y):
    """A round cap with nowhere to go: the stroke's own width is the dot."""
    return [(x, y), (x + 0.001, y)]


# Every glyph is a list of polylines on a unit box: x right, y down, (0,0) is
# the top-left of the drawing box and (1,1) its bottom-left corner... which is
# to say a capital letter fills it exactly.
GLYPHS = {
    " ": [],
    "!": [[(0.5, 0.0), (0.5, 0.70)], dot(0.5, 0.95)],
    '"': [[(0.35, 0.0), (0.30, 0.25)], [(0.65, 0.0), (0.60, 0.25)]],
    "#": [[(0.28, 0.05), (0.18, 0.95)], [(0.72, 0.05), (0.62, 0.95)],
          [(0.08, 0.35), (0.88, 0.35)], [(0.05, 0.65), (0.85, 0.65)]],
    "$": [[(0.90, 0.20), (0.65, 0.08), (0.30, 0.10), (0.15, 0.28),
           (0.35, 0.45), (0.70, 0.55), (0.88, 0.72), (0.70, 0.92),
           (0.30, 0.92), (0.10, 0.80)],
          [(0.50, 0.00), (0.50, 1.00)]],
    "%": [circle(0.24, 0.20, 0.19, 0.19), circle(0.76, 0.80, 0.19, 0.19),
          [(0.92, 0.05), (0.08, 0.95)]],
    "&": [[(0.95, 1.00), (0.25, 0.30), (0.32, 0.08), (0.60, 0.10),
           (0.58, 0.35), (0.12, 0.68), (0.25, 0.97), (0.62, 0.95),
           (0.85, 0.70)]],
    "'": [[(0.50, 0.00), (0.45, 0.25)]],
    "(": [[(0.70, 0.00), (0.35, 0.30), (0.35, 0.70), (0.70, 1.00)]],
    ")": [[(0.30, 0.00), (0.65, 0.30), (0.65, 0.70), (0.30, 1.00)]],
    "*": [[(0.5, 0.15), (0.5, 0.75)], [(0.18, 0.28), (0.82, 0.62)],
          [(0.82, 0.28), (0.18, 0.62)]],
    "+": [[(0.10, 0.50), (0.90, 0.50)], [(0.50, 0.15), (0.50, 0.85)]],
    ",": [[(0.55, 0.88), (0.38, 1.10)]],
    "-": [[(0.10, 0.52), (0.90, 0.52)]],
    ".": [dot(0.50, 0.95)],
    "/": [[(0.90, 0.00), (0.10, 1.00)]],
    "0": [circle(0.5, 0.5, 0.45, 0.5), [(0.78, 0.18), (0.22, 0.82)]],
    "1": [[(0.18, 0.22), (0.50, 0.00), (0.50, 1.00)],
          [(0.18, 1.00), (0.82, 1.00)]],
    "2": [[(0.06, 0.24), (0.20, 0.04), (0.55, 0.00), (0.85, 0.12),
           (0.88, 0.36), (0.06, 1.00), (0.95, 1.00)]],
    "3": [[(0.08, 0.16), (0.32, 0.00), (0.75, 0.06), (0.85, 0.28),
           (0.45, 0.48), (0.88, 0.64), (0.80, 0.94), (0.32, 1.00),
           (0.06, 0.86)]],
    "4": [[(0.72, 0.00), (0.05, 0.70), (0.98, 0.70)],
          [(0.72, 0.32), (0.72, 1.00)]],
    "5": [[(0.88, 0.02), (0.18, 0.02), (0.12, 0.42), (0.48, 0.34),
           (0.85, 0.50), (0.88, 0.78), (0.58, 0.99), (0.20, 0.97),
           (0.06, 0.84)]],
    "6": [[(0.84, 0.06), (0.40, 0.06), (0.12, 0.40), (0.06, 0.74),
           (0.30, 0.99), (0.70, 0.97), (0.90, 0.74), (0.74, 0.50),
           (0.34, 0.46), (0.12, 0.62)]],
    "7": [[(0.04, 0.00), (0.96, 0.00), (0.34, 1.00)]],
    "8": [circle(0.5, 0.245, 0.38, 0.245), circle(0.5, 0.745, 0.45, 0.255)],
    "9": [[(0.16, 0.94), (0.60, 0.94), (0.88, 0.60), (0.94, 0.26),
           (0.70, 0.01), (0.30, 0.03), (0.10, 0.26), (0.26, 0.50),
           (0.66, 0.54), (0.88, 0.38)]],
    ":": [dot(0.5, 0.32), dot(0.5, 0.88)],
    ";": [dot(0.5, 0.32), [(0.55, 0.82), (0.38, 1.05)]],
    "<": [[(0.85, 0.12), (0.15, 0.50), (0.85, 0.88)]],
    "=": [[(0.10, 0.36), (0.90, 0.36)], [(0.10, 0.68), (0.90, 0.68)]],
    ">": [[(0.15, 0.12), (0.85, 0.50), (0.15, 0.88)]],
    "?": [[(0.10, 0.22), (0.30, 0.02), (0.70, 0.02), (0.90, 0.24),
           (0.52, 0.52), (0.50, 0.70)], dot(0.50, 0.95)],
    "@": [arc(0.5, 0.5, 0.50, 0.50, -30, 260, 20),
          circle(0.48, 0.55, 0.16, 0.16), [(0.98, 0.55), (0.98, 0.72)]],
    "A": [[(0.02, 1.00), (0.50, 0.00), (0.98, 1.00)],
          [(0.19, 0.64), (0.81, 0.64)]],
    "B": [[(0.05, 0.00), (0.05, 1.00)],
          [(0.05, 0.00), (0.55, 0.00)] + arc(0.55, 0.25, 0.40, 0.25, -90, 90)
          + [(0.05, 0.50)],
          [(0.05, 0.50), (0.58, 0.50)] + arc(0.58, 0.75, 0.40, 0.25, -90, 90)
          + [(0.05, 1.00)]],
    "C": [arc(0.5, 0.5, 0.48, 0.5, 45, 315)],
    "D": [[(0.05, 0.00), (0.05, 1.00)],
          [(0.05, 0.00), (0.45, 0.00)] + arc(0.45, 0.50, 0.50, 0.50, -90, 90)
          + [(0.05, 1.00)]],
    "E": [[(0.92, 0.00), (0.05, 0.00), (0.05, 1.00), (0.92, 1.00)],
          [(0.05, 0.50), (0.72, 0.50)]],
    "F": [[(0.92, 0.00), (0.05, 0.00), (0.05, 1.00)],
          [(0.05, 0.50), (0.70, 0.50)]],
    # the opening has to be at the top right, which means going round the other
    # way: start at the end of the bar and travel down, left, over the top
    "G": [arc(0.5, 0.5, 0.48, 0.5, 0, 315, 20), [(0.98, 0.50), (0.52, 0.50)]],
    "H": [[(0.05, 0.00), (0.05, 1.00)], [(0.95, 0.00), (0.95, 1.00)],
          [(0.05, 0.52), (0.95, 0.52)]],
    "I": [[(0.18, 0.00), (0.82, 0.00)], [(0.50, 0.00), (0.50, 1.00)],
          [(0.18, 1.00), (0.82, 1.00)]],
    "J": [[(0.80, 0.00), (0.80, 0.72)] + arc(0.45, 0.72, 0.35, 0.28, 0, 180)],
    "K": [[(0.05, 0.00), (0.05, 1.00)], [(0.92, 0.00), (0.08, 0.58)],
          [(0.32, 0.42), (0.95, 1.00)]],
    "L": [[(0.08, 0.00), (0.08, 1.00), (0.92, 1.00)]],
    "M": [[(0.02, 1.00), (0.02, 0.00), (0.50, 0.62), (0.98, 0.00),
           (0.98, 1.00)]],
    "N": [[(0.05, 1.00), (0.05, 0.00), (0.95, 1.00), (0.95, 0.00)]],
    "O": [circle(0.5, 0.5, 0.47, 0.5)],
    "P": [[(0.05, 0.00), (0.05, 1.00)],
          [(0.05, 0.00), (0.55, 0.00)] + arc(0.55, 0.28, 0.42, 0.28, -90, 90)
          + [(0.05, 0.56)]],
    "Q": [circle(0.5, 0.5, 0.47, 0.5), [(0.62, 0.68), (1.00, 1.05)]],
    "R": [[(0.05, 0.00), (0.05, 1.00)],
          [(0.05, 0.00), (0.52, 0.00)] + arc(0.52, 0.28, 0.42, 0.28, -90, 90)
          + [(0.05, 0.56)],
          [(0.48, 0.56), (0.98, 1.00)]],
    "S": [[(0.94, 0.16), (0.72, 0.02), (0.30, 0.02), (0.06, 0.22),
           (0.16, 0.44), (0.50, 0.50), (0.86, 0.58), (0.95, 0.80),
           (0.70, 0.98), (0.24, 0.98), (0.05, 0.84)]],
    "T": [[(0.02, 0.00), (0.98, 0.00)], [(0.50, 0.00), (0.50, 1.00)]],
    "U": [[(0.05, 0.00), (0.05, 0.62)] + arc(0.50, 0.62, 0.45, 0.38, 180, 0)
          + [(0.95, 0.00)]],
    "V": [[(0.02, 0.00), (0.50, 1.00), (0.98, 0.00)]],
    "W": [[(0.00, 0.00), (0.22, 1.00), (0.50, 0.34), (0.78, 1.00),
           (1.00, 0.00)]],
    "X": [[(0.04, 0.00), (0.96, 1.00)], [(0.96, 0.00), (0.04, 1.00)]],
    "Y": [[(0.04, 0.00), (0.50, 0.52), (0.96, 0.00)],
          [(0.50, 0.52), (0.50, 1.00)]],
    "Z": [[(0.04, 0.00), (0.96, 0.00), (0.04, 1.00), (0.96, 1.00)]],
    "[": [[(0.72, 0.00), (0.32, 0.00), (0.32, 1.00), (0.72, 1.00)]],
    "\\": [[(0.10, 0.00), (0.90, 1.00)]],
    "]": [[(0.28, 0.00), (0.68, 0.00), (0.68, 1.00), (0.28, 1.00)]],
    "^": [[(0.18, 0.32), (0.50, 0.02), (0.82, 0.32)]],
    "_": [[(0.00, 1.00), (1.00, 1.00)]],
}


def main():
    from PIL import Image, ImageDraw

    out = sys.argv[1] if len(sys.argv) > 1 else "font.png"
    size = CELL * COLS
    big = Image.new("L", (size * SS, size * SS), 0)
    draw = ImageDraw.Draw(big)
    width = max(1, int(round(STROKE * SS)))

    missing = []
    for i in range(COUNT):
        ch = chr(FIRST + i)
        strokes = GLYPHS.get(ch)
        if strokes is None:
            missing.append(ch)
            continue
        ox = ((i % COLS) * CELL + GLYPH_X) * SS
        oy = ((i // COLS) * CELL + GLYPH_Y) * SS
        for poly in strokes:
            pts = [(ox + x * GLYPH_W * SS, oy + y * GLYPH_H * SS)
                   for (x, y) in poly]
            # joint="curve" rounds the corners between segments, which is what
            # turns a 24-point polygon into something the eye reads as a circle
            draw.line(pts, fill=255, width=width, joint="curve")
            # round caps: PIL has none, so put a disc on each end
            for (px, py) in (pts[0], pts[-1]):
                r = width / 2.0
                draw.ellipse([px - r, py - r, px + r, py + r], fill=255)

    small = big.resize((size, size), Image.LANCZOS)
    img = Image.new("RGBA", (size, size), (255, 255, 255, 0))
    img.putalpha(small)
    img.paste((255, 255, 255), (0, 0, size, size), None)   # RGB stays white
    img.putalpha(small)

    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    img.save(out)
    print("  %s: %dx%d, %d cells of %d, %d glyphs%s"
          % (out, size, size, COLS * COLS, CELL, COUNT - len(missing),
             ", MISSING " + " ".join(missing) if missing else ""))


if __name__ == "__main__":
    main()
