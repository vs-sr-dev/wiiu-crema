#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Samuele Voltan
"""Nintendo DSP-ADPCM: the encoder, and the decoder that keeps it honest.

Four bits a sample instead of sixteen, so 4:1, and the reason it is worth having
at all is that **the Wii U decodes it in hardware**. An AX voice set to
AX_VOICE_FORMAT_ADPCM costs the DSP the same as an LPCM16 one; the compression is
free at playback and paid for entirely here.

The format, which is not written down in wut and was read out of Cemu's decoder
(`src/Cafe/OS/libs/snd_core/ax_mix.cpp`, AX_readADPCMSamples):

  * A **frame is 8 bytes and holds 14 samples.** One header byte, then seven
    bytes of two nibbles each.
  * The header byte is `predictor << 4 | scale`. The scale is a shift: the
    quantisation step of that frame is 2^scale. The predictor selects one of
    **eight** coefficient pairs — the decoder masks the high nibble with 7, so a
    ninth would silently be the first.
  * The coefficients are **per voice, not per frame**: sixteen int16 in Q11 that
    AX is handed once. So an instrument gets eight second-order predictors of its
    own, chosen for its own material, and each frame says which one fits.
  * Decoding one sample:

        v  = (signed_nibble << 11) * 2^scale
        v += yn1*a1 + yn2*a2
        v  = (v + 1024) >> 11          # Q11 back to whole samples
        clamp to int16, then shift it into the history

  * **Offsets are counted in nibbles**, and the header nibbles count. Sample `s`
    lives at nibble `(s // 14) * 16 + (s % 14) + 2`. Getting this wrong is the
    single easiest way to make a voice play static, because everything else about
    the setup looks right.

What the encoder does, in the order the quality depends on:

  1. Fit a second-order predictor to every frame by least squares. That gives one
     candidate (a1, a2) per frame — hundreds of them, and only eight may survive.
  2. Cluster the candidates into eight with k-means, seeded from the data's own
     quantiles so the result is deterministic. This is the step people skip, and
     skipping it is why a fixed coefficient table sounds worse: a pulse wave and
     an explosion want different predictors, and the format lets each instrument
     have its own.
  3. For every frame, try all eight against the smallest scale that does not
     clamp, and keep the best by squared error. **Closed loop**: the history fed
     to the predictor is what the decoder will actually reconstruct, not the
     original signal, so quantisation error cannot accumulate unseen.
  4. Decode the result with the same arithmetic the console uses and report the
     signal-to-noise ratio. A compressor that cannot tell you what it cost is a
     compressor you have to trust.
"""

FRAME_SAMPLES = 14
FRAME_BYTES = 8
NIBBLES_PER_FRAME = 16
COEF_PAIRS = 8


def clamp16(v):
    return -32768 if v < -32768 else (32767 if v > 32767 else v)


def nibble_offset(sample):
    """Where AX must be told a sample is. See the note above about the two
    header nibbles: this is the whole reason `currentOffset` starts at 2."""
    return (sample // FRAME_SAMPLES) * NIBBLES_PER_FRAME + \
           (sample % FRAME_SAMPLES) + 2


def _decode_one(nib, scale, a1, a2, yn1, yn2):
    v = (nib << 11) * (1 << scale)
    v += yn1 * a1 + yn2 * a2
    return clamp16((v + 0x400) >> 11)


# --- the predictor a frame would like ----------------------------------------

def _fit_frame(prev2, prev1, frame):
    """Least squares a1, a2 in Q11 for x[n] = a1*x[n-1] + a2*x[n-2]."""
    hist = [prev2, prev1] + list(frame)
    r0 = r1 = r2 = b1 = b2 = 0
    for n in range(2, len(hist)):
        x, x1, x2 = hist[n], hist[n - 1], hist[n - 2]
        r0 += x1 * x1
        r1 += x1 * x2
        r2 += x2 * x2
        b1 += x * x1
        b2 += x * x2
    det = r0 * r2 - r1 * r1
    if det == 0:
        # A silent or constant frame has nothing to say about prediction. Not an
        # error, and not a reason to invent a number: it abstains.
        return None
    a1 = (b1 * r2 - b2 * r1) / det
    a2 = (b2 * r0 - b1 * r1) / det
    return (a1 * 2048.0, a2 * 2048.0)


def _kmeans(points, k=COEF_PAIRS, rounds=12):
    """Deterministic on purpose — a build that produces different bytes on
    Tuesday is a build nobody can bisect."""
    if not points:
        return [(0.0, 0.0)] * k
    if len(points) <= k:
        return list(points) + [points[-1]] * (k - len(points))

    ordered = sorted(points)
    centres = [ordered[(i * (len(ordered) - 1)) // (k - 1)] for i in range(k)]
    for _ in range(rounds):
        sums = [[0.0, 0.0, 0] for _ in range(k)]
        for (x, y) in points:
            best, bestd = 0, None
            for i, (cx, cy) in enumerate(centres):
                d = (x - cx) ** 2 + (y - cy) ** 2
                if bestd is None or d < bestd:
                    best, bestd = i, d
            sums[best][0] += x
            sums[best][1] += y
            sums[best][2] += 1
        moved = False
        for i, (sx, sy, n) in enumerate(sums):
            if n == 0:
                continue          # keep an empty cluster where it was
            new = (sx / n, sy / n)
            if new != centres[i]:
                centres[i] = new
                moved = True
        if not moved:
            break
    return centres


def _coefficients(samples):
    candidates = []
    for base in range(0, len(samples), FRAME_SAMPLES):
        frame = samples[base:base + FRAME_SAMPLES]
        p2 = samples[base - 2] if base >= 2 else 0
        p1 = samples[base - 1] if base >= 1 else 0
        fit = _fit_frame(p2, p1, frame)
        if fit is not None:
            candidates.append(fit)

    pairs = []
    for (a1, a2) in _kmeans(candidates):
        # Q11 int16, and kept inside ±4.0 so a predictor cannot be wildly
        # unstable. The decoder clamps every sample so it could not run away
        # anyway, but a predictor that spends its life clamped is a predictor
        # that predicts nothing.
        pairs.append((max(-8192, min(8191, int(round(a1)))),
                      max(-8192, min(8191, int(round(a2))))))
    return pairs


# --- encoding -----------------------------------------------------------------

def _encode_frame(frame, a1, a2, scale, yn1, yn2):
    """One frame with one predictor at one scale. Returns (nibbles, error,
    clamped, yn1, yn2) where `clamped` means the step was too small to reach the
    signal — which is the test that chooses the scale."""
    nibs = []
    err = 0
    clamped = False
    step = 1 << scale
    for target in frame:
        pred = yn1 * a1 + yn2 * a2
        # What nibble comes closest to the sample the decoder would need.
        num = (target << 11) - pred
        den = 2048 * step
        nib = int(round(num / den))
        if nib > 7:
            nib, clamped = 7, True
        elif nib < -8:
            nib, clamped = -8, True
        got = _decode_one(nib, scale, a1, a2, yn1, yn2)
        d = target - got
        err += d * d
        yn2, yn1 = yn1, got
        nibs.append(nib)
    return nibs, err, clamped, yn1, yn2


def encode(samples):
    """PCM16 in, a dict out: data, coefficients, predScale, yn1, yn2 and stats.

    yn1/yn2 are the state a voice must START from, which is zero twice: the
    encoder began with silence in front of the first sample, so a voice that
    begins any other way decodes the first frame differently from the way it was
    encoded."""
    samples = list(samples)
    if not samples:
        raise ValueError("nothing to encode")

    # The last frame is filled out with its own last sample rather than with
    # zeros: the padding is never played (the end offset stops at the real final
    # sample) but a step down to silence would force that frame a scale wider
    # than it needs, and pay for it in the samples that ARE played.
    pad = (-len(samples)) % FRAME_SAMPLES
    if pad:
        samples += [samples[-1]] * pad

    pairs = _coefficients(samples)

    out = bytearray()
    yn1 = yn2 = 0
    total_err = 0
    first_header = None
    scale_hist = [0] * 16
    pred_hist = [0] * COEF_PAIRS

    for base in range(0, len(samples), FRAME_SAMPLES):
        frame = samples[base:base + FRAME_SAMPLES]
        best = None
        for p, (a1, a2) in enumerate(pairs):
            for scale in range(16):
                nibs, err, clamped, n1, n2 = _encode_frame(frame, a1, a2, scale,
                                                           yn1, yn2)
                if clamped and scale < 15:
                    continue      # the step cannot reach the signal: widen it
                if best is None or err < best[0]:
                    best = (err, p, scale, nibs, n1, n2)
                break             # the smallest scale that fits is the one
        err, p, scale, nibs, yn1, yn2 = best
        total_err += err
        scale_hist[scale] += 1
        pred_hist[p] += 1

        header = (p << 4) | scale
        if first_header is None:
            first_header = header
        out.append(header)
        for i in range(0, FRAME_SAMPLES, 2):
            hi = nibs[i] & 0xF
            lo = nibs[i + 1] & 0xF
            out.append((hi << 4) | lo)

    coefs = []
    for (a1, a2) in pairs:
        coefs += [a1, a2]
    return {
        "data": bytes(out),
        "coefficients": coefs,
        "predScale": first_header,
        "yn1": 0,
        "yn2": 0,
        "error": total_err,
        "scales": scale_hist,
        "predictors": pred_hist,
    }


def decode(data, count, coefs, pred_scale, yn1=0, yn2=0):
    """The console's arithmetic, so a claim about quality is a measurement.

    Deliberately walks nibbles rather than frames, because that is what the DSP
    does and it is the only way this also verifies the offset arithmetic."""
    out = []
    for s in range(count):
        nib_off = nibble_offset(s)
        if nib_off % NIBBLES_PER_FRAME == 2:
            header = data[(nib_off // NIBBLES_PER_FRAME) * FRAME_BYTES]
            scale = header & 0xF
            p = (header >> 4) & 7
            a1, a2 = coefs[p * 2], coefs[p * 2 + 1]
        byte = data[nib_off >> 1]
        nib = (byte >> 4) if (nib_off & 1) == 0 else (byte & 0xF)
        if nib >= 8:
            nib -= 16
        v = _decode_one(nib, scale, a1, a2, yn1, yn2)
        yn2, yn1 = yn1, v
        out.append(v)
    return out


def snr_db(original, decoded):
    import math
    sig = sum(float(s) * s for s in original)
    noise = sum((float(a) - b) ** 2 for a, b in zip(original, decoded))
    if noise == 0:
        return float("inf")
    if sig == 0:
        return 0.0
    return 10.0 * math.log10(sig / noise)
