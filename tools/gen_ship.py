#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Samuele Voltan
#
# Generates the first real asset for the Crema pipeline: a low-poly ship,
# home-grown so the repository stays clean-room, plus a hull texture to go
# with it. Output is plain .obj + .png — the formats an artist would hand you,
# so the bake path is the same one real content will take.
#
#   python tools/gen_ship.py examples/poc10-mesh/assets
#
# Conventions match the rest of Crema: Y up, -Z forward, faces CCW seen from
# outside (the culling front face).

import math
import os
import sys

# --- geometry helpers --------------------------------------------------------

class Mesh:
    def __init__(self):
        self.tris = []          # (p0, p1, p2), each p = (x, y, z)

    def tri(self, a, b, c):
        # skip degenerates (the nose cap loft produces a few)
        n = normal(a, b, c)
        if n is not None:
            self.tris.append((a, b, c))

    def quad(self, a, b, c, d):
        self.tri(a, b, c)
        self.tri(a, c, d)


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


def normal(a, b, c):
    n = cross(sub(b, a), sub(c, a))
    length = math.sqrt(n[0] ** 2 + n[1] ** 2 + n[2] ** 2)
    if length < 1e-9:
        return None
    return (n[0] / length, n[1] / length, n[2] / length)


def loft(mesh, sections):
    """sections: list of (z, [(x, y), ...]) rings, front to back, same length.

    Ring points run counter-clockwise in XY seen from +Z (behind the ship),
    which makes the side quads face outwards."""
    for (z0, ring0), (z1, ring1) in zip(sections, sections[1:]):
        n = len(ring0)
        for i in range(n):
            j = (i + 1) % n
            a0 = (ring0[i][0], ring0[i][1], z0)
            a1 = (ring0[j][0], ring0[j][1], z0)
            b1 = (ring1[j][0], ring1[j][1], z1)
            b0 = (ring1[i][0], ring1[i][1], z1)
            mesh.quad(a0, a1, b1, b0)


def cap(mesh, z, ring, facing_back):
    """Flat polygon closing a ring, normal along +Z (back) or -Z (front)."""
    cx = sum(p[0] for p in ring) / len(ring)
    cy = sum(p[1] for p in ring) / len(ring)
    centre = (cx, cy, z)
    n = len(ring)
    for i in range(n):
        j = (i + 1) % n
        a = (ring[i][0], ring[i][1], z)
        b = (ring[j][0], ring[j][1], z)
        if facing_back:
            mesh.tri(centre, a, b)
        else:
            mesh.tri(centre, b, a)


def signed_area_xz(points):
    total = 0.0
    for i in range(len(points)):
        x0, z0 = points[i]
        x1, z1 = points[(i + 1) % len(points)]
        total += x0 * z1 - x1 * z0
    return total * 0.5


def extrude_xz(mesh, points, y_lo, y_hi):
    """Extrude a flat XZ polygon into a slab. Ordering is normalised so the
    top face ends up pointing +Y whatever order the caller passed in."""
    # CCW in the (x, z) plane gives a -Y normal, so the top face wants CW.
    pts = list(points)
    if signed_area_xz(pts) > 0:
        pts.reverse()

    top = [(p[0], y_hi, p[1]) for p in pts]
    bot = [(p[0], y_lo, p[1]) for p in pts]

    for i in range(1, len(pts) - 1):
        mesh.tri(top[0], top[i], top[i + 1])          # +Y
        mesh.tri(bot[0], bot[i + 1], bot[i])          # -Y

    for i in range(len(pts)):
        j = (i + 1) % len(pts)
        mesh.quad(top[i], bot[i], bot[j], top[j])     # sides, facing outwards


def hex_ring(rx, ry, squash=0.72):
    """Six points, counter-clockwise in XY seen from +Z."""
    ring = []
    for k in range(6):
        a = math.radians(30 + 60 * k)
        ring.append((rx * math.cos(a), ry * squash * math.sin(a)))
    return ring


# --- the ship ----------------------------------------------------------------

def build_ship():
    m = Mesh()

    # fuselage: hexagonal loft from a nose point back to the engine deck
    profile = [
        (-3.40, 0.02), (-2.60, 0.22), (-1.60, 0.42),
        (-0.30, 0.52), (1.00, 0.48), (2.10, 0.40),
    ]
    sections = [(z, hex_ring(r, r)) for z, r in profile]
    loft(m, sections)
    cap(m, profile[0][0], sections[0][1], facing_back=False)     # nose
    cap(m, profile[-1][0], sections[-1][1], facing_back=True)    # engine deck

    # cockpit blister, a smaller loft riding on the spine
    canopy = [
        (-1.90, 0.05), (-1.40, 0.20), (-0.70, 0.26), (0.10, 0.18), (0.45, 0.04),
    ]
    canopy_sections = []
    for z, r in canopy:
        ring = [(x, y * 0.62 + 0.34) for x, y in hex_ring(r, r, squash=1.0)]
        canopy_sections.append((z, ring))
    loft(m, canopy_sections)
    cap(m, canopy_sections[0][0], canopy_sections[0][1], facing_back=False)
    cap(m, canopy_sections[-1][0], canopy_sections[-1][1], facing_back=True)

    # wings: swept planform in XZ, extruded thin. Mirrored for the other side.
    wing = [(0.35, -0.60), (2.65, 1.05), (2.65, 1.60), (0.35, 0.95)]
    for side in (1.0, -1.0):
        extrude_xz(m, [(x * side, z) for x, z in wing], -0.09, 0.09)

    # wingtip fins, angled up at the ends of each wing
    fin = [(2.05, 0.95), (2.70, 1.05), (2.70, 1.62), (2.05, 1.55)]
    for side in (1.0, -1.0):
        pts = [(x * side, z) for x, z in fin]
        extrude_xz(m, pts, 0.05, 0.95)

    # engine nacelles hanging off the back of the fuselage
    for side in (1.0, -1.0):
        nozzle = [
            (0.55, 1.40), (1.15, 1.40), (1.15, 2.45), (0.55, 2.45),
        ]
        extrude_xz(m, [(x * side, z) for x, z in nozzle], -0.28, 0.28)

    # dorsal tail fin
    tail = [(-0.10, 0.90), (0.10, 0.90), (0.10, 2.20), (-0.10, 2.20)]
    extrude_xz(m, tail, 0.30, 1.35)

    return m


# --- planar UVs and OBJ output ----------------------------------------------

def face_uvs(tri, scale=0.42):
    """Project onto the plane the face points at most: cheap, seamless enough
    for a panel texture, and it needs no unwrapping tool."""
    n = normal(*tri)
    ax, ay, az = abs(n[0]), abs(n[1]), abs(n[2])
    uvs = []
    for p in tri:
        if ax >= ay and ax >= az:
            u, v = p[2], p[1]
        elif ay >= az:
            u, v = p[0], p[2]
        else:
            u, v = p[0], p[1]
        uvs.append((u * scale, v * scale))
    return uvs


def write_obj(mesh, path):
    lines = ["# Crema ship - generated by tools/gen_ship.py, MIT",
             "o ship"]
    verts, norms, texs = [], [], []
    faces = []
    for tri in mesh.tris:
        n = normal(*tri)
        uvs = face_uvs(tri)
        norms.append(n)
        ni = len(norms)
        idx = []
        for p, uv in zip(tri, uvs):
            verts.append(p)
            texs.append(uv)
            idx.append((len(verts), len(texs), ni))
        faces.append(idx)

    for p in verts:
        lines.append("v %.6f %.6f %.6f" % p)
    for t in texs:
        lines.append("vt %.6f %.6f" % t)
    for n in norms:
        lines.append("vn %.6f %.6f %.6f" % n)
    for f in faces:
        lines.append("f " + " ".join("%d/%d/%d" % i for i in f))

    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines) + "\n")
    return len(mesh.tris), len(verts)


# --- hull texture ------------------------------------------------------------

def write_hull_texture(path, size=256):
    from PIL import Image, ImageDraw

    img = Image.new("RGBA", (size, size), (150, 156, 168, 255))
    d = ImageDraw.Draw(img)

    # deterministic mottling, no RNG seed to remember
    for y in range(size):
        for x in range(size):
            n = ((x * 37 + y * 61) % 23) - 11
            r, g, b, a = img.getpixel((x, y))
            img.putpixel((x, y), (r + n, g + n, b + n, a))

    # panel lines on a 64px grid, with a lighter bevel above each seam
    for i in range(0, size, 64):
        d.line([(i, 0), (i, size)], fill=(96, 101, 112, 255), width=2)
        d.line([(0, i), (size, i)], fill=(96, 101, 112, 255), width=2)
        d.line([(i + 2, 0), (i + 2, size)], fill=(178, 184, 196, 255))
        d.line([(0, i + 2), (size, i + 2)], fill=(178, 184, 196, 255))

    # a squadron stripe and a couple of hatches to break the repetition
    d.rectangle([16, 96, 240, 128], fill=(196, 72, 48, 255))
    d.rectangle([16, 96, 240, 100], fill=(224, 120, 92, 255))
    d.ellipse([176, 176, 224, 224], outline=(88, 92, 104, 255), width=3)
    d.ellipse([188, 188, 212, 212], fill=(120, 126, 140, 255))

    # rivets along the seams
    for i in range(0, size, 64):
        for k in range(8, size, 16):
            d.point((i + 5, k), fill=(84, 88, 98, 255))
            d.point((k, i + 5), fill=(84, 88, 98, 255))

    img.save(path)


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "assets"
    os.makedirs(out_dir, exist_ok=True)

    ship = build_ship()
    obj_path = os.path.join(out_dir, "ship.obj")
    tris, verts = write_obj(ship, obj_path)
    print("%s: %d triangles, %d vertices" % (obj_path, tris, verts))

    tex_path = os.path.join(out_dir, "hull.png")
    write_hull_texture(tex_path)
    print("%s: 256x256 hull panels" % tex_path)


if __name__ == "__main__":
    main()
