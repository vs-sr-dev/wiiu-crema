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


# --- wavetables --------------------------------------------------------------
#
# A single cycle in a loop is one timbre forever. A *wavetable* is several cycles
# in a row, looped as a whole: the voice walks through them in order and starts
# again, so the timbre moves without anything on the console doing anything.
#
# The thing worth writing down is that **this needed no change anywhere else.**
# Not in the .cbank format, not in crema_bank, not in the sequencer, not in AX.
# The reason is the same one the whole audio design rests on: `cycleSamples` says
# how long ONE cycle is, and that is all the pitch arithmetic ever wanted —
#
#     ratio = frequency * cycleSamples / rate
#
# while the loop is the whole buffer. Sixteen cycles or a hundred, the note is at
# the same pitch and the extra cycles are timbre. A hardware sampler was already
# a wavetable synthesiser; nobody had asked it.
#
# What it is NOT, and the difference matters when writing a part: the table is
# walked once per cycle of the note, so its speed is the note's own frequency
# divided by the number of cycles. At A4 with 48 cycles the sweep is 9.2 Hz; an
# octave up it is 18.3 Hz. That is not an LFO — a chip's PWM runs at its own rate
# and does not care what note is playing. Pitch-locked modulation is a different
# instrument, brighter and thinner up top, and getting the LFO version would mean
# moving a voice's sample pointer between cycles from the audio thread, which is
# a different feature with a click in it.
#
# The parameter goes out and comes back rather than round, so the last cycle is a
# neighbour of the first and the table joins onto itself. A sweep that wrapped
# from wide back to narrow in one step would tick once per table.

WT_CYCLES = 48


def _tri(u):
    """1 -> 0 -> 1 across u in [0,1): the modulator that makes a table cyclic.

    Out and back rather than round, so the last cycle is a neighbour of the first
    and the table joins onto itself. A sweep that wrapped from one extreme to the
    other in a single step would tick once per table.

    It starts at **1** and not at 0, and that is not a detail — it is the
    difference between a table you can play a melody on and one you cannot. A
    voice is retriggered from sample zero on every note-on, so **cycle 0 is the
    attack**: whatever is there is what the ear hears as the note's identity, and
    everything after it is the note evolving. It was written the other way round
    first, with the sweep starting at the extreme the character is *absent* — a
    plain saw for `sync`, the thinnest and quietest duty for `pwm` — and on
    hardware it sounded like this, exactly as reported: "as if the wavetable moved
    in mono-legato compared to the clean attacks of the other bars, muddying the
    sound." It was not legato. A sixteenth at 132 BPM lasts 113 ms and at D3
    covers seventeen of forty-eight cycles, so every note was hearing only the
    dullest quarter of the table and never reaching the part with the sound in it.
    Sixteen notes that all begin soft and swell slightly have no transients, and a
    run of them blurs into one continuous morph.

    So the strong end goes first, and the middle of the table is the far side of
    the sweep. Short notes get the whole character immediately; long ones get the
    journey.
    """
    return 1.0 - (2.0 * u if u < 0.5 else 2.0 * (1.0 - u))


def _dc_free(samples, cycle):
    """Take each cycle's own average out, then rescale to the peak it had.

    This is the difference between a wavetable that works and one that hums, and
    it was found by ear on the console before it was found by arithmetic: the
    bass arpeggio buzzed on the one bar that used a table.

    A pulse of duty d has an average of d - (1 - d) times its amplitude — a 9%
    pulse sits at -81% of full scale and a 50% one sits at zero. On a single cycle
    that is a *constant* offset: it wastes headroom and is otherwise inaudible.
    On a table that sweeps the duty it is not constant, and **a DC offset that
    moves is not an offset, it is a signal** — here an 81%-amplitude waveform at
    the table's own rate, which is 2 to 9 Hz on the notes this song plays.
    Measured before the fix: `pwm` swept from -16120 to 0 across its 48 cycles.

    A real chip does not have this problem because its output is AC-coupled: a
    capacitor removes the DC before it reaches a speaker. These samples go
    straight into a digital mixer, so the capacitor has to be here. Removing each
    cycle's own mean is the idealised version of one — a high-pass whose cutoff is
    exactly the cycle rate, which leaves the table exactly periodic where a real
    one-pole filter would droop.

    The rescale matters as much as the subtraction. A 9% pulse with its mean taken
    out is a tall thin spike, +2A(1-d) high, which overflows int16 long before it
    sounds like anything. Normalising the whole table back to the peak it started
    with keeps it in range *and* keeps it comparable to every other instrument in
    the bank — and the loudness relation that comes out is the right one: narrow
    duty is thin and quiet, wide duty is full and loud, which is what pulse-width
    modulation sounds like on hardware that has a capacitor in it.
    """
    out = []
    for c in range(0, len(samples), cycle):
        chunk = samples[c:c + cycle]
        mean = sum(chunk) / float(len(chunk))
        out += [v - mean for v in chunk]
    before = max(abs(v) for v in samples) or 1
    after = max(abs(v) for v in out) or 1.0
    k = before / after
    return [int(round(v * k)) for v in out]


def wt_pwm(cycles=WT_CYCLES, lo=0.09, hi=0.50):
    """Pulse width modulation, the oldest trick there is. The duty lands on
    multiples of 1/32 because a cycle is 32 samples, so the sweep is a staircase
    of about a dozen steps — which is also what it sounded like on the machines
    this is imitating, where the duty was two bits in a register.

    `hi` is where it starts and `lo` is the far side of the sweep, not the other
    way round: see `_tri`. A wide pulse is the loudest cycle in the table and a
    9% one is the quietest, and a note that begins on the quiet end has no
    attack."""
    out = []
    for c in range(cycles):
        duty = lo + (hi - lo) * _tri(c / cycles)
        out += [clip16(0.62 if (i / CYCLE) < duty else -0.62)
                for i in range(CYCLE)]
    return _dc_free(out, CYCLE)


def wt_sync(cycles=WT_CYCLES, lo=1.0, hi=2.7):
    """Hard sync: a saw running faster than the cycle it is restarted by. The
    discontinuity where it is forced back to the top is the sound, and sweeping
    the ratio moves a formant across the harmonics.

    It starts at `hi` and sweeps down to `lo`, which is a ratio of 1.0 — a plain
    saw with no sync in it at all. Starting there instead was the mistake that
    made a bar of sixteenths sound like one wobble: every note opened on the one
    cycle of the table that has no character."""
    out = []
    for c in range(cycles):
        ratio = lo + (hi - lo) * _tri(c / cycles)
        for i in range(CYCLE):
            ph = ((i / CYCLE) * ratio) % 1.0
            out.append(clip16(0.62 - 1.24 * ph))
    return _dc_free(out, CYCLE)


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

# Which instruments are worth compressing, and this list is a measurement rather
# than a policy. The Wii U decodes ADPCM in hardware, so it is free at playback
# and the only question is what it costs in quality — which the baker prints as a
# signal-to-noise ratio at every build. What the numbers said:
#
#   engine   79.8 dB   a sum of five sine harmonics: a second-order predictor
#                      guesses it almost exactly, so this is very nearly lossless
#   boom     29.2 dB   filtered noise and a thump; it is an explosion
#   noise    26.6 dB   noise compressed as noise, which is honest enough
#   laser    25.6 dB   the brightest sound in the game, and the one whose edges
#                      soften most — left uncompressed, it is only 14 KB
#   pwm      21.1 dB   and this is the real lesson: a 32-sample cycle is 2.3
#   pulse50  2.67:1    frames long, so the header byte stops being a rounding
#                      error and the predictor never gets going. Wavetables and
#                      single cycles stay LPCM16; they are also the smallest
#                      things in the bank, so there was nothing to win.
#
# Between them boom and engine are 70 KB of the bank's 110, and they come down to
# 20. The ratio is 3.5:1 and not 4:1, because fourteen samples cost eight bytes
# and not seven — one of them is the header.
ADPCM = {"boom", "engine"}


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
        # Same cycle length as everything above, forty-eight of them. The file is
        # 1536 samples instead of 32 and nothing downstream noticed.
        ("pwm",      wt_pwm(),     True,  CYCLE,
         adsr(3, 210, 0.70, 130), LEAD_VIB),
        ("sync",     wt_sync(),    True,  CYCLE,
         adsr(2, 240, 0.55, 100), None),
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
        elif loops and len(samples) > cycle:
            # A table's own seam is the join from its last cycle back to its
            # first, and it is only quiet because the modulator turns around
            # rather than wrapping. Printing the number is how that stays true.
            tables = len(samples) // cycle
            wrap = abs(samples[0] - samples[-cycle])
            # And the number that actually mattered: how far the per-cycle
            # average moves across the table. A DC offset that moves IS a signal,
            # at the table's own rate, and at 81% of full amplitude it was audible
            # as a hum on the console before anyone looked for it. See _dc_free.
            means = [sum(samples[i * cycle:(i + 1) * cycle]) / float(cycle)
                     for i in range(tables)]
            swing = max(means) - min(means)
            seam = "  table of %d cycles, sweep at f/%d, joint %d, DC swing %d" \
                   " (%.1f%% of peak)" % (tables, tables, wrap, round(swing),
                                          100.0 * swing / peak)
        elif loops:
            seam = "  single cycle"
        entry = {
            "name": name,
            "file": name + ".wav",
            "loop": bool(loops),
            "loopStart": 0,
            "cycleSamples": cycle,
        }
        if name in ADPCM:
            entry["format"] = "adpcm"
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
