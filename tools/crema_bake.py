#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Samuele Voltan
#
# Crema asset baker: turns what an artist hands you into what the console
# wants, so the Espresso does no parsing and no conversion at load time.
#
#   python tools/crema_bake.py mesh    in.obj  out.cmesh
#   python tools/crema_bake.py texture in.png  out.ctex
#
# Everything is written BIG-ENDIAN on purpose: the Wii U CPU is big-endian and
# the GX2 fetch shader swaps attribute words for the GPU, so a baked file can
# be read straight into a GX2R buffer with no byteswap anywhere on the console.
#
# .cmesh layout (offsets in bytes)
#    0  magic 'CMSH' | 4 version | 8 vertexCount | 12 indexCount
#   16  vertexStride | 20 attribCount | 24 aabbMin[3] | 36 aabbMax[3]
#   48  vertexOffset | 52 indexOffset | 56 reserved[2]              = 64
#   64  attribCount x { location, offset, type }                    = 12 each
#       vertex blob (vertexCount * stride), then U16 index blob
#
# .ctex layout
#    0  magic 'CTEX' | 4 version | 8 width | 12 height | 16 mipLevels
#   20  format (1 = RGBA8) | 24 dataOffset | 28 reserved            = 32
#   32  every mip level, tightly packed, largest first

import os
import struct
import sys

VERSION = 1
HEADER_SIZE = 64
TEX_HEADER_SIZE = 32

ATTRIB_FLOAT2 = 1
ATTRIB_FLOAT3 = 2
ATTRIB_FLOAT4 = 3
ATTRIB_UNORM8x4 = 4

ATTRIB_SIZE = {
    ATTRIB_FLOAT2: 8,
    ATTRIB_FLOAT3: 12,
    ATTRIB_FLOAT4: 16,
    ATTRIB_UNORM8x4: 4,
}


# --- OBJ -> .cmesh -----------------------------------------------------------

def parse_obj(path):
    positions, uvs, normals = [], [], []
    faces = []
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            parts = line.split()
            if not parts:
                continue
            tag = parts[0]
            if tag == "v":
                positions.append(tuple(float(v) for v in parts[1:4]))
            elif tag == "vt":
                uvs.append(tuple(float(v) for v in parts[1:3]))
            elif tag == "vn":
                normals.append(tuple(float(v) for v in parts[1:4]))
            elif tag == "f":
                corners = []
                for token in parts[1:]:
                    bits = (token.split("/") + ["", ""])[:3]
                    vi = int(bits[0])
                    ti = int(bits[1]) if bits[1] else 0
                    ni = int(bits[2]) if bits[2] else 0
                    corners.append((vi, ti, ni))
                # fan-triangulate n-gons
                for k in range(1, len(corners) - 1):
                    faces.append((corners[0], corners[k], corners[k + 1]))
    return positions, uvs, normals, faces


def deref(values, index, fallback):
    if index == 0 or not values:
        return fallback
    return values[index - 1] if index > 0 else values[index]


def check_winding(vertices, indices):
    """Faces must be CCW seen from outside — get it wrong and back-face culling
    makes the model see-through, a bug that is far cheaper to catch here than
    on the console.

    Comparing each normal against the mesh centre only works for convex blobs;
    it lies about wings and any flat slab sitting off-centre. So do it properly:
    split the mesh into connected components (by welded position, since flat
    shading splits vertices), check each one is closed, and take the signed
    volume — which is positive exactly when a closed surface faces outwards,
    whatever its shape."""
    pos_ids = {}
    tris = []
    for i in range(0, len(indices), 3):
        ids = []
        for k in range(3):
            v = vertices[indices[i + k]]
            key = (round(v[0], 5), round(v[1], 5), round(v[2], 5))
            if key not in pos_ids:
                pos_ids[key] = len(pos_ids)
            ids.append(pos_ids[key])
        tris.append(tuple(ids))

    points = [None] * len(pos_ids)
    for key, idx in pos_ids.items():
        points[idx] = key

    parent = list(range(len(tris)))

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    def union(a, b):
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[rb] = ra

    edges = {}
    for t, (a, b, c) in enumerate(tris):
        for u, v in ((a, b), (b, c), (c, a)):
            edges.setdefault((min(u, v), max(u, v)), []).append(t)
    for shared in edges.values():
        for t in shared[1:]:
            union(shared[0], t)

    groups = {}
    for t in range(len(tris)):
        groups.setdefault(find(t), []).append(t)

    problems = []
    for members in groups.values():
        used = {}
        volume = 0.0
        for t in members:
            a, b, c = tris[t]
            for u, v in ((a, b), (b, c), (c, a)):
                used[(min(u, v), max(u, v))] = used.get((min(u, v), max(u, v)), 0) + 1
            pa, pb, pc = points[a], points[b], points[c]
            cx = pb[1] * pc[2] - pb[2] * pc[1]
            cy = pb[2] * pc[0] - pb[0] * pc[2]
            cz = pb[0] * pc[1] - pb[1] * pc[0]
            volume += (pa[0] * cx + pa[1] * cy + pa[2] * cz) / 6.0
        open_edges = sum(1 for n in used.values() if n != 2)
        if open_edges:
            problems.append("%d tris: not closed (%d loose edges)"
                            % (len(members), open_edges))
        elif volume < 0:
            problems.append("%d tris: inside-out (volume %.3f)"
                            % (len(members), volume))
    return len(groups), problems


def bake_mesh(src, dst):
    positions, uvs, normals, faces = parse_obj(src)
    if not faces:
        raise SystemExit("%s: no faces found" % src)

    layout = [
        (0, 0, ATTRIB_FLOAT3),    # location 0: position
        (1, 12, ATTRIB_FLOAT3),   # location 1: normal
        (2, 24, ATTRIB_FLOAT2),   # location 2: uv
    ]
    stride = 32

    unique = {}
    vertices = []     # (px,py,pz, nx,ny,nz, u,v)
    indices = []
    for face in faces:
        for vi, ti, ni in face:
            pos = deref(positions, vi, (0.0, 0.0, 0.0))
            nrm = deref(normals, ni, (0.0, 1.0, 0.0))
            uv = deref(uvs, ti, (0.0, 0.0))
            key = (pos, nrm, uv)
            index = unique.get(key)
            if index is None:
                index = len(vertices)
                unique[key] = index
                vertices.append(pos + nrm + uv)
            indices.append(index)

    if len(vertices) > 65535:
        raise SystemExit("%s: %d vertices exceeds the U16 index limit"
                         % (src, len(vertices)))

    aabb_min = [min(v[i] for v in vertices) for i in range(3)]
    aabb_max = [max(v[i] for v in vertices) for i in range(3)]

    parts, problems = check_winding(vertices, indices)
    if problems:
        print("  %d parts, %d with problems:" % (parts, len(problems)))
        for line in problems:
            print("    WARNING: " + line)
    else:
        print("  %d parts, all closed and facing outward" % parts)

    vertex_blob = b"".join(struct.pack(">8f", *v) for v in vertices)
    index_blob = struct.pack(">%dH" % len(indices), *indices)

    attrib_bytes = b"".join(struct.pack(">3I", loc, off, typ)
                            for loc, off, typ in layout)
    vertex_offset = HEADER_SIZE + len(attrib_bytes)
    # keep the vertex blob 64-byte aligned: it is copied straight into GPU memory
    pad = (-vertex_offset) % 64
    vertex_offset += pad
    index_offset = vertex_offset + len(vertex_blob)

    header = struct.pack(
        ">4s5I3f3f2I2I",
        b"CMSH", VERSION, len(vertices), len(indices), stride, len(layout),
        aabb_min[0], aabb_min[1], aabb_min[2],
        aabb_max[0], aabb_max[1], aabb_max[2],
        vertex_offset, index_offset, 0, 0)
    assert len(header) == HEADER_SIZE, len(header)

    with open(dst, "wb") as fh:
        fh.write(header)
        fh.write(attrib_bytes)
        fh.write(b"\0" * pad)
        fh.write(vertex_blob)
        fh.write(index_blob)

    print("  %s: %d verts, %d tris, stride %d, %d bytes"
          % (dst, len(vertices), len(indices) // 3, stride,
             os.path.getsize(dst)))
    print("  bounds: [%.2f %.2f %.2f] .. [%.2f %.2f %.2f]"
          % (*aabb_min, *aabb_max))


# --- PNG -> .ctex ------------------------------------------------------------

def box_reduce(pixels, w, h):
    dw = max(1, w // 2)
    dh = max(1, h // 2)
    out = bytearray(dw * dh * 4)
    for y in range(dh):
        y0 = min(2 * y, h - 1)
        y1 = min(2 * y + 1, h - 1)
        for x in range(dw):
            x0 = min(2 * x, w - 1)
            x1 = min(2 * x + 1, w - 1)
            for c in range(4):
                total = (pixels[(y0 * w + x0) * 4 + c] +
                         pixels[(y0 * w + x1) * 4 + c] +
                         pixels[(y1 * w + x0) * 4 + c] +
                         pixels[(y1 * w + x1) * 4 + c])
                out[(y * dw + x) * 4 + c] = total // 4
    return bytes(out), dw, dh


def bake_texture(src, dst, mips=True):
    from PIL import Image

    img = Image.open(src).convert("RGBA")
    w, h = img.size
    if w & (w - 1) or h & (h - 1):
        raise SystemExit("%s: %dx%d is not power-of-two" % (src, w, h))

    levels = [bytes(img.tobytes())]
    # Mips are mandatory for anything the world minifies (lesson 6), and wrong
    # for a font atlas: a HUD glyph is drawn at roughly its own size, and a
    # smaller level would only ever arrive as blur — or worse, bleed the
    # neighbouring glyph into it once the cells are 4 pixels wide.
    lw, lh = w, h
    while mips and (lw > 1 or lh > 1):
        data, lw, lh = box_reduce(levels[-1], lw, lh)
        levels.append(data)

    header = struct.pack(">4s6I I", b"CTEX", VERSION, w, h, len(levels), 1,
                         TEX_HEADER_SIZE, 0)
    assert len(header) == TEX_HEADER_SIZE, len(header)

    with open(dst, "wb") as fh:
        fh.write(header)
        for data in levels:
            fh.write(data)

    print("  %s: %dx%d, %d mip levels, %d bytes"
          % (dst, w, h, len(levels), os.path.getsize(dst)))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    flags = set(a for a in sys.argv[1:] if a.startswith("--"))
    if len(args) != 3 or args[0] not in ("mesh", "texture"):
        print(__doc__ or "")
        print("usage: crema_bake.py {mesh|texture} <input> <output> [--no-mips]")
        raise SystemExit(2)

    kind, src, dst = args
    os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
    print("baking %s: %s" % (kind, src))
    if kind == "mesh":
        bake_mesh(src, dst)
    else:
        bake_texture(src, dst, mips="--no-mips" not in flags)


if __name__ == "__main__":
    main()
