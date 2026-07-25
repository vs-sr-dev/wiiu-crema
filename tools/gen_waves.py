#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Samuele Voltan
"""Generate Crema's chip instruments and sound effects as WAV + a manifest.

The premise, worth stating because everything here follows from it: **a sampler
with pitch and loop control IS a sound chip.** A pulse wave is one cycle in a
loop with the duty already baked into it; a NES triangle is its sixteen steps
written out exactly; noise is an LFSR sequence generated once, offline. Each of
those maps onto an AX voice with nothing left over — and unlike the chip it
imitates, you are not limited to four of them.

So the instruments are made here, on the PC, where a wrong number is a rerun
rather than a rebuild. What comes out is ordinary 16-bit mono WAV, which means
the preview tool is whatever plays WAVs on your machine, plus a bank.json that
carries what a WAV cannot: where the loop starts, and how many samples make one
cycle of a pitched instrument.

That last number is what turns a sample into an instrument. A voice playing a
32-sample cycle at rate R sounds at R/32 Hz, so a note is just a playback rate:
    ratio = frequency * cycleSamples / sampleRate
Which is why the cycles are short — 32 samples at 32 kHz sits at 1000 Hz, and
the notes a game needs land either side of it without the resampler running out
of range in either direction.

    python tools/gen_waves.py examples/poc11-flight/assets/audio
"""

import json
import math
import os
import struct
import sys

RATE = 32000        # half of the renderer's 48 kHz: half the memory, and it
                    # keeps the resampler on the critical path from frame one
CYCLE = 32          # samples in one cycle of a pitched instrument


def clip16(v):
    if v > 1.0:
        v = 1.0
    if v < -1.0:
        v = -1.0
    return int(v * 32000.0)


def write_wav(path, samples, rate=RATE):
    data = b"".join(struct.pack("<h", s) for s in samples)
    with open(path, "wb") as fh:
        fh.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE")
        fh.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate,
                                       rate * 2, 2, 16))
        fh.write(b"data" + struct.pack("<I", len(data)) + data)


# --- the chip instruments ----------------------------------------------------

def pulse(duty):
    """One cycle. The duty is baked in, which is the whole trick: the DSP only
    has to loop it, and the timbre came for free from the shape."""
    return [clip16(0.62 if (i / CYCLE) < duty else -0.62) for i in range(CYCLE)]


def triangle():
    """The NES triangle's steps, exactly. It is not a smooth ramp and never was
    — the stair-step is the sound, and rounding it off would be an improvement
    nobody asked for."""
    out = []
    for i in range(CYCLE):
        step = i if i < CYCLE // 2 else (CYCLE - 1 - i)      # 0..15..0
        out.append(clip16((step / (CYCLE // 2 - 1)) * 1.24 - 0.62))
    return out


def saw():
    return [clip16(0.62 - 1.24 * (i / CYCLE)) for i in range(CYCLE)]


def noise(length=4096, seed=0x7FFF, hold=1):
    """The NES noise channel: xor two taps, shift the bit back in. Not white
    noise, and that is exactly why chip noise sounds like chip noise."""
    reg = seed
    out = []
    value = 0.0
    counter = 0
    for _ in range(length):
        if counter == 0:
            bit = (reg ^ (reg >> 1)) & 1
            reg = (reg >> 1) | (bit << 14)
            value = 0.55 if (reg & 1) else -0.55
            counter = hold
        counter -= 1
        out.append(clip16(value))
    return out


# --- the sound effects -------------------------------------------------------
# Ported unchanged from the C that first proved AX worked on hardware. The
# arithmetic is identical; what moved is *when* it runs. Generating a sound the
# console will never vary at runtime is work done every boot for no reason.

def laser():
    n = RATE * 22 // 100                     # 0.22 s
    out = []
    phase = 0.0
    for i in range(n):
        t = i / RATE
        u = i / n
        freq = 1500.0 * (0.18 ** u)          # two and a half octaves down
        phase = (phase + freq / RATE) % 1.0
        pulse_v = 1.0 if phase < 0.30 else -1.0   # 30% duty: thin, bright
        out.append(clip16(pulse_v * math.exp(-7.0 * t) * 0.75))
    return out


def boom():
    n = RATE * 9 // 10                       # 0.9 s
    out = []
    reg, lp, phase, sample, hold = 0x7FFF, 0.0, 0.0, 0.0, 0
    for i in range(n):
        t = i / RATE
        u = i / n
        # holding each noise value for a few samples lowers its pitch: the chip
        # trick for turning a hiss into rubble
        if hold == 0:
            bit = (reg ^ (reg >> 1)) & 1
            reg = (reg >> 1) | (bit << 14)
            sample = 1.0 if (reg & 1) else -1.0
            hold = 2 + int(u * 8.0)
        else:
            hold -= 1
        cutoff = 0.55 * math.exp(-2.2 * t) + 0.03
        lp += (sample - lp) * cutoff
        thump = math.sin(phase * 2.0 * math.pi) * math.exp(-9.0 * t)
        phase = (phase + (70.0 * (0.45 ** u)) / RATE) % 1.0
        # saturate rather than clamp: an explosion is meant to sound squashed,
        # but by a curve, not by a ceiling
        mix = lp * math.exp(-3.2 * t) * 0.85 + thump * 0.55
        out.append(clip16(math.tanh(mix) * 0.98))
    return out


ENGINE_CYCLES = 11


def engine():
    """0.2 s holding exactly eleven cycles of 55 Hz, so every harmonic and the
    slow wobble are whole multiples of the loop. A loop that does not close on
    itself clicks once per period, forever — and at 5 Hz you hear it."""
    n = RATE // 5
    out = []
    for i in range(n):
        ph = i / n
        w = ph * ENGINE_CYCLES * 2.0 * math.pi
        v = (math.sin(w) * 0.55 + math.sin(w * 2) * 0.28 +
             math.sin(w * 3) * 0.16 + math.sin(w * 5) * 0.09)
        out.append(clip16(v * (0.85 + 0.15 * math.sin(ph * 2.0 * math.pi)) * 0.6))
    return out


# --- the shape of a note -----------------------------------------------------
#
# A single cycle in a loop is a waveform, not an instrument. What it is missing
# is time: a note that comes up, settles, and falls away when you let go of it.
# Without this every note is a rectangle — full volume the instant it starts,
# full until it stops, silence — and a tune made of rectangles sounds like a
# test tone playing a melody. It is not the reverb that was missing.
#
# Times are milliseconds; sustain is a fraction of the note's own volume, held
# until the note-off; vibrato waits before it starts because that is what a
# player does — you commit to holding a note first, and only then move it.

def adsr(attack, decay, sustain, release):
    return {"attack": attack, "decay": decay, "sustain": sustain,
            "release": release}


def vibrato(delay, rate, depth):
    return {"delay": delay, "rate": rate, "depth": depth}


# The leads carry the melody, so they keep most of their body after the decay
# and get a slow, shallow vibrato that only reaches the notes held long enough
# to need it. The bass pulse does not: a wobble under everything else is mud.
# The triangle is the NES bass part and stays where it is put. And the noise
# channel is percussion — sustain zero is what turns a burst of static into a
# hit, and the sequencer ends the voice as soon as the decay has run out.
LEAD_VIB = vibrato(delay=240, rate=5.4, depth=22)


def build():
    return [
        ("pulse12",  pulse(0.125), True,  CYCLE,
         adsr(2, 130, 0.55, 90),  LEAD_VIB),
        ("pulse25",  pulse(0.25),  True,  CYCLE,
         adsr(3, 170, 0.62, 110), LEAD_VIB),
        ("pulse50",  pulse(0.50),  True,  CYCLE,
         adsr(4, 220, 0.72, 130), None),
        ("triangle", triangle(),   True,  CYCLE,
         adsr(2, 320, 0.88, 70),  None),
        ("saw",      saw(),        True,  CYCLE,
         adsr(6, 190, 0.60, 120), vibrato(delay=300, rate=4.8, depth=18)),
        ("noise",    noise(),      True,  0,
         adsr(0, 55, 0.0, 40),    None),
        # The effects shape themselves — a laser that already fades cannot be
        # told to fade again — so they keep the rectangle, which for a one-shot
        # means "play exactly what was baked".
        ("laser",    laser(),      False, 0, None, None),
        ("boom",     boom(),       False, 0, None, None),
        ("engine",   engine(),     True,  0, None, None),
    ]


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "audio"
    os.makedirs(outdir, exist_ok=True)

    manifest = {"rate": RATE, "instruments": []}
    for name, samples, loops, cycle, env, vib in build():
        write_wav(os.path.join(outdir, name + ".wav"), samples)
        peak = max(abs(s) for s in samples)
        clipped = sum(1 for s in samples if abs(s) >= 32000)
        # The loop seam, measured rather than hoped for: on a sampled loop the
        # step from the last sample back to the first must not exceed the steps
        # inside it, or it ticks once per period. On a single-cycle oscillator
        # the same test is meaningless — a saw's jump back to the top IS the
        # waveform, and a pulse is nothing but two of them.
        seam = ""
        if loops and cycle == 0 and len(samples) > 2:
            worst = max(abs(samples[i] - samples[i - 1])
                        for i in range(1, len(samples)))
            wrap = abs(samples[0] - samples[-1])
            seam = "  seam %d/%d %s" % (wrap, worst,
                                        "ok" if wrap <= worst else "WILL CLICK")
        elif loops:
            seam = "  single cycle"
        entry = {
            "name": name,
            "file": name + ".wav",
            "loop": bool(loops),
            "loopStart": 0,
            "cycleSamples": cycle,
        }
        if env:
            entry["envelope"] = env
        if vib:
            entry["vibrato"] = vib
        manifest["instruments"].append(entry)
        shape = ""
        if env:
            shape = "  %d/%d/%.2f/%d ms" % (env["attack"], env["decay"],
                                            env["sustain"], env["release"])
        if vib:
            shape += "  vib %.1f Hz %+d cents" % (vib["rate"], vib["depth"])
        print("  %-9s %6d samples  %.3f s  peak %5d%s%s%s"
              % (name, len(samples), len(samples) / RATE, peak,
                 "  CLIPPED %d" % clipped if clipped else "", seam, shape))

    path = os.path.join(outdir, "bank.json")
    with open(path, "w") as fh:
        json.dump(manifest, fh, indent=2)
    print("  %s: %d instruments at %d Hz" % (path, len(manifest["instruments"]),
                                             RATE))


if __name__ == "__main__":
    main()
