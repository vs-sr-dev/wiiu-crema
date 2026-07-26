#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Samuele Voltan
#
# Crema asset baker: turns what an artist hands you into what the console
# wants, so the Espresso does no parsing and no conversion at load time.
#
#   python tools/crema_bake.py mesh    in.obj  out.cmesh
#   python tools/crema_bake.py texture in.png  out.ctex
#   python tools/crema_bake.py pak     out.cpak  a.cmesh b.ctex ...
#   python tools/crema_bake.py bank    out.cbank audio/bank.json
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
#
# .cpak layout — the shape is dictated by what was measured on the console:
# opening a file costs ~0.15 ms, the FIRST read on a stream costs 3-4 ms
# whatever its size, and every read after that 0.74 ms. So an archive is worth
# having only if it can be loaded in a fixed number of reads no matter how many
# assets are in it, and this one takes exactly two: the header says how long the
# rest is, and the rest — directory and every payload — arrives in one go.
#    0  magic 'CPAK' | 4 version | 8 entryCount | 12 totalSize
#   16  reserved[4]                                                 = 32
#   32  entryCount x { name[32], offset, size }                     = 40 each
#       payloads, each 64-byte aligned, offsets counted from the start of file

import json
import os
import struct
import sys

VERSION = 1
BANK_VERSION = 3    # v3 carries ADPCM; the other formats are still v1
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


# .cbank layout — an instrument is a sample plus the numbers a WAV cannot carry:
# where the loop starts, how long one cycle is, and what the note does over its
# own lifetime. The cycle is what makes it an instrument rather than a noise: a
# voice playing a cycle of `cycleSamples` at rate R sounds at R/cycleSamples Hz,
# so a note is a playback rate and nothing else. Zero means "not pitched" —
# drive the rate yourself.
#
# v2 added the sixteen bytes after `flags`: attack, decay, sustain, release, and
# a vibrato. They are the difference between a note and a rectangle, and an
# instrument that leaves them all zero gets the rectangle it had in v1.
#
# v3 added ADPCM, and the only field it needed to invent was `format` — which was
# v2's `reserved:u16`, and 0 still means the LPCM16 it always meant. The rest is
# the state an AX voice must be handed to decode: eight coefficient pairs of its
# own, the first frame's header byte, and the two history samples to start from
# (zero twice, since the encoder began in silence). `sampleCount` stays a count of
# SAMPLES for both formats; the console converts to nibbles, because that
# conversion is a property of the hardware and not of the file.
#    0  magic 'CBNK' | 4 version | 8 count | 12 rate | 16 reserved[4]  = 32
#   32  count x { name[24], offset, sampleCount, loopStart, cycleSamples,
#                flags,
#                attackMs:u16, decayMs:u16, sustain:u16 (per mille),
#                releaseMs:u16, vibDelayMs:u16, vibRateMilliHz:u16,
#                vibDepthCents:s16, format:u16,
#                coefficients:s16[16], predScale:u16, yn1:s16, yn2:s16,
#                loopPredScale:u16, loopYn1:s16, loopYn2:s16 }         = 104
#       payloads, 64-byte aligned, offsets from the start of file
BANK_HEADER_SIZE = 32
BANK_ENTRY_SIZE = 104
BANK_FLAG_LOOPING = 1

BANK_FORMAT_LPCM16 = 0
BANK_FORMAT_ADPCM = 1


def read_wav_mono16(path):
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise SystemExit("%s: not a RIFF/WAVE file" % path)
    pos, fmt, pcm = 12, None, None
    while pos + 8 <= len(data):
        cid = data[pos:pos + 4]
        size = struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            fmt = struct.unpack("<HHIIHH", body[:16])
        elif cid == b"data":
            pcm = body
        pos += 8 + size + (size & 1)
    if not fmt or pcm is None:
        raise SystemExit("%s: missing fmt or data chunk" % path)
    tag, channels, rate, _, _, bits = fmt
    if tag != 1 or channels != 1 or bits != 16:
        raise SystemExit("%s: need 16-bit mono PCM, got %d-bit %d-channel "
                         "format %d" % (path, bits, channels, tag))
    return pcm, rate


def _clamp(v, lo, hi, what):
    if v < lo or v > hi:
        raise SystemExit("%s out of range: %s (expected %s..%s)"
                         % (what, v, lo, hi))
    return v


def ms(v):
    return _clamp(int(round(float(v))), 0, 65535, "a time in ms")


def permille(v):
    return _clamp(int(round(float(v) * 1000.0)), 0, 1000, "a sustain level")


def milli_hz(v):
    return _clamp(int(round(float(v) * 1000.0)), 0, 65535, "a vibrato rate")


def cents(v):
    return _clamp(int(round(float(v))), -32768, 32767, "a vibrato depth")


def encode_adpcm(inst, samples):
    """Compress one instrument and work out the state a voice must start from.

    The loop is the fiddly part and it is fiddly for a reason worth knowing: an
    ADPCM sample is not decodable on its own. It is a correction to a prediction
    made from the two samples before it, so jumping into the middle of a stream
    means telling the hardware what those two samples were and which frame header
    was in force. AX has fields for exactly that (`AXVoiceAdpcmLoopData`), and
    they are filled in here by decoding up to the loop point with the console's
    own arithmetic — not by guessing.
    """
    import adpcm

    r = adpcm.encode(samples)
    decoded = adpcm.decode(r["data"], len(samples), r["coefficients"],
                           r["predScale"])
    loop_start = int(inst.get("loopStart", 0))
    if inst.get("loop"):
        if loop_start % adpcm.FRAME_SAMPLES != 0:
            raise SystemExit(
                "%s: an ADPCM loop must start on a frame boundary (a multiple "
                "of %d samples), and %d is not one. A frame carries its own "
                "scale in a header byte, so there is nowhere else to enter it."
                % (inst["name"], adpcm.FRAME_SAMPLES, loop_start))
        header = r["data"][(loop_start // adpcm.FRAME_SAMPLES) *
                           adpcm.FRAME_BYTES]
        loop_yn1 = decoded[loop_start - 1] if loop_start >= 1 else 0
        loop_yn2 = decoded[loop_start - 2] if loop_start >= 2 else 0
    else:
        header, loop_yn1, loop_yn2 = 0, 0, 0

    return r["data"], {
        "coefficients": r["coefficients"],
        "predScale": r["predScale"],
        "yn1": r["yn1"],
        "yn2": r["yn2"],
        "loopPredScale": header,
        "loopYn1": loop_yn1,
        "loopYn2": loop_yn2,
        "snr": adpcm.snr_db(samples, decoded),
    }


def bake_bank(dst, manifest_path):
    with open(manifest_path) as fh:
        manifest = json.load(fh)
    root = os.path.dirname(os.path.abspath(manifest_path))
    rate = manifest.get("rate", 32000)

    entries = []
    offset = BANK_HEADER_SIZE + BANK_ENTRY_SIZE * len(manifest["instruments"])
    offset = (offset + PAK_ALIGN - 1) & ~(PAK_ALIGN - 1)
    for inst in manifest["instruments"]:
        pcm, wavRate = read_wav_mono16(os.path.join(root, inst["file"]))
        if wavRate != rate:
            raise SystemExit("%s: %d Hz, but the bank is %d Hz — resample it "
                             "or the pitch will be a lie"
                             % (inst["file"], wavRate, rate))
        name = inst["name"].encode("ascii")
        if len(name) > 23:
            raise SystemExit("%s: name longer than 23 characters" % inst["name"])
        samples = [v for (v,) in struct.iter_unpack("<h", pcm)]

        if inst.get("format", "lpcm16") == "adpcm":
            payload, adpcm_info = encode_adpcm(inst, samples)
        else:
            # The samples are written big-endian: the console reads them as they
            # are, and an AX voice is handed the file's own bytes. ADPCM needs no
            # swap at all — nibbles inside a byte have no byte order.
            payload = b"".join(struct.pack(">h", v) for v in samples)
            adpcm_info = None
        entries.append((name, offset, payload, inst, len(samples), adpcm_info))
        offset += (len(payload) + PAK_ALIGN - 1) & ~(PAK_ALIGN - 1)

    total = offset
    with open(dst, "wb") as fh:
        fh.write(struct.pack(">4s3I4I", b"CBNK", BANK_VERSION, len(entries),
                             rate, 0, 0, 0, 0))
        for name, off, payload, inst, count, adpcm_info in entries:
            flags = BANK_FLAG_LOOPING if inst.get("loop") else 0
            env = inst.get("envelope", {})
            vib = inst.get("vibrato", {})
            fh.write(struct.pack(">24sIIIII", name, off, count,
                                 inst.get("loopStart", 0),
                                 inst.get("cycleSamples", 0), flags))
            # Sustain is written in thousandths because a fraction in a binary
            # file is a mistake waiting to be found by someone with a hex editor.
            fh.write(struct.pack(">HHHHHHhH",
                                 ms(env.get("attack", 0)),
                                 ms(env.get("decay", 0)),
                                 permille(env.get("sustain", 0.0)),
                                 ms(env.get("release", 0)),
                                 ms(vib.get("delay", 0)),
                                 milli_hz(vib.get("rate", 0.0)),
                                 cents(vib.get("depth", 0)),
                                 BANK_FORMAT_ADPCM if adpcm_info
                                 else BANK_FORMAT_LPCM16))
            info = adpcm_info or {"coefficients": [0] * 16, "predScale": 0,
                                  "yn1": 0, "yn2": 0, "loopPredScale": 0,
                                  "loopYn1": 0, "loopYn2": 0}
            fh.write(struct.pack(">16h", *info["coefficients"]))
            fh.write(struct.pack(">HhhHhh", info["predScale"],
                                 info["yn1"], info["yn2"],
                                 info["loopPredScale"],
                                 info["loopYn1"], info["loopYn2"]))
        for name, off, payload, inst, count, adpcm_info in entries:
            fh.write(b"\0" * (off - fh.tell()))
            fh.write(payload)
        fh.write(b"\0" * (total - fh.tell()))

    for name, off, payload, inst, count, adpcm_info in entries:
        env, vib = inst.get("envelope"), inst.get("vibrato")
        shape = ""
        if env:
            shape = "  adsr %d/%d/%.2f/%d" % (env.get("attack", 0),
                                              env.get("decay", 0),
                                              env.get("sustain", 0.0),
                                              env.get("release", 0))
        if vib and vib.get("depth"):
            shape += "  vib %.1f Hz %+d cents after %d ms" % (
                vib.get("rate", 0.0), vib["depth"], vib.get("delay", 0))
        if adpcm_info:
            # The number that decides whether ADPCM was a good idea for this
            # instrument, printed at every build so the decision stays a
            # measurement instead of becoming a habit.
            shape += "  ADPCM %d B, %.2f:1, SNR %.1f dB" % (
                len(payload), count * 2.0 / len(payload), adpcm_info["snr"])
        print("    %-10s %7d samples%s%s%s" % (
            name.decode(), count,
            "  loop@%d" % inst.get("loopStart", 0) if inst.get("loop") else "",
            "  cycle %d" % inst["cycleSamples"] if inst.get("cycleSamples") else "",
            shape))
    print("  %s: %d instruments, %d Hz, %d bytes"
          % (dst, len(entries), rate, total))


PAK_HEADER_SIZE = 32
PAK_ENTRY_SIZE = 40
PAK_ALIGN = 64


def bake_pak(dst, sources):
    """Pack already-baked assets into one archive. Nothing is transformed here
    — a .cpak is a directory and a concatenation, because the console's problem
    was never the bytes, it was touching the files."""
    entries = []
    offset = PAK_HEADER_SIZE + PAK_ENTRY_SIZE * len(sources)
    offset = (offset + PAK_ALIGN - 1) & ~(PAK_ALIGN - 1)

    for src in sources:
        name = os.path.basename(src).encode("ascii")
        if len(name) > 31:
            raise SystemExit("%s: name longer than 31 characters" % src)
        with open(src, "rb") as fh:
            data = fh.read()
        entries.append((name, offset, data))
        offset += (len(data) + PAK_ALIGN - 1) & ~(PAK_ALIGN - 1)

    total = offset
    with open(dst, "wb") as fh:
        fh.write(struct.pack(">4s3I4I", b"CPAK", VERSION, len(entries), total,
                             0, 0, 0, 0))
        for name, off, data in entries:
            fh.write(struct.pack(">32sII", name, off, len(data)))
        for name, off, data in entries:
            fh.write(b"\0" * (off - fh.tell()))
            fh.write(data)
        fh.write(b"\0" * (total - fh.tell()))

    for name, off, data in entries:
        print("    %-20s %8d bytes @ %d" % (name.decode(), len(data), off))
    print("  %s: %d entries, %d bytes, loads in 2 reads"
          % (dst, len(entries), total))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    flags = set(a for a in sys.argv[1:] if a.startswith("--"))
    usage = ("usage: crema_bake.py {mesh|texture} <input> <output> [--no-mips]\n"
             "       crema_bake.py pak <output.cpak> <input> [<input> ...]\n"
             "       crema_bake.py bank <output.cbank> <bank.json>")
    if len(args) < 3 or args[0] not in ("mesh", "texture", "pak", "bank"):
        print(__doc__ or "")
        print(usage)
        raise SystemExit(2)

    kind = args[0]
    if kind == "pak":
        dst, sources = args[1], args[2:]
        os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
        print("packing %d assets" % len(sources))
        bake_pak(dst, sources)
        return

    if len(args) != 3:
        print(usage)
        raise SystemExit(2)
    if kind == "bank":
        dst, src = args[1], args[2]
        os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
        print("baking bank: %s" % src)
        bake_bank(dst, src)
        return
    src, dst = args[1], args[2]
    os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
    print("baking %s: %s" % (kind, src))
    if kind == "mesh":
        bake_mesh(src, dst)
    else:
        bake_texture(src, dst, mips="--no-mips" not in flags)


if __name__ == "__main__":
    main()
