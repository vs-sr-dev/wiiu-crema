#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Samuele Voltan
#
# The four sounds a football match is made of.
#
#   python tools/gen_match_audio.py examples/poc14-kickoff/assets/audio
#
# gen_waves.py makes INSTRUMENTS — one cycle in a loop, played at whatever rate
# a note asks for. These are the other kind: whole events, played once at a
# fixed pitch, where the shape over time IS the sound rather than an envelope
# wrapped around a tone. A kick is a pitch falling through two octaves in a
# tenth of a second; there is no note in it to bend.
#
# Same 32 kHz as the rest of the project, mono, 16-bit — see the header of
# gen_waves.py for why half of 48 is the right rate here.

import json
import math
import os
import struct
import sys

RATE = 32000


def clip16(v):
    v = int(v * 32767.0)
    return max(-32768, min(32767, v))


def write_wav(path, samples):
    data = b"".join(struct.pack("<h", clip16(s)) for s in samples)
    with open(path, "wb") as fh:
        fh.write(b"RIFF")
        fh.write(struct.pack("<I", 36 + len(data)))
        fh.write(b"WAVEfmt ")
        fh.write(struct.pack("<IHHIIHH", 16, 1, 1, RATE, RATE * 2, 2, 16))
        fh.write(b"data")
        fh.write(struct.pack("<I", len(data)))
        fh.write(data)
    return len(samples)


class Noise:
    """A 15-bit LFSR, the same one the noise instrument uses. Deterministic, so
    the bytes in the repository do not change when nothing did."""

    def __init__(self, seed=0x2A57):
        self.state = seed

    def next(self):
        bit = ((self.state >> 0) ^ (self.state >> 1)) & 1
        self.state = (self.state >> 1) | (bit << 14)
        return (self.state & 1) * 2.0 - 1.0


# --- the kick ----------------------------------------------------------------
#
# A boot hitting a ball is two things that arrive together: a click, which is
# leather deforming, and a thump, which is the air inside it. The click is
# three milliseconds of noise and the thump is a sine falling from 190 Hz to
# 55 — the same trick a drum machine uses for a bass drum, because it is the
# same event.

def make_kick(hard=True):
    length = int(RATE * (0.13 if hard else 0.10))
    noise = Noise()
    out = []
    phase = 0.0
    for i in range(length):
        t = i / length
        f = (190.0 if hard else 150.0) * math.exp(-3.1 * t) + 52.0
        phase += 2.0 * math.pi * f / RATE
        body = math.sin(phase) * math.exp(-7.0 * t)
        click = noise.next() * math.exp(-260.0 * t) * (0.55 if hard else 0.35)
        out.append((body * 0.82 + click) * (0.95 if hard else 0.7))
    return out


# --- the whistle -------------------------------------------------------------
#
# A referee's whistle is a pea rattling inside a resonator, which is why it
# warbles: the pea interrupts the airflow a few dozen times a second. So the
# tone is modulated by that rattle rather than being a clean note, and without
# the warble it reads as a kettle.

def make_whistle():
    length = int(RATE * 0.55)
    out = []
    phase = 0.0
    warble = 0.0
    noise = Noise(0x1D3F)
    for i in range(length):
        t = i / length
        env = min(1.0, t * 26.0) * math.exp(-2.6 * max(0.0, t - 0.55))
        warble += 2.0 * math.pi * 34.0 / RATE
        f = 3120.0 * (1.0 + 0.055 * math.sin(warble))
        phase += 2.0 * math.pi * f / RATE
        tone = math.sin(phase) + 0.30 * math.sin(phase * 2.0)
        breath = noise.next() * 0.08
        out.append((tone * 0.42 + breath) * env)
    return out


# --- the post ----------------------------------------------------------------
#
# Aluminium. Two partials that are deliberately not in a harmonic ratio —
# a bar does not ring like a string, and picking 1 : 2.76 rather than 1 : 2 is
# most of the difference between metal and a bell.

def make_post():
    length = int(RATE * 0.45)
    out = []
    for i in range(length):
        t = i / length
        a = math.sin(2.0 * math.pi * 640.0 * i / RATE) * math.exp(-7.0 * t)
        b = math.sin(2.0 * math.pi * 1766.0 * i / RATE) * math.exp(-11.0 * t)
        c = math.sin(2.0 * math.pi * 3390.0 * i / RATE) * math.exp(-19.0 * t)
        out.append((a * 0.6 + b * 0.32 + c * 0.14) * 0.85)
    return out


# --- the crowd ---------------------------------------------------------------
#
# Not a recording and not a roar: a swell of noise run through a one-pole low
# pass, which is what a few thousand people sound like from a hundred metres
# away. Long, so it is baked as ADPCM — four bits a sample, decoded in
# hardware, which is the one compression on this console that is free.

def make_cheer():
    length = int(RATE * 1.9)
    noise = Noise(0x7A11)
    out = []
    lp = 0.0
    lp2 = 0.0
    for i in range(length):
        t = i / length
        # up fast, down slow: a crowd reacts before it decides how loud to be
        env = (1.0 - math.exp(-9.0 * t)) * math.exp(-1.9 * t)
        n = noise.next()
        lp += (n - lp) * 0.10
        lp2 += (lp - lp2) * 0.22
        # a slow surge underneath, so it breathes instead of hissing
        surge = 0.75 + 0.25 * math.sin(2.0 * math.pi * 1.7 * i / RATE)
        out.append(lp2 * env * surge * 2.6)
    return out


BANK = {
    "rate": RATE,
    "instruments": [
        {"name": "kick",    "file": "kick.wav",    "loop": False,
         "loopStart": 0, "cycleSamples": 0},
        {"name": "tap",     "file": "tap.wav",     "loop": False,
         "loopStart": 0, "cycleSamples": 0},
        {"name": "whistle", "file": "whistle.wav", "loop": False,
         "loopStart": 0, "cycleSamples": 0},
        {"name": "post",    "file": "post.wav",    "loop": False,
         "loopStart": 0, "cycleSamples": 0},
        {"name": "cheer",   "file": "cheer.wav",   "loop": False,
         "loopStart": 0, "cycleSamples": 0, "format": "adpcm"},
    ],
}


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "audio"
    os.makedirs(out_dir, exist_ok=True)

    for name, samples in (("kick", make_kick(True)),
                          ("tap", make_kick(False)),
                          ("whistle", make_whistle()),
                          ("post", make_post()),
                          ("cheer", make_cheer())):
        path = os.path.join(out_dir, name + ".wav")
        n = write_wav(path, samples)
        print("%s: %d samples, %.0f ms" % (path, n, n * 1000.0 / RATE))

    with open(os.path.join(out_dir, "bank.json"), "w", encoding="utf-8") as fh:
        json.dump(BANK, fh, indent=2)
    print("%s: %d instruments" % (os.path.join(out_dir, "bank.json"),
                                  len(BANK["instruments"])))


if __name__ == "__main__":
    main()
